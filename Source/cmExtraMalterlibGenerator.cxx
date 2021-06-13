/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmExtraMalterlibGenerator.h"
#include "cmMalterlibRegistry.h"
#include "cmSystemTools.h"
#include "cmCustomCommand.h"

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
#include "cmLocalCommonGenerator.h"
#include "cmRulePlaceholderExpander.h"

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


  std::string GetTargetName(const cmGeneratorTarget* target, std::string const &projectName) {
    std::string prefix;
    switch (target->GetType()) {
      case cmStateEnums::EXECUTABLE:
        prefix = "Com_" + projectName + "_";
        break;
      case cmStateEnums::STATIC_LIBRARY:
        prefix = "Lib_" + projectName + "_";
        break;
      case cmStateEnums::OBJECT_LIBRARY:
        prefix = "Lib_" + projectName + "_";
        break;
      case cmStateEnums::SHARED_LIBRARY:
        prefix = "Dll_" + projectName + "_";
        break;
      case cmStateEnums::MODULE_LIBRARY:
        prefix = "Dll_" + projectName + "_";
        break;
      case cmStateEnums::UTILITY:
        prefix = "Tool_" + projectName + "_";
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
  tempDir = cmSystemTools::GetEnv("CMAKE_MALTERLIB_TEMPDIR");
  baseDir = cmSystemTools::GetEnv("CMAKE_MALTERLIB_BASEDIR");

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

void cmExtraMalterlibGenerator::CollectOutputFiles()
{
  for (auto &Project : this->GlobalGenerator->GetProjectMap()) {
    auto &lgs = Project.second;
    std::string outputDir = lgs[0]->GetCurrentBinaryDirectory();
    std::string projectName = lgs[0]->GetProjectName();

    const cmMakefile* mf = lgs[0]->GetMakefile();
    CollectOutputFilesFromTargets(Project.first, lgs, mf);
  }
}

void cmExtraMalterlibGenerator::Generate()
{
  CollectOutputFiles();
  // for each sub project in the project create a Malterrlib Header files
  for (auto &Project : this->GlobalGenerator->GetProjectMap()) {
    // create a project file
    this->CreateProjectFile(Project.first, Project.second);
  }

  std::set<std::string> AllOutputfiles;
  for (auto &Files : MappedOutputFiles) {
    for (auto &File : Files.second)
      AllOutputfiles.insert(File);
  }

  {
    cmGeneratedFileStream fout((tempDir + "/OutputFiles.list").c_str());
    for (auto &File : AllOutputfiles) {
        fout << File;
        fout << "\n";
    }
  }
  {
    cmGeneratedFileStream fout((tempDir + "/ProtectedFiles.list").c_str());
    for (auto &File : ProtectedFiles) {
        fout << File;
        fout << "\n";
    }
  }
}

void cmExtraMalterlibGenerator::CreateProjectFile(std::string const &_ProjectName, const std::vector<cmLocalGenerator*>& lgs)
{
  std::string outputDir = lgs[0]->GetCurrentBinaryDirectory();
  std::string projectName = lgs[0]->GetProjectName();

  const std::string filename =
    outputDir + "/" + projectName + ".MHeader";

  this->CreateNewProjectFile(_ProjectName, lgs, filename);

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
  {
    cmGeneratedFileStream fout((filename + ".outputs").c_str());
    for (auto &File : MappedOutputFiles[_ProjectName]) {
        fout << File;
        fout << "\n";
    }
  }
}

static bool isDynamic(std::string const &_String) {
  return _String.find("->MakeAbsolute()") != std::string::npos;
}

static std::string makeAbsoluteWrapper(std::string const &fileName) {
  if (fileName.empty())
    return "\".\"->MakeAbsolute()";

  if (isDynamic(fileName))
    return fileName;

  return cmMalterlibRegistry::getEscaped(fileName, true, true) + "->MakeAbsolute()";
}

static std::string makeAbsoluteWrapperEvalString(std::string const &fileName) {
  if (isDynamic(fileName))
    return fileName;

  return "@(" + cmMalterlibRegistry::getEscaped(fileName, true, true) + "->MakeAbsolute()->EscapeHost())";
}

static std::string makeIdentifier(std::string const &stringIdentifier) {
  std::string identifier;
  constexpr static auto underscore = "\\";

  for (auto &character : stringIdentifier) {
    if ((character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z')
        || character == '_') {
      identifier.append(&character, 1);
    } else {
      identifier.append(underscore);
      identifier.append(&character, 1);
    }
  }

  return identifier;
}

void cmExtraMalterlibGenerator::CreateNewProjectFile(std::string const &_ProjectName,
  const std::vector<cmLocalGenerator*>& lgs, const std::string& filename)
{
  cmGeneratedFileStream fout(filename.c_str());
  if (!fout) {
    return;
  }

  cmMalterlibRegistry registry;
  std::string projectName = lgs[0]->GetProjectName();

  auto &child = registry.addChild("Property.CMakeOutputPath_" + makeIdentifier(projectName),
                    "define string = " + makeAbsoluteWrapper(lgs[0]->GetBinaryDirectory()));

  child.RawKey = true;
  child.RawValue = true;

  const cmMakefile* mf = lgs[0]->GetMakefile();
  AppendAllTargets(_ProjectName, lgs, mf, registry);

  registry.output(fout);
}

void cmExtraMalterlibGenerator::CollectOutputFilesFromTargets(std::string const &_ProjectName, std::vector<cmLocalGenerator *> const &lgs, const cmMakefile *mf)
{
  std::string make = mf->GetRequiredDefinition("CMAKE_MAKE_PROGRAM");
  std::string compiler = "";

  // add all executable and library targets and some of the GLOBAL
  // and UTILITY targets
  for (std::vector<cmLocalGenerator*>::const_iterator lg = lgs.begin();
       lg != lgs.end(); lg++) {
    cmMakefile* makefile = (*lg)->GetMakefile();
    for (auto &target : (*lg)->GetGeneratorTargets()) {
      std::string targetName = target->GetName();
      switch (target->GetType()) {
        case cmStateEnums::GLOBAL_TARGET:
          break;
        case cmStateEnums::UTILITY:
        case cmStateEnums::INTERFACE_LIBRARY:
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

          this->CollectOutputFilesFromTarget(_ProjectName, *lg, target.get(), make.c_str(),
                             makefile, compiler.c_str(),
                             false);
          break;
        case cmStateEnums::EXECUTABLE:
        case cmStateEnums::STATIC_LIBRARY:
        case cmStateEnums::SHARED_LIBRARY:
        case cmStateEnums::MODULE_LIBRARY:
        {
          this->CollectOutputFilesFromTarget(_ProjectName, *lg, target.get(), make.c_str(),
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

void cmExtraMalterlibGenerator::AppendAllTargets(std::string const &_ProjectName,
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
    for (auto &target : (*lg)->GetGeneratorTargets()) {
      std::string targetName = target->GetName();
      switch (target->GetType()) {
        case cmStateEnums::GLOBAL_TARGET:
          break;
        case cmStateEnums::UTILITY:
        case cmStateEnums::INTERFACE_LIBRARY:
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

          this->AppendTarget(_ProjectName, registry, *lg, target.get(), make.c_str(),
                             makefile, compiler.c_str(),
                             false);
          break;
        case cmStateEnums::EXECUTABLE:
        case cmStateEnums::STATIC_LIBRARY:
        case cmStateEnums::SHARED_LIBRARY:
        case cmStateEnums::MODULE_LIBRARY:
        {
          this->AppendTarget(_ProjectName, registry, *lg, target.get(), make.c_str(),
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

std::string cmExtraMalterlibGenerator::replaceMappedOutputFiles(std::string const &_ProjectName, std::string const &_String, bool _bEvalString)
{
  std::string OutputString = _String;

  if (StringStartsWithPath(_String, tempDir.c_str())) {
    auto end = MappedOutputFiles[_ProjectName].end();
    if (auto found = MappedOutputFiles[_ProjectName].find(_String); found != end)
      cmSystemTools::ReplaceString(OutputString, *found, getMappedOutputFile(_ProjectName, *found, _bEvalString));
    return OutputString;
  }

  for (auto &mapping : MappedOutputFiles[_ProjectName])
    cmSystemTools::ReplaceString(OutputString, mapping, getMappedOutputFile(_ProjectName, mapping, _bEvalString));

  return OutputString;
}

std::string cmExtraMalterlibGenerator::getMappedOutputFile(std::string const &_ProjectName, std::string const &_String, bool _bEvalString)
{
  if (!StringStartsWithPath(_String, tempDir.c_str()))
      return _String;

  if (auto found = MappedOutputFiles[_ProjectName].find(_String); found != MappedOutputFiles[_ProjectName].end())
  {
    if (_bEvalString)
      return makeAbsoluteWrapperEvalString(_String);
    else
      return makeAbsoluteWrapper(_String);
  }

  return _String;
}

cmMalterlibRegistry& cmExtraMalterlibGenerator::AddFileInGroup(std::string const &_ProjectName, cmMalterlibRegistry& registry,std::string const &fileName)
{
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
    if (path == "/" || path.empty() || path == "@(CompiledFiles)")
      continue;
    addAtRegistry = &addAtRegistry->addUniqueChild("%Group", path);
    if (cmSystemTools::StringStartsWith(path, "`"))
      addAtRegistry->RawValue = true;

    if (bProtectGroups)
      addAtRegistry->Protected = true;
  }

  auto &toReturn = addAtRegistry->addChild("%File", getMappedOutputFile(_ProjectName, fileName, false));

  if (isDynamic(toReturn.Value))
    toReturn.RawValue = true;

  return toReturn;
}

void cmExtraMalterlibGenerator::CollectOutputFilesFromFiles(std::string const &_ProjectName,
  std::vector<cmSourceFile*> const &sourceFiles,
  std::string const &configName,
  cmLocalGenerator* lg,
  const cmGeneratorTarget* target,
  bool isUtilityTarget
)
{
  auto fAddOutput = [&](std::string const &_Output)
    {
      std::string Output;
      if (StringStartsWithPath(_Output, "/DIR:"))
          Output = _Output.substr(5);
      else
          Output = _Output;

      if (StringStartsWithPath(Output, tempDir.c_str())) {
        //std::cout << "Adding mapped: " << Output << "\n";
        MappedOutputFiles[_ProjectName].insert(Output);
        MappedOutputDirectories[_ProjectName].insert(cmSystemTools::GetFilenamePath(Output));
      } else {
        std::cout << "Non mapped output: " << Output << "\n";
      }
    }
  ;
  auto *pMakefile = lg->GetMakefile();
  for (auto &file : sourceFiles)
  {
    if (!file->GetObjectLibrary().empty())
      continue;

    auto *customCommand = file->GetCustomCommand();

    std::string fullPath = file->GetFullPath();
    if (customCommand)
    {
      cmCustomCommandGenerator customCommandGenerator(*customCommand, configName, lg);

      if (customCommandGenerator.GetCC().GetCommandLines().empty())
        continue;

			auto depFile = customCommandGenerator.GetInternalDepfile();

			if (!depFile.empty())
				fAddOutput(depFile);

      for (auto &output : customCommandGenerator.GetOutputs())
      {
        bool symbolic = false;
        if (cmSourceFile *sf = pMakefile->GetSource(output))
        {
          if (sf->GetPropertyAsBool("SYMBOLIC"))
          {
            symbolic = true;
            break;
          }
        }
        if (!symbolic)
          fAddOutput(output);
      }
      for (auto &output : customCommandGenerator.GetByproducts())
        fAddOutput(output);

      continue;
    }
  }
}

std::string cmExtraMalterlibGenerator::MakeCustomLauncher(std::string const &_ProjectName, cmLocalGenerator *localGenerator, cmCustomCommandGenerator const &ccg)
{
  cmProp property_value = localGenerator->GetMakefile()->GetProperty("RULE_LAUNCH_CUSTOM");

  if (!cmNonempty(property_value)) {
    return std::string();
  }

  // Expand rule variables referenced in the given launcher command.
  cmRulePlaceholderExpander::RuleVariables vars;

  std::string output;
  const std::vector<std::string>& outputs = ccg.GetOutputs();
  if (!outputs.empty()) {
    output = outputs[0];
    output = localGenerator->ConvertToOutputFormat(output, cmOutputConverter::SHELL);
  }
  vars.Output = output.c_str();

  std::unique_ptr<cmRulePlaceholderExpander> rulePlaceholderExpander(localGenerator->CreateRulePlaceholderExpander());

  std::string launcher = *property_value;
  rulePlaceholderExpander->ExpandRuleVariables(localGenerator, launcher, vars);
  if (!launcher.empty()) {
    launcher = ConvertCommandParam(_ProjectName, localGenerator, launcher);
    launcher += " ";
  }

  return launcher;
}

std::string cmExtraMalterlibGenerator::ConvertCommandParam(std::string const &_ProjectName, cmLocalGenerator *localGenerator, std::string const &_String)
{
  auto binaryDir = localGenerator->GetBinaryDirectory();
  std::string param = _String;

  cmSystemTools::ReplaceString(param, "@", "@@");

  if (StringStartsWithPath(param, baseDir.c_str()))
    param = makeAbsoluteWrapperEvalString(replaceMappedOutputFiles(_ProjectName, param, true));
  else if (StringStartsWithPath(param, binaryDir.c_str()))
    param = makeAbsoluteWrapperEvalString(replaceMappedOutputFiles(_ProjectName, param, true));
  else
    param = replaceMappedOutputFiles(_ProjectName, param, true);

  return param;
}

void cmExtraMalterlibGenerator::AddFilesToRegistry(std::string const &_ProjectName,
  cmMalterlibRegistry& registry,
  std::vector<cmSourceFile*> const &sourceFiles,
  std::string const &configName,
  cmLocalGenerator *lg,
  const cmGeneratorTarget* target,
  bool isUtilityTarget)
{
  auto *pMakefile = lg->GetMakefile();

  for (auto &file : sourceFiles) {
    if (!file->GetObjectLibrary().empty())
      continue;

    auto *customCommand = file->GetCustomCommand();

    std::string fullPath = file->GetFullPath();

    bool bIsGenerated = file->GetIsGenerated();

    auto Language = file->GetLanguage();

    std::string malterlibType = getMalterlibCompileType(Language, lg);

    if (file->GetPropertyAsBool("HEADER_FILE_ONLY"))
      malterlibType = "Header";

    std::string keys;
    for (auto &key : file->GetProperties().GetKeys()) {
      keys += " ";
      keys += key;
    }

    if (customCommand) {
      auto &outFile = AddFileInGroup(_ProjectName, registry, fullPath);
      cmCustomCommandGenerator customCommandGenerator(*customCommand, configName, lg);

      cmMalterlibRegistry *pOutCompile = nullptr;
      {
        std::string launcher = this->MakeCustomLauncher(_ProjectName, lg, customCommandGenerator);

        std::string commandLines;

        for (unsigned i = 0; i != customCommandGenerator.GetNumberOfCommands(); ++i)
        {
          auto command = ConvertCommandParam(_ProjectName, lg, customCommandGenerator.GetCommand(i));
          std::string commandLine = replaceMappedOutputFiles(_ProjectName, launcher, true);

          {
            std::string commandValue;

            if (!isDynamic(command))
              commandValue = lg->ConvertToOutputFormat(command, cmOutputConverter::SHELL);
            else
              commandValue = command;

            cmMalterlibRegistry::addEscapeStr(commandLine, commandValue, "`\\\r\n\t", "`\\rnt", false);
          }

          customCommandGenerator.AppendArguments
            (
              i
              , commandLine
              , [&](std::string const &_Param, bool &o_bEscape) -> std::string
              {
                auto toReturn = ConvertCommandParam(_ProjectName, lg, _Param);

                o_bEscape = !isDynamic(toReturn);

                return toReturn;
              }
              , [&](std::string const &_Param) -> std::string
              {
                std::string Escaped;
                cmMalterlibRegistry::addEscapeStr(Escaped, _Param, "`\\\r\n\t", "`\\rnt", false);
                return Escaped;
              }
            )
          ;

          if (commandLines.empty())
            commandLines = commandLine;
          else
          {
            commandLines += " && ";
            commandLines += commandLine;
          }
        }

        if (commandLines.empty())
          continue;

        pOutCompile = &outFile.addChild("Compile", "");

        pOutCompile->addChild("Custom_CommandLine", "`" + commandLines + "`").RawValue = true;
        pOutCompile->addChild("AllowNonExisting", "true").RawValue = true;
        if (isUtilityTarget)
          pOutCompile->addChild("Disabled", "false").RawValue = true;
      }

      auto &OutCompile = *pOutCompile;

      auto WorkingDirectory = customCommandGenerator.GetWorkingDirectory();

      if (!malterlibType.empty())
        OutCompile.addChild("Type", malterlibType);

      std::string workingDirectory = customCommandGenerator.GetWorkingDirectory();
      if (workingDirectory.empty())
        workingDirectory = lg->GetCurrentBinaryDirectory();

      OutCompile.addChild("Custom_WorkingDirectory", makeAbsoluteWrapper(workingDirectory)).RawValue = true;

      std::set<std::string> UsedOutputs;
      {
        std::string outputs = "[";
        size_t index = 0;
        for (auto &output : customCommandGenerator.GetOutputs()) {

          if (StringStartsWithPath(output, "/DIR:"))
            continue;

          bool symbolic = false;
          if (cmSourceFile *sf = pMakefile->GetSource(output))
          {
            if (sf->GetPropertyAsBool("SYMBOLIC"))
            {
              symbolic = true;
              break;
            }
          }

          if (symbolic)
            continue;

          UsedOutputs.insert(output);

          std::string newOutput = getMappedOutputFile(_ProjectName, output, false);

          if (index > 0)
            outputs += ", ";
          ++index;

          outputs += makeAbsoluteWrapper(newOutput);
        }
        for (auto &output : customCommandGenerator.GetByproducts()) {
          if (StringStartsWithPath(output, "/DIR:"))
            continue;

          UsedOutputs.insert(output);

          std::string newOutput = getMappedOutputFile(_ProjectName, output, false);

          if (index > 0)
            outputs += ", ";
          ++index;

          outputs += makeAbsoluteWrapper(newOutput);
        }
        outputs += "]";
        OutCompile.addChild("Custom_Outputs", outputs).RawValue = true;
      }

      std::string firstInput;
      {
        std::string inputs = "[";
        size_t index = 0;
        for (auto &dependency : customCommandGenerator.GetDepends()) {
          std::string realDependency;
          if (lg->GetRealDependency(dependency, configName,
                                    realDependency)) {
            realDependency = getMappedOutputFile(_ProjectName, realDependency, false);
            if (index == 0)
              firstInput = realDependency;
            else
              inputs += ", ";

            inputs += makeAbsoluteWrapper(realDependency);
            ++index;
          }
        }
        inputs += "]";
        OutCompile.addChild("Custom_Inputs", inputs).RawValue = true;
      }

      if (!cmSystemTools::FileExists(fullPath) && MappedOutputFiles[_ProjectName].find(fullPath) == MappedOutputFiles[_ProjectName].end())
      {
        ProtectedFiles.insert(fullPath);
        cmGeneratedFileStream fout(fullPath.c_str());
        fout << firstInput;
      }
      continue;
    }

    if (file->GetPropertyAsBool("SYMBOLIC"))
        continue;

    if (isUtilityTarget)
    {
      AddFileInGroup(_ProjectName, registry, fullPath);
    }
    else if (bIsGenerated)
    {
      auto &outFile = AddFileInGroup(_ProjectName, registry, fullPath);
      outFile.addChild("Compile.AllowNonExisting", "true").RawValue = true;
      if (!malterlibType.empty())
        outFile.addChild("Compile.Type", malterlibType);
    }
    else
    {
      auto &outFile = AddFileInGroup(_ProjectName, registry, fullPath);
      if (!malterlibType.empty())
        outFile.addChild("Compile.Type", malterlibType);
      else if (!Language.empty())
        outFile.addChild("Compile.Type", "None");
      else
        outFile.addChild("Compile.Disabled", "true").RawValue = true;

      std::set<std::string> defines;
      const std::string config = configName;
      cmGeneratorExpressionInterpreter genexInterpreter(
        lg, config, target, file->GetLanguage());

      const std::string COMPILE_DEFINITIONS("COMPILE_DEFINITIONS");
      if (cmProp compile_defs = file->GetProperty(COMPILE_DEFINITIONS)) {
        lg->AppendDefines(
          defines, genexInterpreter.Evaluate(*compile_defs, COMPILE_DEFINITIONS));
      }

      std::string defPropName = "COMPILE_DEFINITIONS_";
      defPropName += cmSystemTools::UpperCase(config);
      if (cmProp config_compile_defs = file->GetProperty(defPropName)) {
        lg->AppendDefines(
          defines,
          genexInterpreter.Evaluate(*config_compile_defs, COMPILE_DEFINITIONS));
      }

      if (cmProp cflags = file->GetProperty("COMPILE_FLAGS")) {
        cmGeneratorExpression ge;
        std::unique_ptr<cmCompiledGeneratorExpression> expression = ge.Parse(*cflags);
        std::string processed = expression->Evaluate(lg, configName);
        std::string cStd;
        ParseCompileFlags(defines, cStd, processed);
      }

      if (!defines.empty()) {
        std::vector<std::string> newDefines;

        for (auto &define : defines)
        {
          auto Remapped = replaceMappedOutputFiles(_ProjectName, define, true);
          if (isDynamic(Remapped))
          {
            std::string Escaped;
            cmMalterlibRegistry::addEscapeStr(Escaped, Remapped, "`\\\r\n\t", "`\\rnt", true);
            newDefines.push_back(Escaped);
          }
          else
            newDefines.push_back(cmMalterlibRegistry::getEscaped(Remapped, true, true));
        }

        auto &outDefines = outFile.addChild("Compile.PreprocessorDefines",
                                            "+= [" + cmJoin(newDefines, ", ") + "]");

        outDefines.RawValue = true;
      }
    }
  }
}

void cmExtraMalterlibGenerator::CollectOutputFilesFromTarget(std::string const &_ProjectName, cmLocalGenerator* lg, cmGeneratorTarget* target,
                  const char* make, const cmMakefile* makefile,
                  const char* compiler,
                  bool firstTarget)
{
  if (target == nullptr)
    return;

  if (!target->IsInBuildSystem())
    return;

  bool isUtilityTarget = false;
  if (target->GetType() == cmStateEnums::UTILITY ||
      target->GetType() == cmStateEnums::INTERFACE_LIBRARY ||
      target->GetType() == cmStateEnums::GLOBAL_TARGET) {
    isUtilityTarget = true;
  }

  std::string configName = "Debug";

  cmLocalCommonGenerator *commonGenerator =
    static_cast<cmLocalCommonGenerator *>(lg);

  if (commonGenerator && !commonGenerator->GetConfigNames().empty()) {
    if (commonGenerator->GetConfigNames().size() > 1)
      lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
        "Generator only supports one config");

    configName = commonGenerator->GetConfigNames()[0];
  }

  std::map<std::string, cmMalterlibCompileTypeInfo> compileTypeInfo;

  std::vector<cmSourceFile*> sourceFiles;
  GetTargetFiles(sourceFiles, lg, target, makefile);
  CollectOutputFilesFromFiles(_ProjectName, sourceFiles, configName, lg, target, isUtilityTarget);

  cmTargetDependSet const& targetDependencies =
    const_cast<cmGlobalGenerator*>(GlobalGenerator)->
    GetTargetDirectDepends(target);

  for (auto &dependency : targetDependencies) {
    auto dependencyLocalGenerator = dependency->GetLocalGenerator();
    if (dependency->GetName() == "global_target" || dependency->GetType() == cmStateEnums::INTERFACE_LIBRARY) {
      continue;
    }

    if (dependency->GetType() == cmStateEnums::OBJECT_LIBRARY) {
      if (!isUtilityTarget) {
        std::vector<cmSourceFile*> sourceFiles;
        GetTargetFiles(sourceFiles, dependencyLocalGenerator, &*dependency, dependencyLocalGenerator->GetMakefile());
        CollectOutputFilesFromFiles(_ProjectName, sourceFiles,
                           configName,
                           dependencyLocalGenerator,
                           &*dependency,
                           isUtilityTarget);
      }
      continue;
    }
  }
}

void cmExtraMalterlibGenerator::AppendTarget(std::string const &_ProjectName,
  cmMalterlibRegistry& registry,
  cmLocalGenerator* lg, cmGeneratorTarget* target, const char* make,
  const cmMakefile* makefile, const char* /*compiler*/,
  bool firstTarget)
{
  if (target == nullptr)
    return;


  if (!target->IsInBuildSystem())
    return;

  bool isUtilityTarget = false;
  if (target->GetType() == cmStateEnums::UTILITY ||
      target->GetType() == cmStateEnums::INTERFACE_LIBRARY ||
      target->GetType() == cmStateEnums::GLOBAL_TARGET) {
    isUtilityTarget = true;
  }

  std::string configName = "Debug";

  cmLocalCommonGenerator *commonGenerator =
    static_cast<cmLocalCommonGenerator *>(lg);

  if (commonGenerator && !commonGenerator->GetConfigNames().empty()) {
    if (commonGenerator->GetConfigNames().size() > 1)
      lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
        "Generator only supports one config");

    configName = commonGenerator->GetConfigNames()[0];
  }

  auto &outputTarget = registry.addChild("%Target", GetTargetName(target, lg->GetProjectName()));
  outputTarget.addChild("Property.MalterlibTargetNameType", "Normal");
  outputTarget.addChild("Compile.AllowNonExisting", "true").RawValue = true;
  if (isUtilityTarget)
    outputTarget.addChild("Compile.Disabled", "true").RawValue = true;
  auto &group = outputTarget.addChild("Target.Group", "External/" + lg->GetProjectName());
  group.addChild("!!Target.Group", "undefined").RawValue = true;
  outputTarget.addChild("Target.Type", GetTargetType(target));
  outputTarget.addChild("Target.BaseName", lg->GetProjectName() + "_" + target->GetName());
  outputTarget.addChild("Target.BaseFileName", target->GetName());

  std::map<std::string, cmMalterlibCompileTypeInfo> compileTypeInfo;
  AddTargetCompileInfo(compileTypeInfo, target, lg, configName);

  if (!isUtilityTarget) {
    AddTargetCompileInfo(compileTypeInfo, target, lg, configName);
    std::vector<cmSourceFile*> sourceFiles;
    GetTargetFiles(sourceFiles, lg, target, makefile);
    AddFilesToRegistry(_ProjectName, outputTarget, sourceFiles, configName, lg, target, false);
  } else {
    std::vector<cmSourceFile*> sourceFiles;
    GetTargetFiles(sourceFiles, lg, target, makefile);
    AddFilesToRegistry(_ProjectName, outputTarget, sourceFiles, configName, lg, target, true);
  }

  auto fAddDependencies = [&](auto &_fAddDependencies, cmGeneratorTarget const *_pTarget, bool _bOnlyObjects, mint _Depth) -> void
    {
      cmTargetDependSet const& targetDependencies =
        const_cast<cmGlobalGenerator*>(GlobalGenerator)->
        GetTargetDirectDepends(_pTarget);

      for (auto &dependency : targetDependencies) {
        auto dependencyLocalGenerator = dependency->GetLocalGenerator();
        if (dependency->GetName() == "global_target" || dependency->GetType() == cmStateEnums::INTERFACE_LIBRARY) {
          continue;
        }

        if (dependency->GetType() == cmStateEnums::OBJECT_LIBRARY) {
          if (!isUtilityTarget) {
            std::vector<cmSourceFile*> sourceFiles;
            GetTargetFiles(sourceFiles, dependencyLocalGenerator, &*dependency, dependencyLocalGenerator->GetMakefile());
            AddFilesToRegistry(_ProjectName, outputTarget,
                               sourceFiles,
                               configName,
                               dependencyLocalGenerator,
                               &*dependency,
                               isUtilityTarget);
            AddTargetCompileInfo(compileTypeInfo, &*dependency, dependencyLocalGenerator, configName);
          }
          continue;
        }
        if (_bOnlyObjects)
          continue;

        auto &outputDependency =
          outputTarget.addChild("%Dependency", GetTargetName(dependency, dependency->LocalGenerator->GetProjectName()));

        if (!dependency.IsLink())
          outputDependency.addChild("Dependency.Link", "false").RawValue = true;
        else if (IsStaticLib(target) && IsStaticLib(dependency)) {
          outputDependency.addChild("Dependency.Indirect", "true").RawValue = true;
        }
      }
    }
  ;

  fAddDependencies(fAddDependencies, target, false, 0);

  if (!isUtilityTarget) {
    for (auto &infoMap : compileTypeInfo) {
      auto &info = infoMap.second;
      {
        for (auto &path : info.Includes)
          path = makeAbsoluteWrapper(cmSystemTools::CollapseFullPath(path));
        auto end = cmRemoveDuplicates(info.Includes);
        info.Includes.erase(end, info.Includes.end());
      }
      auto &compileOutput = outputTarget.addChild("Compile", "", true);
      compileOutput.addChild("!!Compile.Type", infoMap.first);
      {
        std::vector<std::string> NewIncludes;

        for (auto &include : info.Includes)
        {
          NewIncludes.push_back(include);
          if (!StringStartsWithPath(include, tempDir.c_str()))
            continue;

          if (auto found = MappedOutputDirectories[_ProjectName].find(include); found != MappedOutputDirectories[_ProjectName].end())
            NewIncludes.push_back(makeAbsoluteWrapper(include));
        }

        auto &outSearchPath = compileOutput.addChild("SearchPath", "+= [" + cmJoin(info.Includes, ", ") + "]");
        outSearchPath.RawValue = true;
      }
      {
        std::vector<std::string> newDefines;

        for (auto &define : info.Defines)
        {
          auto Remapped = replaceMappedOutputFiles(_ProjectName, define, true);
          if (isDynamic(Remapped))
          {
            std::string Escaped;
            cmMalterlibRegistry::addEscapeStr(Escaped, Remapped, "`\\\r\n\t", "`\\rnt", true);
            newDefines.push_back(Escaped);
          }
          else
            newDefines.push_back(cmMalterlibRegistry::getEscaped(Remapped, true, true));
        }

        auto &outDefines = compileOutput.addChild("PreprocessorDefines", "+= [" + cmJoin(newDefines, ", ") + "]");
        outDefines.RawValue = true;
      }
      if (!info.cStd.empty()) {
        if (infoMap.first == "C") {
          compileOutput.addChild("CLanguage",
                                 cmSystemTools::UpperCase(info.cStd));
          outputTarget.addChild("Target.CLanguage",
                                cmSystemTools::UpperCase(info.cStd), true);
        }
      }
    }
  }

  for (auto &child : outputTarget.Children) {
    if (child.Key == "%Group")
      child.pruneLoneChildren();
  }
}
