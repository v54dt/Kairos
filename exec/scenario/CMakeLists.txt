cmake_minimum_required(VERSION 3.24)
project(kairos_exec CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Sources are grouped into subdirs; includes stay flat (`#include "foo.h"`), so every target gets
# all the subdirs on its include path.
set(KAIROS_INC
    ${SRC}/market
    ${SRC}/quote
    ${SRC}/order
    ${SRC}/notify
    ${SRC}/engine
    ${SRC}/sim
    ${SRC}/blacklist
    ${SRC}/scenario)

include(FetchContent)
FetchContent_Declare(
  tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0)
FetchContent_MakeAvailable(tomlplusplus)

# Pre-satisfy libcapnp-dev's libatomic check (false-negative on Ubuntu 24.04).
set(FOUND_LIBATOMIC TRUE)
find_package(CapnProto CONFIG REQUIRED)
set(CAPNPC_SRC_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../schema")
set(CAPNPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/../../schema/kairos.capnp")

find_package(CURL REQUIRED)

# --- libraries ---
add_library(kairos_transport src/notify/http_poster.cpp)
target_include_directories(kairos_transport PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_transport PUBLIC CURL::libcurl)

add_library(kairos_notify src/notify/ntfy_dispatcher.cpp)
target_include_directories(kairos_notify PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_notify PUBLIC kairos_transport)

add_library(kairos_dashboard src/notify/dashboard_metrics.cpp)
target_include_directories(kairos_dashboard PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_dashboard PUBLIC kairos_transport)

# Generated Cap'n Proto code, compiled once and shared by both codecs (linking the generated symbols
# into a single binary twice would collide).
add_library(kairos_capnp ${CAPNP_SRCS})
target_include_directories(kairos_capnp PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kairos_capnp PUBLIC CapnProto::capnp)

add_library(kairos_quote_codec src/quote/quote_codec.cpp)
target_include_directories(kairos_quote_codec PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_quote_codec PUBLIC kairos_capnp)

add_library(kairos_order_codec src/order/order_codec.cpp)
target_include_directories(kairos_order_codec PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_order_codec PUBLIC kairos_capnp)

# Read-only hub status snapshot: JSON serialize + atomic file write (no SDK).
add_library(kairos_hub_status src/order/hub_status.cpp)
target_include_directories(kairos_hub_status PUBLIC ${KAIROS_INC})

# Order hub routing core (socket-agnostic): routes client orders <-> one backend.
add_library(kairos_order_hub src/order/order_hub.cpp)
target_include_directories(kairos_order_hub PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_order_hub PUBLIC kairos_order_codec kairos_hub_status
                                              kairos_order_journal kairos_scenario)

# UDS front end for the order hub: listen socket + per-client reader threads.
add_library(kairos_order_hub_server src/order/order_hub_server.cpp)
target_include_directories(kairos_order_hub_server PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_order_hub_server PUBLIC kairos_order_hub pthread)

# Scenario-side client: an OrderBackend that talks to the hub over UDS (no SDK).
add_library(kairos_hub_backend src/order/hub_order_backend.cpp)
target_include_directories(kairos_hub_backend PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_hub_backend PUBLIC kairos_order_codec pthread)

add_library(kairos_uds src/quote/uds_quote_client.cpp)
target_include_directories(kairos_uds PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_uds PUBLIC kairos_quote_codec pthread)

# Offline fill-model library (pure logic, no capnp/socket): conservative + ProbQueue continuous
# fills and the call-auction engine. Shared by A4 (offline) and B8 (live paper).
add_library(kairos_sim src/sim/fill_model.cpp src/sim/fill_engine.cpp src/sim/auction.cpp)
target_include_directories(kairos_sim PUBLIC ${KAIROS_INC})

# Mock-hub OrderBackend over the fill-model library (no SDK, no real-order path). The seeded fault
# injector is sim-only and links in here so faults are impossible to reach from the live hub.
add_library(kairos_sim_backend src/sim/sim_order_backend.cpp src/sim/fault_injector.cpp)
target_include_directories(kairos_sim_backend PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_sim_backend PUBLIC kairos_sim pthread)

add_library(kairos_blacklist src/blacklist/blacklist.cpp)
target_include_directories(kairos_blacklist PUBLIC ${KAIROS_INC})

# Scenario supervisor: JSON-lines control protocol (fail-closed request decode + status snapshot
# encode). Pure, no I/O.
add_library(kairos_scenario_ctl_proto src/scenario/scenario_ctl_proto.cpp)
target_include_directories(kairos_scenario_ctl_proto PUBLIC ${KAIROS_INC})

# Scenario supervisor: per-scenario state machine, trader stdout parsing, spawn argv, and
# scenario-dir enumeration. Pure logic (toml++ for enumeration).
add_library(kairos_scenario_state src/scenario/scenario_state.cpp)
target_include_directories(kairos_scenario_state PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_scenario_state PUBLIC kairos_scenario_ctl_proto
                                                   tomlplusplus::tomlplusplus)

# Scenario supervisor: process manager. Fork/execv + owns each trader child, one monitor thread per
# child (stdout -> state machine, EOF -> waitpid). No setsid.
add_library(kairos_scenario_process src/scenario/scenario_process.cpp)
target_include_directories(kairos_scenario_process PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_scenario_process PUBLIC kairos_scenario_state pthread)

# Scenario supervisor: UDS control server + the Supervisor that owns the process manager and answers
# list/start/stop with a status snapshot.
add_library(kairos_scenario_ctl_server src/scenario/scenario_ctl_server.cpp)
target_include_directories(kairos_scenario_ctl_server PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_scenario_ctl_server PUBLIC kairos_scenario_process pthread)

add_library(kairos_scenario src/engine/scenario.cpp)
target_include_directories(kairos_scenario PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_scenario PUBLIC tomlplusplus::tomlplusplus)

add_library(kairos_order_journal src/order/order_journal.cpp)
target_include_directories(kairos_order_journal PUBLIC ${KAIROS_INC})

add_library(kairos_engine src/engine/engine.cpp)
target_include_directories(kairos_engine PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_engine PUBLIC kairos_scenario kairos_dashboard kairos_order_journal
                                           pthread)

# Live order backend: real concords StockClient when the stock SDK is present in exec/scenario/lib,
# otherwise a stub so --live reports "no broker SDK". CI has no SDK and builds the stub.
set(CONCORDS_STOCK_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/concords_sdk.so)
if(EXISTS ${CONCORDS_STOCK_LIB})
  add_library(kairos_live src/order/concords_order_backend.cpp)
  target_include_directories(
    kairos_live
    PUBLIC ${KAIROS_INC}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
  target_link_libraries(
    kairos_live
    PUBLIC kairos_scenario
    PRIVATE ${CONCORDS_STOCK_LIB})
  set_target_properties(kairos_live PROPERTIES BUILD_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/lib
                                               INSTALL_RPATH ${CMAKE_CURRENT_SOURCE_DIR}/lib)
else()
  add_library(kairos_live src/order/live_backend_stub.cpp)
  target_include_directories(kairos_live PUBLIC ${KAIROS_INC})
  target_link_libraries(kairos_live PUBLIC kairos_scenario)
endif()

# --- executables ---
add_executable(kairos_quote_tap src/bin/quote_tap_main.cpp)
target_link_libraries(kairos_quote_tap PRIVATE kairos_uds)

add_executable(kairos_scenario_trader src/bin/scenario_trader_main.cpp)
target_link_libraries(
  kairos_scenario_trader PRIVATE kairos_engine kairos_hub_backend kairos_sim_backend kairos_notify
                                 kairos_uds kairos_blacklist)

add_executable(kairos_order_hubd src/bin/order_hub_main.cpp)
target_link_libraries(kairos_order_hubd PRIVATE kairos_order_hub_server kairos_live
                                                tomlplusplus::tomlplusplus)

# Mock order hub: fill-model backend + quote/trade UDS feed. No kairos_live link.
add_executable(kairos_sim_hubd src/bin/sim_hub_main.cpp)
target_link_libraries(kairos_sim_hubd PRIVATE kairos_sim_backend kairos_order_hub_server kairos_uds)

# Scenario supervisor daemon: single owner of every scenario-trader lifecycle.
add_executable(kairos_scenario_supervisord src/bin/scenario_supervisor_main.cpp)
target_link_libraries(kairos_scenario_supervisord PRIVATE kairos_scenario_ctl_server)

# --- tests ---
enable_testing()
add_executable(test_scenario_ctl_proto tests/test_scenario_ctl_proto.cpp)
target_link_libraries(test_scenario_ctl_proto PRIVATE kairos_scenario_ctl_proto)
add_test(NAME test_scenario_ctl_proto COMMAND test_scenario_ctl_proto)

add_executable(test_scenario_state tests/test_scenario_state.cpp)
target_link_libraries(test_scenario_state PRIVATE kairos_scenario_state)
add_test(NAME test_scenario_state COMMAND test_scenario_state)

add_executable(test_scenario_process tests/test_scenario_process.cpp)
target_link_libraries(test_scenario_process PRIVATE kairos_scenario_process)
add_test(NAME test_scenario_process COMMAND test_scenario_process)

add_executable(test_scenario_restart tests/test_scenario_restart.cpp)
target_link_libraries(test_scenario_restart PRIVATE kairos_scenario_process)
add_test(NAME test_scenario_restart COMMAND test_scenario_restart)

add_executable(test_scenario_ctl_server tests/test_scenario_ctl_server.cpp)
target_link_libraries(test_scenario_ctl_server PRIVATE kairos_scenario_ctl_server)
add_test(NAME test_scenario_ctl_server COMMAND test_scenario_ctl_server)

# Opt-in end-to-end proof: self-skips unless KAIROS_SUPERVISOR_E2E=1 and the sim + C++ bins are
# present, so it stays green in CI (which has no sim binary).
add_executable(test_scenario_supervisor_e2e tests/test_scenario_supervisor_e2e.cpp)
add_test(NAME test_scenario_supervisor_e2e COMMAND test_scenario_supervisor_e2e)

# Crash-restart end-to-end against the real supervisor binary (no sim needed): a crasher trader-bin
# drives backoff -> give-up and stop-during-backoff cancellation.
add_executable(test_scenario_restart_e2e tests/test_scenario_restart_e2e.cpp)
add_dependencies(test_scenario_restart_e2e kairos_scenario_supervisord)
add_test(NAME test_scenario_restart_e2e COMMAND test_scenario_restart_e2e)

add_executable(test_market_fees tests/test_market_fees.cpp)
target_include_directories(test_market_fees PRIVATE ${KAIROS_INC})
add_test(NAME test_market_fees COMMAND test_market_fees)

add_executable(test_pricing tests/test_pricing.cpp)
target_include_directories(test_pricing PRIVATE ${KAIROS_INC})
add_test(NAME test_pricing COMMAND test_pricing)

add_executable(test_order_journal tests/test_order_journal.cpp)
target_link_libraries(test_order_journal PRIVATE kairos_order_journal kairos_scenario pthread)
add_test(NAME test_order_journal COMMAND test_order_journal)

add_executable(test_scenario tests/test_scenario.cpp)
target_link_libraries(test_scenario PRIVATE kairos_scenario)
add_test(NAME test_scenario COMMAND test_scenario)

add_executable(test_blacklist tests/test_blacklist.cpp)
target_link_libraries(test_blacklist PRIVATE kairos_blacklist)
add_test(NAME test_blacklist COMMAND test_blacklist)

add_executable(test_quote_codec tests/test_quote_codec.cpp)
target_link_libraries(test_quote_codec PRIVATE kairos_quote_codec)
add_test(NAME test_quote_codec COMMAND test_quote_codec)

add_executable(test_fill_model tests/test_fill_model.cpp)
target_link_libraries(test_fill_model PRIVATE kairos_sim)
add_test(NAME test_fill_model COMMAND test_fill_model)

add_executable(test_prob_queue tests/test_prob_queue.cpp)
target_link_libraries(test_prob_queue PRIVATE kairos_sim)
add_test(NAME test_prob_queue COMMAND test_prob_queue)

add_executable(test_fill_engine tests/test_fill_engine.cpp)
target_link_libraries(test_fill_engine PRIVATE kairos_sim)
add_test(NAME test_fill_engine COMMAND test_fill_engine)

add_executable(test_auction tests/test_auction.cpp)
target_link_libraries(test_auction PRIVATE kairos_sim)
add_test(NAME test_auction COMMAND test_auction)

add_executable(test_queue_sim_paper tests/test_queue_sim_paper.cpp)
target_link_libraries(test_queue_sim_paper PRIVATE kairos_sim_backend)
add_test(NAME test_queue_sim_paper COMMAND test_queue_sim_paper)

add_executable(test_b8_ordering tests/test_b8_ordering.cpp)
target_link_libraries(test_b8_ordering PRIVATE kairos_sim_backend)
add_test(NAME test_b8_ordering COMMAND test_b8_ordering)

add_executable(test_sim_hub_server tests/test_sim_hub_server.cpp)
target_link_libraries(test_sim_hub_server PRIVATE kairos_sim_backend kairos_order_hub_server
                                                  kairos_order_codec)
add_test(NAME test_sim_hub_server COMMAND test_sim_hub_server)

add_executable(test_sim_faults tests/test_sim_faults.cpp)
target_link_libraries(test_sim_faults PRIVATE kairos_sim_backend)
add_test(NAME test_sim_faults COMMAND test_sim_faults)

# Opt-in fault regression drills against the real sim pipeline + trader: self-skips (exit 0) unless
# KAIROS_FAULT_DRILLS=1, so default CI stays green.
add_executable(test_sim_fault_drills tests/test_sim_fault_drills.cpp)
add_dependencies(test_sim_fault_drills kairos_sim_hubd kairos_scenario_trader)
add_test(NAME test_sim_fault_drills COMMAND test_sim_fault_drills)

add_executable(test_order_codec tests/test_order_codec.cpp)
target_link_libraries(test_order_codec PRIVATE kairos_order_codec)
add_test(NAME test_order_codec COMMAND test_order_codec)

add_executable(test_order_hub tests/test_order_hub.cpp)
target_link_libraries(test_order_hub PRIVATE kairos_order_hub)
add_test(NAME test_order_hub COMMAND test_order_hub)

add_executable(test_hub_status tests/test_hub_status.cpp)
target_link_libraries(test_hub_status PRIVATE kairos_hub_status kairos_order_hub)
add_test(NAME test_hub_status COMMAND test_hub_status)

add_executable(test_hub_risk_gate tests/test_hub_risk_gate.cpp)
target_link_libraries(test_hub_risk_gate PRIVATE kairos_order_hub kairos_order_codec
                                                 kairos_hub_status)
add_test(NAME test_hub_risk_gate COMMAND test_hub_risk_gate)

add_executable(test_order_hub_server tests/test_order_hub_server.cpp)
target_link_libraries(test_order_hub_server PRIVATE kairos_order_hub_server)
add_test(NAME test_order_hub_server COMMAND test_order_hub_server)

add_executable(test_hub_unroutable_e2e tests/test_hub_unroutable_e2e.cpp)
target_link_libraries(test_hub_unroutable_e2e PRIVATE kairos_order_hub_server kairos_order_journal
                                                      kairos_order_codec pthread)
add_test(NAME test_hub_unroutable_e2e COMMAND test_hub_unroutable_e2e)

add_executable(test_hub_shutdown_snapshot tests/test_hub_shutdown_snapshot.cpp)
target_link_libraries(test_hub_shutdown_snapshot PRIVATE kairos_order_hub_server)
add_test(NAME test_hub_shutdown_snapshot COMMAND test_hub_shutdown_snapshot)

add_executable(test_hub_order_backend tests/test_hub_order_backend.cpp)
target_link_libraries(test_hub_order_backend PRIVATE kairos_hub_backend kairos_order_hub_server)
add_test(NAME test_hub_order_backend COMMAND test_hub_order_backend)

add_executable(test_uds tests/test_uds.cpp)
target_link_libraries(test_uds PRIVATE kairos_uds)
add_test(NAME test_uds COMMAND test_uds)

add_executable(test_order_backend tests/test_order_backend.cpp)
target_include_directories(test_order_backend PRIVATE ${KAIROS_INC})
add_test(NAME test_order_backend COMMAND test_order_backend)

add_executable(test_engine_logic tests/test_engine_logic.cpp)
target_include_directories(test_engine_logic PRIVATE ${KAIROS_INC})
add_test(NAME test_engine_logic COMMAND test_engine_logic)

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE kairos_engine kairos_sim_backend)
add_test(NAME test_engine COMMAND test_engine)

add_executable(test_event tests/test_event.cpp)
target_include_directories(test_event PRIVATE ${KAIROS_INC})
add_test(NAME test_event COMMAND test_event)

add_executable(test_transport tests/test_transport.cpp)
target_link_libraries(test_transport PRIVATE kairos_transport)
add_test(NAME test_transport COMMAND test_transport)

add_executable(test_ntfy_dispatcher tests/test_ntfy_dispatcher.cpp)
target_link_libraries(test_ntfy_dispatcher PRIVATE kairos_notify)
add_test(NAME test_ntfy_dispatcher COMMAND test_ntfy_dispatcher)

add_executable(test_dashboard_metrics tests/test_dashboard_metrics.cpp)
target_link_libraries(test_dashboard_metrics PRIVATE kairos_dashboard)
add_test(NAME test_dashboard_metrics COMMAND test_dashboard_metrics)
