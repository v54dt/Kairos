# Kairos top-level convenience targets. The E2E sim drills are the primary entry.
#
#   make sim-e2e   run the off-hours end-to-end drills (see core/tests/SIM_E2E.md)

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
EXEC_BUILD := $(ROOT)/exec/scenario/build
SIM_HUBD := $(EXEC_BUILD)/kairos_sim_hubd
TRADER := $(EXEC_BUILD)/kairos_scenario_trader
SUPERVISORD := $(EXEC_BUILD)/kairos_scenario_supervisord
FAULT_DRILLS := $(EXEC_BUILD)/test_sim_fault_drills
RT_FAULT_DRILLS := $(EXEC_BUILD)/test_roundtrip_fault_drills
SIM_BIN := $(ROOT)/core/target/release/kairos-sim

.PHONY: sim-e2e sim-e2e-help drills

sim-e2e:
	@echo "sim-e2e: building release core binaries (kairos-sim + siblings)"
	cd $(ROOT)/core && cargo build --release --bins
	@if [ ! -x "$(SIM_HUBD)" ] || [ ! -x "$(TRADER)" ]; then \
	  echo "sim-e2e: FAIL — missing C++ binaries."; \
	  echo "  expected: $(SIM_HUBD)"; \
	  echo "            $(TRADER)"; \
	  echo "  build them: cmake -S $(ROOT)/exec/scenario -B $(ROOT)/exec/scenario/build \\"; \
	  echo "                -DCMAKE_BUILD_TYPE=Release && \\"; \
	  echo "              cmake --build $(ROOT)/exec/scenario/build \\"; \
	  echo "                --target kairos_sim_hubd kairos_scenario_trader"; \
	  exit 1; \
	fi
	@echo "sim-e2e: running the trader fills->journal->teardown drill"
	cd $(ROOT)/core && KAIROS_SIM_HUBD=$(SIM_HUBD) KAIROS_SCENARIO_TRADER=$(TRADER) \
	  cargo test --test sim_e2e -- --ignored --nocapture
	@echo "sim-e2e: running the tui-reads-sim-quotes smoke"
	cd $(ROOT)/tui && KAIROS_SIM_BIN=$(SIM_BIN) KAIROS_SIM_HUBD=$(SIM_HUBD) \
	  cargo test -p kairos-tui --test sim_quote_smoke -- --ignored --nocapture
	@echo "sim-e2e: PASS — trader filled, journal recorded, tui got sim quotes, no orphans"

sim-e2e-help:
	@echo "make sim-e2e — off-hours end-to-end sim drills (docs: core/tests/SIM_E2E.md)"
	@echo "  1) trader fills the trend_day tape through the isolated sim, journal records it,"
	@echo "     teardown leaves no orphan process groups"
	@echo "  2) the tui feed data source receives real sim quotes"
	@echo "These are #[ignore] tests, excluded from the default cargo test CI job."

# The FULL env-gated drill catalog in one place (docs: docs/RUNBOOK.md section 5).
# Every required env var is set here; fail-fast per step; PASS summary at the end.
drills:
	@echo "drills: building release core binaries (kairos-sim + siblings)"
	cd $(ROOT)/core && cargo build --release --bins
	@if [ ! -x "$(SIM_HUBD)" ] || [ ! -x "$(TRADER)" ] || [ ! -x "$(SUPERVISORD)" ] || [ ! -x "$(FAULT_DRILLS)" ] || [ ! -x "$(RT_FAULT_DRILLS)" ]; then \
	  echo "drills: FAIL — missing C++ binaries under $(EXEC_BUILD)."; \
	  echo "  expected: $(SIM_HUBD)"; \
	  echo "            $(TRADER)"; \
	  echo "            $(SUPERVISORD)"; \
	  echo "            $(FAULT_DRILLS)"; \
	  echo "            $(RT_FAULT_DRILLS)"; \
	  echo "  build them: cmake -S $(ROOT)/exec/scenario -B $(EXEC_BUILD) \\"; \
	  echo "                -DCMAKE_BUILD_TYPE=Release && cmake --build $(EXEC_BUILD) -j"; \
	  exit 1; \
	fi
	@echo "drills: [1/10] core sim_e2e (trader fills -> journal -> teardown)"
	cd $(ROOT)/core && KAIROS_SIM_HUBD=$(SIM_HUBD) KAIROS_SCENARIO_TRADER=$(TRADER) \
	  cargo test --test sim_e2e -- --ignored --nocapture
	@echo "drills: [2/10] tui sim_quote_smoke (tui reads sim quotes)"
	cd $(ROOT)/tui && KAIROS_SIM_BIN=$(SIM_BIN) KAIROS_SIM_HUBD=$(SIM_HUBD) \
	  cargo test -p kairos-tui --test sim_quote_smoke -- --ignored --nocapture
	@echo "drills: [3/10] core core_watchdog_e2e (SIGTERM/SIGKILL hardening)"
	cd $(ROOT)/core && cargo test --test core_watchdog_e2e -- --ignored --nocapture
	@echo "drills: [4/10] core tapegen_sim_e2e (generated tape flows quotes+trades)"
	cd $(ROOT)/core && cargo test --release --test tapegen_sim_e2e -- --ignored --nocapture
	@echo "drills: [5/10] core sim_orphan_startup (SIGTERM during startup, no orphans)"
	cd $(ROOT)/core && cargo test --test sim_orphan_startup -- --ignored --nocapture
	@echo "drills: [6/10] core sim_roundtrip (replay -> sim quote socket, no orphans)"
	cd $(ROOT)/core && KAIROS_SIM_HUBD=$(SIM_HUBD) \
	  cargo test --test sim_roundtrip -- --ignored --nocapture
	@echo "drills: [7/10] tui supervisor_e2e (Rust client drives a real supervisor)"
	cd $(ROOT)/tui && KAIROS_SIM_BIN=$(SIM_BIN) KAIROS_SIM_HUBD=$(SIM_HUBD) \
	  KAIROS_SCENARIO_TRADER=$(TRADER) KAIROS_SCENARIO_SUPERVISORD=$(SUPERVISORD) \
	  cargo test -p kairos-tui --test supervisor_e2e -- --ignored --nocapture
	@echo "drills: [8/10] exec scenario_supervisor_e2e (supervisor owns/reaps a filling trader)"
	KAIROS_SUPERVISOR_E2E=1 ctest --test-dir $(EXEC_BUILD) -R scenario_supervisor_e2e --output-on-failure
	@echo "drills: [9/11] exec sim_fault_drills (ack-drop/reject/late-fill/disconnect)"
	KAIROS_FAULT_DRILLS=1 $(FAULT_DRILLS)
	@echo "drills: [10/11] exec scenario_restart_e2e (backoff/give-up/cancel)"
	ctest --test-dir $(EXEC_BUILD) -R scenario_restart_e2e --output-on-failure
	@echo "drills: [11/11] exec roundtrip_fault_drills (round-trip incident replays)"
	KAIROS_FAULT_DRILLS=1 $(RT_FAULT_DRILLS)
	@echo "drills: PASS — full env-gated catalog green (11 steps)"
