/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */

#include "cmMalterlibRegistry.h"

#include <assert.h>

namespace
{
  char *strEscapeStr(char *_pStrDest, const char *_pStrSource, 
                     const char *_pEscapeChars, const char *_pReplaceChars, bool _bAddQuotes) {
    assert(strlen(_pEscapeChars) == strlen(_pReplaceChars));

    char *pDest = _pStrDest;

		if (_bAddQuotes)
			*(pDest++) = _pReplaceChars[0];
    const char *pParse = _pStrSource;

    while (*pParse) {
      size_t iEscape = 0;
      while (_pEscapeChars[iEscape]) {
        if (*pParse == _pEscapeChars[iEscape])
          break;
        ++iEscape;
      }
      if (_pEscapeChars[iEscape]) {
        *(pDest++) = '\\';
        *(pDest++) = _pReplaceChars[iEscape];
        ++pParse;
        continue;
      }
      *(pDest++) = *(pParse++);
    }
		if (_bAddQuotes)
			*(pDest++) = _pReplaceChars[0];
    *(pDest++) = 0;
    return _pStrDest;
  }
  
  template <bool t_bEscapeNewLines>
  static std::string getEscapedStr(const std::string &_Str, bool _bForceEscape, 
                                   const std::string &_PreData) {
    std::string toReturn;
    bool bNeedEscape = _bForceEscape;

    if (_Str.empty())
      bNeedEscape = true;

    if (!bNeedEscape) {
      size_t Len = _Str.length();

      char Current;
      char Prev = 0;
      for (size_t i = 0; i < Len; ++i, (Prev = Current)) {
        Current = _Str[i];
        if (Current == '\"' || Current == '{' || Current == '#' 
            || Current == '\\') {
          bNeedEscape = true;
          break;
        }
        else if (Prev == '/' && (Current == '*' || Current == '/')) {
          // Strings containing comments.
          bNeedEscape = true;
          break;
        }
        else if (Current == '.' || Current == '%' || Current == '&' || Current == '|' || Current == '!' || Current == '_' || Current == '+' || Current == '-') {
        }
        else if (Current < '0') {
          bNeedEscape = true;
          break;
        }
        else if (Current > '9' && Current < 'A') {
          bNeedEscape = true;
          break;
        }
        else if (Current > 'Z' && Current < 'a') {
          bNeedEscape = true;
          break;
        }
        else if (Current > 'z') {
          bNeedEscape = true;
          break;
        }
        else if (t_bEscapeNewLines) {
          if (Current == '\n') {
            bNeedEscape = true;
            break;
          }
        }
      }
    }

    if (bNeedEscape) {
      if (t_bEscapeNewLines) {
        size_t Len = _Str.length();
        size_t iStart = 0;
        for (size_t i = 0; i < Len; ++i) {
          char Current = _Str[i];
          if (Current == '\n') {
            cmMalterlibRegistry::addEscapeStr(toReturn, _Str.substr(iStart, (i+1)-iStart),
                         "\"\\\r\n\t", "\"\\rnt", true);
            toReturn += "\\\n";
            toReturn += _PreData;
            iStart = i+1;
          }
        }
        cmMalterlibRegistry::addEscapeStr(toReturn, _Str.substr(iStart, Len - iStart),
                     "\"\\\r\n\t", "\"\\rnt", true);
      }
      else
        cmMalterlibRegistry::addEscapeStr(toReturn, _Str, "\"\\\r\n\t", "\"\\rnt", true);
    }
    else
      toReturn = _Str;
    return toReturn;
  }

  std::string makeTabs(std::string const &str) {
    size_t numChars = 0;
    for (auto const &character : str) {
      if (character == '\t')
        numChars += 4;
      else
        ++numChars;
    }
    size_t numTabs = numChars / 4;
    std::string returnString = std::string(numTabs, '\t');
    returnString.insert(returnString.length(), numChars - numTabs * 4, ' ');
    return returnString;
  }  
}

std::string &cmMalterlibRegistry::addEscapeStr(std::string &_StrDest, const std::string &_StrSource, const char *_pEscapedChars, const char *_pReplaceChars, bool _bAddQuotes)
{
	const char *pSource = _StrSource.c_str();
	const char *pParse = pSource;
	size_t NeededSize = _bAddQuotes ? 3 : 1;
	while (*pParse) {
		const char *pEscape = _pEscapedChars;
		while (*pEscape) {
			if (*pParse == *pEscape)
				break;
			++pEscape;
		}
		if (*pEscape)
			NeededSize += 2;
		else
			++NeededSize;

		++pParse;
	}

	size_t currentLength = _StrDest.length();
	_StrDest.resize(currentLength + NeededSize);
	char *pDest = &_StrDest[currentLength];
	strEscapeStr(pDest, pSource, _pEscapedChars, _pReplaceChars, _bAddQuotes);
	_StrDest.resize(strlen(&_StrDest[0]));
	return _StrDest;
}


std::string cmMalterlibRegistry::getEscaped(std::string const &_Str, bool _bForceEscape, bool _bEscapeNewLines) {
  if (_bEscapeNewLines)
    return getEscapedStr<true>(_Str, _bForceEscape, {});
  else
    return getEscapedStr<false>(_Str, _bForceEscape, {});
}

cmMalterlibRegistry &cmMalterlibRegistry::setChild(std::string const &key, 
                                                   std::string const &value) {
  auto found = ChildrenMap.find(key);
  if (found != ChildrenMap.end()) {
    auto &Child = *found->second;
    Child.Value = value;
    return Child; 
  }
  return addChild(key, value);
}

cmMalterlibRegistry &cmMalterlibRegistry::addUniqueChild(
  std::string const &key, 
  std::string const &value) {
  
  auto found = ChildrenValueMap.find(
    std::pair<std::string, std::string>(key, value));
  if (found != ChildrenValueMap.end())
    return *found->second; 
  return addChild(key, value);
}

cmMalterlibRegistry &cmMalterlibRegistry::addChild(std::string const &key, 
                                                   std::string const &value,
                                                   bool pushFront) {
  cmMalterlibRegistry child;
  child.Key = key;
  child.Value = value;
  cmMalterlibRegistry *added;
  if (pushFront) {
    Children.push_front(child);
    added = &Children.front(); 
  } else {
    Children.push_back(child);
    added = &Children.back(); 
  }
  ChildrenMap[key] = added;
  ChildrenValueMap[std::pair<std::string, std::string>(key, value)] = added;
  return *added;
}

void cmMalterlibRegistry::output(cmGeneratedFileStream &stream) {
  for (auto &child : Children)
    child.outputRecursive(stream, std::string{});
}

void cmMalterlibRegistry::outputRecursive(cmGeneratedFileStream &stream, 
                                          std::string const &indent) {
  if (!Value.empty() || Children.empty()) {
    std::string prefix;
    prefix = indent;
    if (RawKey)
      prefix += Key;
    else
      prefix += getEscapedStr<false>(Key, false, std::string());
    prefix += " ";
    stream << prefix;
    prefix = makeTabs(prefix);
    if (RawValue)
      stream << Value;
    else
      stream << getEscapedStr<true>(Value, Value != "true" && Value != "false", prefix);
  } else {
    stream << indent;
    stream << getEscapedStr<false>(Key, false, std::string());
  }
  stream << "\n";
  
  if (Children.empty())
    return;
  
  stream << indent;
  stream << "{\n";
  std::string newIndent = indent;
  newIndent += "\t";
  for (auto &child : Children)
    child.outputRecursive(stream, newIndent);
  stream << indent;
  stream << "}\n";
}

cmMalterlibRegistry cmMalterlibRegistry::pruneLoneChildrenRecursive()
{
  if (Children.size() == 1 && !Protected && Key == "%Group")
    return Children.front().pruneLoneChildrenRecursive();
  return *this;
}

void cmMalterlibRegistry::pruneLoneChildren() {
  if (Children.size() == 1) {
    Children.front() = Children.front().pruneLoneChildrenRecursive();
  }
}
