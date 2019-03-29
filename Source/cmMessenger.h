/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include "cmConfigure.h" // IWYU pragma: keep

#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include <cm/optional>

#include "cmDiagnosticContext.h"
#include "cmDiagnostics.h"
#include "cmListFileCache.h"
#include "cmMessageType.h" // IWYU pragma: keep

class cmStateSnapshot;

#ifdef CMake_ENABLE_DEBUGGER
namespace cmDebugger {
class cmDebuggerAdapter;
}
#endif

class cmMessenger
{
public:
  void IssueMessage(
    MessageType type, std::string const& text,
    cmListFileBacktrace const& backtrace = cmListFileBacktrace());

  void IssueDiagnostic(cmDiagnosticCategory category, std::string const& text,
                       cmStateSnapshot const& fallbackContext,
                       cmDiagnosticContext const& context = {});

  void DisplayMessage(MessageType type, cmDiagnosticCategory category,
                      std::string const& text,
                      cmListFileBacktrace const& backtrace);

  void SetTopSource(cm::optional<std::string> topSource);

  // Print the top of a backtrace.
  void PrintBacktraceTitle(std::ostream& out,
                           cmListFileBacktrace const& bt) const;
#ifdef CMake_ENABLE_DEBUGGER
  void SetDebuggerAdapter(
    std::shared_ptr<cmDebugger::cmDebuggerAdapter> const& debuggerAdapter)
  {
    DebuggerAdapter = debuggerAdapter;
  }
#endif

  struct Message
  {
    MessageType Type;
    cmDiagnosticCategory Category;
    cmListFileBacktrace Backtrace;
    std::string Text;
  };

  std::vector<Message> const& GetDisplayedMessages() const
  {
    return this->DisplayedMessages;
  }

  // For the Malterlib generator: while it exists, messages and diagnostics
  // are dropped, and errors aren't recorded, as for the link interfaces that
  // it evaluates on top of those that CMake evaluates, of which the targets
  // that use them report what is wrong.
  class MalterlibSuppressScope
  {
  public:
    explicit MalterlibSuppressScope(cmMessenger& messenger)
      : Messenger(messenger)
    {
      ++this->Messenger.MalterlibSuppressDepth;
    }
    ~MalterlibSuppressScope() { --this->Messenger.MalterlibSuppressDepth; }
    MalterlibSuppressScope(MalterlibSuppressScope const&) = delete;
    MalterlibSuppressScope& operator=(MalterlibSuppressScope const&) = delete;

  private:
    cmMessenger& Messenger;
  };

private:
  int MalterlibSuppressDepth = 0;

  cm::optional<std::string> TopSource;

  std::vector<Message> DisplayedMessages;

#ifdef CMake_ENABLE_DEBUGGER
  std::shared_ptr<cmDebugger::cmDebuggerAdapter> DebuggerAdapter;
#endif
};
