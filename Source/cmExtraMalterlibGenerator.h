/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#ifndef cmExtraMalterlibGenerator_h
#define cmExtraMalterlibGenerator_h

#include <cmConfigure.h>
#include "cmCustomCommandGenerator.h"

#include "cmExternalMakefileProjectGenerator.h"
#include "cmMalterlibRegistry.h"

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <cm/optional>
#include <cm/string_view>

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

  // Evaluations for what the import writes (EvaluateMarkedValue)
  std::string EvaluateMarkedString(std::function<std::string()> const &_fEvaluate);
  std::vector<std::string> EvaluateMarkedList(std::function<std::vector<std::string>()> const &_fEvaluate);
  std::set<std::string> EvaluateMarkedSet(std::function<std::set<std::string>()> const &_fEvaluate);
  std::string RemoveTargetDirectoryMarkers(std::string _Text) const;

private:
  template <typename t_CValue>
  t_CValue EvaluateMarkedValue(std::function<t_CValue()> const &_fEvaluate);
  bool IsMarkedOf(std::string const &_Marked, std::string const &_Plain) const;
  bool MatchMarked(std::string const &_Marked, std::string::size_type _iMarked, std::string const &_Plain,
                   std::string::size_type _iPlain) const;
  void TakeMarked(std::string &o_Plain, std::string &&_Marked) const;
  void TakeMarked(std::vector<std::string> &o_Plain, std::vector<std::string> &&_Marked) const;
  void TakeMarked(std::set<std::string> &o_Plain, std::set<std::string> &&_Marked) const;
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
  // How a mapped file is written: as an expression of its path, as its path in an eval string, or as an argument of a
  // shell command in an eval string, which escapes itself for the shell
  enum class MappedForm
  {
    Expression,
    EvalString,
    ShellArgument
  };
  std::string getMappedOutputFile(std::string const &_ProjectName, std::string const &_String, MappedForm _Form,
                                  bool _bWindowsSeparators = false);
  std::string replaceMappedOutputFiles(std::string const &_ProjectName, std::string const &_String, MappedForm _Form);
  // Per project, the files that targets link and the outputs of commands, which texts name by these spellings
  struct cmMappedPathSpelling
  {
    std::string const *pPath;
    bool bWindowsSeparators;
    bool bTargetFile; // Rather than an output of a command
  };
  using cmMappedPathEntry = std::pair<std::string, cmMappedPathSpelling>;
  struct cmMappedPathSet
  {
    std::vector<cmMappedPathEntry> Spellings; // Sorted
    std::size_t nPaths = std::size_t(-1); // In the tables that the set has, which only grow
    bool StartsPath[256] = {};
  };
  std::map<std::string, cmMappedPathSet> MappedPathSets;
  cmMappedPathSet const &GetMappedPathSet(std::string const &_ProjectName);
  static cmMappedPathSpelling const *FindLongestMappedPath(cmMappedPathSet const &_Set, cm::string_view _Text,
                                                          std::size_t &o_Size);
  std::string MakeCustomLauncher(std::string const &_ProjectName, cmLocalGenerator *localGenerator, cmCustomCommandGenerator const &ccg);
  std::string ConvertCommandParam(std::string const &_ProjectName, cmLocalGenerator *localGenerator, std::string const &_String);
  std::string GetCustomCommandLines(std::string const &_ProjectName, cmLocalGenerator *lg,
                                    cmCustomCommandGenerator const &customCommandGenerator, std::string const &configName,
                                    std::string const &commandWorkingDirectory,
                                    std::set<std::string> const &_OtherFiles = {});
  void CollectBuildEventFiles(std::string const &_ProjectName, cmLocalGenerator *lg, cmGeneratorTarget const *target);
  void AddBuildEvents(std::string const &_ProjectName, cmMalterlibRegistry &outputTarget, cmLocalGenerator *lg,
                      cmGeneratorTarget const *target, std::string const &configName, bool isUtilityTarget);
  void AddBundleContents(std::string const &_ProjectName, cmMalterlibRegistry &outputTarget, cmGeneratorTarget const *target,
                         std::string const &configName);

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
    std::shared_ptr<std::set<std::string> const> Defines; // The sources that have only those of the target share them
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
  // Per project, the outputs of the custom commands, before the files of the build events are added to them
  std::map<std::string, std::set<std::string>> CommandOutputFiles;
  struct cmMalterlibTargetFile
  {
    std::string TargetName; // Of the Malterlib target
    std::string Property; // Of the Malterlib target, with the file that it links
    std::string FinalProperty; // Of the Malterlib target, with the file that it has built, signed and deployed
  };
  // Per project, the files that its targets link in the build tree of CMake
  std::map<std::string, std::map<std::string, cmMalterlibTargetFile>> TargetFiles;
  std::string CurrentTargetName; // Of the Malterlib target that AppendTarget adds
  std::set<std::string> TargetFileDependencies; // The Malterlib targets whose files that target uses, by GetTargetFileExpression
  std::set<std::string> TargetPathDependencies; // The Malterlib targets that only name the directories that that target uses
  enum class TargetFileVersion
  {
    Linked, // As the Malterlib target links it
    Deployed, // Once the Malterlib build has signed and deployed it
    ForTarget // Linked for the target itself, unless MapOwnTargetToDeployed, and deployed for the other targets
  };
  bool MapOwnTargetToDeployed = false; // While the POST_BUILD of the target that runs on its deployed file is added
  bool MapPathsOnly = false; // While paths are mapped that the build doesn't read, as of definitions, which don't order the build
  bool InOwnPostBuild = false; // While a POST_BUILD of the target is added
  std::string GetTargetFileExpression(std::string const &_ProjectName, std::string const &_File,
                                      TargetFileVersion _Version = TargetFileVersion::ForTarget, bool _bOrdersBuild = true);
  std::string GetTargetPropertyExpression(cmMalterlibTargetFile const &_TargetFile, std::string const &_Property,
                                          bool _bOrdersBuild = true);
  std::string GetTargetSidecarExpression(std::string const &_ProjectName, std::string const &_File);
  std::string GetTargetPathExpression(std::string const &_ProjectName, std::string const &_Path);
  cmCustomCommandGenerator MakeCommandGenerator(cmCustomCommand const &_Command, std::string const &_Config, cmLocalGenerator *lg);
  // The files whose directories the evaluated commands name, by the index in their markers
  std::vector<std::string> TargetDirectories;
  // The files that configure_file and file(GENERATE) write, which the import cache holds, also where they are in the directory of a
  // target
  std::set<std::string> ConfigureOutputFiles;
  bool IsConfigureFile(std::string const &_Path) const;
  struct cmFileInDirectory
  {
    std::string::size_type nSize; // Of the path after the directory
    std::string Mapped;
  };
  cm::optional<cmFileInDirectory> FindFileInDirectory(std::string const &_ProjectName, std::string const &_String,
                                                      std::string::size_type _iPath, std::string::size_type _iLimit,
                                                      std::string const &_Directory, bool _bWindowsSeparators, MappedForm _Form);
  struct cmTargetDirectoryMarker
  {
    std::string::size_type iStart; // Of the directory
    std::string::size_type iMarker;
    std::string::size_type iEnd; // Of the marker
    std::string const *pPath; // The file
    bool bWindowsSeparators; // Of the directory
  };
  cm::optional<cmTargetDirectoryMarker> FindTargetDirectoryMarker(std::string const &_Text, std::string::size_type _iFrom) const;
  struct cmTargetPath
  {
    enum class EKind
    {
      None,
      FileDirectory, // Key is the file
      Bundle // Key is the bundle directory
    };
    EKind Kind;
    std::string Key;
    std::string Rest; // Of the path, after the directory
  };
  cmTargetPath FindTargetPath(std::string const &_ProjectName, std::string const &_Path) const;
  // Per project, the bundle directories of the application bundles in the build tree of CMake
  std::map<std::string, std::set<std::string>> TargetBundleDirectories;
  std::string GetWorkingDirectoryExpression(std::string const &_ProjectName, std::string const &_WorkingDirectory);
  std::string GetSearchDirectoryValue(std::string const &_ProjectName, std::string const &_Directory);
  // Per project, the files that the build events of a target write beside the file that it links, by the name of that file
  // with an extension or in its directory, with that file and whether its POST_BUILD writes them, which can run on the deployed
  // file
  struct cmMalterlibSidecarFile
  {
    std::string TargetFile;
    bool bPostBuild;
    bool bInDirectory; // Rather than by the name of the file with an extension
  };
  std::map<std::string, std::map<std::string, cmMalterlibSidecarFile>> TargetSidecarFiles;
  // Per project, the outputs of the commands that run nothing in the configuration, with the files that they depend on
  std::map<std::string, std::map<std::string, std::vector<std::string>>> PhonyOutputDependencies;
  void ResolvePhonyDependency(std::string const &_ProjectName, std::string const &_Dependency,
                              std::vector<std::string> &o_Dependencies, std::set<std::string> &o_Visited);

  std::set<std::string> ProtectedFiles;

  // Shared by the targets, so that deep graphs of static libraries are walked once. The generator has one configuration.
  std::map<cmGeneratorTarget const*, std::set<cmGeneratorTarget const*>> LinkClosures;
  std::map<cmGeneratorTarget const*, std::set<cmGeneratorTarget const*>> ReachableTargets; // Through the build order
  std::map<cmGeneratorTarget const*, std::set<cmGeneratorTarget const*>> RestoredLinks; // Links the build order leaves out
};

#endif
