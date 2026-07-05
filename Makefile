# Kairos top-level convenience targets. The E2E sim drills are the primary entry.
#
#   make sim-e2e   run the off-hours end-to-end drills (see core/tests/SIM_E2E.md)

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SIM_HUBD := $(ROOT)/exec/scenario/build/kairos_sim_hubd
TRADER := $(ROOT)/exec/scenario/build/kairos_scenario_trader
SIM_BIN := $(ROOT)/core/target/release/kairos-sim

.PHONY: sim-e2e sim-e2e-help

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
