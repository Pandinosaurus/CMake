/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmProjectCommand.h"

#include <array>
#include <cstdio>
#include <functional>
#include <limits>
#include <utility>

#include <cmext/string_view>

#include "cmsys/RegularExpression.hxx"

#include "cmExecutionStatus.h"
#include "cmList.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmPolicies.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmValue.h"

static bool IncludeByVariable(cmExecutionStatus& status,
                              std::string const& variable);
static void TopLevelCMakeVarCondSet(cmMakefile& mf, std::string const& name,
                                    std::string const& value);

bool cmProjectCommand(std::vector<std::string> const& args,
                      cmExecutionStatus& status)
{
  if (args.empty()) {
    status.SetError("PROJECT called with incorrect number of arguments");
    return false;
  }

  cmMakefile& mf = status.GetMakefile();
  if (mf.IsRootMakefile() &&
      !mf.GetDefinition("CMAKE_MINIMUM_REQUIRED_VERSION")) {
    mf.IssueMessage(
      MessageType::AUTHOR_WARNING,
      "cmake_minimum_required() should be called prior to this top-level "
      "project() call. Please see the cmake-commands(7) manual for usage "
      "documentation of both commands.");
  }

  if (!IncludeByVariable(status, "CMAKE_PROJECT_INCLUDE_BEFORE")) {
    return false;
  }

  std::string const& projectName = args[0];

  if (!IncludeByVariable(status,
                         "CMAKE_PROJECT_" + projectName + "_INCLUDE_BEFORE")) {
    return false;
  }

  mf.SetProjectName(projectName);

  cmPolicies::PolicyStatus cmp0180 = mf.GetPolicyStatus(cmPolicies::CMP0180);

  std::string varName = cmStrCat(projectName, "_BINARY_DIR"_s);
  bool nonCacheVarAlreadySet = mf.IsNormalDefinitionSet(varName);
  mf.AddCacheDefinition(varName, mf.GetCurrentBinaryDirectory(),
                        "Value Computed by CMake", cmStateEnums::STATIC);
  if (cmp0180 == cmPolicies::NEW || nonCacheVarAlreadySet) {
    mf.AddDefinition(varName, mf.GetCurrentBinaryDirectory());
  }

  varName = cmStrCat(projectName, "_SOURCE_DIR"_s);
  nonCacheVarAlreadySet = mf.IsNormalDefinitionSet(varName);
  mf.AddCacheDefinition(varName, mf.GetCurrentSourceDirectory(),
                        "Value Computed by CMake", cmStateEnums::STATIC);
  if (cmp0180 == cmPolicies::NEW || nonCacheVarAlreadySet) {
    mf.AddDefinition(varName, mf.GetCurrentSourceDirectory());
  }

  mf.AddDefinition("PROJECT_BINARY_DIR", mf.GetCurrentBinaryDirectory());
  mf.AddDefinition("PROJECT_SOURCE_DIR", mf.GetCurrentSourceDirectory());

  mf.AddDefinition("PROJECT_NAME", projectName);

  mf.AddDefinitionBool("PROJECT_IS_TOP_LEVEL", mf.IsRootMakefile());

  varName = cmStrCat(projectName, "_IS_TOP_LEVEL"_s);
  nonCacheVarAlreadySet = mf.IsNormalDefinitionSet(varName);
  mf.AddCacheDefinition(varName, mf.IsRootMakefile() ? "ON" : "OFF",
                        "Value Computed by CMake", cmStateEnums::STATIC);
  if (cmp0180 == cmPolicies::NEW || nonCacheVarAlreadySet) {
    mf.AddDefinition(varName, mf.IsRootMakefile() ? "ON" : "OFF");
  }

  // Set the CMAKE_PROJECT_NAME variable to be the highest-level
  // project name in the tree. If there are two project commands
  // in the same CMakeLists.txt file, and it is the top level
  // CMakeLists.txt file, then go with the last one, so that
  // CMAKE_PROJECT_NAME will match PROJECT_NAME, and cmake --build
  // will work.
  if (!mf.GetDefinition("CMAKE_PROJECT_NAME") || mf.IsRootMakefile()) {
    mf.RemoveDefinition("CMAKE_PROJECT_NAME");
    mf.AddCacheDefinition("CMAKE_PROJECT_NAME", projectName,
                          "Value Computed by CMake", cmStateEnums::STATIC);
  }

  bool haveVersion = false;
  bool haveLanguages = false;
  bool haveDescription = false;
  bool haveHomepage = false;
  std::string version;
  std::string description;
  std::string homepage;
  std::vector<std::string> languages;
  std::function<void()> missedValueReporter;
  auto resetReporter = [&missedValueReporter]() {
    missedValueReporter = std::function<void()>();
  };
  enum Doing
  {
    DoingDescription,
    DoingHomepage,
    DoingLanguages,
    DoingVersion
  };
  Doing doing = DoingLanguages;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "LANGUAGES") {
      if (haveLanguages) {
        mf.IssueMessage(MessageType::FATAL_ERROR,
                        "LANGUAGES may be specified at most once.");
        cmSystemTools::SetFatalErrorOccurred();
        return true;
      }
      haveLanguages = true;
      if (missedValueReporter) {
        missedValueReporter();
      }
      doing = DoingLanguages;
      if (!languages.empty()) {
        std::string msg = cmStrCat(
          "the following parameters must be specified after LANGUAGES "
          "keyword: ",
          cmJoin(languages, ", "), '.');
        mf.IssueMessage(MessageType::WARNING, msg);
      }
    } else if (args[i] == "VERSION") {
      if (haveVersion) {
        mf.IssueMessage(MessageType::FATAL_ERROR,
                        "VERSION may be specified at most once.");
        cmSystemTools::SetFatalErrorOccurred();
        return true;
      }
      haveVersion = true;
      if (missedValueReporter) {
        missedValueReporter();
      }
      doing = DoingVersion;
      missedValueReporter = [&mf, &resetReporter]() {
        mf.IssueMessage(
          MessageType::WARNING,
          "VERSION keyword not followed by a value or was followed by a "
          "value that expanded to nothing.");
        resetReporter();
      };
    } else if (args[i] == "DESCRIPTION") {
      if (haveDescription) {
        mf.IssueMessage(MessageType::FATAL_ERROR,
                        "DESCRIPTION may be specified at most once.");
        cmSystemTools::SetFatalErrorOccurred();
        return true;
      }
      haveDescription = true;
      if (missedValueReporter) {
        missedValueReporter();
      }
      doing = DoingDescription;
      missedValueReporter = [&mf, &resetReporter]() {
        mf.IssueMessage(
          MessageType::WARNING,
          "DESCRIPTION keyword not followed by a value or was followed "
          "by a value that expanded to nothing.");
        resetReporter();
      };
    } else if (args[i] == "HOMEPAGE_URL") {
      if (haveHomepage) {
        mf.IssueMessage(MessageType::FATAL_ERROR,
                        "HOMEPAGE_URL may be specified at most once.");
        cmSystemTools::SetFatalErrorOccurred();
        return true;
      }
      haveHomepage = true;
      doing = DoingHomepage;
      missedValueReporter = [&mf, &resetReporter]() {
        mf.IssueMessage(
          MessageType::WARNING,
          "HOMEPAGE_URL keyword not followed by a value or was followed "
          "by a value that expanded to nothing.");
        resetReporter();
      };
    } else if (doing == DoingVersion) {
      doing = DoingLanguages;
      version = args[i];
      resetReporter();
    } else if (doing == DoingDescription) {
      doing = DoingLanguages;
      description = args[i];
      resetReporter();
    } else if (doing == DoingHomepage) {
      doing = DoingLanguages;
      homepage = args[i];
      resetReporter();
    } else // doing == DoingLanguages
    {
      languages.push_back(args[i]);
    }
  }

  if (missedValueReporter) {
    missedValueReporter();
  }

  if ((haveVersion || haveDescription || haveHomepage) && !haveLanguages &&
      !languages.empty()) {
    mf.IssueMessage(MessageType::FATAL_ERROR,
                    "project with VERSION, DESCRIPTION or HOMEPAGE_URL must "
                    "use LANGUAGES before language names.");
    cmSystemTools::SetFatalErrorOccurred();
    return true;
  }
  if (haveLanguages && languages.empty()) {
    languages.emplace_back("NONE");
  }

  if (haveVersion) {
    cmsys::RegularExpression vx(
      R"(^([0-9]+(\.[0-9]+(\.[0-9]+(\.[0-9]+)?)?)?)?$)");
    if (!vx.find(version)) {
      std::string e = R"(VERSION ")" + version + R"(" format invalid.)";
      mf.IssueMessage(MessageType::FATAL_ERROR, e);
      cmSystemTools::SetFatalErrorOccurred();
      return true;
    }

    cmPolicies::PolicyStatus const cmp0096 =
      mf.GetPolicyStatus(cmPolicies::CMP0096);

    constexpr std::size_t MAX_VERSION_COMPONENTS = 4u;
    std::string version_string;
    std::array<std::string, MAX_VERSION_COMPONENTS> version_components;

    if (cmp0096 == cmPolicies::OLD || cmp0096 == cmPolicies::WARN) {
      constexpr size_t maxIntLength =
        std::numeric_limits<unsigned>::digits10 + 2;
      char vb[MAX_VERSION_COMPONENTS][maxIntLength];
      unsigned v[MAX_VERSION_COMPONENTS] = { 0, 0, 0, 0 };
      int const vc = std::sscanf(version.c_str(), "%u.%u.%u.%u", &v[0], &v[1],
                                 &v[2], &v[3]);
      for (auto i = 0u; i < MAX_VERSION_COMPONENTS; ++i) {
        if (static_cast<int>(i) < vc) {
          std::snprintf(vb[i], maxIntLength, "%u", v[i]);
          version_string += &"."[static_cast<std::size_t>(i == 0)];
          version_string += vb[i];
          version_components[i] = vb[i];
        } else {
          vb[i][0] = '\x00';
        }
      }
    } else {
      // The regex above verified that we have a .-separated string of
      // non-negative integer components.  Keep the original string.
      version_string = std::move(version);
      // Split the integer components.
      auto components = cmSystemTools::SplitString(version_string, '.');
      for (auto i = 0u; i < components.size(); ++i) {
        version_components[i] = std::move(components[i]);
      }
    }

    std::string vv;
    vv = projectName + "_VERSION";
    mf.AddDefinition("PROJECT_VERSION", version_string);
    mf.AddDefinition(vv, version_string);
    vv = projectName + "_VERSION_MAJOR";
    mf.AddDefinition("PROJECT_VERSION_MAJOR", version_components[0]);
    mf.AddDefinition(vv, version_components[0]);
    vv = projectName + "_VERSION_MINOR";
    mf.AddDefinition("PROJECT_VERSION_MINOR", version_components[1]);
    mf.AddDefinition(vv, version_components[1]);
    vv = projectName + "_VERSION_PATCH";
    mf.AddDefinition("PROJECT_VERSION_PATCH", version_components[2]);
    mf.AddDefinition(vv, version_components[2]);
    vv = projectName + "_VERSION_TWEAK";
    mf.AddDefinition("PROJECT_VERSION_TWEAK", version_components[3]);
    mf.AddDefinition(vv, version_components[3]);
    // Also, try set top level variables
    TopLevelCMakeVarCondSet(mf, "CMAKE_PROJECT_VERSION", version_string);
    TopLevelCMakeVarCondSet(mf, "CMAKE_PROJECT_VERSION_MAJOR",
                            version_components[0]);
    TopLevelCMakeVarCondSet(mf, "CMAKE_PROJECT_VERSION_MINOR",
                            version_components[1]);
    TopLevelCMakeVarCondSet(mf, "CMAKE_PROJECT_VERSION_PATCH",
                            version_components[2]);
    TopLevelCMakeVarCondSet(mf, "CMAKE_PROJECT_VERSION_TWEAK",
                            version_components[3]);
  } else {
    // Set project VERSION variables to empty
    std::vector<std::string> vv = { "PROJECT_VERSION",
                                    "PROJECT_VERSION_MAJOR",
                                    "PROJECT_VERSION_MINOR",
                                    "PROJECT_VERSION_PATCH",
                                    "PROJECT_VERSION_TWEAK",
                                    projectName + "_VERSION",
                                    projectName + "_VERSION_MAJOR",
                                    projectName + "_VERSION_MINOR",
                                    projectName + "_VERSION_PATCH",
                                    projectName + "_VERSION_TWEAK" };
    if (mf.IsRootMakefile()) {
      vv.emplace_back("CMAKE_PROJECT_VERSION");
      vv.emplace_back("CMAKE_PROJECT_VERSION_MAJOR");
      vv.emplace_back("CMAKE_PROJECT_VERSION_MINOR");
      vv.emplace_back("CMAKE_PROJECT_VERSION_PATCH");
      vv.emplace_back("CMAKE_PROJECT_VERSION_TWEAK");
    }
    for (std::string const& i : vv) {
      cmValue v = mf.GetDefinition(i);
      if (cmNonempty(v)) {
        mf.AddDefinition(i, "");
      }
    }
  }

  mf.AddDefinition("PROJECT_DESCRIPTION", description);
  mf.AddDefinition(projectName + "_DESCRIPTION", description);
  TopLevelCMakeVarCondSet(mf, "CMAKE_PROJECT_DESCRIPTION", description);

  mf.AddDefinition("PROJECT_HOMEPAGE_URL", homepage);
  mf.AddDefinition(projectName + "_HOMEPAGE_URL", homepage);
  TopLevelCMakeVarCondSet(mf, "CMAKE_PROJECT_HOMEPAGE_URL", homepage);

  if (languages.empty()) {
    // if no language is specified do c and c++
    languages = { "C", "CXX" };
  }
  mf.EnableLanguage(languages, false);

  if (!IncludeByVariable(status, "CMAKE_PROJECT_INCLUDE")) {
    return false;
  }

  if (!IncludeByVariable(status,
                         "CMAKE_PROJECT_" + projectName + "_INCLUDE")) {
    return false;
  }

  return true;
}

static bool IncludeByVariable(cmExecutionStatus& status,
                              std::string const& variable)
{
  cmMakefile& mf = status.GetMakefile();
  cmValue include = mf.GetDefinition(variable);
  if (!include) {
    return true;
  }
  cmList includeFiles{ *include };

  bool failed = false;
  for (auto filePath : includeFiles) {
    // Any relative path without a .cmake extension is checked for valid cmake
    // modules. This logic should be consistent with CMake's include() command.
    // Otherwise default to checking relative path w.r.t. source directory
    if (!cmSystemTools::FileIsFullPath(filePath) &&
        !cmHasLiteralSuffix(filePath, ".cmake")) {
      std::string mfile = mf.GetModulesFile(cmStrCat(filePath, ".cmake"));
      if (mfile.empty()) {
        status.SetError(
          cmStrCat("could not find requested module:\n  ", filePath));
        failed = true;
        continue;
      }
      filePath = mfile;
    }
    std::string includeFile = cmSystemTools::CollapseFullPath(
      filePath, mf.GetCurrentSourceDirectory());
    if (!cmSystemTools::FileExists(includeFile)) {
      status.SetError(
        cmStrCat("could not find requested file:\n  ", filePath));
      failed = true;
      continue;
    }
    if (cmSystemTools::FileIsDirectory(includeFile)) {
      status.SetError(
        cmStrCat("requested file is a directory:\n  ", filePath));
      failed = true;
      continue;
    }

    bool const readit = mf.ReadDependentFile(filePath);
    if (readit) {
      // If the included file ran successfully, continue to the next file
      continue;
    }

    if (cmSystemTools::GetFatalErrorOccurred()) {
      failed = true;
      continue;
    }

    status.SetError(cmStrCat("could not load requested file:\n  ", filePath));
    failed = true;
  }
  // At this point all files were processed
  return !failed;
}

static void TopLevelCMakeVarCondSet(cmMakefile& mf, std::string const& name,
                                    std::string const& value)
{
  // Set the CMAKE_PROJECT_XXX variable to be the highest-level
  // project name in the tree. If there are two project commands
  // in the same CMakeLists.txt file, and it is the top level
  // CMakeLists.txt file, then go with the last one.
  if (!mf.GetDefinition(name) || mf.IsRootMakefile()) {
    mf.RemoveDefinition(name);
    mf.AddCacheDefinition(name, value, "Value Computed by CMake",
                          cmStateEnums::STATIC);
  }
}
