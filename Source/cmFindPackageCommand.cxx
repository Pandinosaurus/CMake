/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#include "cmFindPackageCommand.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <deque>
#include <functional>
#include <iterator>
#include <sstream>
#include <unordered_set>
#include <utility>

#include <cm/memory>
#include <cm/optional>
#include <cmext/algorithm>
#include <cmext/string_view>

#include "cmsys/Directory.hxx"
#include "cmsys/FStream.hxx"
#include "cmsys/Glob.hxx"
#include "cmsys/RegularExpression.hxx"
#include "cmsys/String.h"

#include "cmAlgorithms.h"
#include "cmConfigureLog.h"
#include "cmDependencyProvider.h"
#include "cmExecutionStatus.h"
#include "cmExperimental.h"
#include "cmFindPackageStack.h"
#include "cmList.h"
#include "cmListFileCache.h"
#include "cmMakefile.h"
#include "cmMessageType.h"
#include "cmPackageState.h"
#include "cmPolicies.h"
#include "cmRange.h"
#include "cmSearchPath.h"
#include "cmState.h"
#include "cmStateSnapshot.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"
#include "cmValue.h"
#include "cmVersion.h"
#include "cmWindowsRegistry.h"

#if defined(__HAIKU__)
#  include <FindDirectory.h>
#  include <StorageDefs.h>
#endif

#if defined(_WIN32) && !defined(__CYGWIN__)
#  include <windows.h>
// http://msdn.microsoft.com/en-us/library/aa384253%28v=vs.85%29.aspx
#  if !defined(KEY_WOW64_32KEY)
#    define KEY_WOW64_32KEY 0x0200
#  endif
#  if !defined(KEY_WOW64_64KEY)
#    define KEY_WOW64_64KEY 0x0100
#  endif
#endif

namespace {

using pdt = cmFindPackageCommand::PackageDescriptionType;
using ParsedVersion = cmPackageInfoReader::Pep440Version;

template <template <typename> class Op>
struct StrverscmpOp
{
  bool operator()(std::string const& lhs, std::string const& rhs) const
  {
    return Op<int>()(cmSystemTools::strverscmp(lhs, rhs), 0);
  }
};

std::size_t collectPathsForDebug(std::string& buffer,
                                 cmSearchPath const& searchPath,
                                 std::size_t const startIndex = 0)
{
  auto const& paths = searchPath.GetPaths();
  if (paths.empty()) {
    buffer += "  none\n";
    return 0;
  }
  for (auto i = startIndex; i < paths.size(); i++) {
    buffer += "  " + paths[i].Path + "\n";
  }
  return paths.size();
}

#if !(defined(_WIN32) && !defined(__CYGWIN__))
class cmFindPackageCommandHoldFile
{
  char const* File;

public:
  cmFindPackageCommandHoldFile(char const* const f)
    : File(f)
  {
  }
  ~cmFindPackageCommandHoldFile()
  {
    if (this->File) {
      cmSystemTools::RemoveFile(this->File);
    }
  }
  cmFindPackageCommandHoldFile(cmFindPackageCommandHoldFile const&) = delete;
  cmFindPackageCommandHoldFile& operator=(
    cmFindPackageCommandHoldFile const&) = delete;
  void Release() { this->File = nullptr; }
};
#endif

bool isDirentryToIgnore(char const* const fname)
{
  assert(fname);
  assert(fname[0] != 0);
  return fname[0] == '.' &&
    (fname[1] == 0 || (fname[1] == '.' && fname[2] == 0));
}

class cmAppendPathSegmentGenerator
{
public:
  cmAppendPathSegmentGenerator(cm::string_view dirName)
    : DirName{ dirName }
  {
  }

  std::string GetNextCandidate(std::string const& parent)
  {
    if (this->NeedReset) {
      return {};
    }
    this->NeedReset = true;
    return cmStrCat(parent, this->DirName, '/');
  }

  void Reset() { this->NeedReset = false; }

private:
  cm::string_view const DirName;
  bool NeedReset = false;
};

class cmEnumPathSegmentsGenerator
{
public:
  cmEnumPathSegmentsGenerator(std::vector<cm::string_view> const& init)
    : Names{ init }
    , Current{ this->Names.get().cbegin() }
  {
  }

  std::string GetNextCandidate(std::string const& parent)
  {
    if (this->Current != this->Names.get().cend()) {
      return cmStrCat(parent, *this->Current++, '/');
    }
    return {};
  }

  void Reset() { this->Current = this->Names.get().cbegin(); }

private:
  std::reference_wrapper<std::vector<cm::string_view> const> Names;
  std::vector<cm::string_view>::const_iterator Current;
};

class cmCaseInsensitiveDirectoryListGenerator
{
public:
  cmCaseInsensitiveDirectoryListGenerator(cm::string_view name)
    : DirName{ name }
  {
  }

  std::string GetNextCandidate(std::string const& parent)
  {
    if (!this->Loaded) {
      this->CurrentIdx = 0ul;
      this->Loaded = true;
      if (!this->DirectoryLister.Load(parent)) {
        return {};
      }
    }

    while (this->CurrentIdx < this->DirectoryLister.GetNumberOfFiles()) {
      char const* const fname =
        this->DirectoryLister.GetFile(this->CurrentIdx++);
      if (isDirentryToIgnore(fname)) {
        continue;
      }
      if (cmsysString_strcasecmp(fname, this->DirName.data()) == 0) {
        auto candidate = cmStrCat(parent, fname, '/');
        if (cmSystemTools::FileIsDirectory(candidate)) {
          return candidate;
        }
      }
    }
    return {};
  }

  void Reset() { this->Loaded = false; }

private:
  cmsys::Directory DirectoryLister;
  cm::string_view const DirName;
  unsigned long CurrentIdx = 0ul;
  bool Loaded = false;
};

class cmDirectoryListGenerator
{
public:
  cmDirectoryListGenerator(std::vector<std::string> const* names,
                           bool exactMatch)
    : Names{ names }
    , ExactMatch{ exactMatch }
    , Current{ this->Matches.cbegin() }
  {
    assert(names || !exactMatch);
    assert(!names || !names->empty());
  }
  virtual ~cmDirectoryListGenerator() = default;

  std::string GetNextCandidate(std::string const& parent)
  {
    // Construct a list of matches if not yet
    if (this->Matches.empty()) {
      cmsys::Directory directoryLister;
      // ALERT `Directory::Load()` keeps only names
      // internally and LOST entry type from `dirent`.
      // So, `Directory::FileIsDirectory` gonna use
      // `SystemTools::FileIsDirectory()` and waste a syscall.
      // TODO Need to enhance the `Directory` class.
      directoryLister.Load(parent);

      // ATTENTION Is it guaranteed that first two entries are
      // `.` and `..`?
      // TODO If so, just start with index 2 and drop the
      // `isDirentryToIgnore(i)` condition to check.
      for (auto i = 0ul; i < directoryLister.GetNumberOfFiles(); ++i) {
        char const* const fname = directoryLister.GetFile(i);
        // Skip entries to ignore or that aren't directories.
        if (isDirentryToIgnore(fname)) {
          continue;
        }

        if (!this->Names) {
          if (directoryLister.FileIsDirectory(i)) {
            this->Matches.emplace_back(fname);
          }
        } else {
          for (auto const& n : *this->Names) {
            // NOTE Customization point for
            // `cmMacProjectDirectoryListGenerator`
            auto const name = this->TransformNameBeforeCmp(n);
            // Skip entries that don't match.
            auto const equal =
              ((this->ExactMatch
                  ? cmsysString_strcasecmp(fname, name.c_str())
                  : cmsysString_strncasecmp(fname, name.c_str(),
                                            name.length())) == 0);
            if (equal) {
              if (directoryLister.FileIsDirectory(i)) {
                this->Matches.emplace_back(fname);
              }
              break;
            }
          }
        }
      }
      // NOTE Customization point for `cmProjectDirectoryListGenerator`
      this->OnMatchesLoaded();

      this->Current = this->Matches.cbegin();
    }

    if (this->Current != this->Matches.cend()) {
      auto candidate = cmStrCat(parent, *this->Current++, '/');
      return candidate;
    }

    return {};
  }

  void Reset()
  {
    this->Matches.clear();
    this->Current = this->Matches.cbegin();
  }

protected:
  virtual void OnMatchesLoaded() {}
  virtual std::string TransformNameBeforeCmp(std::string same) { return same; }

  std::vector<std::string> const* Names;
  bool const ExactMatch;
  std::vector<std::string> Matches;
  std::vector<std::string>::const_iterator Current;
};

class cmProjectDirectoryListGenerator : public cmDirectoryListGenerator
{
public:
  cmProjectDirectoryListGenerator(std::vector<std::string> const* names,
                                  cmFindPackageCommand::SortOrderType so,
                                  cmFindPackageCommand::SortDirectionType sd,
                                  bool exactMatch)
    : cmDirectoryListGenerator{ names, exactMatch }
    , SortOrder{ so }
    , SortDirection{ sd }
  {
  }

protected:
  void OnMatchesLoaded() override
  {
    // check if there is a specific sorting order to perform
    if (this->SortOrder != cmFindPackageCommand::None) {
      cmFindPackageCommand::Sort(this->Matches.begin(), this->Matches.end(),
                                 this->SortOrder, this->SortDirection);
    }
  }

private:
  // sort parameters
  cmFindPackageCommand::SortOrderType const SortOrder;
  cmFindPackageCommand::SortDirectionType const SortDirection;
};

class cmMacProjectDirectoryListGenerator : public cmDirectoryListGenerator
{
public:
  cmMacProjectDirectoryListGenerator(std::vector<std::string> const* names,
                                     cm::string_view ext)
    : cmDirectoryListGenerator{ names, true }
    , Extension{ ext }
  {
  }

protected:
  std::string TransformNameBeforeCmp(std::string name) override
  {
    return cmStrCat(name, this->Extension);
  }

private:
  cm::string_view const Extension;
};

class cmAnyDirectoryListGenerator : public cmProjectDirectoryListGenerator
{
public:
  cmAnyDirectoryListGenerator(cmFindPackageCommand::SortOrderType so,
                              cmFindPackageCommand::SortDirectionType sd)
    : cmProjectDirectoryListGenerator(nullptr, so, sd, false)
  {
  }
};

#if defined(__LCC__)
#  define CM_LCC_DIAG_SUPPRESS_1222
#  pragma diag_suppress 1222 // invalid error number (3288, but works anyway)
#  define CM_LCC_DIAG_SUPPRESS_3288
#  pragma diag_suppress 3288 // parameter was declared but never referenced
#  define CM_LCC_DIAG_SUPPRESS_3301
#  pragma diag_suppress 3301 // parameter was declared but never referenced
#  define CM_LCC_DIAG_SUPPRESS_3308
#  pragma diag_suppress 3308 // parameter was declared but never referenced
#endif

void ResetGenerator()
{
}

template <typename Generator>
void ResetGenerator(Generator&& generator)
{
  std::forward<Generator&&>(generator).Reset();
}

template <typename Generator, typename... Generators>
void ResetGenerator(Generator&& generator, Generators&&... generators)
{
  ResetGenerator(std::forward<Generator&&>(generator));
  ResetGenerator(std::forward<Generators&&>(generators)...);
}

template <typename CallbackFn>
bool TryGeneratedPaths(CallbackFn&& filesCollector,
                       cmFindPackageCommand::PackageDescriptionType type,
                       std::string const& fullPath)
{
  assert(!fullPath.empty() && fullPath.back() == '/');
  return std::forward<CallbackFn&&>(filesCollector)(fullPath, type);
}

template <typename CallbackFn, typename Generator, typename... Rest>
bool TryGeneratedPaths(CallbackFn&& filesCollector,
                       cmFindPackageCommand::PackageDescriptionType type,
                       std::string const& startPath, Generator&& gen,
                       Rest&&... tail)
{
  ResetGenerator(std::forward<Generator&&>(gen));
  for (auto path = gen.GetNextCandidate(startPath); !path.empty();
       path = gen.GetNextCandidate(startPath)) {
    ResetGenerator(std::forward<Rest&&>(tail)...);
    if (TryGeneratedPaths(std::forward<CallbackFn&&>(filesCollector), type,
                          path, std::forward<Rest&&>(tail)...)) {
      return true;
    }
  }
  return false;
}

#ifdef CM_LCC_DIAG_SUPPRESS_3308
#  undef CM_LCC_DIAG_SUPPRESS_3308
#  pragma diag_default 3308
#endif

#ifdef CM_LCC_DIAG_SUPPRESS_3301
#  undef CM_LCC_DIAG_SUPPRESS_3301
#  pragma diag_default 3301
#endif

#ifdef CM_LCC_DIAG_SUPPRESS_3288
#  undef CM_LCC_DIAG_SUPPRESS_3288
#  pragma diag_default 3288
#endif

#ifdef CM_LCC_DIAG_SUPPRESS_1222
#  undef CM_LCC_DIAG_SUPPRESS_1222
#  pragma diag_default 1222
#endif

// Parse the version number and store the results that were
// successfully parsed.
unsigned int parseVersion(std::string const& version, unsigned int& major,
                          unsigned int& minor, unsigned int& patch,
                          unsigned int& tweak)
{
  return static_cast<unsigned int>(std::sscanf(
    version.c_str(), "%u.%u.%u.%u", &major, &minor, &patch, &tweak));
}

} // anonymous namespace

class cmFindPackageCommand::FlushDebugBufferOnExit
{
  cmFindPackageCommand& Command;

public:
  FlushDebugBufferOnExit(cmFindPackageCommand& command)
    : Command(command)
  {
  }
  ~FlushDebugBufferOnExit()
  {
    if (!Command.DebugBuffer.empty()) {
      Command.DebugMessage(Command.DebugBuffer);
    }
  }
};

class cmFindPackageCommand::PushPopRootPathStack
{
  cmFindPackageCommand& Command;

public:
  PushPopRootPathStack(cmFindPackageCommand& command)
    : Command(command)
  {
    Command.PushFindPackageRootPathStack();
  }
  ~PushPopRootPathStack() { Command.PopFindPackageRootPathStack(); }
};

class cmFindPackageCommand::SetRestoreFindDefinitions
{
  cmFindPackageCommand& Command;

public:
  SetRestoreFindDefinitions(cmFindPackageCommand& command)
    : Command(command)
  {
    Command.SetModuleVariables();
  }
  ~SetRestoreFindDefinitions() { Command.RestoreFindDefinitions(); }
};

cmFindPackageCommand::PathLabel
  cmFindPackageCommand::PathLabel::PackageRedirect("PACKAGE_REDIRECT");
cmFindPackageCommand::PathLabel cmFindPackageCommand::PathLabel::UserRegistry(
  "PACKAGE_REGISTRY");
cmFindPackageCommand::PathLabel cmFindPackageCommand::PathLabel::Builds(
  "BUILDS");
cmFindPackageCommand::PathLabel
  cmFindPackageCommand::PathLabel::SystemRegistry("SYSTEM_PACKAGE_REGISTRY");

cm::string_view const cmFindPackageCommand::VERSION_ENDPOINT_INCLUDED(
  "INCLUDE");
cm::string_view const cmFindPackageCommand::VERSION_ENDPOINT_EXCLUDED(
  "EXCLUDE");

void cmFindPackageCommand::Sort(std::vector<std::string>::iterator begin,
                                std::vector<std::string>::iterator end,
                                SortOrderType const order,
                                SortDirectionType const dir)
{
  if (order == Name_order) {
    if (dir == Dec) {
      std::sort(begin, end, std::greater<std::string>());
    } else {
      std::sort(begin, end);
    }
  } else if (order == Natural) {
    // natural order uses letters and numbers (contiguous numbers digit are
    // compared such that e.g. 000  00 < 01 < 010 < 09 < 0 < 1 < 9 < 10
    if (dir == Dec) {
      std::sort(begin, end, StrverscmpOp<std::greater>());
    } else {
      std::sort(begin, end, StrverscmpOp<std::less>());
    }
  }
  // else do not sort
}

cmFindPackageCommand::cmFindPackageCommand(cmExecutionStatus& status)
  : cmFindCommon(status)
  , VersionRangeMin(VERSION_ENDPOINT_INCLUDED)
  , VersionRangeMax(VERSION_ENDPOINT_INCLUDED)
{
  this->CMakePathName = "PACKAGE";
  this->AppendSearchPathGroups();

  this->DeprecatedFindModules["Boost"] = cmPolicies::CMP0167;
  this->DeprecatedFindModules["CABLE"] = cmPolicies::CMP0191;
  this->DeprecatedFindModules["CUDA"] = cmPolicies::CMP0146;
  this->DeprecatedFindModules["Dart"] = cmPolicies::CMP0145;
  this->DeprecatedFindModules["GCCXML"] = cmPolicies::CMP0188;
  this->DeprecatedFindModules["PythonInterp"] = cmPolicies::CMP0148;
  this->DeprecatedFindModules["PythonLibs"] = cmPolicies::CMP0148;
  this->DeprecatedFindModules["Qt"] = cmPolicies::CMP0084;
}

cmFindPackageCommand::~cmFindPackageCommand()
{
  if (this->DebugState) {
    this->DebugState->Write();
  }
}

void cmFindPackageCommand::AppendSearchPathGroups()
{
  // Update the All group with new paths. Note that package redirection must
  // take precedence over everything else, so it has to be first in the array.
  std::vector<cmFindCommon::PathLabel>* const labels =
    &this->PathGroupLabelMap[PathGroup::All];
  labels->insert(labels->begin(), PathLabel::PackageRedirect);
  labels->insert(
    std::find(labels->begin(), labels->end(), PathLabel::CMakeSystem),
    PathLabel::UserRegistry);
  labels->insert(
    std::find(labels->begin(), labels->end(), PathLabel::CMakeSystem),
    PathLabel::Builds);
  labels->insert(std::find(labels->begin(), labels->end(), PathLabel::Guess),
                 PathLabel::SystemRegistry);

  // Create the new path objects
  this->LabeledPaths.emplace(PathLabel::PackageRedirect, cmSearchPath{ this });
  this->LabeledPaths.emplace(PathLabel::UserRegistry, cmSearchPath{ this });
  this->LabeledPaths.emplace(PathLabel::Builds, cmSearchPath{ this });
  this->LabeledPaths.emplace(PathLabel::SystemRegistry, cmSearchPath{ this });
}

void cmFindPackageCommand::InheritOptions(cmFindPackageCommand* other)
{
  this->RequiredCMakeVersion = other->RequiredCMakeVersion;
  this->LibraryArchitecture = other->LibraryArchitecture;
  this->UseLib32Paths = other->UseLib32Paths;
  this->UseLib64Paths = other->UseLib64Paths;
  this->UseLibx32Paths = other->UseLibx32Paths;
  this->NoUserRegistry = other->NoUserRegistry;
  this->NoSystemRegistry = other->NoSystemRegistry;
  this->UseRealPath = other->UseRealPath;
  this->SortOrder = other->SortOrder;
  this->SortDirection = other->SortDirection;

  this->GlobalScope = other->GlobalScope;
  this->RegistryView = other->RegistryView;
  this->NoDefaultPath = other->NoDefaultPath;
  this->NoPackageRootPath = other->NoPackageRootPath;
  this->NoCMakePath = other->NoCMakePath;
  this->NoCMakeEnvironmentPath = other->NoCMakeEnvironmentPath;
  this->NoSystemEnvironmentPath = other->NoSystemEnvironmentPath;
  this->NoCMakeSystemPath = other->NoCMakeSystemPath;
  this->NoCMakeInstallPath = other->NoCMakeInstallPath;
  this->FindRootPathMode = other->FindRootPathMode;

  this->SearchFrameworkLast = other->SearchFrameworkLast;
  this->SearchFrameworkFirst = other->SearchFrameworkFirst;
  this->SearchFrameworkOnly = other->SearchFrameworkOnly;
  this->SearchAppBundleLast = other->SearchAppBundleLast;
  this->SearchAppBundleFirst = other->SearchAppBundleFirst;
  this->SearchAppBundleOnly = other->SearchAppBundleOnly;
  this->SearchPathSuffixes = other->SearchPathSuffixes;

  this->Quiet = other->Quiet;
}

bool cmFindPackageCommand::IsFound() const
{
  return this->InitialState == FindState::Found;
}

bool cmFindPackageCommand::IsDefined() const
{
  return this->InitialState == FindState::Found ||
    this->InitialState == FindState::NotFound;
}

bool cmFindPackageCommand::InitialPass(std::vector<std::string> const& args)
{
  if (args.empty()) {
    this->SetError("called with incorrect number of arguments");
    return false;
  }

  if (this->Makefile->GetStateSnapshot().GetUnwindState() ==
      cmStateEnums::UNWINDING) {
    this->SetError("called while already in an UNWIND state");
    return false;
  }

  // Lookup required version of CMake.
  if (cmValue const rv =
        this->Makefile->GetDefinition("CMAKE_MINIMUM_REQUIRED_VERSION")) {
    unsigned int v[3] = { 0, 0, 0 };
    std::sscanf(rv->c_str(), "%u.%u.%u", &v[0], &v[1], &v[2]);
    this->RequiredCMakeVersion = CMake_VERSION_ENCODE(v[0], v[1], v[2]);
  }

  // Lookup target architecture, if any.
  if (cmValue const arch =
        this->Makefile->GetDefinition("CMAKE_LIBRARY_ARCHITECTURE")) {
    this->LibraryArchitecture = *arch;
  }

  // Lookup whether lib32 paths should be used.
  if (this->Makefile->PlatformIs32Bit() &&
      this->Makefile->GetState()->GetGlobalPropertyAsBool(
        "FIND_LIBRARY_USE_LIB32_PATHS")) {
    this->UseLib32Paths = true;
  }

  // Lookup whether lib64 paths should be used.
  if (this->Makefile->PlatformIs64Bit() &&
      this->Makefile->GetState()->GetGlobalPropertyAsBool(
        "FIND_LIBRARY_USE_LIB64_PATHS")) {
    this->UseLib64Paths = true;
  }

  // Lookup whether libx32 paths should be used.
  if (this->Makefile->PlatformIsx32() &&
      this->Makefile->GetState()->GetGlobalPropertyAsBool(
        "FIND_LIBRARY_USE_LIBX32_PATHS")) {
    this->UseLibx32Paths = true;
  }

  // Check if User Package Registry should be disabled
  // The `CMAKE_FIND_USE_PACKAGE_REGISTRY` has
  // priority over the deprecated CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY
  if (cmValue const def =
        this->Makefile->GetDefinition("CMAKE_FIND_USE_PACKAGE_REGISTRY")) {
    this->NoUserRegistry = !def.IsOn();
  } else if (this->Makefile->IsOn("CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY")) {
    this->NoUserRegistry = true;
  }

  // Check if System Package Registry should be disabled
  // The `CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY` has
  // priority over the deprecated CMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY
  if (cmValue const def = this->Makefile->GetDefinition(
        "CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY")) {
    this->NoSystemRegistry = !def.IsOn();
  } else if (this->Makefile->IsOn(
               "CMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY")) {
    this->NoSystemRegistry = true;
  }

  // Check whether we should resolve symlinks when finding packages
  if (this->Makefile->IsOn("CMAKE_FIND_PACKAGE_RESOLVE_SYMLINKS")) {
    this->UseRealPath = true;
  }

  // Check if Sorting should be enabled
  if (cmValue const so =
        this->Makefile->GetDefinition("CMAKE_FIND_PACKAGE_SORT_ORDER")) {

    if (*so == "NAME") {
      this->SortOrder = Name_order;
    } else if (*so == "NATURAL") {
      this->SortOrder = Natural;
    } else {
      this->SortOrder = None;
    }
  }
  if (cmValue const sd =
        this->Makefile->GetDefinition("CMAKE_FIND_PACKAGE_SORT_DIRECTION")) {
    this->SortDirection = (*sd == "DEC") ? Dec : Asc;
  }

  // Find what search path locations have been enabled/disable.
  this->SelectDefaultSearchModes();

  // Find the current root path mode.
  this->SelectDefaultRootPathMode();

  // Find the current bundle/framework search policy.
  this->SelectDefaultMacMode();

  // Record options.
  this->Name = args[0];
  cm::string_view componentsSep = ""_s;

  // Always search directly in a generated path.
  this->SearchPathSuffixes.emplace_back();

  // Process debug mode
  cmMakefile::DebugFindPkgRAII debugFindPkgRAII(this->Makefile, this->Name);
  this->FullDebugMode = this->ComputeIfDebugModeWanted();
  if (this->FullDebugMode || !this->ComputeIfImplicitDebugModeSuppressed()) {
    this->DebugState = cm::make_unique<cmFindPackageDebugState>(this);
  }

  // Parse the arguments.
  enum Doing
  {
    DoingNone,
    DoingComponents,
    DoingOptionalComponents,
    DoingNames,
    DoingPaths,
    DoingPathSuffixes,
    DoingConfigs,
    DoingHints
  };
  Doing doing = DoingNone;
  cmsys::RegularExpression versionRegex(
    R"V(^([0-9]+(\.[0-9]+)*)(\.\.\.(<?)([0-9]+(\.[0-9]+)*))?$)V");
  bool haveVersion = false;
  std::vector<std::size_t> configArgs;
  std::vector<std::size_t> moduleArgs;
  for (std::size_t i = 1u; i < args.size(); ++i) {
    if (args[i] == "QUIET") {
      this->Quiet = true;
      doing = DoingNone;
    } else if (args[i] == "BYPASS_PROVIDER") {
      this->BypassProvider = true;
      doing = DoingNone;
    } else if (args[i] == "EXACT") {
      this->VersionExact = true;
      doing = DoingNone;
    } else if (args[i] == "GLOBAL") {
      this->GlobalScope = true;
      doing = DoingNone;
    } else if (args[i] == "MODULE") {
      moduleArgs.push_back(i);
      doing = DoingNone;
      // XXX(clang-tidy): https://bugs.llvm.org/show_bug.cgi?id=44165
      // NOLINTNEXTLINE(bugprone-branch-clone)
    } else if (args[i] == "CONFIG") {
      configArgs.push_back(i);
      doing = DoingNone;
      // XXX(clang-tidy): https://bugs.llvm.org/show_bug.cgi?id=44165
      // NOLINTNEXTLINE(bugprone-branch-clone)
    } else if (args[i] == "NO_MODULE") {
      configArgs.push_back(i);
      doing = DoingNone;
    } else if (args[i] == "REQUIRED") {
      if (this->Required == RequiredStatus::OptionalExplicit) {
        this->SetError("cannot be both REQUIRED and OPTIONAL");
        return false;
      }
      this->Required = RequiredStatus::RequiredExplicit;
      doing = DoingComponents;
    } else if (args[i] == "OPTIONAL") {
      if (this->Required == RequiredStatus::RequiredExplicit) {
        this->SetError("cannot be both REQUIRED and OPTIONAL");
        return false;
      }
      this->Required = RequiredStatus::OptionalExplicit;
      doing = DoingComponents;
    } else if (args[i] == "COMPONENTS") {
      doing = DoingComponents;
    } else if (args[i] == "OPTIONAL_COMPONENTS") {
      doing = DoingOptionalComponents;
    } else if (args[i] == "NAMES") {
      configArgs.push_back(i);
      doing = DoingNames;
    } else if (args[i] == "PATHS") {
      configArgs.push_back(i);
      doing = DoingPaths;
    } else if (args[i] == "HINTS") {
      configArgs.push_back(i);
      doing = DoingHints;
    } else if (args[i] == "PATH_SUFFIXES") {
      configArgs.push_back(i);
      doing = DoingPathSuffixes;
    } else if (args[i] == "CONFIGS") {
      configArgs.push_back(i);
      doing = DoingConfigs;
    } else if (args[i] == "NO_POLICY_SCOPE") {
      this->PolicyScope = false;
      doing = DoingNone;
    } else if (args[i] == "NO_CMAKE_PACKAGE_REGISTRY") {
      this->NoUserRegistry = true;
      configArgs.push_back(i);
      doing = DoingNone;
    } else if (args[i] == "NO_CMAKE_SYSTEM_PACKAGE_REGISTRY") {
      this->NoSystemRegistry = true;
      configArgs.push_back(i);
      doing = DoingNone;
      // XXX(clang-tidy): https://bugs.llvm.org/show_bug.cgi?id=44165
      // NOLINTNEXTLINE(bugprone-branch-clone)
    } else if (args[i] == "NO_CMAKE_BUILDS_PATH") {
      // Ignore legacy option.
      configArgs.push_back(i);
      doing = DoingNone;
    } else if (args[i] == "REGISTRY_VIEW") {
      if (++i == args.size()) {
        this->SetError("missing required argument for REGISTRY_VIEW");
        return false;
      }
      auto view = cmWindowsRegistry::ToView(args[i]);
      if (view) {
        this->RegistryView = *view;
        this->RegistryViewDefined = true;
      } else {
        this->SetError(
          cmStrCat("given invalid value for REGISTRY_VIEW: ", args[i]));
        return false;
      }
    } else if (args[i] == "UNWIND_INCLUDE") {
      if (this->Makefile->GetStateSnapshot().GetUnwindType() !=
          cmStateEnums::CAN_UNWIND) {
        this->SetError("called with UNWIND_INCLUDE in an invalid context");
        return false;
      }
      this->ScopeUnwind = true;
      doing = DoingNone;
    } else if (this->CheckCommonArgument(args[i])) {
      configArgs.push_back(i);
      doing = DoingNone;
    } else if ((doing == DoingComponents) ||
               (doing == DoingOptionalComponents)) {
      // Set a variable telling the find script whether this component
      // is required.
      if (doing == DoingOptionalComponents) {
        this->OptionalComponents.insert(args[i]);
      } else {
        this->RequiredComponents.insert(args[i]);
      }

      // Append to the list of required components.
      this->Components += componentsSep;
      this->Components += args[i];
      componentsSep = ";"_s;
    } else if (doing == DoingNames) {
      this->Names.push_back(args[i]);
    } else if (doing == DoingPaths) {
      this->UserGuessArgs.push_back(args[i]);
    } else if (doing == DoingHints) {
      this->UserHintsArgs.push_back(args[i]);
    } else if (doing == DoingPathSuffixes) {
      this->AddPathSuffix(args[i]);
    } else if (doing == DoingConfigs) {
      if (args[i].find_first_of(":/\\") != std::string::npos ||
          cmSystemTools::GetFilenameLastExtension(args[i]) != ".cmake") {
        this->SetError(cmStrCat(
          "given CONFIGS option followed by invalid file name \"", args[i],
          "\".  The names given must be file names without "
          "a path and with a \".cmake\" extension."));
        return false;
      }
      this->Configs.emplace_back(args[i], pdt::CMake);
    } else if (!haveVersion && versionRegex.find(args[i])) {
      haveVersion = true;
      this->VersionComplete = args[i];
    } else {
      this->SetError(
        cmStrCat("called with invalid argument \"", args[i], '"'));
      return false;
    }
  }

  if (this->Required == RequiredStatus::Optional &&
      this->Makefile->IsOn("CMAKE_FIND_REQUIRED")) {
    this->Required = RequiredStatus::RequiredFromFindVar;
  }

  if (!this->GlobalScope) {
    cmValue value(
      this->Makefile->GetDefinition("CMAKE_FIND_PACKAGE_TARGETS_GLOBAL"));
    this->GlobalScope = value.IsOn();
  }

  std::vector<std::string> doubledComponents;
  std::set_intersection(
    this->RequiredComponents.begin(), this->RequiredComponents.end(),
    this->OptionalComponents.begin(), this->OptionalComponents.end(),
    std::back_inserter(doubledComponents));
  if (!doubledComponents.empty()) {
    this->SetError(
      cmStrCat("called with components that are both required and "
               "optional:\n",
               cmWrap("  ", doubledComponents, "", "\n"), '\n'));
    return false;
  }

  // Check and eliminate search modes not allowed by the args provided
  this->UseFindModules = configArgs.empty();
  this->UseConfigFiles = moduleArgs.empty();
  if (this->UseConfigFiles &&
      cmExperimental::HasSupportEnabled(
        *this->Makefile, cmExperimental::Feature::ImportPackageInfo)) {
    this->UseCpsFiles = this->Configs.empty();
  } else {
    this->UseCpsFiles = false;
  }
  if (!this->UseFindModules && !this->UseConfigFiles) {
    std::ostringstream e;
    e << "given options exclusive to Module mode:\n";
    for (auto si : moduleArgs) {
      e << "  " << args[si] << "\n";
    }
    e << "and options exclusive to Config mode:\n";
    for (auto si : configArgs) {
      e << "  " << args[si] << "\n";
    }
    e << "The options are incompatible.";
    this->SetError(e.str());
    return false;
  }

  bool canBeIrrelevant = true;
  if (this->UseConfigFiles || this->UseCpsFiles) {
    canBeIrrelevant = false;
    if (cmValue v = this->Makefile->GetState()->GetCacheEntryValue(
          cmStrCat(this->Name, "_DIR"))) {
      if (!v.IsNOTFOUND()) {
        this->InitialState = FindState::Found;
      } else {
        this->InitialState = FindState::NotFound;
      }
    }
  }

  if (this->UseFindModules &&
      (this->InitialState == FindState::Undefined ||
       this->InitialState == FindState::NotFound)) {
    // There are no definitive cache variables to know if a given `Find` module
    // has been searched for or not. However, if we have a `_FOUND` variable,
    // use that as an indication of a previous search.
    if (cmValue v =
          this->Makefile->GetDefinition(cmStrCat(this->Name, "_FOUND"))) {
      if (v.IsOn()) {
        this->InitialState = FindState::Found;
      } else {
        this->InitialState = FindState::NotFound;
      }
    }
  }

  // If there is no signaling variable and there's no reason to expect a cache
  // variable, mark the initial state as "irrelevant".
  if (this->InitialState == FindState::Undefined && canBeIrrelevant) {
    this->InitialState = FindState::Irrelevant;
  }

  // Ignore EXACT with no version.
  if (this->VersionComplete.empty() && this->VersionExact) {
    this->VersionExact = false;
    this->Makefile->IssueMessage(
      MessageType::AUTHOR_WARNING,
      "Ignoring EXACT since no version is requested.");
  }

  if (this->VersionComplete.empty() || this->Components.empty()) {
    // Check whether we are recursing inside "Find<name>.cmake" within
    // another find_package(<name>) call.
    std::string const mod = cmStrCat(this->Name, "_FIND_MODULE");
    if (this->Makefile->IsOn(mod)) {
      if (this->VersionComplete.empty()) {
        // Get version information from the outer call if necessary.
        // Requested version string.
        std::string const ver = cmStrCat(this->Name, "_FIND_VERSION_COMPLETE");
        this->VersionComplete = this->Makefile->GetSafeDefinition(ver);

        // Whether an exact version is required.
        std::string const exact = cmStrCat(this->Name, "_FIND_VERSION_EXACT");
        this->VersionExact = this->Makefile->IsOn(exact);
      }
      if (this->Components.empty()) {
        std::string const componentsVar = this->Name + "_FIND_COMPONENTS";
        this->Components = this->Makefile->GetSafeDefinition(componentsVar);
        for (auto const& component : cmList{ this->Components }) {
          std::string const crVar =
            cmStrCat(this->Name, "_FIND_REQUIRED_"_s, component);
          if (this->Makefile->GetDefinition(crVar).IsOn()) {
            this->RequiredComponents.insert(component);
          } else {
            this->OptionalComponents.insert(component);
          }
        }
      }
    }
  }

  // fill various parts of version specification
  if (!this->VersionComplete.empty()) {
    if (!versionRegex.find(this->VersionComplete)) {
      this->SetError("called with invalid version specification.");
      return false;
    }

    this->Version = versionRegex.match(1);
    this->VersionMax = versionRegex.match(5);
    if (versionRegex.match(4) == "<"_s) {
      this->VersionRangeMax = VERSION_ENDPOINT_EXCLUDED;
    }
    if (!this->VersionMax.empty()) {
      this->VersionRange = this->VersionComplete;
    }
  }

  if (!this->VersionRange.empty()) {
    // version range must not be empty
    if ((this->VersionRangeMax == VERSION_ENDPOINT_INCLUDED &&
         cmSystemTools::VersionCompareGreater(this->Version,
                                              this->VersionMax)) ||
        (this->VersionRangeMax == VERSION_ENDPOINT_EXCLUDED &&
         cmSystemTools::VersionCompareGreaterEq(this->Version,
                                                this->VersionMax))) {
      this->SetError("specified version range is empty.");
      return false;
    }
  }

  if (this->VersionExact && !this->VersionRange.empty()) {
    this->SetError("EXACT cannot be specified with a version range.");
    return false;
  }

  if (!this->Version.empty()) {
    this->VersionCount =
      parseVersion(this->Version, this->VersionMajor, this->VersionMinor,
                   this->VersionPatch, this->VersionTweak);
  }
  if (!this->VersionMax.empty()) {
    this->VersionMaxCount = parseVersion(
      this->VersionMax, this->VersionMaxMajor, this->VersionMaxMinor,
      this->VersionMaxPatch, this->VersionMaxTweak);
  }

  bool result = this->FindPackage(
    this->BypassProvider ? std::vector<std::string>{} : args);

  std::string const foundVar = cmStrCat(this->Name, "_FOUND");
  bool const isFound = this->Makefile->IsOn(foundVar) ||
    this->Makefile->IsOn(cmSystemTools::UpperCase(foundVar));

  if (this->ScopeUnwind && (!result || !isFound)) {
    this->Makefile->GetStateSnapshot().SetUnwindState(cmStateEnums::UNWINDING);
  }

  return result;
}

bool cmFindPackageCommand::FindPackage(
  std::vector<std::string> const& argsForProvider)
{
  std::string const makePackageRequiredVar =
    cmStrCat("CMAKE_REQUIRE_FIND_PACKAGE_", this->Name);
  bool const makePackageRequiredSet =
    this->Makefile->IsOn(makePackageRequiredVar);
  if (makePackageRequiredSet) {
    if (this->IsRequired()) {
      this->Makefile->IssueMessage(
        MessageType::WARNING,
        cmStrCat("for module ", this->Name,
                 " already called with REQUIRED, thus ",
                 makePackageRequiredVar, " has no effect."));
    } else {
      this->Required = RequiredStatus::RequiredFromPackageVar;
    }
  }

  std::string const disableFindPackageVar =
    cmStrCat("CMAKE_DISABLE_FIND_PACKAGE_", this->Name);
  if (this->Makefile->IsOn(disableFindPackageVar)) {
    if (this->IsRequired()) {
      this->SetError(
        cmStrCat("for module ", this->Name,
                 (makePackageRequiredSet
                    ? " was made REQUIRED with " + makePackageRequiredVar
                    : " called with REQUIRED, "),
                 " but ", disableFindPackageVar,
                 " is enabled. A REQUIRED package cannot be disabled."));
      return false;
    }
    return true;
  }

  // Restore PACKAGE_PREFIX_DIR to its pre-call value when we return. If our
  // caller is a file generated by configure_package_config_file(), and if
  // the package we are about to load also has a config file created by that
  // command, it will overwrite PACKAGE_PREFIX_DIR. We need to restore it in
  // case something still refers to it in our caller's scope after we return.
  class RestoreVariableOnLeavingScope
  {
    cmMakefile* makefile_;
    cm::optional<std::string> value_;

  public:
    RestoreVariableOnLeavingScope(cmMakefile* makefile)
      : makefile_(makefile)
    {
      cmValue v = makefile->GetDefinition("PACKAGE_PREFIX_DIR");
      if (v) {
        value_ = *v;
      }
    }
    ~RestoreVariableOnLeavingScope()
    {
      if (this->value_) {
        makefile_->AddDefinition("PACKAGE_PREFIX_DIR", *value_);
      } else {
        makefile_->RemoveDefinition("PACKAGE_PREFIX_DIR");
      }
    }
  };
  RestoreVariableOnLeavingScope restorePackagePrefixDir(this->Makefile);

  // Now choose what method(s) we will use to satisfy the request. Note that
  // we still want all the above checking of arguments, etc. regardless of the
  // method used. This will ensure ill-formed arguments are caught earlier,
  // before things like dependency providers need to deal with them.

  // A dependency provider (if set) gets first look before other methods.
  // We do this before modifying the package root path stack because a
  // provider might use methods that ignore that.
  cmState* const state = this->Makefile->GetState();
  cmState::Command const providerCommand = state->GetDependencyProviderCommand(
    cmDependencyProvider::Method::FindPackage);
  if (argsForProvider.empty()) {
    if (this->DebugModeEnabled() && providerCommand) {
      this->DebugMessage(
        "BYPASS_PROVIDER given, skipping dependency provider");
    }
  } else if (providerCommand) {
    if (this->DebugModeEnabled()) {
      this->DebugMessage(cmStrCat("Trying dependency provider command: ",
                                  state->GetDependencyProvider()->GetCommand(),
                                  "()"));
    }
    std::vector<cmListFileArgument> listFileArgs(argsForProvider.size() + 1);
    listFileArgs[0] =
      cmListFileArgument("FIND_PACKAGE", cmListFileArgument::Unquoted, 0);
    std::transform(argsForProvider.begin(), argsForProvider.end(),
                   listFileArgs.begin() + 1, [](std::string const& arg) {
                     return cmListFileArgument(arg,
                                               cmListFileArgument::Bracket, 0);
                   });
    if (!providerCommand(listFileArgs, this->Status)) {
      return false;
    }
    std::string providerName;
    if (auto depProvider = state->GetDependencyProvider()) {
      providerName = depProvider->GetCommand();
    } else {
      providerName = "<no provider?>";
    }
    auto searchPath = cmStrCat("dependency_provider::", providerName);
    if (this->Makefile->IsOn(cmStrCat(this->Name, "_FOUND"))) {
      if (this->DebugModeEnabled()) {
        this->DebugMessage("Package was found by the dependency provider");
      }
      if (this->DebugState) {
        this->DebugState->FoundAt(searchPath);
      }
      this->FileFound = searchPath;
      this->FileFoundMode = FoundPackageMode::Provider;
      this->AppendSuccessInformation();
      return true;
    }
    this->ConsideredPaths.emplace_back(searchPath, FoundPackageMode::Provider,
                                       SearchResult::NotFound);
  }

  // Limit package nesting depth well below the recursion depth limit because
  // find_package nesting uses more stack space than normal recursion.
  {
    static std::size_t const findPackageDepthMinMax = 100;
    std::size_t const findPackageDepthMax = std::max(
      this->Makefile->GetRecursionDepthLimit() / 2, findPackageDepthMinMax);
    std::size_t const findPackageDepth =
      this->Makefile->FindPackageRootPathStack.size() + 1;
    if (findPackageDepth > findPackageDepthMax) {
      this->SetError(cmStrCat("maximum nesting depth of ", findPackageDepthMax,
                              " exceeded."));
      return false;
    }
  }

  // RAII objects to ensure we leave this function with consistent state.
  FlushDebugBufferOnExit flushDebugBufferOnExit(*this);
  PushPopRootPathStack pushPopRootPathStack(*this);
  SetRestoreFindDefinitions setRestoreFindDefinitions(*this);
  cmFindPackageStackRAII findPackageStackRAII(this->Makefile, this->Name);

  // See if we have been told to delegate to FetchContent or some other
  // redirected config package first. We have to check all names that
  // find_package() may look for, but only need to invoke the override for the
  // first one that matches.
  auto overrideNames = this->Names;
  if (overrideNames.empty()) {
    overrideNames.push_back(this->Name);
  }
  bool forceConfigMode = false;
  auto const redirectsDir =
    this->Makefile->GetSafeDefinition("CMAKE_FIND_PACKAGE_REDIRECTS_DIR");
  for (auto const& overrideName : overrideNames) {
    auto const nameLower = cmSystemTools::LowerCase(overrideName);
    auto const delegatePropName =
      cmStrCat("_FetchContent_", nameLower, "_override_find_package");
    cmValue const delegateToFetchContentProp =
      this->Makefile->GetState()->GetGlobalProperty(delegatePropName);
    if (delegateToFetchContentProp.IsOn()) {
      // When this property is set, the FetchContent module has already been
      // included at least once, so we know the FetchContent_MakeAvailable()
      // command will be defined. Any future find_package() calls after this
      // one for this package will by-pass this once-only delegation.
      // The following call will typically create a <name>-config.cmake file
      // in the redirectsDir, which we still want to process like any other
      // config file to ensure we follow normal find_package() processing.
      cmListFileFunction func(
        "FetchContent_MakeAvailable", 0, 0,
        { cmListFileArgument(overrideName, cmListFileArgument::Unquoted, 0) });
      if (!this->Makefile->ExecuteCommand(func, this->Status)) {
        return false;
      }
    }

    if (cmSystemTools::FileExists(
          cmStrCat(redirectsDir, '/', nameLower, "-config.cmake")) ||
        cmSystemTools::FileExists(
          cmStrCat(redirectsDir, '/', overrideName, "Config.cmake"))) {
      // Force the use of this redirected config package file, regardless of
      // the type of find_package() call. Files in the redirectsDir must always
      // take priority over everything else.
      forceConfigMode = true;
      this->UseConfigFiles = true;
      this->UseFindModules = false;
      this->Names.clear();
      this->Names.emplace_back(overrideName); // Force finding this one
      this->Variable = cmStrCat(this->Name, "_DIR");
      this->SetConfigDirCacheVariable(redirectsDir);
      break;
    }
  }

  // See if there is a Find<PackageName>.cmake module.
  bool loadedPackage = false;
  if (forceConfigMode) {
    loadedPackage = this->FindPackageUsingConfigMode();
  } else if (this->Makefile->IsOn("CMAKE_FIND_PACKAGE_PREFER_CONFIG")) {
    if (this->UseConfigFiles && this->FindPackageUsingConfigMode()) {
      loadedPackage = true;
    } else {
      if (this->FindPackageUsingModuleMode()) {
        loadedPackage = true;
      } else {
        // The package was not loaded. Report errors.
        if (this->HandlePackageMode(HandlePackageModeType::Module)) {
          loadedPackage = true;
        }
      }
    }
  } else {
    if (this->UseFindModules && this->FindPackageUsingModuleMode()) {
      loadedPackage = true;
    } else {
      // Handle CMAKE_FIND_PACKAGE_WARN_NO_MODULE (warn when CONFIG mode is
      // implicitly assumed)
      if (this->UseFindModules && this->UseConfigFiles &&
          this->Makefile->IsOn("CMAKE_FIND_PACKAGE_WARN_NO_MODULE")) {
        std::ostringstream aw;
        if (this->RequiredCMakeVersion >= CMake_VERSION_ENCODE(2, 8, 8)) {
          aw << "find_package called without either MODULE or CONFIG option "
                "and "
                "no Find"
             << this->Name
             << ".cmake module is in CMAKE_MODULE_PATH.  "
                "Add MODULE to exclusively request Module mode and fail if "
                "Find"
             << this->Name
             << ".cmake is missing.  "
                "Add CONFIG to exclusively request Config mode and search for "
                "a "
                "package configuration file provided by "
             << this->Name << " (" << this->Name << "Config.cmake or "
             << cmSystemTools::LowerCase(this->Name) << "-config.cmake).  ";
        } else {
          aw << "find_package called without NO_MODULE option and no "
                "Find"
             << this->Name
             << ".cmake module is in CMAKE_MODULE_PATH.  "
                "Add NO_MODULE to exclusively request Config mode and search "
                "for a "
                "package configuration file provided by "
             << this->Name << " (" << this->Name << "Config.cmake or "
             << cmSystemTools::LowerCase(this->Name)
             << "-config.cmake).  Otherwise make Find" << this->Name
             << ".cmake available in CMAKE_MODULE_PATH.";
        }
        aw << "\n"
              "(Variable CMAKE_FIND_PACKAGE_WARN_NO_MODULE enabled this "
              "warning.)";
        this->Makefile->IssueMessage(MessageType::AUTHOR_WARNING, aw.str());
      }

      if (this->FindPackageUsingConfigMode()) {
        loadedPackage = true;
      }
    }
  }

  this->AppendSuccessInformation();

  return loadedPackage;
}

bool cmFindPackageCommand::FindPackageUsingModuleMode()
{
  bool foundModule = false;
  if (!this->FindModule(foundModule)) {
    return false;
  }
  return foundModule;
}

bool cmFindPackageCommand::FindPackageUsingConfigMode()
{
  this->Variable = cmStrCat(this->Name, "_DIR");

  // Add the default name.
  if (this->Names.empty()) {
    this->Names.push_back(this->Name);
  }

  // Add the default configs.
  if (this->Configs.empty()) {
    for (std::string const& n : this->Names) {
      std::string config;
      if (this->UseCpsFiles) {
        config = cmStrCat(n, ".cps");
        this->Configs.emplace_back(std::move(config), pdt::Cps);

        config = cmStrCat(cmSystemTools::LowerCase(n), ".cps");
        if (config != this->Configs.front().Name) {
          this->Configs.emplace_back(std::move(config), pdt::Cps);
        }
      }

      config = cmStrCat(n, "Config.cmake");
      this->Configs.emplace_back(std::move(config), pdt::CMake);

      config = cmStrCat(cmSystemTools::LowerCase(n), "-config.cmake");
      this->Configs.emplace_back(std::move(config), pdt::CMake);
    }
  }

  // get igonored paths from vars and reroot them.
  std::vector<std::string> ignored;
  this->GetIgnoredPaths(ignored);
  this->RerootPaths(ignored);

  // Construct a set of ignored paths
  this->IgnoredPaths.clear();
  this->IgnoredPaths.insert(ignored.begin(), ignored.end());

  // get igonored prefix paths from vars and reroot them.
  std::vector<std::string> ignoredPrefixes;
  this->GetIgnoredPrefixPaths(ignoredPrefixes);
  this->RerootPaths(ignoredPrefixes);

  // Construct a set of ignored prefix paths
  this->IgnoredPrefixPaths.clear();
  this->IgnoredPrefixPaths.insert(ignoredPrefixes.begin(),
                                  ignoredPrefixes.end());

  // Find and load the package.
  return this->HandlePackageMode(HandlePackageModeType::Config);
}

void cmFindPackageCommand::SetVersionVariables(
  std::function<void(std::string const&, cm::string_view)> const&
    addDefinition,
  std::string const& prefix, std::string const& version,
  unsigned int const count, unsigned int const major, unsigned int const minor,
  unsigned int const patch, unsigned int const tweak)
{
  addDefinition(prefix, version);

  char buf[64];
  snprintf(buf, sizeof(buf), "%u", major);
  addDefinition(prefix + "_MAJOR", buf);
  snprintf(buf, sizeof(buf), "%u", minor);
  addDefinition(prefix + "_MINOR", buf);
  snprintf(buf, sizeof(buf), "%u", patch);
  addDefinition(prefix + "_PATCH", buf);
  snprintf(buf, sizeof(buf), "%u", tweak);
  addDefinition(prefix + "_TWEAK", buf);
  snprintf(buf, sizeof(buf), "%u", count);
  addDefinition(prefix + "_COUNT", buf);
}

void cmFindPackageCommand::SetModuleVariables()
{
  this->AddFindDefinition("CMAKE_FIND_PACKAGE_NAME", this->Name);

  // Nested find calls are not automatically required.
  this->AddFindDefinition("CMAKE_FIND_REQUIRED", ""_s);

  // Store the list of components and associated variable definitions.
  std::string components_var = this->Name + "_FIND_COMPONENTS";
  this->AddFindDefinition(components_var, this->Components);
  for (auto const& component : this->OptionalComponents) {
    this->AddFindDefinition(
      cmStrCat(this->Name, "_FIND_REQUIRED_"_s, component), "0"_s);
  }
  for (auto const& component : this->RequiredComponents) {
    this->AddFindDefinition(
      cmStrCat(this->Name, "_FIND_REQUIRED_"_s, component), "1"_s);
  }

  if (this->Quiet) {
    // Tell the module that is about to be read that it should find
    // quietly.
    std::string quietly = cmStrCat(this->Name, "_FIND_QUIETLY");
    this->AddFindDefinition(quietly, "1"_s);
  }

  if (this->IsRequired()) {
    // Tell the module that is about to be read that it should report
    // a fatal error if the package is not found.
    std::string req = cmStrCat(this->Name, "_FIND_REQUIRED");
    this->AddFindDefinition(req, "1"_s);
  }

  if (!this->VersionComplete.empty()) {
    std::string req = cmStrCat(this->Name, "_FIND_VERSION_COMPLETE");
    this->AddFindDefinition(req, this->VersionComplete);
  }

  // Tell the module that is about to be read what version of the
  // package has been requested.
  auto addDefinition = [this](std::string const& variable,
                              cm::string_view value) {
    this->AddFindDefinition(variable, value);
  };

  if (!this->Version.empty()) {
    auto prefix = cmStrCat(this->Name, "_FIND_VERSION"_s);
    this->SetVersionVariables(addDefinition, prefix, this->Version,
                              this->VersionCount, this->VersionMajor,
                              this->VersionMinor, this->VersionPatch,
                              this->VersionTweak);

    // Tell the module whether an exact version has been requested.
    auto exact = cmStrCat(this->Name, "_FIND_VERSION_EXACT");
    this->AddFindDefinition(exact, this->VersionExact ? "1"_s : "0"_s);
  }
  if (!this->VersionRange.empty()) {
    auto prefix = cmStrCat(this->Name, "_FIND_VERSION_MIN"_s);
    this->SetVersionVariables(addDefinition, prefix, this->Version,
                              this->VersionCount, this->VersionMajor,
                              this->VersionMinor, this->VersionPatch,
                              this->VersionTweak);

    prefix = cmStrCat(this->Name, "_FIND_VERSION_MAX"_s);
    this->SetVersionVariables(addDefinition, prefix, this->VersionMax,
                              this->VersionMaxCount, this->VersionMaxMajor,
                              this->VersionMaxMinor, this->VersionMaxPatch,
                              this->VersionMaxTweak);

    auto id = cmStrCat(this->Name, "_FIND_VERSION_RANGE");
    this->AddFindDefinition(id, this->VersionRange);
    id = cmStrCat(this->Name, "_FIND_VERSION_RANGE_MIN");
    this->AddFindDefinition(id, this->VersionRangeMin);
    id = cmStrCat(this->Name, "_FIND_VERSION_RANGE_MAX");
    this->AddFindDefinition(id, this->VersionRangeMax);
  }

  if (this->RegistryViewDefined) {
    this->AddFindDefinition(cmStrCat(this->Name, "_FIND_REGISTRY_VIEW"),
                            cmWindowsRegistry::FromView(this->RegistryView));
  }
}

void cmFindPackageCommand::AddFindDefinition(std::string const& var,
                                             cm::string_view const value)
{
  if (cmValue old = this->Makefile->GetDefinition(var)) {
    this->OriginalDefs[var].exists = true;
    this->OriginalDefs[var].value = *old;
  } else {
    this->OriginalDefs[var].exists = false;
  }
  this->Makefile->AddDefinition(var, value);
}

void cmFindPackageCommand::RestoreFindDefinitions()
{
  for (auto const& i : this->OriginalDefs) {
    OriginalDef const& od = i.second;
    if (od.exists) {
      this->Makefile->AddDefinition(i.first, od.value);
    } else {
      this->Makefile->RemoveDefinition(i.first);
    }
  }
}

bool cmFindPackageCommand::FindModule(bool& found)
{
  std::string moduleFileName = cmStrCat("Find", this->Name, ".cmake");

  bool system = false;
  std::string debugBuffer = cmStrCat(
    "find_package considered the following paths for ", moduleFileName, ":\n");
  std::string mfile = this->Makefile->GetModulesFile(
    moduleFileName, system, this->DebugModeEnabled(), debugBuffer);
  if (this->DebugModeEnabled()) {
    if (mfile.empty()) {
      debugBuffer = cmStrCat(debugBuffer, "The file was not found.\n");
    } else {
      debugBuffer =
        cmStrCat(debugBuffer, "The file was found at\n  ", mfile, '\n');
    }
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }

  if (!mfile.empty()) {
    if (system) {
      auto const it = this->DeprecatedFindModules.find(this->Name);
      if (it != this->DeprecatedFindModules.end()) {
        cmPolicies::PolicyStatus status =
          this->Makefile->GetPolicyStatus(it->second);
        switch (status) {
          case cmPolicies::WARN: {
            this->Makefile->IssueMessage(
              MessageType::AUTHOR_WARNING,
              cmStrCat(cmPolicies::GetPolicyWarning(it->second), '\n'));
            CM_FALLTHROUGH;
          }
          case cmPolicies::OLD:
            break;
          case cmPolicies::NEW:
            return true;
        }
      }
    }

    // Load the module we found, and set "<name>_FIND_MODULE" to true
    // while inside it.
    found = true;
    std::string const var = cmStrCat(this->Name, "_FIND_MODULE");
    this->Makefile->AddDefinition(var, "1");
    bool result = this->ReadListFile(mfile, DoPolicyScope);
    this->Makefile->RemoveDefinition(var);

    std::string const foundVar = cmStrCat(this->Name, "_FOUND");
    if (this->Makefile->IsDefinitionSet(foundVar) &&
        !this->Makefile->IsOn(foundVar)) {

      if (this->DebugModeEnabled()) {
        this->DebugBuffer = cmStrCat(
          this->DebugBuffer, "The module is considered not found due to ",
          foundVar, " being FALSE.");
      }

      this->ConsideredPaths.emplace_back(mfile, FoundPackageMode::Module,
                                         SearchResult::NotFound);
      std::string const notFoundMessageVar =
        cmStrCat(this->Name, "_NOT_FOUND_MESSAGE");
      if (cmValue notFoundMessage =
            this->Makefile->GetDefinition(notFoundMessageVar)) {

        this->ConsideredPaths.back().Message = *notFoundMessage;
      }
    } else {
      if (this->DebugState) {
        this->DebugState->FoundAt(mfile);
      }
      this->FileFound = mfile;
      this->FileFoundMode = FoundPackageMode::Module;
      std::string const versionVar = cmStrCat(this->Name, "_VERSION");
      if (cmValue version = this->Makefile->GetDefinition(versionVar)) {
        this->VersionFound = *version;
      }
    }
    return result;
  }
  return true;
}

bool cmFindPackageCommand::HandlePackageMode(
  HandlePackageModeType const handlePackageModeType)
{
  this->ConsideredConfigs.clear();

  // Try to find the config file.
  cmValue def = this->Makefile->GetDefinition(this->Variable);

  // Try to load the config file if the directory is known
  bool fileFound = false;
  if (this->UseConfigFiles) {
    if (!def.IsOff()) {
      // Get the directory from the variable value.
      std::string dir = *def;
      cmSystemTools::ConvertToUnixSlashes(dir);

      // Treat relative paths with respect to the current source dir.
      if (!cmSystemTools::FileIsFullPath(dir)) {
        dir = "/" + dir;
        dir = this->Makefile->GetCurrentSourceDirectory() + dir;
      }
      // The file location was cached.  Look for the correct file.
      std::string file;
      FoundPackageMode foundMode = FoundPackageMode::None;
      if (this->FindConfigFile(dir, pdt::Any, file, foundMode)) {
        if (this->DebugState) {
          this->DebugState->FoundAt(file);
        }
        this->FileFound = std::move(file);
        this->FileFoundMode = foundMode;
        fileFound = true;
      }
      def = this->Makefile->GetDefinition(this->Variable);
    }

    // Search for the config file if it is not already found.
    if (def.IsOff() || !fileFound) {
      fileFound = this->FindConfig();
    }

    // Sanity check.
    if (fileFound && this->FileFound.empty()) {
      this->Makefile->IssueMessage(
        MessageType::INTERNAL_ERROR,
        "fileFound is true but FileFound is empty!");
      fileFound = false;
    }
  }

  std::string const foundVar = cmStrCat(this->Name, "_FOUND");
  std::string const notFoundMessageVar =
    cmStrCat(this->Name, "_NOT_FOUND_MESSAGE");
  std::string notFoundMessage;

  // If the directory for the config file was found, try to read the file.
  bool result = true;
  bool found = false;
  bool configFileSetFOUNDFalse = false;
  std::vector<std::string> missingTargets;

  if (fileFound) {
    if (this->Makefile->IsDefinitionSet(foundVar) &&
        !this->Makefile->IsOn(foundVar)) {
      // by removing Foo_FOUND here if it is FALSE, we don't really change
      // the situation for the Config file which is about to be included,
      // but we make it possible to detect later on whether the Config file
      // has set Foo_FOUND to FALSE itself:
      this->Makefile->RemoveDefinition(foundVar);
    }
    this->Makefile->RemoveDefinition(notFoundMessageVar);

    // Set the version variables before loading the config file.
    // It may override them.
    this->StoreVersionFound();

    // Parse the configuration file.
    if (this->CpsReader) {
      // The package has been found.
      found = true;
      result = this->ReadPackage();
    } else if (this->ReadListFile(this->FileFound, DoPolicyScope)) {
      // The package has been found.
      found = true;

      // Check whether the Config file has set Foo_FOUND to FALSE:
      if (this->Makefile->IsDefinitionSet(foundVar) &&
          !this->Makefile->IsOn(foundVar)) {
        // we get here if the Config file has set Foo_FOUND actively to FALSE
        found = false;
        configFileSetFOUNDFalse = true;
        notFoundMessage =
          this->Makefile->GetSafeDefinition(notFoundMessageVar);
      }

      // Check whether the required targets are defined.
      if (found && !this->RequiredTargets.empty()) {
        for (std::string const& t : this->RequiredTargets) {
          std::string qualifiedTarget = cmStrCat(this->Name, "::"_s, t);
          if (!this->Makefile->FindImportedTarget(qualifiedTarget)) {
            missingTargets.emplace_back(std::move(qualifiedTarget));
            found = false;
          }
        }
      }
    } else {
      // The configuration file is invalid.
      result = false;
    }
  }

  if (this->UseFindModules && !found &&
      handlePackageModeType == HandlePackageModeType::Config &&
      this->Makefile->IsOn("CMAKE_FIND_PACKAGE_PREFER_CONFIG")) {
    // Config mode failed. Allow Module case.
    result = false;
  }

  // package not found
  if (result && !found) {
    // warn if package required or
    // (neither quiet nor in config mode and not explicitly optional)
    if (this->IsRequired() ||
        (!(this->Quiet ||
           (this->UseConfigFiles && !this->UseFindModules &&
            this->ConsideredConfigs.empty())) &&
         this->Required != RequiredStatus::OptionalExplicit)) {
      // The variable is not set.
      std::ostringstream e;
      std::ostringstream aw;
      if (configFileSetFOUNDFalse) {
        e << "Found package configuration file:\n"
             "  "
          << this->FileFound
          << "\n"
             "but it set "
          << foundVar << " to FALSE so package \"" << this->Name
          << "\" is considered to be NOT FOUND.";
        if (!notFoundMessage.empty()) {
          e << " Reason given by package: \n" << notFoundMessage << "\n";
        }
      } else if (!missingTargets.empty()) {
        e << "Found package configuration file:\n"
             "  "
          << this->FileFound
          << "\n"
             "but the following required targets were not found:\n"
             "  "
          << cmJoin(cmMakeRange(missingTargets), ", "_s);
      } else if (!this->ConsideredConfigs.empty()) {
        // If there are files in ConsideredConfigs, it means that
        // FooConfig.cmake have been found, but they didn't have appropriate
        // versions.
        auto duplicate_end = cmRemoveDuplicates(this->ConsideredConfigs);
        e << "Could not find a configuration file for package \"" << this->Name
          << "\" that "
          << (this->VersionExact ? "exactly matches" : "is compatible with")
          << " requested version "
          << (this->VersionRange.empty() ? "" : "range ") << '"'
          << this->VersionComplete
          << "\".\n"
             "The following configuration files were considered but not "
             "accepted:\n";

        for (ConfigFileInfo const& info :
             cmMakeRange(this->ConsideredConfigs.cbegin(), duplicate_end)) {
          e << "  " << info.filename << ", version: " << info.version << '\n';
        }
      } else {
        std::string requestedVersionString;
        if (!this->VersionComplete.empty()) {
          requestedVersionString =
            cmStrCat(" (requested version ", this->VersionComplete, ')');
        }

        if (this->UseConfigFiles) {
          if (this->UseFindModules) {
            e << "By not providing \"Find" << this->Name
              << ".cmake\" in "
                 "CMAKE_MODULE_PATH this project has asked CMake to find a "
                 "package configuration file provided by \""
              << this->Name
              << "\", "
                 "but CMake did not find one.\n";
          }

          if (this->Configs.size() == 1) {
            e << "Could not find a package configuration file named \""
              << this->Configs[0].Name << "\" provided by package \""
              << this->Name << "\"" << requestedVersionString << ".\n";
          } else {
            auto configs = cmMakeRange(this->Configs);
            auto configNames =
              configs.transform([](ConfigName const& cn) { return cn.Name; });
            e << "Could not find a package configuration file provided by \""
              << this->Name << "\"" << requestedVersionString
              << " with any of the following names:\n"
              << cmWrap("  "_s, configNames, ""_s, "\n"_s) << '\n';
          }

          e << "Add the installation prefix of \"" << this->Name
            << "\" to CMAKE_PREFIX_PATH or set \"" << this->Variable
            << "\" to a directory containing one of the above files. "
               "If \""
            << this->Name
            << "\" provides a separate development "
               "package or SDK, be sure it has been installed.";
        } else // if(!this->UseFindModules && !this->UseConfigFiles)
        {
          e << "No \"Find" << this->Name
            << ".cmake\" found in "
               "CMAKE_MODULE_PATH.";

          aw
            << "Find" << this->Name
            << ".cmake must either be part of this "
               "project itself, in this case adjust CMAKE_MODULE_PATH so that "
               "it points to the correct location inside its source tree.\n"
               "Or it must be installed by a package which has already been "
               "found via find_package().  In this case make sure that "
               "package has indeed been found and adjust CMAKE_MODULE_PATH to "
               "contain the location where that package has installed "
               "Find"
            << this->Name
            << ".cmake.  This must be a location "
               "provided by that package.  This error in general means that "
               "the buildsystem of this project is relying on a Find-module "
               "without ensuring that it is actually available.\n";
        }
      }
      if (this->Required == RequiredStatus::RequiredFromFindVar) {
        e << "\nThis package is considered required because the "
             "CMAKE_FIND_REQUIRED variable has been enabled.\n";
      } else if (this->Required == RequiredStatus::RequiredFromPackageVar) {
        e << "\nThis package is considered required because the "
          << cmStrCat("CMAKE_REQUIRE_FIND_PACKAGE_", this->Name)
          << " variable has been enabled.\n";
      }

      this->Makefile->IssueMessage(
        this->IsRequired() ? MessageType::FATAL_ERROR : MessageType::WARNING,
        e.str());
      if (this->IsRequired()) {
        cmSystemTools::SetFatalErrorOccurred();
      }

      if (!aw.str().empty()) {
        this->Makefile->IssueMessage(MessageType::AUTHOR_WARNING, aw.str());
      }
    }
    // output result if in config mode but not in quiet mode
    else if (!this->Quiet) {
      this->Makefile->DisplayStatus(cmStrCat("Could NOT find ", this->Name,
                                             " (missing: ", this->Name,
                                             "_DIR)"),
                                    -1);
    }
  }

  // Set a variable marking whether the package was found.
  this->Makefile->AddDefinition(foundVar, found ? "1" : "0");

  // Set a variable naming the configuration file that was found.
  std::string const fileVar = cmStrCat(this->Name, "_CONFIG");
  if (found) {
    this->Makefile->AddDefinition(fileVar, this->FileFound);
  } else {
    this->Makefile->RemoveDefinition(fileVar);
  }

  std::string const consideredConfigsVar =
    cmStrCat(this->Name, "_CONSIDERED_CONFIGS");
  std::string const consideredVersionsVar =
    cmStrCat(this->Name, "_CONSIDERED_VERSIONS");

  std::string consideredConfigFiles;
  std::string consideredVersions;

  char const* sep = "";
  for (ConfigFileInfo const& i : this->ConsideredConfigs) {
    consideredConfigFiles += sep;
    consideredVersions += sep;
    consideredConfigFiles += i.filename;
    consideredVersions += i.version;
    sep = ";";
  }

  this->Makefile->AddDefinition(consideredConfigsVar, consideredConfigFiles);

  this->Makefile->AddDefinition(consideredVersionsVar, consideredVersions);

  return result;
}

bool cmFindPackageCommand::FindConfig()
{
  // Compute the set of search prefixes.
  this->ComputePrefixes();

  // Look for the project's configuration file.
  bool found = false;
  if (this->DebugModeEnabled()) {
    this->DebugBuffer = cmStrCat(this->DebugBuffer,
                                 "find_package considered the following "
                                 "locations for ",
                                 this->Name, "'s Config module:\n");
  }

  if (!found && this->UseCpsFiles) {
    found = this->FindEnvironmentConfig();
  }

  // Search for frameworks.
  if (!found && (this->SearchFrameworkFirst || this->SearchFrameworkOnly)) {
    found = this->FindFrameworkConfig();
  }

  // Search for apps.
  if (!found && (this->SearchAppBundleFirst || this->SearchAppBundleOnly)) {
    found = this->FindAppBundleConfig();
  }

  // Search prefixes.
  if (!found && !(this->SearchFrameworkOnly || this->SearchAppBundleOnly)) {
    found = this->FindPrefixedConfig();
  }

  // Search for frameworks.
  if (!found && this->SearchFrameworkLast) {
    found = this->FindFrameworkConfig();
  }

  // Search for apps.
  if (!found && this->SearchAppBundleLast) {
    found = this->FindAppBundleConfig();
  }

  if (this->DebugModeEnabled()) {
    if (found) {
      this->DebugBuffer = cmStrCat(
        this->DebugBuffer, "The file was found at\n  ", this->FileFound, '\n');
    } else {
      this->DebugBuffer =
        cmStrCat(this->DebugBuffer, "The file was not found.\n");
    }
  }

  // Store the entry in the cache so it can be set by the user.
  std::string init;
  if (found) {
    init = cmSystemTools::GetFilenamePath(this->FileFound);
  } else {
    init = this->Variable + "-NOTFOUND";
  }
  // We force the value since we do not get here if it was already set.
  this->SetConfigDirCacheVariable(init);

  return found;
}

void cmFindPackageCommand::SetConfigDirCacheVariable(std::string const& value)
{
  std::string const help =
    cmStrCat("The directory containing a CMake configuration file for ",
             this->Name, '.');
  this->Makefile->AddCacheDefinition(this->Variable, value, help,
                                     cmStateEnums::PATH, true);
  if (this->Makefile->GetPolicyStatus(cmPolicies::CMP0126) ==
        cmPolicies::NEW &&
      this->Makefile->IsNormalDefinitionSet(this->Variable)) {
    this->Makefile->AddDefinition(this->Variable, value);
  }
}

bool cmFindPackageCommand::FindPrefixedConfig()
{
  std::vector<std::string> const& prefixes = this->SearchPaths;
  return std::any_of(
    prefixes.begin(), prefixes.end(),
    [this](std::string const& p) -> bool { return this->SearchPrefix(p); });
}

bool cmFindPackageCommand::FindFrameworkConfig()
{
  std::vector<std::string> const& prefixes = this->SearchPaths;
  return std::any_of(prefixes.begin(), prefixes.end(),
                     [this](std::string const& p) -> bool {
                       return this->SearchFrameworkPrefix(p);
                     });
}

bool cmFindPackageCommand::FindAppBundleConfig()
{
  std::vector<std::string> const& prefixes = this->SearchPaths;
  return std::any_of(prefixes.begin(), prefixes.end(),
                     [this](std::string const& p) -> bool {
                       return this->SearchAppBundlePrefix(p);
                     });
}

bool cmFindPackageCommand::FindEnvironmentConfig()
{
  std::vector<std::string> const& prefixes =
    cmSystemTools::GetEnvPathNormalized("CPS_PATH");
  return std::any_of(prefixes.begin(), prefixes.end(),
                     [this](std::string const& p) -> bool {
                       return this->SearchEnvironmentPrefix(p);
                     });
}

cmFindPackageCommand::AppendixMap cmFindPackageCommand::FindAppendices(
  std::string const& base, cmPackageInfoReader const& baseReader) const
{
  AppendixMap appendices;

  // Find package appendices.
  cmsys::Glob glob;
  glob.RecurseOff();
  if (glob.FindFiles(cmStrCat(cmSystemTools::GetFilenamePath(base), "/"_s,
                              cmSystemTools::GetFilenameWithoutExtension(base),
                              "[-:]*.[Cc][Pp][Ss]"_s))) {
    // Check glob results for valid appendices.
    for (std::string const& extra : glob.GetFiles()) {
      // Exclude configuration-specific files for now; we look at them later
      // when we load their respective configuration-agnostic appendices.
      if (extra.find('@') != std::string::npos) {
        continue;
      }

      std::unique_ptr<cmPackageInfoReader> reader =
        cmPackageInfoReader::Read(extra, &baseReader);
      if (reader && reader->GetName() == this->Name) {
        std::vector<std::string> components = reader->GetComponentNames();
        Appendix appendix{ std::move(reader), std::move(components) };
        appendices.emplace(extra, std::move(appendix));
      }
    }
  }

  return appendices;
}

bool cmFindPackageCommand::ReadListFile(std::string const& f,
                                        PolicyScopeRule const psr)
{
  bool const noPolicyScope = !this->PolicyScope || psr == NoPolicyScope;

  using ITScope = cmMakefile::ImportedTargetScope;
  ITScope scope = this->GlobalScope ? ITScope::Global : ITScope::Local;
  cmMakefile::SetGlobalTargetImportScope globScope(this->Makefile, scope);

  auto oldUnwind = this->Makefile->GetStateSnapshot().GetUnwindType();

  // This allows child snapshots to inherit the CAN_UNWIND state from us, we'll
  // reset it immediately after the dependent file is done
  this->Makefile->GetStateSnapshot().SetUnwindType(cmStateEnums::CAN_UNWIND);
  bool result = this->Makefile->ReadDependentFile(f, noPolicyScope);

  this->Makefile->GetStateSnapshot().SetUnwindType(oldUnwind);
  this->Makefile->GetStateSnapshot().SetUnwindState(
    cmStateEnums::NOT_UNWINDING);

  if (!result) {
    std::string const e =
      cmStrCat("Error reading CMake code from \"", f, "\".");
    this->SetError(e);
  }

  return result;
}

bool cmFindPackageCommand::ReadPackage()
{
  // Resolve any transitive dependencies for the root file.
  if (!FindPackageDependencies(this->FileFound, *this->CpsReader,
                               this->Required)) {
    return false;
  }

  bool const hasComponentsRequested =
    !this->RequiredComponents.empty() || !this->OptionalComponents.empty();

  cmMakefile::CallRAII scope{ this->Makefile, this->FileFound, this->Status };

  // Loop over appendices.
  auto iter = this->CpsAppendices.begin();
  while (iter != this->CpsAppendices.end()) {
    RequiredStatus required = RequiredStatus::Optional;
    bool important = false;

    // Check if this appendix provides any requested components.
    if (hasComponentsRequested) {
      auto providesAny = [&iter](
                           std::set<std::string> const& desiredComponents) {
        return std::any_of(iter->second.Components.begin(),
                           iter->second.Components.end(),
                           [&desiredComponents](std::string const& component) {
                             return cm::contains(desiredComponents, component);
                           });
      };

      if (providesAny(this->RequiredComponents)) {
        important = true;
        required = this->Required;
      } else if (!providesAny(this->OptionalComponents)) {
        // This appendix doesn't provide any requested components; remove it
        // from the set to be imported.
        iter = this->CpsAppendices.erase(iter);
        continue;
      }
    }

    // Resolve any transitive dependencies for the appendix.
    if (!this->FindPackageDependencies(iter->first, iter->second, required)) {
      if (important) {
        // Some dependencies are missing, and we need(ed) this appendix; fail.
        return false;
      }

      // Some dependencies are missing, but we don't need this appendix; remove
      // it from the set to be imported.
      iter = this->CpsAppendices.erase(iter);
    } else {
      ++iter;
    }
  }

  // If we made it here, we want to actually import something, but we also
  // need to ensure we don't try to import the same file more than once (which
  // will fail due to the targets already existing). Retrieve the package state
  // so we can record what we're doing.
  cmPackageState& state =
    this->Makefile->GetStateSnapshot().GetPackageState(this->FileFound);

  // Import targets from root file.
  if (!this->ImportPackageTargets(state, this->FileFound, *this->CpsReader)) {
    return false;
  }

  // Import targets from appendices.
  // NOLINTNEXTLINE(readability-use-anyofallof)
  for (auto const& appendix : this->CpsAppendices) {
    cmMakefile::CallRAII appendixScope{ this->Makefile, appendix.first,
                                        this->Status };
    if (!this->ImportPackageTargets(state, appendix.first, appendix.second)) {
      return false;
    }
  }

  return true;
}

bool cmFindPackageCommand::FindPackageDependencies(
  std::string const& filePath, cmPackageInfoReader const& reader,
  RequiredStatus required)
{
  // Get package requirements.
  for (cmPackageRequirement const& dep : reader.GetRequirements()) {
    cmExecutionStatus status{ *this->Makefile };
    cmMakefile::CallRAII scope{ this->Makefile, filePath, status };

    // For each requirement, set up a nested instance to find it.
    cmFindPackageCommand fp{ status };
    fp.InheritOptions(this);

    fp.Name = dep.Name;
    fp.Required = required;
    fp.UseFindModules = false;
    fp.UseCpsFiles = true;

    fp.Version = dep.Version;
    fp.VersionComplete = dep.Version;
    fp.VersionCount =
      parseVersion(fp.Version, fp.VersionMajor, fp.VersionMinor,
                   fp.VersionPatch, fp.VersionTweak);

    fp.Components = cmJoin(cmMakeRange(dep.Components), ";"_s);
    fp.OptionalComponents =
      std::set<std::string>{ dep.Components.begin(), dep.Components.end() };
    fp.RequiredTargets = fp.OptionalComponents;

    // TODO set hints

    // Try to find the requirement; fail if we can't.
    if (!fp.FindPackage() || fp.FileFound.empty()) {
      this->SetError(cmStrCat("could not find "_s, dep.Name,
                              ", required by "_s, this->Name, '.'));
      return false;
    }
  }

  // All requirements (if any) were found.
  return true;
}

bool cmFindPackageCommand::ImportPackageTargets(cmPackageState& packageState,
                                                std::string const& filePath,
                                                cmPackageInfoReader& reader)
{
  // Check if we've already imported this file.
  std::string fileName = cmSystemTools::GetFilenameName(filePath);
  if (cm::contains(packageState.ImportedFiles, fileName)) {
    return true;
  }

  // Import base file.
  if (!reader.ImportTargets(this->Makefile, this->Status)) {
    return false;
  }

  // Find supplemental configuration files.
  cmsys::Glob glob;
  glob.RecurseOff();
  if (glob.FindFiles(
        cmStrCat(cmSystemTools::GetFilenamePath(filePath), '/',
                 cmSystemTools::GetFilenameWithoutExtension(filePath),
                 "@*.[Cc][Pp][Ss]"_s))) {

    // Try to read supplemental data from each file found.
    for (std::string const& extra : glob.GetFiles()) {
      std::unique_ptr<cmPackageInfoReader> configReader =
        cmPackageInfoReader::Read(extra, &reader);
      if (configReader && configReader->GetName() == this->Name) {
        if (!configReader->ImportTargetConfigurations(this->Makefile,
                                                      this->Status)) {
          return false;
        }
      }
    }
  }

  packageState.ImportedFiles.emplace(std::move(fileName));
  return true;
}

void cmFindPackageCommand::AppendToFoundProperty(bool const found)
{
  cmList foundContents;
  cmValue foundProp =
    this->Makefile->GetState()->GetGlobalProperty("PACKAGES_FOUND");
  if (!foundProp.IsEmpty()) {
    foundContents.assign(*foundProp);
    foundContents.remove_items({ this->Name });
  }

  cmList notFoundContents;
  cmValue notFoundProp =
    this->Makefile->GetState()->GetGlobalProperty("PACKAGES_NOT_FOUND");
  if (!notFoundProp.IsEmpty()) {
    notFoundContents.assign(*notFoundProp);
    notFoundContents.remove_items({ this->Name });
  }

  if (found) {
    foundContents.push_back(this->Name);
  } else {
    notFoundContents.push_back(this->Name);
  }

  this->Makefile->GetState()->SetGlobalProperty("PACKAGES_FOUND",
                                                foundContents.to_string());

  this->Makefile->GetState()->SetGlobalProperty("PACKAGES_NOT_FOUND",
                                                notFoundContents.to_string());
}

void cmFindPackageCommand::AppendSuccessInformation()
{
  {
    std::string const transitivePropName =
      cmStrCat("_CMAKE_", this->Name, "_TRANSITIVE_DEPENDENCY");
    this->Makefile->GetState()->SetGlobalProperty(transitivePropName, "False");
  }
  std::string const found = cmStrCat(this->Name, "_FOUND");
  std::string const upperFound = cmSystemTools::UpperCase(found);

  bool const upperResult = this->Makefile->IsOn(upperFound);
  bool const result = this->Makefile->IsOn(found);
  bool const packageFound = (result || upperResult);

  this->AppendToFoundProperty(packageFound);

  // Record whether the find was quiet or not, so this can be used
  // e.g. in FeatureSummary.cmake
  std::string const quietInfoPropName =
    cmStrCat("_CMAKE_", this->Name, "_QUIET");
  this->Makefile->GetState()->SetGlobalProperty(
    quietInfoPropName, this->Quiet ? "TRUE" : "FALSE");

  // set a global property to record the required version of this package
  std::string const versionInfoPropName =
    cmStrCat("_CMAKE_", this->Name, "_REQUIRED_VERSION");
  std::string versionInfo;
  if (!this->VersionRange.empty()) {
    versionInfo = this->VersionRange;
  } else if (!this->Version.empty()) {
    versionInfo =
      cmStrCat(this->VersionExact ? "==" : ">=", ' ', this->Version);
  }
  this->Makefile->GetState()->SetGlobalProperty(versionInfoPropName,
                                                versionInfo);
  if (this->IsRequired()) {
    std::string const requiredInfoPropName =
      cmStrCat("_CMAKE_", this->Name, "_TYPE");
    this->Makefile->GetState()->SetGlobalProperty(requiredInfoPropName,
                                                  "REQUIRED");
  }
}

void cmFindPackageCommand::PushFindPackageRootPathStack()
{
  // Allocate a PACKAGE_ROOT_PATH for the current find_package call.
  this->Makefile->FindPackageRootPathStack.emplace_back();
  std::vector<std::string>& rootPaths =
    this->Makefile->FindPackageRootPathStack.back();

  // Add root paths from <PackageName>_ROOT CMake and environment variables,
  // subject to CMP0074.
  std::string const rootVar = this->Name + "_ROOT";
  cmValue rootDef = this->Makefile->GetDefinition(rootVar);
  if (rootDef && rootDef.IsEmpty()) {
    rootDef = nullptr;
  }
  cm::optional<std::string> rootEnv = cmSystemTools::GetEnvVar(rootVar);
  if (rootEnv && rootEnv->empty()) {
    rootEnv = cm::nullopt;
  }
  switch (this->Makefile->GetPolicyStatus(cmPolicies::CMP0074)) {
    case cmPolicies::WARN:
      this->Makefile->MaybeWarnCMP0074(rootVar, rootDef, rootEnv);
      CM_FALLTHROUGH;
    case cmPolicies::OLD:
      // OLD behavior is to ignore the <PackageName>_ROOT variables.
      return;
    case cmPolicies::NEW: {
      // NEW behavior is to honor the <PackageName>_ROOT variables.
    } break;
  }

  // Add root paths from <PACKAGENAME>_ROOT CMake and environment variables,
  // if they are different than <PackageName>_ROOT, and subject to CMP0144.
  std::string const rootVAR = cmSystemTools::UpperCase(rootVar);
  cmValue rootDEF;
  cm::optional<std::string> rootENV;
  if (rootVAR != rootVar) {
    rootDEF = this->Makefile->GetDefinition(rootVAR);
    if (rootDEF && (rootDEF.IsEmpty() || rootDEF == rootDef)) {
      rootDEF = nullptr;
    }
    rootENV = cmSystemTools::GetEnvVar(rootVAR);
    if (rootENV && (rootENV->empty() || rootENV == rootEnv)) {
      rootENV = cm::nullopt;
    }
  }

  switch (this->Makefile->GetPolicyStatus(cmPolicies::CMP0144)) {
    case cmPolicies::WARN:
      this->Makefile->MaybeWarnCMP0144(rootVAR, rootDEF, rootENV);
      CM_FALLTHROUGH;
    case cmPolicies::OLD:
      // OLD behavior is to ignore the <PACKAGENAME>_ROOT variables.
      rootDEF = nullptr;
      rootENV = cm::nullopt;
      break;
    case cmPolicies::NEW: {
      // NEW behavior is to honor the <PACKAGENAME>_ROOT variables.
    } break;
  }

  if (rootDef) {
    cmExpandList(*rootDef, rootPaths);
  }
  if (rootDEF) {
    cmExpandList(*rootDEF, rootPaths);
  }
  if (rootEnv) {
    std::vector<std::string> p =
      cmSystemTools::SplitEnvPathNormalized(*rootEnv);
    std::move(p.begin(), p.end(), std::back_inserter(rootPaths));
  }
  if (rootENV) {
    std::vector<std::string> p =
      cmSystemTools::SplitEnvPathNormalized(*rootENV);
    std::move(p.begin(), p.end(), std::back_inserter(rootPaths));
  }
}

void cmFindPackageCommand::PopFindPackageRootPathStack()
{
  this->Makefile->FindPackageRootPathStack.pop_back();
}

void cmFindPackageCommand::ComputePrefixes()
{
  this->FillPrefixesPackageRedirect();

  if (!this->NoDefaultPath) {
    if (!this->NoPackageRootPath) {
      this->FillPrefixesPackageRoot();
    }
    if (!this->NoCMakePath) {
      this->FillPrefixesCMakeVariable();
    }
    if (!this->NoCMakeEnvironmentPath) {
      this->FillPrefixesCMakeEnvironment();
    }
  }

  this->FillPrefixesUserHints();

  if (!this->NoDefaultPath) {
    if (!this->NoSystemEnvironmentPath) {
      this->FillPrefixesSystemEnvironment();
    }
    if (!this->NoUserRegistry) {
      this->FillPrefixesUserRegistry();
    }
    if (!this->NoCMakeSystemPath) {
      this->FillPrefixesCMakeSystemVariable();
    }
    if (!this->NoSystemRegistry) {
      this->FillPrefixesSystemRegistry();
    }
  }
  this->FillPrefixesUserGuess();

  this->ComputeFinalPaths(IgnorePaths::No, &this->DebugBuffer);
}

void cmFindPackageCommand::FillPrefixesPackageRedirect()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::PackageRedirect];

  auto const redirectDir =
    this->Makefile->GetDefinition("CMAKE_FIND_PACKAGE_REDIRECTS_DIR");
  if (redirectDir && !redirectDir->empty()) {
    paths.AddPath(*redirectDir);
  }
  if (this->DebugModeEnabled()) {
    std::string debugBuffer =
      "The internally managed CMAKE_FIND_PACKAGE_REDIRECTS_DIR.\n";
    collectPathsForDebug(debugBuffer, paths);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

void cmFindPackageCommand::FillPrefixesPackageRoot()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::PackageRoot];

  // Add the PACKAGE_ROOT_PATH from each enclosing find_package call.
  for (auto pkgPaths = this->Makefile->FindPackageRootPathStack.rbegin();
       pkgPaths != this->Makefile->FindPackageRootPathStack.rend();
       ++pkgPaths) {
    for (std::string const& path : *pkgPaths) {
      paths.AddPath(path);
    }
  }
  if (this->DebugModeEnabled()) {
    std::string debugBuffer = "<PackageName>_ROOT CMake variable "
                              "[CMAKE_FIND_USE_PACKAGE_ROOT_PATH].\n";
    collectPathsForDebug(debugBuffer, paths);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

void cmFindPackageCommand::FillPrefixesCMakeEnvironment()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::CMakeEnvironment];
  std::string debugBuffer;
  std::size_t debugOffset = 0;

  // Check the environment variable with the same name as the cache
  // entry.
  paths.AddEnvPath(this->Variable);
  if (this->DebugModeEnabled()) {
    debugBuffer = cmStrCat("Env variable ", this->Variable,
                           " [CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH].\n");
    debugOffset = collectPathsForDebug(debugBuffer, paths);
  }

  // And now the general CMake environment variables
  paths.AddEnvPath("CMAKE_PREFIX_PATH");
  if (this->DebugModeEnabled()) {
    debugBuffer = cmStrCat(debugBuffer,
                           "CMAKE_PREFIX_PATH env variable "
                           "[CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH].\n");
    debugOffset = collectPathsForDebug(debugBuffer, paths, debugOffset);
  }

  paths.AddEnvPath("CMAKE_FRAMEWORK_PATH");
  paths.AddEnvPath("CMAKE_APPBUNDLE_PATH");
  if (this->DebugModeEnabled()) {
    debugBuffer =
      cmStrCat(debugBuffer,
               "CMAKE_FRAMEWORK_PATH and CMAKE_APPBUNDLE_PATH env "
               "variables [CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH].\n");
    collectPathsForDebug(debugBuffer, paths, debugOffset);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

void cmFindPackageCommand::FillPrefixesCMakeVariable()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::CMake];
  std::string debugBuffer;
  std::size_t debugOffset = 0;

  paths.AddCMakePath("CMAKE_PREFIX_PATH");
  if (this->DebugModeEnabled()) {
    debugBuffer = "CMAKE_PREFIX_PATH variable [CMAKE_FIND_USE_CMAKE_PATH].\n";
    debugOffset = collectPathsForDebug(debugBuffer, paths);
  }

  paths.AddCMakePath("CMAKE_FRAMEWORK_PATH");
  paths.AddCMakePath("CMAKE_APPBUNDLE_PATH");
  if (this->DebugModeEnabled()) {
    debugBuffer =
      cmStrCat(debugBuffer,
               "CMAKE_FRAMEWORK_PATH and CMAKE_APPBUNDLE_PATH variables "
               "[CMAKE_FIND_USE_CMAKE_PATH].\n");
    collectPathsForDebug(debugBuffer, paths, debugOffset);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

void cmFindPackageCommand::FillPrefixesSystemEnvironment()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::SystemEnvironment];

  // Use the system search path to generate prefixes.
  // Relative paths are interpreted with respect to the current
  // working directory.
  std::vector<std::string> envPATH =
    cmSystemTools::GetEnvPathNormalized("PATH");
  for (std::string const& i : envPATH) {
    // If the path is a PREFIX/bin case then add its parent instead.
    if ((cmHasLiteralSuffix(i, "/bin")) || (cmHasLiteralSuffix(i, "/sbin"))) {
      paths.AddPath(cmSystemTools::GetFilenamePath(i));
    } else {
      paths.AddPath(i);
    }
  }
  if (this->DebugModeEnabled()) {
    std::string debugBuffer = "Standard system environment variables "
                              "[CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH].\n";
    collectPathsForDebug(debugBuffer, paths);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

void cmFindPackageCommand::FillPrefixesUserRegistry()
{
#if defined(_WIN32) && !defined(__CYGWIN__)
  this->LoadPackageRegistryWinUser();
#elif defined(__HAIKU__)
  char dir[B_PATH_NAME_LENGTH];
  if (find_directory(B_USER_SETTINGS_DIRECTORY, -1, false, dir, sizeof(dir)) ==
      B_OK) {
    std::string fname = cmStrCat(dir, "/cmake/packages/", Name);
    this->LoadPackageRegistryDir(fname,
                                 this->LabeledPaths[PathLabel::UserRegistry]);
  }
#else
  std::string dir;
  if (cmSystemTools::GetEnv("HOME", dir)) {
    dir += "/.cmake/packages/";
    dir += this->Name;
    this->LoadPackageRegistryDir(dir,
                                 this->LabeledPaths[PathLabel::UserRegistry]);
  }
#endif
  if (this->DebugModeEnabled()) {
    std::string debugBuffer =
      "CMake User Package Registry [CMAKE_FIND_USE_PACKAGE_REGISTRY].\n";
    collectPathsForDebug(debugBuffer,
                         this->LabeledPaths[PathLabel::UserRegistry]);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

void cmFindPackageCommand::FillPrefixesSystemRegistry()
{
  if (this->NoSystemRegistry || this->NoDefaultPath) {
    return;
  }

#if defined(_WIN32) && !defined(__CYGWIN__)
  this->LoadPackageRegistryWinSystem();
#endif

  if (this->DebugModeEnabled()) {
    std::string debugBuffer =
      "CMake System Package Registry "
      "[CMAKE_FIND_PACKAGE_NO_SYSTEM_PACKAGE_REGISTRY].\n";
    collectPathsForDebug(debugBuffer,
                         this->LabeledPaths[PathLabel::SystemRegistry]);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

#if defined(_WIN32) && !defined(__CYGWIN__)
void cmFindPackageCommand::LoadPackageRegistryWinUser()
{
  // HKEY_CURRENT_USER\\Software shares 32-bit and 64-bit views.
  this->LoadPackageRegistryWin(true, 0,
                               this->LabeledPaths[PathLabel::UserRegistry]);
}

void cmFindPackageCommand::LoadPackageRegistryWinSystem()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::SystemRegistry];

  // HKEY_LOCAL_MACHINE\\SOFTWARE has separate 32-bit and 64-bit views.
  // Prefer the target platform view first.
  if (this->Makefile->PlatformIs64Bit()) {
    this->LoadPackageRegistryWin(false, KEY_WOW64_64KEY, paths);
    this->LoadPackageRegistryWin(false, KEY_WOW64_32KEY, paths);
  } else {
    this->LoadPackageRegistryWin(false, KEY_WOW64_32KEY, paths);
    this->LoadPackageRegistryWin(false, KEY_WOW64_64KEY, paths);
  }
}

void cmFindPackageCommand::LoadPackageRegistryWin(bool const user,
                                                  unsigned int const view,
                                                  cmSearchPath& outPaths)
{
  std::wstring key = L"Software\\Kitware\\CMake\\Packages\\";
  key += cmsys::Encoding::ToWide(this->Name);
  std::set<std::wstring> bad;
  HKEY hKey;
  if (RegOpenKeyExW(user ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE, key.c_str(),
                    0, KEY_QUERY_VALUE | view, &hKey) == ERROR_SUCCESS) {
    DWORD valueType = REG_NONE;
    wchar_t name[16383]; // RegEnumValue docs limit name to 32767 _bytes_
    std::vector<wchar_t> data(512);
    bool done = false;
    DWORD index = 0;
    while (!done) {
      DWORD nameSize = static_cast<DWORD>(sizeof(name));
      DWORD dataSize = static_cast<DWORD>(data.size() * sizeof(data[0]));
      switch (RegEnumValueW(hKey, index, name, &nameSize, 0, &valueType,
                            (BYTE*)&data[0], &dataSize)) {
        case ERROR_SUCCESS:
          ++index;
          if (valueType == REG_SZ) {
            data[dataSize] = 0;
            if (!this->CheckPackageRegistryEntry(
                  cmsys::Encoding::ToNarrow(&data[0]), outPaths)) {
              // The entry is invalid.
              bad.insert(name);
            }
          }
          break;
        case ERROR_MORE_DATA:
          data.resize((dataSize + sizeof(data[0]) - 1) / sizeof(data[0]));
          break;
        case ERROR_NO_MORE_ITEMS:
        default:
          done = true;
          break;
      }
    }
    RegCloseKey(hKey);
  }

  // Remove bad values if possible.
  if (user && !bad.empty() &&
      RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_SET_VALUE | view,
                    &hKey) == ERROR_SUCCESS) {
    for (std::wstring const& v : bad) {
      RegDeleteValueW(hKey, v.c_str());
    }
    RegCloseKey(hKey);
  }
}

#else
void cmFindPackageCommand::LoadPackageRegistryDir(std::string const& dir,
                                                  cmSearchPath& outPaths)
{
  cmsys::Directory files;
  if (!files.Load(dir)) {
    return;
  }

  std::string fname;
  for (unsigned long i = 0; i < files.GetNumberOfFiles(); ++i) {
    fname = cmStrCat(dir, '/', files.GetFile(i));

    if (!cmSystemTools::FileIsDirectory(fname)) {
      // Hold this file hostage until it behaves.
      cmFindPackageCommandHoldFile holdFile(fname.c_str());

      // Load the file.
      cmsys::ifstream fin(fname.c_str(), std::ios::in | std::ios::binary);
      std::string fentry;
      if (fin && cmSystemTools::GetLineFromStream(fin, fentry) &&
          this->CheckPackageRegistryEntry(fentry, outPaths)) {
        // The file references an existing package, so release it.
        holdFile.Release();
      }
    }
  }

  // TODO: Wipe out the directory if it is empty.
}
#endif

bool cmFindPackageCommand::CheckPackageRegistryEntry(std::string const& fname,
                                                     cmSearchPath& outPaths)
{
  // Parse the content of one package registry entry.
  if (cmSystemTools::FileIsFullPath(fname)) {
    // The first line in the stream is the full path to a file or
    // directory containing the package.
    if (cmSystemTools::FileExists(fname)) {
      // The path exists.  Look for the package here.
      if (!cmSystemTools::FileIsDirectory(fname)) {
        outPaths.AddPath(cmSystemTools::GetFilenamePath(fname));
      } else {
        outPaths.AddPath(fname);
      }
      return true;
    }
    // The path does not exist.  Assume the stream content is
    // associated with an old package that no longer exists, and
    // delete it to keep the package registry clean.
    return false;
  }
  // The first line in the stream is not the full path to a file or
  // directory.  Assume the stream content was created by a future
  // version of CMake that uses a different format, and leave it.
  return true;
}

void cmFindPackageCommand::FillPrefixesCMakeSystemVariable()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::CMakeSystem];

  bool const install_prefix_in_list =
    !this->Makefile->IsOn("CMAKE_FIND_NO_INSTALL_PREFIX");
  bool const remove_install_prefix = this->NoCMakeInstallPath;
  bool const add_install_prefix = !this->NoCMakeInstallPath &&
    this->Makefile->IsDefinitionSet("CMAKE_FIND_USE_INSTALL_PREFIX");

  // We have 3 possible states for `CMAKE_SYSTEM_PREFIX_PATH` and
  // `CMAKE_INSTALL_PREFIX`.
  // Either we need to remove `CMAKE_INSTALL_PREFIX`, add
  // `CMAKE_INSTALL_PREFIX`, or do nothing.
  //
  // When we need to remove `CMAKE_INSTALL_PREFIX` we remove the Nth occurrence
  // of `CMAKE_INSTALL_PREFIX` from `CMAKE_SYSTEM_PREFIX_PATH`, where `N` is
  // computed by `CMakeSystemSpecificInformation.cmake` while constructing
  // `CMAKE_SYSTEM_PREFIX_PATH`. This ensures that if projects / toolchains
  // have removed `CMAKE_INSTALL_PREFIX` from the list, we don't remove
  // some other entry by mistake
  long install_prefix_count = -1;
  std::string install_path_to_remove;
  if (cmValue to_skip = this->Makefile->GetDefinition(
        "_CMAKE_SYSTEM_PREFIX_PATH_INSTALL_PREFIX_COUNT")) {
    cmStrToLong(*to_skip, &install_prefix_count);
  }
  if (cmValue install_value = this->Makefile->GetDefinition(
        "_CMAKE_SYSTEM_PREFIX_PATH_INSTALL_PREFIX_VALUE")) {
    install_path_to_remove = *install_value;
  }

  if (remove_install_prefix && install_prefix_in_list &&
      install_prefix_count > 0 && !install_path_to_remove.empty()) {

    cmValue prefix_paths =
      this->Makefile->GetDefinition("CMAKE_SYSTEM_PREFIX_PATH");
    // remove entry from CMAKE_SYSTEM_PREFIX_PATH
    cmList expanded{ *prefix_paths };
    long count = 0;
    for (auto const& path : expanded) {
      bool const to_add =
        !(path == install_path_to_remove && ++count == install_prefix_count);
      if (to_add) {
        paths.AddPath(path);
      }
    }
  } else if (add_install_prefix && !install_prefix_in_list) {
    paths.AddCMakePath("CMAKE_INSTALL_PREFIX");
    paths.AddCMakePath("CMAKE_SYSTEM_PREFIX_PATH");
  } else {
    // Otherwise the current setup of `CMAKE_SYSTEM_PREFIX_PATH` is correct
    paths.AddCMakePath("CMAKE_SYSTEM_PREFIX_PATH");
  }

  paths.AddCMakePath("CMAKE_SYSTEM_FRAMEWORK_PATH");
  paths.AddCMakePath("CMAKE_SYSTEM_APPBUNDLE_PATH");

  if (this->DebugModeEnabled()) {
    std::string debugBuffer = "CMake variables defined in the Platform file "
                              "[CMAKE_FIND_USE_CMAKE_SYSTEM_PATH].\n";
    collectPathsForDebug(debugBuffer, paths);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

void cmFindPackageCommand::FillPrefixesUserGuess()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::Guess];

  for (std::string const& p : this->UserGuessArgs) {
    paths.AddUserPath(p);
  }
  if (this->DebugModeEnabled()) {
    std::string debugBuffer =
      "Paths specified by the find_package PATHS option.\n";
    collectPathsForDebug(debugBuffer, paths);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

void cmFindPackageCommand::FillPrefixesUserHints()
{
  cmSearchPath& paths = this->LabeledPaths[PathLabel::Hints];

  for (std::string const& p : this->UserHintsArgs) {
    paths.AddUserPath(p);
  }
  if (this->DebugModeEnabled()) {
    std::string debugBuffer =
      "Paths specified by the find_package HINTS option.\n";
    collectPathsForDebug(debugBuffer, paths);
    this->DebugBuffer = cmStrCat(this->DebugBuffer, debugBuffer);
  }
}

bool cmFindPackageCommand::SearchDirectory(std::string const& dir,
                                           PackageDescriptionType type)
{
  assert(!dir.empty() && dir.back() == '/');

  // Check each path suffix on this directory.
  for (std::string const& s : this->SearchPathSuffixes) {
    std::string d = dir;
    if (!s.empty()) {
      d += s;
      d += '/';
    }
    if (this->CheckDirectory(d, type)) {
      return true;
    }
  }
  return false;
}

bool cmFindPackageCommand::CheckDirectory(std::string const& dir,
                                          PackageDescriptionType type)
{
  assert(!dir.empty() && dir.back() == '/');

  std::string const d = dir.substr(0, dir.size() - 1);
  if (cm::contains(this->IgnoredPaths, d)) {
    this->ConsideredPaths.emplace_back(
      dir, cmFindPackageCommand::FoundMode(type), SearchResult::Ignored);
    return false;
  }

  // Look for the file in this directory.
  std::string file;
  FoundPackageMode foundMode = FoundPackageMode::None;
  if (this->FindConfigFile(d, type, file, foundMode)) {
    this->FileFound = std::move(file);
    this->FileFoundMode = foundMode;
    return true;
  }
  return false;
}

bool cmFindPackageCommand::FindConfigFile(std::string const& dir,
                                          PackageDescriptionType type,
                                          std::string& file,
                                          FoundPackageMode& foundMode)
{
  for (auto const& config : this->Configs) {
    if (type != pdt::Any && config.Type != type) {
      continue;
    }
    file = cmStrCat(dir, '/', config.Name);
    if (this->DebugModeEnabled()) {
      this->DebugBuffer = cmStrCat(this->DebugBuffer, "  ", file, '\n');
    }
    if (cmSystemTools::FileExists(file, true)) {
      if (this->CheckVersion(file)) {
        // Allow resolving symlinks when the config file is found through a
        // link
        if (this->UseRealPath) {
          file = cmSystemTools::GetRealPath(file);
        } else {
          file = cmSystemTools::ToNormalizedPathOnDisk(file);
        }
        foundMode = cmFindPackageCommand::FoundMode(config.Type);
        return true;
      }
      this->ConsideredPaths.emplace_back(file,
                                         cmFindPackageCommand::FoundMode(type),
                                         SearchResult::InsufficientVersion);
    } else {
      this->ConsideredPaths.emplace_back(
        file, cmFindPackageCommand::FoundMode(type), SearchResult::NoExist);
    }
  }
  return false;
}

bool cmFindPackageCommand::CheckVersion(std::string const& config_file)
{
  bool result = false; // by default, assume the version is not ok.
  bool haveResult = false;
  std::string version = "unknown";

  // Get the file extension.
  std::string::size_type pos = config_file.rfind('.');
  std::string ext = cmSystemTools::LowerCase(config_file.substr(pos));

  if (ext == ".cps"_s) {
    std::unique_ptr<cmPackageInfoReader> reader =
      cmPackageInfoReader::Read(config_file);
    if (reader && reader->GetName() == this->Name) {
      // Read version information.
      cm::optional<std::string> cpsVersion = reader->GetVersion();
      cm::optional<ParsedVersion> const& parsedVersion =
        reader->ParseVersion(cpsVersion);
      bool const hasVersion = cpsVersion.has_value();

      // Test for version compatibility.
      result = this->Version.empty();
      if (hasVersion) {
        version = std::move(*cpsVersion);

        if (!this->Version.empty()) {
          if (!parsedVersion) {
            // If we don't understand the version, compare the exact versions
            // using full string comparison. This is the correct behavior for
            // the "custom" schema, and the best we can do otherwise.
            result = (this->Version == version);
          } else if (this->VersionExact) {
            // If EXACT is specified, the version must be exactly the requested
            // version.
            result =
              cmSystemTools::VersionCompareEqual(this->Version, version);
          } else {
            // Do we have a compat_version?
            cm::optional<std::string> const& compatVersion =
              reader->GetCompatVersion();
            if (reader->ParseVersion(compatVersion)) {
              // If yes, the initial result is whether the requested version is
              // between the actual version and the compat version, inclusive.
              result = cmSystemTools::VersionCompareGreaterEq(version,
                                                              this->Version) &&
                cmSystemTools::VersionCompareGreaterEq(this->Version,
                                                       *compatVersion);

              if (result && !this->VersionMax.empty()) {
                // We must also check that the version is less than the version
                // limit.
                if (this->VersionRangeMax == VERSION_ENDPOINT_EXCLUDED) {
                  result = cmSystemTools::VersionCompareGreater(
                    this->VersionMax, version);
                } else {
                  result = cmSystemTools::VersionCompareGreaterEq(
                    this->VersionMax, version);
                }
              }
            } else {
              // If no, compat_version is assumed to be exactly the actual
              // version, so the result is whether the requested version is
              // exactly the actual version, and we can ignore the version
              // limit.
              result =
                cmSystemTools::VersionCompareEqual(this->Version, version);
            }
          }
        }
      }

      if (result) {
        // Locate appendices.
        cmFindPackageCommand::AppendixMap appendices =
          this->FindAppendices(config_file, *reader);

        // Collect available components.
        std::set<std::string> allComponents;

        std::vector<std::string> const& rootComponents =
          reader->GetComponentNames();
        allComponents.insert(rootComponents.begin(), rootComponents.end());

        for (auto const& appendix : appendices) {
          allComponents.insert(appendix.second.Components.begin(),
                               appendix.second.Components.end());
        }

        // Verify that all required components are available.
        std::set<std::string> requiredComponents = this->RequiredComponents;
        requiredComponents.insert(this->RequiredTargets.begin(),
                                  this->RequiredTargets.end());

        std::vector<std::string> missingComponents;
        std::set_difference(requiredComponents.begin(),
                            requiredComponents.end(), allComponents.begin(),
                            allComponents.end(),
                            std::back_inserter(missingComponents));
        if (!missingComponents.empty()) {
          result = false;
        }

        if (result && hasVersion) {
          this->VersionFound = version;

          if (parsedVersion) {
            std::vector<unsigned> const& versionParts =
              parsedVersion->ReleaseComponents;

            this->VersionFoundCount =
              static_cast<unsigned>(versionParts.size());
            switch (std::min(this->VersionFoundCount, 4u)) {
              case 4:
                this->VersionFoundTweak = versionParts[3];
                CM_FALLTHROUGH;
              case 3:
                this->VersionFoundPatch = versionParts[2];
                CM_FALLTHROUGH;
              case 2:
                this->VersionFoundMinor = versionParts[1];
                CM_FALLTHROUGH;
              case 1:
                this->VersionFoundMajor = versionParts[0];
                CM_FALLTHROUGH;
              default:
                break;
            }
          } else {
            this->VersionFoundCount = 0;
          }
        }
        this->CpsReader = std::move(reader);
        this->CpsAppendices = std::move(appendices);
        this->RequiredComponents = std::move(requiredComponents);
      }
    }
  } else {
    // Get the filename without the .cmake extension.
    std::string version_file_base = config_file.substr(0, pos);

    // Look for foo-config-version.cmake
    std::string version_file = cmStrCat(version_file_base, "-version.cmake");
    if (!haveResult && cmSystemTools::FileExists(version_file, true)) {
      result = this->CheckVersionFile(version_file, version);
      haveResult = true;
    }

    // Look for fooConfigVersion.cmake
    version_file = cmStrCat(version_file_base, "Version.cmake");
    if (!haveResult && cmSystemTools::FileExists(version_file, true)) {
      result = this->CheckVersionFile(version_file, version);
      haveResult = true;
    }

    // If no version was requested a versionless package is acceptable.
    if (!haveResult && this->Version.empty()) {
      result = true;
    }
  }

  ConfigFileInfo configFileInfo;
  configFileInfo.filename = config_file;
  configFileInfo.version = version;
  this->ConsideredConfigs.push_back(std::move(configFileInfo));

  return result;
}

bool cmFindPackageCommand::CheckVersionFile(std::string const& version_file,
                                            std::string& result_version)
{
  // The version file will be loaded in an isolated scope.
  cmMakefile::ScopePushPop const varScope(this->Makefile);
  cmMakefile::PolicyPushPop const polScope(this->Makefile);
  static_cast<void>(varScope);
  static_cast<void>(polScope);

  // Clear the output variables.
  this->Makefile->RemoveDefinition("PACKAGE_VERSION");
  this->Makefile->RemoveDefinition("PACKAGE_VERSION_UNSUITABLE");
  this->Makefile->RemoveDefinition("PACKAGE_VERSION_COMPATIBLE");
  this->Makefile->RemoveDefinition("PACKAGE_VERSION_EXACT");

  // Set the input variables.
  this->Makefile->AddDefinition("PACKAGE_FIND_NAME", this->Name);
  this->Makefile->AddDefinition("PACKAGE_FIND_VERSION_COMPLETE",
                                this->VersionComplete);

  auto addDefinition = [this](std::string const& variable,
                              cm::string_view value) {
    this->Makefile->AddDefinition(variable, value);
  };
  this->SetVersionVariables(addDefinition, "PACKAGE_FIND_VERSION",
                            this->Version, this->VersionCount,
                            this->VersionMajor, this->VersionMinor,
                            this->VersionPatch, this->VersionTweak);
  if (!this->VersionRange.empty()) {
    this->SetVersionVariables(addDefinition, "PACKAGE_FIND_VERSION_MIN",
                              this->Version, this->VersionCount,
                              this->VersionMajor, this->VersionMinor,
                              this->VersionPatch, this->VersionTweak);
    this->SetVersionVariables(addDefinition, "PACKAGE_FIND_VERSION_MAX",
                              this->VersionMax, this->VersionMaxCount,
                              this->VersionMaxMajor, this->VersionMaxMinor,
                              this->VersionMaxPatch, this->VersionMaxTweak);

    this->Makefile->AddDefinition("PACKAGE_FIND_VERSION_RANGE",
                                  this->VersionComplete);
    this->Makefile->AddDefinition("PACKAGE_FIND_VERSION_RANGE_MIN",
                                  this->VersionRangeMin);
    this->Makefile->AddDefinition("PACKAGE_FIND_VERSION_RANGE_MAX",
                                  this->VersionRangeMax);
  }

  // Load the version check file.
  // Pass NoPolicyScope because we do our own policy push/pop.
  bool suitable = false;
  if (this->ReadListFile(version_file, NoPolicyScope)) {
    // Check the output variables.
    bool okay = this->Makefile->IsOn("PACKAGE_VERSION_EXACT");
    bool const unsuitable = this->Makefile->IsOn("PACKAGE_VERSION_UNSUITABLE");
    if (!okay && !this->VersionExact) {
      okay = this->Makefile->IsOn("PACKAGE_VERSION_COMPATIBLE");
    }

    // The package is suitable if the version is okay and not
    // explicitly unsuitable.
    suitable = !unsuitable && (okay || this->Version.empty());
    if (suitable) {
      // Get the version found.
      this->VersionFound =
        this->Makefile->GetSafeDefinition("PACKAGE_VERSION");

      // Try to parse the version number and store the results that were
      // successfully parsed.
      unsigned int parsed_major;
      unsigned int parsed_minor;
      unsigned int parsed_patch;
      unsigned int parsed_tweak;
      this->VersionFoundCount =
        parseVersion(this->VersionFound, parsed_major, parsed_minor,
                     parsed_patch, parsed_tweak);
      switch (this->VersionFoundCount) {
        case 4:
          this->VersionFoundTweak = parsed_tweak;
          CM_FALLTHROUGH;
        case 3:
          this->VersionFoundPatch = parsed_patch;
          CM_FALLTHROUGH;
        case 2:
          this->VersionFoundMinor = parsed_minor;
          CM_FALLTHROUGH;
        case 1:
          this->VersionFoundMajor = parsed_major;
          CM_FALLTHROUGH;
        default:
          break;
      }
    }
  }

  result_version = this->Makefile->GetSafeDefinition("PACKAGE_VERSION");
  if (result_version.empty()) {
    result_version = "unknown";
  }

  // Succeed if the version is suitable.
  return suitable;
}

void cmFindPackageCommand::StoreVersionFound()
{
  // Store the whole version string.
  std::string const ver = cmStrCat(this->Name, "_VERSION");
  auto addDefinition = [this](std::string const& variable,
                              cm::string_view value) {
    this->Makefile->AddDefinition(variable, value);
  };

  this->SetVersionVariables(addDefinition, ver, this->VersionFound,
                            this->VersionFoundCount, this->VersionFoundMajor,
                            this->VersionFoundMinor, this->VersionFoundPatch,
                            this->VersionFoundTweak);

  if (this->VersionFound.empty()) {
    this->Makefile->RemoveDefinition(ver);
  }
}

bool cmFindPackageCommand::SearchPrefix(std::string const& prefix)
{
  assert(!prefix.empty() && prefix.back() == '/');

  // Skip this if the prefix does not exist.
  if (!cmSystemTools::FileIsDirectory(prefix)) {
    return false;
  }

  // Skip this if it's in ignored paths.
  std::string prefixWithoutSlash = prefix;
  if (prefixWithoutSlash != "/" && prefixWithoutSlash.back() == '/') {
    prefixWithoutSlash.erase(prefixWithoutSlash.length() - 1);
  }
  if (this->IgnoredPaths.count(prefixWithoutSlash) ||
      this->IgnoredPrefixPaths.count(prefixWithoutSlash)) {
    return false;
  }

  auto searchFn = [this](std::string const& fullPath,
                         PackageDescriptionType type) -> bool {
    return this->SearchDirectory(fullPath, type);
  };

  auto iCpsGen = cmCaseInsensitiveDirectoryListGenerator{ "cps"_s };
  auto iCMakeGen = cmCaseInsensitiveDirectoryListGenerator{ "cmake"_s };
  auto anyDirGen =
    cmAnyDirectoryListGenerator{ this->SortOrder, this->SortDirection };
  auto cpsPkgDirGen =
    cmProjectDirectoryListGenerator{ &this->Names, this->SortOrder,
                                     this->SortDirection, true };
  auto cmakePkgDirGen =
    cmProjectDirectoryListGenerator{ &this->Names, this->SortOrder,
                                     this->SortDirection, false };

  // PREFIX/(Foo|foo|FOO)/(cps|CPS)/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, cpsPkgDirGen, iCpsGen)) {
    return true;
  }

  // PREFIX/(Foo|foo|FOO)/*/(cps|CPS)/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, cpsPkgDirGen, iCpsGen,
                        anyDirGen)) {
    return true;
  }

  // PREFIX/(cps|CPS)/(Foo|foo|FOO)/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, iCpsGen, cpsPkgDirGen)) {
    return true;
  }

  // PREFIX/(cps|CPS)/(Foo|foo|FOO)/*/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, iCpsGen, cpsPkgDirGen,
                        anyDirGen)) {
    return true;
  }

  // PREFIX/(cps|CPS)/ (useful on windows or in build trees)
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, iCpsGen)) {
    return true;
  }

  // PREFIX/ (useful on windows or in build trees)
  if (this->SearchDirectory(prefix, pdt::CMake)) {
    return true;
  }

  // PREFIX/(cmake|CMake)/ (useful on windows or in build trees)
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, iCMakeGen)) {
    return true;
  }

  // PREFIX/(Foo|foo|FOO).*/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, cmakePkgDirGen)) {
    return true;
  }

  // PREFIX/(Foo|foo|FOO).*/(cmake|CMake)/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, cmakePkgDirGen,
                        iCMakeGen)) {
    return true;
  }

  auto secondPkgDirGen =
    cmProjectDirectoryListGenerator{ &this->Names, this->SortOrder,
                                     this->SortDirection, false };

  // PREFIX/(Foo|foo|FOO).*/(cmake|CMake)/(Foo|foo|FOO).*/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, cmakePkgDirGen,
                        iCMakeGen, secondPkgDirGen)) {
    return true;
  }

  // Construct list of common install locations (lib and share).
  std::vector<cm::string_view> common;
  std::string libArch;
  if (!this->LibraryArchitecture.empty()) {
    libArch = "lib/" + this->LibraryArchitecture;
    common.emplace_back(libArch);
  }
  if (this->UseLib32Paths) {
    common.emplace_back("lib32"_s);
  }
  if (this->UseLib64Paths) {
    common.emplace_back("lib64"_s);
  }
  if (this->UseLibx32Paths) {
    common.emplace_back("libx32"_s);
  }
  common.emplace_back("lib"_s);
  common.emplace_back("share"_s);

  auto commonGen = cmEnumPathSegmentsGenerator{ common };
  auto cmakeGen = cmAppendPathSegmentGenerator{ "cmake"_s };
  auto cpsGen = cmAppendPathSegmentGenerator{ "cps"_s };

  // PREFIX/(lib/ARCH|lib*|share)/cps/(Foo|foo|FOO)/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, commonGen, cpsGen,
                        cpsPkgDirGen)) {
    return true;
  }

  // PREFIX/(lib/ARCH|lib*|share)/cps/(Foo|foo|FOO)/*/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, commonGen, cpsGen,
                        cpsPkgDirGen, anyDirGen)) {
    return true;
  }

  // PREFIX/(lib/ARCH|lib*|share)/cps/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, commonGen, cpsGen)) {
    return true;
  }

  // PREFIX/(lib/ARCH|lib*|share)/cmake/(Foo|foo|FOO).*/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, commonGen, cmakeGen,
                        cmakePkgDirGen)) {
    return true;
  }

  // PREFIX/(lib/ARCH|lib*|share)/(Foo|foo|FOO).*/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, commonGen,
                        cmakePkgDirGen)) {
    return true;
  }

  // PREFIX/(lib/ARCH|lib*|share)/(Foo|foo|FOO).*/(cmake|CMake)/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, commonGen,
                        cmakePkgDirGen, iCMakeGen)) {
    return true;
  }

  // PREFIX/(Foo|foo|FOO).*/(lib/ARCH|lib*|share)/cmake/(Foo|foo|FOO).*/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, cmakePkgDirGen,
                        commonGen, cmakeGen, secondPkgDirGen)) {
    return true;
  }

  // PREFIX/(Foo|foo|FOO).*/(lib/ARCH|lib*|share)/(Foo|foo|FOO).*/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, cmakePkgDirGen,
                        commonGen, secondPkgDirGen)) {
    return true;
  }

  // PREFIX/(Foo|foo|FOO).*/(lib/ARCH|lib*|share)/(Foo|foo|FOO).*/(cmake|CMake)/
  return TryGeneratedPaths(searchFn, pdt::CMake, prefix, cmakePkgDirGen,
                           commonGen, secondPkgDirGen, iCMakeGen);
}

bool cmFindPackageCommand::SearchFrameworkPrefix(std::string const& prefix)
{
  assert(!prefix.empty() && prefix.back() == '/');

  auto searchFn = [this](std::string const& fullPath,
                         PackageDescriptionType type) -> bool {
    return this->SearchDirectory(fullPath, type);
  };

  auto iCMakeGen = cmCaseInsensitiveDirectoryListGenerator{ "cmake"_s };
  auto iCpsGen = cmCaseInsensitiveDirectoryListGenerator{ "cps"_s };
  auto fwGen =
    cmMacProjectDirectoryListGenerator{ &this->Names, ".framework"_s };
  auto rGen = cmAppendPathSegmentGenerator{ "Resources"_s };
  auto vGen = cmAppendPathSegmentGenerator{ "Versions"_s };
  auto anyGen =
    cmAnyDirectoryListGenerator{ this->SortOrder, this->SortDirection };

  // <prefix>/Foo.framework/Versions/*/Resources/CPS/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, fwGen, vGen, anyGen, rGen,
                        iCpsGen)) {
    return true;
  }

  // <prefix>/Foo.framework/Resources/CPS/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, fwGen, rGen, iCpsGen)) {
    return true;
  }

  // <prefix>/Foo.framework/Resources/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, fwGen, rGen)) {
    return true;
  }

  // <prefix>/Foo.framework/Resources/CMake/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, fwGen, rGen,
                        iCMakeGen)) {
    return true;
  }

  // <prefix>/Foo.framework/Versions/*/Resources/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, fwGen, vGen, anyGen,
                        rGen)) {
    return true;
  }

  // <prefix>/Foo.framework/Versions/*/Resources/CMake/
  return TryGeneratedPaths(searchFn, pdt::CMake, prefix, fwGen, vGen, anyGen,
                           rGen, iCMakeGen);
}

bool cmFindPackageCommand::SearchAppBundlePrefix(std::string const& prefix)
{
  assert(!prefix.empty() && prefix.back() == '/');

  auto searchFn = [this](std::string const& fullPath,
                         PackageDescriptionType type) -> bool {
    return this->SearchDirectory(fullPath, type);
  };

  auto appGen = cmMacProjectDirectoryListGenerator{ &this->Names, ".app"_s };
  auto crGen = cmAppendPathSegmentGenerator{ "Contents/Resources"_s };

  // <prefix>/Foo.app/Contents/Resources/CPS/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, appGen, crGen,
                        cmCaseInsensitiveDirectoryListGenerator{ "cps"_s })) {
    return true;
  }

  // <prefix>/Foo.app/Contents/Resources/
  if (TryGeneratedPaths(searchFn, pdt::CMake, prefix, appGen, crGen)) {
    return true;
  }

  // <prefix>/Foo.app/Contents/Resources/CMake/
  return TryGeneratedPaths(
    searchFn, pdt::CMake, prefix, appGen, crGen,
    cmCaseInsensitiveDirectoryListGenerator{ "cmake"_s });
}

bool cmFindPackageCommand::SearchEnvironmentPrefix(std::string const& prefix)
{
  assert(!prefix.empty() && prefix.back() == '/');

  // Skip this if the prefix does not exist.
  if (!cmSystemTools::FileIsDirectory(prefix)) {
    return false;
  }

  auto searchFn = [this](std::string const& fullPath,
                         PackageDescriptionType type) -> bool {
    return this->SearchDirectory(fullPath, type);
  };

  auto pkgDirGen =
    cmProjectDirectoryListGenerator{ &this->Names, this->SortOrder,
                                     this->SortDirection, true };

  // <environment-path>/(Foo|foo|FOO)/cps/
  if (TryGeneratedPaths(searchFn, pdt::Cps, prefix, pkgDirGen,
                        cmAppendPathSegmentGenerator{ "cps"_s })) {
    return true;
  }

  // <environment-path>/(Foo|foo|FOO)/
  return TryGeneratedPaths(searchFn, pdt::Cps, prefix, pkgDirGen);
}

bool cmFindPackageCommand::IsRequired() const
{
  return this->Required == RequiredStatus::RequiredExplicit ||
    this->Required == RequiredStatus::RequiredFromPackageVar ||
    this->Required == RequiredStatus::RequiredFromFindVar;
}

cmFindPackageCommand::FoundPackageMode cmFindPackageCommand::FoundMode(
  PackageDescriptionType type)
{
  switch (type) {
    case PackageDescriptionType::Any:
      return FoundPackageMode::None;
    case PackageDescriptionType::CMake:
      return FoundPackageMode::Config;
    case PackageDescriptionType::Cps:
      return FoundPackageMode::Cps;
  }
  return FoundPackageMode::None;
}

// TODO: Debug cmsys::Glob double slash problem.

bool cmFindPackage(std::vector<std::string> const& args,
                   cmExecutionStatus& status)
{
  return cmFindPackageCommand(status).InitialPass(args);
}

cmFindPackageDebugState::cmFindPackageDebugState(
  cmFindPackageCommand const* findPackage)
  : cmFindCommonDebugState("find_package", findPackage)
  , FindPackageCommand(findPackage)
{
}

cmFindPackageDebugState::~cmFindPackageDebugState() = default;

void cmFindPackageDebugState::FoundAtImpl(std::string const& path,
                                          std::string regexName)
{
  (void)path;
  (void)regexName;
}

void cmFindPackageDebugState::FailedAtImpl(std::string const& path,
                                           std::string regexName)
{
  (void)path;
  (void)regexName;
}

bool cmFindPackageDebugState::ShouldImplicitlyLogEvents() const
{
  auto const* fpc = this->FindPackageCommand;
  bool const canUsePackage = fpc->UseConfigFiles || fpc->UseCpsFiles;
  return canUsePackage &&
    fpc->FileFoundMode != cmFindPackageCommand::FoundPackageMode::Module &&
    std::any_of(fpc->ConsideredPaths.begin(), fpc->ConsideredPaths.end(),
                [](cmFindPackageCommand::ConsideredPath const& cp) {
                  return cp.Mode >
                    cmFindPackageCommand::FoundPackageMode::Module;
                });
}

void cmFindPackageDebugState::WriteDebug() const
{
}

#ifndef CMAKE_BOOTSTRAP
void cmFindPackageDebugState::WriteEvent(cmConfigureLog& log,
                                         cmMakefile const& mf) const
{
  (void)log;
  (void)mf;

  log.BeginEvent("find_package-v1", mf);

  auto const* fpc = this->FindPackageCommand;

  log.WriteValue("name"_s, fpc->Name);
  if (!fpc->Components.empty()) {
    log.BeginObject("components"_s);
    log.BeginArray();
    for (auto const& component : cmList{ fpc->Components }) {
      log.NextArrayElement();
      log.WriteValue("name"_s, component);
      log.WriteValue("required"_s,
                     fpc->RequiredComponents.find(component) !=
                       fpc->RequiredComponents.end());
      log.WriteValue("found"_s,
                     mf.IsOn(cmStrCat(fpc->Name, '_', component, "_FOUND")));
    }
    log.EndArray();
    log.EndObject();
  }
  if (!fpc->Configs.empty()) {
    auto pdt_name =
      [](cmFindPackageCommand::PackageDescriptionType type) -> std::string {
      switch (type) {
        case pdt::Any:
          return "any";
        case pdt::CMake:
          return "cmake";
        case pdt::Cps:
          return "cps";
      }
      assert(false);
      return "<UNKNOWN>";
    };

    log.BeginObject("configs"_s);
    log.BeginArray();
    for (auto const& config : fpc->Configs) {
      log.NextArrayElement();
      log.WriteValue("filename"_s, config.Name);
      log.WriteValue("kind"_s, pdt_name(config.Type));
    }
    log.EndArray();
    log.EndObject();
  }
  {
    log.BeginObject("version_request"_s);
    if (!fpc->Version.empty()) {
      log.WriteValue("version"_s, fpc->Version);
    }
    if (!fpc->VersionComplete.empty()) {
      log.WriteValue("version_complete"_s, fpc->VersionComplete);
    }
    if (!fpc->VersionRange.empty()) {
      log.WriteValue("min"_s, std::string(fpc->VersionRangeMin));
      log.WriteValue("max"_s, std::string(fpc->VersionRangeMax));
    }
    log.WriteValue("exact"_s, fpc->VersionExact);
    log.EndObject();
  }
  {
    auto required_str =
      [](cmFindPackageCommand::RequiredStatus status) -> std::string {
      switch (status) {
        case cmFindPackageCommand::RequiredStatus::Optional:
          return "optional";
        case cmFindPackageCommand::RequiredStatus::OptionalExplicit:
          return "optional_explicit";
        case cmFindPackageCommand::RequiredStatus::RequiredExplicit:
          return "required_explicit";
        case cmFindPackageCommand::RequiredStatus::RequiredFromPackageVar:
          return "required_from_package_variable";
        case cmFindPackageCommand::RequiredStatus::RequiredFromFindVar:
          return "required_from_find_variable";
      }
      assert(false);
      return "<UNKNOWN>";
    };
    log.BeginObject("settings"_s);
    log.WriteValue("required"_s, required_str(fpc->Required));
    log.WriteValue("quiet"_s, fpc->Quiet);
    log.WriteValue("global"_s, fpc->GlobalScope);
    log.WriteValue("policy_scope"_s, fpc->PolicyScope);
    log.WriteValue("bypass_provider"_s, fpc->BypassProvider);
    if (!fpc->UserHintsArgs.empty()) {
      log.WriteValue("hints"_s, fpc->UserHintsArgs);
    }
    if (!fpc->Names.empty()) {
      log.WriteValue("names"_s, fpc->Names);
    }
    if (!fpc->UserGuessArgs.empty()) {
      log.WriteValue("search_paths"_s, fpc->UserGuessArgs);
    }
    if (!fpc->SearchPathSuffixes.empty()) {
      log.WriteValue("path_suffixes"_s, fpc->SearchPathSuffixes);
    }
    if (fpc->RegistryViewDefined) {
      log.WriteValue(
        "registry_view"_s,
        std::string(cmWindowsRegistry::FromView(fpc->RegistryView)));
    }
    {
      auto find_root_path_mode =
        [](cmFindCommon::RootPathMode mode) -> std::string {
        switch (mode) {
          case cmFindCommon::RootPathModeNever:
            return "NEVER";
          case cmFindCommon::RootPathModeOnly:
            return "ONLY";
          case cmFindCommon::RootPathModeBoth:
            return "BOTH";
        }
        assert(false);
        return "<UNKNOWN>";
      };
      log.BeginObject("paths"_s);
      log.WriteValue("CMAKE_FIND_USE_CMAKE_PATH"_s, !fpc->NoDefaultPath);
      log.WriteValue("CMAKE_FIND_USE_CMAKE_ENVIRONMENT_PATH"_s,
                     !fpc->NoCMakeEnvironmentPath);
      log.WriteValue("CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH"_s,
                     !fpc->NoSystemEnvironmentPath);
      log.WriteValue("CMAKE_FIND_USE_CMAKE_SYSTEM_PATH"_s,
                     !fpc->NoCMakeSystemPath);
      log.WriteValue("CMAKE_FIND_USE_INSTALL_PREFIX"_s,
                     !fpc->NoCMakeInstallPath);
      log.WriteValue("CMAKE_FIND_USE_PACKAGE_ROOT_PATH"_s,
                     !fpc->NoPackageRootPath);
      log.WriteValue("CMAKE_FIND_USE_CMAKE_PACKAGE_REGISTRY"_s,
                     !fpc->NoUserRegistry);
      log.WriteValue("CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY"_s,
                     !fpc->NoSystemRegistry);
      log.WriteValue("CMAKE_FIND_ROOT_PATH_MODE"_s,
                     find_root_path_mode(fpc->FindRootPathMode));
      log.EndObject();
    }
    log.EndObject();
  }

  auto found_mode =
    [](cmFindPackageCommand::FoundPackageMode status) -> std::string {
    switch (status) {
      case cmFindPackageCommand::FoundPackageMode::None:
        return "none?";
      case cmFindPackageCommand::FoundPackageMode::Module:
        return "module";
      case cmFindPackageCommand::FoundPackageMode::Config:
        return "config";
      case cmFindPackageCommand::FoundPackageMode::Cps:
        return "cps";
      case cmFindPackageCommand::FoundPackageMode::Provider:
        return "provider";
    }
    assert(false);
    return "<UNKNOWN>";
  };
  if (!fpc->ConsideredPaths.empty()) {
    auto search_result =
      [](cmFindPackageCommand::SearchResult type) -> std::string {
      switch (type) {
        case cmFindPackageCommand::SearchResult::InsufficientVersion:
          return "insufficient_version";
        case cmFindPackageCommand::SearchResult::NoExist:
          return "no_exist";
        case cmFindPackageCommand::SearchResult::Ignored:
          return "ignored";
        case cmFindPackageCommand::SearchResult::NoConfigFile:
          return "no_config_file";
        case cmFindPackageCommand::SearchResult::NotFound:
          return "not_found";
      }
      assert(false);
      return "<UNKNOWN>";
    };

    log.BeginObject("candidates"_s);
    log.BeginArray();
    for (auto const& considered : fpc->ConsideredPaths) {
      log.NextArrayElement();
      log.WriteValue("path"_s, considered.Path);
      log.WriteValue("mode"_s, found_mode(considered.Mode));
      log.WriteValue("reason"_s, search_result(considered.Reason));
      if (!considered.Message.empty()) {
        log.WriteValue("message"_s, considered.Message);
      }
    }
    log.EndArray();
    log.EndObject();
  }
  // TODO: Add provider information (see #26925)
  if (!fpc->FileFound.empty()) {
    log.BeginObject("found"_s);
    log.WriteValue("path"_s, fpc->FileFound);
    log.WriteValue("mode"_s, found_mode(fpc->FileFoundMode));
    log.WriteValue("version"_s, fpc->VersionFound);
    log.EndObject();
  } else {
    log.WriteValue("found"_s, nullptr);
  }

  this->WriteSearchVariables(log, mf);

  log.EndEvent();
}

std::vector<std::pair<cmFindCommonDebugState::VariableSource, std::string>>
cmFindPackageDebugState::ExtraSearchVariables() const
{
  std::vector<std::pair<cmFindCommonDebugState::VariableSource, std::string>>
    extraSearches;
  if (this->FindPackageCommand->UseFindModules) {
    extraSearches.emplace_back(VariableSource::PathList, "CMAKE_MODULE_PATH");
  }
  return extraSearches;
}
#endif
