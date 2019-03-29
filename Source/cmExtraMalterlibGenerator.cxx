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
#include <cctype>
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
#include "cmGenExContext.h"
#include "cmGeneratorExpression.h"
#include "cmGeneratorExpressionDAGChecker.h"
#include "cmGeneratorExpressionEvaluationFile.h"
#include "cmGeneratorFileSet.h"
#include "cmGeneratorTarget.h"
#include "cmGlobalGenerator.h"
#include "cmList.h"
#include "cmLocalGenerator.h"
#include "cmMakefile.h"
#include "cmSourceFile.h"
#include "cmStandardLevelResolver.h"
#include "cmState.h"
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


  // The configuration that a generation is of, as AppendTarget checks that there is one
  std::string GetConfigName(cmLocalGenerator *lg) {
    cmLocalCommonGenerator *commonGenerator = static_cast<cmLocalCommonGenerator *>(lg);
    if (commonGenerator && !commonGenerator->GetConfigNames().empty())
      return commonGenerator->GetConfigNames()[0];
    return "Debug";
  }

  std::string GetTargetName(const cmGeneratorTarget* target, std::string const &projectName) {
    std::string prefix;
    switch (target->GetType()) {
      case cm::TargetType::EXECUTABLE:
        // As Malterlib names a UIExecutable, as an application bundle is (GetTargetType)
        prefix = (target->IsAppBundleOnApple() ? "Exe_" : "Com_") + projectName + "_";
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
        // An application bundle is a bundle of Malterlib
        return target->IsAppBundleOnApple() ? "UIExecutable" : "ConsoleExecutable";
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

  // The name of a function-like macro ends at its parameters. The white space around a name doesn't belong to it.
  std::string macroName(std::string const& define)
  {
    return cmTrimWhitespace(cm::string_view(define).substr(0, define.find_first_of("=(")));
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
    bool bUndefine;
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
    if (architectureId == "x64" || architectureId == "AMD64")
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
    // CMake leaves CMAKE_<LANG>_COMPILER_ARCHITECTURE_ID empty for a language whose compiler it doesn't identify by a
    // compiled test, as ASM. The import compiles all its languages for the same architecture, which CMake then has for
    // the others, or which the import configures.
    cmMakefile const* makefile = lg->GetMakefile();
    cmList architectures;
    for (std::string const& architectureLanguage : { language, std::string("C"), std::string("CXX"), std::string("OBJC"),
                                                     std::string("OBJCXX") }) {
      architectures.assign(makefile->GetSafeDefinition(cmStrCat("CMAKE_", architectureLanguage, "_COMPILER_ARCHITECTURE_ID")));
      if (!architectures.empty())
        break;
    }
    if (architectures.empty())
      architectures.assign(makefile->GetSafeDefinition("CMAKE_OSX_ARCHITECTURES"));
    if (architectures.empty())
      architectures.assign(makefile->GetSafeDefinition("CMAKE_SYSTEM_PROCESSOR"));
    for (std::string const& architecture : architectures) {
      if (compilerArchitecture(architecture) == selected)
        return true;
    }
    return false;
  }

  // Whether an option of the driver of GCC or Clang passes the argument after it on to another tool than the compiler, as
  // -Xlinker passes it to the linker
  bool takesOperandOfOtherTool(std::string const& option)
  {
    static std::set<std::string> const options{ "-Xanalyzer", "-Xassembler",     "-Xcuda-fatbinary", "-Xcuda-ptxas",
                                                "-Xlinker",   "-Xoffload-linker", "-mllvm",           "-z" };
    return options.count(option) || option == "-Xopenmp-target" || cmHasLiteralPrefix(option, "-Xopenmp-target=");
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
        else if (bMSVCDriver && (option == "/link" || option == "-link"))
          break; // cl passes the rest of its command line to the linker
        else if (!bMSVCDriver && takesOperandOfOtherTool(option) && iOption + 1 < _Options.size())
          ++iOption; // The operand is for the linker, the assembler or another tool, not an option of the compiler
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
      // cl, and clang-cl, take /DNAME#VALUE for NAME=VALUE, as every spelling of -D, also in the options that clang-cl gets
      // with /clang:, but not in those that go to the preprocessor or the compiler as they are
      bool const bMSVCDefines =
        bMSVCOptions && (_Stage == cmMalterlibDefines::Driver || _Stage == cmMalterlibDefines::DriverAppended);
      auto fNormalizeDefine = [&](std::string&& _Define) -> std::string {
        if (bMSVCDefines) {
          size_t const iSeparator = _Define.find_first_of("=#");
          if (iSeparator != std::string::npos && _Define[iSeparator] == '#')
            _Define[iSeparator] = '=';
        }
        return std::move(_Define);
      };
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
        // Of /std:, cl and clang-cl only apply the C standards to C, and ignore the C++ ones
        if (bMSVCDriver && (cmHasLiteralPrefix(option, "/std:") || cmHasLiteralPrefix(option, "-std:"))) {
          std::string const standard = option.substr(5);
          if (!cmHasLiteralPrefix(standard, "c++"))
            o_CStd = standard == "clatest" ? "c23" : standard;
          continue;
        }
        // The drivers of GCC and Clang take --define-macro and --undefine-macro as -D and -U
        if (bDriver) {
          bool const bLongUndefine = option == "--undefine-macro" || cmHasLiteralPrefix(option, "--undefine-macro=");
          if (bLongUndefine || option == "--define-macro" || cmHasLiteralPrefix(option, "--define-macro=")) {
            size_t const iEquals = option.find('=');
            std::string define;
            if (iEquals != std::string::npos)
              define = option.substr(iEquals + 1);
            else if (iOption + 1 < _Options.size())
              define = _Options[++iOption];
            else
              continue;
            o_Defines.push_back({ bLongUndefine ? std::move(define) : fNormalizeDefine(std::move(define)), bLongUndefine });
            continue;
          }
        }
        bool const bDefine = cmHasLiteralPrefix(option, "-D") || (bMSVCDriver && cmHasLiteralPrefix(option, "/D"));
        bool const bUndefine = cmHasLiteralPrefix(option, "-U") || (bMSVCDriver && cmHasLiteralPrefix(option, "/U"));
        if (!bDefine && !bUndefine)
          continue;
        std::string define;
        if (option.size() > 2)
          define = option.substr(2);
        else if (iOption + 1 < _Options.size())
          define = _Options[++iOption];
        else
          continue;
        o_Defines.push_back({ bDefine ? fNormalizeDefine(std::move(define)) : std::move(define), bUndefine });
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
  // A directory that the compiler searches, collapsed lexically as the paths of the checkout and of the build that Malterlib
  // maps, except where .. leaves a symbolic link: the compiler resolves .. from where the link points, so there the link is
  // replaced by its target. The path keeps its spelling elsewhere, as of the roots that import caches map, and directories
  // that only the build creates collapse as the rest.
  std::string collapseSearchDirectory(std::string const& _Path, std::string const& _Base = std::string(), int _Depth = 0)
  {
    std::string collapsed = _Base.empty() ? cmSystemTools::CollapseFullPath(_Path) : cmSystemTools::CollapseFullPath(_Path, _Base);
    // A .. after the marker of a directory of a target is resolved from the directory of the Malterlib target, which isn't on disk
    if (_Path.find("..") == std::string::npos || cmSystemTools::MalterlibMarkerEnd(collapsed) != std::string::npos)
      return collapsed;
    std::string const path = cmSystemTools::FileIsFullPath(_Path)
      ? _Path
      : cmStrCat(_Base.empty() ? cmSystemTools::GetCurrentWorkingDirectory() : _Base, '/', _Path);
    std::vector<std::string> components;
    cmSystemTools::SplitPath(path, components);
    std::vector<std::string> resolved{ components.front() };
    bool bThroughLink = false;
    for (size_t iComponent = 1; iComponent < components.size(); ++iComponent) {
      std::string const& component = components[iComponent];
      if (component.empty() || component == ".")
        continue;
      if (component != "..") {
        resolved.push_back(component);
        continue;
      }
      // Where a link points can be another link, which .. also leaves from where that points
      std::string left = cmSystemTools::JoinPath(resolved);
      std::string target;
      int depth = _Depth;
      while (resolved.size() > 1 && depth < 32 && cmSystemTools::FileIsSymlink(left) &&
             cmSystemTools::ReadSymlink(left, target)) {
        resolved.pop_back();
        left = collapseSearchDirectory(target, cmSystemTools::JoinPath(resolved), ++depth);
        resolved.clear();
        cmSystemTools::SplitPath(left, resolved);
        bThroughLink = true;
      }
      if (resolved.size() > 1)
        resolved.pop_back();
    }
    return bThroughLink ? cmSystemTools::CollapseFullPath(cmSystemTools::JoinPath(resolved)) : collapsed;
  }

  // CMake writes a newline in compile flags, options and definitions to its build.ninja as the end of a line with $, which
  // ninja joins with the next line without the spaces that start it
  std::string joinNinjaContinuations(std::string&& _Text)
  {
    if (_Text.find('\n') == std::string::npos)
      return std::move(_Text);
    std::string joined;
    for (size_t iChar = 0; iChar < _Text.size(); ++iChar) {
      if (_Text[iChar] != '\n') {
        joined += _Text[iChar];
        continue;
      }
      while (iChar + 1 < _Text.size() && _Text[iChar + 1] == ' ')
        ++iChar;
    }
    return joined;
  }

  std::vector<std::string> argumentsFromFlags(cmLocalGenerator* lg, std::string const& flags)
  {
    std::string commandLine;
    for (size_t iChar = 0; iChar < flags.size(); ++iChar) {
      // The flags of the target are escaped for ninja, where a newline is $ and a newline, and those of sources aren't
      bool const bEscapedNewline = flags[iChar] == '$' && iChar + 1 < flags.size() && flags[iChar + 1] == '\n';
      if (bEscapedNewline || flags[iChar] == '\n') {
        if (bEscapedNewline)
          ++iChar;
        while (iChar + 1 < flags.size() && flags[iChar + 1] == ' ')
          ++iChar;
        continue;
      }
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

  // A target asks for a standard with its property, as C_STANDARD, or with a feature that it compiles with, its own or one of
  // its dependencies, as of target_compile_features(Target PUBLIC c_std_17)
  bool asksForStandard(cmGeneratorTarget const* target, std::string const& language, std::string const& configName)
  {
    if (target->GetLanguageStandard(language, configName))
      return true;
    cmStandardLevelResolver const resolver(target->Makefile);
    for (BT<std::string> const& feature : target->GetCompileFeatures(configName)) {
      std::string featureLanguage;
      if (resolver.CompileFeatureKnown(target->GetName(), feature.Value, featureLanguage, nullptr) &&
          featureLanguage == language)
        return true;
    }
    return false;
  }

  // On Apple platforms CMake passes a framework among the include directories as its directory, with -F, or -iframework for
  // a directory that the target marks as a system directory, and leaves out that of the frameworks of the system. CMake decides
  // by the spelling of the directory, which is the include directory without the markers of the directories of targets.
  // Returns false for an include directory that isn't a framework.
  bool frameworkSearchDirectory(cmLocalGenerator* lg, cmGeneratorTarget const* target, std::string const& config,
                                std::string const& language, std::string const& spelling, std::string const& include,
                                std::vector<std::string>& o_Directories, std::vector<std::string>& o_SystemDirectories)
  {
    cmMakefile* makefile = lg->GetMakefile();
    if (!makefile->IsOn("APPLE") || makefile->GetSafeDefinition(cmStrCat("CMAKE_", language, "_FRAMEWORK_SEARCH_FLAG")).empty() ||
        !cmSystemTools::IsPathToFramework(spelling)) {
      return false;
    }
    std::string const directory = cmSystemTools::GetFilenamePath(spelling);
    if (directory == "/System/Library/Frameworks")
      return true;
    bool const bSystem = !makefile->GetSafeDefinition(cmStrCat("CMAKE_", language, "_SYSTEM_FRAMEWORK_SEARCH_FLAG")).empty() &&
      target->IsSystemIncludeDirectory(directory, config, language);
    (bSystem ? o_SystemDirectories : o_Directories).push_back(cmSystemTools::GetFilenamePath(include));
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
    bool bRaw; // A shell operator, or an argument that the shell expands, which stays as written
    bool bSeparated; // Whether whitespace separates it from the token before, as in 2>&1 nothing does
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
      cmExtraMalterlibGenerator &generator
      , std::map<std::string, cmMalterlibCompileTypeInfo> &compileTypeInfo
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
      std::vector<std::string> includes = generator.EvaluateMarkedList([&]() -> std::vector<std::string> {
        std::vector<std::string> directories;
        lg->GetIncludeDirectories(directories, target, language, configName);
        return directories;
      });
      bool const bSystemIncludes = usesSystemIncludeFlag(lg, language);
      for (std::string& include : includes) {
        std::string const spelling = generator.RemoveTargetDirectoryMarkers(include);
        if (frameworkSearchDirectory(lg, target, configName, language, spelling, include, info.FrameworkDirectories,
                                     info.SystemFrameworkDirectories)) {
          continue;
        }
        bool const bSystem = bSystemIncludes && target->IsSystemIncludeDirectory(spelling, configName, language);
        auto& toIncludes = bSystem ? info.SystemIncludes : info.Includes;
        toIncludes.push_back(std::move(include));
      }
      std::set<std::string> const defines = generator.EvaluateMarkedSet([&]() -> std::set<std::string> {
        std::set<std::string> targetDefines;
        lg->GetTargetDefines(target, configName, language, targetDefines);
        return targetDefines;
      });
      info.Defines.insert(defines.begin(), defines.end());
      // With the architecture, as the overload without it returns the flags. The flags come after the definitions.
      std::string const compileFlags = generator.EvaluateMarkedString([&]() -> std::string {
        std::string flags;
        lg->GetTargetCompileFlags(const_cast<cmGeneratorTarget* >(target), configName, language, flags, std::string());
        return flags;
      });
      applyDefines(info.Defines, definesFromFlags(lg, language, compileFlags, &info.cStd));
      // CMake leaves out the flag of the standard that a target asks for when the default of the compiler satisfies it, which
      // Malterlib's default need not, so then the target compiles with the default of the compiler
      if (language == "C" && info.cStd.empty() && asksForStandard(target, language, configName)) {
        cmMakefile const* makefile = lg->GetMakefile();
        std::string const standard = makefile->GetSafeDefinition("CMAKE_C_STANDARD_DEFAULT");
        if (!standard.empty())
          info.cStd = cmStrCat(makefile->IsOn("CMAKE_C_EXTENSIONS_DEFAULT") ? "gnu" : "c", standard);
      }
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
  // The markers are recognized until the generator is done, also where it collapses the values of the marked evaluation
  struct cmMarkedDirectoriesScope
  {
    explicit cmMarkedDirectoriesScope(std::vector<std::string> const *pDirectories) { cmMalterlibMarkedDirectories = pDirectories; }
    ~cmMarkedDirectoriesScope() { cmMalterlibMarkedDirectories = nullptr; }
    cmMarkedDirectoriesScope(cmMarkedDirectoriesScope const &) = delete;
    cmMarkedDirectoriesScope &operator=(cmMarkedDirectoriesScope const &) = delete;
  } const markedDirectories(&TargetDirectories);
  for (auto const &pLocalGenerator : this->GlobalGenerator->GetLocalGenerators()) {
    cmMakefile const *pMakefile = pLocalGenerator->GetMakefile();
    for (std::string const &file : pMakefile->GetOutputFiles())
      ConfigureOutputFiles.insert(cmSystemTools::CollapseFullPath(file));
    for (auto const &pEvaluationFile : pMakefile->GetEvaluationFiles()) {
      for (std::string const &file : pEvaluationFile->GetFiles())
        ConfigureOutputFiles.insert(cmSystemTools::CollapseFullPath(file));
    }
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

// How an evaluated path in an eval string ends, where it escapes itself for the shell that the state escapes for: the shell of
// the host, or bash on Windows, as for the bash scripts of build events (cmMalterlibUnixShellScope)
static std::string escapeSuffix(cmState const *state) {
#ifdef _WIN32
  if (!state->UseWindowsShell())
    return "->EscapeBash())";
#endif
  return "->EscapeHost())";
}

// Escapes for a Unix shell while it lives, also where the shell of Windows runs the other commands
struct cmMalterlibUnixShellScope
{
  explicit cmMalterlibUnixShellScope(cmState *state)
    : State(state)
    , bWindowsShell(state->UseWindowsShell())
  {
    State->SetWindowsShell(false);
  }
  ~cmMalterlibUnixShellScope() { State->SetWindowsShell(bWindowsShell); }
  cmMalterlibUnixShellScope(cmMalterlibUnixShellScope const &) = delete;
  cmMalterlibUnixShellScope &operator=(cmMalterlibUnixShellScope const &) = delete;

  cmState *State;
  bool bWindowsShell;
};

static std::string makeAbsoluteWrapperEvalString(cmState const *state, std::string const &fileName) {
  if (isDynamic(fileName))
    return fileName;

  return cmStrCat("@(", cmMalterlibRegistry::getEscaped(fileName, true, true), "->MakeAbsolute()", escapeSuffix(state));
}

// Escapes for the shell only: the Malterlib build system escapes for its generators itself, as for Ninja
static std::string escapeForShell(cmLocalGenerator *localGenerator, std::string const &argument) {
  return cmOutputConverter::EscapeForShell(
    argument, localGenerator->GetState()->UseWindowsShell() ? 0 : cmOutputConverter::Shell_Flag_IsUnix);
}

// A literal part of an argument is escaped as such also when it is what EscapeForShell leaves as a shell operator, such as
// the > of '>path', which a whole argument of > would be. Those are the only ones it leaves with < > | &.
static std::string escapeFragmentForShell(cmLocalGenerator *localGenerator, std::string const &fragment) {
  std::string escaped = escapeForShell(localGenerator, fragment);
  if (escaped != fragment || fragment.find_first_of("<>|&") == std::string::npos)
    return escaped;
  return localGenerator->GetState()->UseWindowsShell() ? cmStrCat('"', fragment, '"') : cmStrCat('\'', fragment, '\'');
}

// As cmCustomCommandGenerator escapes the arguments of a command that is not VERBATIM: only spaces
static std::string escapeForShellOldStyle(cmLocalGenerator *localGenerator, std::string const &argument) {
  if (localGenerator->GetState()->UseWindowsShell()) {
    if (argument.find(' ') != std::string::npos && argument.find('"') == std::string::npos)
      return cmStrCat('"', argument, '"');
    return argument;
  }
  std::string escaped;
  for (char character : argument) {
    if (character == ' ')
      escaped += '\\';
    escaped += character;
  }
  return escaped;
}

static size_t evalStringEnd(std::string const &text, size_t iChar);

// Where the expression @(...) that starts at an index ends, after its ), past its strings, also eval strings, as `@(...)/a)`,
// whose parentheses are their own
static size_t evalExpressionEnd(std::string const &text, size_t iChar) {
  int depth = 0;
  for (iChar += 1; iChar < text.size();) {
    char const character = text[iChar];
    if (character == '"' || character == '\'') {
      for (++iChar; iChar < text.size() && text[iChar] != character; ++iChar) {
        if (text[iChar] == '\\')
          ++iChar;
      }
      ++iChar;
      continue;
    }
    if (character == '`') {
      iChar = evalStringEnd(text, iChar);
      continue;
    }
    ++iChar;
    if (character == '(')
      ++depth;
    else if (character == ')' && --depth == 0)
      break;
  }
  return std::min(iChar, text.size());
}

// Where the eval string `...` that starts at an index ends, after its `, past its escapes and expressions
static size_t evalStringEnd(std::string const &text, size_t iChar) {
  for (iChar += 1; iChar < text.size();) {
    char const character = text[iChar];
    if (character == '\\') {
      iChar += 2;
      continue;
    }
    if (character == '`')
      return iChar + 1;
    if (character == '@' && iChar + 1 < text.size() && text[iChar + 1] == '@') {
      iChar += 2;
      continue;
    }
    if (character == '@' && iChar + 1 < text.size() && text[iChar + 1] == '(') {
      iChar = evalExpressionEnd(text, iChar);
      continue;
    }
    ++iChar;
  }
  return text.size();
}

// The literal parts and the evaluated expressions, @(...), of the text of an eval string, where @@ is a literal @
struct cmMalterlibArgumentPart
{
  std::string Text;
  bool bExpression;
};
static std::vector<cmMalterlibArgumentPart> splitEvalString(std::string const &argument) {
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
    iChar = evalExpressionEnd(argument, iChar);
    parts.push_back({ argument.substr(iStart, iChar - iStart), true });
  }
  fAddLiteral();
  return parts;
}

// The text of an eval string in a string of the build system, whose literal parts are escaped for it and whose expressions
// stay as they are, as their own strings are escaped for the expression
static void addEvalStringText(std::string &o_Dest, std::string const &text) {
  for (cmMalterlibArgumentPart const &part : splitEvalString(text)) {
    if (part.bExpression)
      o_Dest += part.Text;
    else
      cmMalterlibRegistry::addEscapeStr(o_Dest, part.Text, "`\\\r\n\t", "`\\rnt", false);
  }
}

// An argument with evaluated paths stays one argument for the shell, as its literal parts are escaped, as the other arguments
// of the command are. The evaluated paths escape themselves (escapeSuffix).
static std::string escapeDynamicArgument(cmLocalGenerator *localGenerator, std::string const &argument, bool bOldStyle) {
  std::string escaped;
  // An empty argument stays an argument, as CMake quotes it, except in the old style
  if (argument.empty()) {
    if (!bOldStyle)
      cmMalterlibRegistry::addEscapeStr(escaped, escapeForShell(localGenerator, argument), "`\\\r\n\t", "`\\rnt", false);
    return escaped;
  }

  std::vector<cmMalterlibArgumentPart> parts = splitEvalString(argument);
  std::string const suffix = escapeSuffix(localGenerator->GetState());

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
        bWhole = bWhole && cmHasSuffix(part.Text, suffix);
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
          escaped += cmStrCat(cm::string_view(part.Text).substr(0, part.Text.size() - suffix.size()), ')');
        else
          fAddEvalString(part.Text);
      }
      if (!bAsIs)
        escaped += cmStrCat('`', suffix);
      return escaped;
    }
  }

  for (cmMalterlibArgumentPart const& part : parts) {
    if (part.bExpression)
      escaped += part.Text;
    else if (bOldStyle)
      fAddEvalString(escapeForShellOldStyle(localGenerator, part.Text));
    else if (parts.size() > 1)
      fAddEvalString(escapeFragmentForShell(localGenerator, part.Text));
    else // A whole argument, which stays a shell operator, as > of a redirection
      fAddEvalString(escapeForShell(localGenerator, part.Text));
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
  // After the files of all targets are known, for the targets that the import has
  CommandOutputFiles[_ProjectName] = MappedOutputFiles[_ProjectName];
  for (cmLocalGenerator *lg : lgs) {
    for (auto &target : lg->GetGeneratorTargets()) {
      if (target->IsInBuildSystem() && target->GetType() != cm::TargetType::GLOBAL_TARGET)
        CollectBuildEventFiles(_ProjectName, lg, target.get());
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

// As GetCustomCommandLines, which leaves out the commands whose command is empty in the configuration
static bool runsNothing(cmCustomCommandGenerator const &commandGenerator) {
  for (unsigned i = 0; i != commandGenerator.GetNumberOfCommands(); ++i) {
    if (!commandGenerator.GetCommand(i).empty())
      return false;
  }
  return true;
}

// Whether a character after a path continues the name of a file, as the @ of an image for two pixels per point does, rather than
// end the path, as a separator of paths, a quote or a character that a command, a list or an expression puts after a path does
static bool namesFile(char character) {
  return character && !std::isspace(static_cast<unsigned char>(character)) && !std::strchr("\"'`;,:()[]{}<>|&=/\\", character);
}

// The lexical form of an absolute path, without the . and .. segments and the doubled separators that it has, which is how CMake
// names the files of commands, as a DEPENDS of ${CMAKE_SOURCE_DIR}/../file, so that every spelling of a path names the same file.
// It is empty for a path that has none of those, that isn't absolute or whose .. would leave its root, and for a path with a
// marker of a directory of a target, which is mapped by that directory, as $<TARGET_FILE_DIR>/../Resources is in the bundle of
// the Malterlib target. The two separators that start a UNC path, as //server/share, are its root.
static std::string lexicalPath(cm::string_view _Path)
{
  auto const fSeparator = [](char _Character) { return _Character == '/' || _Character == '\\'; };
  std::size_t nRoot = 0;
  if (_Path.size() >= 3 && fSeparator(_Path[0]) && fSeparator(_Path[1]) && !fSeparator(_Path[2]))
    nRoot = 2;
  else if (!_Path.empty() && fSeparator(_Path[0]))
    nRoot = 1;
  else if (_Path.size() >= 3 && std::isalpha(static_cast<unsigned char>(_Path[0])) && _Path[1] == ':' &&
           (_Path[2] == '/' || _Path[2] == '\\'))
    nRoot = 3;
  else
    return std::string();
  if (_Path.find(".malterlib-file-dir-") != cm::string_view::npos)
    return std::string();
  std::vector<cm::string_view> segments;
  bool bChanged = false;
  for (std::size_t iSegment = nRoot; iSegment <= _Path.size();) {
    std::size_t iEnd = _Path.find_first_of("/\\", iSegment);
    if (iEnd == cm::string_view::npos)
      iEnd = _Path.size();
    cm::string_view const segment = _Path.substr(iSegment, iEnd - iSegment);
    if (segment == "..") {
      if (segments.empty())
        return std::string();
      segments.pop_back();
      bChanged = true;
    } else if (segment == "." || (segment.empty() && iEnd != _Path.size()))
      bChanged = true;
    else if (!segment.empty())
      segments.push_back(segment);
    iSegment = iEnd + 1;
  }
  if (!bChanged)
    return std::string();
  char const separator = _Path.find('\\') != cm::string_view::npos ? '\\' : '/';
  std::string lexical(_Path.substr(0, nRoot));
  for (char &character : lexical) {
    if (fSeparator(character))
      character = separator;
  }
  for (std::size_t iSegment = 0; iSegment < segments.size(); ++iSegment) {
    if (iSegment)
      lexical += separator;
    lexical.append(segments[iSegment].data(), segments[iSegment].size());
  }
  return lexical;
}

// A path by its lexical form, where it has one
static std::string const &lexicalOrPath(std::string const &_Path, std::string &o_Lexical)
{
  o_Lexical = lexicalPath(_Path);
  return o_Lexical.empty() ? _Path : o_Lexical;
}

// The paths that the import maps in the texts of a project, sorted, also with the separators of Windows, as $<SHELL_PATH> and
// file(TO_NATIVE_PATH) write them. It is built again when the files of targets, the files beside them or the mapped outputs
// grow, which they only do.
cmExtraMalterlibGenerator::cmMappedPathSet const &cmExtraMalterlibGenerator::GetMappedPathSet(std::string const &_ProjectName)
{
  auto &targetFiles = TargetFiles[_ProjectName];
  auto &outputFiles = MappedOutputFiles[_ProjectName];
  auto &sidecarFiles = TargetSidecarFiles[_ProjectName];
  cmMappedPathSet &set = MappedPathSets[_ProjectName];
  std::size_t const nPaths = targetFiles.size() + outputFiles.size() + sidecarFiles.size();
  if (set.nPaths == nPaths)
    return set;
  set = cmMappedPathSet();
  set.nPaths = nPaths;
  auto fAdd = [&](std::string const &_Path, bool _bTargetFile) {
    for (bool bWindowsSeparators : { false, true }) {
      std::string spelling = _Path;
      if (bWindowsSeparators) {
        if (spelling.find('/') == std::string::npos)
          continue;
        std::replace(spelling.begin(), spelling.end(), '/', '\\');
      }
      if (spelling.empty())
        continue;
      set.StartsPath[static_cast<unsigned char>(spelling[0])] = true;
      set.Spellings.emplace_back(std::move(spelling), cmMappedPathSpelling{ &_Path, bWindowsSeparators, _bTargetFile });
    }
  };
  for (auto const &targetFile : targetFiles)
    fAdd(targetFile.first, true);
  for (std::string const &output : outputFiles)
    fAdd(output, false);
  // Also outside the build tree
  for (auto const &sidecar : sidecarFiles)
    fAdd(sidecar.first, false);
  // A file that a target links and a command writes, as a copy, is the file of the target
  std::stable_sort(set.Spellings.begin(), set.Spellings.end(),
                   [](cmMappedPathEntry const &_Left, cmMappedPathEntry const &_Right) { return _Left.first < _Right.first; });
  set.Spellings.erase(std::unique(set.Spellings.begin(), set.Spellings.end(),
                                  [](cmMappedPathEntry const &_Left, cmMappedPathEntry const &_Right) {
                                    return _Left.first == _Right.first;
                                  }),
                      set.Spellings.end());
  return set;
}

// The longest spelling of the set that the text starts with, where the text ends or has a character that names no file after
// it, so that a mapped path doesn't replace the start of another file, as of plugins.json in the directory of the output
// plugins. It is found as TCMultiReplace of Malterlib finds it: the largest spelling that sorts before the text either starts
// it, which makes it the longest that does, or shares a prefix with it, and only the spellings within that prefix can start
// the text.
//
// A path that the text writes with . or .. segments or doubled separators names the file of its lexical form, where that is
// longer than what the text starts with as it is.
cmExtraMalterlibGenerator::cmMappedPathSpelling const *cmExtraMalterlibGenerator::FindLongestMappedPath(
  cmMappedPathSet const &_Set, cm::string_view _Text, std::size_t &o_Size)
{
  if (_Text.empty() || !_Set.StartsPath[static_cast<unsigned char>(_Text[0])])
    return nullptr;
  cmMappedPathSpelling const *pFound = nullptr;
  o_Size = 0;
  cm::string_view prefix = _Text;
  while (!prefix.empty()) {
    auto iSpelling = std::upper_bound(_Set.Spellings.begin(), _Set.Spellings.end(), prefix,
                                      [](cm::string_view _Prefix, cmMappedPathEntry const &_Entry) { return _Prefix < _Entry.first; });
    if (iSpelling == _Set.Spellings.begin())
      break;
    --iSpelling;
    cm::string_view const spelling = iSpelling->first;
    if (cmHasPrefix(prefix, spelling)) {
      if (spelling.size() == _Text.size() || !namesFile(_Text[spelling.size()])) {
        o_Size = spelling.size();
        pFound = &iSpelling->second;
        break;
      }
      prefix = prefix.substr(0, spelling.size() - 1);
      continue;
    }
    std::size_t nCommon = 0;
    while (nCommon < spelling.size() && nCommon < prefix.size() && spelling[nCommon] == prefix[nCommon])
      ++nCommon;
    prefix = prefix.substr(0, nCommon);
  }

  // The path that the text starts with, up to what names no file other than a space, which a directory can have, of which each
  // prefix that ends before a separator, before a space or at its end is a path, the longest first
  auto const fSeparator = [](char _Character) { return _Character == '/' || _Character == '\\'; };
  std::size_t nPath = 0;
  bool bLexical = false;
  while (nPath < _Text.size() &&
         (namesFile(_Text[nPath]) || fSeparator(_Text[nPath]) || _Text[nPath] == ' ' || (nPath == 1 && _Text[nPath] == ':'))) {
    bLexical = bLexical || (nPath > 0 && fSeparator(_Text[nPath - 1]) && (_Text[nPath] == '.' || fSeparator(_Text[nPath])));
    ++nPath;
  }
  if (!bLexical)
    return pFound;
  for (std::size_t nPrefix = nPath; nPrefix > o_Size;) {
    std::string const lexical = lexicalPath(_Text.substr(0, nPrefix));
    if (!lexical.empty()) {
      auto iSpelling = std::lower_bound(
        _Set.Spellings.begin(), _Set.Spellings.end(), lexical,
        [](cmMappedPathEntry const &_Entry, std::string const &_Lexical) { return _Entry.first < _Lexical; });
      if (iSpelling != _Set.Spellings.end() && iSpelling->first == lexical) {
        o_Size = nPrefix;
        return &iSpelling->second;
      }
    }
    do
      --nPrefix;
    while (nPrefix > 0 && !fSeparator(_Text[nPrefix]) && _Text[nPrefix] != ' ');
  }
  return pFound;
}

std::string cmExtraMalterlibGenerator::replaceMappedOutputFiles(std::string const &_ProjectName, std::string const &_String,
                                                                MappedForm _Form)
{
  // The text around the mapped files of an eval string, as of a shell argument, is literal, where an @ is @@
  auto fLiteral = [&](cm::string_view _Text) {
    std::string literal(_Text);
    if (_Form != MappedForm::Expression)
      cmSystemTools::ReplaceString(literal, "@", "@@");
    return literal;
  };

  // Each path where it ends, as FindLongestMappedPath finds it, also with the separators of Windows, as $<SHELL_PATH> and
  // file(TO_NATIVE_PATH) write it, which it is mapped with.
  struct cmMappedMatch
  {
    std::string::size_type Start;
    std::string::size_type Size;
    std::string const *pPath; // Mapped where the match is kept, as mapping a file can make the target depend on another
    bool bWindowsSeparators;
    std::string Mapped;
  };
  std::vector<cmMappedMatch> matches;
  // The files that targets link, wherever the text names them, as of $<TARGET_FILE> in a script that a command runs, in a list
  // or in a definition, and the bundles of application bundles with what is in them, and the outputs of commands
  bool bTargetFile = false;
  {
    cmMappedPathSet const &set = GetMappedPathSet(_ProjectName);
    for (std::string::size_type iChar = 0; iChar < _String.size();) {
      std::size_t size = 0;
      cmMappedPathSpelling const *pSpelling = FindLongestMappedPath(set, cm::string_view(_String).substr(iChar), size);
      if (!pSpelling) {
        ++iChar;
        continue;
      }
      // A file in the directory of a bundle that a command writes or that the configure step wrote, rather than what the bundle has
      if (pSpelling->bTargetFile) {
        if (auto file = FindFileInDirectory(_ProjectName, _String, iChar + size, _String.size(), *pSpelling->pPath,
                                            pSpelling->bWindowsSeparators, _Form)) {
          matches.push_back({ iChar, size + file->nSize, nullptr, false, std::move(file->Mapped) });
          iChar += size + file->nSize;
          continue;
        }
      }
      bTargetFile = bTargetFile || pSpelling->bTargetFile;
      matches.push_back({ iChar, size, pSpelling->pPath, pSpelling->bWindowsSeparators, std::string() });
      iChar += size;
    }
  }

  // A path in the build tree is a mapped output as a whole, or none. A list of paths, as of the byproducts of a build event for
  // a script, has each of its paths mapped.
  if (!bTargetFile && StringStartsWithPath(_String, tempDir.c_str()) && _String.find(';') == std::string::npos &&
      !FindTargetDirectoryMarker(_String, 0)) {
    std::string lexical;
    if (MappedOutputFiles[_ProjectName].count(lexicalOrPath(_String, lexical)))
      return getMappedOutputFile(_ProjectName, _String, _Form);
    return fLiteral(_String);
  }
  // The directories of targets, which are the directories of the Malterlib targets where it builds them
  for (auto marker = FindTargetDirectoryMarker(_String, 0); marker; marker = FindTargetDirectoryMarker(_String, marker->iEnd)) {
    std::string::size_type const iStart = marker->iStart == std::string::npos ? marker->iMarker : marker->iStart;
    std::string const directory = _String.substr(iStart, marker->iEnd - iStart);
    // A file that the text names in the directory, up to where the next marker starts
    if (marker->iStart != std::string::npos) {
      auto const nextMarker = FindTargetDirectoryMarker(_String, marker->iEnd);
      std::string::size_type const iLimit = !nextMarker
        ? _String.size()
        : (nextMarker->iStart == std::string::npos ? nextMarker->iMarker : nextMarker->iStart);
      if (auto file = FindFileInDirectory(_ProjectName, _String, marker->iEnd, iLimit, cmSystemTools::GetFilenamePath(*marker->pPath),
                                          marker->bWindowsSeparators, _Form)) {
        matches.push_back({ iStart, marker->iEnd + file->nSize - iStart, nullptr, false, std::move(file->Mapped) });
        continue;
      }
    }
    std::string mapped = marker->iStart == std::string::npos
      ? std::string()
      : getMappedOutputFile(_ProjectName, directory, _Form, marker->bWindowsSeparators);
    if (!isDynamic(mapped))
      mapped = fLiteral(RemoveTargetDirectoryMarkers(directory));
    matches.push_back({ iStart, directory.size(), nullptr, false, std::move(mapped) });
  }
  if (matches.empty())
    return fLiteral(_String);

  // The longest of the files that start at the same place, and none in another
  std::sort(matches.begin(), matches.end(), [](cmMappedMatch const &_Left, cmMappedMatch const &_Right) {
    return _Left.Start != _Right.Start ? _Left.Start < _Right.Start : _Left.Size > _Right.Size;
  });
  std::string OutputString;
  std::string::size_type iLiteral = 0;
  for (cmMappedMatch const &match : matches) {
    if (match.Start < iLiteral)
      continue;
    OutputString += fLiteral(cm::string_view(_String).substr(iLiteral, match.Start - iLiteral));
    OutputString += match.pPath ? getMappedOutputFile(_ProjectName, *match.pPath, _Form, match.bWindowsSeparators) : match.Mapped;
    iLiteral = match.Start + match.Size;
  }
  OutputString += fLiteral(cm::string_view(_String).substr(iLiteral));

  return OutputString;
}

// The property with the file that a target of the import links, in the target that AppendTarget adds, which is the file of
// the target itself or of one of its dependencies, as CMake makes a target depend on the targets that its commands use. The
// build events of a target use the file that it links, except its POST_BUILD, which runs once the Malterlib build has signed and
// deployed it, and other targets the file that it has deployed, as only that one runs where executables have to be signed.
// Without _bOrdersBuild, where a path only names where the file is, as a working directory, the target doesn't build after it.
std::string cmExtraMalterlibGenerator::GetTargetFileExpression(std::string const &_ProjectName, std::string const &_File,
                                                              TargetFileVersion _Version, bool _bOrdersBuild)
{
  auto &targetFiles = TargetFiles[_ProjectName];
  auto iFile = targetFiles.find(_File);
  if (iFile == targetFiles.end())
    return std::string();
  cmMalterlibTargetFile const &targetFile = iFile->second;
  bool const bDeployed = _Version == TargetFileVersion::Deployed ||
    (_Version == TargetFileVersion::ForTarget && (targetFile.TargetName != CurrentTargetName || MapOwnTargetToDeployed));
  // The bundle of an application bundle, as of $<TARGET_BUNDLE_DIR>, only names a directory, which doesn't order the build
  bool const bOrdersBuild = _bOrdersBuild && !TargetBundleDirectories[_ProjectName].count(_File);
  return GetTargetPropertyExpression(targetFile, bDeployed ? targetFile.FinalProperty : targetFile.Property, bOrdersBuild);
}

// A property of the Malterlib target of a target file, of the target itself or of a dependency, which a target of another
// file depends on
std::string cmExtraMalterlibGenerator::GetTargetPropertyExpression(cmMalterlibTargetFile const &_TargetFile,
                                                                  std::string const &_Property, bool _bOrdersBuild)
{
  if (_TargetFile.TargetName == CurrentTargetName)
    return _Property;
  (_bOrdersBuild && !MapPathsOnly ? TargetFileDependencies : TargetPathDependencies).insert(_TargetFile.TargetName);
  std::string dependencyPath = cmStrCat("Dependency:", _TargetFile.TargetName);
  // As CEntity::f_GetPathForGetProperty escapes a name with these
  if (_TargetFile.TargetName.find_first_of(".\"") != std::string::npos)
    dependencyPath = cmMalterlibRegistry::getEscaped(dependencyPath, true, false);
  return cmStrCat('&', _Property, "->GetProperty(&Dependency.TargetPropertyPath->GetProperty(",
                  cmMalterlibRegistry::getEscaped(dependencyPath, true, false), "))");
}

// A file that the build events of a target write beside the file that it links, by its name with an extension, as a stamp that a
// POST_BUILD writes to $<TARGET_FILE>.stamp, or in its directory as $<TARGET_FILE_DIR> (CollectBuildEventFiles), is beside the
// file of the Malterlib target that the event that writes it runs on:
// the linked file before the link, and for a POST_BUILD the deployed one, unless Target.CMakePostBuildOnLinkedFile makes it run
// on the linked file. In the POST_BUILD of the target itself, that is the file that the POST_BUILD names.
std::string cmExtraMalterlibGenerator::GetTargetSidecarExpression(std::string const &_ProjectName, std::string const &_File)
{
  auto &sidecarFiles = TargetSidecarFiles[_ProjectName];
  auto iSidecar = sidecarFiles.find(_File);
  if (iSidecar == sidecarFiles.end())
    return std::string();
  cmMalterlibSidecarFile const &sidecar = iSidecar->second;
  std::string const &targetFile = sidecar.TargetFile;
  std::string rawRest =
    _File.substr(sidecar.bInDirectory ? cmSystemTools::GetFilenamePath(targetFile).size() : targetFile.size());
  std::string rest;
  cmSystemTools::ReplaceString(rawRest, "@", "@@");
  cmMalterlibRegistry::addEscapeStr(rest, rawRest, "`\\", "`\\", false);
  char const *pSuffix = sidecar.bInDirectory ? "->GetPath()" : "";
  cmMalterlibTargetFile const &owner = TargetFiles[_ProjectName][targetFile];
  if (!sidecar.bPostBuild) {
    return cmStrCat("`@(", GetTargetFileExpression(_ProjectName, targetFile, TargetFileVersion::Linked), pSuffix, ')', rest,
                    '`');
  }
  if (InOwnPostBuild && owner.TargetName == CurrentTargetName)
    return cmStrCat("`@(", GetTargetFileExpression(_ProjectName, targetFile), pSuffix, ')', rest, '`');
  return cmStrCat("`@(((", GetTargetPropertyExpression(owner, "Target.CMakePostBuildOnLinkedFile"), ") ? (",
                  GetTargetFileExpression(_ProjectName, targetFile, TargetFileVersion::Linked), ") : (",
                  GetTargetFileExpression(_ProjectName, targetFile, TargetFileVersion::Deployed), "))", pSuffix, ')', rest,
                  '`');
}

namespace {
// While it lives, the directories of targets in what generator expressions evaluate to end with their markers
struct cmMarkersScope
{
  explicit cmMarkersScope(std::vector<std::string> *pDirectories) { cmMalterlibTargetDirectories = pDirectories; }
  ~cmMarkersScope() { cmMalterlibTargetDirectories = nullptr; }
  cmMarkersScope(cmMarkersScope const &) = delete;
  cmMarkersScope &operator=(cmMarkersScope const &) = delete;
};
}

// The values of an evaluation for what the import writes, with the directories of targets ending with their markers
// (cmMalterlibTargetDirectories) where the generator maps them, as the evaluation runs twice: once without the markers, and once
// with them, whose values are taken where they are those without them once the markers are removed. A generator expression that
// computes something else from such a directory, as $<PATH:IS_PREFIX,...>, gets it without its marker. Where the evaluation
// with the markers fails, as where they make a condition take a branch that names a target that doesn't exist, the values are
// those without them.
template <typename t_CValue>
t_CValue cmExtraMalterlibGenerator::EvaluateMarkedValue(std::function<t_CValue()> const &_fEvaluate)
{
  t_CValue plain = _fEvaluate();
  cmMalterlibMarkedEvaluationFailed = false;
  t_CValue marked;
  {
    cmMarkersScope const markers(&TargetDirectories);
    marked = _fEvaluate();
  }
  if (!cmMalterlibMarkedEvaluationFailed)
    TakeMarked(plain, std::move(marked));
  return plain;
}

std::string cmExtraMalterlibGenerator::EvaluateMarkedString(std::function<std::string()> const &_fEvaluate)
{
  return EvaluateMarkedValue(_fEvaluate);
}

std::vector<std::string> cmExtraMalterlibGenerator::EvaluateMarkedList(std::function<std::vector<std::string>()> const &_fEvaluate)
{
  return EvaluateMarkedValue(_fEvaluate);
}

std::set<std::string> cmExtraMalterlibGenerator::EvaluateMarkedSet(std::function<std::set<std::string>()> const &_fEvaluate)
{
  return EvaluateMarkedValue(_fEvaluate);
}

// Whether a value of the marked evaluation is the value of the evaluation without the markers: with each marker removed, or with
// the .. after it resolved where the evaluation collapsed that path, each on its own, as a value can name a path that CMake collapsed
// beside one that it didn't
bool cmExtraMalterlibGenerator::IsMarkedOf(std::string const &_Marked, std::string const &_Plain) const
{
  return MatchMarked(_Marked, 0, _Plain, 0);
}

bool cmExtraMalterlibGenerator::MatchMarked(std::string const &_Marked, std::string::size_type _iMarked, std::string const &_Plain,
                                            std::string::size_type _iPlain) const
{
  auto const fSeparator = [](char _Character) { return _Character == '/' || _Character == '\\'; };
  auto const marker = FindTargetDirectoryMarker(_Marked, _iMarked);
  std::string::size_type const iLiteralEnd =
    !marker ? _Marked.size() : (marker->iStart == std::string::npos || marker->iStart < _iMarked ? marker->iMarker : marker->iStart);
  std::string::size_type const nLiteral = iLiteralEnd - _iMarked;
  if (_Plain.compare(_iPlain, nLiteral, _Marked, _iMarked, nLiteral) != 0)
    return false;
  _iPlain += nLiteral;
  if (!marker)
    return _iPlain == _Plain.size();
  if (marker->iStart == std::string::npos || marker->iStart < _iMarked)
    return MatchMarked(_Marked, marker->iEnd, _Plain, _iPlain);

  // As written, without the marker
  std::string::size_type const nDirectory = marker->iMarker - marker->iStart;
  if (_Plain.compare(_iPlain, nDirectory, _Marked, marker->iStart, nDirectory) == 0 &&
      MatchMarked(_Marked, marker->iEnd, _Plain, _iPlain + nDirectory))
    return true;

  // With the .. after the marker resolved from its directory
  std::string directory = cmSystemTools::GetFilenamePath(*marker->pPath);
  std::string::size_type iRest = marker->iEnd;
  while (iRest + 2 < _Marked.size() && fSeparator(_Marked[iRest]) && _Marked[iRest + 1] == '.' && _Marked[iRest + 2] == '.' &&
         (iRest + 3 == _Marked.size() || fSeparator(_Marked[iRest + 3]) || !namesFile(_Marked[iRest + 3]))) {
    directory = cmSystemTools::GetFilenamePath(directory);
    iRest += 3;
  }
  if (iRest == marker->iEnd)
    return false;
  if (marker->bWindowsSeparators)
    std::replace(directory.begin(), directory.end(), '/', '\\');
  return _Plain.compare(_iPlain, directory.size(), directory) == 0 && MatchMarked(_Marked, iRest, _Plain, _iPlain + directory.size());
}

void cmExtraMalterlibGenerator::TakeMarked(std::string &o_Plain, std::string &&_Marked) const
{
  if (IsMarkedOf(_Marked, o_Plain))
    o_Plain = std::move(_Marked);
}

// A list as a whole where the markers only make values distinct that the evaluation without them leaves out as duplicates, as
// the directories of targets that CMake puts in the same directory, which Malterlib builds in their own, and otherwise value by
// value, in the order of the list without them
void cmExtraMalterlibGenerator::TakeMarked(std::vector<std::string> &o_Plain, std::vector<std::string> &&_Marked) const
{
  // As a whole, also where values that the markers make distinct are one value without them
  std::vector<std::string> unique;
  std::set<std::string> seen;
  for (std::string const &marked : _Marked) {
    if (seen.insert(RemoveTargetDirectoryMarkers(marked)).second)
      unique.push_back(marked);
  }
  for (std::vector<std::string> const *pList : { &_Marked, &unique }) {
    if (pList->size() != o_Plain.size())
      continue;
    bool bWhole = true;
    for (std::size_t iValue = 0; bWhole && iValue < o_Plain.size(); ++iValue)
      bWhole = IsMarkedOf((*pList)[iValue], o_Plain[iValue]);
    if (bWhole) {
      o_Plain = std::move(_Marked);
      return;
    }
  }
  std::vector<bool> taken(_Marked.size(), false);
  for (std::string &plain : o_Plain) {
    for (std::size_t iMarked = 0; iMarked < _Marked.size(); ++iMarked) {
      if (taken[iMarked] || !IsMarkedOf(_Marked[iMarked], plain))
        continue;
      taken[iMarked] = true;
      plain = std::move(_Marked[iMarked]);
      break;
    }
  }
}

// Value by value, as of definitions, which the markers can't make more of: a macro is defined once
void cmExtraMalterlibGenerator::TakeMarked(std::set<std::string> &o_Plain, std::set<std::string> &&_Marked) const
{
  std::set<std::string> values;
  std::set<std::string> taken;
  for (std::string const &marked : _Marked) {
    for (std::string const &plain : o_Plain) {
      if (!taken.count(plain) && IsMarkedOf(marked, plain)) {
        taken.insert(plain);
        values.insert(marked);
        break;
      }
    }
  }
  for (std::string const &plain : o_Plain) {
    if (!taken.count(plain))
      values.insert(plain);
  }
  o_Plain = std::move(values);
}

// The generator of a custom command, evaluated as EvaluateMarkedValue evaluates, where the command uses the directories of
// targets as they are, as an argument or a part of one
cmCustomCommandGenerator cmExtraMalterlibGenerator::MakeCommandGenerator(cmCustomCommand const &_Command,
                                                                        std::string const &_Config, cmLocalGenerator *lg)
{
  cmCustomCommandGenerator generator(_Command, _Config, lg);
  {
    cmMalterlibMarkedEvaluationFailed = false;
    cmMarkersScope const markers(&TargetDirectories);
    cmCustomCommandGenerator marked(_Command, _Config, lg);
    if (cmMalterlibMarkedEvaluationFailed)
      return generator;
    generator.TakeMalterlibMarkedValues(std::move(marked), [this](std::string const &_Marked, std::string const &_Plain) {
      return IsMarkedOf(_Marked, _Plain);
    });
  }
  return generator;
}

// The first marker of a directory of a target in a text from an index, if any, with where the directory that it ends starts,
// which is unknown where something changed the text before it, and where the marker ends. The directory can also have the
// separators of Windows, as $<SHELL_PATH> writes it.
cm::optional<cmExtraMalterlibGenerator::cmTargetDirectoryMarker>
cmExtraMalterlibGenerator::FindTargetDirectoryMarker(std::string const &_Text, std::string::size_type _iFrom) const
{
  cm::string_view const prefix = ".malterlib-file-dir-";
  for (auto iMarker = _Text.find(prefix.data(), _iFrom, prefix.size()); iMarker != std::string::npos;
       iMarker = _Text.find(prefix.data(), iMarker + 1, prefix.size())) {
    std::string::size_type const iIndex = iMarker + prefix.size();
    std::string::size_type iEnd = iIndex;
    std::size_t index = 0;
    while (iEnd < _Text.size() && _Text[iEnd] >= '0' && _Text[iEnd] <= '9')
      index = index * 10 + std::size_t(_Text[iEnd++] - '0');
    if (iEnd == iIndex || index >= TargetDirectories.size())
      continue;
    std::string const &path = TargetDirectories[index];
    std::string const directory = cmSystemTools::GetFilenamePath(path);
    cmTargetDirectoryMarker marker{ std::string::npos, iMarker, iEnd, &path, false };
    if (iMarker >= directory.size()) {
      std::string::size_type const iStart = iMarker - directory.size();
      if (_Text.compare(iStart, directory.size(), directory) == 0)
        marker.iStart = iStart;
      else {
        std::string windowsDirectory = directory;
        std::replace(windowsDirectory.begin(), windowsDirectory.end(), '/', '\\');
        if (_Text.compare(iStart, windowsDirectory.size(), windowsDirectory) == 0) {
          marker.iStart = iStart;
          marker.bWindowsSeparators = true;
        }
      }
    }
    return marker;
  }
  return cm::nullopt;
}

// A text with the directories of targets that GetTargetPathExpression doesn't map, as of targets that the import doesn't build,
// as they are in the build tree of CMake
std::string cmExtraMalterlibGenerator::RemoveTargetDirectoryMarkers(std::string _Text) const
{
  for (auto marker = FindTargetDirectoryMarker(_Text, 0); marker; marker = FindTargetDirectoryMarker(_Text, marker->iMarker))
    _Text.erase(marker->iMarker, marker->iEnd - marker->iMarker);
  return _Text;
}

// Whether a path names a file that the configure step wrote, which the import cache holds, also in the directory of a target:
// one of configure_file and file(GENERATE), or any other file there, as of file(WRITE), file(COPY) and execute_process, as the
// build tree that the import configures in is new and nothing is built in it, by any spelling of the path
bool cmExtraMalterlibGenerator::IsConfigureFile(std::string const &_Path) const
{
  std::string path = _Path;
  std::replace(path.begin(), path.end(), '\\', '/');
  std::string lexical;
  std::string const &file = lexicalOrPath(path, lexical);
  return ConfigureOutputFiles.count(file) || (cmSystemTools::FileExists(file, true) && StringStartsWithPath(file, tempDir.c_str()));
}

// A file that the text names in a directory of a target or a bundle, from _iPath, which starts with a separator, up to _iLimit: the
// longest path that it names, up to a separator or a space, which a directory can have, or where it names no more file, by its
// lexical form. An output or byproduct of a command is mapped as the output, as the build writes it wherever the text names it, and
// a file that the configure step wrote is in the import cache, by its lexical form, which the cache has its directories for.
cm::optional<cmExtraMalterlibGenerator::cmFileInDirectory> cmExtraMalterlibGenerator::FindFileInDirectory(
  std::string const &_ProjectName, std::string const &_String, std::string::size_type _iPath, std::string::size_type _iLimit,
  std::string const &_Directory, bool _bWindowsSeparators, MappedForm _Form)
{
  auto const fSeparator = [](char _Character) { return _Character == '/' || _Character == '\\'; };
  if (_iPath >= _iLimit || !fSeparator(_String[_iPath]))
    return cm::nullopt;
  std::string::size_type iPathEnd = _iPath;
  while (iPathEnd < _iLimit && (namesFile(_String[iPathEnd]) || fSeparator(_String[iPathEnd]) || _String[iPathEnd] == ' '))
    ++iPathEnd;
  auto const &outputFiles = MappedOutputFiles[_ProjectName];
  for (std::string::size_type iEnd = iPathEnd; iEnd > _iPath + 1;) {
    if (!fSeparator(_String[iEnd - 1]) && _String[iEnd - 1] != ' ') {
      std::string path = _Directory + _String.substr(_iPath, iEnd - _iPath);
      std::replace(path.begin(), path.end(), '\\', '/');
      std::string lexical;
      std::string file = lexicalOrPath(path, lexical);
      if (outputFiles.count(file))
        return cmFileInDirectory{ iEnd - _iPath, getMappedOutputFile(_ProjectName, file, _Form, _bWindowsSeparators) };
      if (IsConfigureFile(file)) {
        if (_bWindowsSeparators)
          std::replace(file.begin(), file.end(), '/', '\\');
        if (_Form != MappedForm::Expression)
          cmSystemTools::ReplaceString(file, "@", "@@");
        return cmFileInDirectory{ iEnd - _iPath, std::move(file) };
      }
    }
    do
      --iEnd;
    while (iEnd > _iPath + 1 && !fSeparator(_String[iEnd]) && _String[iEnd] != ' ');
  }
  return cm::nullopt;
}

// The directory of a target that a path is in, if any, as GetTargetPathExpression maps it
cmExtraMalterlibGenerator::cmTargetPath cmExtraMalterlibGenerator::FindTargetPath(std::string const &_ProjectName,
                                                                                  std::string const &_Path) const
{
  auto fInDirectory = [&](std::string::size_type _iEnd) {
    return _Path.size() == _iEnd || _Path[_iEnd] == '/' || _Path[_iEnd] == '\\';
  };
  // An output or byproduct of a command is mapped as the output, and a file that the configure step wrote is in the import cache,
  // as the other files that it writes, also in a directory of a target or a bundle
  {
    std::string plain = RemoveTargetDirectoryMarkers(_Path);
    std::replace(plain.begin(), plain.end(), '\\', '/');
    std::string lexical;
    std::string const &file = lexicalOrPath(plain, lexical);
    auto const iOutputs = MappedOutputFiles.find(_ProjectName);
    if ((iOutputs != MappedOutputFiles.end() && iOutputs->second.count(file)) || IsConfigureFile(file))
      return cmTargetPath{};
  }
  auto const marker = FindTargetDirectoryMarker(_Path, 0);
  if (marker && marker->iStart == 0 && fInDirectory(marker->iEnd)) {
    std::string rest = RemoveTargetDirectoryMarkers(_Path.substr(marker->iEnd));
    auto const iFiles = TargetFiles.find(_ProjectName);
    if (iFiles != TargetFiles.end() && iFiles->second.count(*marker->pPath))
      return cmTargetPath{ cmTargetPath::EKind::FileDirectory, *marker->pPath, std::move(rest) };
    return cmTargetPath{};
  }
  auto const iBundles = TargetBundleDirectories.find(_ProjectName);
  if (iBundles != TargetBundleDirectories.end()) {
    for (std::string const &bundleDirectory : iBundles->second) {
      if (cmHasPrefix(_Path, bundleDirectory) && fInDirectory(bundleDirectory.size()))
        return cmTargetPath{ cmTargetPath::EKind::Bundle, bundleDirectory, _Path.substr(bundleDirectory.size()) };
    }
  }
  return cmTargetPath{};
}

// A path in a directory of a target, which names that directory without ordering the build after the target, as CMake does
// (CMP0112), as an expression:
// - in the directory of a file of the target, as of $<TARGET_FILE_DIR>, the directory of the file of the Malterlib target;
// - in the bundle of an application bundle, as of $<TARGET_BUNDLE_DIR> and $<TARGET_BUNDLE_CONTENT_DIR>, the bundle that Malterlib
//   builds.
std::string cmExtraMalterlibGenerator::GetTargetPathExpression(std::string const &_ProjectName, std::string const &_Path)
{
  cmTargetPath targetPath = FindTargetPath(_ProjectName, _Path);
  std::string base;
  switch (targetPath.Kind) {
    case cmTargetPath::EKind::None:
      return std::string();
    case cmTargetPath::EKind::FileDirectory:
      base = cmStrCat(GetTargetFileExpression(_ProjectName, targetPath.Key, TargetFileVersion::ForTarget, false), "->GetPath()");
      break;
    case cmTargetPath::EKind::Bundle:
      base = GetTargetFileExpression(_ProjectName, targetPath.Key, TargetFileVersion::ForTarget, false);
      break;
  }
  std::string escapedRest;
  cmSystemTools::ReplaceString(targetPath.Rest, "@", "@@");
  cmMalterlibRegistry::addEscapeStr(escapedRest, targetPath.Rest, "`\\", "`\\", false);
  return cmStrCat("`@(", base, ')', escapedRest, '`');
}

// With _bWindowsSeparators, the path is written with the separators of Windows, as the text that names it does
std::string cmExtraMalterlibGenerator::getMappedOutputFile(std::string const &_ProjectName, std::string const &_String,
                                                           MappedForm _Form, bool _bWindowsSeparators)
{
  std::string lexical;
  std::string const *pPath = &lexicalOrPath(_String, lexical);
  // An output or byproduct of a command that the text names in a directory of a target is mapped as the output
  std::string plainLexical;
  if (FindTargetDirectoryMarker(_String, 0)) {
    std::string plain = RemoveTargetDirectoryMarkers(_String);
    std::replace(plain.begin(), plain.end(), '\\', '/');
    std::string plainLexicalForm;
    plainLexical = lexicalOrPath(plain, plainLexicalForm);
    if (MappedOutputFiles[_ProjectName].count(plainLexical))
      pPath = &plainLexical;
  }
  std::string const &path = *pPath;
  std::string expression = GetTargetFileExpression(_ProjectName, path);
  if (expression.empty())
    expression = GetTargetPathExpression(_ProjectName, path);
  if (expression.empty()) {
    bool const bMappedOutput = StringStartsWithPath(path, tempDir.c_str()) && MappedOutputFiles[_ProjectName].count(path);
    if (!bMappedOutput && !TargetSidecarFiles[_ProjectName].count(path)) {
      std::string unmapped = RemoveTargetDirectoryMarkers(_String);
      if (_bWindowsSeparators)
        std::replace(unmapped.begin(), unmapped.end(), '/', '\\');
      return unmapped;
    }
    expression = GetTargetSidecarExpression(_ProjectName, path);
    if (expression.empty())
      expression = cmMalterlibRegistry::getEscaped(path, true, true);
  }

  char const *pSeparators = _bWindowsSeparators ? "->WindowsPath()" : "";
  switch (_Form) {
    case MappedForm::Expression:
      return cmStrCat(expression, "->MakeAbsolute()", pSeparators);
    case MappedForm::EvalString:
      return cmStrCat("@(", expression, "->MakeAbsolute()", pSeparators, ')');
    case MappedForm::ShellArgument:
      return cmStrCat("@(", expression, "->MakeAbsolute()", pSeparators,
                      escapeSuffix(GlobalGenerator->GetCMakeInstance()->GetState()));
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

  auto &toReturn = addAtRegistry->addChild("%File", getMappedOutputFile(_ProjectName, fileName, MappedForm::Expression));

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

      std::string lexical;
      Output = lexicalOrPath(Output, lexical);
      if (StringStartsWithPath(Output, tempDir.c_str())) {
        //std::cout << "Adding mapped: " << Output << "\n";
        MappedOutputFiles[_ProjectName].insert(Output);
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

      cmCustomCommandGenerator customCommandGenerator = MakeCommandGenerator(*customCommand, configName, lg);

      // The outputs of a command that runs nothing in the configuration, which CMake's Ninja makes phony, stand for the files
      // that it depends on in the commands that depend on them
      if (runsNothing(customCommandGenerator)) {
        std::vector<std::string> dependencies;
        for (std::string const &dependency : customCommandGenerator.GetDepends()) {
          std::string realDependency;
          if (lg->GetRealDependency(dependency, configName, realDependency,
                                    customCommandGenerator.GetCC().GetCMP0212Status()))
            dependencies.push_back(std::move(realDependency));
        }
        std::string lexical;
        for (std::string const &output : customCommandGenerator.GetOutputs())
          PhonyOutputDependencies[_ProjectName][lexicalOrPath(output, lexical)] = dependencies;
        for (std::string const &byproduct : customCommandGenerator.GetByproducts())
          PhonyOutputDependencies[_ProjectName][lexicalOrPath(byproduct, lexical)] = dependencies;
      }

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
            symbolic = true;
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
      if (!argument.bRaw) {
        cmMalterlibRegistry::addEscapeStr(launcher, text, "`\\\r\n\t", "`\\rnt", false);
        continue;
      }
      // The files that targets link and the outputs of custom commands in an argument that the shell expands are mapped in
      // place, wherever they end in it, the longest that matches at each position, as replaceMappedOutputFiles maps them. Within
      // the quotes of the argument the shell takes them as they are, so they are only escaped for it outside of quotes.
      cmMappedPathSet const &set = GetMappedPathSet(_ProjectName);
      std::string const &rawText = argument.Text;
      std::string mappedText;
      bool bInSingleQuotes = false;
      bool bInDoubleQuotes = false;
      bool const bWindowsShell = localGenerator->GetState()->UseWindowsShell();
      std::string const suffix = escapeSuffix(localGenerator->GetState());
      auto fAddLiteral = [&](char _Character) {
        mappedText += _Character;
        if (_Character == '@')
          mappedText += '@';
      };
      for (size_t iChar = 0; iChar < rawText.size();) {
        std::size_t size = 0;
        cmMappedPathSpelling const *pSpelling = FindLongestMappedPath(set, cm::string_view(rawText).substr(iChar), size);
        if (!pSpelling) {
          char const character = rawText[iChar];
          if (!bWindowsShell && character == '\\' && !bInSingleQuotes && iChar + 1 < rawText.size()) {
            fAddLiteral(character);
            fAddLiteral(rawText[iChar + 1]);
            iChar += 2;
            continue;
          }
          if (!bWindowsShell && character == '\'' && !bInDoubleQuotes)
            bInSingleQuotes = !bInSingleQuotes;
          if (character == '"' && !bInSingleQuotes)
            bInDoubleQuotes = !bInDoubleQuotes;
          fAddLiteral(character);
          ++iChar;
          continue;
        }
        std::string const mapped =
          getMappedOutputFile(_ProjectName, *pSpelling->pPath, MappedForm::ShellArgument, pSpelling->bWindowsSeparators);
        if ((bInSingleQuotes || bInDoubleQuotes) && cmHasSuffix(mapped, suffix))
          mappedText += cmStrCat(cm::string_view(mapped).substr(0, mapped.size() - suffix.size()), ')');
        else
          mappedText += mapped;
        iChar += size;
      }
      addEvalStringText(launcher, mappedText);
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

  // The files are mapped by their paths wherever the argument names them, as a file that a target of the import links, as
  // $<TARGET_FILE> or as the target that a command runs, also in the value of an argument that assigns it or in a list, which
  // is the file that its Malterlib target links. A path that isn't mapped is the string of an expression, where an @ is itself.
  std::string const mapped = replaceMappedOutputFiles(_ProjectName, _String, MappedForm::ShellArgument);
  if (!isDynamic(mapped) &&
      (StringStartsWithPath(_String, baseDir.c_str()) || StringStartsWithPath(_String, binaryDir.c_str())))
    return makeAbsoluteWrapperEvalString(localGenerator->GetState(), RemoveTargetDirectoryMarkers(_String));

  return mapped;
}

// The command lines of a custom command, joined with &&, with its files and those that the targets of the import link mapped
// to the files of the Malterlib build
// The other files, of the commands that the command runs with, as the byproducts of the other build events of its target, are
// files of the command too
std::string cmExtraMalterlibGenerator::GetCustomCommandLines(std::string const &_ProjectName, cmLocalGenerator *lg,
                                                            cmCustomCommandGenerator const &customCommandGenerator,
                                                            std::string const &configName,
                                                            std::string const &commandWorkingDirectory,
                                                            std::set<std::string> const &_OtherFiles)
{
  // A relative path in the working directory of the command that names one of its outputs, or a file that it depends on
  // that the build generates, is the path of that file, which the import maps to the intermediate directory of the build,
  // while the command runs in the directory of the import cache. Other arguments are left as they are, as they need not
  // be paths, and so is a command that doesn't name a path, which the shell looks up in PATH.
  std::set<std::string> commandFiles = _OtherFiles;
  for (std::string const& file : customCommandGenerator.GetOutputs())
    commandFiles.insert(cmSystemTools::CollapseFullPath(file));
  for (std::string const& file : customCommandGenerator.GetByproducts())
    commandFiles.insert(cmSystemTools::CollapseFullPath(file));
  // The dependencies, also those of the outputs of commands that run nothing, which stand for them
  std::vector<std::string> realDependencies;
  std::set<std::string> visitedDependencies;
  for (std::string const& dependency : customCommandGenerator.GetDepends()) {
    std::string realDependency;
    if (lg->GetRealDependency(dependency, configName, realDependency, customCommandGenerator.GetCC().GetCMP0212Status())) {
      realDependency = cmSystemTools::CollapseFullPath(realDependency);
      commandFiles.insert(realDependency);
      ResolvePhonyDependency(_ProjectName, realDependency, realDependencies, visitedDependencies);
    }
  }
  for (std::string const& dependency : realDependencies)
    commandFiles.insert(cmSystemTools::CollapseFullPath(dependency));
  // The files in directories of targets, by their paths in the build tree of CMake, which a relative path names
  std::map<std::string, std::string> targetPathFiles;
  for (std::string const& file : commandFiles) {
    if (FindTargetPath(_ProjectName, file).Kind != cmTargetPath::EKind::None)
      targetPathFiles.emplace(RemoveTargetDirectoryMarkers(file), file);
  }
  auto fCommandFile = [&](std::string const& _Path) -> std::string {
    if (_Path.empty() || cmSystemTools::FileIsFullPath(_Path))
      return std::string();
    std::string path = cmSystemTools::CollapseFullPath(_Path, commandWorkingDirectory);
    auto const iFile = targetPathFiles.find(RemoveTargetDirectoryMarkers(path));
    if (iFile != targetPathFiles.end())
      return iFile->second;
    return commandFiles.count(path) && MappedOutputFiles[_ProjectName].count(path) ? path : std::string();
  };
  // Also the value of an argument that assigns it, as -DOUT=<file> for a script of CMake
  auto fOutputPath = [&](std::string const& _Param) -> std::string {
    std::string path = fCommandFile(_Param);
    if (!path.empty())
      return path;
    auto iAssign = _Param.find('=');
    if (iAssign != std::string::npos) {
      path = fCommandFile(_Param.substr(iAssign + 1));
      if (!path.empty())
        return _Param.substr(0, iAssign + 1) + path;
    }
    return _Param;
  };

  std::string launcher = this->MakeCustomLauncher(_ProjectName, lg, customCommandGenerator);

  std::string commandLines;

  for (unsigned i = 0; i != customCommandGenerator.GetNumberOfCommands(); ++i)
  {
    std::string const& commandName = customCommandGenerator.GetCommand(i);
    // As in upstream's Ninja generator, as of a generator expression that gives no command
    if (commandName.empty())
      continue;
    auto command = ConvertCommandParam(
      _ProjectName, lg, commandName.find_first_of("/\\") == std::string::npos ? commandName : fOutputPath(commandName));
    std::string commandLine = launcher;
    // A batch file that another one runs without call ends it, as the scripts of Visual Studio are, so CMake's generators for
    // the shell of Windows call them
    if (launcher.empty() && lg->GetState()->UseWindowsShell() && commandName.size() > 4) {
      std::string const extension = cmSystemTools::LowerCase(commandName.substr(commandName.size() - 4));
      if (extension == ".bat" || extension == ".cmd")
        commandLine += "call ";
    }

    // Escaped for the shell only, as the arguments are: the script runs the command itself, so the escaping that CMake's
    // generators add for Ninja or Make, as of a $, would change it
    if (!isDynamic(command)) {
      cmMalterlibRegistry::addEscapeStr(commandLine, escapeForShell(lg, lg->ConvertDirectorySeparatorsForShell(command)),
                                        "`\\\r\n\t", "`\\rnt", false);
    } else {
      // The literal parts of a command with mapped paths, as a name with a space after $<TARGET_FILE_DIR>, escaped as an argument
      commandLine += escapeDynamicArgument(lg, command, customCommandGenerator.GetCC().GetEscapeOldStyle());
    }

    customCommandGenerator.AppendArguments
      (
        i
        , commandLine
        , [&](std::string const &_Param, bool &o_bEscape) -> std::string
        {
          // Escaped here for the shell only, as the Malterlib build system escapes for Ninja itself
          o_bEscape = false;
          std::string const converted = ConvertCommandParam(_ProjectName, lg, fOutputPath(_Param));
          return escapeDynamicArgument(lg, converted, customCommandGenerator.GetCC().GetEscapeOldStyle());
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
  return commandLines;
}

// The build events of a target run where CMake's Ninja generator runs them, in every Malterlib generator: PRE_BUILD and then
// PRE_LINK after the compiles and before the link, and POST_BUILD after the link, in the command that links, before the Malterlib
// build copies the linked file to its final destination. The scripts that run them are batch files for Visual Studio on Windows
// (ScriptWindowsBat) and bash scripts elsewhere.
void cmExtraMalterlibGenerator::AddBuildEvents(std::string const &_ProjectName, cmMalterlibRegistry &outputTarget,
                                               cmLocalGenerator *lg, cmGeneratorTarget const *target,
                                               std::string const &configName, bool isUtilityTarget)
{
  struct cmMalterlibBuildEventScript
  {
    std::string Bash;
    std::string Bat;
    std::vector<std::string> Outputs;
  };
  // The byproducts of the build events of the target, which one event can write and another read by their relative paths
  std::set<std::string> eventFiles;
  for (auto const *pCommands :
       { &target->GetPreBuildCommands(), &target->GetPreLinkCommands(), &target->GetPostBuildCommands() }) {
    for (cmCustomCommand const &command : *pCommands) {
      cmCustomCommandGenerator const commandGenerator = MakeCommandGenerator(command, configName, lg);
      for (std::string const &byproduct : commandGenerator.GetByproducts())
        eventFiles.insert(cmSystemTools::CollapseFullPath(byproduct));
    }
  }
  auto fScript = [&](std::vector<cmCustomCommand> const &commands) -> cmMalterlibBuildEventScript {
    cmMalterlibBuildEventScript script;
    for (cmCustomCommand const &command : commands) {
      cmCustomCommandGenerator commandGenerator = MakeCommandGenerator(command, configName, lg);
      std::string workingDirectory = commandGenerator.GetWorkingDirectory();
      if (workingDirectory.empty())
        workingDirectory = lg->GetCurrentBinaryDirectory();
      std::string const commandLines =
        GetCustomCommandLines(_ProjectName, lg, commandGenerator, configName, workingDirectory, eventFiles);
      if (commandLines.empty())
        continue;
      // Bash runs the bash script also on Windows, where the other commands are escaped for the shell of Windows
      std::string bashCommandLines = commandLines;
      std::string bashSuffix = escapeSuffix(lg->GetState());
      if (lg->GetState()->UseWindowsShell()) {
        cmMalterlibUnixShellScope unixShell(lg->GetState());
        bashCommandLines = GetCustomCommandLines(_ProjectName, lg, commandGenerator, configName, workingDirectory, eventFiles);
        bashSuffix = escapeSuffix(lg->GetState());
      }
      // The directories of the files that the command writes, as CollectBuildEventFiles finds them, and its working directory
      std::set<std::string> directories;
      auto fAddDirectory = [&](std::string const &_File) {
        if (MappedOutputFiles[_ProjectName].count(_File))
          directories.insert(cmStrCat(getMappedOutputFile(_ProjectName, _File, MappedForm::Expression), "->GetPath()"));
      };
      // Also of a byproduct in a directory of a target, as Ninja makes the directories of the outputs of a link
      for (std::string const &byproduct : commandGenerator.GetByproducts()) {
        if (FindTargetPath(_ProjectName, byproduct).Kind != cmTargetPath::EKind::None)
          directories.insert(cmStrCat(getMappedOutputFile(_ProjectName, byproduct, MappedForm::Expression), "->GetPath()"));
        else
          fAddDirectory(byproduct);
      }
      for (unsigned i = 0; i < commandGenerator.GetNumberOfCommands(); ++i) {
        std::string arguments;
        commandGenerator.AppendArguments
          (
            i
            , arguments
            , [&](std::string const &_Param, bool &o_bEscape) -> std::string
            {
              o_bEscape = false;
              fAddDirectory(_Param);
              std::string::size_type const iAssign = _Param.find('=');
              if (iAssign != std::string::npos)
                fAddDirectory(_Param.substr(iAssign + 1));
              return _Param;
            }
            , [](std::string const &_Param) -> std::string
            {
              return _Param;
            }
          )
        ;
      }
      std::string const directory = GetWorkingDirectoryExpression(_ProjectName, workingDirectory);
      directories.insert(directory);
      for (std::string const &createDirectory : directories) {
        script.Bash += cmStrCat("@(ScriptMkDir(", createDirectory, "))\\r\\n");
        script.Bat += cmStrCat("@(ScriptMkDir(", createDirectory, "))");
      }
      script.Bash += cmStrCat("(cd @(", directory, bashSuffix, " && ", bashCommandLines, ") || exit 1\\r\\n");
      // The directory of the command, which the scripts that run after it in the same event don't run in. pushd only takes
      // the separators of Windows.
      script.Bat += cmStrCat("pushd @(", directory, "->WindowsPath()", escapeSuffix(lg->GetState()), " && ", commandLines,
                             " || exit /b 1\\r\\npopd\\r\\n");
      // A byproduct outside the build tree is a path of its own
      for (std::string const &byproduct : commandGenerator.GetByproducts())
        script.Outputs.push_back(makeAbsoluteWrapper(getMappedOutputFile(_ProjectName, byproduct, MappedForm::Expression)));
    }
    return script;
  };

  cmMalterlibBuildEventScript const preBuild = fScript(target->GetPreBuildCommands());
  cmMalterlibBuildEventScript const preLink = fScript(target->GetPreLinkCommands());
  // The POST_BUILD of a target that links runs once the Malterlib build has signed and deployed its file, which its own files
  // then name, so that it can run the target, and on the file that it links, before the deploy, when the target sets
  // Target.CMakePostBuildOnLinkedFile, which can change without generating the import again
  InOwnPostBuild = true;
  MapOwnTargetToDeployed = !isUtilityTarget;
  cmMalterlibBuildEventScript const postBuild = fScript(target->GetPostBuildCommands());
  MapOwnTargetToDeployed = false;
  cmMalterlibBuildEventScript const postBuildOnLinked = isUtilityTarget ? postBuild : fScript(target->GetPostBuildCommands());
  InOwnPostBuild = false;
  if (preBuild.Bash.empty() && preLink.Bash.empty() && postBuild.Bash.empty())
    return;

  // The condition of the target that adds them, if any
  auto fAddCondition = [](cmMalterlibRegistry &entry, char const *pCondition) {
    if (pCondition)
      entry.addChild(pCondition, "true").RawValue = true;
  };
  auto fAddScript = [&](std::string const &key, cmMalterlibBuildEventScript const &script, char const *pCondition = nullptr) {
    for (bool bBat : { false, true }) {
      auto &entry = outputTarget.addChild(key, cmStrCat("=+ `", bBat ? script.Bat : script.Bash, '`'));
      entry.RawValue = true;
      auto &condition = entry.addChild("&");
      auto &batCondition = condition.addChild("ScriptWindowsBat", bBat ? "true" : "false");
      batCondition.RawValue = true;
      fAddCondition(condition, pCondition);
    }
  };
  auto fAddOutputs = [&](std::string const &key, std::vector<std::string> const &outputs, char const *pCondition = nullptr) {
    if (outputs.empty())
      return;
    std::string value = "=+ [";
    for (std::size_t i = 0; i < outputs.size(); ++i)
      value += cmStrCat(i ? ", " : "", outputs[i]);
    value += ']';
    auto &entry = outputTarget.addChild(key, value);
    entry.RawValue = true;
    if (pCondition)
      fAddCondition(entry.addChild("&"), pCondition);
  };

  // A target that doesn't link, as of add_custom_target, runs PRE_BUILD after its commands, before POST_BUILD, and doesn't run
  // PRE_LINK
  if (isUtilityTarget) {
    for (cmMalterlibBuildEventScript const *pScript : { &preBuild, &postBuild }) {
      if (pScript->Bash.empty())
        continue;
      fAddScript("Property.MalterlibPostBuildScript_First", *pScript);
      fAddOutputs("Target.PostBuildScriptOutputs", pScript->Outputs);
    }
    return;
  }

  for (cmMalterlibBuildEventScript const *pScript : { &preBuild, &preLink }) {
    if (pScript->Bash.empty())
      continue;
    fAddScript("Target.PreLinkScript", *pScript);
    fAddOutputs("Target.PreLinkScriptOutputs", pScript->Outputs);
  }
  if (!preBuild.Bash.empty() || !preLink.Bash.empty())
    outputTarget.addChild("Target.HasPreLinkScript", "true").RawValue = true;
  if (!postBuild.Bash.empty()) {
    fAddScript("Property.MalterlibPostBuildScript_Signed", postBuild, "!Target.CMakePostBuildOnLinkedFile");
    fAddOutputs("Target.PostBuildScriptOutputs", postBuild.Outputs, "!Target.CMakePostBuildOnLinkedFile");
    fAddScript("Property.MalterlibPostBuildScript_First", postBuildOnLinked, "Target.CMakePostBuildOnLinkedFile");
    fAddOutputs("Target.PostBuildScriptOutputs", postBuildOnLinked.Outputs, "Target.CMakePostBuildOnLinkedFile");
  }
}

// The files that CMake copies into the bundle of an application bundle, its RESOURCE and the other sources with a
// MACOSX_PACKAGE_LOCATION that it doesn't compile, are copied into the bundle that the Malterlib build deploys, before it signs it,
// as the Malterlib build copies its own resources
void cmExtraMalterlibGenerator::AddBundleContents(std::string const &_ProjectName, cmMalterlibRegistry &outputTarget,
                                                  cmGeneratorTarget const *target, std::string const &configName)
{
  std::vector<cmSourceFile const *> headerSources;
  target->GetHeaderSources(headerSources, configName);
  std::vector<cmSourceFile const *> extraSources;
  target->GetExtraSources(extraSources, configName);
  // Where the content directory is in the bundle, which is Contents on macOS
  std::string const bundleDirectory =
    target->BuildBundleDirectory(cmStrCat(target->GetDirectory(configName), '/'), configName, cmGeneratorTarget::BundleDirLevel);
  std::string const contentDirectory = target->GetMacContentDirectory(configName, cmStateEnums::RuntimeBinaryArtifact);
  if (!cmHasPrefix(contentDirectory, bundleDirectory))
    return;
  std::string script;
  std::vector<std::string> inputs;
  std::vector<std::string> outputs;
  for (std::vector<cmSourceFile const *> const *pSources : { &headerSources, &extraSources }) {
    for (cmSourceFile const *source : *pSources) {
      cmGeneratorTarget::SourceFileFlags const flags = target->GetTargetSourceFileFlags(source);
      if (flags.Type == cmGeneratorTarget::SourceFileTypeNormal || !flags.MacFolder)
        continue;
      std::string const input = makeAbsoluteWrapper(getMappedOutputFile(_ProjectName, source->GetFullPath(), MappedForm::Expression));
      std::string location = cmStrCat(contentDirectory.substr(bundleDirectory.size()), '/', flags.MacFolder);
      std::string directory;
      cmSystemTools::ReplaceString(location, "@", "@@");
      cmMalterlibRegistry::addEscapeStr(directory, location, "`\\", "`\\", false);
      directory = cmStrCat("`@(MalterlibFinalModuleDestinationPath)/@(Target.FileName)@(Target.FileExtension)", directory, '`');
      std::string name;
      std::string fileName = cmSystemTools::GetFilenameName(source->GetFullPath());
      cmSystemTools::ReplaceString(fileName, "@", "@@");
      cmMalterlibRegistry::addEscapeStr(name, fileName, "`\\", "`\\", false);
      script += cmStrCat("@(ScriptMkDir(", directory, "))\\r\\nMTool DiffCopy @((", input, ")->Escape()) @((", directory,
                         ")->Escape()) \"\" 0\\r\\n");
      inputs.push_back(input);
      outputs.push_back(cmStrCat('`', directory.substr(1, directory.size() - 2), '/', name, '`'));
    }
  }
  if (script.empty())
    return;
  outputTarget.addChild("Property.MalterlibPostBuildScriptPreSign", cmStrCat("=+ `", script, '`')).RawValue = true;
  outputTarget.addChild("Property.MalterlibPostBuildScriptInputs", cmStrCat("=+ [", cmJoin(inputs, ", "), ']')).RawValue = true;
  outputTarget.addChild("Target.PostBuildScriptOutputs", cmStrCat("=+ [", cmJoin(outputs, ", "), ']')).RawValue = true;
}

// Whether a target that the target depends on, also through other targets, has the command too, which the build then runs
// there and orders the target after, so that one rule generates its outputs, as in CMake's Ninja generator
bool cmExtraMalterlibGenerator::CustomCommandIsInDependency(cmLocalGenerator *lg, const cmGeneratorTarget* target, cmCustomCommand* customCommand) {
  cmLocalNinjaGenerator *ninjaGenerator = static_cast<cmLocalNinjaGenerator *>(lg);
  auto *customCommandTargets = ninjaGenerator->GetTargetsForCustomCommand(customCommand);
  if (!customCommandTargets)
    return false;

  std::set<cmGeneratorTarget const *> visited;
  std::vector<cmGeneratorTarget const *> pending{ target };
  while (!pending.empty()) {
    cmGeneratorTarget const *current = pending.back();
    pending.pop_back();
    for (auto &dependency : const_cast<cmGlobalGenerator*>(GlobalGenerator)->GetTargetDirectDepends(current)) {
      cmGeneratorTarget const *dependencyTarget = dependency;
      if (!visited.insert(dependencyTarget).second)
        continue;
      if (customCommandTargets->count(const_cast<cmGeneratorTarget *>(dependencyTarget)))
        return true;
      pending.push_back(dependencyTarget);
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
    std::shared_ptr<std::set<std::string> const> Defines; // CompileDefinitions with those of Arguments
    std::string CStd;
  };
  std::map<std::string, cmMalterlibTargetOptions> targetOptionsCache;
  auto fTargetOptions = [&](std::string const& language) -> cmMalterlibTargetOptions const& {
    auto iOptions = targetOptionsCache.find(language);
    if (iOptions != targetOptionsCache.end())
      return iOptions->second;
    cmMalterlibTargetOptions& options = targetOptionsCache[language];
    std::string const flags = EvaluateMarkedString([&]() -> std::string {
      std::string targetFlags;
      lg->GetTargetCompileFlags(const_cast<cmGeneratorTarget*>(target), configName, language, targetFlags, std::string());
      return targetFlags;
    });
    options.Arguments = argumentsFromFlags(lg, flags);
    options.CompileDefinitions = EvaluateMarkedSet([&]() -> std::set<std::string> {
      std::set<std::string> targetDefines;
      lg->GetTargetDefines(target, configName, language, targetDefines);
      return targetDefines;
    });
    auto defines = std::make_shared<std::set<std::string>>(options.CompileDefinitions);
    applyDefines(*defines, definesFromOptions(lg, language, options.Arguments, &options.CStd));
    options.Defines = std::move(defines);
    return options;
  };

  for (auto &file : sourceFiles) {
    if (!file->GetObjectLibrary().empty())
      continue;
    // A target with a unity build compiles its sources one by one, as without it: CMake generates the unity sources in its
    // build tree, which import caches leave out, and they include the sources by their absolute paths. The UNITY_SOURCE_FILE
    // of a unity source is its own path, and that of a source that it includes is the path of the unity source.
    if (cmValue unitySourceFile = file->GetProperty("UNITY_SOURCE_FILE")) {
      if (*unitySourceFile == file->GetFullPath())
        continue;
    }

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
      cmCustomCommandGenerator customCommandGenerator = MakeCommandGenerator(*customCommand, configName, lg);

      // The depfile that CMake transforms for Ninja names the dependencies relative to the build tree of CMake, which the
      // Malterlib build doesn't run in, so the command would only be run again when its declared inputs change
      if (!customCommandGenerator.GetDepfile().empty()) {
        lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          cmStrCat("The custom command of ", fullPath, " has a DEPFILE, which the Malterlib generator can't give the build"));
      }

      std::string firstInput;
      std::string inputs = "[";
      {
        std::vector<std::string> realDependencies;
        std::set<std::string> visitedDependencies;
        for (auto &dependency : customCommandGenerator.GetDepends()) {
          std::string realDependency;
          if (lg->GetRealDependency(dependency, configName,
                                    realDependency,
                                    customCommandGenerator.GetCC().GetCMP0212Status()))
            ResolvePhonyDependency(_ProjectName, realDependency, realDependencies, visitedDependencies);
        }
        size_t index = 0;
        for (std::string realDependency : realDependencies) {
          // The placeholder of the rule names the file that a target links by its path, not by the property of the target
          bool const bTargetFile = !GetTargetFileExpression(_ProjectName, realDependency).empty();
          std::string const dependencyPath = realDependency;
          realDependency = getMappedOutputFile(_ProjectName, realDependency, MappedForm::Expression);
          if (index == 0)
            firstInput = bTargetFile ? dependencyPath : realDependency;
          else
            inputs += ", ";

          inputs += makeAbsoluteWrapper(realDependency);
          ++index;
        }
        inputs += "]";
      }

      std::string commandWorkingDirectory = customCommandGenerator.GetWorkingDirectory();
      if (commandWorkingDirectory.empty())
        commandWorkingDirectory = lg->GetCurrentBinaryDirectory();
      cmMalterlibRegistry *pOutCompile = nullptr;
      {
        std::string const commandLines =
          GetCustomCommandLines(_ProjectName, lg, customCommandGenerator, configName, commandWorkingDirectory);

        // A command without command lines builds nothing, but what depends on it waits for its inputs, as in CMake's Ninja, so
        // its inputs give the target the dependencies on the targets that generate them, without a command to compile
        if (commandLines.empty()) {
          outFile.addChild("Compile", "").addChild("Custom_Inputs", inputs).RawValue = true;
          continue;
        }

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

      OutCompile
        .addChild("Custom_WorkingDirectory",
                  GetWorkingDirectoryExpression(_ProjectName, commandWorkingDirectory))
        .RawValue = true;

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
              symbolic = true;
          }

          if (symbolic)
            continue;

          UsedOutputs.insert(output);

          std::string newOutput = getMappedOutputFile(_ProjectName, output, MappedForm::Expression);

          if (index > 0)
            outputs += ", ";
          ++index;

          outputs += makeAbsoluteWrapper(newOutput);
        }
        for (auto &output : customCommandGenerator.GetByproducts()) {
          if (StringStartsWithPath(output, "/DIR:"))
            continue;

          UsedOutputs.insert(output);

          std::string newOutput = getMappedOutputFile(_ProjectName, output, MappedForm::Expression);

          if (index > 0)
            outputs += ", ";
          ++index;

          outputs += makeAbsoluteWrapper(newOutput);
        }
        outputs += "]";
        OutCompile.addChild("Custom_Outputs", outputs).RawValue = true;
      }

      OutCompile.addChild("Custom_Inputs", inputs).RawValue = true;

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
      std::set<std::string> const sortedDefines = EvaluateMarkedSet([&]() -> std::set<std::string> {
        std::set<std::string> defines;
        if (fileSet) {
          auto fileSetDefines = fileSet->BelongsTo(target)
            ? fileSet->GetCompileDefinitions(config, Language)
            : fileSet->GetInterfaceCompileDefinitions(config, Language);
          if (!fileSetDefines.empty())
            lg->AppendDefines(defines, fileSetDefines);
        }

        const std::string COMPILE_DEFINITIONS("COMPILE_DEFINITIONS");
        if (auto compile_defs = file->GetProperty(COMPILE_DEFINITIONS))
          lg->AppendDefines(defines, genexInterpreter.Evaluate(*compile_defs, COMPILE_DEFINITIONS));

        std::string defPropName = "COMPILE_DEFINITIONS_";
        defPropName += cmSystemTools::UpperCase(config);
        if (auto config_compile_defs = file->GetProperty(defPropName))
          lg->AppendDefines(defines, genexInterpreter.Evaluate(*config_compile_defs, COMPILE_DEFINITIONS));
        return defines;
      });

      // The options of the source and of its file set follow those of the target on the command line, which the compiler
      // reads as a whole: an option can take its operand from the next property, and its stages apply across them.
      std::vector<std::string> const fileArguments = EvaluateMarkedList([&]() -> std::vector<std::string> {
        std::vector<std::string> arguments;
        const std::string COMPILE_FLAGS("COMPILE_FLAGS");
        if (auto cflags = file->GetProperty(COMPILE_FLAGS)) {
          for (std::string& argument : argumentsFromFlags(lg, genexInterpreter.Evaluate(*cflags, COMPILE_FLAGS)))
            arguments.push_back(std::move(argument));
        }

        const std::string COMPILE_OPTIONS("COMPILE_OPTIONS");
        if (auto sourceOptions = file->GetProperty(COMPILE_OPTIONS)) {
          for (std::string& argument : cmList{ genexInterpreter.Evaluate(*sourceOptions, COMPILE_OPTIONS) })
            arguments.push_back(joinNinjaContinuations(std::move(argument)));
        }

        if (fileSet) {
          auto fileSetOptions = fileSet->BelongsTo(target)
            ? fileSet->GetCompileOptions(config, Language)
            : fileSet->GetInterfaceCompileOptions(config, Language);
          for (std::string& argument : cm::remove_BT(fileSetOptions))
            arguments.push_back(joinNinjaContinuations(std::move(argument)));
        }
        return arguments;
      });

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
          SourceDefines.push_back({ &outFile, malterlibType, std::make_shared<std::set<std::string> const>(std::move(fileDefines)) });

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
        // CMake decides whether a directory is a system directory by its spelling, as the property has it, without the markers
        // of the directories of targets
        auto fAddInclude = [&](std::string const& _Include, std::string const& _Base) {
          std::string spelling = RemoveTargetDirectoryMarkers(_Include);
          cmSystemTools::ConvertToUnixSlashes(spelling);
          std::string include = _Include;
          cmSystemTools::ConvertToUnixSlashes(include);
          if (frameworkSearchDirectory(lg, target, config, Language, spelling, include, frameworkDirectories,
                                       systemFrameworkDirectories)) {
            return;
          }
          bool const bSystem = target->IsSystemIncludeDirectory(spelling, config, Language);
          auto& toIncludes = bSystem ? systemIncludes : includes;
          toIncludes.push_back(collapseSearchDirectory(_Include, _Base));
        };
        if (fileSet) {
          std::vector<std::string> const fileSetIncludes = EvaluateMarkedList([&]() -> std::vector<std::string> {
            return cm::remove_BT(fileSet->BelongsTo(target)
              ? fileSet->GetIncludeDirectories(config, Language)
              : fileSet->GetInterfaceIncludeDirectories(config, Language));
          });
          for (std::string const& include : fileSetIncludes)
            fAddInclude(include, std::string());
        }
        const std::string INCLUDE_DIRECTORIES("INCLUDE_DIRECTORIES");
        if (auto sourceIncludes = file->GetProperty(INCLUDE_DIRECTORIES)) {
          std::vector<std::string> const sourceIncludeList = EvaluateMarkedList([&]() -> std::vector<std::string> {
            return cmList{ genexInterpreter.Evaluate(*sourceIncludes, INCLUDE_DIRECTORIES) }.data();
          });
          for (std::string const& include : sourceIncludeList)
            fAddInclude(include, lg->GetCurrentSourceDirectory());
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
            include = GetSearchDirectoryValue(_ProjectName, include);
          outFile.addChild("Compile.SearchPath", "+= [" + cmJoin(includes, ", ") + "]").RawValue = true;
        }
        if (!systemIncludes.empty()) {
          auto end = cmRemoveDuplicates(systemIncludes);
          systemIncludes.erase(end, systemIncludes.end());
          for (auto &include : systemIncludes)
            include = GetSearchDirectoryValue(_ProjectName, include);
          outFile.addChild("Compile.SystemSearchPath", "+= [" + cmJoin(systemIncludes, ", ") + "]").RawValue = true;
        }
        auto fAddFrameworkDirectories = [&](std::vector<std::string>& _Directories, char const* _pKey) {
          if (_Directories.empty())
            return;
          for (auto &directory : _Directories)
            directory = GetSearchDirectoryValue(_ProjectName, collapseSearchDirectory(directory));
          auto end = cmRemoveDuplicates(_Directories);
          _Directories.erase(end, _Directories.end());
          outFile.addChild(_pKey, "+= [" + cmJoin(_Directories, ", ") + "]").RawValue = true;
        };
        fAddFrameworkDirectories(frameworkDirectories, "Compile.FrameworkSearchPath");
        fAddFrameworkDirectories(systemFrameworkDirectories, "Compile.SystemFrameworkSearchPath");
      }
    }
  }
}

// A directory that the compiler searches, as an expression, mapped as the paths of the texts that the import writes are, as a
// directory of a target, as $<TARGET_FILE_DIR>, or in a bundle that it builds. A compile only needs the directory, not the
// target, so it doesn't order the build after the target, as in CMake.
std::string cmExtraMalterlibGenerator::GetSearchDirectoryValue(std::string const &_ProjectName, std::string const &_Directory)
{
  // A directory in the build tree that a command writes keeps its path, which import caches map both to where the build writes
  // it and to the copy of the build tree that they keep, with what the configuration wrote in it
  std::string lexical;
  if (!FindTargetDirectoryMarker(_Directory, 0) && MappedOutputFiles[_ProjectName].count(lexicalOrPath(_Directory, lexical)))
    return makeAbsoluteWrapper(_Directory);
  MapPathsOnly = true;
  std::string const mapped = replaceMappedOutputFiles(_ProjectName, _Directory, MappedForm::EvalString);
  MapPathsOnly = false;
  if (mapped.find('@') == std::string::npos)
    return makeAbsoluteWrapper(RemoveTargetDirectoryMarkers(_Directory));
  std::string value = "`";
  addEvalStringText(value, mapped);
  value += '`';
  return value;
}

// The working directory of a command, which is a directory of a target where the command names it as one, as $<TARGET_FILE_DIR>
// for a command that runs $<TARGET_FILE> there, since the Malterlib target has its files elsewhere. A directory that only is the
// same, as the build tree of CMake that targets put their files in by default, stays as it is.
std::string cmExtraMalterlibGenerator::GetWorkingDirectoryExpression(std::string const &_ProjectName,
                                                                     std::string const &_WorkingDirectory)
{
  std::string const expression = GetTargetPathExpression(_ProjectName, _WorkingDirectory);
  if (!expression.empty())
    return cmStrCat(expression, "->MakeAbsolute()");
  return makeAbsoluteWrapper(RemoveTargetDirectoryMarkers(_WorkingDirectory));
}

// The files that a dependency is, which are those that an output of a command that runs nothing depends on
void cmExtraMalterlibGenerator::ResolvePhonyDependency(std::string const &_ProjectName, std::string const &_Dependency,
                                                       std::vector<std::string> &o_Dependencies, std::set<std::string> &o_Visited)
{
  std::string lexical;
  std::string const &dependency = lexicalOrPath(_Dependency, lexical);
  if (!o_Visited.insert(dependency).second)
    return;
  auto &phonyOutputs = PhonyOutputDependencies[_ProjectName];
  auto iPhony = phonyOutputs.find(dependency);
  if (iPhony == phonyOutputs.end()) {
    o_Dependencies.push_back(dependency);
    return;
  }
  for (std::string const &phonyDependency : iPhony->second)
    ResolvePhonyDependency(_ProjectName, phonyDependency, o_Dependencies, o_Visited);
}

// The files that the build events of a target write in the build tree are mapped as the outputs of custom commands are: its
// byproducts, and as build events need not declare what they write, the files in the build tree that they name that don't
// exist after CMake's configure step and that no target links. Those that exist are outputs of the configure step, which the
// import cache holds.
void cmExtraMalterlibGenerator::CollectBuildEventFiles(std::string const &_ProjectName, cmLocalGenerator *lg,
                                                       cmGeneratorTarget const *target)
{
  std::string const configName = GetConfigName(lg);
  // The files that the target links, which the build events can write files beside
  std::string const targetName = GetTargetName(target, lg->GetProjectName());
  std::vector<std::string> targetFiles;
  for (auto const &targetFile : TargetFiles[_ProjectName]) {
    if (targetFile.second.TargetName == targetName)
      targetFiles.push_back(targetFile.first);
  }
  bool bPostBuild = false;
  // A file beside a file of the target is where the event that declares it as a byproduct writes it, or else where the first
  // event that names it does, as later events can read it
  std::set<std::string> declaredSidecars;
  auto fAddSidecar = [&](std::string const &_Path, cmMalterlibSidecarFile &&_Sidecar, bool _bDeclared) {
    auto &sidecars = TargetSidecarFiles[_ProjectName];
    if (sidecars.count(_Path) && (!_bDeclared || declaredSidecars.count(_Path)))
      return;
    sidecars[_Path] = std::move(_Sidecar);
    if (_bDeclared)
      declaredSidecars.insert(_Path);
  };
  // A file of the build tree is mapped where the Malterlib build writes it. A file beside a file of the target is beside the file
  // of the Malterlib target, wherever the target has its files, as RUNTIME_OUTPUT_DIRECTORY can put them outside the build tree.
  auto fAddFile = [&](std::string const &_Path, bool _bDeclared) {
    if (StringStartsWithPath(_Path, tempDir.c_str()))
      MappedOutputFiles[_ProjectName].insert(_Path);
    // A file that a custom command writes, which a build event can read, stays where that command writes it
    if (CommandOutputFiles[_ProjectName].count(_Path))
      return;
    // Beside the longest of the files of the target that its name starts with, as the real name of a versioned shared library
    // rather than its name link
    std::string const *pTargetFile = nullptr;
    for (std::string const &targetFile : targetFiles) {
      if (_Path.size() <= targetFile.size() + 1 || !cmHasPrefix(_Path, targetFile) || _Path[targetFile.size()] != '.')
        continue;
      cm::string_view const extension = cm::string_view(_Path).substr(targetFile.size());
      if (std::all_of(extension.begin(), extension.end(), namesFile) && (!pTargetFile || targetFile.size() > pTargetFile->size()))
        pTargetFile = &targetFile;
    }
    if (pTargetFile)
      fAddSidecar(_Path, cmMalterlibSidecarFile{ *pTargetFile, bPostBuild, false }, _bDeclared);
  };
  // A path in a directory of a target, which GetTargetPathExpression maps, isn't mapped as a file of the build tree. Other
  // directories of targets are those in the build tree of CMake.
  auto fBuildTreePath = [&](std::string const &_Path) -> std::string {
    if (FindTargetPath(_ProjectName, _Path).Kind != cmTargetPath::EKind::None)
      return std::string();
    return RemoveTargetDirectoryMarkers(_Path);
  };
  // Each of the paths of a list, as of -DFILES=<file>;<file> for a script of CMake
  auto fAddWrittenFile = [&](std::string const &_Param) {
    for (std::string const &element : cmList(_Param)) {
      std::string lexical;
      std::string const path = lexicalOrPath(fBuildTreePath(element), lexical);
      if (cmSystemTools::FileIsFullPath(path) && !TargetFiles[_ProjectName].count(path) && !cmSystemTools::FileExists(path))
        fAddFile(path, false);
    }
  };
  for (std::vector<cmCustomCommand> const *pCommands :
       { &target->GetPreBuildCommands(), &target->GetPreLinkCommands(), &target->GetPostBuildCommands() }) {
    bPostBuild = pCommands == &target->GetPostBuildCommands();
    for (cmCustomCommand const &command : *pCommands) {
      cmCustomCommandGenerator commandGenerator = MakeCommandGenerator(command, configName, lg);
      // The directories of the files of the target that the command names, as $<TARGET_FILE_DIR>, by their paths in the build
      // tree of CMake, with the files
      std::map<std::string, std::string> ownDirectories;
      auto fAddOwnDirectories = [&](std::string const &_Text) {
        for (auto marker = FindTargetDirectoryMarker(_Text, 0); marker;
             marker = FindTargetDirectoryMarker(_Text, marker->iEnd)) {
          if (marker->iStart == std::string::npos)
            continue;
          auto iFile = TargetFiles[_ProjectName].find(*marker->pPath);
          if (iFile != TargetFiles[_ProjectName].end() && iFile->second.TargetName == targetName)
            ownDirectories[cmSystemTools::GetFilenamePath(*marker->pPath)] = *marker->pPath;
        }
      };
      fAddOwnDirectories(commandGenerator.GetWorkingDirectory());
      for (unsigned i = 0; i < commandGenerator.GetNumberOfCommands(); ++i) {
        std::string arguments;
        commandGenerator.AppendArguments
          (
            i
            , arguments
            , [&](std::string const &_Param, bool &o_bEscape) -> std::string
            {
              o_bEscape = false;
              fAddOwnDirectories(_Param);
              fAddWrittenFile(_Param);
              std::string::size_type const iAssign = _Param.find('=');
              if (iAssign != std::string::npos)
                fAddWrittenFile(_Param.substr(iAssign + 1));
              return _Param;
            }
            , [](std::string const &_Param) -> std::string
            {
              return _Param;
            }
          )
        ;
      }
      // A byproduct in the directory of the file of the target or in a directory in it, which a command that names that directory
      // as $<TARGET_FILE_DIR> writes, as a copy into it, is beside the file of the Malterlib target too, as byproducts can't name
      // the directory so
      for (std::string const &byproduct : commandGenerator.GetByproducts()) {
        std::string lexical;
        std::string const path = lexicalOrPath(fBuildTreePath(byproduct), lexical);
        if (path.empty())
          continue;
        fAddFile(path, true);
        if (CommandOutputFiles[_ProjectName].count(path) || declaredSidecars.count(path))
          continue;
        std::string const *pOwnFile = nullptr;
        std::size_t nOwnDirectory = 0;
        for (auto const &ownDirectory : ownDirectories) {
          if (ownDirectory.first.size() > nOwnDirectory && StringStartsWithPath(path, ownDirectory.first.c_str()) &&
              path.size() > ownDirectory.first.size() && path[ownDirectory.first.size()] == '/') {
            pOwnFile = &ownDirectory.second;
            nOwnDirectory = ownDirectory.first.size();
          }
        }
        if (pOwnFile)
          fAddSidecar(path, cmMalterlibSidecarFile{ *pOwnFile, bPostBuild, true }, true);
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

  // The files that the target links, which its Malterlib target links in the paths of the Malterlib build, as the executable of
  // an application bundle, which is the one in the bundle that Malterlib builds. Other bundles have other layouts there.
  cm::TargetType const type = target->GetType();
  if ((type == cm::TargetType::EXECUTABLE || type == cm::TargetType::STATIC_LIBRARY || type == cm::TargetType::SHARED_LIBRARY ||
       type == cm::TargetType::MODULE_LIBRARY) &&
      (!target->IsBundleOnApple() || target->IsAppBundleOnApple())) {
    std::string const targetName = GetTargetName(target, lg->GetProjectName());
    auto fAddTargetFile = [&](std::string const &_File, char const *_pProperty, char const *_pFinalProperty) {
      std::string lexical;
      if (!_File.empty())
        TargetFiles[_ProjectName][lexicalOrPath(_File, lexical)] = cmMalterlibTargetFile{ targetName, _pProperty, _pFinalProperty };
    };
    bool const bAppBundle = target->IsAppBundleOnApple();
    // The bundle of an application bundle, as of $<TARGET_BUNDLE_DIR>, is the bundle that Malterlib builds
    if (bAppBundle) {
      std::string const bundleDirectory = target->BuildBundleDirectory(cmStrCat(target->GetDirectory(configName), '/'),
                                                                       configName, cmGeneratorTarget::BundleDirLevel);
      fAddTargetFile(bundleDirectory, "MalterlibOriginalModuleDestination", "MalterlibFinalModuleDestination");
      std::string lexical;
      TargetBundleDirectories[_ProjectName].insert(lexicalOrPath(bundleDirectory, lexical));
    }
    char const *pProperty = bAppBundle ? "MalterlibOriginalModuleDestinationExecutable" : "MalterlibOriginalModuleDestination";
    char const *pFinalProperty = bAppBundle ? "MalterlibFinalModuleDestinationExecutable" : "MalterlibFinalModuleDestination";
    std::string const runtimeFile = target->GetFullPath(configName, cmStateEnums::RuntimeBinaryArtifact);
    fAddTargetFile(runtimeFile, pProperty, pFinalProperty);
    // The real name of a versioned shared library, which the name above links to
    fAddTargetFile(target->GetFullPath(configName, cmStateEnums::RuntimeBinaryArtifact, true), pProperty, pFinalProperty);
    // And its soname, as of $<TARGET_SONAME_FILE>, which differs from both where its VERSION and SOVERSION differ
    if (type == cm::TargetType::SHARED_LIBRARY && !target->IsDLLPlatform()) {
      std::string const soName = target->GetSOName(configName);
      if (!soName.empty())
        fAddTargetFile(cmStrCat(target->GetDirectory(configName), '/', soName), pProperty, pFinalProperty);
    }
    // The program database that the linker writes on Windows, which is the symbol file of the Malterlib target
    if (lg->GetMakefile()->IsOn("WIN32") && type != cm::TargetType::STATIC_LIBRARY) {
      fAddTargetFile(cmStrCat(target->GetPDBDirectory(configName), '/', target->GetPDBName(configName)), "Target.SymbolFile",
                     "Target.SymbolFile");
    }
    // The import library of a module on Windows. Elsewhere, as for the text stub of an Apple library that exports, what
    // links to the target is the module itself, which is what the Malterlib build links.
    if (target->HasImportLibrary(configName)) {
      std::string const importFile = target->GetFullPath(configName, cmStateEnums::ImportLibraryArtifact);
      if (target->IsDLLPlatform())
        fAddTargetFile(importFile, "MalterlibOriginalModuleDestinationLib", "MalterlibOriginalModuleDestinationLib");
      else
        fAddTargetFile(importFile, pProperty, pFinalProperty);
    }
  }

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

  CurrentTargetName = GetTargetName(target, lg->GetProjectName());
  TargetFileDependencies.clear();
  TargetPathDependencies.clear();
  auto &outputTarget = registry.addChild("%Target", CurrentTargetName);
  outputTarget.addChild("Property.MalterlibTargetNameType", "Normal");
  outputTarget.addChild("Compile.AllowNonExisting", "true").RawValue = true;
  if (isUtilityTarget)
    outputTarget.addChild("Compile.Disabled", "true").RawValue = true;
  auto &group = outputTarget.addChild("Target.Group", "External/" + lg->GetProjectName());
  group.addChild("!!Target.Group", "undefined").RawValue = true;
  outputTarget.addChild("Target.Type", GetTargetType(target));
  // The Info.plist that CMake generates in the bundle of an application bundle, from its MACOSX_BUNDLE properties
  if (target->IsAppBundleOnApple()) {
    std::string const contentDirectory = target->BuildBundleDirectory(cmStrCat(target->GetDirectory(configName), '/'),
                                                                      configName, cmGeneratorTarget::ContentLevel);
    std::string const infoPList = cmStrCat(contentDirectory, "/Info.plist");
    outputTarget.addChild("Target.MalterlibInfoPListFile", makeAbsoluteWrapper(infoPList)).RawValue = true;
    AddBundleContents(_ProjectName, outputTarget, target, configName);
  }
  outputTarget.addChild("Target.BaseName", lg->GetProjectName() + "_" + target->GetName());
  // The file of the target is named as CMake names it: with its OUTPUT_NAME, prefix and suffix, and the version that a shared
  // library has in its name. An application bundle is named as its executable, as Malterlib makes the bundle itself.
  std::string baseFileName = target->GetName();
  cm::TargetType const nameType = target->GetType();
  if (nameType == cm::TargetType::EXECUTABLE || nameType == cm::TargetType::STATIC_LIBRARY ||
      nameType == cm::TargetType::SHARED_LIBRARY || nameType == cm::TargetType::MODULE_LIBRARY) {
    cmGeneratorTarget::NameComponents const &components = target->GetFullNameComponents(configName);
    std::string const stem = components.prefix + components.base;
    std::string const fileName =
      cmSystemTools::GetFilenameName(target->GetFullPath(configName, cmStateEnums::RuntimeBinaryArtifact, true));
    if (target->IsAppBundleOnApple()) {
      // As CMake names the executable in the bundle, with its prefix and suffix, and the bundle with it and its BUNDLE_EXTENSION
      baseFileName = target->GetFullName(configName, cmStateEnums::RuntimeBinaryArtifact);
      if (cmValue const extension = target->GetProperty("BUNDLE_EXTENSION")) {
        outputTarget.addChild("Target.FileExtension", cmMalterlibRegistry::getEscaped(cmStrCat('.', *extension), true, true))
          .RawValue = true;
      }
    } else if (cmHasPrefix(fileName, stem)) {
      baseFileName = stem;
      outputTarget.addChild("Target.FileExtension", cmMalterlibRegistry::getEscaped(fileName.substr(stem.size()), true, true))
        .RawValue = true;
    }
  }
  outputTarget.addChild("Target.BaseFileName", cmMalterlibRegistry::getEscaped(baseFileName, true, true)).RawValue = true;
  if (IsObjectLib(target))
    outputTarget.addChild("Target.ObjectLibrary", "true").RawValue = true;

  std::map<std::string, cmMalterlibCompileTypeInfo> compileTypeInfo;
  AddTargetCompileInfo(*this, compileTypeInfo, target, lg, configName);

  // CMake synthesizes targets that build the C++ modules that targets import, as std for import std, which CMake 4.5 adds
  // to the C++20 targets that scan for modules. Their module interfaces aren't built by the Malterlib build system.
  if (target->IsSynthetic()) {
    lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
      cmStrCat("CMake synthesizes target ", target->GetName(), " to build the C++ modules that targets import, as std, which "
               "the Malterlib generator can't build. If no source imports modules, set CMAKE_CXX_SCAN_FOR_MODULES to OFF for "
               "the import."));
  }

  if (!isUtilityTarget) {
    AddTargetCompileInfo(*this, compileTypeInfo, target, lg, configName);
    std::vector<cmSourceFile*> sourceFiles;
    GetTargetFiles(sourceFiles, lg, target, makefile);
    SourceDefines.clear();
    AddFilesToRegistry(_ProjectName, outputTarget, sourceFiles, configName, lg, target, false);

    // Wrappers are commands that CMake runs the compiler or the linker with, for the languages that it applies them to,
    // which the Malterlib build system has no counterpart of. Only those of what the target compiles and links, and with a
    // command for the configuration, as cmCommonTargetGenerator::ParseWrappers evaluates them.
    auto fHasWrapperCommand = [&](std::vector<BT<std::string>> const& wrappers, cm::string_view prefix,
                                  std::string const& property, std::string const& language) -> bool {
      cm::GenEx::Context context(lg, configName, language);
      for (BT<std::string> const& wrapper : wrappers) {
        cmValue definition = lg->GetMakefile()->GetState()->GetGlobalProperty(cmStrCat(prefix, wrapper.Value));
        // CMake reports one that isn't defined
        if (!definition.IsSet())
          return true;
        cmGeneratorExpressionDAGChecker dagChecker{ target, property, nullptr, nullptr, context };
        if (!cmGeneratorExpression::Evaluate(*definition, lg, configName, target, &dagChecker, target, language).empty())
          return true;
      }
      return false;
    };
    // The languages that the sources compile as, also those that a file set sets
    std::set<std::string> compiledLanguages;
    {
      std::vector<cmSourceFile const*> objectSources;
      target->GetObjectSources(objectSources, configName);
      for (cmSourceFile const* source : objectSources)
        compiledLanguages.insert(target->GetSourceFileLanguage(source, configName));
    }
    for (std::string const& language : compiledLanguages) {
      static std::set<std::string> const wrappedLanguages{ "C", "CXX", "Fortran", "CUDA", "HIP", "ISPC", "OBJC", "OBJCXX", "SYCL" };
      if (wrappedLanguages.count(language) &&
          fHasWrapperCommand(target->GetCompileWrappers(configName, language), "CMAKE_COMPILE_WRAPPER_", "COMPILE_WRAPPERS",
                             language)) {
        lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          cmStrCat("Target ", target->GetName(), " has compile wrappers (COMPILE_WRAPPERS) for ", language,
                   ", which the Malterlib generator can't apply"));
      }
    }
    // C++ modules need the module scan and the dependencies on the built module interfaces that CMake's Ninja generator
    // adds, which the Malterlib build system has no counterpart of. That includes the units that AUTOMOC generates for them.
    // A file set of modules without sources in the configuration, as one whose sources depend on it, builds as in CMake.
    bool bModuleSources = false;
    if (target->HaveCxx20ModuleSources()) {
      std::vector<cmSourceFile *> configSources;
      target->GetSourceFiles(configSources, configName);
      for (cmSourceFile const *pSource : configSources) {
        cmGeneratorFileSet const *pFileSet = target->GetFileSetForSource(configName, pSource);
        if (pFileSet && pFileSet->GetType() == cm::FileSetMetadata::CXX_MODULES)
          bModuleSources = true;
      }
    }
    if (bModuleSources) {
      lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
        cmStrCat("Target ", target->GetName(), " has C++ module sources (CXX_MODULES file sets), which the Malterlib generator "
                 "can't build"));
    }
    cm::TargetType const type = target->GetType();
    std::string const linkerLanguage = target->GetLinkerLanguage(configName);
    bool bWrapsLink =
      type == cm::TargetType::EXECUTABLE || type == cm::TargetType::SHARED_LIBRARY || type == cm::TargetType::MODULE_LIBRARY;
    // The rules of a static library only get the wrappers where they run the compiler of the language or the linker, as
    // when the toolchain creates the library with the compiler driver, as cmNinjaNormalTargetGenerator::ComputeLinkCmd
    // chooses the rules
    if (type == cm::TargetType::STATIC_LIBRARY && !linkerLanguage.empty()) {
      cmMakefile const* targetMakefile = lg->GetMakefile();
      std::vector<std::string> rules;
      if (cmValue createRule = targetMakefile->GetDefinition(target->GetCreateRuleVariable(linkerLanguage, configName))) {
        rules.push_back(*createRule);
      } else {
        for (char const* rule : { "_ARCHIVE_CREATE", "_ARCHIVE_FINISH" }) {
          std::string const ruleVariable = target->GetFeatureSpecificLinkRuleVariable(
            cmStrCat("CMAKE_", linkerLanguage, rule), linkerLanguage, configName);
          rules.push_back(targetMakefile->GetSafeDefinition(ruleVariable));
        }
      }
      for (std::string const& rule : rules) {
        if (rule.find("<CMAKE_LINKER>") != std::string::npos ||
            rule.find(cmStrCat("<CMAKE_", linkerLanguage, "_COMPILER>")) != std::string::npos ||
            rule.find(cmStrCat("<CMAKE_", linkerLanguage, "_HOST_LINK_LAUNCHER>")) != std::string::npos) {
          bWrapsLink = true;
        }
      }
    }
    if (bWrapsLink) {
      if (!linkerLanguage.empty() &&
          fHasWrapperCommand(target->GetLinkWrappers(configName, linkerLanguage), "CMAKE_LINK_WRAPPER_", "LINK_WRAPPERS",
                             linkerLanguage)) {
        lg->GetMakefile()->IssueMessage(MessageType::FATAL_ERROR,
          cmStrCat("Target ", target->GetName(), " has link wrappers (LINK_WRAPPERS), which the Malterlib generator can't apply"));
      }
    }
  } else {
    std::vector<cmSourceFile*> sourceFiles;
    GetTargetFiles(sourceFiles, lg, target, makefile);
    AddFilesToRegistry(_ProjectName, outputTarget, sourceFiles, configName, lg, target, true);
  }

  AddBuildEvents(_ProjectName, outputTarget, lg, target, configName, isUtilityTarget);

  // The targets linked into this one. Libraries guarded by $<COMPILE_ONLY:...> only order the build, but their
  // dependency edges are link edges too.
  std::set<cmGeneratorTarget const*> linkedTargets;
  // Objects named by $<TARGET_OBJECTS:...> are linked, even when their library is also compile only.
  std::map<cmGeneratorTarget const*, std::set<std::string>> linkedObjects;
  {
    bool const bStaticLibrary = target->GetType() == cm::TargetType::STATIC_LIBRARY;
    bool const bObjectLibrary = IsObjectLib(target);
    // Neither has a link step: what they link is passed on to the targets that link them
    bool const bPassesOnLinks = bStaticLibrary || bObjectLibrary;
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

    // The objects among the sources of an object library are only built before it: they aren't among its objects
    if (!bObjectLibrary) {
      std::vector<cmSourceFile const*> objectFiles;
      target->GetExternalObjects(objectFiles, configName);
      for (cmSourceFile const* objectFile : objectFiles)
        fAddObjectLibrary(objectFile);
    }
    // A static or object library doesn't link the objects that link interfaces name with $<TARGET_OBJECTS:...>, nor are they
    // in an archive: the targets that link it link them, and CMake evaluates the interfaces for each of those targets. A
    // Malterlib dependency is passed on to every target that depends on the library, also to native ones, so an object library
    // only becomes a dependency of a static library when the interfaces name its objects for every target: those of the link
    // interface of the library, which has what it links privately as well, and of the libraries reached from it, by entries
    // of the interfaces without a condition on the target that links (head sensitive), all the way. And only an object
    // library that the import builds, with all of its objects. What a target of the import that links gets is decided for
    // that target. An object library passes on no objects: a target that only links its objects doesn't get its interface,
    // but would get what a dependency of it passes on.
    std::map<cmGeneratorTarget*, std::set<std::string>> passedOnObjects;
    auto fPassOnObjects = [&](std::vector<cmLinkItem> const& objects) {
      if (!bStaticLibrary)
        return;
      for (cmLinkItem const& object : objects) {
        if (object.HeadSensitive || !object.ObjectSource || object.ObjectSource->GetObjectLibrary().empty())
          continue;
        cmGeneratorTarget* objectTarget = fBuiltObjectLibrary(object.ObjectSource);
        if (objectTarget && objectTarget != target)
          passedOnObjects[objectTarget].insert(object.ObjectSource->GetFullPath());
      }
    };

    struct cmMalterlibLinked
    {
      cmGeneratorTarget const* Target;
      bool bConditional; // Reached through an entry with a condition on the target that links, for a static or object library
      // Reached through the link interface of the static or object library itself, which CMake only evaluates for the targets
      // that link it: only its entries without such a condition, evaluated quietly
      bool bOwnInterface;
    };
    std::vector<cmMalterlibLinked> pending;
    // CMake links the objects of an object library where it is linked directly, not through the interfaces of libraries,
    // which pass on objects as $<TARGET_OBJECTS:...>. Qt passes on those of the initializers of its static plugins without
    // conditions on the target that links when the Malterlib generator imports it, so that a static library passes them on
    // to the native targets that depend on it as objects of its interface.
    std::set<cmGeneratorTarget const*> directlyLinked;
    if (cmLinkImplementation const* impl = target->GetLinkImplementation(configName, cmGeneratorTarget::UseTo::Link)) {
      if (!bPassesOnLinks)
        fAddLinkedObjects(impl->Objects);
      for (cmLinkItem const& lib : impl->Libraries) {
        // INTERFACE_LINK_LIBRARIES_DIRECT of a library adds the objects that it names as paths, which CMake links as files.
        // Their sources are of the directory of the library that has the property. Only for the target that links.
        if (!lib.Target && lib.InterfaceDirectFrom && !bPassesOnLinks && cmSystemTools::FileIsFullPath(lib.AsStr())) {
          cmSourceFile const* object =
            lib.InterfaceDirectFrom->GetLocalGenerator()->GetMakefile()->GetSource(lib.AsStr(), cmSourceFileLocationKind::Known);
          if (object && object->GetPropertyAsBool("EXTERNAL_OBJECT"))
            fAddObjectLibrary(object);
          continue;
        }
        if (lib.Target) {
          // For a static or object library these are dependencies; what it passes on is found from its link interface
          pending.push_back({ lib.Target, bPassesOnLinks });
          directlyLinked.insert(lib.Target);
        }
      }
    }
    if (bStaticLibrary) {
      cmLinkInterfaceLibraries const iface = target->GetMalterlibUnconditionalLinkInterface(configName, target);
      fPassOnObjects(iface.Objects);
      for (cmLinkItem const& lib : iface.Libraries) {
        if (lib.Target)
          pending.push_back({ lib.Target, false, true });
      }
    }
    // Whether only reached conditionally, for each walk: that through the link interface of the library itself, which only
    // passes on objects, doesn't stand for the walk through what it links, which decides what it links
    std::map<std::pair<cmGeneratorTarget const*, bool>, bool> visited;
    std::set<cmGeneratorTarget const*> reached; // By either walk
    // CMake puts the objects of the object libraries that a static library links in its archive. Malterlib links them where
    // the static library is linked.
    std::set<cmGeneratorTarget const*> archivedObjectLibraries;
    while (!pending.empty()) {
      cmMalterlibLinked const linked = pending.back();
      pending.pop_back();
      auto iVisited = visited.find({ linked.Target, linked.bOwnInterface });
      // Again when it was only reached conditionally before
      if (iVisited != visited.end() && (!iVisited->second || linked.bConditional))
        continue;
      visited[{ linked.Target, linked.bOwnInterface }] = linked.bConditional;
      bool const bFirst = reached.insert(linked.Target).second;
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
      cmLinkInterfaceLibraries ownInterface;
      cmLinkInterfaceLibraries const* iface = nullptr;
      if (linked.bOwnInterface) {
        ownInterface = linked.Target->GetMalterlibUnconditionalLinkInterface(configName, target);
        iface = &ownInterface;
      } else
        iface = linked.Target->GetLinkInterfaceLibraries(configName, target, cmGeneratorTarget::UseTo::Link);
      if (iface) {
        if (!bPassesOnLinks)
          fAddLinkedObjects(iface->Objects);
        else if (!linked.bConditional)
          fPassOnObjects(iface->Objects);
        for (cmLinkItem const& lib : iface->Libraries) {
          if (lib.Target)
            pending.push_back({ lib.Target, bPassesOnLinks && (linked.bConditional || lib.HeadSensitive), linked.bOwnInterface });
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
    // An object library has no link step and links nothing: a Malterlib dependency of a target that only links its objects
    // ($<TARGET_OBJECTS:...>) would get what it passes on, which CMake doesn't give that target. The targets that link it by
    // name get what it links by their own evaluation.
    if (!bObjectLibrary) {
      for (cmGeneratorTarget const* linked : reached) {
        // Nor does a target link the objects of the object libraries that it only reaches through what it links: for it they
        // are as interface libraries
        if (IsObjectLib(linked) && !directlyLinked.count(linked))
          continue;
        linkedTargets.insert(linked);
      }
      // Those of the sources aren't in the link closure
      linkedTargets.insert(archivedObjectLibraries.begin(), archivedObjectLibraries.end());
    }

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
        // An object library passes on nothing
        if (IsStaticLib(next) && !IsObjectLib(next))
          pending.push_back(next);
      }
    }
    return linked;
  };

  std::set<std::string> addedDependencies;
  std::map<std::string, cmMalterlibRegistry *> linkedDependencies;
  auto fAddDependencies = [&](cmGeneratorTarget const *_pTarget, bool _bOnlyObjects, std::size_t _Depth) -> void
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
            AddTargetCompileInfo(*this, compileTypeInfo, &*dependency, dependencyLocalGenerator, configName);
          }
          continue;
        }*/
        if (_bOnlyObjects)
          continue;

        std::string const dependencyName = GetTargetName(dependency, dependency->LocalGenerator->GetProjectName());
        addedDependencies.insert(dependencyName);
        auto &outputDependency = outputTarget.addChild("%Dependency", dependencyName);

        // The edge doesn't say whether the target links the dependency: CMake keeps one edge per dependency, and an edge of
        // add_dependencies or of $<TARGET_OBJECTS> that only compiles can stand for a link too. The targets linked decide.
        bool const bLinked = linkedTargets.count(dependency) != 0 && dependency->GetType() != cm::TargetType::INTERFACE_LIBRARY;
        if (!bLinked) {
          outputDependency.addChild("Dependency.Link", "false").RawValue = true;
          // Before compiling, as for the headers it can generate
          outputDependency.addChild("Dependency.Stage", "\"Compile\"").RawValue = true;
        } else {
          linkedDependencies[dependencyName] = &outputDependency;
          // As CMake orders the objects of the target after the commands of the targets that it links, which can generate the
          // headers of the library
          outputDependency.addChild("Dependency.CompileAfterGenerated", "true").RawValue = true;
          if (IsStaticLib(target) && IsStaticLib(dependency)) {
            outputDependency.addChild("Dependency.Indirect", "true").RawValue = true;
            outputDependency.addChild("Dependency.IndirectOrdered", "true").RawValue = true;
          }
        }
      }
    }
  ;

  fAddDependencies(target, false, 0);

  // A Malterlib dependency also orders the build, so a library that is built after the target, as in a cycle of static
  // libraries, can't be a dependency of it. Targets that depend on the target natively would not link the library.
  // An object library links nothing, as above
  if (!isUtilityTarget && !IsObjectLib(target)) {
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
      std::string const dependencyName = GetTargetName(library, library->LocalGenerator->GetProjectName());
      auto &outputDependency = outputTarget.addChild("%Dependency", dependencyName);
      addedDependencies.insert(dependencyName);
      linkedDependencies[dependencyName] = &outputDependency;
      outputDependency.addChild("Dependency.CompileAfterGenerated", "true").RawValue = true;
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
      std::string const dependencyName = GetTargetName(objectLibrary, objectLibrary->LocalGenerator->GetProjectName());
      auto &outputDependency = outputTarget.addChild("%Dependency", dependencyName);
      addedDependencies.insert(dependencyName);
      linkedDependencies[dependencyName] = &outputDependency;
      if (IsStaticLib(target)) {
        outputDependency.addChild("Dependency.Indirect", "true").RawValue = true;
        outputDependency.addChild("Dependency.IndirectOrdered", "true").RawValue = true;
      }
    }
  }

  if (!isUtilityTarget) {
    // The definitions stay in the order that CMake sorts them in, with their newlines, which decides which definition of a
    // macro the compiler uses, and are only joined as ninja joins them here
    auto fDefinesValue = [&](std::set<std::string> const& _Defines) {
      std::vector<std::string> values;
      for (auto &define : _Defines) {
        // Paths in the value of a definition, which the compile command quotes as a whole. An @ of the value is a literal @
        // of the eval string, as of @( in the value, which the import cache would otherwise evaluate. A compile only needs the
        // path of the file of a target, not the file, so a definition doesn't order the build after the target, as in CMake.
        MapPathsOnly = true;
        auto Remapped =
          replaceMappedOutputFiles(_ProjectName, joinNinjaContinuations(std::string(define)), MappedForm::EvalString);
        MapPathsOnly = false;
        if (Remapped.find('@') != std::string::npos) {
          // Only the literal parts are escaped, as the expressions, as of a file beside the file of a target, have their own
          std::string Escaped = "`";
          addEvalStringText(Escaped, Remapped);
          Escaped += '`';
          values.push_back(Escaped);
        } else
          values.push_back(cmMalterlibRegistry::getEscaped(Remapped, true, true));
      }
      return cmJoin(values, ", ");
    };
    // Sources of a compile type that the target has no settings for get all their definitions
    for (cmMalterlibSourceDefines const& source : SourceDefines) {
      if (!compileTypeInfo.count(source.CompileType) && !source.Defines->empty())
        source.File->addChild("Compile.PreprocessorDefines", "=+ [" + fDefinesValue(*source.Defines) + "]").RawValue = true;
    }
    for (auto &infoMap : compileTypeInfo) {
      auto &info = infoMap.second;
      {
        for (auto &path : info.Includes)
          path = GetSearchDirectoryValue(_ProjectName, collapseSearchDirectory(path));
        auto end = cmRemoveDuplicates(info.Includes);
        info.Includes.erase(end, info.Includes.end());
      }
      auto &compileOutput = outputTarget.addChild("Compile", "", true);
      compileOutput.addChild("!!Compile.Type", infoMap.first);
      compileOutput.addChild("SearchPath", "+= [" + cmJoin(info.Includes, ", ") + "]").RawValue = true;
      if (!info.SystemIncludes.empty()) {
        for (auto &path : info.SystemIncludes)
          path = GetSearchDirectoryValue(_ProjectName, collapseSearchDirectory(path));
        auto end = cmRemoveDuplicates(info.SystemIncludes);
        info.SystemIncludes.erase(end, info.SystemIncludes.end());
        compileOutput.addChild("SystemSearchPath", "+= [" + cmJoin(info.SystemIncludes, ", ") + "]").RawValue = true;
      }
      auto fAddFrameworkDirectories = [&](std::vector<std::string>& _Directories, char const* _pKey) {
        if (_Directories.empty())
          return;
        for (auto &path : _Directories)
          path = GetSearchDirectoryValue(_ProjectName, collapseSearchDirectory(path));
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
        std::set<std::set<std::string> const*> comparedDefines;
        for (cmMalterlibSourceDefines const& source : SourceDefines) {
          if (source.CompileType != infoMap.first || !comparedDefines.insert(source.Defines.get()).second)
            continue;
          for (std::string const& define : *source.Defines) {
            if (!info.Defines.count(define))
              sourceMacros.insert(macroName(define));
          }
          for (std::string const& define : info.Defines) {
            if (!source.Defines->count(define))
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
          for (std::string const& define : *source.Defines) {
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

  // A target whose file the target uses, which CMake orders by the file alone, as for a command that depends on its
  // $<TARGET_FILE>, is a dependency too, where GetTargetFileExpression finds the path of the file. The commands run after it,
  // also after one that the target links, which otherwise only orders the link and the files that its commands generate.
  for (std::string const &dependencyName : TargetFileDependencies) {
    auto const iLinked = linkedDependencies.find(dependencyName);
    if (iLinked != linkedDependencies.end())
      iLinked->second->addChild("Dependency.Stage", "\"Compile\"").RawValue = true;
    if (addedDependencies.count(dependencyName))
      continue;
    auto &outputDependency = outputTarget.addChild("%Dependency", dependencyName);
    outputDependency.addChild("Dependency.Link", "false").RawValue = true;
    outputDependency.addChild("Dependency.Stage", "\"Compile\"").RawValue = true;
  }
  // A target that only names where its files are, as a working directory, only gives the target the properties of its target
  for (std::string const &dependencyName : TargetPathDependencies) {
    if (TargetFileDependencies.count(dependencyName) || addedDependencies.count(dependencyName) ||
        linkedDependencies.count(dependencyName))
      continue;
    auto &outputDependency = outputTarget.addChild("%Dependency", dependencyName);
    outputDependency.addChild("Dependency.Link", "false").RawValue = true;
    outputDependency.addChild("Dependency.Stage", "\"None\"").RawValue = true;
  }

  for (auto &child : outputTarget.Children) {
    if (child.Key == "%Group")
      child.pruneLoneChildren();
  }
}
