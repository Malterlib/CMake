/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */

#pragma once

#include <list>
#include <map>
#include <string>
#include "cmGeneratedFileStream.h"

class cmMalterlibRegistry
{
public:
  cmMalterlibRegistry &addChild(std::string const &key, 
                                std::string const &value = std::string{},
                                bool pushFront = false);
  cmMalterlibRegistry &setChild(std::string const &key, 
                                std::string const &value);
  cmMalterlibRegistry &addUniqueChild(std::string const &key, 
                                      std::string const &value);
  void output(cmGeneratedFileStream &stream);
  void pruneLoneChildren();
  std::vector<cmMalterlibRegistry>::iterator getChildIterator(); 

  std::string Key;
  std::string Value;
  std::list<cmMalterlibRegistry> Children;
  std::map<std::string, cmMalterlibRegistry *> ChildrenMap;
  std::map<std::pair<std::string, std::string>, cmMalterlibRegistry *> 
    ChildrenValueMap;
  bool Protected = false;
  
private:
  void outputRecursive(cmGeneratedFileStream &stream, 
                       std::string const &indent);
  cmMalterlibRegistry pruneLoneChildrenRecursive();
  
};
