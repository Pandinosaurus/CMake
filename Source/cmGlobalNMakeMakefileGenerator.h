/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "cm_codecvt_Encoding.hxx"

#include "cmGlobalGeneratorFactory.h"
#include "cmGlobalUnixMakefileGenerator3.h"
#include "cmValue.h"

class cmMakefile;
class cmake;

/** \class cmGlobalNMakeMakefileGenerator
 * \brief Write a NMake makefiles.
 *
 * cmGlobalNMakeMakefileGenerator manages nmake build process for a tree
 */
class cmGlobalNMakeMakefileGenerator : public cmGlobalUnixMakefileGenerator3
{
public:
  cmGlobalNMakeMakefileGenerator(cmake* cm);
  static std::unique_ptr<cmGlobalGeneratorFactory> NewFactory()
  {
    return std::unique_ptr<cmGlobalGeneratorFactory>(
      new cmGlobalGeneratorSimpleFactory<cmGlobalNMakeMakefileGenerator>());
  }
  //! Get the name for the generator.
  std::string GetName() const override
  {
    return cmGlobalNMakeMakefileGenerator::GetActualName();
  }
  static std::string GetActualName() { return "NMake Makefiles"; }

  /** Get encoding used by generator for makefile files */
  codecvt_Encoding GetMakefileEncoding() const override
  {
    return this->NMakeSupportsUTF8 ? codecvt_Encoding::UTF8_WITH_BOM
                                   : codecvt_Encoding::ANSI;
  }

  /** Get the documentation entry for this generator.  */
  static cmDocumentationEntry GetDocumentation();

  /**
   * Try to determine system information such as shared library
   * extension, pthreads, byte order etc.
   */
  void EnableLanguage(std::vector<std::string> const& languages, cmMakefile*,
                      bool optional) override;

  bool IsGNUMakeJobServerAware() const override { return false; }

protected:
  std::vector<GeneratedMakeCommand> GenerateBuildCommand(
    std::string const& makeProgram, std::string const& projectName,
    std::string const& projectDir, std::vector<std::string> const& targetNames,
    std::string const& config, int jobs, bool verbose,
    cmBuildOptions buildOptions = cmBuildOptions(),
    std::vector<std::string> const& makeOptions =
      std::vector<std::string>()) override;

  void PrintBuildCommandAdvice(std::ostream& os, int jobs) const override;

private:
  bool NMakeSupportsUTF8 = false;
  std::string NMakeVersion;
  bool FindMakeProgram(cmMakefile* mf) override;
  void CheckNMakeFeatures();

  void PrintCompilerAdvice(std::ostream& os, std::string const& lang,
                           cmValue envVar) const override;
};
