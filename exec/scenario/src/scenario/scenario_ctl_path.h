#ifndef KAIROS_EXEC_SCENARIO_CTL_PATH_H_
#define KAIROS_EXEC_SCENARIO_CTL_PATH_H_

#include <cstdlib>
#include <string>

#include "socket_path.h"

namespace kairos::exec {

// Supervisor control UDS (TUI/operator <-> supervisor). Same env>XDG>run-user
// precedence as the order hub; never /tmp. $KAIROS_SCENARIO_CTL_SOCK overrides.
inline std::string ScenarioCtlSocketPath() {
  std::string p =
      ResolveSock(std::getenv("KAIROS_SCENARIO_CTL_SOCK"), std::getenv("XDG_RUNTIME_DIR"),
                  RunUserDir(), "kairos-scenario-ctl.sock");
  if (p.empty()) NoRuntimeDir("KAIROS_SCENARIO_CTL_SOCK");
  return p;
}

}  // namespace kairos::exec

#endif  // KAIROS_EXEC_SCENARIO_CTL_PATH_H_
