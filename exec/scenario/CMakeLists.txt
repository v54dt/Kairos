cmake_minimum_required(VERSION 3.24)
project(kairos_exec CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Pure-logic self-tests (no broker SDK, no network) for the TW market + fee model. The execution
# engine (concords order backend, UDS quote consumer) is added in later phases.
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

add_library(kairos_quote_codec src/quote_codec.cpp ${CAPNP_SRCS})
target_include_directories(kairos_quote_codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                     ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kairos_quote_codec PUBLIC CapnProto::capnp)

add_library(kairos_uds src/uds_quote_client.cpp)
target_include_directories(kairos_uds PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kairos_uds PUBLIC kairos_quote_codec pthread)

add_executable(kairos_quote_tap src/quote_tap_main.cpp)
target_link_libraries(kairos_quote_tap PRIVATE kairos_uds)

add_library(kairos_scenario src/scenario.cpp)
target_include_directories(kairos_scenario PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(kairos_scenario PUBLIC tomlplusplus::tomlplusplus)

enable_testing()
add_executable(test_market_fees tests/test_market_fees.cpp)
target_include_directories(test_market_fees PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME test_market_fees COMMAND test_market_fees)

add_executable(test_pricing tests/test_pricing.cpp)
target_include_directories(test_pricing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME test_pricing COMMAND test_pricing)

add_executable(test_scenario tests/test_scenario.cpp)
target_link_libraries(test_scenario PRIVATE kairos_scenario)
add_test(NAME test_scenario COMMAND test_scenario)

add_executable(test_quote_codec tests/test_quote_codec.cpp)
target_link_libraries(test_quote_codec PRIVATE kairos_quote_codec)
add_test(NAME test_quote_codec COMMAND test_quote_codec)

add_executable(test_uds tests/test_uds.cpp)
target_link_libraries(test_uds PRIVATE kairos_uds)
add_test(NAME test_uds COMMAND test_uds)

add_executable(test_order_backend tests/test_order_backend.cpp)
target_include_directories(test_order_backend PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME test_order_backend COMMAND test_order_backend)
