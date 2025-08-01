/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <cm/optional>
#include <cm/string_view>

#include "cmGlobalVisualStudio8Generator.h"

class cmGeneratorTarget;
class cmLocalGenerator;
class cmMakefile;
class cmSourceFile;
class cmake;
struct cmIDEFlagTable;

/** \class cmGlobalVisualStudio10Generator
 * \brief Write a Unix makefiles.
 *
 * cmGlobalVisualStudio10Generator manages UNIX build process for a tree
 */
class cmGlobalVisualStudio10Generator : public cmGlobalVisualStudio8Generator
{
public:
  bool IsVisualStudioAtLeast10() const override { return true; }

  bool SetSystemName(std::string const& s, cmMakefile* mf) override;
  bool SetGeneratorToolset(std::string const& ts, bool build,
                           cmMakefile* mf) override;

  std::vector<GeneratedMakeCommand> GenerateBuildCommand(
    std::string const& makeProgram, std::string const& projectName,
    std::string const& projectDir, std::vector<std::string> const& targetNames,
    std::string const& config, int jobs, bool verbose,
    cmBuildOptions buildOptions = cmBuildOptions(),
    std::vector<std::string> const& makeOptions =
      std::vector<std::string>()) override;

  //! create the correct local generator
  std::unique_ptr<cmLocalGenerator> CreateLocalGenerator(
    cmMakefile* mf) override;

  /**
   * Try to determine system information such as shared library
   * extension, pthreads, byte order etc.
   */
  void EnableLanguage(std::vector<std::string> const& languages, cmMakefile*,
                      bool optional) override;

  void AddAndroidExecutableWarning(std::string const& name)
  {
    this->AndroidExecutableWarnings.insert(name);
  }

  bool IsCudaEnabled() const { return this->CudaEnabled; }

  /** Generating for Nsight Tegra VS plugin?  */
  bool IsNsightTegra() const;
  std::string GetNsightTegraVersion() const;

  /** The vctargets path for the target platform.  */
  char const* GetCustomVCTargetsPath() const;

  /** The toolset name for the target platform.  */
  char const* GetPlatformToolset() const;
  std::string const& GetPlatformToolsetString() const;

  /** The toolset version props file, if any.  */
  std::string const& GetPlatformToolsetVersionProps() const;

  /** The toolset host architecture name (e.g. x64 for 64-bit host tools).  */
  char const* GetPlatformToolsetHostArchitecture() const;
  std::string const& GetPlatformToolsetHostArchitectureString() const;

  /** The cuda toolset version.  */
  char const* GetPlatformToolsetCuda() const;
  std::string const& GetPlatformToolsetCudaString() const;

  /** The custom cuda install directory */
  char const* GetPlatformToolsetCudaCustomDir() const;
  std::string const& GetPlatformToolsetCudaCustomDirString() const;

  /** The nvcc subdirectory of a custom cuda install directory */
  std::string const& GetPlatformToolsetCudaNvccSubdirString() const;

  /** The visual studio integration subdirectory of a custom cuda install
   * directory */
  std::string const& GetPlatformToolsetCudaVSIntegrationSubdirString() const;

  /** The fortran toolset name.  */
  cm::optional<std::string> GetPlatformToolsetFortran() const override
  {
    return this->GeneratorToolsetFortran;
  }

  /** Return whether we need to use No/Debug instead of false/true
      for GenerateDebugInformation.  */
  bool GetPlatformToolsetNeedsDebugEnum() const
  {
    return this->PlatformToolsetNeedsDebugEnum;
  }

  /** Return the CMAKE_SYSTEM_NAME.  */
  std::string const& GetSystemName() const { return this->SystemName; }

  /** Return the CMAKE_SYSTEM_VERSION.  */
  std::string const& GetSystemVersion() const { return this->SystemVersion; }

  /** Return the Windows version targeted on VS 2015 and above.  */
  std::string const& GetWindowsTargetPlatformVersion() const
  {
    return this->WindowsTargetPlatformVersion;
  }

  /** Return true if building for WindowsCE */
  bool TargetsWindowsCE() const override { return this->SystemIsWindowsCE; }

  /** Return true if building for WindowsPhone */
  bool TargetsWindowsPhone() const { return this->SystemIsWindowsPhone; }

  /** Return true if building for WindowsStore */
  bool TargetsWindowsStore() const { return this->SystemIsWindowsStore; }

  /** Return true if building for WindowsKernelModeDriver */
  bool TargetsWindowsKernelModeDriver() const
  {
    return this->SystemIsWindowsKernelModeDriver;
  }

  /** Return true if building for Android */
  bool TargetsAndroid() const { return this->SystemIsAndroid; }

  char const* GetCMakeCFGIntDir() const override { return "$(Configuration)"; }

  /** Generate an <output>.rule file path for a given command output.  */
  std::string GenerateRuleFile(std::string const& output) const override;

  void PathTooLong(cmGeneratorTarget* target, cmSourceFile const* sf,
                   std::string const& sfRel);

  std::string Encoding() override;
  char const* GetToolsVersion() const;

  virtual cm::optional<std::string> GetVSInstanceVersion() const { return {}; }

  bool GetSupportsUnityBuilds() const { return this->SupportsUnityBuilds; }

  virtual cm::optional<std::string> FindMSBuildCommandEarly(cmMakefile* mf);

  bool FindMakeProgram(cmMakefile* mf) override;

  bool IsIPOSupported() const override { return true; }

  virtual bool IsStdOutEncodingSupported() const { return false; }

  virtual bool IsUtf8EncodingSupported() const { return false; }

  virtual bool IsScanDependenciesSupported() const { return false; }

  static std::string GetInstalledNsightTegraVersion();

  /** Return the first two components of CMAKE_SYSTEM_VERSION.  */
  std::string GetApplicationTypeRevision() const;

  virtual char const* GetAndroidApplicationTypeRevision() const { return ""; }

  cmIDEFlagTable const* GetClFlagTable() const;
  cmIDEFlagTable const* GetCSharpFlagTable() const;
  cmIDEFlagTable const* GetRcFlagTable() const;
  cmIDEFlagTable const* GetLibFlagTable() const;
  cmIDEFlagTable const* GetLinkFlagTable() const;
  cmIDEFlagTable const* GetCudaFlagTable() const;
  cmIDEFlagTable const* GetCudaHostFlagTable() const;
  cmIDEFlagTable const* GetMarmasmFlagTable() const;
  cmIDEFlagTable const* GetMasmFlagTable() const;
  cmIDEFlagTable const* GetNasmFlagTable() const;

  bool IsMsBuildRestoreSupported() const;
  bool IsBuildInParallelSupported() const;

  bool SupportsShortObjectNames() const override;

protected:
  cmGlobalVisualStudio10Generator(cmake* cm, std::string const& name);

  void Generate() override;
  virtual bool InitializeSystem(cmMakefile* mf);
  virtual bool InitializeWindows(cmMakefile* mf);
  virtual bool InitializeWindowsCE(cmMakefile* mf);
  virtual bool InitializeWindowsPhone(cmMakefile* mf);
  virtual bool InitializeWindowsStore(cmMakefile* mf);
  virtual bool InitializeWindowsKernelModeDriver(cmMakefile* mf);
  virtual bool InitializeTegraAndroid(cmMakefile* mf);
  virtual bool InitializeAndroid(cmMakefile* mf);

  bool InitializePlatform(cmMakefile* mf) override;
  virtual bool InitializePlatformWindows(cmMakefile* mf);
  virtual bool VerifyNoGeneratorPlatformVersion(cmMakefile* mf) const;

  virtual bool ProcessGeneratorToolsetField(std::string const& key,
                                            std::string const& value);

  virtual std::string SelectWindowsCEToolset() const;
  virtual bool SelectWindowsPhoneToolset(std::string& toolset) const;
  virtual bool SelectWindowsStoreToolset(std::string& toolset) const;

  enum class AuxToolset
  {
    None,
    Default,
    PropsExist,
    PropsMissing,
    PropsIndeterminate
  };
  virtual AuxToolset FindAuxToolset(std::string& version,
                                    std::string& props) const;

  std::string const& GetMSBuildCommand();

  cmIDEFlagTable const* LoadFlagTable(std::string const& toolSpecificName,
                                      std::string const& defaultName,
                                      std::string const& table) const;

  std::string GeneratorToolset;
  std::string GeneratorToolsetVersionProps;
  std::string GeneratorToolsetHostArchitecture;
  std::string GeneratorToolsetCustomVCTargetsDir;
  std::string GeneratorToolsetCuda;
  std::string GeneratorToolsetCudaCustomDir;
  std::string GeneratorToolsetCudaNvccSubdir;
  std::string GeneratorToolsetCudaVSIntegrationSubdir;
  cm::optional<std::string> GeneratorToolsetFortran;
  std::string DefaultPlatformToolset;
  std::string DefaultPlatformToolsetHostArchitecture;
  std::string DefaultAndroidToolset;
  std::string WindowsTargetPlatformVersion;
  std::string SystemName;
  std::string SystemVersion;
  std::string NsightTegraVersion;
  std::string DefaultCLFlagTableName;
  std::string DefaultCSharpFlagTableName;
  std::string DefaultLibFlagTableName;
  std::string DefaultLinkFlagTableName;
  std::string DefaultCudaFlagTableName;
  std::string DefaultCudaHostFlagTableName;
  std::string DefaultMarmasmFlagTableName;
  std::string DefaultMasmFlagTableName;
  std::string DefaultNasmFlagTableName;
  std::string DefaultRCFlagTableName;
  bool SupportsUnityBuilds = false;
  bool SystemIsWindowsCE = false;
  bool SystemIsWindowsPhone = false;
  bool SystemIsWindowsStore = false;
  bool SystemIsWindowsKernelModeDriver = false;
  bool SystemIsAndroid = false;
  bool MSBuildCommandInitialized = false;

private:
  struct LongestSourcePath
  {
    LongestSourcePath() = default;
    size_t Length = 0;
    cmGeneratorTarget* Target = nullptr;
    cmSourceFile const* SourceFile = nullptr;
    std::string SourceRel;
  };
  LongestSourcePath LongestSource;

  std::string MSBuildCommand;
  std::set<std::string> AndroidExecutableWarnings;
  virtual std::string FindMSBuildCommand();
  std::string FindDevEnvCommand() override;
  std::string GetVSMakeProgram() override { return this->GetMSBuildCommand(); }

  std::string GeneratorToolsetVersion;

  bool PlatformToolsetNeedsDebugEnum = false;

  bool ParseGeneratorToolset(std::string const& ts, cmMakefile* mf);

  std::string GetClFlagTableName() const;
  std::string GetCSharpFlagTableName() const;
  std::string GetRcFlagTableName() const;
  std::string GetLibFlagTableName() const;
  std::string GetLinkFlagTableName() const;
  std::string GetMasmFlagTableName() const;
  std::string CanonicalToolsetName(std::string const& toolset) const;

  cm::optional<std::string> FindFlagTable(cm::string_view toolsetName,
                                          cm::string_view table) const;

  std::string CustomFlagTableDir;

  std::string CustomVCTargetsPath;
  std::string VCTargetsPath;
  bool FindVCTargetsPath(cmMakefile* mf);

  bool CudaEnabled = false;

  // We do not use the reload macros for VS >= 10.
  std::string GetUserMacrosDirectory() override { return ""; }
};
