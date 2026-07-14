#ifndef KAIROS_EXEC_ENGINE_EXIT_CODES_H_
#define KAIROS_EXEC_ENGINE_EXIT_CODES_H_

namespace kairos::exec {

// Distinct non-zero trader exits the supervisor classifies on. kNoJournalExit and
// kHaltExit are deliberate fail-closed stops a restart cannot fix (terminal);
// kConnectFailExit is a transient backend failure that stays restart-eligible.
constexpr int kNoJournalExit = 2;
constexpr int kConnectFailExit = 3;
constexpr int kHaltExit = 17;

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_ENGINE_EXIT_CODES_H_
