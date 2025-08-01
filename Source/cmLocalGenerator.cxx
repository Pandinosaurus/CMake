/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmLocalGenerator.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cm/memory>
#include <cm/optional>
#include <cm/string_view>
#include <cmext/algorithm>
#include <cmext/string_view>

#include "cmsys/RegularExpression.hxx"

#include "cmAlgorithms.h"
#include "cmComputeLinkInformation.h"
#include "cmCryptoHash.h"
#include "cmCustomCommand.h"
#include "cmCustomCommandGenerator.h"
#include "cmCustomCommandLines.h"
#include "cmCustomCommandTypes.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorExpressionEvaluationFile.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmInstallGenerator.h"
#include "cmInstallScriptGenerator.h"
#include "cmInstallTargetGenerator.h"
#include "cmLinkLineComputer.h"
#include "cmLinkLineDeviceComputer.h"
#include "cmList.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmRange.h"
#include "cmRulePlaceholderExpander.h"
#include "cmSourceFile.h"
#include "cmSourceFileLocation.h"
#include "cmSourceFileLocationKind.h"
#include "cmStandardLevelResolver.h"
#include "cmState.h"
#include "cmStateDirectory.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmTarget.h"
#include "cmTestGenerator.h"
#include "cmValue.h"
#include "cmake.h"

#if defined(__HAIKU__)
#  include <FindDirectory.h>
#  include <StorageDefs.h>
#endif

namespace {
// List of variables that are replaced when
// rules are expanded.  These variables are
// replaced in the form <var> with GetSafeDefinition(var).
// ${LANG} is replaced in the variable first with all enabled
// languages.
auto ruleReplaceVars = {
  "CMAKE_${LANG}_COMPILER",
  "CMAKE_SHARED_MODULE_${LANG}_FLAGS",
  "CMAKE_SHARED_LIBRARY_${LANG}_FLAGS",
  "CMAKE_SHARED_LIBRARY_SONAME_${LANG}_FLAG",
  "CMAKE_${LANG}_ARCHIVE",
  "CMAKE_AR",
  "CMAKE_SOURCE_DIR",
  "CMAKE_BINARY_DIR",
  "CMAKE_CURRENT_SOURCE_DIR",
  "CMAKE_CURRENT_BINARY_DIR",
  "CMAKE_RANLIB",
  "CMAKE_MT",
  "CMAKE_TAPI",
  "CMAKE_CUDA_HOST_COMPILER",
  "CMAKE_CUDA_HOST_LINK_LAUNCHER",
  "CMAKE_HIP_HOST_COMPILER",
  "CMAKE_HIP_HOST_LINK_LAUNCHER",
  "CMAKE_CL_SHOWINCLUDES_PREFIX",
};

// Variables whose placeholders now map to an empty string.
// Our platform modules no longer use these, but third-party code might.
auto ruleReplaceEmptyVars = {
  "CMAKE_SHARED_LIBRARY_CREATE_${LANG}_FLAGS",
  "CMAKE_SHARED_MODULE_CREATE_${LANG}_FLAGS",
  "CMAKE_${LANG}_LINK_FLAGS",
};
}

cmLocalGenerator::cmLocalGenerator(cmGlobalGenerator* gg, cmMakefile* makefile)
  : cmOutputConverter(makefile->GetStateSnapshot())
  , DirectoryBacktrace(makefile->GetBacktrace())
{
  this->GlobalGenerator = gg;

  this->Makefile = makefile;

  this->AliasTargets = makefile->GetAliasTargets();

  this->EmitUniversalBinaryFlags = true;

  this->ComputeObjectMaxPath();

  // Canonicalize entries of the CPATH environment variable the same
  // way detection of CMAKE_<LANG>_IMPLICIT_INCLUDE_DIRECTORIES does.
  {
    std::vector<std::string> cpath;
    cmSystemTools::GetPath(cpath, "CPATH");
    for (std::string const& cp : cpath) {
      if (cmSystemTools::FileIsFullPath(cp)) {
        this->EnvCPATH.emplace_back(cmSystemTools::CollapseFullPath(cp));
      }
    }
  }

  std::vector<std::string> enabledLanguages =
    this->GetState()->GetEnabledLanguages();

  if (cmValue sysrootCompile =
        this->Makefile->GetDefinition("CMAKE_SYSROOT_COMPILE")) {
    this->CompilerSysroot = *sysrootCompile;
  } else {
    this->CompilerSysroot = this->Makefile->GetSafeDefinition("CMAKE_SYSROOT");
  }

  if (cmValue sysrootLink =
        this->Makefile->GetDefinition("CMAKE_SYSROOT_LINK")) {
    this->LinkerSysroot = *sysrootLink;
  } else {
    this->LinkerSysroot = this->Makefile->GetSafeDefinition("CMAKE_SYSROOT");
  }

  // OSX SYSROOT can be required by some tools, like tapi
  {
    cmValue osxSysroot = this->Makefile->GetDefinition("CMAKE_OSX_SYSROOT");
    this->VariableMappings["CMAKE_OSX_SYSROOT"] =
      osxSysroot.IsEmpty() ? "/" : this->EscapeForShell(*osxSysroot, true);
  }

  if (cmValue appleArchSysroots =
        this->Makefile->GetDefinition("CMAKE_APPLE_ARCH_SYSROOTS")) {
    std::string const& appleArchs =
      this->Makefile->GetSafeDefinition("CMAKE_OSX_ARCHITECTURES");
    cmList archs(appleArchs);
    cmList sysroots{ appleArchSysroots, cmList::EmptyElements::Yes };
    if (archs.size() == sysroots.size()) {
      for (cmList::size_type i = 0; i < archs.size(); ++i) {
        this->AppleArchSysroots[archs[i]] = sysroots[i];
      }
    } else {
      std::string const e =
        cmStrCat("CMAKE_APPLE_ARCH_SYSROOTS:\n  ", *appleArchSysroots,
                 "\n"
                 "is not the same length as CMAKE_OSX_ARCHITECTURES:\n  ",
                 appleArchs);
      this->IssueMessage(MessageType::FATAL_ERROR, e);
    }
  }

  for (std::string const& lang : enabledLanguages) {
    if (lang == "NONE") {
      continue;
    }
    this->Compilers["CMAKE_" + lang + "_COMPILER"] = lang;

    this->VariableMappings["CMAKE_" + lang + "_COMPILER"] =
      this->Makefile->GetSafeDefinition("CMAKE_" + lang + "_COMPILER");

    std::string const& compilerArg1 = "CMAKE_" + lang + "_COMPILER_ARG1";
    std::string const& compilerTarget = "CMAKE_" + lang + "_COMPILER_TARGET";
    std::string const& compilerOptionTarget =
      "CMAKE_" + lang + "_COMPILE_OPTIONS_TARGET";
    std::string const& compilerExternalToolchain =
      "CMAKE_" + lang + "_COMPILER_EXTERNAL_TOOLCHAIN";
    std::string const& compilerOptionExternalToolchain =
      "CMAKE_" + lang + "_COMPILE_OPTIONS_EXTERNAL_TOOLCHAIN";
    std::string const& compilerOptionSysroot =
      "CMAKE_" + lang + "_COMPILE_OPTIONS_SYSROOT";

    this->VariableMappings[compilerArg1] =
      this->Makefile->GetSafeDefinition(compilerArg1);
    this->VariableMappings[compilerTarget] =
      this->Makefile->GetSafeDefinition(compilerTarget);
    this->VariableMappings[compilerOptionTarget] =
      this->Makefile->GetSafeDefinition(compilerOptionTarget);
    this->VariableMappings[compilerExternalToolchain] =
      this->Makefile->GetSafeDefinition(compilerExternalToolchain);
    this->VariableMappings[compilerOptionExternalToolchain] =
      this->Makefile->GetSafeDefinition(compilerOptionExternalToolchain);
    this->VariableMappings[compilerOptionSysroot] =
      this->Makefile->GetSafeDefinition(compilerOptionSysroot);

    for (std::string replaceVar : ruleReplaceVars) {
      if (replaceVar.find("${LANG}") != std::string::npos) {
        cmSystemTools::ReplaceString(replaceVar, "${LANG}", lang);
      }

      this->VariableMappings[replaceVar] =
        this->Makefile->GetSafeDefinition(replaceVar);
    }

    for (std::string replaceVar : ruleReplaceEmptyVars) {
      if (replaceVar.find("${LANG}") != std::string::npos) {
        cmSystemTools::ReplaceString(replaceVar, "${LANG}", lang);
      }

      this->VariableMappings[replaceVar] = std::string();
    }
  }
}

std::unique_ptr<cmRulePlaceholderExpander>
cmLocalGenerator::CreateRulePlaceholderExpander(cmBuildStep buildStep) const
{
  return cm::make_unique<cmRulePlaceholderExpander>(
    buildStep, this->Compilers, this->VariableMappings, this->CompilerSysroot,
    this->LinkerSysroot);
}

cmLocalGenerator::~cmLocalGenerator() = default;

void cmLocalGenerator::IssueMessage(MessageType t,
                                    std::string const& text) const
{
  this->GetCMakeInstance()->IssueMessage(t, text, this->DirectoryBacktrace);
}

void cmLocalGenerator::ComputeObjectMaxPath()
{
// Choose a maximum object file name length.
#if defined(_WIN32) || defined(__CYGWIN__)
  this->ObjectPathMax = 250;
#else
  this->ObjectPathMax = 1000;
#endif
  cmValue plen = this->Makefile->GetDefinition("CMAKE_OBJECT_PATH_MAX");
  if (cmNonempty(plen)) {
    unsigned int pmax;
    if (sscanf(plen->c_str(), "%u", &pmax) == 1) {
      if (pmax >= 128) {
        this->ObjectPathMax = pmax;
      } else {
        std::ostringstream w;
        w << "CMAKE_OBJECT_PATH_MAX is set to " << pmax
          << ", which is less than the minimum of 128.  "
             "The value will be ignored.";
        this->IssueMessage(MessageType::AUTHOR_WARNING, w.str());
      }
    } else {
      std::ostringstream w;
      w << "CMAKE_OBJECT_PATH_MAX is set to \"" << *plen
        << "\", which fails to parse as a positive integer.  "
           "The value will be ignored.";
      this->IssueMessage(MessageType::AUTHOR_WARNING, w.str());
    }
  }
  this->ObjectMaxPathViolations.clear();
}

static void MoveSystemIncludesToEnd(std::vector<std::string>& includeDirs,
                                    std::string const& config,
                                    std::string const& lang,
                                    cmGeneratorTarget const* target)
{
  if (!target) {
    return;
  }

  std::stable_sort(
    includeDirs.begin(), includeDirs.end(),
    [&target, &config, &lang](std::string const& a, std::string const& b) {
      return !target->IsSystemIncludeDirectory(a, config, lang) &&
        target->IsSystemIncludeDirectory(b, config, lang);
    });
}

static void MoveSystemIncludesToEnd(std::vector<BT<std::string>>& includeDirs,
                                    std::string const& config,
                                    std::string const& lang,
                                    cmGeneratorTarget const* target)
{
  if (!target) {
    return;
  }

  std::stable_sort(includeDirs.begin(), includeDirs.end(),
                   [target, &config, &lang](BT<std::string> const& a,
                                            BT<std::string> const& b) {
                     return !target->IsSystemIncludeDirectory(a.Value, config,
                                                              lang) &&
                       target->IsSystemIncludeDirectory(b.Value, config, lang);
                   });
}

void cmLocalGenerator::TraceDependencies() const
{
  // Generate the rule files for each target.
  auto const& targets = this->GetGeneratorTargets();
  for (auto const& target : targets) {
    if (!target->IsInBuildSystem()) {
      continue;
    }
    target->TraceDependencies();
  }
}

void cmLocalGenerator::GenerateTestFiles()
{
  if (!this->Makefile->IsOn("CMAKE_TESTING_ENABLED")) {
    return;
  }

  // Compute the set of configurations.
  std::vector<std::string> configurationTypes =
    this->Makefile->GetGeneratorConfigs(cmMakefile::OnlyMultiConfig);
  std::string config = this->Makefile->GetDefaultConfiguration();

  std::string file =
    cmStrCat(this->StateSnapshot.GetDirectory().GetCurrentBinary(),
             "/CTestTestfile.cmake");
  this->GlobalGenerator->AddTestFile(file);

  cmGeneratedFileStream fout(file);

  fout << "# CMake generated Testfile for \n"
          "# Source directory: "
       << this->StateSnapshot.GetDirectory().GetCurrentSource()
       << "\n"
          "# Build directory: "
       << this->StateSnapshot.GetDirectory().GetCurrentBinary()
       << "\n"
          "# \n"
          "# This file includes the relevant testing commands "
          "required for \n"
          "# testing this directory and lists subdirectories to "
          "be tested as well.\n";

  std::string resourceSpecFile =
    this->Makefile->GetSafeDefinition("CTEST_RESOURCE_SPEC_FILE");
  if (!resourceSpecFile.empty()) {
    fout << "set(CTEST_RESOURCE_SPEC_FILE \"" << resourceSpecFile << "\")\n";
  }

  cmValue testIncludeFile = this->Makefile->GetProperty("TEST_INCLUDE_FILE");
  if (testIncludeFile) {
    fout << "include(\"" << *testIncludeFile << "\")\n";
  }

  cmValue testIncludeFiles = this->Makefile->GetProperty("TEST_INCLUDE_FILES");
  if (testIncludeFiles) {
    cmList includesList{ *testIncludeFiles };
    for (std::string const& i : includesList) {
      fout << "include(\"" << i << "\")\n";
    }
  }

  // Ask each test generator to write its code.
  for (auto const& tester : this->Makefile->GetTestGenerators()) {
    tester->Compute(this);
    tester->Generate(fout, config, configurationTypes);
  }
  using vec_t = std::vector<cmStateSnapshot>;
  vec_t const& children = this->Makefile->GetStateSnapshot().GetChildren();
  for (cmStateSnapshot const& i : children) {
    // TODO: Use add_subdirectory instead?
    std::string outP = i.GetDirectory().GetCurrentBinary();
    outP = this->MaybeRelativeToCurBinDir(outP);
    outP = cmOutputConverter::EscapeForCMake(outP);
    fout << "subdirs(" << outP << ")\n";
  }

  // Add directory labels property
  cmValue directoryLabels =
    this->Makefile->GetDefinition("CMAKE_DIRECTORY_LABELS");
  cmValue labels = this->Makefile->GetProperty("LABELS");

  if (labels || directoryLabels) {
    fout << "set_directory_properties(PROPERTIES LABELS ";
    if (labels) {
      fout << cmOutputConverter::EscapeForCMake(*labels);
    }
    if (labels && directoryLabels) {
      fout << ";";
    }
    if (directoryLabels) {
      fout << cmOutputConverter::EscapeForCMake(*directoryLabels);
    }
    fout << ")\n";
  }
}

void cmLocalGenerator::CreateEvaluationFileOutputs()
{
  std::vector<std::string> const& configs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
  for (std::string const& c : configs) {
    this->CreateEvaluationFileOutputs(c);
  }
}

void cmLocalGenerator::CreateEvaluationFileOutputs(std::string const& config)
{
  for (auto const& geef : this->Makefile->GetEvaluationFiles()) {
    geef->CreateOutputFile(this, config);
  }
}

void cmLocalGenerator::ProcessEvaluationFiles(
  std::vector<std::string>& generatedFiles)
{
  for (auto const& geef : this->Makefile->GetEvaluationFiles()) {
    geef->Generate(this);
    if (cmSystemTools::GetFatalErrorOccurred()) {
      return;
    }
    std::vector<std::string> files = geef->GetFiles();
    std::sort(files.begin(), files.end());

    std::vector<std::string> intersection;
    std::set_intersection(files.begin(), files.end(), generatedFiles.begin(),
                          generatedFiles.end(),
                          std::back_inserter(intersection));
    if (!intersection.empty()) {
      cmSystemTools::Error("Files to be generated by multiple different "
                           "commands: " +
                           cmWrap('"', intersection, '"', " "));
      return;
    }

    cm::append(generatedFiles, files);
    std::inplace_merge(generatedFiles.begin(),
                       generatedFiles.end() - files.size(),
                       generatedFiles.end());
  }
}

void cmLocalGenerator::GenerateInstallRules()
{
  // Compute the install prefix.
  cmValue installPrefix =
    this->Makefile->GetDefinition("CMAKE_INSTALL_PREFIX");
  std::string prefix = *installPrefix;

#if defined(_WIN32) && !defined(__CYGWIN__)
  if (!installPrefix) {
    if (!cmSystemTools::GetEnv("SystemDrive", prefix)) {
      prefix = "C:";
    }
    cmValue project_name = this->Makefile->GetDefinition("PROJECT_NAME");
    if (cmNonempty(project_name)) {
      prefix += "/Program Files/";
      prefix += *project_name;
    } else {
      prefix += "/InstalledCMakeProject";
    }
  }
#elif defined(__HAIKU__)
  char dir[B_PATH_NAME_LENGTH];
  if (!installPrefix) {
    if (find_directory(B_SYSTEM_DIRECTORY, -1, false, dir, sizeof(dir)) ==
        B_OK) {
      prefix = dir;
    } else {
      prefix = "/boot/system";
    }
  }
#else
  if (!installPrefix) {
    prefix = "/usr/local";
  }
#endif
  if (cmValue stagingPrefix =
        this->Makefile->GetDefinition("CMAKE_STAGING_PREFIX")) {
    prefix = *stagingPrefix;
  }

  // Compute the set of configurations.
  std::vector<std::string> configurationTypes =
    this->Makefile->GetGeneratorConfigs(cmMakefile::OnlyMultiConfig);
  std::string config = this->Makefile->GetDefaultConfiguration();

  // Choose a default install configuration.
  std::string default_config = config;
  char const* default_order[] = { "RELEASE", "MINSIZEREL", "RELWITHDEBINFO",
                                  "DEBUG", nullptr };
  for (char const** c = default_order; *c && default_config.empty(); ++c) {
    for (std::string const& configurationType : configurationTypes) {
      if (cmSystemTools::UpperCase(configurationType) == *c) {
        default_config = configurationType;
      }
    }
  }
  if (default_config.empty() && !configurationTypes.empty()) {
    default_config = configurationTypes[0];
  }

  // Create the install script file.
  std::string file = this->StateSnapshot.GetDirectory().GetCurrentBinary();
  std::string homedir = this->GetState()->GetBinaryDirectory();
  int toplevel_install = 0;
  if (file == homedir) {
    toplevel_install = 1;
  }
  file += "/cmake_install.cmake";
  this->GetGlobalGenerator()->AddInstallScript(file);
  cmGeneratedFileStream fout(file);

  // Write the header.
  /* clang-format off */
  fout << "# Install script for directory: "
       << this->StateSnapshot.GetDirectory().GetCurrentSource()
       << "\n\n"
          "# Set the install prefix\n"
          "if(NOT DEFINED CMAKE_INSTALL_PREFIX)\n"
          "  set(CMAKE_INSTALL_PREFIX \"" << prefix << "\")\n"
          "endif()\n"
       << R"(string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX )"
       << "\"${CMAKE_INSTALL_PREFIX}\")\n\n";
  /* clang-format on */

  // Write support code for generating per-configuration install rules.
  /* clang-format off */
  fout <<
    "# Set the install configuration name.\n"
    "if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)\n"
    "  if(BUILD_TYPE)\n"
    "    string(REGEX REPLACE \"^[^A-Za-z0-9_]+\" \"\"\n"
    "           CMAKE_INSTALL_CONFIG_NAME \"${BUILD_TYPE}\")\n"
    "  else()\n"
    "    set(CMAKE_INSTALL_CONFIG_NAME \"" << default_config << "\")\n"
    "  endif()\n"
    "  message(STATUS \"Install configuration: "
    "\\\"${CMAKE_INSTALL_CONFIG_NAME}\\\"\")\n"
    "endif()\n"
    "\n";
  /* clang-format on */

  // Write support code for dealing with component-specific installs.
  /* clang-format off */
  fout <<
    "# Set the component getting installed.\n"
    "if(NOT CMAKE_INSTALL_COMPONENT)\n"
    "  if(COMPONENT)\n"
    "    message(STATUS \"Install component: \\\"${COMPONENT}\\\"\")\n"
    "    set(CMAKE_INSTALL_COMPONENT \"${COMPONENT}\")\n"
    "  else()\n"
    "    set(CMAKE_INSTALL_COMPONENT)\n"
    "  endif()\n"
    "endif()\n"
    "\n";
  /* clang-format on */

  // Copy user-specified install options to the install code.
  if (cmValue so_no_exe =
        this->Makefile->GetDefinition("CMAKE_INSTALL_SO_NO_EXE")) {
    /* clang-format off */
    fout <<
      "# Install shared libraries without execute permission?\n"
      "if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)\n"
      "  set(CMAKE_INSTALL_SO_NO_EXE \"" << *so_no_exe << "\")\n"
      "endif()\n"
      "\n";
    /* clang-format on */
  }

  // Copy cmake cross compile state to install code.
  if (cmValue crosscompiling =
        this->Makefile->GetDefinition("CMAKE_CROSSCOMPILING")) {
    /* clang-format off */
    fout <<
      "# Is this installation the result of a crosscompile?\n"
      "if(NOT DEFINED CMAKE_CROSSCOMPILING)\n"
      "  set(CMAKE_CROSSCOMPILING \"" << *crosscompiling << "\")\n"
      "endif()\n"
      "\n";
    /* clang-format on */
  }

  // Write default directory permissions.
  if (cmValue defaultDirPermissions = this->Makefile->GetDefinition(
        "CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS")) {
    /* clang-format off */
    fout <<
      "# Set default install directory permissions.\n"
      "if(NOT DEFINED CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS)\n"
      "  set(CMAKE_INSTALL_DEFAULT_DIRECTORY_PERMISSIONS \""
         << *defaultDirPermissions << "\")\n"
      "endif()\n"
      "\n";
    /* clang-format on */
  }

  // Write out CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM so that
  // installed code that uses `file(GET_RUNTIME_DEPENDENCIES)`
  // has same platform variable as when running cmake
  if (cmValue platform = this->Makefile->GetDefinition(
        "CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM")) {
    /* clang-format off */
    fout <<
      "# Set OS and executable format for runtime-dependencies.\n"
      "if(NOT DEFINED CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM)\n"
      "  set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM \""
         << *platform << "\")\n"
      "endif()\n"
      "\n";
    /* clang-format on */
  }

  // Write out CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL so that
  // installed code that uses `file(GET_RUNTIME_DEPENDENCIES)`
  // has same tool selected as when running cmake
  if (cmValue command =
        this->Makefile->GetDefinition("CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL")) {
    /* clang-format off */
    fout <<
      "# Set tool for dependency-resolution of runtime-dependencies.\n"
      "if(NOT DEFINED CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL)\n"
      "  set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL \""
         << *command << "\")\n"
      "endif()\n"
      "\n";
    /* clang-format on */
  }

  // Write out CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND so that
  // installed code that uses `file(GET_RUNTIME_DEPENDENCIES)`
  // has same path to the tool as when running cmake
  if (cmValue command = this->Makefile->GetDefinition(
        "CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND")) {
    /* clang-format off */
    fout <<
      "# Set path to tool for dependency-resolution of runtime-dependencies.\n"
      "if(NOT DEFINED CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND)\n"
      "  set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND \""
         << *command << "\")\n"
      "endif()\n"
      "\n";
    /* clang-format on */
  }

  // Write out CMAKE_OBJDUMP so that installed code that uses
  // `file(GET_RUNTIME_DEPENDENCIES)` and hasn't specified
  // CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND has consistent
  // logic to fallback to CMAKE_OBJDUMP when `objdump` is
  // not on the path
  if (cmValue command = this->Makefile->GetDefinition("CMAKE_OBJDUMP")) {
    /* clang-format off */
    fout <<
      "# Set path to fallback-tool for dependency-resolution.\n"
      "if(NOT DEFINED CMAKE_OBJDUMP)\n"
      "  set(CMAKE_OBJDUMP \""
         << *command << "\")\n"
      "endif()\n"
      "\n";
    /* clang-format on */
  }

  this->AddGeneratorSpecificInstallSetup(fout);

  // Ask each install generator to write its code.
  cmPolicies::PolicyStatus status = this->GetPolicyStatus(cmPolicies::CMP0082);
  auto const& installers = this->Makefile->GetInstallGenerators();
  bool haveSubdirectoryInstall = false;
  bool haveInstallAfterSubdirectory = false;
  if (status == cmPolicies::WARN) {
    for (auto const& installer : installers) {
      installer->CheckCMP0082(haveSubdirectoryInstall,
                              haveInstallAfterSubdirectory);
      installer->Generate(fout, config, configurationTypes);
    }
  } else {
    for (auto const& installer : installers) {
      installer->Generate(fout, config, configurationTypes);
    }
  }

  // Write rules from old-style specification stored in targets.
  this->GenerateTargetInstallRules(fout, config, configurationTypes);

  // Include install scripts from subdirectories.
  switch (status) {
    case cmPolicies::WARN:
      if (haveInstallAfterSubdirectory &&
          this->Makefile->PolicyOptionalWarningEnabled(
            "CMAKE_POLICY_WARNING_CMP0082")) {
        std::ostringstream e;
        e << cmPolicies::GetPolicyWarning(cmPolicies::CMP0082) << "\n";
        this->IssueMessage(MessageType::AUTHOR_WARNING, e.str());
      }
      CM_FALLTHROUGH;
    case cmPolicies::OLD: {
      std::vector<cmStateSnapshot> children =
        this->Makefile->GetStateSnapshot().GetChildren();
      if (!children.empty()) {
        fout << "if(NOT CMAKE_INSTALL_LOCAL_ONLY)\n";
        fout << "  # Include the install script for each subdirectory.\n";
        for (cmStateSnapshot const& c : children) {
          if (!c.GetDirectory().GetPropertyAsBool("EXCLUDE_FROM_ALL")) {
            std::string odir = c.GetDirectory().GetCurrentBinary();
            cmSystemTools::ConvertToUnixSlashes(odir);
            fout << "  include(\"" << odir << "/cmake_install.cmake\")\n";
          }
        }
        fout << "\n";
        fout << "endif()\n\n";
      }
    } break;

    case cmPolicies::NEW:
      // NEW behavior is handled in
      // cmInstallSubdirectoryGenerator::GenerateScript()
      break;
  }

  /* clang-format off */

    fout <<
      "string(REPLACE \";\" \"\\n\" CMAKE_INSTALL_MANIFEST_CONTENT\n"
      "       \"${CMAKE_INSTALL_MANIFEST_FILES}\")\n"
      "if(CMAKE_INSTALL_LOCAL_ONLY)\n"
      "  file(WRITE \"" <<
      this->StateSnapshot.GetDirectory().GetCurrentBinary() <<
      "/install_local_manifest.txt\"\n"
      "     \"${CMAKE_INSTALL_MANIFEST_CONTENT}\")\n"
      "endif()\n";

    if (toplevel_install) {
      fout <<
        "if(CMAKE_INSTALL_COMPONENT)\n"
        "  if(CMAKE_INSTALL_COMPONENT MATCHES \"^[a-zA-Z0-9_.+-]+$\")\n"
        "    set(CMAKE_INSTALL_MANIFEST \"install_manifest_"
        "${CMAKE_INSTALL_COMPONENT}.txt\")\n"
        "  else()\n"
        "    string(MD5 CMAKE_INST_COMP_HASH \"${CMAKE_INSTALL_COMPONENT}\")\n"
        "    set(CMAKE_INSTALL_MANIFEST \"install_manifest_"
        "${CMAKE_INST_COMP_HASH}.txt\")\n"
        "    unset(CMAKE_INST_COMP_HASH)\n"
        "  endif()\n"
        "else()\n"
        "  set(CMAKE_INSTALL_MANIFEST \"install_manifest.txt\")\n"
        "endif()\n"
        "\n"
        "if(NOT CMAKE_INSTALL_LOCAL_ONLY)\n"
        "  file(WRITE \"" << homedir << "/${CMAKE_INSTALL_MANIFEST}\"\n"
        "     \"${CMAKE_INSTALL_MANIFEST_CONTENT}\")\n"
        "endif()\n";
    }
  /* clang-format on */
}

void cmLocalGenerator::AddGeneratorTarget(
  std::unique_ptr<cmGeneratorTarget> gt)
{
  cmGeneratorTarget* gt_ptr = gt.get();

  this->GeneratorTargets.push_back(std::move(gt));
  this->GeneratorTargetSearchIndex.emplace(gt_ptr->GetName(), gt_ptr);
  this->GlobalGenerator->IndexGeneratorTarget(gt_ptr);
}

void cmLocalGenerator::AddImportedGeneratorTarget(cmGeneratorTarget* gt)
{
  this->ImportedGeneratorTargets.emplace(gt->GetName(), gt);
  this->GlobalGenerator->IndexGeneratorTarget(gt);
}

void cmLocalGenerator::AddOwnedImportedGeneratorTarget(
  std::unique_ptr<cmGeneratorTarget> gt)
{
  this->OwnedImportedGeneratorTargets.push_back(std::move(gt));
}

cmGeneratorTarget* cmLocalGenerator::FindLocalNonAliasGeneratorTarget(
  std::string const& name) const
{
  auto ti = this->GeneratorTargetSearchIndex.find(name);
  if (ti != this->GeneratorTargetSearchIndex.end()) {
    return ti->second;
  }
  return nullptr;
}

void cmLocalGenerator::ComputeTargetManifest()
{
  // Collect the set of configuration types.
  std::vector<std::string> configNames =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);

  // Add our targets to the manifest for each configuration.
  auto const& targets = this->GetGeneratorTargets();
  for (auto const& target : targets) {
    if (!target->IsInBuildSystem()) {
      continue;
    }
    for (std::string const& c : configNames) {
      target->ComputeTargetManifest(c);
    }
  }
}

bool cmLocalGenerator::ComputeTargetCompileFeatures()
{
  // Collect the set of configuration types.
  std::vector<std::string> configNames =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);

  using LanguagePair = std::pair<std::string, std::string>;
  std::vector<LanguagePair> pairedLanguages{
    { "OBJC", "C" }, { "OBJCXX", "CXX" }, { "CUDA", "CXX" }, { "HIP", "CXX" }
  };
  std::set<LanguagePair> inferredEnabledLanguages;
  for (auto const& lang : pairedLanguages) {
    if (this->Makefile->GetState()->GetLanguageEnabled(lang.first)) {
      inferredEnabledLanguages.insert(lang);
    }
  }

  // Process compile features of all targets.
  auto const& targets = this->GetGeneratorTargets();
  for (auto const& target : targets) {
    for (std::string const& c : configNames) {
      if (!target->ComputeCompileFeatures(c)) {
        return false;
      }
    }

    // Now that C/C++ _STANDARD values have been computed
    // set the values to ObjC/ObjCXX _STANDARD variables
    if (target->CanCompileSources()) {
      for (std::string const& c : configNames) {
        target->ComputeCompileFeatures(c, inferredEnabledLanguages);
      }
    }
  }

  return true;
}

bool cmLocalGenerator::IsRootMakefile() const
{
  return !this->StateSnapshot.GetBuildsystemDirectoryParent().IsValid();
}

cmState* cmLocalGenerator::GetState() const
{
  return this->GlobalGenerator->GetCMakeInstance()->GetState();
}

cmStateSnapshot cmLocalGenerator::GetStateSnapshot() const
{
  return this->Makefile->GetStateSnapshot();
}

std::string cmLocalGenerator::GetRuleLauncher(cmGeneratorTarget* target,
                                              std::string const& prop,
                                              std::string const& config)
{
  cmValue value = this->Makefile->GetProperty(prop);
  if (target) {
    value = target->GetProperty(prop);
  }
  if (value) {
    return cmGeneratorExpression::Evaluate(*value, this, config, target);
  }
  return "";
}

std::string cmLocalGenerator::ConvertToIncludeReference(
  std::string const& path, OutputFormat format)
{
  return this->ConvertToOutputForExisting(path, format);
}

std::string cmLocalGenerator::GetIncludeFlags(
  std::vector<std::string> const& includeDirs, cmGeneratorTarget* target,
  std::string const& lang, std::string const& config, bool forResponseFile)
{
  if (lang.empty()) {
    return "";
  }

  std::vector<std::string> includes = includeDirs;
  MoveSystemIncludesToEnd(includes, config, lang, target);

  OutputFormat shellFormat = forResponseFile ? RESPONSE : SHELL;
  std::ostringstream includeFlags;

  std::string const& includeFlag =
    this->Makefile->GetSafeDefinition(cmStrCat("CMAKE_INCLUDE_FLAG_", lang));
  bool quotePaths = false;
  if (this->Makefile->GetDefinition("CMAKE_QUOTE_INCLUDE_PATHS")) {
    quotePaths = true;
  }
  std::string sep = " ";
  bool repeatFlag = true;
  // should the include flag be repeated like ie. -IA -IB
  if (cmValue incSep = this->Makefile->GetDefinition(
        cmStrCat("CMAKE_INCLUDE_FLAG_SEP_", lang))) {
    // if there is a separator then the flag is not repeated but is only
    // given once i.e.  -classpath a:b:c
    sep = *incSep;
    repeatFlag = false;
  }

  // Support special system include flag if it is available and the
  // normal flag is repeated for each directory.
  cmValue sysIncludeFlag = nullptr;
  cmValue sysIncludeFlagWarning = nullptr;
  if (repeatFlag) {
    sysIncludeFlag = this->Makefile->GetDefinition(
      cmStrCat("CMAKE_INCLUDE_SYSTEM_FLAG_", lang));
    sysIncludeFlagWarning = this->Makefile->GetDefinition(
      cmStrCat("CMAKE_INCLUDE_SYSTEM_FLAG_", lang, "_WARNING"));
  }

  cmValue fwSearchFlag = this->Makefile->GetDefinition(
    cmStrCat("CMAKE_", lang, "_FRAMEWORK_SEARCH_FLAG"));
  cmValue sysFwSearchFlag = this->Makefile->GetDefinition(
    cmStrCat("CMAKE_", lang, "_SYSTEM_FRAMEWORK_SEARCH_FLAG"));

  bool flagUsed = false;
  bool sysIncludeFlagUsed = false;
  std::set<std::string> emitted;
#ifdef __APPLE__
  emitted.insert("/System/Library/Frameworks");
#endif
  for (std::string const& i : includes) {
    if (cmNonempty(fwSearchFlag) && this->Makefile->IsOn("APPLE") &&
        cmSystemTools::IsPathToFramework(i)) {
      std::string const frameworkDir = cmSystemTools::GetFilenamePath(i);
      if (emitted.insert(frameworkDir).second) {
        if (sysFwSearchFlag && target &&
            target->IsSystemIncludeDirectory(frameworkDir, config, lang)) {
          includeFlags << *sysFwSearchFlag;
        } else {
          includeFlags << *fwSearchFlag;
        }
        includeFlags << this->ConvertToOutputFormat(frameworkDir, shellFormat)
                     << " ";
      }
      continue;
    }

    if (!flagUsed || repeatFlag) {
      if (sysIncludeFlag && target &&
          target->IsSystemIncludeDirectory(i, config, lang)) {
        includeFlags << *sysIncludeFlag;
        sysIncludeFlagUsed = true;
      } else {
        includeFlags << includeFlag;
      }
      flagUsed = true;
    }
    std::string includePath = this->ConvertToIncludeReference(i, shellFormat);
    if (quotePaths && !includePath.empty() && includePath.front() != '\"') {
      includeFlags << "\"";
    }
    includeFlags << includePath;
    if (quotePaths && !includePath.empty() && includePath.front() != '\"') {
      includeFlags << "\"";
    }
    includeFlags << sep;
  }
  if (sysIncludeFlagUsed && sysIncludeFlagWarning) {
    includeFlags << *sysIncludeFlagWarning;
  }
  std::string flags = includeFlags.str();
  // remove trailing separators
  if ((sep[0] != ' ') && !flags.empty() && flags.back() == sep[0]) {
    flags.back() = ' ';
  }
  return cmTrimWhitespace(flags);
}

void cmLocalGenerator::AddCompileOptions(std::string& flags,
                                         cmGeneratorTarget* target,
                                         std::string const& lang,
                                         std::string const& config)
{
  std::vector<BT<std::string>> tmpFlags;
  this->AddCompileOptions(tmpFlags, target, lang, config);
  this->AppendFlags(flags, tmpFlags);
}

void cmLocalGenerator::AddCompileOptions(std::vector<BT<std::string>>& flags,
                                         cmGeneratorTarget* target,
                                         std::string const& lang,
                                         std::string const& config)
{
  std::string langFlagRegexVar = cmStrCat("CMAKE_", lang, "_FLAG_REGEX");

  if (cmValue langFlagRegexStr =
        this->Makefile->GetDefinition(langFlagRegexVar)) {
    // Filter flags acceptable to this language.
    if (cmValue targetFlags = target->GetProperty("COMPILE_FLAGS")) {
      std::vector<std::string> opts;
      cmSystemTools::ParseWindowsCommandLine(targetFlags->c_str(), opts);
      // Re-escape these flags since COMPILE_FLAGS were already parsed
      // as a command line above.
      std::string compileOpts;
      this->AppendCompileOptions(compileOpts, opts, langFlagRegexStr->c_str());
      if (!compileOpts.empty()) {
        flags.emplace_back(std::move(compileOpts));
      }
    }
    std::vector<BT<std::string>> targetCompileOpts =
      target->GetCompileOptions(config, lang);
    // COMPILE_OPTIONS are escaped.
    this->AppendCompileOptions(flags, targetCompileOpts,
                               langFlagRegexStr->c_str());
  } else {
    // Use all flags.
    if (cmValue targetFlags = target->GetProperty("COMPILE_FLAGS")) {
      // COMPILE_FLAGS are not escaped for historical reasons.
      std::string compileFlags;
      this->AppendFlags(compileFlags, *targetFlags);
      if (!compileFlags.empty()) {
        flags.emplace_back(std::move(compileFlags));
      }
    }
    std::vector<BT<std::string>> targetCompileOpts =
      target->GetCompileOptions(config, lang);
    // COMPILE_OPTIONS are escaped.
    this->AppendCompileOptions(flags, targetCompileOpts);
  }

  cmStandardLevelResolver standardResolver(this->Makefile);
  for (auto const& it : target->GetMaxLanguageStandards()) {
    cmValue standard = target->GetLanguageStandard(it.first, config);
    if (!standard) {
      continue;
    }
    if (standardResolver.IsLaterStandard(it.first, *standard, it.second)) {
      std::ostringstream e;
      e << "The COMPILE_FEATURES property of target \"" << target->GetName()
        << "\" was evaluated when computing the link "
           "implementation, and the \""
        << it.first << "_STANDARD\" was \"" << it.second
        << "\" for that computation.  Computing the "
           "COMPILE_FEATURES based on the link implementation resulted in a "
           "higher \""
        << it.first << "_STANDARD\" \"" << *standard
        << "\".  "
           "This is not permitted. The COMPILE_FEATURES may not both depend "
           "on "
           "and be depended on by the link implementation.\n";
      this->IssueMessage(MessageType::FATAL_ERROR, e.str());
      return;
    }
  }

  // Add Warning as errors flags
  if (!this->GetCMakeInstance()->GetIgnoreCompileWarningAsError()) {
    cmValue const wError = target->GetProperty("COMPILE_WARNING_AS_ERROR");
    cmValue const wErrorOpts = this->Makefile->GetDefinition(
      cmStrCat("CMAKE_", lang, "_COMPILE_OPTIONS_WARNING_AS_ERROR"));
    if (wError.IsOn() && wErrorOpts.IsSet()) {
      std::string wErrorFlags;
      this->AppendCompileOptions(wErrorFlags, *wErrorOpts);
      if (!wErrorFlags.empty()) {
        flags.emplace_back(std::move(wErrorFlags));
      }
    }
  }

  // Add compile flag for the MSVC compiler only.
  cmMakefile* mf = this->GetMakefile();
  if (cmValue jmc =
        mf->GetDefinition("CMAKE_" + lang + "_COMPILE_OPTIONS_JMC")) {

    // Handle Just My Code debugging flags, /JMC.
    // If the target is a Managed C++ one, /JMC is not compatible.
    if (target->GetManagedType(config) !=
        cmGeneratorTarget::ManagedType::Managed) {
      // add /JMC flags if target property VS_JUST_MY_CODE_DEBUGGING is set
      // to ON
      if (cmValue jmcExprGen =
            target->GetProperty("VS_JUST_MY_CODE_DEBUGGING")) {
        std::string isJMCEnabled =
          cmGeneratorExpression::Evaluate(*jmcExprGen, this, config);
        if (cmIsOn(isJMCEnabled)) {
          cmList optList{ *jmc };
          std::string jmcFlags;
          this->AppendCompileOptions(jmcFlags, optList);
          if (!jmcFlags.empty()) {
            flags.emplace_back(std::move(jmcFlags));
          }
        }
      }
    }
  }
}

cmTarget* cmLocalGenerator::AddCustomCommandToTarget(
  std::string const& target, cmCustomCommandType type,
  std::unique_ptr<cmCustomCommand> cc, cmObjectLibraryCommands objLibCommands)
{
  cmTarget* t = this->Makefile->GetCustomCommandTarget(
    target, objLibCommands, this->DirectoryBacktrace);
  if (!t) {
    return nullptr;
  }

  cc->SetBacktrace(this->DirectoryBacktrace);

  detail::AddCustomCommandToTarget(*this, cmCommandOrigin::Generator, t, type,
                                   std::move(cc));

  return t;
}

cmSourceFile* cmLocalGenerator::AddCustomCommandToOutput(
  std::unique_ptr<cmCustomCommand> cc, bool replace)
{
  // Make sure there is at least one output.
  if (cc->GetOutputs().empty()) {
    cmSystemTools::Error("Attempt to add a custom rule with no output!");
    return nullptr;
  }

  cc->SetBacktrace(this->DirectoryBacktrace);
  return detail::AddCustomCommandToOutput(*this, cmCommandOrigin::Generator,
                                          std::move(cc), replace);
}

cmTarget* cmLocalGenerator::AddUtilityCommand(
  std::string const& utilityName, bool excludeFromAll,
  std::unique_ptr<cmCustomCommand> cc)
{
  cmTarget* target =
    this->Makefile->AddNewUtilityTarget(utilityName, excludeFromAll);
  target->SetIsGeneratorProvided(true);

  if (cc->GetCommandLines().empty() && cc->GetDepends().empty()) {
    return target;
  }

  cc->SetBacktrace(this->DirectoryBacktrace);
  detail::AddUtilityCommand(*this, cmCommandOrigin::Generator, target,
                            std::move(cc));

  return target;
}

std::vector<BT<std::string>> cmLocalGenerator::GetIncludeDirectoriesImplicit(
  cmGeneratorTarget const* target, std::string const& lang,
  std::string const& config, bool stripImplicitDirs,
  bool appendAllImplicitDirs) const
{
  std::vector<BT<std::string>> result;
  // Do not repeat an include path.
  std::set<std::string> emitted;

  auto emitDir = [&result, &emitted](std::string const& dir) {
    if (emitted.insert(dir).second) {
      result.emplace_back(dir);
    }
  };

  auto emitBT = [&result, &emitted](BT<std::string> const& dir) {
    if (emitted.insert(dir.Value).second) {
      result.emplace_back(dir);
    }
  };

  // When automatic include directories are requested for a build then
  // include the source and binary directories at the beginning of the
  // include path to approximate include file behavior for an
  // in-source build.  This does not account for the case of a source
  // file in a subdirectory of the current source directory but we
  // cannot fix this because not all native build tools support
  // per-source-file include paths.
  if (this->Makefile->IsOn("CMAKE_INCLUDE_CURRENT_DIR")) {
    // Current binary directory
    emitDir(this->StateSnapshot.GetDirectory().GetCurrentBinary());
    // Current source directory
    emitDir(this->StateSnapshot.GetDirectory().GetCurrentSource());
  }

  if (!target) {
    return result;
  }

  // Standard include directories to be added unconditionally at the end.
  // These are intended to simulate additional implicit include directories.
  cmList userStandardDirs;
  {
    std::string const value = this->Makefile->GetSafeDefinition(
      cmStrCat("CMAKE_", lang, "_STANDARD_INCLUDE_DIRECTORIES"));
    userStandardDirs.assign(value);
    for (std::string& usd : userStandardDirs) {
      cmSystemTools::ConvertToUnixSlashes(usd);
    }
  }

  // Implicit include directories
  std::vector<std::string> implicitDirs;
  std::set<std::string> implicitSet;
  // Include directories to be excluded as if they were implicit.
  std::set<std::string> implicitExclude;
  {
    // Raw list of implicit include directories
    // Start with "standard" directories that we unconditionally add below.
    std::vector<std::string> impDirVec = userStandardDirs;

    // Load implicit include directories for this language.
    // We ignore this for Fortran because:
    // * There are no standard library headers to avoid overriding.
    // * Compilers like gfortran do not search their own implicit include
    //   directories for modules ('.mod' files).
    if (lang != "Fortran") {
      size_t const impDirVecOldSize = impDirVec.size();
      cmList::append(impDirVec,
                     this->Makefile->GetDefinition(cmStrCat(
                       "CMAKE_", lang, "_IMPLICIT_INCLUDE_DIRECTORIES")));
      // FIXME: Use cmRange with 'advance()' when it supports non-const.
      for (size_t i = impDirVecOldSize; i < impDirVec.size(); ++i) {
        cmSystemTools::ConvertToUnixSlashes(impDirVec[i]);
      }

      // The CMAKE_<LANG>_IMPLICIT_INCLUDE_DIRECTORIES are computed using
      // try_compile in CMAKE_DETERMINE_COMPILER_ABI, but the implicit include
      // directories are not known during that try_compile.  This can be a
      // problem when the HIP runtime include path is /usr/include because the
      // runtime include path is always added to the userDirs and the compiler
      // includes standard library headers via "__clang_hip_runtime_wrapper.h".
      if (lang == "HIP" && impDirVec.size() == impDirVecOldSize &&
          !cm::contains(impDirVec, "/usr/include")) {
        implicitExclude.emplace("/usr/include");
      }
    }

    // The Platform/UnixPaths module used to hard-code /usr/include for C, CXX,
    // and CUDA in CMAKE_<LANG>_IMPLICIT_INCLUDE_DIRECTORIES, but those
    // variables are now computed.  On macOS the /usr/include directory is
    // inside the platform SDK so the computed value does not contain it
    // directly.  In this case adding -I/usr/include can hide SDK headers so we
    // must still exclude it.
    if ((lang == "C" || lang == "CXX" || lang == "CUDA") &&
        !cm::contains(impDirVec, "/usr/include") &&
        std::find_if(impDirVec.begin(), impDirVec.end(),
                     [](std::string const& d) {
                       return cmHasLiteralSuffix(d, "/usr/include");
                     }) != impDirVec.end()) {
      // Only exclude this hard coded path for backwards compatibility.
      implicitExclude.emplace("/usr/include");
    }

    for (std::string const& i : impDirVec) {
      if (implicitSet.insert(this->GlobalGenerator->GetRealPath(i)).second) {
        implicitDirs.emplace_back(i);
      }
    }
  }

  bool const isCorCxx = (lang == "C" || lang == "CXX");

  // Resolve symlinks in CPATH for comparison with resolved include paths.
  // We do this here instead of when EnvCPATH is populated in case symlinks
  // on disk have changed in the meantime.
  std::set<std::string> resolvedEnvCPATH;
  if (isCorCxx) {
    for (std::string const& i : this->EnvCPATH) {
      resolvedEnvCPATH.emplace(this->GlobalGenerator->GetRealPath(i));
    }
  }

  // Checks if this is not an excluded (implicit) include directory.
  auto notExcluded = [this, &implicitSet, &implicitExclude, &resolvedEnvCPATH,
                      isCorCxx](std::string const& dir) -> bool {
    std::string const& real_dir = this->GlobalGenerator->GetRealPath(dir);
    return
      // Do not exclude directories that are not in any excluded set.
      !(cm::contains(implicitSet, real_dir) ||
        cm::contains(implicitExclude, dir))
      // Do not exclude entries of the CPATH environment variable even though
      // they are implicitly searched by the compiler.  They are meant to be
      // user-specified directories that can be re-ordered or converted to
      // -isystem without breaking real compiler builtin headers.
      || (isCorCxx && cm::contains(resolvedEnvCPATH, real_dir));
  };

  // Get the target-specific include directories.
  std::vector<BT<std::string>> userDirs =
    target->GetIncludeDirectories(config, lang);

  // Support putting all the in-project include directories first if
  // it is requested by the project.
  if (this->Makefile->IsOn("CMAKE_INCLUDE_DIRECTORIES_PROJECT_BEFORE")) {
    std::string const& topSourceDir = this->GetState()->GetSourceDirectory();
    std::string const& topBinaryDir = this->GetState()->GetBinaryDirectory();
    for (BT<std::string> const& udr : userDirs) {
      // Emit this directory only if it is a subdirectory of the
      // top-level source or binary tree.
      if (cmSystemTools::ComparePath(udr.Value, topSourceDir) ||
          cmSystemTools::ComparePath(udr.Value, topBinaryDir) ||
          cmSystemTools::IsSubDirectory(udr.Value, topSourceDir) ||
          cmSystemTools::IsSubDirectory(udr.Value, topBinaryDir)) {
        if (notExcluded(udr.Value)) {
          emitBT(udr);
        }
      }
    }
  }

  // Emit remaining non implicit user directories.
  for (BT<std::string> const& udr : userDirs) {
    if (notExcluded(udr.Value)) {
      emitBT(udr);
    }
  }

  // Sort result
  MoveSystemIncludesToEnd(result, config, lang, target);

  // Append standard include directories for this language.
  userDirs.reserve(userDirs.size() + userStandardDirs.size());
  for (std::string& usd : userStandardDirs) {
    emitDir(usd);
    userDirs.emplace_back(std::move(usd));
  }

  // Append compiler implicit include directories
  if (!stripImplicitDirs) {
    // Append implicit directories that were requested by the user only
    for (BT<std::string> const& udr : userDirs) {
      if (cm::contains(implicitSet, cmSystemTools::GetRealPath(udr.Value))) {
        emitBT(udr);
      }
    }
    // Append remaining implicit directories (on demand)
    if (appendAllImplicitDirs) {
      for (std::string& imd : implicitDirs) {
        emitDir(imd);
      }
    }
  }

  return result;
}

void cmLocalGenerator::GetIncludeDirectoriesImplicit(
  std::vector<std::string>& dirs, cmGeneratorTarget const* target,
  std::string const& lang, std::string const& config, bool stripImplicitDirs,
  bool appendAllImplicitDirs) const
{
  std::vector<BT<std::string>> tmp = this->GetIncludeDirectoriesImplicit(
    target, lang, config, stripImplicitDirs, appendAllImplicitDirs);
  dirs.reserve(dirs.size() + tmp.size());
  for (BT<std::string>& v : tmp) {
    dirs.emplace_back(std::move(v.Value));
  }
}

std::vector<BT<std::string>> cmLocalGenerator::GetIncludeDirectories(
  cmGeneratorTarget const* target, std::string const& lang,
  std::string const& config) const
{
  return this->GetIncludeDirectoriesImplicit(target, lang, config);
}

void cmLocalGenerator::GetIncludeDirectories(std::vector<std::string>& dirs,
                                             cmGeneratorTarget const* target,
                                             std::string const& lang,
                                             std::string const& config) const
{
  this->GetIncludeDirectoriesImplicit(dirs, target, lang, config);
}

void cmLocalGenerator::GetStaticLibraryFlags(std::string& flags,
                                             std::string const& config,
                                             std::string const& linkLanguage,
                                             cmGeneratorTarget* target)
{
  std::vector<BT<std::string>> tmpFlags =
    this->GetStaticLibraryFlags(config, linkLanguage, target);
  this->AppendFlags(flags, tmpFlags);
}

std::vector<BT<std::string>> cmLocalGenerator::GetStaticLibraryFlags(
  std::string const& config, std::string const& linkLanguage,
  cmGeneratorTarget* target)
{
  std::string const configUpper = cmSystemTools::UpperCase(config);
  std::vector<BT<std::string>> flags;
  if (linkLanguage != "Swift" && !this->IsSplitSwiftBuild()) {
    std::string staticLibFlags;
    this->AppendFlags(
      staticLibFlags,
      this->Makefile->GetSafeDefinition("CMAKE_STATIC_LINKER_FLAGS"));
    if (!configUpper.empty()) {
      std::string name = "CMAKE_STATIC_LINKER_FLAGS_" + configUpper;
      this->AppendFlags(staticLibFlags,
                        this->Makefile->GetSafeDefinition(name));
    }
    if (!staticLibFlags.empty()) {
      flags.emplace_back(std::move(staticLibFlags));
    }
  }

  std::string staticLibFlags;
  this->AppendFlags(staticLibFlags,
                    target->GetSafeProperty("STATIC_LIBRARY_FLAGS"));
  if (!configUpper.empty()) {
    std::string name = "STATIC_LIBRARY_FLAGS_" + configUpper;
    this->AppendFlags(staticLibFlags, target->GetSafeProperty(name));
  }

  if (!staticLibFlags.empty()) {
    flags.emplace_back(std::move(staticLibFlags));
  }

  std::vector<BT<std::string>> staticLibOpts =
    target->GetStaticLibraryLinkOptions(config, linkLanguage);
  // STATIC_LIBRARY_OPTIONS are escaped.
  this->AppendCompileOptions(flags, staticLibOpts);

  return flags;
}

void cmLocalGenerator::GetDeviceLinkFlags(
  cmLinkLineDeviceComputer& linkLineComputer, std::string const& config,
  std::string& linkLibs, std::string& linkFlags, std::string& frameworkPath,
  std::string& linkPath, cmGeneratorTarget* target)
{
  cmGeneratorTarget::DeviceLinkSetter setter(*target);

  cmComputeLinkInformation* pcli = target->GetLinkInformation(config);

  auto linklang = linkLineComputer.GetLinkerLanguage(target, config);
  auto ipoEnabled = target->IsIPOEnabled(linklang, config);
  if (!ipoEnabled) {
    ipoEnabled = linkLineComputer.ComputeRequiresDeviceLinkingIPOFlag(*pcli);
  }
  if (ipoEnabled) {
    if (cmValue cudaIPOFlags = this->Makefile->GetDefinition(
          "CMAKE_CUDA_DEVICE_LINK_OPTIONS_IPO")) {
      linkFlags += *cudaIPOFlags;
    }
  }

  if (pcli) {
    // Compute the required device link libraries when
    // resolving gpu lang device symbols
    this->OutputLinkLibraries(pcli, &linkLineComputer, linkLibs, frameworkPath,
                              linkPath);
  }

  this->AddVisibilityPresetFlags(linkFlags, target, "CUDA");
  this->GetGlobalGenerator()->EncodeLiteral(linkFlags);

  std::vector<std::string> linkOpts;
  target->GetLinkOptions(linkOpts, config, "CUDA");
  this->SetLinkScriptShell(this->GetGlobalGenerator()->GetUseLinkScript());
  // LINK_OPTIONS are escaped.
  this->AppendCompileOptions(linkFlags, linkOpts);
  this->SetLinkScriptShell(false);
}

void cmLocalGenerator::GetTargetFlags(
  cmLinkLineComputer* linkLineComputer, std::string const& config,
  std::string& linkLibs, std::string& flags, std::string& linkFlags,
  std::string& frameworkPath, std::string& linkPath, cmGeneratorTarget* target)
{
  std::vector<BT<std::string>> linkFlagsList;
  std::vector<BT<std::string>> linkPathList;
  std::vector<BT<std::string>> linkLibsList;
  this->GetTargetFlags(linkLineComputer, config, linkLibsList, flags,
                       linkFlagsList, frameworkPath, linkPathList, target);
  this->AppendFlags(linkFlags, linkFlagsList);
  this->AppendFlags(linkPath, linkPathList);
  this->AppendFlags(linkLibs, linkLibsList);
}

void cmLocalGenerator::GetTargetFlags(
  cmLinkLineComputer* linkLineComputer, std::string const& config,
  std::vector<BT<std::string>>& linkLibs, std::string& flags,
  std::vector<BT<std::string>>& linkFlags, std::string& frameworkPath,
  std::vector<BT<std::string>>& linkPath, cmGeneratorTarget* target)
{
  std::string const configUpper = cmSystemTools::UpperCase(config);
  cmComputeLinkInformation* pcli = target->GetLinkInformation(config);
  char const* libraryLinkVariable =
    "CMAKE_SHARED_LINKER_FLAGS"; // default to shared library

  std::string const linkLanguage =
    linkLineComputer->GetLinkerLanguage(target, config);

  {
    std::string createFlags;
    this->AppendTargetCreationLinkFlags(createFlags, target, linkLanguage);
    if (!createFlags.empty()) {
      linkFlags.emplace_back(std::move(createFlags));
    }
  }

  switch (target->GetType()) {
    case cmStateEnums::STATIC_LIBRARY:
      linkFlags = this->GetStaticLibraryFlags(config, linkLanguage, target);
      break;
    case cmStateEnums::MODULE_LIBRARY:
      libraryLinkVariable = "CMAKE_MODULE_LINKER_FLAGS";
      CM_FALLTHROUGH;
    case cmStateEnums::SHARED_LIBRARY: {
      if (this->IsSplitSwiftBuild() || linkLanguage != "Swift") {
        std::string libFlags;
        this->AddConfigVariableFlags(libFlags, libraryLinkVariable, target,
                                     cmBuildStep::Link, linkLanguage, config);
        if (!libFlags.empty()) {
          linkFlags.emplace_back(std::move(libFlags));
        }
      }

      std::string sharedLibFlags;
      cmValue targetLinkFlags = target->GetProperty("LINK_FLAGS");
      if (targetLinkFlags) {
        sharedLibFlags += *targetLinkFlags;
        sharedLibFlags += " ";
      }
      if (!configUpper.empty()) {
        targetLinkFlags =
          target->GetProperty(cmStrCat("LINK_FLAGS_", configUpper));
        if (targetLinkFlags) {
          sharedLibFlags += *targetLinkFlags;
          sharedLibFlags += " ";
        }
      }

      if (!sharedLibFlags.empty()) {
        this->GetGlobalGenerator()->EncodeLiteral(sharedLibFlags);
        linkFlags.emplace_back(std::move(sharedLibFlags));
      }

      std::vector<BT<std::string>> linkOpts =
        target->GetLinkOptions(config, linkLanguage);
      this->SetLinkScriptShell(this->GetGlobalGenerator()->GetUseLinkScript());
      // LINK_OPTIONS are escaped.
      this->AppendCompileOptions(linkFlags, linkOpts);
      this->SetLinkScriptShell(false);

      if (pcli) {
        this->OutputLinkLibraries(pcli, linkLineComputer, linkLibs,
                                  frameworkPath, linkPath);
      }
    } break;
    case cmStateEnums::EXECUTABLE: {
      if (linkLanguage.empty()) {
        cmSystemTools::Error(
          "CMake can not determine linker language for target: " +
          target->GetName());
        return;
      }

      if (linkLanguage != "Swift") {
        std::string exeFlags;
        this->AddConfigVariableFlags(exeFlags, "CMAKE_EXE_LINKER_FLAGS",
                                     target, cmBuildStep::Link, linkLanguage,
                                     config);
        if (!exeFlags.empty()) {
          linkFlags.emplace_back(std::move(exeFlags));
        }
      }

      {
        auto exeType = cmStrCat(
          "CMAKE_", linkLanguage, "_CREATE_",
          (target->IsWin32Executable(config) ? "WIN32" : "CONSOLE"), "_EXE");
        std::string exeFlags;
        this->AppendFlags(exeFlags, this->Makefile->GetDefinition(exeType),
                          exeType, target, cmBuildStep::Link, linkLanguage);
        if (!exeFlags.empty()) {
          linkFlags.emplace_back(std::move(exeFlags));
        }
      }

      std::string exeFlags;
      if (target->IsExecutableWithExports()) {
        exeFlags += this->Makefile->GetSafeDefinition(
          cmStrCat("CMAKE_EXE_EXPORTS_", linkLanguage, "_FLAG"));
        exeFlags += " ";
      }

      this->AddLanguageFlagsForLinking(flags, target, linkLanguage, config);
      if (pcli) {
        this->OutputLinkLibraries(pcli, linkLineComputer, linkLibs,
                                  frameworkPath, linkPath);
      }

      if (this->Makefile->IsOn("BUILD_SHARED_LIBS")) {
        std::string sFlagVar = "CMAKE_SHARED_BUILD_" + linkLanguage + "_FLAGS";
        exeFlags += this->Makefile->GetSafeDefinition(sFlagVar);
        exeFlags += " ";
      }

      std::string exeExportFlags =
        this->GetExeExportFlags(linkLanguage, *target);
      if (!exeExportFlags.empty()) {
        exeFlags += exeExportFlags;
        exeFlags += " ";
      }

      cmValue targetLinkFlags = target->GetProperty("LINK_FLAGS");
      if (targetLinkFlags) {
        exeFlags += *targetLinkFlags;
        exeFlags += " ";
      }
      if (!configUpper.empty()) {
        targetLinkFlags =
          target->GetProperty(cmStrCat("LINK_FLAGS_", configUpper));
        if (targetLinkFlags) {
          exeFlags += *targetLinkFlags;
          exeFlags += " ";
        }
      }

      if (!exeFlags.empty()) {
        this->GetGlobalGenerator()->EncodeLiteral(exeFlags);
        linkFlags.emplace_back(std::move(exeFlags));
      }

      std::vector<BT<std::string>> linkOpts =
        target->GetLinkOptions(config, linkLanguage);
      this->SetLinkScriptShell(this->GetGlobalGenerator()->GetUseLinkScript());
      // LINK_OPTIONS are escaped.
      this->AppendCompileOptions(linkFlags, linkOpts);
      this->SetLinkScriptShell(false);
    } break;
    default:
      break;
  }

  std::string extraLinkFlags;
  this->AppendLinkerTypeFlags(extraLinkFlags, target, config, linkLanguage);
  this->AppendPositionIndependentLinkerFlags(extraLinkFlags, target, config,
                                             linkLanguage);
  this->AppendWarningAsErrorLinkerFlags(extraLinkFlags, target, linkLanguage);
  this->AppendIPOLinkerFlags(extraLinkFlags, target, config, linkLanguage);
  this->AppendModuleDefinitionFlag(extraLinkFlags, target, linkLineComputer,
                                   config, linkLanguage);

  if (!extraLinkFlags.empty()) {
    this->GetGlobalGenerator()->EncodeLiteral(extraLinkFlags);
    linkFlags.emplace_back(std::move(extraLinkFlags));
  }
}

void cmLocalGenerator::GetTargetCompileFlags(cmGeneratorTarget* target,
                                             std::string const& config,
                                             std::string const& lang,
                                             std::string& flags,
                                             std::string const& arch)
{
  std::vector<BT<std::string>> tmpFlags =
    this->GetTargetCompileFlags(target, config, lang, arch);
  this->AppendFlags(flags, tmpFlags);
}

std::vector<BT<std::string>> cmLocalGenerator::GetTargetCompileFlags(
  cmGeneratorTarget* target, std::string const& config,
  std::string const& lang, std::string const& arch)
{
  std::vector<BT<std::string>> flags;
  std::string compileFlags;

  cmMakefile* mf = this->GetMakefile();

  // Add language-specific flags.
  this->AddLanguageFlags(compileFlags, target, cmBuildStep::Compile, lang,
                         config);

  if (target->IsIPOEnabled(lang, config)) {
    this->AppendFeatureOptions(compileFlags, lang, "IPO");
  }

  this->AddArchitectureFlags(compileFlags, target, lang, config, arch);

  if (lang == "Fortran") {
    this->AppendFlags(compileFlags,
                      this->GetTargetFortranFlags(target, config));
  } else if (lang == "Swift") {
    // Only set the compile mode if CMP0157 is set
    if (cm::optional<cmSwiftCompileMode> swiftCompileMode =
          this->GetSwiftCompileMode(target, config)) {
      std::string swiftCompileModeFlag;
      switch (*swiftCompileMode) {
        case cmSwiftCompileMode::Incremental: {
          swiftCompileModeFlag = "-incremental";
          if (cmValue flag =
                mf->GetDefinition("CMAKE_Swift_COMPILE_OPTIONS_INCREMENTAL")) {
            swiftCompileModeFlag = *flag;
          }
          break;
        }
        case cmSwiftCompileMode::Wholemodule: {
          swiftCompileModeFlag = "-wmo";
          if (cmValue flag =
                mf->GetDefinition("CMAKE_Swift_COMPILE_OPTIONS_WMO")) {
            swiftCompileModeFlag = *flag;
          }
          break;
        }
        case cmSwiftCompileMode::Singlefile:
          break;
        case cmSwiftCompileMode::Unknown: {
          this->IssueMessage(
            MessageType::AUTHOR_WARNING,
            cmStrCat("Unknown Swift_COMPILATION_MODE on target '",
                     target->GetName(), '\''));
        }
      }
      this->AppendFlags(compileFlags, swiftCompileModeFlag);
    }
  }

  this->AddFeatureFlags(compileFlags, target, lang, config);
  this->AddVisibilityPresetFlags(compileFlags, target, lang);
  this->AddColorDiagnosticsFlags(compileFlags, lang);
  this->AppendFlags(compileFlags, mf->GetDefineFlags());
  this->AppendFlags(compileFlags,
                    this->GetFrameworkFlags(lang, config, target));
  this->AppendFlags(compileFlags,
                    this->GetXcFrameworkFlags(lang, config, target));

  if (!compileFlags.empty()) {
    flags.emplace_back(std::move(compileFlags));
  }
  this->AddCompileOptions(flags, target, lang, config);
  return flags;
}

std::string cmLocalGenerator::GetFrameworkFlags(std::string const& lang,
                                                std::string const& config,
                                                cmGeneratorTarget* target)
{
  cmLocalGenerator* lg = target->GetLocalGenerator();
  cmMakefile* mf = lg->GetMakefile();

  if (!target->IsApple()) {
    return std::string();
  }

  cmValue fwSearchFlag =
    mf->GetDefinition(cmStrCat("CMAKE_", lang, "_FRAMEWORK_SEARCH_FLAG"));
  cmValue sysFwSearchFlag = mf->GetDefinition(
    cmStrCat("CMAKE_", lang, "_SYSTEM_FRAMEWORK_SEARCH_FLAG"));

  if (!fwSearchFlag && !sysFwSearchFlag) {
    return std::string{};
  }

  std::set<std::string> emitted;
#ifdef __APPLE__ /* don't insert this when crosscompiling e.g. to iphone */
  emitted.insert("/System/Library/Frameworks");
#endif
  std::vector<std::string> includes;

  lg->GetIncludeDirectories(includes, target, "C", config);
  // check all include directories for frameworks as this
  // will already have added a -F for the framework
  for (std::string const& include : includes) {
    if (lg->GetGlobalGenerator()->NameResolvesToFramework(include)) {
      std::string frameworkDir = cmStrCat(include, "/../");
      frameworkDir = cmSystemTools::CollapseFullPath(frameworkDir);
      emitted.insert(frameworkDir);
    }
  }

  std::string flags;
  if (cmComputeLinkInformation* cli = target->GetLinkInformation(config)) {
    std::vector<std::string> const& frameworks = cli->GetFrameworkPaths();
    for (std::string const& framework : frameworks) {
      if (emitted.insert(framework).second) {
        if (sysFwSearchFlag &&
            target->IsSystemIncludeDirectory(framework, config, lang)) {
          flags += *sysFwSearchFlag;
        } else {
          flags += *fwSearchFlag;
        }
        flags +=
          lg->ConvertToOutputFormat(framework, cmOutputConverter::SHELL);
        flags += " ";
      }
    }
  }
  return flags;
}

std::string cmLocalGenerator::GetXcFrameworkFlags(std::string const& lang,
                                                  std::string const& config,
                                                  cmGeneratorTarget* target)
{
  cmLocalGenerator* lg = target->GetLocalGenerator();
  cmMakefile* mf = lg->GetMakefile();

  if (!target->IsApple()) {
    return std::string();
  }

  cmValue includeSearchFlag =
    mf->GetDefinition(cmStrCat("CMAKE_INCLUDE_FLAG_", lang));
  cmValue sysIncludeSearchFlag =
    mf->GetDefinition(cmStrCat("CMAKE_INCLUDE_SYSTEM_FLAG_", lang));

  if (!includeSearchFlag && !sysIncludeSearchFlag) {
    return std::string{};
  }

  std::string flags;
  if (cmComputeLinkInformation* cli = target->GetLinkInformation(config)) {
    std::vector<std::string> const& paths = cli->GetXcFrameworkHeaderPaths();
    for (std::string const& path : paths) {
      if (sysIncludeSearchFlag &&
          target->IsSystemIncludeDirectory(path, config, lang)) {
        flags += *sysIncludeSearchFlag;
      } else {
        flags += *includeSearchFlag;
      }
      flags += lg->ConvertToOutputFormat(path, cmOutputConverter::SHELL);
      flags += " ";
    }
  }
  return flags;
}

void cmLocalGenerator::GetTargetDefines(cmGeneratorTarget const* target,
                                        std::string const& config,
                                        std::string const& lang,
                                        std::set<std::string>& defines) const
{
  std::set<BT<std::string>> tmp = this->GetTargetDefines(target, config, lang);
  for (BT<std::string> const& v : tmp) {
    defines.emplace(v.Value);
  }
}

std::set<BT<std::string>> cmLocalGenerator::GetTargetDefines(
  cmGeneratorTarget const* target, std::string const& config,
  std::string const& lang) const
{
  std::set<BT<std::string>> defines;

  // Add the export symbol definition for shared library objects.
  if (std::string const* exportMacro = target->GetExportMacro()) {
    this->AppendDefines(defines, *exportMacro);
  }

  // Add preprocessor definitions for this target and configuration.
  std::vector<BT<std::string>> targetDefines =
    target->GetCompileDefinitions(config, lang);
  this->AppendDefines(defines, targetDefines);

  return defines;
}

std::string cmLocalGenerator::GetTargetFortranFlags(
  cmGeneratorTarget const* /*unused*/, std::string const& /*unused*/)
{
  // Implemented by specific generators that override this.
  return std::string();
}

/**
 * Output the linking rules on a command line.  For executables,
 * targetLibrary should be a NULL pointer.  For libraries, it should point
 * to the name of the library.  This will not link a library against itself.
 */
void cmLocalGenerator::OutputLinkLibraries(
  cmComputeLinkInformation* pcli, cmLinkLineComputer* linkLineComputer,
  std::string& linkLibraries, std::string& frameworkPath,
  std::string& linkPath)
{
  std::vector<BT<std::string>> linkLibrariesList;
  std::vector<BT<std::string>> linkPathList;
  this->OutputLinkLibraries(pcli, linkLineComputer, linkLibrariesList,
                            frameworkPath, linkPathList);
  pcli->AppendValues(linkLibraries, linkLibrariesList);
  pcli->AppendValues(linkPath, linkPathList);
}

void cmLocalGenerator::OutputLinkLibraries(
  cmComputeLinkInformation* pcli, cmLinkLineComputer* linkLineComputer,
  std::vector<BT<std::string>>& linkLibraries, std::string& frameworkPath,
  std::vector<BT<std::string>>& linkPath)
{
  cmComputeLinkInformation& cli = *pcli;

  std::string linkLanguage = cli.GetLinkLanguage();

  std::string libPathFlag;
  if (cmValue value = this->Makefile->GetDefinition(
        "CMAKE_" + cli.GetLinkLanguage() + "_LIBRARY_PATH_FLAG")) {
    libPathFlag = *value;
  } else {
    libPathFlag =
      this->Makefile->GetRequiredDefinition("CMAKE_LIBRARY_PATH_FLAG");
  }

  std::string libPathTerminator;
  if (cmValue value = this->Makefile->GetDefinition(
        "CMAKE_" + cli.GetLinkLanguage() + "_LIBRARY_PATH_TERMINATOR")) {
    libPathTerminator = *value;
  } else {
    libPathTerminator =
      this->Makefile->GetRequiredDefinition("CMAKE_LIBRARY_PATH_TERMINATOR");
  }

  // Add standard link directories for this language
  std::string stdLinkDirString = this->Makefile->GetSafeDefinition(
    cmStrCat("CMAKE_", cli.GetLinkLanguage(), "_STANDARD_LINK_DIRECTORIES"));

  // Add standard libraries for this language.
  std::string stdLibString = this->Makefile->GetSafeDefinition(
    cmStrCat("CMAKE_", cli.GetLinkLanguage(), "_STANDARD_LIBRARIES"));

  // Append the framework search path flags.
  cmValue fwSearchFlag = this->Makefile->GetDefinition(
    cmStrCat("CMAKE_", linkLanguage, "_FRAMEWORK_SEARCH_FLAG"));

  frameworkPath = linkLineComputer->ComputeFrameworkPath(cli, fwSearchFlag);
  linkLineComputer->ComputeLinkPath(cli, libPathFlag, libPathTerminator,
                                    stdLinkDirString, linkPath);
  linkLineComputer->ComputeLinkLibraries(cli, stdLibString, linkLibraries);
}

std::string cmLocalGenerator::GetExeExportFlags(
  std::string const& linkLanguage, cmGeneratorTarget& tgt) const
{
  std::string linkFlags;

  // Flags to export symbols from an executable.
  if (tgt.GetType() == cmStateEnums::EXECUTABLE &&
      this->StateSnapshot.GetState()->GetGlobalPropertyAsBool(
        "TARGET_SUPPORTS_SHARED_LIBS")) {
    // Only add the flags if ENABLE_EXPORTS is on,
    // except on AIX where we compute symbol exports.
    if (!tgt.IsAIX() && tgt.GetPropertyAsBool("ENABLE_EXPORTS")) {
      linkFlags = this->Makefile->GetSafeDefinition(
        cmStrCat("CMAKE_SHARED_LIBRARY_LINK_", linkLanguage, "_FLAGS"));
    }
  }
  return linkFlags;
}

bool cmLocalGenerator::AllAppleArchSysrootsAreTheSame(
  std::vector<std::string> const& archs, cmValue sysroot)
{
  if (!sysroot) {
    return false;
  }

  return std::all_of(archs.begin(), archs.end(),
                     [this, sysroot](std::string const& arch) -> bool {
                       std::string const& archSysroot =
                         this->AppleArchSysroots[arch];
                       return cmIsOff(archSysroot) || *sysroot == archSysroot;
                     });
}

void cmLocalGenerator::AddArchitectureFlags(std::string& flags,
                                            cmGeneratorTarget const* target,
                                            std::string const& lang,
                                            std::string const& config,
                                            std::string const& filterArch)
{
  // Only add Apple specific flags on Apple platforms
  if (target->IsApple() && this->EmitUniversalBinaryFlags) {
    std::vector<std::string> archs = target->GetAppleArchs(config, lang);
    if (!archs.empty() &&
        (lang == "C" || lang == "CXX" || lang == "OBJC" || lang == "OBJCXX" ||
         lang == "ASM")) {
      for (std::string const& arch : archs) {
        if (filterArch.empty() || filterArch == arch) {
          flags += " -arch ";
          flags += arch;
        }
      }
    }

    cmValue sysroot = this->Makefile->GetDefinition("CMAKE_OSX_SYSROOT");
    if (sysroot.IsEmpty() &&
        this->Makefile->IsOn(
          cmStrCat("CMAKE_", lang, "_COMPILER_APPLE_SYSROOT_REQUIRED"))) {
      sysroot = this->Makefile->GetDefinition("_CMAKE_OSX_SYSROOT_PATH");
    }
    if (sysroot && *sysroot == "/") {
      sysroot = nullptr;
    }
    std::string sysrootFlagVar = "CMAKE_" + lang + "_SYSROOT_FLAG";
    cmValue sysrootFlag = this->Makefile->GetDefinition(sysrootFlagVar);
    if (cmNonempty(sysrootFlag)) {
      if (!this->AppleArchSysroots.empty() &&
          !this->AllAppleArchSysrootsAreTheSame(archs, sysroot)) {
        for (std::string const& arch : archs) {
          std::string const& archSysroot = this->AppleArchSysroots[arch];
          if (cmIsOff(archSysroot)) {
            continue;
          }
          if (filterArch.empty() || filterArch == arch) {
            flags += " -Xarch_" + arch + " ";
            // Combine sysroot flag and path to work with -Xarch
            std::string arch_sysroot = *sysrootFlag + archSysroot;
            flags += this->ConvertToOutputFormat(arch_sysroot, SHELL);
          }
        }
      } else if (cmNonempty(sysroot)) {
        flags += " ";
        flags += *sysrootFlag;
        flags += " ";
        flags += this->ConvertToOutputFormat(*sysroot, SHELL);
      }
    }

    cmValue deploymentTarget =
      this->Makefile->GetDefinition("CMAKE_OSX_DEPLOYMENT_TARGET");
    if (cmNonempty(deploymentTarget)) {
      std::string deploymentTargetFlagVar =
        "CMAKE_" + lang + "_OSX_DEPLOYMENT_TARGET_FLAG";
      cmValue deploymentTargetFlag =
        this->Makefile->GetDefinition(deploymentTargetFlagVar);
      if (cmNonempty(deploymentTargetFlag) &&
          // CMAKE_<LANG>_COMPILER_TARGET overrides a --target= for
          // CMAKE_OSX_DEPLOYMENT_TARGET, e.g., for visionOS.
          (!cmHasLiteralPrefix(*deploymentTarget, "--target=") ||
           this->Makefile
             ->GetDefinition(cmStrCat("CMAKE_", lang, "_COMPILER_TARGET"))
             .IsEmpty())) {
        std::string flag = *deploymentTargetFlag;

        // Add the deployment target architecture to the flag, if needed.
        static std::string const kARCH = "<ARCH>";
        std::string::size_type archPos = flag.find(kARCH);
        if (archPos != std::string::npos) {
          // This placeholder is meant for visionOS, so default to arm64
          // unless only non-arm64 archs are given.
          std::string const arch =
            (archs.empty() || cm::contains(archs, "arm64")) ? "arm64"
                                                            : archs[0];
          // Replace the placeholder with its value.
          flag = cmStrCat(flag.substr(0, archPos), arch,
                          flag.substr(archPos + kARCH.size()));
        }

        // Add the deployment target version to the flag.
        static std::string const kVERSION_MIN = "<VERSION_MIN>";
        std::string::size_type verPos = flag.find(kVERSION_MIN);
        if (verPos != std::string::npos) {
          // Replace the placeholder with its value.
          flag = cmStrCat(flag.substr(0, verPos), *deploymentTarget,
                          flag.substr(verPos + kVERSION_MIN.size()));
        } else {
          // There is no placeholder, so append the value.
          flag = cmStrCat(flag, *deploymentTarget);
        }

        flags += " ";
        flags += flag;
      }
    }
  }
}

void cmLocalGenerator::AddLanguageFlags(std::string& flags,
                                        cmGeneratorTarget const* target,
                                        cmBuildStep compileOrLink,
                                        std::string const& lang,
                                        std::string const& config)
{
  // Add language-specific flags.
  this->AddConfigVariableFlags(flags, cmStrCat("CMAKE_", lang, "_FLAGS"),
                               config);

  // Add the language standard flag for compiling, and sometimes linking.
  if (compileOrLink == cmBuildStep::Compile ||
      (compileOrLink == cmBuildStep::Link &&
       // Some toolchains require use of the language standard flag
       // when linking in order to use the matching standard library.
       // FIXME: If CMake gains an abstraction for standard library
       // selection, this will have to be reconciled with it.
       this->Makefile->IsOn(
         cmStrCat("CMAKE_", lang, "_LINK_WITH_STANDARD_COMPILE_OPTION")))) {
    cmStandardLevelResolver standardResolver(this->Makefile);
    std::string const& optionFlagDef =
      standardResolver.GetCompileOptionDef(target, lang, config);
    if (!optionFlagDef.empty()) {
      cmValue opt =
        target->Target->GetMakefile()->GetDefinition(optionFlagDef);
      if (opt) {
        cmList optList{ *opt };
        for (std::string const& i : optList) {
          this->AppendFlagEscape(flags, i);
        }
      }
    }
  }

  std::string compilerId = this->Makefile->GetSafeDefinition(
    cmStrCat("CMAKE_", lang, "_COMPILER_ID"));

  std::string compilerSimulateId = this->Makefile->GetSafeDefinition(
    cmStrCat("CMAKE_", lang, "_SIMULATE_ID"));

  bool const compilerTargetsMsvcABI =
    (compilerId == "MSVC" || compilerSimulateId == "MSVC");
  bool const compilerTargetsWatcomABI =
    (compilerId == "OpenWatcom" || compilerSimulateId == "OpenWatcom");

  if (lang == "Swift") {
    if (cmValue v = target->GetProperty("Swift_LANGUAGE_VERSION")) {
      if (cmSystemTools::VersionCompare(
            cmSystemTools::OP_GREATER_EQUAL,
            this->Makefile->GetDefinition("CMAKE_Swift_COMPILER_VERSION"),
            "4.2")) {
        this->AppendFlags(flags, "-swift-version " + *v);
      }
    }
  } else if (lang == "CUDA") {
    target->AddCUDAArchitectureFlags(compileOrLink, config, flags);
    target->AddCUDAToolkitFlags(flags);
  } else if (lang == "ISPC") {
    target->AddISPCTargetFlags(flags);
  } else if (lang == "RC" &&
             this->Makefile->GetSafeDefinition("CMAKE_RC_COMPILER")
                 .find("llvm-rc") != std::string::npos) {
    compilerId = this->Makefile->GetSafeDefinition("CMAKE_C_COMPILER_ID");
    if (!compilerId.empty()) {
      compilerSimulateId =
        this->Makefile->GetSafeDefinition("CMAKE_C_SIMULATE_ID");
    } else {
      compilerId = this->Makefile->GetSafeDefinition("CMAKE_CXX_COMPILER_ID");
      compilerSimulateId =
        this->Makefile->GetSafeDefinition("CMAKE_CXX_SIMULATE_ID");
    }
  } else if (lang == "HIP") {
    target->AddHIPArchitectureFlags(compileOrLink, config, flags);
  }

  // Add VFS Overlay for Clang compilers
  if (compilerId == "Clang") {
    if (cmValue vfsOverlay =
          this->Makefile->GetDefinition("CMAKE_CLANG_VFS_OVERLAY")) {
      if (compilerSimulateId == "MSVC") {
        this->AppendCompileOptions(
          flags,
          std::vector<std::string>{ "-Xclang", "-ivfsoverlay", "-Xclang",
                                    *vfsOverlay });
      } else {
        this->AppendCompileOptions(
          flags, std::vector<std::string>{ "-ivfsoverlay", *vfsOverlay });
      }
    }
  }
  // Add MSVC runtime library flags.  This is activated by the presence
  // of a default selection whether or not it is overridden by a property.
  cmValue msvcRuntimeLibraryDefault =
    this->Makefile->GetDefinition("CMAKE_MSVC_RUNTIME_LIBRARY_DEFAULT");
  if (cmNonempty(msvcRuntimeLibraryDefault)) {
    cmValue msvcRuntimeLibraryValue =
      target->GetProperty("MSVC_RUNTIME_LIBRARY");
    if (!msvcRuntimeLibraryValue) {
      msvcRuntimeLibraryValue = msvcRuntimeLibraryDefault;
    }
    std::string const msvcRuntimeLibrary = cmGeneratorExpression::Evaluate(
      *msvcRuntimeLibraryValue, this, config, target);
    if (!msvcRuntimeLibrary.empty()) {
      if (cmValue msvcRuntimeLibraryOptions = this->Makefile->GetDefinition(
            "CMAKE_" + lang + "_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_" +
            msvcRuntimeLibrary)) {
        this->AppendCompileOptions(flags, *msvcRuntimeLibraryOptions);
      } else if (compilerTargetsMsvcABI &&
                 !cmSystemTools::GetErrorOccurredFlag()) {
        // The compiler uses the MSVC ABI so it needs a known runtime library.
        this->IssueMessage(MessageType::FATAL_ERROR,
                           "MSVC_RUNTIME_LIBRARY value '" +
                             msvcRuntimeLibrary + "' not known for this " +
                             lang + " compiler.");
      }
    }
  }

  // Add Watcom runtime library flags.  This is activated by the presence
  // of a default selection whether or not it is overridden by a property.
  cmValue watcomRuntimeLibraryDefault =
    this->Makefile->GetDefinition("CMAKE_WATCOM_RUNTIME_LIBRARY_DEFAULT");
  if (cmNonempty(watcomRuntimeLibraryDefault)) {
    cmValue watcomRuntimeLibraryValue =
      target->GetProperty("WATCOM_RUNTIME_LIBRARY");
    if (!watcomRuntimeLibraryValue) {
      watcomRuntimeLibraryValue = watcomRuntimeLibraryDefault;
    }
    std::string const watcomRuntimeLibrary = cmGeneratorExpression::Evaluate(
      *watcomRuntimeLibraryValue, this, config, target);
    if (!watcomRuntimeLibrary.empty()) {
      if (cmValue watcomRuntimeLibraryOptions = this->Makefile->GetDefinition(
            "CMAKE_" + lang + "_COMPILE_OPTIONS_WATCOM_RUNTIME_LIBRARY_" +
            watcomRuntimeLibrary)) {
        this->AppendCompileOptions(flags, *watcomRuntimeLibraryOptions);
      } else if (compilerTargetsWatcomABI &&
                 !cmSystemTools::GetErrorOccurredFlag()) {
        // The compiler uses the Watcom ABI so it needs a known runtime
        // library.
        this->IssueMessage(MessageType::FATAL_ERROR,
                           "WATCOM_RUNTIME_LIBRARY value '" +
                             watcomRuntimeLibrary + "' not known for this " +
                             lang + " compiler.");
      }
    }
  }

  // Add MSVC runtime checks flags. This is activated by the presence
  // of a default selection whether or not it is overridden by a property.
  cmValue msvcRuntimeChecksDefault =
    this->Makefile->GetDefinition("CMAKE_MSVC_RUNTIME_CHECKS_DEFAULT");
  if (cmNonempty(msvcRuntimeChecksDefault)) {
    cmValue msvcRuntimeChecksValue =
      target->GetProperty("MSVC_RUNTIME_CHECKS");
    if (!msvcRuntimeChecksValue) {
      msvcRuntimeChecksValue = msvcRuntimeChecksDefault;
    }
    cmList msvcRuntimeChecksList = cmGeneratorExpression::Evaluate(
      *msvcRuntimeChecksValue, this, config, target);
    msvcRuntimeChecksList.remove_duplicates();

    // RTC1/RTCsu VS GUI workaround
    std::string const stackFrameErrorCheck = "StackFrameErrorCheck";
    std::string const unitinitializedVariable = "UninitializedVariable";
    std::string const rtcSU = "RTCsu";
    if ((cm::contains(msvcRuntimeChecksList, stackFrameErrorCheck) &&
         cm::contains(msvcRuntimeChecksList, unitinitializedVariable)) ||
        cm::contains(msvcRuntimeChecksList, rtcSU)) {
      msvcRuntimeChecksList.remove_items(
        { stackFrameErrorCheck, unitinitializedVariable, rtcSU });
      msvcRuntimeChecksList.append(rtcSU);
    }

    for (std::string const& msvcRuntimeChecks : msvcRuntimeChecksList) {
      if (cmValue msvcRuntimeChecksOptions =
            this->Makefile->GetDefinition(cmStrCat(
              "CMAKE_", lang,
              "_COMPILE_OPTIONS_MSVC_RUNTIME_CHECKS_" + msvcRuntimeChecks))) {
        this->AppendCompileOptions(flags, *msvcRuntimeChecksOptions);
      } else if (compilerTargetsMsvcABI &&
                 !cmSystemTools::GetErrorOccurredFlag()) {
        // The compiler uses the MSVC ABI so it needs a known runtime checks.
        this->IssueMessage(MessageType::FATAL_ERROR,
                           cmStrCat("MSVC_RUNTIME_CHECKS value '",
                                    msvcRuntimeChecks, "' not known for this ",
                                    lang, " compiler."));
      }
    }
  }

  // Add MSVC debug information format flags if CMP0141 is NEW.
  if (cm::optional<std::string> msvcDebugInformationFormat =
        this->GetMSVCDebugFormatName(config, target)) {
    if (!msvcDebugInformationFormat->empty()) {
      if (cmValue msvcDebugInformationFormatOptions =
            this->Makefile->GetDefinition(
              cmStrCat("CMAKE_", lang,
                       "_COMPILE_OPTIONS_MSVC_DEBUG_INFORMATION_FORMAT_",
                       *msvcDebugInformationFormat))) {
        this->AppendCompileOptions(flags, *msvcDebugInformationFormatOptions);
      } else if (compilerTargetsMsvcABI &&
                 !cmSystemTools::GetErrorOccurredFlag()) {
        // The compiler uses the MSVC ABI so it needs a known runtime library.
        this->IssueMessage(MessageType::FATAL_ERROR,
                           cmStrCat("MSVC_DEBUG_INFORMATION_FORMAT value '",
                                    *msvcDebugInformationFormat,
                                    "' not known for this ", lang,
                                    " compiler."));
      }
    }
  }
}

void cmLocalGenerator::AddLanguageFlagsForLinking(
  std::string& flags, cmGeneratorTarget const* target, std::string const& lang,
  std::string const& config)
{
  this->AddLanguageFlags(flags, target, cmBuildStep::Link, lang, config);

  if (target->IsIPOEnabled(lang, config)) {
    this->AppendFeatureOptions(flags, lang, "IPO");
  }
}

cmGeneratorTarget* cmLocalGenerator::FindGeneratorTargetToUse(
  std::string const& name) const
{
  auto imported = this->ImportedGeneratorTargets.find(name);
  if (imported != this->ImportedGeneratorTargets.end()) {
    return imported->second;
  }

  // find local alias to imported target
  auto aliased = this->AliasTargets.find(name);
  if (aliased != this->AliasTargets.end()) {
    imported = this->ImportedGeneratorTargets.find(aliased->second);
    if (imported != this->ImportedGeneratorTargets.end()) {
      return imported->second;
    }
  }

  if (cmGeneratorTarget* t = this->FindLocalNonAliasGeneratorTarget(name)) {
    return t;
  }

  return this->GetGlobalGenerator()->FindGeneratorTarget(name);
}

bool cmLocalGenerator::GetRealDependency(std::string const& inName,
                                         std::string const& config,
                                         std::string& dep)
{
  // Older CMake code may specify the dependency using the target
  // output file rather than the target name.  Such code would have
  // been written before there was support for target properties that
  // modify the name so stripping down to just the file name should
  // produce the target name in this case.
  std::string name = cmSystemTools::GetFilenameName(inName);

  // If the input name is the empty string, there is no real
  // dependency. Short-circuit the other checks:
  if (name.empty()) {
    return false;
  }
  if (cmSystemTools::GetFilenameLastExtension(name) == ".exe") {
    name = cmSystemTools::GetFilenameWithoutLastExtension(name);
  }

  // Look for a CMake target with the given name.
  if (cmGeneratorTarget* target = this->FindGeneratorTargetToUse(name)) {
    // make sure it is not just a coincidence that the target name
    // found is part of the inName
    if (cmSystemTools::FileIsFullPath(inName)) {
      std::string tLocation;
      if (target->GetType() >= cmStateEnums::EXECUTABLE &&
          target->GetType() <= cmStateEnums::MODULE_LIBRARY) {
        tLocation = target->GetLocation(config);
        tLocation = cmSystemTools::GetFilenamePath(tLocation);
        tLocation = cmSystemTools::CollapseFullPath(tLocation);
      }
      std::string depLocation =
        cmSystemTools::GetFilenamePath(std::string(inName));
      depLocation = cmSystemTools::CollapseFullPath(depLocation);
      if (depLocation != tLocation) {
        // it is a full path to a depend that has the same name
        // as a target but is in a different location so do not use
        // the target as the depend
        dep = inName;
        return true;
      }
    }
    switch (target->GetType()) {
      case cmStateEnums::EXECUTABLE:
      case cmStateEnums::STATIC_LIBRARY:
      case cmStateEnums::SHARED_LIBRARY:
      case cmStateEnums::MODULE_LIBRARY:
      case cmStateEnums::UNKNOWN_LIBRARY:
        dep = target->GetFullPath(config, cmStateEnums::RuntimeBinaryArtifact,
                                  /*realname=*/true);
        return true;
      case cmStateEnums::OBJECT_LIBRARY:
        // An object library has no single file on which to depend.
        // This was listed to get the target-level dependency.
      case cmStateEnums::INTERFACE_LIBRARY:
        // An interface library has no file on which to depend.
        // This was listed to get the target-level dependency.
      case cmStateEnums::UTILITY:
      case cmStateEnums::GLOBAL_TARGET:
        // A utility target has no file on which to depend.  This was listed
        // only to get the target-level dependency.
        return false;
    }
  }

  // The name was not that of a CMake target.  It must name a file.
  if (cmSystemTools::FileIsFullPath(inName)) {
    // This is a full path.  Return it as given.
    dep = inName;
    return true;
  }

  // Check for a source file in this directory that matches the
  // dependency.
  if (cmSourceFile* sf = this->Makefile->GetSource(inName)) {
    dep = sf->ResolveFullPath();
    return true;
  }

  // Treat the name as relative to the source directory in which it
  // was given.
  dep = cmStrCat(this->GetCurrentSourceDirectory(), '/', inName);

  // If the in-source path does not exist, assume it instead lives in the
  // binary directory.
  if (!cmSystemTools::FileExists(dep)) {
    dep = cmStrCat(this->GetCurrentBinaryDirectory(), '/', inName);
  }

  dep = cmSystemTools::CollapseFullPath(dep);

  return true;
}

static void AddVisibilityCompileOption(std::string& flags,
                                       cmGeneratorTarget const* target,
                                       cmLocalGenerator* lg,
                                       std::string const& lang)
{
  std::string compileOption = "CMAKE_" + lang + "_COMPILE_OPTIONS_VISIBILITY";
  cmValue opt = lg->GetMakefile()->GetDefinition(compileOption);
  if (!opt) {
    return;
  }
  std::string flagDefine = lang + "_VISIBILITY_PRESET";

  cmValue prop = target->GetProperty(flagDefine);
  if (!prop) {
    return;
  }
  if ((*prop != "hidden") && (*prop != "default") && (*prop != "protected") &&
      (*prop != "internal")) {
    std::ostringstream e;
    e << "Target " << target->GetName() << " uses unsupported value \""
      << *prop << "\" for " << flagDefine << "."
      << " The supported values are: default, hidden, protected, and "
         "internal.";
    cmSystemTools::Error(e.str());
    return;
  }
  std::string option = *opt + *prop;
  lg->AppendFlags(flags, option);
}

static void AddInlineVisibilityCompileOption(std::string& flags,
                                             cmGeneratorTarget const* target,
                                             cmLocalGenerator* lg,
                                             std::string const& lang)
{
  std::string compileOption =
    cmStrCat("CMAKE_", lang, "_COMPILE_OPTIONS_VISIBILITY_INLINES_HIDDEN");
  cmValue opt = lg->GetMakefile()->GetDefinition(compileOption);
  if (!opt) {
    return;
  }

  bool prop = target->GetPropertyAsBool("VISIBILITY_INLINES_HIDDEN");
  if (!prop) {
    return;
  }
  lg->AppendFlags(flags, *opt);
}

void cmLocalGenerator::AddVisibilityPresetFlags(
  std::string& flags, cmGeneratorTarget const* target, std::string const& lang)
{
  if (lang.empty()) {
    return;
  }

  AddVisibilityCompileOption(flags, target, this, lang);

  if (lang == "CXX" || lang == "OBJCXX") {
    AddInlineVisibilityCompileOption(flags, target, this, lang);
  }
}

void cmLocalGenerator::AddFeatureFlags(std::string& flags,
                                       cmGeneratorTarget const* target,
                                       std::string const& lang,
                                       std::string const& config)
{
  int targetType = target->GetType();

  bool shared = ((targetType == cmStateEnums::SHARED_LIBRARY) ||
                 (targetType == cmStateEnums::MODULE_LIBRARY));

  if (target->GetLinkInterfaceDependentBoolProperty(
        "POSITION_INDEPENDENT_CODE", config)) {
    this->AddPositionIndependentFlags(flags, lang, targetType);
  }
  if (shared) {
    this->AppendFeatureOptions(flags, lang, "DLL");
  }
}

void cmLocalGenerator::AddPositionIndependentFlags(std::string& flags,
                                                   std::string const& lang,
                                                   int targetType)
{
  std::string picFlags;

  if (targetType == cmStateEnums::EXECUTABLE) {
    picFlags = this->Makefile->GetSafeDefinition(
      cmStrCat("CMAKE_", lang, "_COMPILE_OPTIONS_PIE"));
  }
  if (picFlags.empty()) {
    picFlags = this->Makefile->GetSafeDefinition(
      cmStrCat("CMAKE_", lang, "_COMPILE_OPTIONS_PIC"));
  }
  if (!picFlags.empty()) {
    cmList options{ picFlags };
    for (std::string const& o : options) {
      this->AppendFlagEscape(flags, o);
    }
  }
}

void cmLocalGenerator::AddColorDiagnosticsFlags(std::string& flags,
                                                std::string const& lang)
{
  cmValue diag = this->Makefile->GetDefinition("CMAKE_COLOR_DIAGNOSTICS");
  if (diag.IsSet()) {
    std::string colorFlagName;
    if (diag.IsOn()) {
      colorFlagName =
        cmStrCat("CMAKE_", lang, "_COMPILE_OPTIONS_COLOR_DIAGNOSTICS");
    } else {
      colorFlagName =
        cmStrCat("CMAKE_", lang, "_COMPILE_OPTIONS_COLOR_DIAGNOSTICS_OFF");
    }

    cmList options{ this->Makefile->GetDefinition(colorFlagName) };

    for (auto const& option : options) {
      this->AppendFlagEscape(flags, option);
    }
  }
}

void cmLocalGenerator::AddConfigVariableFlags(std::string& flags,
                                              std::string const& var,
                                              std::string const& config)
{
  // Add the flags from the variable itself.
  this->AppendFlags(flags, this->Makefile->GetSafeDefinition(var));
  // Add the flags from the build-type specific variable.
  if (!config.empty()) {
    std::string const flagsVar =
      cmStrCat(var, '_', cmSystemTools::UpperCase(config));
    this->AppendFlags(flags, this->Makefile->GetSafeDefinition(flagsVar));
  }
}
void cmLocalGenerator::AddConfigVariableFlags(std::string& flags,
                                              std::string const& var,
                                              cmGeneratorTarget const* target,
                                              cmBuildStep compileOrLink,
                                              std::string const& lang,
                                              std::string const& config)
{
  std::string newFlags;
  this->AddConfigVariableFlags(newFlags, var, config);
  if (!newFlags.empty()) {
    this->AppendFlags(flags, newFlags, var, target, compileOrLink, lang);
  }
}

void cmLocalGenerator::AppendFlags(std::string& flags,
                                   std::string const& newFlags) const
{
  bool allSpaces = std::all_of(newFlags.begin(), newFlags.end(), cmIsSpace);

  if (!newFlags.empty() && !allSpaces) {
    if (!flags.empty()) {
      flags += " ";
    }
    flags += newFlags;
  }
}

void cmLocalGenerator::AppendFlags(
  std::string& flags, std::vector<BT<std::string>> const& newFlags) const
{
  for (BT<std::string> const& flag : newFlags) {
    this->AppendFlags(flags, flag.Value);
  }
}

void cmLocalGenerator::AppendFlagEscape(std::string& flags,
                                        std::string const& rawFlag) const
{
  this->AppendFlags(
    flags,
    this->EscapeForShell(rawFlag, false, false, false, this->IsNinjaMulti()));
}

void cmLocalGenerator::AppendFlags(std::string& flags,
                                   std::string const& newFlags,
                                   std::string const& name,
                                   cmGeneratorTarget const* target,
                                   cmBuildStep compileOrLink,
                                   std::string const& language)
{
  switch (target->GetPolicyStatusCMP0181()) {
    case cmPolicies::WARN:
      if (!this->Makefile->GetCMakeInstance()->GetIsInTryCompile() &&
          this->Makefile->PolicyOptionalWarningEnabled(
            "CMAKE_POLICY_WARNING_CMP0181")) {
        this->Makefile->GetCMakeInstance()->IssueMessage(
          MessageType::AUTHOR_WARNING,
          cmStrCat(cmPolicies::GetPolicyWarning(cmPolicies::CMP0181),
                   "\nSince the policy is not set, the contents of variable '",
                   name,
                   "' will "
                   "be used as is."),
          target->GetBacktrace());
      }
      CM_FALLTHROUGH;
    case cmPolicies::OLD:
      this->AppendFlags(
        flags, this->GetGlobalGenerator()->GetEncodedLiteral(newFlags));
      break;
    case cmPolicies::NEW:
      if (compileOrLink == cmBuildStep::Link) {
        std::vector<std::string> options;
        cmSystemTools::ParseUnixCommandLine(newFlags.c_str(), options);
        this->SetLinkScriptShell(this->GlobalGenerator->GetUseLinkScript());
        std::vector<BT<std::string>> optionsWithBT{ options.size() };
        std::transform(options.cbegin(), options.cend(), optionsWithBT.begin(),
                       [](std::string const& item) -> BT<std::string> {
                         return BT<std::string>{ item };
                       });
        target->ResolveLinkerWrapper(optionsWithBT, language);
        for (auto const& item : optionsWithBT) {
          this->AppendFlagEscape(flags, item.Value);
        }
        this->SetLinkScriptShell(false);
      } else {
        this->AppendFlags(flags, newFlags);
      }
  }
}

void cmLocalGenerator::AddISPCDependencies(cmGeneratorTarget* target)
{
  std::vector<std::string> enabledLanguages =
    this->GetState()->GetEnabledLanguages();
  if (std::find(enabledLanguages.begin(), enabledLanguages.end(), "ISPC") ==
      enabledLanguages.end()) {
    return;
  }

  cmValue ispcHeaderSuffixProp = target->GetProperty("ISPC_HEADER_SUFFIX");
  assert(ispcHeaderSuffixProp);

  std::vector<std::string> ispcArchSuffixes =
    detail::ComputeISPCObjectSuffixes(target);
  bool const extra_objects = (ispcArchSuffixes.size() > 1);

  std::vector<std::string> configsList =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
  for (std::string const& config : configsList) {

    std::string rootObjectDir = target->GetObjectDirectory(config);
    std::string headerDir = rootObjectDir;
    if (cmValue prop = target->GetProperty("ISPC_HEADER_DIRECTORY")) {
      headerDir = cmSystemTools::CollapseFullPath(
        cmStrCat(this->GetBinaryDirectory(), '/', *prop));
    }

    std::vector<cmSourceFile*> sources;
    target->GetSourceFiles(sources, config);

    // build up the list of ispc headers and extra objects that this target is
    // generating
    for (cmSourceFile const* sf : sources) {
      // Generate this object file's rule file.
      std::string const& lang = sf->GetLanguage();
      if (lang == "ISPC") {
        std::string const& objectName = target->GetObjectName(sf);

        // Drop both ".obj" and the source file extension
        std::string ispcSource =
          cmSystemTools::GetFilenameWithoutLastExtension(objectName);
        ispcSource =
          cmSystemTools::GetFilenameWithoutLastExtension(ispcSource);

        auto headerPath =
          cmStrCat(headerDir, '/', ispcSource, *ispcHeaderSuffixProp);
        target->AddISPCGeneratedHeader(headerPath, config);
        if (extra_objects) {
          std::vector<std::string> objs = detail::ComputeISPCExtraObjects(
            objectName, rootObjectDir, ispcArchSuffixes);
          target->AddISPCGeneratedObject(std::move(objs), config);
        }
      }
    }
  }
}

void cmLocalGenerator::AddPchDependencies(cmGeneratorTarget* target)
{
  std::vector<std::string> configsList =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);

  for (std::string const& config : configsList) {
    // FIXME: Refactor collection of sources to not evaluate object
    // libraries.
    std::vector<cmSourceFile*> sources;
    target->GetSourceFiles(sources, config);

    std::string const configUpper = cmSystemTools::UpperCase(config);
    static std::array<std::string, 4> const langs = { { "C", "CXX", "OBJC",
                                                        "OBJCXX" } };

    std::set<std::string> pchLangSet;
    if (this->GetGlobalGenerator()->IsXcode()) {
      for (std::string const& lang : langs) {
        std::string const pchHeader = target->GetPchHeader(config, lang, "");
        if (!pchHeader.empty()) {
          pchLangSet.emplace(lang);
        }
      }
    }

    for (std::string const& lang : langs) {
      auto langSources = std::count_if(
        sources.begin(), sources.end(), [lang](cmSourceFile* sf) {
          return lang == sf->GetLanguage() &&
            !sf->GetProperty("SKIP_PRECOMPILE_HEADERS");
        });
      if (langSources == 0) {
        continue;
      }

      std::vector<std::string> pchArchs = target->GetPchArchs(config, lang);
      if (pchArchs.size() > 1) {
        std::string useMultiArchPch;
        for (std::string const& arch : pchArchs) {
          std::string const pchHeader =
            target->GetPchHeader(config, lang, arch);
          if (!pchHeader.empty()) {
            useMultiArchPch = cmStrCat(useMultiArchPch, ";-Xarch_", arch,
                                       ";-include", pchHeader);
          }
        }

        if (!useMultiArchPch.empty()) {

          target->Target->AppendProperty(
            cmStrCat(lang, "_COMPILE_OPTIONS_USE_PCH"),
            cmStrCat("$<$<CONFIG:", config, ">:", useMultiArchPch, '>'));
        }
      }

      for (std::string const& arch : pchArchs) {
        std::string const pchSource = target->GetPchSource(config, lang, arch);
        std::string const pchHeader = target->GetPchHeader(config, lang, arch);

        if (pchSource.empty() || pchHeader.empty()) {
          if (this->GetGlobalGenerator()->IsXcode() && !pchLangSet.empty()) {
            for (auto* sf : sources) {
              auto const sourceLanguage = sf->GetLanguage();
              if (!sourceLanguage.empty() &&
                  pchLangSet.find(sourceLanguage) == pchLangSet.end()) {
                sf->SetProperty("SKIP_PRECOMPILE_HEADERS", "ON");
              }
            }
          }
          continue;
        }

        cmValue pchExtension =
          this->Makefile->GetDefinition("CMAKE_PCH_EXTENSION");

        if (pchExtension.IsEmpty()) {
          continue;
        }

        auto* reuseTarget = target->GetPchReuseTarget();
        bool const haveReuseTarget = reuseTarget && reuseTarget != target;

        auto* pch_sf = this->Makefile->GetOrCreateSource(
          pchSource, false, cmSourceFileLocationKind::Known);
        // PCH sources should never be scanned as they cannot contain C++
        // module references.
        pch_sf->SetProperty("CXX_SCAN_FOR_MODULES", "0");

        if (!this->GetGlobalGenerator()->IsXcode()) {
          if (!haveReuseTarget) {
            target->AddSource(pchSource, true);
          }

          std::string const pchFile = target->GetPchFile(config, lang, arch);

          // Exclude the pch files from linking
          if (this->Makefile->IsOn("CMAKE_LINK_PCH")) {
            if (!haveReuseTarget) {
              pch_sf->AppendProperty(
                "OBJECT_OUTPUTS",
                cmStrCat("$<$<CONFIG:", config, ">:", pchFile, '>'));
            } else {
              if (this->Makefile->IsOn("CMAKE_PCH_COPY_COMPILE_PDB")) {

                std::string const compilerId =
                  this->Makefile->GetSafeDefinition(
                    cmStrCat("CMAKE_", lang, "_COMPILER_ID"));

                std::string const compilerVersion =
                  this->Makefile->GetSafeDefinition(
                    cmStrCat("CMAKE_", lang, "_COMPILER_VERSION"));

                std::string const langFlags =
                  this->Makefile->GetSafeDefinition(
                    cmStrCat("CMAKE_", lang, "_FLAGS_", configUpper));

                bool editAndContinueDebugInfo = false;
                bool programDatabaseDebugInfo = false;
                cm::optional<std::string> msvcDebugInformationFormat =
                  this->GetMSVCDebugFormatName(config, target);
                if (msvcDebugInformationFormat &&
                    !msvcDebugInformationFormat->empty()) {
                  editAndContinueDebugInfo =
                    *msvcDebugInformationFormat == "EditAndContinue";
                  programDatabaseDebugInfo =
                    *msvcDebugInformationFormat == "ProgramDatabase";
                } else {
                  editAndContinueDebugInfo =
                    langFlags.find("/ZI") != std::string::npos ||
                    langFlags.find("-ZI") != std::string::npos;
                  programDatabaseDebugInfo =
                    langFlags.find("/Zi") != std::string::npos ||
                    langFlags.find("-Zi") != std::string::npos;
                }

                // MSVC 2008 is producing both .pdb and .idb files with /Zi.
                bool msvc2008OrLess =
                  cmSystemTools::VersionCompare(cmSystemTools::OP_LESS,
                                                compilerVersion, "16.0") &&
                  compilerId == "MSVC";
                // but not when used via toolset -Tv90
                if (this->Makefile->GetSafeDefinition(
                      "CMAKE_VS_PLATFORM_TOOLSET") == "v90") {
                  msvc2008OrLess = false;
                }

                if (editAndContinueDebugInfo || msvc2008OrLess) {
                  this->CopyPchCompilePdb(config, lang, target, reuseTarget,
                                          { ".pdb", ".idb" });
                } else if (programDatabaseDebugInfo) {
                  this->CopyPchCompilePdb(config, lang, target, reuseTarget,
                                          { ".pdb" });
                }
              }

              // Link to the pch object file
              std::string pchSourceObj =
                reuseTarget->GetPchFileObject(config, lang, arch);

              if (target->GetType() != cmStateEnums::OBJECT_LIBRARY) {
                std::string linkerProperty = "LINK_FLAGS_";
                if (target->GetType() == cmStateEnums::STATIC_LIBRARY) {
                  linkerProperty = "STATIC_LIBRARY_FLAGS_";
                }
                target->Target->AppendProperty(
                  cmStrCat(linkerProperty, configUpper),
                  cmStrCat(' ',
                           this->ConvertToOutputFormat(pchSourceObj, SHELL)),
                  cm::nullopt, true);
              } else if (reuseTarget->GetType() ==
                         cmStateEnums::OBJECT_LIBRARY) {
                target->Target->AppendProperty(
                  "INTERFACE_LINK_LIBRARIES",
                  cmStrCat("$<$<CONFIG:", config,
                           ">:$<LINK_ONLY:", pchSourceObj, ">>"));
              }
            }
          } else {
            pch_sf->SetProperty("PCH_EXTENSION", pchExtension);
          }

          // Add pchHeader to source files, which will
          // be grouped as "Precompile Header File"
          auto* pchHeader_sf = this->Makefile->GetOrCreateSource(
            pchHeader, false, cmSourceFileLocationKind::Known);
          std::string err;
          pchHeader_sf->ResolveFullPath(&err);
          if (!err.empty()) {
            std::ostringstream msg;
            msg << "Unable to resolve full path of PCH-header '" << pchHeader
                << "' assigned to target " << target->GetName()
                << ", although its path is supposed to be known!";
            this->IssueMessage(MessageType::FATAL_ERROR, msg.str());
          }
          target->AddSource(pchHeader);
        }
      }
    }
  }
}

void cmLocalGenerator::CopyPchCompilePdb(
  std::string const& config, std::string const& language,
  cmGeneratorTarget* target, cmGeneratorTarget* reuseTarget,
  std::vector<std::string> const& extensions)
{
  std::string const copy_script = cmStrCat(target->GetSupportDirectory(),
                                           "/copy_idb_pdb_", config, ".cmake");
  cmGeneratedFileStream file(copy_script);

  file << "# CMake generated file\n";

  file << "# The compiler generated pdb file needs to be written to disk\n"
       << "# by mspdbsrv. The foreach retry loop is needed to make sure\n"
       << "# the pdb file is ready to be copied.\n\n";

  auto configGenex = [&](cm::string_view expr) -> std::string {
    if (this->GetGlobalGenerator()->IsMultiConfig()) {
      return cmStrCat("$<$<CONFIG:", config, ">:", expr, '>');
    }
    return std::string(expr);
  };

  std::vector<std::string> outputs;
  auto replaceExtension = [](std::string const& path,
                             std::string const& ext) -> std::string {
    auto const dir = cmSystemTools::GetFilenamePath(path);
    auto const base = cmSystemTools::GetFilenameWithoutLastExtension(path);
    if (dir.empty()) {
      return cmStrCat(base, ext);
    }
    return cmStrCat(dir, '/', base, ext);
  };
  for (auto const& extension : extensions) {
    std::string const from_file =
      replaceExtension(reuseTarget->GetCompilePDBPath(config), extension);
    std::string const to_dir = target->GetCompilePDBDirectory(config);
    std::string const to_file = cmStrCat(
      replaceExtension(reuseTarget->GetCompilePDBName(config), extension),
      '/');
    std::string const dest_file = cmStrCat(to_dir, to_file);

    file << "foreach(retry RANGE 1 30)\n";
    file << "  if (EXISTS \"" << from_file << "\" AND (NOT EXISTS \""
         << dest_file << "\" OR NOT \"" << dest_file << "\" IS_NEWER_THAN \""
         << from_file << "\"))\n";
    file << "    file(MAKE_DIRECTORY \"" << to_dir << "\")\n";
    file << "    execute_process(COMMAND ${CMAKE_COMMAND} -E copy";
    file << " \"" << from_file << "\""
         << " \"" << to_dir << "\" RESULT_VARIABLE result "
         << " ERROR_QUIET)\n";
    file << "    if (NOT result EQUAL 0)\n"
         << "      execute_process(COMMAND ${CMAKE_COMMAND}"
         << " -E sleep 1)\n"
         << "    else()\n";
    file << "      break()\n"
         << "    endif()\n";
    file << "  elseif(NOT EXISTS \"" << from_file << "\")\n"
         << "    execute_process(COMMAND ${CMAKE_COMMAND}"
         << " -E sleep 1)\n"
         << "  endif()\n";
    file << "endforeach()\n";
    outputs.push_back(configGenex(dest_file));
  }

  cmCustomCommandLines commandLines =
    cmMakeSingleCommandLine({ configGenex(cmSystemTools::GetCMakeCommand()),
                              configGenex("-P"), configGenex(copy_script) });

  auto const comment =
    cmStrCat("Copying PDB for PCH reuse from ", reuseTarget->GetName(),
             " for ", target->GetName());
  ;

  auto cc = cm::make_unique<cmCustomCommand>();
  cc->SetCommandLines(commandLines);
  cc->SetComment(comment.c_str());
  cc->SetStdPipesUTF8(true);
  cc->AppendDepends(
    { reuseTarget->GetPchFile(config, language), copy_script });

  if (this->GetGlobalGenerator()->IsVisualStudio()) {
    cc->SetByproducts(outputs);
    this->AddCustomCommandToTarget(
      target->GetName(), cmCustomCommandType::PRE_BUILD, std::move(cc),
      cmObjectLibraryCommands::Accept);
  } else {
    cc->SetOutputs(outputs);
    cmSourceFile* copy_rule = this->AddCustomCommandToOutput(std::move(cc));
    copy_rule->SetProperty("CXX_SCAN_FOR_MODULES", "0");

    if (copy_rule) {
      target->AddSource(copy_rule->ResolveFullPath());
    }
  }
}

cm::optional<std::string> cmLocalGenerator::GetMSVCDebugFormatName(
  std::string const& config, cmGeneratorTarget const* target)
{
  // MSVC debug information format selection is activated by the presence
  // of a default whether or not it is overridden by a property.
  cm::optional<std::string> msvcDebugInformationFormat;
  cmValue msvcDebugInformationFormatDefault = this->Makefile->GetDefinition(
    "CMAKE_MSVC_DEBUG_INFORMATION_FORMAT_DEFAULT");
  if (cmNonempty(msvcDebugInformationFormatDefault)) {
    cmValue msvcDebugInformationFormatValue =
      target->GetProperty("MSVC_DEBUG_INFORMATION_FORMAT");
    if (!msvcDebugInformationFormatValue) {
      msvcDebugInformationFormatValue = msvcDebugInformationFormatDefault;
    }
    msvcDebugInformationFormat = cmGeneratorExpression::Evaluate(
      *msvcDebugInformationFormatValue, this, config, target);
  }
  return msvcDebugInformationFormat;
}

cm::optional<cmSwiftCompileMode> cmLocalGenerator::GetSwiftCompileMode(
  cmGeneratorTarget const* target, std::string const& config)
{
  cmMakefile const* mf = this->GetMakefile();
  cmValue const swiftCompileModeDefault =
    mf->GetDefinition("CMAKE_Swift_COMPILATION_MODE_DEFAULT");
  if (!cmNonempty(swiftCompileModeDefault)) {
    return {};
  }
  cmValue swiftCompileMode = target->GetProperty("Swift_COMPILATION_MODE");
  if (!swiftCompileMode) {
    swiftCompileMode = swiftCompileModeDefault;
  }

  std::string const expandedCompileMode =
    cmGeneratorExpression::Evaluate(*swiftCompileMode, this, config, target);
  if (expandedCompileMode == "wholemodule") {
    return cmSwiftCompileMode::Wholemodule;
  }
  if (expandedCompileMode == "singlefile") {
    return cmSwiftCompileMode::Singlefile;
  }
  if (expandedCompileMode == "incremental") {
    return cmSwiftCompileMode::Incremental;
  }
  return cmSwiftCompileMode::Unknown;
}

bool cmLocalGenerator::IsSplitSwiftBuild() const
{
  return cmNonempty(this->GetMakefile()->GetDefinition(
    "CMAKE_Swift_COMPILATION_MODE_DEFAULT"));
}

namespace {

inline void RegisterUnitySources(cmGeneratorTarget* target, cmSourceFile* sf,
                                 std::string const& filename)
{
  target->AddSourceFileToUnityBatch(sf->ResolveFullPath());
  sf->SetProperty("UNITY_SOURCE_FILE", filename);
}
}

cmLocalGenerator::UnitySource cmLocalGenerator::WriteUnitySource(
  cmGeneratorTarget* target, std::vector<std::string> const& configs,
  cmRange<std::vector<UnityBatchedSource>::const_iterator> sources,
  cmValue beforeInclude, cmValue afterInclude, std::string filename,
  std::string const& unityFileDirectory, UnityPathMode pathMode) const
{
  cmValue uniqueIdName = target->GetProperty("UNITY_BUILD_UNIQUE_ID");
  cmGeneratedFileStream file(
    filename, false, target->GetGlobalGenerator()->GetMakefileEncoding());
  file.SetCopyIfDifferent(true);
  file << "/* generated by CMake */\n\n";

  bool perConfig = false;
  for (UnityBatchedSource const& ubs : sources) {
    cm::optional<std::string> cond;
    if (ubs.Configs.size() != configs.size()) {
      perConfig = true;
      cond = std::string();
      cm::string_view sep;
      for (size_t ci : ubs.Configs) {
        cond = cmStrCat(*cond, sep, "defined(CMAKE_UNITY_CONFIG_",
                        cmSystemTools::UpperCase(configs[ci]), ')');
        sep = " || "_s;
      }
    }
    RegisterUnitySources(target, ubs.Source, filename);
    WriteUnitySourceInclude(file, cond, ubs.Source->ResolveFullPath(),
                            beforeInclude, afterInclude, uniqueIdName,
                            pathMode, unityFileDirectory);
  }

  return UnitySource(std::move(filename), perConfig);
}

void cmLocalGenerator::WriteUnitySourceInclude(
  std::ostream& unity_file, cm::optional<std::string> const& cond,
  std::string const& sf_full_path, cmValue beforeInclude, cmValue afterInclude,
  cmValue uniqueIdName, UnityPathMode pathMode,
  std::string const& unityFileDirectory) const
{
  if (cond) {
    unity_file << "#if " << *cond << "\n";
  }

  std::string pathToHash;
  std::string relocatableIncludePath;
  auto PathEqOrSubDir = [](std::string const& a, std::string const& b) {
    return (cmSystemTools::ComparePath(a, b) ||
            cmSystemTools::IsSubDirectory(a, b));
  };
  auto const path = cmSystemTools::GetFilenamePath(sf_full_path);
  if (PathEqOrSubDir(path, this->GetBinaryDirectory())) {
    relocatableIncludePath =
      cmSystemTools::RelativePath(unityFileDirectory, sf_full_path);
    pathToHash = "BLD_" +
      cmSystemTools::RelativePath(this->GetBinaryDirectory(), sf_full_path);
  } else if (PathEqOrSubDir(path, this->GetSourceDirectory())) {
    relocatableIncludePath =
      cmSystemTools::RelativePath(this->GetSourceDirectory(), sf_full_path);
    pathToHash = "SRC_" + relocatableIncludePath;
  } else {
    relocatableIncludePath = sf_full_path;
    pathToHash = "ABS_" + sf_full_path;
  }

  if (cmNonempty(uniqueIdName)) {
    cmCryptoHash hasher(cmCryptoHash::AlgoMD5);
    unity_file << "/* " << pathToHash << " */\n"
               << "#undef " << *uniqueIdName << "\n"
               << "#define " << *uniqueIdName << " unity_"
               << hasher.HashString(pathToHash) << "\n";
  }

  if (beforeInclude) {
    unity_file << *beforeInclude << "\n";
  }

  // clang-tidy-17 has new include checks that needs NOLINT too.
  unity_file
    << "/* NOLINTNEXTLINE(bugprone-suspicious-include,misc-include-cleaner) "
       "*/\n";
  if (pathMode == UnityPathMode::Relative) {
    unity_file << "#include \"" << relocatableIncludePath << "\"\n";
  } else {
    unity_file << "#include \"" << sf_full_path << "\"\n";
  }

  if (afterInclude) {
    unity_file << *afterInclude << "\n";
  }
  if (cond) {
    unity_file << "#endif\n";
  }
  unity_file << "\n";
}

namespace {
std::string unity_file_extension(std::string const& lang)
{
  std::string extension;
  if (lang == "C") {
    extension = "_c.c";
  } else if (lang == "CXX") {
    extension = "_cxx.cxx";
  } else if (lang == "CUDA") {
    extension = "_cu.cu";
  } else if (lang == "OBJC") {
    extension = "_m.m";
  } else if (lang == "OBJCXX") {
    extension = "_mm.mm";
  }
  return extension;
}
}

std::vector<cmLocalGenerator::UnitySource>
cmLocalGenerator::AddUnityFilesModeAuto(
  cmGeneratorTarget* target, std::string const& lang,
  std::vector<std::string> const& configs,
  std::vector<UnityBatchedSource> const& filtered_sources,
  cmValue beforeInclude, cmValue afterInclude,
  std::string const& filename_base, UnityPathMode pathMode, size_t batchSize)
{
  if (batchSize == 0) {
    batchSize = filtered_sources.size();
  }

  std::vector<UnitySource> unity_files;
  for (size_t itemsLeft = filtered_sources.size(), chunk, batch = 0;
       itemsLeft > 0; itemsLeft -= chunk, ++batch) {

    chunk = std::min(itemsLeft, batchSize);

    std::string filename =
      cmStrCat(filename_base, "unity_", batch, unity_file_extension(lang));
    auto const begin = filtered_sources.begin() + batch * batchSize;
    auto const end = begin + chunk;
    unity_files.emplace_back(this->WriteUnitySource(
      target, configs, cmMakeRange(begin, end), beforeInclude, afterInclude,
      std::move(filename), filename_base, pathMode));
  }
  return unity_files;
}

std::vector<cmLocalGenerator::UnitySource>
cmLocalGenerator::AddUnityFilesModeGroup(
  cmGeneratorTarget* target, std::string const& lang,
  std::vector<std::string> const& configs,
  std::vector<UnityBatchedSource> const& filtered_sources,
  cmValue beforeInclude, cmValue afterInclude,
  std::string const& filename_base, UnityPathMode pathMode)
{
  std::vector<UnitySource> unity_files;

  // sources organized by group name. Drop any source
  // without a group
  std::unordered_map<std::string, std::vector<UnityBatchedSource>>
    explicit_mapping;
  for (UnityBatchedSource const& ubs : filtered_sources) {
    if (cmValue value = ubs.Source->GetProperty("UNITY_GROUP")) {
      auto i = explicit_mapping.find(*value);
      if (i == explicit_mapping.end()) {
        std::vector<UnityBatchedSource> sources{ ubs };
        explicit_mapping.emplace(*value, std::move(sources));
      } else {
        i->second.emplace_back(ubs);
      }
    }
  }

  for (auto const& item : explicit_mapping) {
    auto const& name = item.first;
    std::string filename =
      cmStrCat(filename_base, "unity_", name, unity_file_extension(lang));
    unity_files.emplace_back(this->WriteUnitySource(
      target, configs, cmMakeRange(item.second), beforeInclude, afterInclude,
      std::move(filename), filename_base, pathMode));
  }

  return unity_files;
}

void cmLocalGenerator::AddUnityBuild(cmGeneratorTarget* target)
{
  if (!target->GetPropertyAsBool("UNITY_BUILD")) {
    return;
  }

  std::vector<UnityBatchedSource> unitySources;

  std::vector<std::string> configs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);

  std::map<cmSourceFile const*, size_t> index;

  for (size_t ci = 0; ci < configs.size(); ++ci) {
    // FIXME: Refactor collection of sources to not evaluate object libraries.
    // Their final set of object files might be transformed by unity builds.
    std::vector<cmSourceFile*> sources;
    target->GetSourceFiles(sources, configs[ci]);
    for (cmSourceFile* sf : sources) {
      // Files which need C++ scanning cannot participate in unity builds as
      // there is a single place in TUs that may perform module-dependency bits
      // and a unity source cannot `#include` them in-order and represent a
      // valid TU.
      if (sf->GetLanguage() == "CXX"_s &&
          target->NeedDyndepForSource("CXX", configs[ci], sf)) {
        continue;
      }

      auto mi = index.find(sf);
      if (mi == index.end()) {
        unitySources.emplace_back(sf);
        std::map<cmSourceFile const*, size_t>::value_type entry(
          sf, unitySources.size() - 1);
        mi = index.insert(entry).first;
      }
      unitySources[mi->second].Configs.emplace_back(ci);
    }
  }

  std::string filename_base =
    cmStrCat(target->GetCMFSupportDirectory(), "/Unity/");

  cmValue batchSizeString = target->GetProperty("UNITY_BUILD_BATCH_SIZE");
  size_t const unityBatchSize = batchSizeString
    ? static_cast<size_t>(std::atoi(batchSizeString->c_str()))
    : 0;

  cmValue beforeInclude =
    target->GetProperty("UNITY_BUILD_CODE_BEFORE_INCLUDE");
  cmValue afterInclude = target->GetProperty("UNITY_BUILD_CODE_AFTER_INCLUDE");
  cmValue unityMode = target->GetProperty("UNITY_BUILD_MODE");
  UnityPathMode pathMode = target->GetPropertyAsBool("UNITY_BUILD_RELOCATABLE")
    ? UnityPathMode::Relative
    : UnityPathMode::Absolute;

  for (std::string lang : { "C", "CXX", "OBJC", "OBJCXX", "CUDA" }) {
    std::vector<UnityBatchedSource> filtered_sources;
    std::copy_if(unitySources.begin(), unitySources.end(),
                 std::back_inserter(filtered_sources),
                 [&](UnityBatchedSource const& ubs) -> bool {
                   cmSourceFile* sf = ubs.Source;
                   return sf->GetLanguage() == lang &&
                     !sf->GetPropertyAsBool("SKIP_UNITY_BUILD_INCLUSION") &&
                     !sf->GetPropertyAsBool("HEADER_FILE_ONLY") &&
                     !sf->GetProperty("COMPILE_OPTIONS") &&
                     !sf->GetProperty("COMPILE_DEFINITIONS") &&
                     !sf->GetProperty("COMPILE_FLAGS") &&
                     !sf->GetProperty("INCLUDE_DIRECTORIES");
                 });

    std::vector<UnitySource> unity_files;
    if (!unityMode || *unityMode == "BATCH") {
      unity_files = AddUnityFilesModeAuto(
        target, lang, configs, filtered_sources, beforeInclude, afterInclude,
        filename_base, pathMode, unityBatchSize);
    } else if (unityMode && *unityMode == "GROUP") {
      unity_files = AddUnityFilesModeGroup(
        target, lang, configs, filtered_sources, beforeInclude, afterInclude,
        filename_base, pathMode);
    } else {
      // unity mode is set to an unsupported value
      std::string e("Invalid UNITY_BUILD_MODE value of " + *unityMode +
                    " assigned to target " + target->GetName() +
                    ". Acceptable values are BATCH and GROUP.");
      this->IssueMessage(MessageType::FATAL_ERROR, e);
    }

    for (UnitySource const& file : unity_files) {
      auto* unity = this->GetMakefile()->GetOrCreateSource(file.Path);
      target->AddSource(file.Path, true);
      unity->SetProperty("SKIP_UNITY_BUILD_INCLUSION", "ON");
      unity->SetProperty("UNITY_SOURCE_FILE", file.Path);
      unity->SetProperty("CXX_SCAN_FOR_MODULES", "0");
      if (file.PerConfig) {
        unity->SetProperty("COMPILE_DEFINITIONS",
                           "CMAKE_UNITY_CONFIG_$<UPPER_CASE:$<CONFIG>>");
      }

      if (pathMode == UnityPathMode::Relative) {
        unity->AppendProperty("INCLUDE_DIRECTORIES",
                              this->GetSourceDirectory(), false);
      }
    }
  }
}

void cmLocalGenerator::AppendTargetCreationLinkFlags(
  std::string& flags, cmGeneratorTarget const* target,
  std::string const& linkLanguage)
{
  std::string createFlagsVar;
  cmValue createFlagsVal;
  switch (target->GetType()) {
    case cmStateEnums::STATIC_LIBRARY:
      break;
    case cmStateEnums::MODULE_LIBRARY:
      createFlagsVar =
        cmStrCat("CMAKE_SHARED_MODULE_CREATE_", linkLanguage, "_FLAGS");
      createFlagsVal = this->Makefile->GetDefinition(createFlagsVar);
      // On some platforms we use shared library creation flags for modules.
      CM_FALLTHROUGH;
    case cmStateEnums::SHARED_LIBRARY:
      if (!createFlagsVal) {
        createFlagsVar =
          cmStrCat("CMAKE_SHARED_LIBRARY_CREATE_", linkLanguage, "_FLAGS");
        createFlagsVal = this->Makefile->GetDefinition(createFlagsVar);
      }
      break;
    case cmStateEnums::EXECUTABLE:
      createFlagsVar = cmStrCat("CMAKE_", linkLanguage, "_LINK_FLAGS");
      createFlagsVal = this->Makefile->GetDefinition(createFlagsVar);
      break;
    default:
      break;
  }
  if (createFlagsVal) {
    this->AppendFlags(flags, *createFlagsVal, createFlagsVar, target,
                      cmBuildStep::Link, linkLanguage);
  }
}

void cmLocalGenerator::AppendLinkerTypeFlags(std::string& flags,
                                             cmGeneratorTarget* target,
                                             std::string const& config,
                                             std::string const& linkLanguage)
{
  switch (target->GetType()) {
    case cmStateEnums::EXECUTABLE:
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY:
      break;
    default:
      return;
  }

  auto linkMode =
    cmStrCat("CMAKE_", linkLanguage, target->IsDeviceLink() ? "_DEVICE_" : "_",
             "LINK_MODE");
  auto mode = this->Makefile->GetDefinition(linkMode);
  if (mode && mode != "DRIVER"_s) {
    return;
  }

  auto linkerType = target->GetLinkerTypeProperty(linkLanguage, config);
  if (linkerType.empty()) {
    linkerType = "DEFAULT";
  }
  auto usingLinker =
    cmStrCat("CMAKE_", linkLanguage, "_USING_",
             target->IsDeviceLink() ? "DEVICE_" : "", "LINKER_", linkerType);
  auto linkerTypeFlags = this->Makefile->GetDefinition(usingLinker);
  if (linkerTypeFlags) {
    if (!linkerTypeFlags.IsEmpty()) {
      auto linkerFlags = cmExpandListWithBacktrace(linkerTypeFlags);
      target->ResolveLinkerWrapper(linkerFlags, linkLanguage);
      this->AppendFlags(flags, linkerFlags);
    }
  } else if (linkerType != "DEFAULT"_s) {
    auto isCMakeLinkerType = [](std::string const& type) -> bool {
      return std::all_of(type.cbegin(), type.cend(),
                         [](char c) { return std::isupper(c); });
    };
    if (isCMakeLinkerType(linkerType)) {
      this->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("LINKER_TYPE '", linkerType,
                 "' is unknown or not supported by this toolchain."));
    } else {
      this->IssueMessage(
        MessageType::FATAL_ERROR,
        cmStrCat("LINKER_TYPE '", linkerType,
                 "' is unknown. Did you forget to define the '", usingLinker,
                 "' variable?"));
    }
  }
}

void cmLocalGenerator::AppendIPOLinkerFlags(std::string& flags,
                                            cmGeneratorTarget* target,
                                            std::string const& config,
                                            std::string const& lang)
{
  if (!target->IsIPOEnabled(lang, config)) {
    return;
  }

  switch (target->GetType()) {
    case cmStateEnums::EXECUTABLE:
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY:
      break;
    default:
      return;
  }

  std::string const name = "CMAKE_" + lang + "_LINK_OPTIONS_IPO";
  cmValue rawFlagsList = this->Makefile->GetDefinition(name);
  if (!rawFlagsList) {
    return;
  }

  cmList flagsList{ *rawFlagsList };
  for (std::string const& o : flagsList) {
    this->AppendFlagEscape(flags, o);
  }
}

void cmLocalGenerator::AppendPositionIndependentLinkerFlags(
  std::string& flags, cmGeneratorTarget* target, std::string const& config,
  std::string const& lang)
{
  // For now, only EXECUTABLE is concerned
  if (target->GetType() != cmStateEnums::EXECUTABLE) {
    return;
  }

  char const* PICValue = target->GetLinkPIEProperty(config);
  if (!PICValue) {
    // POSITION_INDEPENDENT_CODE is not set
    return;
  }

  std::string const mode = cmIsOn(PICValue) ? "PIE" : "NO_PIE";

  std::string supported = "CMAKE_" + lang + "_LINK_" + mode + "_SUPPORTED";
  if (this->Makefile->GetDefinition(supported).IsOff()) {
    return;
  }

  std::string name = "CMAKE_" + lang + "_LINK_OPTIONS_" + mode;

  auto pieFlags = this->Makefile->GetSafeDefinition(name);
  if (pieFlags.empty()) {
    return;
  }

  cmList flagsList{ pieFlags };
  for (auto const& flag : flagsList) {
    this->AppendFlagEscape(flags, flag);
  }
}

void cmLocalGenerator::AppendWarningAsErrorLinkerFlags(
  std::string& flags, cmGeneratorTarget* target, std::string const& lang)
{
  if (this->GetCMakeInstance()->GetIgnoreLinkWarningAsError()) {
    return;
  }

  switch (target->GetType()) {
    case cmStateEnums::EXECUTABLE:
    case cmStateEnums::SHARED_LIBRARY:
    case cmStateEnums::MODULE_LIBRARY:
      break;
    default:
      return;
  }

  auto const wError = target->GetProperty("LINK_WARNING_AS_ERROR");
  if (wError.IsOff()) {
    return;
  }
  cmList wErrorOptions;
  if (wError.IsOn()) {
    wErrorOptions = { "DRIVER", "LINKER" };
  } else {
    wErrorOptions = wError;
    std::sort(wErrorOptions.begin(), wErrorOptions.end());
    wErrorOptions.erase(
      std::unique(wErrorOptions.begin(), wErrorOptions.end()),
      wErrorOptions.end());
  }

  auto linkModeIsDriver =
    this->Makefile->GetDefinition(cmStrCat("CMAKE_", lang, "_LINK_MODE")) ==
    "DRIVER"_s;
  std::string errorMessage;
  for (auto const& option : wErrorOptions) {
    if (option != "DRIVER"_s && option != "LINKER"_s) {
      errorMessage += cmStrCat("  ", option, '\n');
      continue;
    }

    if (option == "DRIVER"_s && !linkModeIsDriver) {
      continue;
    }

    auto const wErrorOpts = this->Makefile->GetDefinition(cmStrCat(
      "CMAKE_", lang, '_', (option == "DRIVER"_s ? "COMPILE" : "LINK"),
      "_OPTIONS_WARNING_AS_ERROR"));
    if (wErrorOpts.IsSet()) {
      auto items =
        cmExpandListWithBacktrace(wErrorOpts, target->GetBacktrace());
      if (option == "LINKER"_s) {
        target->ResolveLinkerWrapper(items, lang);
      }
      for (auto const& item : items) {
        this->AppendFlagEscape(flags, item.Value);
      }
    }
  }
  if (!errorMessage.empty()) {
    this->Makefile->GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat(
        "Erroneous value(s) for 'LINK_WARNING_AS_ERROR' property of target '",
        target->GetName(), "':\n", errorMessage));
  }
}

void cmLocalGenerator::AppendDependencyInfoLinkerFlags(
  std::string& flags, cmGeneratorTarget* target, std::string const& config,
  std::string const& linkLanguage)
{
  if (!this->GetGlobalGenerator()->SupportsLinkerDependencyFile() ||
      !target->HasLinkDependencyFile(config)) {
    return;
  }

  auto depFlag = *this->Makefile->GetDefinition(
    cmStrCat("CMAKE_", linkLanguage, "_LINKER_DEPFILE_FLAGS"));
  if (depFlag.empty()) {
    return;
  }

  auto depFile = this->ConvertToOutputFormat(
    this->MaybeRelativeToWorkDir(this->GetLinkDependencyFile(target, config)),
    cmOutputConverter::SHELL);
  auto rulePlaceholderExpander = this->CreateRulePlaceholderExpander();
  cmRulePlaceholderExpander::RuleVariables linkDepsVariables;
  linkDepsVariables.DependencyFile = depFile.c_str();
  rulePlaceholderExpander->ExpandRuleVariables(this, depFlag,
                                               linkDepsVariables);
  auto depFlags = cmExpandListWithBacktrace(depFlag);
  target->ResolveLinkerWrapper(depFlags, linkLanguage);

  this->AppendFlags(flags, depFlags);
}

std::string cmLocalGenerator::GetLinkDependencyFile(
  cmGeneratorTarget* /*target*/, std::string const& /*config*/) const
{
  return "link.d";
}

void cmLocalGenerator::AppendModuleDefinitionFlag(
  std::string& flags, cmGeneratorTarget const* target,
  cmLinkLineComputer* linkLineComputer, std::string const& config,
  std::string const& lang)
{
  cmGeneratorTarget::ModuleDefinitionInfo const* mdi =
    target->GetModuleDefinitionInfo(config);
  if (!mdi || mdi->DefFile.empty()) {
    return;
  }

  cmValue defFileFlag = this->Makefile->GetDefinition(
    cmStrCat("CMAKE_", lang, "_LINK_DEF_FILE_FLAG"));
  if (!defFileFlag) {
    defFileFlag = this->Makefile->GetDefinition("CMAKE_LINK_DEF_FILE_FLAG");
  }
  if (!defFileFlag) {
    return;
  }

  // Append the flag and value.  Use ConvertToLinkReference to help
  // vs6's "cl -link" pass it to the linker.
  std::string flag =
    cmStrCat(*defFileFlag,
             this->ConvertToOutputFormat(
               linkLineComputer->ConvertToLinkReference(mdi->DefFile),
               cmOutputConverter::SHELL));
  this->AppendFlags(flags, flag);
}

bool cmLocalGenerator::AppendLWYUFlags(std::string& flags,
                                       cmGeneratorTarget const* target,
                                       std::string const& lang)
{
  auto useLWYU = target->GetPropertyAsBool("LINK_WHAT_YOU_USE") &&
    (target->GetType() == cmStateEnums::TargetType::EXECUTABLE ||
     target->GetType() == cmStateEnums::TargetType::SHARED_LIBRARY ||
     target->GetType() == cmStateEnums::TargetType::MODULE_LIBRARY);

  if (useLWYU) {
    auto const& lwyuFlag = this->GetMakefile()->GetSafeDefinition(
      cmStrCat("CMAKE_", lang, "_LINK_WHAT_YOU_USE_FLAG"));
    useLWYU = !lwyuFlag.empty();

    if (useLWYU) {
      std::vector<BT<std::string>> lwyuOpts;
      lwyuOpts.emplace_back(lwyuFlag);
      this->AppendFlags(flags, target->ResolveLinkerWrapper(lwyuOpts, lang));
    }
  }

  return useLWYU;
}

void cmLocalGenerator::AppendCompileOptions(std::string& options,
                                            std::string const& options_list,
                                            char const* regex) const
{
  // Short-circuit if there are no options.
  if (options_list.empty()) {
    return;
  }

  // Expand the list of options.
  cmList options_vec{ options_list };
  this->AppendCompileOptions(options, options_vec, regex);
}

void cmLocalGenerator::AppendCompileOptions(
  std::string& options, std::vector<std::string> const& options_vec,
  char const* regex) const
{
  if (regex) {
    // Filter flags upon specified reges.
    cmsys::RegularExpression r(regex);

    for (std::string const& opt : options_vec) {
      if (r.find(opt)) {
        this->AppendFlagEscape(options, opt);
      }
    }
  } else {
    for (std::string const& opt : options_vec) {
      this->AppendFlagEscape(options, opt);
    }
  }
}

void cmLocalGenerator::AppendCompileOptions(
  std::vector<BT<std::string>>& options,
  std::vector<BT<std::string>> const& options_vec, char const* regex) const
{
  if (regex) {
    // Filter flags upon specified regular expressions.
    cmsys::RegularExpression r(regex);

    for (BT<std::string> const& opt : options_vec) {
      if (r.find(opt.Value)) {
        std::string flag;
        this->AppendFlagEscape(flag, opt.Value);
        options.emplace_back(std::move(flag), opt.Backtrace);
      }
    }
  } else {
    for (BT<std::string> const& opt : options_vec) {
      std::string flag;
      this->AppendFlagEscape(flag, opt.Value);
      options.emplace_back(std::move(flag), opt.Backtrace);
    }
  }
}

void cmLocalGenerator::AppendIncludeDirectories(
  std::vector<std::string>& includes, std::string const& includes_list,
  cmSourceFile const& sourceFile) const
{
  // Short-circuit if there are no includes.
  if (includes_list.empty()) {
    return;
  }

  // Expand the list of includes.
  cmList includes_vec{ includes_list };
  this->AppendIncludeDirectories(includes, includes_vec, sourceFile);
}

void cmLocalGenerator::AppendIncludeDirectories(
  std::vector<std::string>& includes,
  std::vector<std::string> const& includes_vec,
  cmSourceFile const& sourceFile) const
{
  std::unordered_set<std::string> uniqueIncludes;

  for (std::string const& include : includes_vec) {
    if (!cmSystemTools::FileIsFullPath(include)) {
      std::ostringstream e;
      e << "Found relative path while evaluating include directories of "
           "\""
        << sourceFile.GetLocation().GetName() << "\":\n  \"" << include
        << "\"\n";

      this->IssueMessage(MessageType::FATAL_ERROR, e.str());
      return;
    }

    std::string inc = include;

    if (!cmIsOff(inc)) {
      cmSystemTools::ConvertToUnixSlashes(inc);
    }

    if (uniqueIncludes.insert(inc).second) {
      includes.push_back(std::move(inc));
    }
  }
}

void cmLocalGenerator::AppendDefines(std::set<std::string>& defines,
                                     std::string const& defines_list) const
{
  std::set<BT<std::string>> tmp;
  this->AppendDefines(tmp, cmExpandListWithBacktrace(defines_list));
  for (BT<std::string> const& i : tmp) {
    defines.emplace(i.Value);
  }
}

void cmLocalGenerator::AppendDefines(std::set<BT<std::string>>& defines,
                                     std::string const& defines_list) const
{
  // Short-circuit if there are no definitions.
  if (defines_list.empty()) {
    return;
  }

  // Expand the list of definitions.
  this->AppendDefines(defines, cmExpandListWithBacktrace(defines_list));
}

void cmLocalGenerator::AppendDefines(
  std::set<BT<std::string>>& defines,
  std::vector<BT<std::string>> const& defines_vec) const
{
  for (BT<std::string> const& d : defines_vec) {
    // Skip unsupported definitions.
    if (!this->CheckDefinition(d.Value)) {
      continue;
    }
    // remove any leading -D
    if (cmHasLiteralPrefix(d.Value, "-D")) {
      defines.emplace(d.Value.substr(2), d.Backtrace);
    } else {
      defines.insert(d);
    }
  }
}

void cmLocalGenerator::JoinDefines(std::set<std::string> const& defines,
                                   std::string& definesString,
                                   std::string const& lang)
{
  // Lookup the define flag for the current language.
  std::string dflag = "-D";
  if (!lang.empty()) {
    cmValue df =
      this->Makefile->GetDefinition(cmStrCat("CMAKE_", lang, "_DEFINE_FLAG"));
    if (cmNonempty(df)) {
      dflag = *df;
    }
  }
  char const* itemSeparator = definesString.empty() ? "" : " ";
  for (std::string const& define : defines) {
    // Append the definition with proper escaping.
    std::string def = dflag;
    if (this->GetState()->UseWatcomWMake()) {
      // The Watcom compiler does its own command line parsing instead
      // of using the windows shell rules.  Definitions are one of
      //   -DNAME
      //   -DNAME=<cpp-token>
      //   -DNAME="c-string with spaces and other characters(?@#$)"
      //
      // Watcom will properly parse each of these cases from the
      // command line without any escapes.  However we still have to
      // get the '$' and '#' characters through WMake as '$$' and
      // '$#'.
      for (char c : define) {
        if (c == '$' || c == '#') {
          def += '$';
        }
        def += c;
      }
    } else {
      // Make the definition appear properly on the command line.  Use
      // -DNAME="value" instead of -D"NAME=value" for historical reasons.
      std::string::size_type eq = define.find('=');
      def += define.substr(0, eq);
      if (eq != std::string::npos) {
        def += "=";
        def += this->EscapeForShell(define.substr(eq + 1), true);
      }
    }
    definesString += itemSeparator;
    itemSeparator = " ";
    definesString += def;
  }
}

void cmLocalGenerator::AppendFeatureOptions(std::string& flags,
                                            std::string const& lang,
                                            char const* feature)
{
  cmValue optionList = this->Makefile->GetDefinition(
    cmStrCat("CMAKE_", lang, "_COMPILE_OPTIONS_", feature));
  if (optionList) {
    cmList options{ *optionList };
    for (std::string const& o : options) {
      this->AppendFlagEscape(flags, o);
    }
  }
}

cmValue cmLocalGenerator::GetFeature(std::string const& feature,
                                     std::string const& config)
{
  std::string featureName = feature;
  // TODO: Define accumulation policy for features (prepend, append,
  // replace). Currently we always replace.
  if (!config.empty()) {
    featureName += "_";
    featureName += cmSystemTools::UpperCase(config);
  }
  cmStateSnapshot snp = this->StateSnapshot;
  while (snp.IsValid()) {
    if (cmValue value = snp.GetDirectory().GetProperty(featureName)) {
      return value;
    }
    snp = snp.GetBuildsystemDirectoryParent();
  }
  return nullptr;
}

std::string cmLocalGenerator::GetProjectName() const
{
  return this->StateSnapshot.GetProjectName();
}

std::string cmLocalGenerator::ConstructComment(
  cmCustomCommandGenerator const& ccg, char const* default_comment) const
{
  // Check for a comment provided with the command.
  if (cm::optional<std::string> comment = ccg.GetComment()) {
    return *comment;
  }

  // Construct a reasonable default comment if possible.
  if (!ccg.GetOutputs().empty()) {
    std::string comment;
    comment = "Generating ";
    char const* sep = "";
    for (std::string const& o : ccg.GetOutputs()) {
      comment += sep;
      comment += this->MaybeRelativeToCurBinDir(o);
      sep = ", ";
    }
    return comment;
  }

  // Otherwise use the provided default.
  return default_comment;
}

class cmInstallTargetGeneratorLocal : public cmInstallTargetGenerator
{
public:
  cmInstallTargetGeneratorLocal(cmLocalGenerator* lg, std::string const& t,
                                std::string const& dest, bool implib)
    : cmInstallTargetGenerator(
        t, dest, implib, "", std::vector<std::string>(), "Unspecified",
        cmInstallGenerator::SelectMessageLevel(lg->GetMakefile()), false,
        false)
  {
    this->Compute(lg);
  }
};

void cmLocalGenerator::GenerateTargetInstallRules(
  std::ostream& os, std::string const& config,
  std::vector<std::string> const& configurationTypes)
{
  // Convert the old-style install specification from each target to
  // an install generator and run it.
  auto const& tgts = this->GetGeneratorTargets();
  for (auto const& l : tgts) {
    if (l->GetType() == cmStateEnums::INTERFACE_LIBRARY) {
      continue;
    }

    // Include the user-specified pre-install script for this target.
    if (cmValue preinstall = l->GetProperty("PRE_INSTALL_SCRIPT")) {
      cmInstallScriptGenerator g(*preinstall, false, "", false, false);
      g.Generate(os, config, configurationTypes);
    }

    // Install this target if a destination is given.
    if (!l->Target->GetInstallPath().empty()) {
      // Compute the full install destination.  Note that converting
      // to unix slashes also removes any trailing slash.
      // We also skip over the leading slash given by the user.
      std::string destination = l->Target->GetInstallPath().substr(1);
      cmSystemTools::ConvertToUnixSlashes(destination);
      if (destination.empty()) {
        destination = ".";
      }

      // Generate the proper install generator for this target type.
      switch (l->GetType()) {
        case cmStateEnums::EXECUTABLE:
        case cmStateEnums::STATIC_LIBRARY:
        case cmStateEnums::MODULE_LIBRARY: {
          // Use a target install generator.
          cmInstallTargetGeneratorLocal g(this, l->GetName(), destination,
                                          false);
          g.Generate(os, config, configurationTypes);
        } break;
        case cmStateEnums::SHARED_LIBRARY: {
#if defined(_WIN32) || defined(__CYGWIN__)
          // Special code to handle DLL.  Install the import library
          // to the normal destination and the DLL to the runtime
          // destination.
          cmInstallTargetGeneratorLocal g1(this, l->GetName(), destination,
                                           true);
          g1.Generate(os, config, configurationTypes);
          // We also skip over the leading slash given by the user.
          destination = l->Target->GetRuntimeInstallPath().substr(1);
          cmSystemTools::ConvertToUnixSlashes(destination);
          cmInstallTargetGeneratorLocal g2(this, l->GetName(), destination,
                                           false);
          g2.Generate(os, config, configurationTypes);
#else
          // Use a target install generator.
          cmInstallTargetGeneratorLocal g(this, l->GetName(), destination,
                                          false);
          g.Generate(os, config, configurationTypes);
#endif
        } break;
        default:
          break;
      }
    }

    // Include the user-specified post-install script for this target.
    if (cmValue postinstall = l->GetProperty("POST_INSTALL_SCRIPT")) {
      cmInstallScriptGenerator g(*postinstall, false, "", false, false);
      g.Generate(os, config, configurationTypes);
    }
  }
}

namespace {
bool cmLocalGeneratorShortenObjectName(std::string& objName,
                                       std::string::size_type max_len)
{
  // Check if the path can be shortened using an md5 sum replacement for
  // a portion of the path.
  std::string::size_type md5Len = 32;
  std::string::size_type numExtraChars = objName.size() - max_len + md5Len;
  std::string::size_type pos = objName.find('/', numExtraChars);
  if (pos == std::string::npos) {
    pos = objName.rfind('/', numExtraChars);
    if (pos == std::string::npos || pos <= md5Len) {
      return false;
    }
  }

  // Replace the beginning of the path portion of the object name with
  // its own md5 sum.
  cmCryptoHash md5(cmCryptoHash::AlgoMD5);
  std::string md5name = cmStrCat(md5.HashString(objName.substr(0, pos)),
                                 cm::string_view(objName).substr(pos));
  objName = md5name;

  // The object name is now shorter, check if it is short enough.
  return pos >= numExtraChars;
}

bool cmLocalGeneratorCheckObjectName(std::string& objName,
                                     std::string::size_type dir_len,
                                     std::string::size_type max_total_len)
{
  // Enforce the maximum file name length if possible.
  std::string::size_type max_obj_len = max_total_len;
  if (dir_len < max_total_len) {
    max_obj_len = max_total_len - dir_len;
    if (objName.size() > max_obj_len) {
      // The current object file name is too long.  Try to shorten it.
      return cmLocalGeneratorShortenObjectName(objName, max_obj_len);
    }
    // The object file name is short enough.
    return true;
  }
  // The build directory in which the object will be stored is
  // already too deep.
  return false;
}
}

std::string cmLocalGenerator::CreateSafeObjectFileName(
  std::string const& sin) const
{
  // Start with the original name.
  std::string ssin = sin;

  // Avoid full paths by removing leading slashes.
  ssin.erase(0, ssin.find_first_not_of('/'));

  // Avoid full paths by removing colons.
  std::replace(ssin.begin(), ssin.end(), ':', '_');

  // Avoid relative paths that go up the tree.
  cmSystemTools::ReplaceString(ssin, "../", "__/");

  // Avoid spaces.
  std::replace(ssin.begin(), ssin.end(), ' ', '_');

  return ssin;
}

std::string& cmLocalGenerator::CreateSafeUniqueObjectFileName(
  std::string const& sin, std::string const& dir_max)
{
  // Look for an existing mapped name for this object file.
  auto it = this->UniqueObjectNamesMap.find(sin);

  // If no entry exists create one.
  if (it == this->UniqueObjectNamesMap.end()) {
    auto ssin = this->CreateSafeObjectFileName(sin);

    // Mangle the name if necessary.
    if (this->Makefile->IsOn("CMAKE_MANGLE_OBJECT_FILE_NAMES")) {
      bool done;
      int cc = 0;
      char rpstr[100];
      snprintf(rpstr, sizeof(rpstr), "_p_");
      cmSystemTools::ReplaceString(ssin, "+", rpstr);
      std::string sssin = sin;
      do {
        done = true;
        for (it = this->UniqueObjectNamesMap.begin();
             it != this->UniqueObjectNamesMap.end(); ++it) {
          if (it->second == ssin) {
            done = false;
          }
        }
        if (done) {
          break;
        }
        sssin = ssin;
        cmSystemTools::ReplaceString(ssin, "_p_", rpstr);
        snprintf(rpstr, sizeof(rpstr), "_p%d_", cc++);
      } while (!done);
    }

    if (!cmLocalGeneratorCheckObjectName(ssin, dir_max.size(),
                                         this->ObjectPathMax)) {
      // Warn if this is the first time the path has been seen.
      if (this->ObjectMaxPathViolations.insert(dir_max).second) {
        std::ostringstream m;
        /* clang-format off */
        m << "The object file directory\n"
          << "  " << dir_max << "\n"
          << "has " << dir_max.size() << " characters.  "
          << "The maximum full path to an object file is "
          << this->ObjectPathMax << " characters "
          << "(see CMAKE_OBJECT_PATH_MAX).  "
          << "Object file\n"
          << "  " << ssin << "\n"
          << "cannot be safely placed under this directory.  "
          << "The build may not work correctly.";
        /* clang-format on */
        this->IssueMessage(MessageType::WARNING, m.str());
      }
    }

    // Insert the newly mapped object file name.
    std::map<std::string, std::string>::value_type e(sin, ssin);
    it = this->UniqueObjectNamesMap.insert(e).first;
  }

  // Return the map entry.
  return it->second;
}

void cmLocalGenerator::ComputeObjectFilenames(
  std::map<cmSourceFile const*, std::string>& /*unused*/,
  cmGeneratorTarget const* /*unused*/)
{
}

bool cmLocalGenerator::IsWindowsShell() const
{
  return this->GetState()->UseWindowsShell();
}

bool cmLocalGenerator::IsWatcomWMake() const
{
  return this->GetState()->UseWatcomWMake();
}

bool cmLocalGenerator::IsMinGWMake() const
{
  return this->GetState()->UseMinGWMake();
}

bool cmLocalGenerator::IsNMake() const
{
  return this->GetState()->UseNMake();
}

bool cmLocalGenerator::IsNinjaMulti() const
{
  return this->GetState()->UseNinjaMulti();
}

namespace {
std::string relativeIfUnder(std::string const& top, std::string const& cur,
                            std::string const& path)
{
  // Use a path relative to 'cur' if it can be expressed without
  // a `../` sequence that leaves 'top'.
  if (cmSystemTools::IsSubDirectory(path, cur) ||
      (cmSystemTools::IsSubDirectory(cur, top) &&
       cmSystemTools::IsSubDirectory(path, top))) {
    return cmSystemTools::ForceToRelativePath(cur, path);
  }
  return path;
}
}

std::string cmLocalGenerator::GetRelativeSourceFileName(
  cmSourceFile const& source) const
{
  // Construct the object file name using the full path to the source
  // file which is its only unique identification.
  std::string const& fullPath = source.GetFullPath();

  // Try referencing the source relative to the source tree.
  std::string relFromSource = relativeIfUnder(
    this->GetSourceDirectory(), this->GetCurrentSourceDirectory(), fullPath);
  assert(!relFromSource.empty());
  bool relSource = !cmSystemTools::FileIsFullPath(relFromSource);
  bool subSource = relSource && relFromSource[0] != '.';

  // Try referencing the source relative to the binary tree.
  std::string relFromBinary = relativeIfUnder(
    this->GetBinaryDirectory(), this->GetCurrentBinaryDirectory(), fullPath);
  assert(!relFromBinary.empty());
  bool relBinary = !cmSystemTools::FileIsFullPath(relFromBinary);
  bool subBinary = relBinary && relFromBinary[0] != '.';

  // Select a nice-looking reference to the source file to construct
  // the object file name.
  std::string objectName;
  // XXX(clang-tidy): https://bugs.llvm.org/show_bug.cgi?id=44165
  // NOLINTNEXTLINE(bugprone-branch-clone)
  if ((relSource && !relBinary) || (subSource && !subBinary)) {
    objectName = relFromSource;
  } else if ((relBinary && !relSource) || (subBinary && !subSource) ||
             relFromBinary.length() < relFromSource.length()) {
    objectName = relFromBinary;
  } else {
    objectName = relFromSource;
  }
  return objectName;
}

std::string cmLocalGenerator::GetObjectFileNameWithoutTarget(
  cmSourceFile const& source, std::string const& dir_max,
  bool* hasSourceExtension, char const* customOutputExtension)
{

  // This can return an absolute path in the case where source is
  // not relative to the current source or binary directoreis
  std::string objectName = this->GetRelativeSourceFileName(source);
  // if it is still a full path check for the try compile case
  // try compile never have in source sources, and should not
  // have conflicting source file names in the same target
  if (cmSystemTools::FileIsFullPath(objectName)) {
    if (this->GetGlobalGenerator()->GetCMakeInstance()->GetIsInTryCompile()) {
      objectName = cmSystemTools::GetFilenameName(source.GetFullPath());
    }
  }
  bool const isPchObject = objectName.find("cmake_pch") != std::string::npos;

  // Short object path policy selected, use as little info as necessary to
  // select an object name
  bool keptSourceExtension = true;
  if (this->UseShortObjectNames()) {
    objectName = this->GetShortObjectFileName(source);
    keptSourceExtension = false;
  }

  // Ensure that for the CMakeFiles/<target>.dir/generated_source_file
  // we don't end up having:
  // CMakeFiles/<target>.dir/CMakeFiles/<target>.dir/generated_source_file.obj
  cmValue unitySourceFile = source.GetProperty("UNITY_SOURCE_FILE");
  cmValue pchExtension = source.GetProperty("PCH_EXTENSION");
  if (unitySourceFile || pchExtension || isPchObject) {
    if (pchExtension) {
      customOutputExtension = pchExtension->c_str();
    }

    cmsys::RegularExpression var("(CMakeFiles/[^/]+.dir/)");
    if (var.find(objectName)) {
      objectName.erase(var.start(), var.end() - var.start());
    }
  }

  // Replace the original source file extension with the object file
  // extension.
  if (!source.GetPropertyAsBool("KEEP_EXTENSION")) {
    // Decide whether this language wants to replace the source
    // extension with the object extension.
    bool replaceExt = false;
    std::string lang = source.GetLanguage();
    if (!lang.empty()) {
      replaceExt = this->Makefile->IsOn(
        cmStrCat("CMAKE_", lang, "_OUTPUT_EXTENSION_REPLACE"));
    }

    // Remove the source extension if it is to be replaced.
    if (replaceExt || customOutputExtension) {
      keptSourceExtension = false;
      std::string::size_type dot_pos = objectName.rfind('.');
      if (dot_pos != std::string::npos) {
        objectName = objectName.substr(0, dot_pos);
      }
    }

    // Strip source file extension when shortening object file paths
    if (this->UseShortObjectNames()) {
      objectName = cmSystemTools::GetFilenameWithoutExtension(objectName);
    }
    // Store the new extension.
    if (customOutputExtension) {
      objectName += customOutputExtension;
    } else {
      objectName += this->GlobalGenerator->GetLanguageOutputExtension(source);
    }
  }
  if (hasSourceExtension) {
    *hasSourceExtension = keptSourceExtension;
  }

  // Convert to a safe name.
  return this->CreateSafeUniqueObjectFileName(objectName, dir_max);
}

bool cmLocalGenerator::UseShortObjectNames(
  cmStateEnums::IntermediateDirKind kind) const
{
  return this->GlobalGenerator->UseShortObjectNames(kind);
}

std::string cmLocalGenerator::GetObjectOutputRoot(
  cmStateEnums::IntermediateDirKind kind) const
{
  if (this->UseShortObjectNames(kind)) {
    return cmStrCat(this->GetCurrentBinaryDirectory(), '/',
                    this->GlobalGenerator->GetShortBinaryOutputDir());
  }
  return cmStrCat(this->GetCurrentBinaryDirectory(), "/CMakeFiles");
}

bool cmLocalGenerator::AlwaysUsesCMFPaths() const
{
  return true;
}

std::string cmLocalGenerator::GetShortObjectFileName(
  cmSourceFile const& source) const
{
  std::string objectName = this->GetRelativeSourceFileName(source);
  std::string objectFileName =
    cmSystemTools::GetFilenameName(source.GetFullPath());
  cmCryptoHash objNameHasher(cmCryptoHash::AlgoSHA3_512);
  std::string terseObjectName =
    objNameHasher.HashString(objectName).substr(0, 8);
  return terseObjectName;
}

std::string cmLocalGenerator::ComputeShortTargetDirectory(
  cmGeneratorTarget const* target) const
{
  auto const& tgtName = target->GetName();
  return this->GetGlobalGenerator()->ComputeTargetShortName(
    this->GetCurrentBinaryDirectory(), tgtName);
}

std::string cmLocalGenerator::GetSourceFileLanguage(cmSourceFile const& source)
{
  return source.GetLanguage();
}

cmake* cmLocalGenerator::GetCMakeInstance() const
{
  return this->GlobalGenerator->GetCMakeInstance();
}

std::string const& cmLocalGenerator::GetSourceDirectory() const
{
  return this->GetCMakeInstance()->GetHomeDirectory();
}

std::string const& cmLocalGenerator::GetBinaryDirectory() const
{
  return this->GetCMakeInstance()->GetHomeOutputDirectory();
}

std::string const& cmLocalGenerator::GetCurrentBinaryDirectory() const
{
  return this->StateSnapshot.GetDirectory().GetCurrentBinary();
}

std::string const& cmLocalGenerator::GetCurrentSourceDirectory() const
{
  return this->StateSnapshot.GetDirectory().GetCurrentSource();
}

std::string cmLocalGenerator::GetTargetDirectory(
  cmGeneratorTarget const* /*unused*/) const
{
  cmSystemTools::Error("GetTargetDirectory"
                       " called on cmLocalGenerator");
  return "";
}

cmPolicies::PolicyStatus cmLocalGenerator::GetPolicyStatus(
  cmPolicies::PolicyID id) const
{
  return this->Makefile->GetPolicyStatus(id);
}

bool cmLocalGenerator::CheckDefinition(std::string const& define) const
{
  // Many compilers do not support -DNAME(arg)=sdf so we disable it.
  std::string::size_type pos = define.find_first_of("(=");
  if (pos != std::string::npos) {
    if (define[pos] == '(') {
      std::ostringstream e;
      /* clang-format off */
      e << "WARNING: Function-style preprocessor definitions may not be "
           "passed on the compiler command line because many compilers "
           "do not support it.\n"
           "CMake is dropping a preprocessor definition: " << define << "\n"
           "Consider defining the macro in a (configured) header file.\n";
      /* clang-format on */
      cmSystemTools::Message(e.str());
      return false;
    }
  }

  // Many compilers do not support # in the value so we disable it.
  if (define.find_first_of('#') != std::string::npos) {
    std::ostringstream e;
    /* clang-format off */
    e << "WARNING: Preprocessor definitions containing '#' may not be "
         "passed on the compiler command line because many compilers "
         "do not support it.\n"
         "CMake is dropping a preprocessor definition: " << define << "\n"
         "Consider defining the macro in a (configured) header file.\n";
    /* clang-format on */
    cmSystemTools::Message(e.str());
    return false;
  }

  // Assume it is supported.
  return true;
}

static void cmLGInfoProp(cmMakefile* mf, cmGeneratorTarget* target,
                         std::string const& prop)
{
  if (cmValue val = target->GetProperty(prop)) {
    mf->AddDefinition(prop, *val);
  }
}

void cmLocalGenerator::GenerateAppleInfoPList(cmGeneratorTarget* target,
                                              std::string const& targetName,
                                              std::string const& fname)
{
  // Find the Info.plist template.
  cmValue in = target->GetProperty("MACOSX_BUNDLE_INFO_PLIST");
  std::string inFile = cmNonempty(in) ? *in : "MacOSXBundleInfo.plist.in";
  if (!cmSystemTools::FileIsFullPath(inFile)) {
    std::string inMod = this->Makefile->GetModulesFile(inFile);
    if (!inMod.empty()) {
      inFile = inMod;
    }
  }
  if (!cmSystemTools::FileExists(inFile, true)) {
    std::ostringstream e;
    e << "Target " << target->GetName() << " Info.plist template \"" << inFile
      << "\" could not be found.";
    cmSystemTools::Error(e.str());
    return;
  }

  // Convert target properties to variables in an isolated makefile
  // scope to configure the file.  If properties are set they will
  // override user make variables.  If not the configuration will fall
  // back to the directory-level values set by the user.
  cmMakefile* mf = this->Makefile;
  cmMakefile::ScopePushPop varScope(mf);
  mf->AddDefinition("MACOSX_BUNDLE_EXECUTABLE_NAME", targetName);
  cmLGInfoProp(mf, target, "MACOSX_BUNDLE_INFO_STRING");
  cmLGInfoProp(mf, target, "MACOSX_BUNDLE_ICON_FILE");
  cmLGInfoProp(mf, target, "MACOSX_BUNDLE_GUI_IDENTIFIER");
  cmLGInfoProp(mf, target, "MACOSX_BUNDLE_LONG_VERSION_STRING");
  cmLGInfoProp(mf, target, "MACOSX_BUNDLE_BUNDLE_NAME");
  cmLGInfoProp(mf, target, "MACOSX_BUNDLE_SHORT_VERSION_STRING");
  cmLGInfoProp(mf, target, "MACOSX_BUNDLE_BUNDLE_VERSION");
  cmLGInfoProp(mf, target, "MACOSX_BUNDLE_COPYRIGHT");
  mf->ConfigureFile(inFile, fname, false, false, false);
}

void cmLocalGenerator::GenerateFrameworkInfoPList(
  cmGeneratorTarget* target, std::string const& targetName,
  std::string const& fname)
{
  // Find the Info.plist template.
  cmValue in = target->GetProperty("MACOSX_FRAMEWORK_INFO_PLIST");
  std::string inFile = cmNonempty(in) ? *in : "MacOSXFrameworkInfo.plist.in";
  if (!cmSystemTools::FileIsFullPath(inFile)) {
    std::string inMod = this->Makefile->GetModulesFile(inFile);
    if (!inMod.empty()) {
      inFile = inMod;
    }
  }
  if (!cmSystemTools::FileExists(inFile, true)) {
    std::ostringstream e;
    e << "Target " << target->GetName() << " Info.plist template \"" << inFile
      << "\" could not be found.";
    cmSystemTools::Error(e.str());
    return;
  }

  // Convert target properties to variables in an isolated makefile
  // scope to configure the file.  If properties are set they will
  // override user make variables.  If not the configuration will fall
  // back to the directory-level values set by the user.
  cmMakefile* mf = this->Makefile;
  cmMakefile::ScopePushPop varScope(mf);
  mf->AddDefinition("MACOSX_FRAMEWORK_NAME", targetName);
  cmLGInfoProp(mf, target, "MACOSX_FRAMEWORK_ICON_FILE");
  cmLGInfoProp(mf, target, "MACOSX_FRAMEWORK_IDENTIFIER");
  cmLGInfoProp(mf, target, "MACOSX_FRAMEWORK_SHORT_VERSION_STRING");
  cmLGInfoProp(mf, target, "MACOSX_FRAMEWORK_BUNDLE_NAME");
  cmLGInfoProp(mf, target, "MACOSX_FRAMEWORK_BUNDLE_VERSION");
  mf->ConfigureFile(inFile, fname, false, false, false);
}

namespace {
cm::string_view CustomOutputRoleKeyword(cmLocalGenerator::OutputRole role)
{
  return (role == cmLocalGenerator::OutputRole::Primary ? "OUTPUT"_s
                                                        : "BYPRODUCTS"_s);
}

void CreateGeneratedSource(cmLocalGenerator& lg, std::string const& output,
                           cmLocalGenerator::OutputRole role,
                           cmCommandOrigin origin,
                           cmListFileBacktrace const& lfbt)
{
  if (cmGeneratorExpression::Find(output) != std::string::npos) {
    lg.GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR,
      "Generator expressions in custom command outputs are not implemented!",
      lfbt);
    return;
  }

  // Make sure the file will not be generated into the source
  // directory during an out of source build.
  if (!lg.GetMakefile()->CanIWriteThisFile(output)) {
    lg.GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat(CustomOutputRoleKeyword(role), " path\n  ", output,
               "\nin a source directory as an output of custom command."),
      lfbt);
    return;
  }

  // Make sure the output file name has no invalid characters.
  bool const hashNotAllowed = lg.GetState()->UseBorlandMake();
  std::string::size_type pos = output.find_first_of("<>");
  if (pos == std::string::npos && hashNotAllowed) {
    pos = output.find_first_of('#');
  }

  if (pos != std::string::npos) {
    lg.GetCMakeInstance()->IssueMessage(
      MessageType::FATAL_ERROR,
      cmStrCat(CustomOutputRoleKeyword(role), " containing a \"", output[pos],
               "\" is not allowed."),
      lfbt);
    return;
  }

  // Outputs without generator expressions from the project are already
  // created and marked as generated.  Do not mark them again, because
  // other commands might have overwritten the property.
  if (origin == cmCommandOrigin::Generator) {
    lg.GetMakefile()->GetOrCreateGeneratedSource(output);
  }
}

std::string ComputeCustomCommandRuleFileName(cmLocalGenerator& lg,
                                             cmListFileBacktrace const& bt,
                                             std::string const& output)
{
  // If the output path has no generator expressions, use it directly.
  if (cmGeneratorExpression::Find(output) == std::string::npos) {
    return output;
  }

  // The output path contains a generator expression, but we must choose
  // a single source file path to which to attach the custom command.
  // Use some heuristics to provide a nice-looking name when possible.

  // If the only genex is $<CONFIG>, replace that gracefully.
  {
    std::string simple = output;
    cmSystemTools::ReplaceString(simple, "$<CONFIG>", "(CONFIG)");
    if (cmGeneratorExpression::Find(simple) == std::string::npos) {
      return simple;
    }
  }

  // If the genex evaluates to the same value in all configurations, use that.
  {
    std::vector<std::string> allConfigOutputs =
      lg.ExpandCustomCommandOutputGenex(output, bt);
    if (allConfigOutputs.size() == 1) {
      return allConfigOutputs.front();
    }
  }

  // Fall back to a deterministic unique name.
  cmCryptoHash h(cmCryptoHash::AlgoSHA256);
  return cmStrCat(lg.GetCurrentBinaryDirectory(), "/CMakeFiles/",
                  h.HashString(output).substr(0, 16));
}

cmSourceFile* AddCustomCommand(cmLocalGenerator& lg, cmCommandOrigin origin,
                               std::unique_ptr<cmCustomCommand> cc,
                               bool replace)
{
  cmMakefile* mf = lg.GetMakefile();
  auto const& lfbt = cc->GetBacktrace();
  auto const& outputs = cc->GetOutputs();
  auto const& byproducts = cc->GetByproducts();
  auto const& commandLines = cc->GetCommandLines();

  // Choose a source file on which to store the custom command.
  cmSourceFile* file = nullptr;
  if (!commandLines.empty() && cc->HasMainDependency()) {
    auto const& main_dependency = cc->GetMainDependency();
    // The main dependency was specified.  Use it unless a different
    // custom command already used it.
    file = mf->GetSource(main_dependency);
    if (file && file->GetCustomCommand() && !replace) {
      // The main dependency already has a custom command.
      if (commandLines == file->GetCustomCommand()->GetCommandLines()) {
        // The existing custom command is identical.  Silently ignore
        // the duplicate.
        return file;
      }
      // The existing custom command is different.  We need to
      // generate a rule file for this new command.
      file = nullptr;
    } else if (!file) {
      file = mf->CreateSource(main_dependency);
    }
  }

  // Generate a rule file if the main dependency is not available.
  if (!file) {
    cmGlobalGenerator* gg = lg.GetGlobalGenerator();

    // Construct a rule file associated with the first output produced.
    std::string outName = gg->GenerateRuleFile(
      ComputeCustomCommandRuleFileName(lg, lfbt, outputs[0]));

    // Check if the rule file already exists.
    file = mf->GetSource(outName, cmSourceFileLocationKind::Known);
    if (file && file->GetCustomCommand() && !replace) {
      // The rule file already exists.
      if (commandLines != file->GetCustomCommand()->GetCommandLines()) {
        lg.GetCMakeInstance()->IssueMessage(
          MessageType::FATAL_ERROR,
          cmStrCat("Attempt to add a custom rule to output\n  ", outName,
                   "\nwhich already has a custom rule."),
          lfbt);
      }
      return file;
    }

    // Create a cmSourceFile for the rule file.
    if (!file) {
      file = mf->CreateSource(outName, true, cmSourceFileLocationKind::Known);
    }
    file->SetProperty("__CMAKE_RULE", "1");
  }

  // Attach the custom command to the file.
  if (file) {
    cc->SetEscapeAllowMakeVars(true);

    lg.AddSourceOutputs(file, outputs, cmLocalGenerator::OutputRole::Primary,
                        lfbt, origin);
    lg.AddSourceOutputs(file, byproducts,
                        cmLocalGenerator::OutputRole::Byproduct, lfbt, origin);

    file->SetCustomCommand(std::move(cc));
  }
  return file;
}

bool AnyOutputMatches(std::string const& name,
                      std::vector<std::string> const& outputs)
{
  return std::any_of(outputs.begin(), outputs.end(),
                     [&name](std::string const& output) -> bool {
                       std::string::size_type pos = output.rfind(name);
                       // If the output matches exactly
                       return (pos != std::string::npos &&
                               pos == output.size() - name.size() &&
                               (pos == 0 || output[pos - 1] == '/'));
                     });
}

bool AnyTargetCommandOutputMatches(
  std::string const& name, std::vector<cmCustomCommand> const& commands)
{
  return std::any_of(commands.begin(), commands.end(),
                     [&name](cmCustomCommand const& command) -> bool {
                       return AnyOutputMatches(name, command.GetByproducts());
                     });
}
}

namespace detail {
void AddCustomCommandToTarget(cmLocalGenerator& lg, cmCommandOrigin origin,
                              cmTarget* target, cmCustomCommandType type,
                              std::unique_ptr<cmCustomCommand> cc)
{
  // Add the command to the appropriate build step for the target.
  cc->SetEscapeAllowMakeVars(true);
  cc->SetTarget(target->GetName());

  lg.AddTargetByproducts(target, cc->GetByproducts(), cc->GetBacktrace(),
                         origin);

  switch (type) {
    case cmCustomCommandType::PRE_BUILD:
      target->AddPreBuildCommand(std::move(*cc));
      break;
    case cmCustomCommandType::PRE_LINK:
      target->AddPreLinkCommand(std::move(*cc));
      break;
    case cmCustomCommandType::POST_BUILD:
      target->AddPostBuildCommand(std::move(*cc));
      break;
  }

  cc.reset();
}

cmSourceFile* AddCustomCommandToOutput(cmLocalGenerator& lg,
                                       cmCommandOrigin origin,
                                       std::unique_ptr<cmCustomCommand> cc,
                                       bool replace)
{
  return AddCustomCommand(lg, origin, std::move(cc), replace);
}

void AppendCustomCommandToOutput(cmLocalGenerator& lg,
                                 cmListFileBacktrace const& lfbt,
                                 std::string const& output,
                                 std::vector<std::string> const& depends,
                                 cmImplicitDependsList const& implicit_depends,
                                 cmCustomCommandLines const& commandLines)
{
  // Lookup an existing command.
  cmSourceFile* sf = nullptr;
  if (cmGeneratorExpression::Find(output) == std::string::npos) {
    sf = lg.GetSourceFileWithOutput(output);
  } else {
    // This output path has a generator expression.  Evaluate it to
    // find the output for any configurations.
    for (std::string const& out :
         lg.ExpandCustomCommandOutputGenex(output, lfbt)) {
      sf = lg.GetSourceFileWithOutput(out);
      if (sf) {
        break;
      }
    }
  }

  if (sf) {
    if (cmCustomCommand* cc = sf->GetCustomCommand()) {
      cc->AppendCommands(commandLines);
      cc->AppendDepends(depends);
      if (cc->GetCodegen() && !implicit_depends.empty()) {
        lg.GetCMakeInstance()->IssueMessage(
          MessageType::FATAL_ERROR,
          "Cannot append IMPLICIT_DEPENDS to existing CODEGEN custom "
          "command.");
      }
      cc->AppendImplicitDepends(implicit_depends);
      return;
    }
  }

  // No existing command found.
  lg.GetCMakeInstance()->IssueMessage(
    MessageType::FATAL_ERROR,
    cmStrCat("Attempt to APPEND to custom command with output\n  ", output,
             "\nwhich is not already a custom command output."),
    lfbt);
}

void AddUtilityCommand(cmLocalGenerator& lg, cmCommandOrigin origin,
                       cmTarget* target, std::unique_ptr<cmCustomCommand> cc)
{
  // They might be moved away
  auto byproducts = cc->GetByproducts();
  auto lfbt = cc->GetBacktrace();

  // Use an empty comment to avoid generation of default comment.
  if (!cc->GetComment()) {
    cc->SetComment("");
  }

  // Create the generated symbolic output name of the utility target.
  std::string output =
    lg.CreateUtilityOutput(target->GetName(), byproducts, lfbt);
  cc->SetOutputs(output);

  cmSourceFile* rule = AddCustomCommand(lg, origin, std::move(cc),
                                        /*replace=*/false);
  if (rule) {
    lg.AddTargetByproducts(target, byproducts, lfbt, origin);
  }

  target->AddSource(output);
}

std::vector<std::string> ComputeISPCObjectSuffixes(cmGeneratorTarget* target)
{
  cmValue const targetProperty = target->GetProperty("ISPC_INSTRUCTION_SETS");
  cmList ispcTargets;

  if (!targetProperty.IsOff()) {
    ispcTargets.assign(targetProperty);
    for (auto& ispcTarget : ispcTargets) {
      // transform targets into the suffixes
      auto pos = ispcTarget.find('-');
      auto target_suffix = ispcTarget.substr(0, pos);
      if (target_suffix ==
          "avx1") { // when targeting avx1 ISPC uses the 'avx' output string
        target_suffix = "avx";
      }
      ispcTarget = target_suffix;
    }
  }
  return std::move(ispcTargets.data());
}

std::vector<std::string> ComputeISPCExtraObjects(
  std::string const& objectName, std::string const& buildDirectory,
  std::vector<std::string> const& ispcSuffixes)
{
  auto normalizedDir = cmSystemTools::CollapseFullPath(buildDirectory);
  std::vector<std::string> computedObjects;
  computedObjects.reserve(ispcSuffixes.size());

  auto extension = cmSystemTools::GetFilenameLastExtension(objectName);

  // We can't use cmSystemTools::GetFilenameWithoutLastExtension as it
  // drops any directories in objectName
  auto objNameNoExt = objectName;
  std::string::size_type dot_pos = objectName.rfind('.');
  if (dot_pos != std::string::npos) {
    objNameNoExt.resize(dot_pos);
  }

  for (auto const& ispcTarget : ispcSuffixes) {
    computedObjects.emplace_back(
      cmStrCat(normalizedDir, '/', objNameNoExt, '_', ispcTarget, extension));
  }

  return computedObjects;
}
}

cmSourcesWithOutput cmLocalGenerator::GetSourcesWithOutput(
  std::string const& name) const
{
  // Linear search?  Also see GetSourceFileWithOutput for detail.
  if (!cmSystemTools::FileIsFullPath(name)) {
    cmSourcesWithOutput sources;
    sources.Target = this->LinearGetTargetWithOutput(name);
    sources.Source = this->LinearGetSourceFileWithOutput(
      name, cmSourceOutputKind::OutputOrByproduct, sources.SourceIsByproduct);
    return sources;
  }
  // Otherwise we use an efficient lookup map.
  auto o = this->OutputToSource.find(name);
  if (o != this->OutputToSource.end()) {
    return o->second.Sources;
  }
  return {};
}

cmSourceFile* cmLocalGenerator::GetSourceFileWithOutput(
  std::string const& name, cmSourceOutputKind kind) const
{
  // If the queried path is not absolute we use the backward compatible
  // linear-time search for an output with a matching suffix.
  if (!cmSystemTools::FileIsFullPath(name)) {
    bool byproduct = false;
    return this->LinearGetSourceFileWithOutput(name, kind, byproduct);
  }
  // Otherwise we use an efficient lookup map.
  auto o = this->OutputToSource.find(name);
  if (o != this->OutputToSource.end() &&
      (!o->second.Sources.SourceIsByproduct ||
       kind == cmSourceOutputKind::OutputOrByproduct)) {
    // Source file could also be null pointer for example if we found the
    // byproduct of a utility target, a PRE_BUILD, PRE_LINK, or POST_BUILD
    // command of a target, or a not yet created custom command.
    return o->second.Sources.Source;
  }
  return nullptr;
}

std::string cmLocalGenerator::CreateUtilityOutput(
  std::string const& targetName, std::vector<std::string> const&,
  cmListFileBacktrace const&)
{
  std::string force =
    cmStrCat(this->GetCurrentBinaryDirectory(), "/CMakeFiles/", targetName);
  // The output is not actually created so mark it symbolic.
  if (cmSourceFile* sf = this->Makefile->GetOrCreateGeneratedSource(force)) {
    sf->SetProperty("SYMBOLIC", "1");
  } else {
    cmSystemTools::Error("Could not get source file entry for " + force);
  }
  return force;
}

std::vector<cmCustomCommandGenerator>
cmLocalGenerator::MakeCustomCommandGenerators(cmCustomCommand const& cc,
                                              std::string const& config)
{
  std::vector<cmCustomCommandGenerator> ccgs;
  ccgs.emplace_back(cc, config, this);
  return ccgs;
}

std::vector<std::string> cmLocalGenerator::ExpandCustomCommandOutputPaths(
  cmCompiledGeneratorExpression const& cge, std::string const& config)
{
  cmList paths{ cge.Evaluate(this, config) };
  for (std::string& p : paths) {
    p = cmSystemTools::CollapseFullPath(p, this->GetCurrentBinaryDirectory());
  }
  return std::move(paths.data());
}

std::vector<std::string> cmLocalGenerator::ExpandCustomCommandOutputGenex(
  std::string const& o, cmListFileBacktrace const& bt)
{
  std::vector<std::string> allConfigOutputs;
  cmGeneratorExpression ge(*this->GetCMakeInstance(), bt);
  std::unique_ptr<cmCompiledGeneratorExpression> cge = ge.Parse(o);
  std::vector<std::string> configs =
    this->Makefile->GetGeneratorConfigs(cmMakefile::IncludeEmptyConfig);
  for (std::string const& config : configs) {
    std::vector<std::string> configOutputs =
      this->ExpandCustomCommandOutputPaths(*cge, config);
    allConfigOutputs.reserve(allConfigOutputs.size() + configOutputs.size());
    std::move(configOutputs.begin(), configOutputs.end(),
              std::back_inserter(allConfigOutputs));
  }
  auto endUnique =
    cmRemoveDuplicates(allConfigOutputs.begin(), allConfigOutputs.end());
  allConfigOutputs.erase(endUnique, allConfigOutputs.end());
  return allConfigOutputs;
}

void cmLocalGenerator::AddTargetByproducts(
  cmTarget* target, std::vector<std::string> const& byproducts,
  cmListFileBacktrace const& bt, cmCommandOrigin origin)
{
  for (std::string const& o : byproducts) {
    if (cmGeneratorExpression::Find(o) == std::string::npos) {
      this->UpdateOutputToSourceMap(o, target, bt, origin);
      continue;
    }

    // This byproduct path has a generator expression.  Evaluate it to
    // register the byproducts for all configurations.
    for (std::string const& b : this->ExpandCustomCommandOutputGenex(o, bt)) {
      this->UpdateOutputToSourceMap(b, target, bt, cmCommandOrigin::Generator);
    }
  }
}

void cmLocalGenerator::AddSourceOutputs(
  cmSourceFile* source, std::vector<std::string> const& outputs,
  OutputRole role, cmListFileBacktrace const& bt, cmCommandOrigin origin)
{
  for (std::string const& o : outputs) {
    if (cmGeneratorExpression::Find(o) == std::string::npos) {
      this->UpdateOutputToSourceMap(o, source, role, bt, origin);
      continue;
    }

    // This output path has a generator expression.  Evaluate it to
    // register the outputs for all configurations.
    for (std::string const& out :
         this->ExpandCustomCommandOutputGenex(o, bt)) {
      this->UpdateOutputToSourceMap(out, source, role, bt,
                                    cmCommandOrigin::Generator);
    }
  }
}

void cmLocalGenerator::UpdateOutputToSourceMap(std::string const& byproduct,
                                               cmTarget* target,
                                               cmListFileBacktrace const& bt,
                                               cmCommandOrigin origin)
{
  SourceEntry entry;
  entry.Sources.Target = target;

  auto pr = this->OutputToSource.emplace(byproduct, entry);
  if (pr.second) {
    CreateGeneratedSource(*this, byproduct, OutputRole::Byproduct, origin, bt);
  } else {
    SourceEntry& current = pr.first->second;
    // Has the target already been set?
    if (!current.Sources.Target) {
      current.Sources.Target = target;
    } else {
      // Multiple custom commands/targets produce the same output (source file
      // or target).  See also comment in other UpdateOutputToSourceMap
      // overload.
      //
      // TODO: Warn the user about this case.
    }
  }
}

void cmLocalGenerator::UpdateOutputToSourceMap(std::string const& output,
                                               cmSourceFile* source,
                                               OutputRole role,
                                               cmListFileBacktrace const& bt,
                                               cmCommandOrigin origin)
{
  SourceEntry entry;
  entry.Sources.Source = source;
  entry.Sources.SourceIsByproduct = role == OutputRole::Byproduct;

  auto pr = this->OutputToSource.emplace(output, entry);
  if (pr.second) {
    CreateGeneratedSource(*this, output, role, origin, bt);
  } else {
    SourceEntry& current = pr.first->second;
    // Outputs take precedence over byproducts
    if (!current.Sources.Source ||
        (current.Sources.SourceIsByproduct && role == OutputRole::Primary)) {
      current.Sources.Source = source;
      current.Sources.SourceIsByproduct = false;
    } else {
      // Multiple custom commands produce the same output but may
      // be attached to a different source file (MAIN_DEPENDENCY).
      // LinearGetSourceFileWithOutput would return the first one,
      // so keep the mapping for the first one.
      //
      // TODO: Warn the user about this case.  However, the VS 8 generator
      // triggers it for separate generate.stamp rules in ZERO_CHECK and
      // individual targets.
    }
  }
}

cmTarget* cmLocalGenerator::LinearGetTargetWithOutput(
  std::string const& name) const
{
  // We go through the ordered vector of targets to get reproducible results
  // should multiple names match.
  for (cmTarget* t : this->Makefile->GetOrderedTargets()) {
    // Does the output of any command match the source file name?
    if (AnyTargetCommandOutputMatches(name, t->GetPreBuildCommands())) {
      return t;
    }
    if (AnyTargetCommandOutputMatches(name, t->GetPreLinkCommands())) {
      return t;
    }
    if (AnyTargetCommandOutputMatches(name, t->GetPostBuildCommands())) {
      return t;
    }
  }
  return nullptr;
}

cmSourceFile* cmLocalGenerator::LinearGetSourceFileWithOutput(
  std::string const& name, cmSourceOutputKind kind, bool& byproduct) const
{
  // Outputs take precedence over byproducts.
  byproduct = false;
  cmSourceFile* fallback = nullptr;

  // Look through all the source files that have custom commands and see if the
  // custom command has the passed source file as an output.
  for (auto const& src : this->Makefile->GetSourceFiles()) {
    // Does this source file have a custom command?
    if (src->GetCustomCommand()) {
      // Does the output of the custom command match the source file name?
      if (AnyOutputMatches(name, src->GetCustomCommand()->GetOutputs())) {
        // Return the first matching output.
        return src.get();
      }
      if (kind == cmSourceOutputKind::OutputOrByproduct) {
        if (AnyOutputMatches(name, src->GetCustomCommand()->GetByproducts())) {
          // Do not return the source yet as there might be a matching output.
          fallback = src.get();
        }
      }
    }
  }

  // Did we find a byproduct?
  byproduct = fallback != nullptr;
  return fallback;
}
