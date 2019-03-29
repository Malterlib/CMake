/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmExtraMalterlibGenerator.h"
#include "cmMalterlibRegistry.h"
#include "cmSystemTools.h"

#include <cmsys/RegularExpression.hxx>
#include <set>
#include <sstream>
#include <string.h>
#include <utility>
#include <iostream>

#include "cmGeneratedFileStream.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmSourceFile.h"
#include "cmStateTypes.h"
#include "cmSystemTools.h"
#include "cmake.h"
#include "cmCustomCommandGenerator.h"
#include "cmLocalCommonGenerator.h"

/*
Malterlib Generator
Author: Erik Olofsson
This generator was initially based off of the Sublime Text generator.
*/

namespace
{
// Returns if string starts with another string
#if defined(_WIN32)
  bool StringStartsWithPath(const std::string& str1, const char* str2)
  {
    if (!str2) {
      return false;
    }
    size_t len1 = str1.size(), len2 = strlen(str2);
    return len1 >= len2 && !strnicmp(str1.c_str(), str2, len2) ? true : false;
  }
#else
  bool StringStartsWithPath(const std::string& str1, const char* str2)
  {
    return cmSystemTools::StringStartsWith(str1, str2);
  }
#endif


  std::string GetTargetName(const cmGeneratorTarget* target) {
    std::string prefix;
    switch (target->GetType()) {
      case cmStateEnums::EXECUTABLE:
        prefix = "Exe_";
        break;
      case cmStateEnums::STATIC_LIBRARY:
        prefix = "Lib_";
        break;
      case cmStateEnums::OBJECT_LIBRARY:
        prefix = "Lib_";
        break;
      case cmStateEnums::SHARED_LIBRARY:
        prefix = "Dll_";
        break;
      case cmStateEnums::MODULE_LIBRARY:
        prefix = "Dll_";
        break;
      case cmStateEnums::UTILITY:
        prefix = "Tool_";
        break;
      default:
        assert(false);
        break;
    }
    return prefix + target->GetName();
  }

  std::string GetTargetType(const cmGeneratorTarget* target) {
    switch (target->GetType()) {
      case cmStateEnums::EXECUTABLE:
        return "ConsoleExecutable";
      case cmStateEnums::STATIC_LIBRARY:
        return "StaticLibrary";
      case cmStateEnums::OBJECT_LIBRARY:
        return "StaticLibrary";
      case cmStateEnums::SHARED_LIBRARY:
        return "SharedDynamicLibrary";
      case cmStateEnums::MODULE_LIBRARY:
        return "DynamicLibrary";
      case cmStateEnums::UTILITY:
        return "Tool";
      default:
        assert(false);
        break;
    }
    return "";
  }
  
  bool IsStaticLib(const cmGeneratorTarget* target) {
    switch (target->GetType()) {
      case cmStateEnums::STATIC_LIBRARY:
      case cmStateEnums::OBJECT_LIBRARY:
        return true;
      default:
        return false;
    }
  }
  
  void ParseCompileFlags(std::set<std::string>& defines, std::string &cStd,
                                  const std::string& flags) {
    cmsys::RegularExpression flagRegex;
    // Regular expression to extract compiler flags from a string
    // https://gist.github.com/3944250
    const char* regexString =
      "(^|[ ])-[DIOUWfgs][^= ]+(=\\\"[^\"]+\\\"|=[^\"][^ ]+)?";
    flagRegex.compile(regexString);
    std::string workString = flags;
    while (flagRegex.find(workString)) {
      std::string::size_type start = flagRegex.start();
      if (workString[start] == ' ') {
        start++;
      }
      std::string flag = workString.substr(start, flagRegex.end() - start);
      if (cmSystemTools::StringStartsWith(flag, "-D")) {
        defines.insert(flag.substr(2));
      }
      else if (cmSystemTools::StringStartsWith(flag, "-std=")) {
        cStd = flag.substr(5);
      }
      if (flagRegex.end() < workString.size()) {
        workString = workString.substr(flagRegex.end());
      } else {
        workString = "";
      }
    }
  }

  std::string getMalterlibCompileType(std::string const &language, 
                                      cmLocalGenerator *localGenerator) {
    if (language.empty())
      return std::string{};
    
    char const *malterlibLanguage = 
      cmSystemTools::GetEnv("CMAKE_MALTERLIB_LANGUAGE_" + language);
    if (!malterlibLanguage) {
        localGenerator->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          "Language not recognized. Please add to Property.CMake_Languages: " + 
          language);
      return std::string{};
    }
    return malterlibLanguage;
  }
  
  struct cmMalterlibCompileTypeInfo
  {
    std::vector<std::string> Includes;
    std::set<std::string> Defines;
    std::string cStd;
  };
  
  void AddTargetCompileInfo(
      std::map<std::string, cmMalterlibCompileTypeInfo> &compileTypeInfo
      , const cmGeneratorTarget* target
      , cmLocalGenerator* lg
      , std::string const &configName) 
  {
    std::set<std::string> targetLanguages;
    target->GetLanguages(targetLanguages, configName);
    for (auto &language : targetLanguages) {
      std::string compileType = getMalterlibCompileType(language, lg);
      if (language.empty())
        continue;
      auto &info = compileTypeInfo[compileType];
      lg->GetIncludeDirectories(info.Includes, target, language, configName);
      lg->GetTargetDefines(target, configName, language, info.Defines);
      std::string compileFlags;
      lg->GetTargetCompileFlags(const_cast<cmGeneratorTarget* >(target), 
                                configName, language, compileFlags);
      
      ParseCompileFlags(info.Defines, info.cStd, compileFlags);
    }
  }
}

cmExternalMakefileProjectGeneratorFactory*
cmExtraMalterlibGenerator::GetFactory()
{
  static cmExternalMakefileProjectGeneratorSimpleFactory<
    cmExtraMalterlibGenerator>
    factory("Malterlib", "Generates Malterlib build system files.");

  if (factory.GetSupportedGlobalGenerators().empty()) {
    factory.AddSupportedGlobalGenerator("Ninja");
  }

  return &factory;
}

cmExtraMalterlibGenerator::cmExtraMalterlibGenerator()
  : cmExternalMakefileProjectGenerator()
{
  char const *hidePrefixesString = 
    cmSystemTools::GetEnv("CMAKE_MALTERLIB_HIDEPREFIXES");
  if (hidePrefixesString) {
    this->HidePrefixes
      = cmSystemTools::SplitString(hidePrefixesString, ';');
  }
  char const *replacePrefixesString = 
    cmSystemTools::GetEnv("CMAKE_MALTERLIB_REPLACEPREFIXES");
  
  if (replacePrefixesString) {
    auto replaceSplit = cmSystemTools::SplitString(replacePrefixesString, ';');
    for (auto &replace : replaceSplit) {
      if (replace.empty())
        continue;
      auto split = cmSystemTools::SplitString(replace, '=');
      if (split.size() != 2)
        continue;
      ReplacePrefixes[split[0]] = split[1];
    }
  }
}

void cmExtraMalterlibGenerator::Generate()
{
  // for each sub project in the project create a Malterrlib Header files
  for (std::map<std::string, std::vector<cmLocalGenerator*> >::const_iterator
         it = this->GlobalGenerator->GetProjectMap().begin();
       it != this->GlobalGenerator->GetProjectMap().end(); ++it) {
    // create a project file
    this->CreateProjectFile(it->second);
  }
}

void cmExtraMalterlibGenerator::CreateProjectFile(
  const std::vector<cmLocalGenerator*>& lgs)
{
  std::string outputDir = lgs[0]->GetCurrentBinaryDirectory();
  std::string projectName = lgs[0]->GetProjectName();

  const std::string filename =
    outputDir + "/" + projectName + ".MHeader";

  this->CreateNewProjectFile(lgs, filename);
  
  {
    std::vector<std::string> lfiles;
    for (std::vector<cmLocalGenerator*>::const_iterator gi = lgs.begin();
         gi != lgs.end(); ++gi) {
      std::vector<std::string> const& lf = (*gi)->GetMakefile()->GetListFiles();
      lfiles.insert(lfiles.end(), lf.begin(), lf.end());
    }

    // sort the array
    std::sort(lfiles.begin(), lfiles.end(), std::less<std::string>());
    std::vector<std::string>::iterator new_end =
      std::unique(lfiles.begin(), lfiles.end());
    lfiles.erase(new_end, lfiles.end());

    cmGeneratedFileStream fout((filename + ".dependencies").c_str());
    for (auto &dependency : lfiles)
    {
      fout << dependency;
      fout << "\n";
    }
  }
}

static std::string makeAbsoluteWrapper(std::string const &fileName) {
  return "@('" + fileName + "'->MakeAbsolute())";
}

void cmExtraMalterlibGenerator::CreateNewProjectFile(
  const std::vector<cmLocalGenerator*>& lgs, const std::string& filename)
{
  cmGeneratedFileStream fout(filename.c_str());
  if (!fout) {
    return;
  }
  
  cmMalterlibRegistry registry;
  std::string projectName = lgs[0]->GetProjectName();
  
  registry.addChild("Property.CMakeOutputPath_" + projectName, 
                    makeAbsoluteWrapper(lgs[0]->GetBinaryDirectory()));
  
  const cmMakefile* mf = lgs[0]->GetMakefile();
  AppendAllTargets(lgs, mf, registry);
  
  registry.output(fout);
}

void cmExtraMalterlibGenerator::AppendAllTargets(
  const std::vector<cmLocalGenerator*>& lgs, const cmMakefile* mf,
  cmMalterlibRegistry& registry)
{
  std::string make = mf->GetRequiredDefinition("CMAKE_MAKE_PROGRAM");
  std::string compiler = "";

  // add all executable and library targets and some of the GLOBAL
  // and UTILITY targets
  for (std::vector<cmLocalGenerator*>::const_iterator lg = lgs.begin();
       lg != lgs.end(); lg++) {
    cmMakefile* makefile = (*lg)->GetMakefile();
    std::vector<cmGeneratorTarget*> targets = (*lg)->GetGeneratorTargets();
    for (std::vector<cmGeneratorTarget*>::iterator ti = targets.begin();
         ti != targets.end(); ti++) {
      std::string targetName = (*ti)->GetName();
      switch ((*ti)->GetType()) {
        case cmStateEnums::GLOBAL_TARGET:
          break;
        case cmStateEnums::UTILITY:
          // Add all utility targets, except the Nightly/Continuous/
          // Experimental-"sub"targets as e.g. NightlyStart
          if (((targetName.find("Nightly") == 0) &&
               (targetName != "Nightly")) ||
              ((targetName.find("Continuous") == 0) &&
               (targetName != "Continuous")) ||
              ((targetName.find("Experimental") == 0) &&
               (targetName != "Experimental"))) {
            break;
          }

          this->AppendTarget(registry, *lg, nullptr, make.c_str(),
                             makefile, compiler.c_str(), 
                             false);
          break;
        case cmStateEnums::EXECUTABLE:
        case cmStateEnums::STATIC_LIBRARY:
        case cmStateEnums::SHARED_LIBRARY:
        case cmStateEnums::MODULE_LIBRARY:
        {
          this->AppendTarget(registry, *lg, *ti, make.c_str(),
                             makefile, compiler.c_str(), 
                             false);
        } break;
        case cmStateEnums::OBJECT_LIBRARY: 
        default:
          break;
      }
    }
  }
}

void cmExtraMalterlibGenerator::GetTargetFiles(
  std::vector<cmSourceFile*> &sourceFiles,
  cmLocalGenerator* lg, const cmGeneratorTarget* target,
  const cmMakefile* makefile)
{
  std::vector<cmSourceFile*> newSourceFiles;
  {
    target->GetSourceFiles(newSourceFiles,
                           makefile->GetSafeDefinition("CMAKE_BUILD_TYPE"));
    std::vector<cmSourceFile*>::const_iterator sourceFilesEnd =
      newSourceFiles.end();
    for (std::vector<cmSourceFile*>::const_iterator iter = 
         newSourceFiles.begin();
         iter != sourceFilesEnd; ++iter) {
      cmSourceFile* sourceFile = *iter;
      sourceFiles.push_back(sourceFile);
    }
  }
}

cmMalterlibRegistry& cmExtraMalterlibGenerator::AddFileInGroup(
  cmMalterlibRegistry& registry,
  std::string const &fileName
) {
  std::string strippedFileName = fileName;
  for (auto &prefix : ReplacePrefixes){
    if (StringStartsWithPath(strippedFileName.c_str(), prefix.first.c_str())) {
      strippedFileName = prefix.second + strippedFileName.substr(prefix.first.size());
      break;
    }
  }
  
  bool bFirstPrefix = true;
  bool bProtectGroups = false;
  for (auto &prefix : HidePrefixes){
    if (StringStartsWithPath(strippedFileName.c_str(), prefix.c_str())) {
      if (bFirstPrefix)
        bProtectGroups = true;
      strippedFileName = strippedFileName.substr(prefix.size()+1);
      break;
    }
    bFirstPrefix = false;
  }
    
  std::vector<std::string> components;
  cmSystemTools::SplitPath(cmSystemTools::GetFilenamePath(strippedFileName), 
                           components);
  cmMalterlibRegistry *addAtRegistry = &registry;
  for (auto &path : components) {
    if (path == "/" || path.empty())
      continue;
    addAtRegistry = &addAtRegistry->addUniqueChild("%Group", path);
    if (bProtectGroups)
      addAtRegistry->Protected = true;
  }
  
  return addAtRegistry->addChild("%File", fileName);
}

void cmExtraMalterlibGenerator::AddFilesToRegistry(
  cmMalterlibRegistry& registry,
  std::vector<cmSourceFile*> const &sourceFiles,
  std::string const &configName,
  cmLocalGenerator *lg,
  const cmGeneratorTarget* target)
{
  for (auto &file : sourceFiles) {
    if (!file->GetObjectLibrary().empty())
      continue;
    
    auto *customCommand = file->GetCustomCommand();
	  
    std::string fullPath = file->GetFullPath();
    auto generated = file->GetProperties().GetPropertyValue("GENERATED");
    
    std::string malterlibType = getMalterlibCompileType(file->GetLanguage(), 
                                                        lg);
    
    if (customCommand) {
      auto &outFile = AddFileInGroup(registry, fullPath);
      cmCustomCommandGenerator customCommandGenerator(*customCommand
        , configName, lg);
      auto &OutCompile = outFile.addChild("Compile", "");
      
      if (!malterlibType.empty())
        OutCompile.addChild("Type", malterlibType);
      OutCompile.addChild("Custom_WorkingDirectory", 
                          makeAbsoluteWrapper(customCommandGenerator.GetWorkingDirectory()));

      {
        std::string outputs;
        for (auto &output : customCommandGenerator.GetOutputs()) {

          if (outputs.empty()) {
            outputs = makeAbsoluteWrapper(output);
          } else {
            outputs += ";";
            outputs += makeAbsoluteWrapper(output);
          }
        }
        OutCompile.addChild("Custom_Outputs", outputs);
      }
      
      std::string firstInput;
      {
        std::string inputs;
        size_t index = 0;
        for (auto &dependency : customCommandGenerator.GetDepends()) {
          std::string realDependency;
          if (lg->GetRealDependency(dependency, configName,
                                    realDependency)) {
            if (index == 0)
              firstInput = realDependency;
            if (inputs.empty()) {
              inputs = makeAbsoluteWrapper(realDependency);
            } else {
              inputs += ";";
              inputs += makeAbsoluteWrapper(realDependency);
            }
            ++index;
          }
        }
        OutCompile.addChild("Custom_Inputs", inputs);
      }
      {
        cmGeneratedFileStream fout(fullPath.c_str());
        fout << firstInput;
      }
      {
        auto baseDir = cmSystemTools::GetEnv("CMAKE_MALTERLIB_BASEDIR");
        auto binaryDir = lg->GetBinaryDirectory();
        
        std::string commandLines;
        for (auto &commandLine :
             customCommandGenerator.GetCC().GetCommandLines()) {

          auto newCommandLine = commandLine;
          for (auto &line : newCommandLine) {
            if (baseDir && StringStartsWithPath(line, baseDir))
              line = makeAbsoluteWrapper(line);
            else if (StringStartsWithPath(line, binaryDir.c_str()))
              line = makeAbsoluteWrapper(line);
          }
          if (commandLines.empty())
            commandLines = cmSystemTools::PrintSingleCommand(newCommandLine);
          else
          {
            commandLines += " && ";
            commandLines += cmSystemTools::PrintSingleCommand(newCommandLine);
          }
        }
        OutCompile.addChild("Custom_CommandLine", commandLines);
      }
      continue;
    }
    if (generated && strcmp(generated, "1") == 0)
    {
      auto &outFile = AddFileInGroup(registry, fullPath);
      outFile.addChild("Compile.AllowNonExisting", "true");
      if (!malterlibType.empty())
        outFile.addChild("Compile.Type", malterlibType);
    }
    else
    {
      auto &outFile = AddFileInGroup(registry, fullPath);
      if (!malterlibType.empty())
        outFile.addChild("Compile.Type", malterlibType);
      else
        outFile.addChild("Compile.Type", "None");

      std::set<std::string> defines;
      const std::string config = configName;
      cmGeneratorExpressionInterpreter genexInterpreter(
        lg, config, target, file->GetLanguage());

      const std::string COMPILE_DEFINITIONS("COMPILE_DEFINITIONS");
      if (const char* compile_defs = file->GetProperty(COMPILE_DEFINITIONS)) {
        lg->AppendDefines(
          defines, genexInterpreter.Evaluate(compile_defs, COMPILE_DEFINITIONS));
      }

      std::string defPropName = "COMPILE_DEFINITIONS_";
      defPropName += cmSystemTools::UpperCase(config);
      if (const char* config_compile_defs = file->GetProperty(defPropName)) {
        lg->AppendDefines(
          defines,
          genexInterpreter.Evaluate(config_compile_defs, COMPILE_DEFINITIONS));
      }

      if (const char* cflags = file->GetProperty("COMPILE_FLAGS")) {
        cmGeneratorExpression ge;
        std::unique_ptr<cmCompiledGeneratorExpression> expression = ge.Parse(cflags);
        std::string processed = expression->Evaluate(lg, configName);
        std::string cStd;
        ParseCompileFlags(defines, cStd, processed);
      }

      if (!defines.empty())
        outFile.addChild("Compile.PreprocessorDefines", 
                         cmJoin(defines, ";") + 
                         ";@(Compile.PreprocessorDefines)");
    }
  }
}

void cmExtraMalterlibGenerator::AppendTarget(
  cmMalterlibRegistry& registry,
  cmLocalGenerator* lg, cmGeneratorTarget* target, const char* make,
  const cmMakefile* makefile, const char* /*compiler*/,
  bool firstTarget)
{
  if (target == nullptr)
    return;
  
  std::string configName = "Debug";
  
  cmLocalCommonGenerator *commonGenerator = 
    static_cast<cmLocalCommonGenerator *>(lg);
  
  if (commonGenerator)
    configName = commonGenerator->GetConfigName();

  auto &outputTarget = registry.addChild("%Target", GetTargetName(target));
  outputTarget.addChild("Property.MalterlibTargetNameType", "Normal");
  outputTarget.addChild("Target.Group", "External/" + lg->GetProjectName());
  outputTarget.addChild("Target.Type", GetTargetType(target));
  outputTarget.addChild("Target.BaseName", target->GetName());

  std::map<std::string, cmMalterlibCompileTypeInfo> compileTypeInfo;
  AddTargetCompileInfo(compileTypeInfo, target, lg, configName);
  
  {
    std::vector<cmSourceFile*> sourceFiles;
    GetTargetFiles(sourceFiles, lg, target, makefile);
    AddFilesToRegistry(outputTarget, sourceFiles, configName, lg, target);
  }
  
  cmTargetDependSet const& targetDependencies = 
    const_cast<cmGlobalGenerator*>(GlobalGenerator)->
    GetTargetDirectDepends(target);
  
  for (auto &dependency : targetDependencies) {
    auto dependencyLocalGenerator = dependency->GetLocalGenerator();
    if (dependency->GetType() == cmStateEnums::INTERFACE_LIBRARY || dependency->GetName() == "global_target") {
      continue;
    }
    
    if (dependency->GetType() == cmStateEnums::OBJECT_LIBRARY) {
      std::vector<cmSourceFile*> sourceFiles;
      GetTargetFiles(sourceFiles, dependencyLocalGenerator, &*dependency, dependencyLocalGenerator->GetMakefile());
      AddFilesToRegistry(outputTarget, 
                         sourceFiles, 
                         configName, 
                         dependencyLocalGenerator, 
                         &*dependency);
      AddTargetCompileInfo(compileTypeInfo, &*dependency, dependencyLocalGenerator, configName);
      continue;
    }
    auto &outputDependency = 
      outputTarget.addChild("%Dependency", GetTargetName(dependency));
    
    if (!dependency.IsLink())
      outputDependency.addChild("Dependency.Link", "false");
    else if (IsStaticLib(target) && IsStaticLib(dependency))
      outputDependency.addChild("Dependency.Indirect", "true");
  }
  
  for (auto &infoMap : compileTypeInfo) {
    auto &info = infoMap.second;
    {
      for (auto &path : info.Includes)
        path = makeAbsoluteWrapper(cmSystemTools::CollapseFullPath(path));
      auto end = cmRemoveDuplicates(info.Includes);
      info.Includes.erase(end, info.Includes.end());
    }
    info.Includes.push_back("@(Compile.SearchPath)");
    auto &compileOutput = outputTarget.addChild("Compile", "", true);
    compileOutput.addChild("!!Compile.Type", infoMap.first);
    compileOutput.addChild("SearchPath", cmJoin(info.Includes, ";"));      
    compileOutput.addChild("PreprocessorDefines", cmJoin(info.Defines, ";") + 
                           ";@(Compile.PreprocessorDefines)");
    if (!info.cStd.empty()) {
      if (infoMap.first == "C") {
        compileOutput.addChild("CLanguage", 
                               cmSystemTools::UpperCase(info.cStd));
        outputTarget.addChild("Target.CLanguage", 
                              cmSystemTools::UpperCase(info.cStd), true);
      }
    }
  }
  
  for (auto &child : outputTarget.Children) {
    if (child.Key == "%Group")
      child.pruneLoneChildren();
  }
}
