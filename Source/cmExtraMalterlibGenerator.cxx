/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */
#include "cmExtraMalterlibGenerator.h"
#include "cmLocalNinjaGenerator.h"
#include "cmMalterlibRegistry.h"
#include "cmSystemTools.h"
#include "cmCustomCommand.h"

#include <cmsys/FStream.hxx>
#include <cmsys/RegularExpression.hxx>
#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string.h>
#include <utility>
#include <iostream>

#include "cmCryptoHash.h"
#include "cmGeneratedFileStream.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorFileSet.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmList.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmSourceFile.h"
#include "cmStateTypes.h"
#include "cmStringAlgorithms.h"
#include "cmTargetTypes.h"
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
      case cm::TargetType::EXECUTABLE:
        prefix = "Com_" + projectName + "_";
        break;
      case cm::TargetType::STATIC_LIBRARY:
        prefix = "Lib_" + projectName + "_";
        break;
      case cm::TargetType::OBJECT_LIBRARY:
        prefix = "Lib_" + projectName + "_";
        break;
      case cm::TargetType::SHARED_LIBRARY:
        prefix = "Dll_" + projectName + "_";
        break;
      case cm::TargetType::MODULE_LIBRARY:
        prefix = "Dll_" + projectName + "_";
        break;
      case cm::TargetType::UTILITY:
      case cm::TargetType::INTERFACE_LIBRARY:
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
      case cm::TargetType::EXECUTABLE:
        return "ConsoleExecutable";
      case cm::TargetType::STATIC_LIBRARY:
        return "StaticLibrary";
      case cm::TargetType::OBJECT_LIBRARY:
        return "StaticLibrary";
      case cm::TargetType::SHARED_LIBRARY:
        return "SharedDynamicLibrary";
      case cm::TargetType::MODULE_LIBRARY:
        return "DynamicLibrary";
      case cm::TargetType::UTILITY:
      case cm::TargetType::INTERFACE_LIBRARY:
        return "Tool";
      default:
        assert(false);
        break;
    }
    return "";
  }

  bool IsStaticLib(const cmGeneratorTarget* target) {
    switch (target->GetType()) {
      case cm::TargetType::STATIC_LIBRARY:
      case cm::TargetType::OBJECT_LIBRARY:
        return true;
      default:
        return false;
    }
  }

  bool IsObjectLib(const cmGeneratorTarget* target) {
    switch (target->GetType()) {
      case cm::TargetType::OBJECT_LIBRARY:
        return true;
      default:
        return false;
    }
  }

  // The name of a function-like macro ends at its parameters
  std::string macroName(std::string const& define)
  {
    return define.substr(0, define.find_first_of("=("));
  }

  // A later definition of a macro replaces the earlier ones, as it does for the compiler
  void setDefine(std::set<std::string>& defines, std::string define)
  {
    std::string const name = macroName(define);
    for (auto iDefine = defines.begin(); iDefine != defines.end();) {
      if (macroName(*iDefine) == name)
        iDefine = defines.erase(iDefine);
      else
        ++iDefine;
    }
    defines.insert(std::move(define));
  }

  // The Malterlib C language of the value of -std=, or empty for a standard that it doesn't have
  std::string malterlibCLanguage(std::string const& standard)
  {
    static std::map<std::string, std::string> const languages = {
      { "c89", "C89" }, { "c90", "C89" }, { "iso9899:1990", "C89" }, { "gnu89", "GNU89" }, { "gnu90", "GNU89" },
      { "c99", "C99" }, { "c9x", "C99" }, { "iso9899:1999", "C99" }, { "gnu99", "GNU99" }, { "gnu9x", "GNU99" },
      { "c11", "C11" }, { "c1x", "C11" }, { "iso9899:2011", "C11" }, { "gnu11", "GNU11" }, { "gnu1x", "GNU11" },
      { "c17", "C17" }, { "c18", "C17" }, { "iso9899:2017", "C17" }, { "iso9899:2018", "C17" },
      { "gnu17", "GNU17" }, { "gnu18", "GNU17" },
      { "c2x", "C2x" }, { "c23", "C2x" }, { "iso9899:2024", "C2x" }, { "gnu2x", "GNU2x" }, { "gnu23", "GNU2x" },
    };
    auto iLanguage = languages.find(cmSystemTools::LowerCase(standard));
    return iLanguage != languages.end() ? iLanguage->second : std::string();
  }

  // A definition or an undefinition of a macro of a command line
  struct cmMalterlibDefine
  {
    std::string Define; // As for -D, or the name of the macro for -U
    bool bUndefine = false;
  };

  std::string defineMacro(cmMalterlibDefine const& define)
  {
    return define.bUndefine ? define.Define : macroName(define.Define);
  }

  // The definitions of a command line in the order that Clang applies them: those of the driver, then those that clang-cl
  // passes to the driver with /clang:, which it appends to the others, then those that the driver forwards to the
  // preprocessor with -Xpreprocessor and -Wp, and last those that it forwards to the compiler with -Xclang. Within a stage
  // they are in the order of the command line.
  struct cmMalterlibDefines
  {
    enum
    {
      Driver,
      DriverAppended,
      Preprocessor,
      Compiler,
      StageCount
    };

    std::vector<cmMalterlibDefine> Stages[StageCount];
  };

  void applyDefine(std::set<std::string>& defines, cmMalterlibDefine const& define)
  {
    if (!define.bUndefine) {
      setDefine(defines, define.Define);
      return;
    }
    for (auto iDefine = defines.begin(); iDefine != defines.end();) {
      if (macroName(*iDefine) == define.Define)
        iDefine = defines.erase(iDefine);
      else
        ++iDefine;
    }
  }

  // Only compilers with the command line of MSVC, such as clang-cl, take /D, as for others it can start a path
  bool usesMSVCOptions(cmLocalGenerator* lg, std::string const& language)
  {
    return lg->GetMakefile()->GetSafeDefinition(cmStrCat("CMAKE_", language, "_COMPILER_FRONTEND_VARIANT")) == "MSVC";
  }

  void applyDefines(std::set<std::string>& defines, cmMalterlibDefines const& stagedDefines)
  {
    for (auto const& stage : stagedDefines.Stages) {
      for (cmMalterlibDefine const& define : stage)
        applyDefine(defines, define);
    }
  }

  // The architecture of a triple of Clang by one name for the spellings that are the same architecture to it. Other
  // architectures only match their own spelling.
  std::string clangArchitecture(std::string const& architecture)
  {
    if (architecture == "arm64" || architecture == "aarch64")
      return "aarch64";
    if (architecture == "arm64_32" || architecture == "aarch64_32")
      return "aarch64_32";
    if (architecture == "x86_64" || architecture == "x86_64h" || architecture == "amd64")
      return "x86_64";
    if (architecture.size() == 4 && architecture[0] == 'i' && architecture[1] >= '3' && architecture[1] <= '9' &&
        architecture[2] == '8' && architecture[3] == '6') {
      return "i386";
    }
    return architecture;
  }

  // The architecture that the compiler targets, of CMAKE_<LANG>_COMPILER_ARCHITECTURE_ID, as Clang names it. Only for the
  // MSVC ABI CMake has names of its own.
  std::string compilerArchitecture(std::string const& architectureId)
  {
    if (architectureId == "ARM64")
      return "aarch64";
    if (architectureId == "ARM64EC")
      return "arm64ec";
    if (architectureId == "x64")
      return "x86_64";
    if (architectureId == "X86")
      return "i386";
    return clangArchitecture(architectureId);
  }

  // Whether the driver passes on the option of -Xarch_<selector>: for the host, which is all that imports compile for, and
  // for a selector that is a triple with the architecture that the driver compiles for. arm64e and arm64ec are
  // architectures of their own.
  bool xarchApplies(cmLocalGenerator* lg, std::string const& language, std::string const& selector)
  {
    if (selector == "host")
      return true;
    if (selector == "device")
      return false;
    std::string const selected = clangArchitecture(selector.substr(0, selector.find('-')));
    cmList const architectures{ lg->GetMakefile()->GetSafeDefinition(cmStrCat("CMAKE_", language, "_COMPILER_ARCHITECTURE_ID")) };
    for (std::string const& architecture : architectures) {
      if (compilerArchitecture(architecture) == selected)
        return true;
    }
    return false;
  }

  // The definitions and undefinitions of compile options, in the order that the compiler applies them, and the C standard
  // that it uses. Only they carry over: the Malterlib build sets the other compiler options itself. Each option is an
  // argument of its own, so a definition can contain spaces. The driver passes the standard of -Xpreprocessor and -Wp, to
  // the compiler before its own, so for the standard the last of the driver wins over those, and one of -Xclang over all.
  cmMalterlibDefines definesFromOptions(cmLocalGenerator* lg, std::string const& language,
                                        std::vector<std::string> const& options, std::string* pCStd = nullptr)
  {
    bool const bMSVCOptions = usesMSVCOptions(lg, language);

    // A forwarded -D or -U takes its macro from the next option forwarded to the same stage. -Wp, forwards each of its
    // comma separated arguments. The options of /clang: are those of the driver, appended to the others, and forward in turn.
    std::vector<std::string> stageOptions[cmMalterlibDefines::StageCount];
    std::vector<std::string> appendedOptions;
    auto fForwardToPreprocessor = [&](std::string const& _Option) {
      size_t iStart = 4;
      while (true) {
        size_t const iComma = _Option.find(',', iStart);
        stageOptions[cmMalterlibDefines::Preprocessor].push_back(_Option.substr(iStart, iComma - iStart));
        if (iComma == std::string::npos)
          break;
        iStart = iComma + 1;
      }
    };
    auto fDistribute = [&](std::vector<std::string> const& _Options, int _DriverStage) {
      // clang-cl only has the options of the driver of Clang in /clang:, and ignores them elsewhere: of those that forward
      // it only takes -Xclang itself
      bool const bMSVCDriver = _DriverStage == cmMalterlibDefines::Driver && bMSVCOptions;
      for (size_t iOption = 0; iOption < _Options.size(); ++iOption) {
        std::string const& option = _Options[iOption];
        if (bMSVCDriver && (cmHasLiteralPrefix(option, "-clang:") || cmHasLiteralPrefix(option, "/clang:"))) {
          appendedOptions.push_back(option.substr(7));
        } else if (!bMSVCDriver && cmHasLiteralPrefix(option, "-Xarch_") && iOption + 1 < _Options.size()) {
          // The option after -Xarch_ is one argument of the driver, so of those that forward only -Wp, can follow it
          std::string const& selected = _Options[++iOption];
          if (!xarchApplies(lg, language, option.substr(7)))
            continue;
          if (cmHasLiteralPrefix(selected, "-Wp,"))
            fForwardToPreprocessor(selected);
          else
            stageOptions[_DriverStage].push_back(selected);
        } else if (!bMSVCDriver && option == "-Xpreprocessor" && iOption + 1 < _Options.size())
          stageOptions[cmMalterlibDefines::Preprocessor].push_back(_Options[++iOption]);
        else if (option == "-Xclang" && iOption + 1 < _Options.size())
          stageOptions[cmMalterlibDefines::Compiler].push_back(_Options[++iOption]);
        else if (cmHasLiteralPrefix(option, "-Xclang="))
          stageOptions[cmMalterlibDefines::Compiler].push_back(option.substr(8));
        else if (!bMSVCDriver && cmHasLiteralPrefix(option, "-Wp,"))
          fForwardToPreprocessor(option);
        else
          stageOptions[_DriverStage].push_back(option);
      }
    };
    fDistribute(options, cmMalterlibDefines::Driver);
    fDistribute(appendedOptions, cmMalterlibDefines::DriverAppended);

    cmMalterlibDefines defines;
    std::string stageCStd[cmMalterlibDefines::StageCount];
    // Only the options of the driver of clang-cl are those of MSVC, where it takes the standard from /std: alone. The driver
    // of Clang also takes it from --std and from -ansi, which is C89, and the last of them applies. The preprocessor and the
    // compiler only take -std= and --std=.
    auto fParse = [&](std::vector<std::string> const& _Options, std::vector<cmMalterlibDefine>& o_Defines, std::string& o_CStd,
                      int _Stage) {
      bool const bMSVCDriver = bMSVCOptions && _Stage == cmMalterlibDefines::Driver;
      bool const bDriver = !bMSVCDriver && (_Stage == cmMalterlibDefines::Driver || _Stage == cmMalterlibDefines::DriverAppended);
      for (size_t iOption = 0; iOption < _Options.size(); ++iOption) {
        std::string const& option = _Options[iOption];
        if (!bMSVCDriver && (cmHasLiteralPrefix(option, "-std=") || cmHasLiteralPrefix(option, "--std="))) {
          o_CStd = option.substr(option.find('=') + 1);
          continue;
        }
        if (bDriver && option == "--std" && iOption + 1 < _Options.size()) {
          o_CStd = _Options[++iOption];
          continue;
        }
        if (bDriver && (option == "-ansi" || option == "--ansi")) {
          o_CStd = "c89";
          continue;
        }
        if (bMSVCDriver && (cmHasLiteralPrefix(option, "/std:") || cmHasLiteralPrefix(option, "-std:"))) {
          o_CStd = option.substr(5);
          if (o_CStd == "clatest")
            o_CStd = "c23";
          continue;
        }
        bool const bDefine = cmHasLiteralPrefix(option, "-D") || (bMSVCDriver && cmHasLiteralPrefix(option, "/D"));
        bool const bUndefine = cmHasLiteralPrefix(option, "-U") || (bMSVCDriver && cmHasLiteralPrefix(option, "/U"));
        if (!bDefine && !bUndefine)
          continue;
        if (option.size() > 2)
          o_Defines.push_back({ option.substr(2), bUndefine });
        else if (iOption + 1 < _Options.size())
          o_Defines.push_back({ _Options[++iOption], bUndefine });
      }
    };
    for (int stage = 0; stage < cmMalterlibDefines::StageCount; ++stage)
      fParse(stageOptions[stage], defines.Stages[stage], stageCStd[stage], stage);
    if (pCStd) {
      for (int stage : { cmMalterlibDefines::Preprocessor, cmMalterlibDefines::Driver, cmMalterlibDefines::DriverAppended,
                         cmMalterlibDefines::Compiler }) {
        if (!stageCStd[stage].empty())
          *pCStd = stageCStd[stage];
      }
    }
    return defines;
  }

  // Splits a command line for a POSIX shell into its words, without expanding anything. Within single quotes everything is
  // literal, and within double quotes a backslash only escapes $, `, ", \ and a newline.
  std::vector<std::string> splitPosixCommandLine(std::string const& commandLine)
  {
    std::vector<std::string> words;
    std::string word;
    bool bWord = false;
    for (size_t iChar = 0; iChar < commandLine.size(); ++iChar) {
      char const character = commandLine[iChar];
      if (character == ' ' || character == '\t' || character == '\n') {
        if (bWord)
          words.push_back(std::move(word));
        word.clear();
        bWord = false;
        continue;
      }
      bWord = true;
      if (character == '\'') {
        size_t const iEnd = commandLine.find('\'', iChar + 1);
        size_t const end = iEnd == std::string::npos ? commandLine.size() : iEnd;
        word.append(commandLine, iChar + 1, end - iChar - 1);
        iChar = end;
      } else if (character == '"') {
        for (++iChar; iChar < commandLine.size() && commandLine[iChar] != '"'; ++iChar) {
          char const quoted = commandLine[iChar];
          if (quoted == '\\' && iChar + 1 < commandLine.size()) {
            char const next = commandLine[iChar + 1];
            if (next == '$' || next == '`' || next == '"' || next == '\\' || next == '\n') {
              ++iChar;
              if (next != '\n')
                word += next;
              continue;
            }
          }
          word += quoted;
        }
      } else if (character == '\\' && iChar + 1 < commandLine.size()) {
        ++iChar;
        if (commandLine[iChar] != '\n')
          word += commandLine[iChar];
      } else {
        word += character;
      }
    }
    if (bWord)
      words.push_back(std::move(word));
    return words;
  }

  // Compile flags are a command line, quoted for the shell of the generator, in the build file of Ninja. Ninja removes its
  // escapes of $, spaces and colons before the shell sees the command line, as from the flags of targets that CMake escapes
  // and from the COMPILE_FLAGS of sources, which CMake writes as they are.
  std::vector<std::string> argumentsFromFlags(cmLocalGenerator* lg, std::string const& flags)
  {
    std::string commandLine;
    for (size_t iChar = 0; iChar < flags.size(); ++iChar) {
      if (flags[iChar] == '$' && iChar + 1 < flags.size() &&
          (flags[iChar + 1] == '$' || flags[iChar + 1] == ' ' || flags[iChar + 1] == ':')) {
        ++iChar;
      }
      commandLine += flags[iChar];
    }
    std::vector<std::string> arguments;
    if (lg->GetState()->UseWindowsShell())
      cmSystemTools::ParseWindowsCommandLine(commandLine.c_str(), arguments);
    else
      arguments = splitPosixCommandLine(commandLine);
    return arguments;
  }

  cmMalterlibDefines definesFromFlags(cmLocalGenerator* lg, std::string const& language, std::string const& flags,
                                      std::string* pCStd = nullptr)
  {
    return definesFromOptions(lg, language, argumentsFromFlags(lg, flags), pCStd);
  }

  // CMake passes the system include directories of a target as such only to the compilers that have a flag for them, as
  // NASM has not, and Malterlib only passes Compile.SystemSearchPath to those compilers
  bool usesSystemIncludeFlag(cmLocalGenerator* lg, std::string const& language)
  {
    return !lg->GetMakefile()->GetSafeDefinition(cmStrCat("CMAKE_INCLUDE_SYSTEM_FLAG_", language)).empty();
  }

  // On Apple platforms CMake passes a framework among the include directories as its directory, with -F, or -iframework for
  // a directory that the target marks as a system directory, and leaves out that of the frameworks of the system. Returns
  // false for an include directory that isn't a framework.
  bool frameworkSearchDirectory(cmLocalGenerator* lg, cmGeneratorTarget const* target, std::string const& config,
                                std::string const& language, std::string const& include, std::vector<std::string>& o_Directories,
                                std::vector<std::string>& o_SystemDirectories)
  {
    cmMakefile* makefile = lg->GetMakefile();
    if (!makefile->IsOn("APPLE") || makefile->GetSafeDefinition(cmStrCat("CMAKE_", language, "_FRAMEWORK_SEARCH_FLAG")).empty() ||
        !cmSystemTools::IsPathToFramework(include)) {
      return false;
    }
    std::string directory = cmSystemTools::GetFilenamePath(include);
    if (directory == "/System/Library/Frameworks")
      return true;
    bool const bSystem = !makefile->GetSafeDefinition(cmStrCat("CMAKE_", language, "_SYSTEM_FRAMEWORK_SEARCH_FLAG")).empty() &&
      target->IsSystemIncludeDirectory(directory, config, language);
    (bSystem ? o_SystemDirectories : o_Directories).push_back(std::move(directory));
    return true;
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

  struct cmMalterlibShellArgument
  {
    std::string Text; // As written, with its quoting
    std::string Value; // What the shell passes
    bool bRaw = false; // A shell operator, or an argument that the shell expands, which stays as written
    bool bSeparated = false; // Whether whitespace separates it from the token before, as in 2>&1 nothing does
  };

  // Splits a command line written for the shell into its arguments and operators. On Windows only double quotes quote, as
  // for cmd, which expands % variables.
  std::vector<cmMalterlibShellArgument> SplitShellArguments(std::string const& commandLine)
  {
#ifdef _WIN32
    char const* const operators = "&|<>()";
#else
    char const* const operators = ";&|<>()";
#endif
    auto fIsOperator = [&](char character) { return character != 0 && std::strchr(operators, character) != nullptr; };

    std::vector<cmMalterlibShellArgument> arguments;
    size_t iChar = 0;
    bool bSeparated = false;
    while (iChar < commandLine.size()) {
      char const first = commandLine[iChar];
      if (first == ' ' || first == '\t') {
        ++iChar;
        bSeparated = true;
        continue;
      }

      size_t const iStart = iChar;
      if (fIsOperator(first)) {
        while (iChar < commandLine.size() && fIsOperator(commandLine[iChar]))
          ++iChar;
        std::string text = commandLine.substr(iStart, iChar - iStart);
        arguments.push_back({ text, text, true, bSeparated });
        bSeparated = false;
        continue;
      }

      std::string value;
      bool bShellExpands = false;
      while (iChar < commandLine.size() && commandLine[iChar] != ' ' && commandLine[iChar] != '\t' &&
             !fIsOperator(commandLine[iChar])) {
        char const character = commandLine[iChar];
#ifdef _WIN32
        if (character == '%')
          bShellExpands = true;
#else
        if (character == '\\' && iChar + 1 < commandLine.size()) {
          value += commandLine[iChar + 1];
          iChar += 2;
          continue;
        }
        if (character == '\'') {
          size_t const iEnd = commandLine.find('\'', iChar + 1);
          size_t const iValueEnd = iEnd == std::string::npos ? commandLine.size() : iEnd;
          value += commandLine.substr(iChar + 1, iValueEnd - iChar - 1);
          iChar = iEnd == std::string::npos ? commandLine.size() : iEnd + 1;
          continue;
        }
        // Unquoted, a glob is expanded too
        if (character == '$' || character == '`' || character == '*' || character == '?' || character == '[')
          bShellExpands = true;
#endif
        if (character == '"') {
          ++iChar;
          while (iChar < commandLine.size() && commandLine[iChar] != '"') {
#ifdef _WIN32
            if (commandLine[iChar] == '%')
              bShellExpands = true;
#else
            if (commandLine[iChar] == '\\' && iChar + 1 < commandLine.size() &&
                std::strchr("\"\\$`", commandLine[iChar + 1])) {
              ++iChar;
            } else if (commandLine[iChar] == '$' || commandLine[iChar] == '`') {
              bShellExpands = true;
            }
#endif
            value += commandLine[iChar];
            ++iChar;
          }
          if (iChar < commandLine.size())
            ++iChar;
          continue;
        }
        value += character;
        ++iChar;
      }

      arguments.push_back({ commandLine.substr(iStart, iChar - iStart), std::move(value), bShellExpands, bSeparated });
      bSeparated = false;
    }
    return arguments;
  }

  struct cmMalterlibCompileTypeInfo
  {
    std::vector<std::string> Includes;
    std::vector<std::string> SystemIncludes; // Searched after Includes, as with -isystem
    std::vector<std::string> FrameworkDirectories;
    std::vector<std::string> SystemFrameworkDirectories;
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
      std::vector<std::string> includes;
      lg->GetIncludeDirectories(includes, target, language, configName);
      bool const bSystemIncludes = usesSystemIncludeFlag(lg, language);
      for (std::string& include : includes) {
        if (frameworkSearchDirectory(lg, target, configName, language, include, info.FrameworkDirectories,
                                     info.SystemFrameworkDirectories)) {
          continue;
        }
        bool const bSystem = bSystemIncludes && target->IsSystemIncludeDirectory(include, configName, language);
        auto& toIncludes = bSystem ? info.SystemIncludes : info.Includes;
        toIncludes.push_back(std::move(include));
      }
      lg->GetTargetDefines(target, configName, language, info.Defines);
      // With the architecture, as the overload without it returns the flags. The flags come after the definitions.
      std::string compileFlags;
      lg->GetTargetCompileFlags(const_cast<cmGeneratorTarget* >(target),
                                configName, language, compileFlags, std::string());
      applyDefines(info.Defines, definesFromFlags(lg, language, compileFlags, &info.cStd));
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
  cmSystemTools::GetEnv("CMAKE_MALTERLIB_TEMPDIR", tempDir);
  cmSystemTools::GetEnv("CMAKE_MALTERLIB_BASEDIR", baseDir);

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
  if (tempDir.empty() || baseDir.empty()) {
    cmSystemTools::Error("The Malterlib generator needs CMAKE_MALTERLIB_TEMPDIR and CMAKE_MALTERLIB_BASEDIR in the "
                         "environment, which MTool sets when it imports a project");
    return;
  }
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

// How an evaluated path in an eval string ends, where it escapes itself for the host
static char const* const escapeHostSuffix = "->EscapeHost())";

static std::string makeAbsoluteWrapperEvalString(std::string const &fileName) {
  if (isDynamic(fileName))
    return fileName;

  return cmStrCat("@(", cmMalterlibRegistry::getEscaped(fileName, true, true), "->MakeAbsolute()", escapeHostSuffix);
}

// Escapes for the shell only: the Malterlib build system escapes for its generators itself, as for Ninja
static std::string escapeForShell(cmLocalGenerator *localGenerator, std::string const &argument) {
  return cmOutputConverter::EscapeForShell(
    argument, localGenerator->GetState()->UseWindowsShell() ? 0 : cmOutputConverter::Shell_Flag_IsUnix);
}

// As cmCustomCommandGenerator escapes the arguments of a command that is not VERBATIM: only spaces
static std::string escapeForShellOldStyle(std::string const &argument) {
#if defined(_WIN32) && !defined(__CYGWIN__)
  if (argument.find(' ') != std::string::npos && argument.find('"') == std::string::npos)
    return cmStrCat('"', argument, '"');
  return argument;
#else
  std::string escaped;
  for (char character : argument) {
    if (character == ' ')
      escaped += '\\';
    escaped += character;
  }
  return escaped;
#endif
}

// An argument with evaluated paths stays one argument for the shell, as its literal parts are escaped, as the other arguments
// of the command are. The evaluated paths escape themselves, with EscapeHost.
static std::string escapeDynamicArgument(cmLocalGenerator *localGenerator, std::string const &argument, bool bOldStyle) {
  std::string escaped;
  // An empty argument stays an argument, as CMake quotes it, except in the old style
  if (argument.empty()) {
    if (!bOldStyle)
      cmMalterlibRegistry::addEscapeStr(escaped, escapeForShell(localGenerator, argument), "`\\\r\n\t", "`\\rnt", false);
    return escaped;
  }

  struct cmMalterlibArgumentPart
  {
    std::string Text;
    bool bExpression;
  };
  std::vector<cmMalterlibArgumentPart> parts;
  std::string literal;
  auto fAddLiteral = [&]() {
    if (!literal.empty())
      parts.push_back({ std::move(literal), false });
    literal.clear();
  };

  size_t iChar = 0;
  while (iChar < argument.size()) {
    if (argument[iChar] == '@' && iChar + 1 < argument.size() && argument[iChar + 1] == '@') {
      literal += "@@";
      iChar += 2;
      continue;
    }
    if (argument[iChar] != '@' || iChar + 1 >= argument.size() || argument[iChar + 1] != '(') {
      literal += argument[iChar++];
      continue;
    }

    fAddLiteral();
    size_t const iStart = iChar;
    int depth = 0;
    bool bInString = false;
    for (iChar += 1; iChar < argument.size(); ++iChar) {
      char const character = argument[iChar];
      if (bInString) {
        if (character == '\\')
          ++iChar;
        else if (character == '"')
          bInString = false;
      } else if (character == '"')
        bInString = true;
      else if (character == '(')
        ++depth;
      else if (character == ')' && --depth == 0) {
        ++iChar;
        break;
      }
    }
    parts.push_back({ argument.substr(iStart, iChar - iStart), true });
  }
  fAddLiteral();

  auto fAddEvalString = [&](std::string const& _Text) {
    cmMalterlibRegistry::addEscapeStr(escaped, _Text, "`\\\r\n\t", "`\\rnt", false);
  };

  // A Windows command line can't join quoted parts: a quote that follows a closing quote is a quote of the argument. So an
  // argument of paths and literal parts is quoted as a whole there, once its paths are known.
  if (parts.size() > 1 && localGenerator->GetState()->UseWindowsShell()) {
    bool bWhole = true;
    bool bLiteralQuote = false;
    for (cmMalterlibArgumentPart const& part : parts) {
      if (part.bExpression)
        bWhole = bWhole && cmHasSuffix(part.Text, escapeHostSuffix);
      else
        bLiteralQuote = bLiteralQuote || part.Text.find('"') != std::string::npos;
    }
    if (bWhole) {
      // CMake leaves an argument of the old style with a quote as it is
      bool const bAsIs = bOldStyle && bLiteralQuote;
      if (!bAsIs)
        escaped += "@(`";
      for (cmMalterlibArgumentPart const& part : parts) {
        // The paths without their own escape
        if (part.bExpression)
          escaped += cmStrCat(cm::string_view(part.Text).substr(0, part.Text.size() - strlen(escapeHostSuffix)), ')');
        else
          fAddEvalString(part.Text);
      }
      if (!bAsIs)
        escaped += cmStrCat('`', escapeHostSuffix);
      return escaped;
    }
  }

  for (cmMalterlibArgumentPart const& part : parts) {
    if (part.bExpression)
      escaped += part.Text;
    else
      fAddEvalString(bOldStyle ? escapeForShellOldStyle(part.Text) : escapeForShell(localGenerator, part.Text));
  }

  return escaped;
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

  auto &child = registry.addChild("Property.CMakeOutputPath_" + makeIdentifier(projectName) + ":",
                    " string = " + makeAbsoluteWrapper(lgs[0]->GetBinaryDirectory()));

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
        case cm::TargetType::GLOBAL_TARGET:
          break;
        case cm::TargetType::UTILITY:
        case cm::TargetType::INTERFACE_LIBRARY:
          // Add all utility targets, except the Nightly/Continuous/
          // Experimental-"sub"targets as e.g. NightlyStart. An interface
          // library can have such a name and be a dependency of a target.
          if (target->GetType() == cm::TargetType::UTILITY &&
              (((targetName.find("Nightly") == 0) &&
                (targetName != "Nightly")) ||
               ((targetName.find("Continuous") == 0) &&
                (targetName != "Continuous")) ||
               ((targetName.find("Experimental") == 0) &&
                (targetName != "Experimental")))) {
            break;
          }

          this->CollectOutputFilesFromTarget(_ProjectName, *lg, target.get(), make.c_str(),
                             makefile, compiler.c_str(),
                             false);
          break;
        case cm::TargetType::EXECUTABLE:
        case cm::TargetType::STATIC_LIBRARY:
        case cm::TargetType::SHARED_LIBRARY:
        case cm::TargetType::MODULE_LIBRARY:
        case cm::TargetType::OBJECT_LIBRARY:
        {
          this->CollectOutputFilesFromTarget(_ProjectName, *lg, target.get(), make.c_str(),
                             makefile, compiler.c_str(),
                             false);
        } break;
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
        case cm::TargetType::GLOBAL_TARGET:
          break;
        case cm::TargetType::UTILITY:
        case cm::TargetType::INTERFACE_LIBRARY:
          // Add all utility targets, except the Nightly/Continuous/
          // Experimental-"sub"targets as e.g. NightlyStart. An interface
          // library can have such a name and be a dependency of a target.
          if (target->GetType() == cm::TargetType::UTILITY &&
              (((targetName.find("Nightly") == 0) &&
                (targetName != "Nightly")) ||
               ((targetName.find("Continuous") == 0) &&
                (targetName != "Continuous")) ||
               ((targetName.find("Experimental") == 0) &&
                (targetName != "Experimental")))) {
            break;
          }

          this->AppendTarget(_ProjectName, registry, *lg, target.get(), make.c_str(),
                             makefile, compiler.c_str(),
                             false);
          break;
        case cm::TargetType::EXECUTABLE:
        case cm::TargetType::STATIC_LIBRARY:
        case cm::TargetType::SHARED_LIBRARY:
        case cm::TargetType::MODULE_LIBRARY:
        case cm::TargetType::OBJECT_LIBRARY:
        {
          this->AppendTarget(_ProjectName, registry, *lg, target.get(), make.c_str(),
                             makefile, compiler.c_str(),
                             false);
        } break;
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
  bool bHidden = false;
  for (auto &prefix : HidePrefixes){
    if (StringStartsWithPath(strippedFileName.c_str(), prefix.c_str())) {
      if (bFirstPrefix)
        bProtectGroups = true;
      strippedFileName = strippedFileName.substr(prefix.size()+1);
      bHidden = true;
      break;
    }
    bFirstPrefix = false;
  }
  // A source outside the hidden directories, as of a project whose CMakeLists.txt is in a subdirectory, is grouped by its path
  // in the checkout, so that the groups don't depend on where the checkout of the host that generated the cache is
  if (!bHidden && !baseDir.empty() && StringStartsWithPath(strippedFileName.c_str(), baseDir.c_str()) &&
      strippedFileName.size() > baseDir.size() && strippedFileName[baseDir.size()] == '/') {
    strippedFileName = strippedFileName.substr(baseDir.size() + 1);
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
      if (CustomCommandIsInDependency(lg, target, customCommand))
        continue;

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
  auto property_value = localGenerator->GetMakefile()->GetProperty("RULE_LAUNCH_CUSTOM");

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

  // The launcher reads all outputs from this file, as a comma separated list that no shell reads. The paths are absolute, so
  // that import caches replace them with the #(CMakeIntermediateDirectory) markers that MalterlibImportReplacePaths expands
  std::string outputList = cmJoin(outputs, ",");
  // Not in CMakeFiles, as cmCustomCommandGenerator::StoreContentToFile stores it, which import caches leave out
  std::string filePathWithOutput;
  if (property_value->find("<OUTPUT_STORE_TO_FILE>") != std::string::npos) {
    std::string const fileDir = cmStrCat(localGenerator->GetBinaryDirectory(), "/MalterlibLauncherOutputs");
    if (cmSystemTools::MakeDirectory(fileDir)) {
      // The name is the same for every directory the import generates in, as the contents are once cached
      std::string nameContents = outputList;
      cmSystemTools::ReplaceString(nameContents, tempDir, "<TempDir>");
      cmSystemTools::ReplaceString(nameContents, baseDir, "<BaseDir>");
      cmCryptoHash hash(cmCryptoHash::AlgoSHA256);
      filePathWithOutput = cmStrCat(fileDir, '/', hash.HashString(nameContents));
      cmGeneratedFileStream file(filePathWithOutput);
      file << outputList;
      // Close doesn't report a failure to move the file in place, as when a directory has its name, so it is read back
      bool bWritten = file.Close() && !cmSystemTools::FileIsDirectory(filePathWithOutput);
      if (bWritten) {
        cmsys::ifstream written(filePathWithOutput.c_str(), std::ios::in | std::ios::binary);
        std::string const contents{ std::istreambuf_iterator<char>(written), std::istreambuf_iterator<char>() };
        bWritten = written.is_open() && contents == outputList;
      }
      if (!bWritten) {
        localGenerator->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          cmStrCat("Cannot write the outputs of a launcher to: ", filePathWithOutput));
      }
    } else {
      localGenerator->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
        cmStrCat("Cannot create the directory of the outputs of launchers: ", fileDir));
    }
  }
  vars.FilePathWithOutput = filePathWithOutput.c_str();

  vars.Role = ccg.GetCC().GetRole().c_str();
  vars.CMTargetName = ccg.GetCC().GetTarget().c_str();
  vars.Config = ccg.GetOutputConfig().c_str();

  std::unique_ptr<cmRulePlaceholderExpander> rulePlaceholderExpander(localGenerator->CreateRulePlaceholderExpander());

  std::string expandedLauncher = *property_value;
  rulePlaceholderExpander->ExpandRuleVariables(localGenerator, expandedLauncher, vars);

  // Each argument converts as the arguments of the command do, so that the paths in the launcher, such as the ctest command
  // and the outputs, map the same way. Arguments without paths to map keep their shell syntax, such as of variable assignments
  std::string launcher;
  for (cmMalterlibShellArgument const& argument : SplitShellArguments(expandedLauncher)) {
    if (argument.bSeparated && !launcher.empty())
      launcher += " ";
    // An assignment of a variable, as of the environment of the command, keeps its unquoted name and = for the shell, and
    // only its value is mapped
    std::string assignment;
    std::string value = argument.Value;
    if (!argument.bRaw && !localGenerator->GetState()->UseWindowsShell()) {
      cmsys::RegularExpression assignmentExpression("^[A-Za-z_][A-Za-z0-9_]*=");
      if (assignmentExpression.find(argument.Text)) {
        assignment = assignmentExpression.match(0);
        value = value.substr(assignment.size());
      }
    }
    // The value of an argument that the shell expands isn't a path to map
    std::string converted = argument.bRaw ? std::string() : ConvertCommandParam(_ProjectName, localGenerator, value);
    std::string unconverted = value;
    cmSystemTools::ReplaceString(unconverted, "@", "@@");
    if (argument.bRaw || converted == unconverted) {
      std::string text = argument.Text;
      cmSystemTools::ReplaceString(text, "@", "@@");
      cmMalterlibRegistry::addEscapeStr(launcher, text, "`\\\r\n\t", "`\\rnt", false);
      continue;
    }
    cmMalterlibRegistry::addEscapeStr(launcher, assignment, "`\\\r\n\t", "`\\rnt", false);
    if (isDynamic(converted))
      launcher += escapeDynamicArgument(localGenerator, converted, false);
    else
      cmMalterlibRegistry::addEscapeStr(launcher, escapeForShell(localGenerator, converted), "`\\\r\n\t", "`\\rnt", false);
  }
  if (!launcher.empty())
    launcher += " ";

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

bool cmExtraMalterlibGenerator::CustomCommandIsInDependency(cmLocalGenerator *lg, const cmGeneratorTarget* target, cmCustomCommand* customCommand) {
  cmLocalNinjaGenerator *ninjaGenerator = static_cast<cmLocalNinjaGenerator *>(lg);
  auto *customCommandTargets = ninjaGenerator->GetTargetsForCustomCommand(customCommand);
  if (customCommandTargets) {
    cmTargetDependSet const& targetDependencies = const_cast<cmGlobalGenerator*>(GlobalGenerator)->GetTargetDirectDepends(target);
    for (auto &dependency : targetDependencies) {
      for (auto &target : *customCommandTargets) {
        cmGeneratorTarget const *customCommandTarget = target;
        cmGeneratorTarget const *dependencyTarget = dependency;
        if (customCommandTarget == dependencyTarget) {
          return true;
        }
      }
    }
  }

  return false;
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

  // What sources compile with by the definitions and compile flags of the target, per language
  struct cmMalterlibTargetOptions
  {
    std::vector<std::string> Arguments; // Of the compile flags
    std::set<std::string> CompileDefinitions;
    std::set<std::string> Defines; // CompileDefinitions with those of Arguments
    std::string CStd;
  };
  std::map<std::string, cmMalterlibTargetOptions> targetOptionsCache;
  auto fTargetOptions = [&](std::string const& language) -> cmMalterlibTargetOptions const& {
    auto iOptions = targetOptionsCache.find(language);
    if (iOptions != targetOptionsCache.end())
      return iOptions->second;
    cmMalterlibTargetOptions& options = targetOptionsCache[language];
    std::string flags;
    lg->GetTargetCompileFlags(const_cast<cmGeneratorTarget*>(target), configName, language, flags, std::string());
    options.Arguments = argumentsFromFlags(lg, flags);
    lg->GetTargetDefines(target, configName, language, options.CompileDefinitions);
    options.Defines = options.CompileDefinitions;
    applyDefines(options.Defines, definesFromOptions(lg, language, options.Arguments, &options.CStd));
    return options;
  };

  for (auto &file : sourceFiles) {
    if (!file->GetObjectLibrary().empty())
      continue;

    auto *customCommand = file->GetCustomCommand();

    std::string fullPath = file->GetFullPath();

    bool bIsGenerated = file->GetIsGenerated();

    // The file set of a source can override its language
    auto Language = target->GetSourceFileLanguage(file, configName);

    std::string malterlibType = getMalterlibCompileType(Language, lg);

    if (file->GetPropertyAsBool("HEADER_FILE_ONLY"))
      malterlibType = "Header";

    std::string keys;
    for (auto &key : file->GetProperties().GetKeys()) {
      keys += " ";
      keys += key;
    }

    if (customCommand) {
      if (CustomCommandIsInDependency(lg, target, customCommand))
        continue;

      auto &outFile = AddFileInGroup(_ProjectName, registry, fullPath);
      cmCustomCommandGenerator customCommandGenerator(*customCommand, configName, lg);

      cmMalterlibRegistry *pOutCompile = nullptr;
      {
        std::string launcher = this->MakeCustomLauncher(_ProjectName, lg, customCommandGenerator);

        std::string commandLines;

        for (unsigned i = 0; i != customCommandGenerator.GetNumberOfCommands(); ++i)
        {
          auto command = ConvertCommandParam(_ProjectName, lg, customCommandGenerator.GetCommand(i));
          std::string commandLine = launcher;

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
                // Escaped here for the shell only, as the Malterlib build system escapes for Ninja itself
                o_bEscape = false;
                return escapeDynamicArgument(
                  lg, ConvertCommandParam(_ProjectName, lg, _Param), customCommandGenerator.GetCC().GetEscapeOldStyle());
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

        pOutCompile->addChild("Type", "Custom");
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
                                    realDependency,
                                    customCommandGenerator.GetCC().GetCMP0212Status())) {
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
    else
    {
      auto &outFile = AddFileInGroup(_ProjectName, registry, fullPath);
      if (bIsGenerated) {
        outFile.addChild("Compile.AllowNonExisting", "true").RawValue = true;
        if (!malterlibType.empty())
          outFile.addChild("Compile.Type", malterlibType);
      }
      else if (!malterlibType.empty())
        outFile.addChild("Compile.Type", malterlibType);
      else if (!Language.empty())
        outFile.addChild("Compile.Type", "None");
      else
        outFile.addChild("Compile.Disabled", "true").RawValue = true;

      const std::string config = configName;
      cmGeneratorExpressionInterpreter genexInterpreter(
        lg, config, target, Language);

      // Sources in file sets also get the definitions, include directories and compile options of their file set, the
      // interface ones when the file set comes from another target. Of the options, as of COMPILE_FLAGS, only the
      // definitions are used.
      cmGeneratorFileSet const* fileSet = target->GetFileSetForSource(config, file);

      // CMake sorts the definitions of the file set and of the source together, after those of the target, so the last of a
      // macro in that order is used
      std::set<std::string> sortedDefines;
      if (fileSet) {
        auto fileSetDefines = fileSet->BelongsTo(target)
          ? fileSet->GetCompileDefinitions(config, Language)
          : fileSet->GetInterfaceCompileDefinitions(config, Language);
        if (!fileSetDefines.empty())
          lg->AppendDefines(sortedDefines, fileSetDefines);
      }

      const std::string COMPILE_DEFINITIONS("COMPILE_DEFINITIONS");
      if (auto compile_defs = file->GetProperty(COMPILE_DEFINITIONS))
        lg->AppendDefines(sortedDefines, genexInterpreter.Evaluate(*compile_defs, COMPILE_DEFINITIONS));

      std::string defPropName = "COMPILE_DEFINITIONS_";
      defPropName += cmSystemTools::UpperCase(config);
      if (auto config_compile_defs = file->GetProperty(defPropName))
        lg->AppendDefines(sortedDefines, genexInterpreter.Evaluate(*config_compile_defs, COMPILE_DEFINITIONS));

      // The options of the source and of its file set follow those of the target on the command line, which the compiler
      // reads as a whole: an option can take its operand from the next property, and its stages apply across them.
      std::vector<std::string> fileArguments;
      const std::string COMPILE_FLAGS("COMPILE_FLAGS");
      if (auto cflags = file->GetProperty(COMPILE_FLAGS)) {
        for (std::string& argument : argumentsFromFlags(lg, genexInterpreter.Evaluate(*cflags, COMPILE_FLAGS)))
          fileArguments.push_back(std::move(argument));
      }

      const std::string COMPILE_OPTIONS("COMPILE_OPTIONS");
      if (auto sourceOptions = file->GetProperty(COMPILE_OPTIONS)) {
        for (std::string const& argument : cmList{ genexInterpreter.Evaluate(*sourceOptions, COMPILE_OPTIONS) })
          fileArguments.push_back(argument);
      }

      if (fileSet) {
        auto fileSetOptions = fileSet->BelongsTo(target)
          ? fileSet->GetCompileOptions(config, Language)
          : fileSet->GetInterfaceCompileOptions(config, Language);
        for (std::string& argument : cm::remove_BT(fileSetOptions))
          fileArguments.push_back(std::move(argument));
      }

      // AppendTarget sets the definitions of the sources once it has those of the target
      bool const bCompiled = !Language.empty() && !malterlibType.empty() && malterlibType == getMalterlibCompileType(Language, lg);
      if (bCompiled && sortedDefines.empty() && fileArguments.empty())
        SourceDefines.push_back({ &outFile, malterlibType, fTargetOptions(Language).Defines });
      else if (!sortedDefines.empty() || !fileArguments.empty()) {
        cmMalterlibTargetOptions const& targetOptions = fTargetOptions(Language);
        std::vector<std::string> arguments = targetOptions.Arguments;
        arguments.insert(arguments.end(), fileArguments.begin(), fileArguments.end());

        std::string fileCStd;
        std::set<std::string> fileDefines = targetOptions.CompileDefinitions;
        for (std::string const& define : sortedDefines)
          setDefine(fileDefines, define);
        applyDefines(fileDefines, definesFromOptions(lg, Language, arguments, &fileCStd));
        if (bCompiled)
          SourceDefines.push_back({ &outFile, malterlibType, std::move(fileDefines) });

        if (fileCStd != targetOptions.CStd && !fileCStd.empty() && malterlibType == "C") {
          std::string const cLanguage = malterlibCLanguage(fileCStd);
          if (!cLanguage.empty()) {
            outFile.addChild("Compile.CLanguage", cLanguage);
          } else {
            lg->GetMakefile()->IssueMessage(MessageType::WARNING,
              cmStrCat("Source \"", file->GetFullPath(), "\" uses -std=", fileCStd,
                       ", which has no Malterlib C language. It is left to Malterlib."));
          }
        }
      }

      // The include directories of the file set come before those of the source, as for CMake. Those that the target marks as
      // system directories are searched after the others, also by a compiler that has no flag for them.
      {
        std::vector<std::string> includes;
        std::vector<std::string> systemIncludes;
        std::vector<std::string> frameworkDirectories;
        std::vector<std::string> systemFrameworkDirectories;
        auto fAddInclude = [&](std::string&& _Include) {
          if (frameworkSearchDirectory(lg, target, config, Language, _Include, frameworkDirectories, systemFrameworkDirectories))
            return;
          bool const bSystem = target->IsSystemIncludeDirectory(_Include, config, Language);
          auto& toIncludes = bSystem ? systemIncludes : includes;
          toIncludes.push_back(std::move(_Include));
        };
        if (fileSet) {
          auto fileSetIncludes = fileSet->BelongsTo(target)
            ? fileSet->GetIncludeDirectories(config, Language)
            : fileSet->GetInterfaceIncludeDirectories(config, Language);
          for (auto &include : cm::remove_BT(fileSetIncludes))
            fAddInclude(cmSystemTools::CollapseFullPath(include));
        }
        const std::string INCLUDE_DIRECTORIES("INCLUDE_DIRECTORIES");
        if (auto sourceIncludes = file->GetProperty(INCLUDE_DIRECTORIES)) {
          for (std::string const& include : cmList{ genexInterpreter.Evaluate(*sourceIncludes, INCLUDE_DIRECTORIES) })
            fAddInclude(cmSystemTools::CollapseFullPath(include, lg->GetCurrentSourceDirectory()));
        }
        if (!usesSystemIncludeFlag(lg, Language)) {
          includes.insert(includes.end(), std::make_move_iterator(systemIncludes.begin()),
                          std::make_move_iterator(systemIncludes.end()));
          systemIncludes.clear();
        }
        if (!includes.empty()) {
          auto end = cmRemoveDuplicates(includes);
          includes.erase(end, includes.end());
          for (auto &include : includes)
            include = makeAbsoluteWrapper(include);
          outFile.addChild("Compile.SearchPath", "+= [" + cmJoin(includes, ", ") + "]").RawValue = true;
        }
        if (!systemIncludes.empty()) {
          auto end = cmRemoveDuplicates(systemIncludes);
          systemIncludes.erase(end, systemIncludes.end());
          for (auto &include : systemIncludes)
            include = makeAbsoluteWrapper(include);
          outFile.addChild("Compile.SystemSearchPath", "+= [" + cmJoin(systemIncludes, ", ") + "]").RawValue = true;
        }
        auto fAddFrameworkDirectories = [&](std::vector<std::string>& _Directories, char const* _pKey) {
          if (_Directories.empty())
            return;
          auto end = cmRemoveDuplicates(_Directories);
          _Directories.erase(end, _Directories.end());
          for (auto &directory : _Directories)
            directory = makeAbsoluteWrapper(directory);
          outFile.addChild(_pKey, "+= [" + cmJoin(_Directories, ", ") + "]").RawValue = true;
        };
        fAddFrameworkDirectories(frameworkDirectories, "Compile.FrameworkSearchPath");
        fAddFrameworkDirectories(systemFrameworkDirectories, "Compile.SystemFrameworkSearchPath");
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
  if (target->GetType() == cm::TargetType::UTILITY ||
      target->GetType() == cm::TargetType::INTERFACE_LIBRARY ||
      target->GetType() == cm::TargetType::GLOBAL_TARGET) {
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

/*  cmTargetDependSet const& targetDependencies =
    const_cast<cmGlobalGenerator*>(GlobalGenerator)->
    GetTargetDirectDepends(target);

  for (auto &dependency : targetDependencies) {
    auto dependencyLocalGenerator = dependency->GetLocalGenerator();
    if (dependency->GetName() == "global_target" || dependency->GetType() == cm::TargetType::INTERFACE_LIBRARY) {
      continue;
    }

    if (dependency->GetType() == cm::TargetType::OBJECT_LIBRARY) {
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
  }*/
}

// The libraries that a target links, directly and through their link interfaces
static std::set<cmGeneratorTarget const*> getLinkClosure(cmGeneratorTarget const* target, std::string const& configName)
{
  std::set<cmGeneratorTarget const*> closure;
  std::vector<cmGeneratorTarget const*> pending;
  if (cmLinkImplementation const* impl = target->GetLinkImplementation(configName, cmGeneratorTarget::UseTo::Link)) {
    for (cmLinkItem const& lib : impl->Libraries) {
      if (lib.Target)
        pending.push_back(lib.Target);
    }
  }
  while (!pending.empty()) {
    cmGeneratorTarget const* linked = pending.back();
    pending.pop_back();
    if (!closure.insert(linked).second)
      continue;
    if (cmLinkInterfaceLibraries const* iface = linked->GetLinkInterfaceLibraries(configName, target, cmGeneratorTarget::UseTo::Link)) {
      for (cmLinkItem const& lib : iface->Libraries) {
        if (lib.Target)
          pending.push_back(lib.Target);
      }
    }
  }
  return closure;
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
  if (target->GetType() == cm::TargetType::UTILITY ||
      target->GetType() == cm::TargetType::INTERFACE_LIBRARY ||
      target->GetType() == cm::TargetType::GLOBAL_TARGET) {
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
  if (IsObjectLib(target))
    outputTarget.addChild("Target.ObjectLibrary", "true").RawValue = true;

  std::map<std::string, cmMalterlibCompileTypeInfo> compileTypeInfo;
  AddTargetCompileInfo(compileTypeInfo, target, lg, configName);

  if (!isUtilityTarget) {
    AddTargetCompileInfo(compileTypeInfo, target, lg, configName);
    std::vector<cmSourceFile*> sourceFiles;
    GetTargetFiles(sourceFiles, lg, target, makefile);
    SourceDefines.clear();
    AddFilesToRegistry(_ProjectName, outputTarget, sourceFiles, configName, lg, target, false);
  } else {
    std::vector<cmSourceFile*> sourceFiles;
    GetTargetFiles(sourceFiles, lg, target, makefile);
    AddFilesToRegistry(_ProjectName, outputTarget, sourceFiles, configName, lg, target, true);
  }

  // The targets linked into this one. Libraries guarded by $<COMPILE_ONLY:...> only order the build, but their
  // dependency edges are link edges too.
  std::set<cmGeneratorTarget const*> linkedTargets;
  // Objects named by $<TARGET_OBJECTS:...> are linked, even when their library is also compile only.
  std::map<cmGeneratorTarget const*, std::set<std::string>> linkedObjects;
  {
    bool const bStaticLibrary = target->GetType() == cm::TargetType::STATIC_LIBRARY;
    // The object library of the import that builds an object, or null for the object of an imported object library. Those
    // aren't built by a target of the import, which a dependency could link, so they aren't supported. Built object
    // libraries are global, and an imported one of the same name in the directory of the target, or of an interface that
    // passes on the objects, doesn't hide them: the object has the path that CMake resolved, which is in the object
    // directory of the library that builds it.
    auto fBuiltObjectLibrary = [&](cmSourceFile const* objectFile) -> cmGeneratorTarget* {
      cmGeneratorTarget* objectTarget = lg->GetGlobalGenerator()->FindGeneratorTarget(objectFile->GetObjectLibrary());
      if (!objectTarget || objectTarget->IsImported())
        return nullptr;
      if (!cmSystemTools::IsSubDirectory(objectFile->GetFullPath(),
                                         cmSystemTools::CollapseFullPath(objectTarget->GetObjectDirectory(configName)))) {
        return nullptr;
      }
      return objectTarget;
    };
    std::set<std::string> importedObjectLibraries;
    auto fAddObjectLibrary = [&](cmSourceFile const* objectFile) {
      std::string const& objectLibrary = objectFile->GetObjectLibrary();
      if (objectLibrary.empty())
        return;
      cmGeneratorTarget* objectTarget = fBuiltObjectLibrary(objectFile);
      if (!objectTarget) {
        if (importedObjectLibraries.insert(objectLibrary).second) {
          // Not by the name of the library, which CMake replaces for one that is linked directly
          lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
            cmStrCat("Target ", target->GetName(), " links the object \"", objectFile->GetFullPath(),
                     "\" of an imported object library, but the Malterlib generator can only link the objects of object "
                     "libraries that the import builds"));
        }
        return;
      }
      linkedTargets.insert(objectTarget);
      linkedObjects[objectTarget].insert(objectFile->GetFullPath());
    };
    // Objects in the libraries linked, also of interfaces
    auto fAddLinkedObjects = [&](std::vector<cmLinkItem> const& objects) {
      for (cmLinkItem const& object : objects) {
        if (object.ObjectSource)
          fAddObjectLibrary(object.ObjectSource);
      }
    };

    std::vector<cmSourceFile const*> objectFiles;
    target->GetExternalObjects(objectFiles, configName);
    for (cmSourceFile const* objectFile : objectFiles)
      fAddObjectLibrary(objectFile);
    // A static library doesn't link the objects that link interfaces name with $<TARGET_OBJECTS:...>, nor are they in its
    // archive: the targets that link it link them, and CMake evaluates the interfaces for each of those targets. A Malterlib
    // dependency is passed on to every target that depends on the library, also to native ones, so an object library only
    // becomes a dependency of the static library when the interfaces name its objects for every target: those of the link
    // interface of the library, which has what it links privately as well, and of the libraries reached from it, as long as
    // no interface on the way has a condition on the target that links. And only an object library that the import builds,
    // with all of its objects. What a target of the import that links gets is decided for that target.
    std::map<cmGeneratorTarget*, std::set<std::string>> passedOnObjects;
    auto fPassOnObjects = [&](std::vector<cmLinkItem> const& objects) {
      for (cmLinkItem const& object : objects) {
        if (!object.ObjectSource || object.ObjectSource->GetObjectLibrary().empty())
          continue;
        if (cmGeneratorTarget* objectTarget = fBuiltObjectLibrary(object.ObjectSource))
          passedOnObjects[objectTarget].insert(object.ObjectSource->GetFullPath());
      }
    };

    struct cmMalterlibLinked
    {
      cmGeneratorTarget const* Target;
      bool bConditional; // Reached through an interface with a condition on the target that links, for a static library
    };
    std::vector<cmMalterlibLinked> pending;
    // CMake links the objects of an object library where it is linked directly, not through the interfaces of libraries,
    // which pass on objects as $<TARGET_OBJECTS:...>. Those of Qt only pass them on to targets that aren't static libraries,
    // so a static library passes on the object libraries it reaches to the native targets that depend on it, as for the
    // initializers of the static plugins of Qt.
    std::set<cmGeneratorTarget const*> directlyLinked;
    if (cmLinkImplementation const* impl = target->GetLinkImplementation(configName, cmGeneratorTarget::UseTo::Link)) {
      if (!bStaticLibrary)
        fAddLinkedObjects(impl->Objects);
      for (cmLinkItem const& lib : impl->Libraries) {
        if (lib.Target) {
          // For a static library these are dependencies; what it passes on is found from its link interface
          pending.push_back({ lib.Target, bStaticLibrary });
          directlyLinked.insert(lib.Target);
        }
      }
    }
    if (bStaticLibrary) {
      if (cmLinkInterfaceLibraries const* iface = target->GetLinkInterfaceLibraries(configName, target, cmGeneratorTarget::UseTo::Link)) {
        if (!iface->HadHeadSensitiveCondition)
          fPassOnObjects(iface->Objects);
        for (cmLinkItem const& lib : iface->Libraries) {
          if (lib.Target)
            pending.push_back({ lib.Target, iface->HadHeadSensitiveCondition });
        }
      }
    }
    std::map<cmGeneratorTarget const*, bool> visited; // Whether only reached conditionally
    // CMake puts the objects of the object libraries that a static library links in its archive. Malterlib links them where
    // the static library is linked.
    std::set<cmGeneratorTarget const*> archivedObjectLibraries;
    while (!pending.empty()) {
      cmMalterlibLinked const linked = pending.back();
      pending.pop_back();
      auto iVisited = visited.find(linked.Target);
      bool const bFirst = iVisited == visited.end();
      // Again when it was only reached conditionally before
      if (!bFirst && (!iVisited->second || linked.bConditional))
        continue;
      visited[linked.Target] = linked.bConditional;
      if (bFirst && linked.Target->GetType() == cm::TargetType::STATIC_LIBRARY) {
        if (cmLinkImplementation const* impl = linked.Target->GetLinkImplementation(configName, cmGeneratorTarget::UseTo::Link)) {
          for (cmLinkItem const& lib : impl->Libraries) {
            if (lib.Target && IsObjectLib(lib.Target))
              archivedObjectLibraries.insert(lib.Target);
          }
        }
        // And those of $<TARGET_OBJECTS:...> in its sources
        std::vector<cmSourceFile const*> archivedObjectFiles;
        linked.Target->GetExternalObjects(archivedObjectFiles, configName);
        for (cmSourceFile const* objectFile : archivedObjectFiles) {
          if (objectFile->GetObjectLibrary().empty())
            continue;
          if (cmGeneratorTarget* objectTarget =
                linked.Target->GetLocalGenerator()->FindGeneratorTargetToUse(objectFile->GetObjectLibrary())) {
            if (IsObjectLib(objectTarget))
              archivedObjectLibraries.insert(objectTarget);
          }
        }
      }
      if (cmLinkInterfaceLibraries const* iface =
            linked.Target->GetLinkInterfaceLibraries(configName, target, cmGeneratorTarget::UseTo::Link)) {
        bool const bConditional = linked.bConditional || (bStaticLibrary && iface->HadHeadSensitiveCondition);
        if (!bStaticLibrary)
          fAddLinkedObjects(iface->Objects);
        else if (!bConditional)
          fPassOnObjects(iface->Objects);
        for (cmLinkItem const& lib : iface->Libraries) {
          if (lib.Target)
            pending.push_back({ lib.Target, bConditional });
        }
      }
    }
    for (auto const& objects : passedOnObjects) {
      if (!IsObjectLib(objects.first))
        continue;
      std::vector<cmSourceFile const*> objectSources;
      objects.first->GetObjectSources(objectSources, configName);
      if (objects.second.size() < objectSources.size())
        continue;
      linkedTargets.insert(objects.first);
      linkedObjects[objects.first].insert(objects.second.begin(), objects.second.end());
    }
    // An object library has no link step, so nothing links the objects of the object libraries that it links: for the
    // targets that link it they are as interface libraries
    bool const bObjectLibrary = IsObjectLib(target);
    for (auto const& linked : visited) {
      if (IsObjectLib(linked.first) && (bObjectLibrary || (!directlyLinked.count(linked.first) && !bStaticLibrary)))
        continue;
      linkedTargets.insert(linked.first);
    }
    // Those of the sources aren't in the link closure
    linkedTargets.insert(archivedObjectLibraries.begin(), archivedObjectLibraries.end());

    // A Malterlib dependency links all objects of an object library, so $<TARGET_OBJECTS:...,SOURCE_FILES:...> can't select some,
    // and it links other libraries as libraries, not their objects
    for (auto const& objects : linkedObjects) {
      if (!IsObjectLib(objects.first)) {
        lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          cmStrCat("Target ", target->GetName(), " links the objects of ", objects.first->GetName(),
                   ", which isn't an object library, but the Malterlib generator can only link the library"));
        continue;
      }
      // The object library can be linked as a whole as well, where it is linked itself or is in the archive of a static
      // library, which is a dependency too
      if (archivedObjectLibraries.count(objects.first) || (directlyLinked.count(objects.first) && !bObjectLibrary))
        continue;
      std::vector<cmSourceFile const*> objectSources;
      objects.first->GetObjectSources(objectSources, configName);
      if (objects.second.size() < objectSources.size()) {
        lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          cmStrCat("Target ", target->GetName(), " links some of the objects of object library ", objects.first->GetName(),
                   ", but the Malterlib generator can only link all of them"));
      }
    }
  }

  // CMake's final build-order graph is acyclic and keeps every add_dependencies edge, but within a cycle of static libraries
  // it orders the link and compile-only edges in one chain. The links that chain leaves out are added from the link closures.
  auto fLinkClosure = [&](cmGeneratorTarget const* _pTarget) -> std::set<cmGeneratorTarget const*> const& {
    auto iClosure = LinkClosures.find(_pTarget);
    if (iClosure == LinkClosures.end())
      iClosure = LinkClosures.emplace(_pTarget, getLinkClosure(_pTarget, configName)).first;
    return iClosure->second;
  };
  auto fDirectDepends = [&](cmGeneratorTarget const* _pTarget) -> cmTargetDependSet const& {
    return const_cast<cmGlobalGenerator*>(GlobalGenerator)->GetTargetDirectDepends(_pTarget);
  };
  // The build-order graph is acyclic, so the targets reachable from a target are those of its dependencies
  std::function<std::set<cmGeneratorTarget const*> const&(cmGeneratorTarget const*)> fReachable =
    [&](cmGeneratorTarget const* _pTarget) -> std::set<cmGeneratorTarget const*> const& {
      auto iReachable = ReachableTargets.find(_pTarget);
      if (iReachable != ReachableTargets.end())
        return iReachable->second;
      std::set<cmGeneratorTarget const*> reachable;
      for (auto& next : fDirectDepends(_pTarget)) {
        reachable.insert(next);
        std::set<cmGeneratorTarget const*> const& nextReachable = fReachable(next);
        reachable.insert(nextReachable.begin(), nextReachable.end());
      }
      return ReachableTargets.emplace(_pTarget, std::move(reachable)).first->second;
    };
  // Through the dependencies that the import has, which are those of the build order and the links restored below
  auto fReaches = [&](cmGeneratorTarget const* _pFrom, cmGeneratorTarget const* _pTo) -> bool {
    if (fReachable(_pFrom).count(_pTo))
      return true;
    if (RestoredLinks.empty())
      return false;
    std::set<cmGeneratorTarget const*> visited;
    std::vector<cmGeneratorTarget const*> pending{ _pFrom };
    while (!pending.empty()) {
      cmGeneratorTarget const* current = pending.back();
      pending.pop_back();
      if (current == _pTo)
        return true;
      if (!visited.insert(current).second)
        continue;
      for (auto& next : fDirectDepends(current))
        pending.push_back(next);
      auto iRestored = RestoredLinks.find(current);
      if (iRestored != RestoredLinks.end())
        pending.insert(pending.end(), iRestored->second.begin(), iRestored->second.end());
    }
    return false;
  };
  // The libraries a target gets from its dependencies, as static libraries pass on the libraries they link
  auto fLinksThroughDependencies = [&](cmGeneratorTarget const* _pTarget) -> std::set<cmGeneratorTarget const*> {
    std::set<cmGeneratorTarget const*> linked;
    std::vector<cmGeneratorTarget const*> pending{ _pTarget };
    std::set<cmGeneratorTarget const*> visited;
    while (!pending.empty()) {
      cmGeneratorTarget const* current = pending.back();
      pending.pop_back();
      if (!visited.insert(current).second)
        continue;
      for (auto& next : fDirectDepends(current)) {
        if (!fLinkClosure(current).count(next))
          continue;
        linked.insert(next);
        if (IsStaticLib(next))
          pending.push_back(next);
      }
    }
    return linked;
  };

  auto fAddDependencies = [&](cmGeneratorTarget const *_pTarget, bool _bOnlyObjects, mint _Depth) -> void
    {
      cmTargetDependSet const& targetDependencies =
        const_cast<cmGlobalGenerator*>(GlobalGenerator)->
        GetTargetDirectDepends(_pTarget);

      for (auto &dependency : targetDependencies) {
        //auto dependencyLocalGenerator = dependency->GetLocalGenerator();
        // An interface library in the build system, as with sources that a custom command generates, is a target that orders
        // the build
        if (dependency->GetName() == "global_target" ||
            (dependency->GetType() == cm::TargetType::INTERFACE_LIBRARY && !dependency->IsInBuildSystem())) {
          continue;
        }

        /*if (dependency->GetType() == cm::TargetType::OBJECT_LIBRARY) {
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
        }*/
        if (_bOnlyObjects)
          continue;

        auto &outputDependency =
          outputTarget.addChild("%Dependency", GetTargetName(dependency, dependency->LocalGenerator->GetProjectName()));

        // The edge doesn't say whether the target links the dependency: CMake keeps one edge per dependency, and an edge of
        // add_dependencies or of $<TARGET_OBJECTS> that only compiles can stand for a link too. The targets linked decide.
        bool const bLinked = linkedTargets.count(dependency) != 0 && dependency->GetType() != cm::TargetType::INTERFACE_LIBRARY;
        if (!bLinked) {
          outputDependency.addChild("Dependency.Link", "false").RawValue = true;
          // Before compiling, as for the headers it can generate
          outputDependency.addChild("Dependency.Stage", "\"Compile\"").RawValue = true;
        }
        else if (IsStaticLib(target) && IsStaticLib(dependency)) {
          outputDependency.addChild("Dependency.Indirect", "true").RawValue = true;
          outputDependency.addChild("Dependency.IndirectOrdered", "true").RawValue = true;
        }
      }
    }
  ;

  fAddDependencies(target, false, 0);

  // A Malterlib dependency also orders the build, so a library that is built after the target, as in a cycle of static
  // libraries, can't be a dependency of it. Targets that depend on the target natively would not link the library.
  if (!isUtilityTarget) {
    // CMake usually makes the libraries of the link closure direct dependencies, so the walk through the dependencies is only
    // needed for the others. In deep graphs of static libraries the walk is quadratic for each target.
    std::set<cmGeneratorTarget const*> const& linkClosure = fLinkClosure(target);
    std::vector<cmGeneratorTarget const*> notDirect;
    {
      std::set<cmGeneratorTarget const*> direct;
      for (auto& dependency : fDirectDepends(target)) {
        if (linkClosure.count(dependency))
          direct.insert(dependency);
      }
      for (cmGeneratorTarget const* library : linkClosure) {
        // The objects of an object library are only linked where it is linked directly, which the dependencies above have
        if (library == target || !library->IsInBuildSystem() || library->GetType() == cm::TargetType::INTERFACE_LIBRARY ||
            IsObjectLib(library) || direct.count(library)) {
          continue;
        }
        notDirect.push_back(library);
      }
    }
    // In the order of the names, as for the other dependencies, so that the import does not change with the addresses
    std::sort(notDirect.begin(), notDirect.end(), [](cmGeneratorTarget const* _pLeft, cmGeneratorTarget const* _pRight) {
      return _pLeft->GetName() < _pRight->GetName();
    });
    std::set<cmGeneratorTarget const*> linked;
    if (!notDirect.empty())
      linked = fLinksThroughDependencies(target);
    for (cmGeneratorTarget const* library : notDirect) {
      if (linked.count(library))
        continue;
      if (fReaches(library, target)) {
        lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          cmStrCat("Target \"", target->GetName(), "\" links \"", library->GetName(),
                   "\", which is built after it. Malterlib dependencies can't express cycles of static libraries."));
        continue;
      }
      RestoredLinks[target].insert(library);
      auto &outputDependency = outputTarget.addChild("%Dependency", GetTargetName(library, library->LocalGenerator->GetProjectName()));
      if (IsStaticLib(target) && IsStaticLib(library)) {
        outputDependency.addChild("Dependency.Indirect", "true").RawValue = true;
        outputDependency.addChild("Dependency.IndirectOrdered", "true").RawValue = true;
      }
    }

    // The object libraries whose objects the target names, which CMake's build order can leave out, as with
    // OPTIMIZE_DEPENDENCIES when they are also compile only. Imported ones aren't built.
    std::set<cmGeneratorTarget const*> direct;
    for (auto& dependency : fDirectDepends(target))
      direct.insert(dependency);
    std::vector<cmGeneratorTarget const*> objectLibraries;
    for (auto const& objects : linkedObjects) {
      if (IsObjectLib(objects.first) && objects.first->IsInBuildSystem() && !direct.count(objects.first))
        objectLibraries.push_back(objects.first);
    }
    std::sort(objectLibraries.begin(), objectLibraries.end(), [](cmGeneratorTarget const* _pLeft, cmGeneratorTarget const* _pRight) {
      return _pLeft->GetName() < _pRight->GetName();
    });
    for (cmGeneratorTarget const* objectLibrary : objectLibraries) {
      if (fReaches(objectLibrary, target)) {
        lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          cmStrCat("Target \"", target->GetName(), "\" links the objects of \"", objectLibrary->GetName(),
                   "\", which is built after it. Malterlib dependencies can't express that."));
        continue;
      }
      RestoredLinks[target].insert(objectLibrary);
      auto &outputDependency =
        outputTarget.addChild("%Dependency", GetTargetName(objectLibrary, objectLibrary->LocalGenerator->GetProjectName()));
      if (IsStaticLib(target)) {
        outputDependency.addChild("Dependency.Indirect", "true").RawValue = true;
        outputDependency.addChild("Dependency.IndirectOrdered", "true").RawValue = true;
      }
    }
  }

  if (!isUtilityTarget) {
    auto fDefinesValue = [&](std::set<std::string> const& _Defines) {
      std::vector<std::string> values;
      for (auto &define : _Defines) {
        auto Remapped = replaceMappedOutputFiles(_ProjectName, define, true);
        if (isDynamic(Remapped)) {
          std::string Escaped;
          cmMalterlibRegistry::addEscapeStr(Escaped, Remapped, "`\\\r\n\t", "`\\rnt", true);
          values.push_back(Escaped);
        } else
          values.push_back(cmMalterlibRegistry::getEscaped(Remapped, true, true));
      }
      return cmJoin(values, ", ");
    };
    // Sources of a compile type that the target has no settings for get all their definitions
    for (cmMalterlibSourceDefines const& source : SourceDefines) {
      if (!compileTypeInfo.count(source.CompileType) && !source.Defines.empty())
        source.File->addChild("Compile.PreprocessorDefines", "=+ [" + fDefinesValue(source.Defines) + "]").RawValue = true;
    }
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
      if (!info.SystemIncludes.empty()) {
        for (auto &path : info.SystemIncludes)
          path = makeAbsoluteWrapper(cmSystemTools::CollapseFullPath(path));
        auto end = cmRemoveDuplicates(info.SystemIncludes);
        info.SystemIncludes.erase(end, info.SystemIncludes.end());
        compileOutput.addChild("SystemSearchPath", "+= [" + cmJoin(info.SystemIncludes, ", ") + "]").RawValue = true;
      }
      auto fAddFrameworkDirectories = [&](std::vector<std::string>& _Directories, char const* _pKey) {
        if (_Directories.empty())
          return;
        for (auto &path : _Directories)
          path = makeAbsoluteWrapper(cmSystemTools::CollapseFullPath(path));
        auto end = cmRemoveDuplicates(_Directories);
        _Directories.erase(end, _Directories.end());
        compileOutput.addChild(_pKey, "+= [" + cmJoin(_Directories, ", ") + "]").RawValue = true;
      };
      fAddFrameworkDirectories(info.FrameworkDirectories, "FrameworkSearchPath");
      fAddFrameworkDirectories(info.SystemFrameworkDirectories, "SystemFrameworkSearchPath");
      {
        // Malterlib adds the definitions of a source to those of its target, so a macro that some sources define differently,
        // or not at all, is defined for each source rather than for the target. The compile then gets one definition of it,
        // also where the target and the source define it.
        std::set<std::string> sourceMacros;
        for (cmMalterlibSourceDefines const& source : SourceDefines) {
          if (source.CompileType != infoMap.first)
            continue;
          for (std::string const& define : source.Defines) {
            if (!info.Defines.count(define))
              sourceMacros.insert(macroName(define));
          }
          for (std::string const& define : info.Defines) {
            if (!source.Defines.count(define))
              sourceMacros.insert(macroName(define));
          }
        }
        std::set<std::string> targetDefines;
        for (std::string const& define : info.Defines) {
          if (!sourceMacros.count(macroName(define)))
            targetDefines.insert(define);
        }
        compileOutput.addChild("PreprocessorDefines", "+= [" + fDefinesValue(targetDefines) + "]").RawValue = true;

        for (cmMalterlibSourceDefines const& source : SourceDefines) {
          if (source.CompileType != infoMap.first)
            continue;
          std::set<std::string> defines;
          for (std::string const& define : source.Defines) {
            if (sourceMacros.count(macroName(define)))
              defines.insert(define);
          }
          if (!defines.empty())
            source.File->addChild("Compile.PreprocessorDefines", "=+ [" + fDefinesValue(defines) + "]").RawValue = true;
        }
      }
      if (!info.cStd.empty() && infoMap.first == "C") {
        std::string const cLanguage = malterlibCLanguage(info.cStd);
        if (!cLanguage.empty()) {
          compileOutput.addChild("CLanguage", cLanguage);
          outputTarget.addChild("Target.CLanguage", cLanguage, true);
        } else {
          lg->GetMakefile()->IssueMessage(MessageType::WARNING,
            cmStrCat("Target \"", target->GetName(), "\" uses -std=", info.cStd,
                     ", which has no Malterlib C language. It is left to Malterlib."));
        }
      }
    }
  }

  for (auto &child : outputTarget.Children) {
    if (child.Key == "%Group")
      child.pruneLoneChildren();
  }
}
