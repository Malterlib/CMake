/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#ifndef cmExtraMalterlibGenerator_h
#define cmExtraMalterlibGenerator_h

#include <cmConfigure.h>
#include "cmCustomCommandGenerator.h"

#include "cmExternalMakefileProjectGenerator.h"
#include "cmMalterlibRegistry.h"

#include <map>
#include <set>
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
  void CollectOutputFiles();
  void CreateProjectFile(std::string const &_ProjectName, const std::vector<cmLocalGenerator*>& lgs);

  void CollectOutputFilesFromTargets(std::string const &_ProjectName, std::vector<cmLocalGenerator *> const &lgs, const cmMakefile *mf);
  void CollectOutputFilesFromTarget(std::string const &_ProjectName, cmLocalGenerator* lg, cmGeneratorTarget* target,
                    const char* make, const cmMakefile* makefile,
                    const char* compiler,
                    bool firstTarget);
  void CollectOutputFilesFromFiles(std::string const &_ProjectName, 
    std::vector<cmSourceFile*> const &sourceFiles,
    std::string const &configName,
    cmLocalGenerator* lg,
    const cmGeneratorTarget* target,
    bool isUtilityTarget
  );
  std::string getMappedOutputFile(std::string const &_ProjectName, std::string const &_String, bool _bEvalString);
  std::string replaceMappedOutputFiles(std::string const &_ProjectName, std::string const &_String, bool _bEvalString);
  std::string MakeCustomLauncher(std::string const &_ProjectName, cmLocalGenerator *localGenerator, cmCustomCommandGenerator const &ccg);
  std::string ConvertCommandParam(std::string const &_ProjectName, cmLocalGenerator *localGenerator, std::string const &_String);

  void CreateNewProjectFile(std::string const &_ProjectName, const std::vector<cmLocalGenerator*>& lgs,
                            const std::string& filename);

  /** Appends all targets as build systems to the project file and get all
   * include directories and compiler definitions used.
   */
  void AppendAllTargets(std::string const &_ProjectName, const std::vector<cmLocalGenerator*>& lgs,
                        const cmMakefile* mf, cmMalterlibRegistry& registry
                        );
  /** Appends the specified target to the generated project file as a Sublime
   *  Text build system.
   */
  void AppendTarget(std::string const &_ProjectName, cmMalterlibRegistry& registry,
                    cmLocalGenerator* lg, cmGeneratorTarget* target,
                    const char* make, const cmMakefile* makefile,
                    const char* compiler,
                    bool firstTarget);
  
  void GetTargetFiles(
    std::vector<cmSourceFile*> &sourceFiles,
    cmLocalGenerator* lg, const cmGeneratorTarget* target,
    const cmMakefile* makefile);

  bool CustomCommandIsInDependency(cmLocalGenerator *lg, const cmGeneratorTarget* target, cmCustomCommand* customCommand);

  void AddFilesToRegistry(std::string const &_ProjectName, 
    cmMalterlibRegistry& registry, 
    std::vector<cmSourceFile*> const &sourceFiles,
    std::string const &configName,
    cmLocalGenerator* lg,
    const cmGeneratorTarget* target,
    bool isUtilityTarget
  );

  // The definitions that a compiled source of the target gets, which AddFilesToRegistry records for AppendTarget
  struct cmMalterlibSourceDefines
  {
    cmMalterlibRegistry* File;
    std::string CompileType;
    std::set<std::string> Defines;
  };
  std::vector<cmMalterlibSourceDefines> SourceDefines;

  cmMalterlibRegistry& AddFileInGroup(std::string const &_ProjectName, 
    cmMalterlibRegistry& registry, 
    std::string const &fileName
  );

  std::string tempDir;
  std::string baseDir;
  std::vector<std::string> HidePrefixes;
  std::map<std::string, std::string> ReplacePrefixes;

  std::map<std::string, std::set<std::string>> MappedOutputFiles;
  std::map<std::string, std::set<std::string>> MappedOutputDirectories;

  std::set<std::string> ProtectedFiles;

  // Shared by the targets, so that deep graphs of static libraries are walked once. The generator has one configuration.
  std::map<cmGeneratorTarget const*, std::set<cmGeneratorTarget const*>> LinkClosures;
  std::map<cmGeneratorTarget const*, std::set<cmGeneratorTarget const*>> ReachableTargets; // Through the build order
  std::map<cmGeneratorTarget const*, std::set<cmGeneratorTarget const*>> RestoredLinks; // Links the build order leaves out
};

#endif
