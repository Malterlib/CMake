/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */

#include "cmTargetDepend.h"
#include "cmGeneratorTarget.h"

bool operator<(cmTargetDepend const& l, cmTargetDepend const& r)
{
  auto const& leftName = l.Target->GetName();
  auto const& rightName = r.Target->GetName();

  if (leftName < rightName)
      return true;
  else if (rightName < leftName)
      return false;

  return l.Target < r.Target;
}
