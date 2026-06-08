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
set(KAIROS_INC ${SRC}/market ${SRC}/quote ${SRC}/order ${SRC}/notify ${SRC}/engine)

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

# Order hub routing core (socket-agnostic): routes client orders <-> one backend.
add_library(kairos_order_hub src/order/order_hub.cpp)
target_include_directories(kairos_order_hub PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_order_hub PUBLIC kairos_order_codec)

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

add_library(kairos_scenario src/engine/scenario.cpp)
target_include_directories(kairos_scenario PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_scenario PUBLIC tomlplusplus::tomlplusplus)

add_library(kairos_engine src/engine/engine.cpp)
target_include_directories(kairos_engine PUBLIC ${KAIROS_INC})
target_link_libraries(kairos_engine PUBLIC kairos_uds kairos_scenario kairos_dashboard)

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
target_link_libraries(kairos_scenario_trader PRIVATE kairos_engine kairos_hub_backend kairos_notify)

add_executable(kairos_order_hubd src/bin/order_hub_main.cpp)
target_link_libraries(kairos_order_hubd PRIVATE kairos_order_hub_server kairos_live
                                                tomlplusplus::tomlplusplus)

# --- tests ---
enable_testing()
add_executable(test_market_fees tests/test_market_fees.cpp)
target_include_directories(test_market_fees PRIVATE ${KAIROS_INC})
add_test(NAME test_market_fees COMMAND test_market_fees)

add_executable(test_pricing tests/test_pricing.cpp)
target_include_directories(test_pricing PRIVATE ${KAIROS_INC})
add_test(NAME test_pricing COMMAND test_pricing)

add_executable(test_scenario tests/test_scenario.cpp)
target_link_libraries(test_scenario PRIVATE kairos_scenario)
add_test(NAME test_scenario COMMAND test_scenario)

add_executable(test_quote_codec tests/test_quote_codec.cpp)
target_link_libraries(test_quote_codec PRIVATE kairos_quote_codec)
add_test(NAME test_quote_codec COMMAND test_quote_codec)

add_executable(test_order_codec tests/test_order_codec.cpp)
target_link_libraries(test_order_codec PRIVATE kairos_order_codec)
add_test(NAME test_order_codec COMMAND test_order_codec)

add_executable(test_order_hub tests/test_order_hub.cpp)
target_link_libraries(test_order_hub PRIVATE kairos_order_hub)
add_test(NAME test_order_hub COMMAND test_order_hub)

add_executable(test_order_hub_server tests/test_order_hub_server.cpp)
target_link_libraries(test_order_hub_server PRIVATE kairos_order_hub_server)
add_test(NAME test_order_hub_server COMMAND test_order_hub_server)

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
