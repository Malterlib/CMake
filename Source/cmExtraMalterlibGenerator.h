/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#ifndef cmExtraMalterlibGenerator_h
#define cmExtraMalterlibGenerator_h

#include <cmConfigure.h>

#include "cmExternalMakefileProjectGenerator.h"
#include "cmMalterlibRegistry.h"

#include <cmsys/String.hxx>

#include <map>
#include <string>
#include <vector>

class cmGeneratedFileStream;
class cmGeneratorTarget;
class cmLocalGenerator;
class cmMakefile;
class cmSourceFile;

/** \class cmExtraMalterlibGenerator
 * \brief Write Malterlib build system files for Makefile based projects
 */
class cmExtraMalterlibGenerator : public cmExternalMakefileProjectGenerator
{
public:
  static cmExternalMakefileProjectGeneratorFactory* GetFactory();
  cmExtraMalterlibGenerator();

  void Generate() override;

private:
  void CreateProjectFile(const std::vector<cmLocalGenerator*>& lgs);

  void CreateNewProjectFile(const std::vector<cmLocalGenerator*>& lgs,
                            const std::string& filename);

  /** Appends all targets as build systems to the project file and get all
   * include directories and compiler definitions used.
   */
  void AppendAllTargets(const std::vector<cmLocalGenerator*>& lgs,
                        const cmMakefile* mf, cmMalterlibRegistry& registry
                        );
  /** Appends the specified target to the generated project file as a Sublime
   *  Text build system.
   */
  void AppendTarget(cmMalterlibRegistry& registry,
                    cmLocalGenerator* lg, cmGeneratorTarget* target,
                    const char* make, const cmMakefile* makefile,
                    const char* compiler,
                    bool firstTarget);
  
  void GetTargetFiles(
    std::vector<cmSourceFile*> &sourceFiles,
    cmLocalGenerator* lg, const cmGeneratorTarget* target,
    const cmMakefile* makefile);

  void AddFilesToRegistry(
    cmMalterlibRegistry& registry, 
    std::vector<cmSourceFile*> const &sourceFiles,
    std::string const &configName,
    cmLocalGenerator* lg,
    const cmGeneratorTarget* target
  );

  cmMalterlibRegistry& AddFileInGroup(
    cmMalterlibRegistry& registry, 
    std::string const &fileName
  );
  
  std::vector<std::string> HidePrefixes;
  std::map<std::string, std::string> ReplacePrefixes;
};

#endif
