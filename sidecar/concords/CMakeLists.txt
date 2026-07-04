cmake_minimum_required(VERSION 3.24)
project(kairos_sidecar CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Pre-satisfy libcapnp-dev's libatomic check (false-negative on Ubuntu 24.04).
set(FOUND_LIBATOMIC TRUE)
find_package(CapnProto CONFIG REQUIRED)

set(CAPNPC_SRC_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../../schema")
set(CAPNPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/../../schema/kairos.capnp")

add_library(kairos_quote_encode src/quote_encode.cpp ${CAPNP_SRCS})
target_include_directories(kairos_quote_encode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
                                                      ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(kairos_quote_encode PUBLIC CapnProto::capnp)

enable_testing()
add_executable(test_encode tests/test_encode.cpp)
target_link_libraries(test_encode PRIVATE kairos_quote_encode)
add_test(NAME test_encode COMMAND test_encode)

add_executable(test_decode tests/test_decode.cpp)
target_link_libraries(test_decode PRIVATE kairos_quote_encode)
add_test(NAME test_decode COMMAND test_decode)

add_executable(test_feed_window tests/test_feed_window.cpp)
target_include_directories(test_feed_window PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME test_feed_window COMMAND test_feed_window)

# Cross-language wire-format regression: the pre-A2 fixture still decodes with capnp defaults (C++
# half of Track C5; Rust half is core/tests/golden_wire.rs).
add_executable(test_golden tests/test_golden.cpp)
target_link_libraries(test_golden PRIVATE kairos_quote_encode)
target_compile_definitions(
  test_golden
  PRIVATE
    KAIROS_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../schema/testdata/quote_golden_envelope.bin"
)
add_test(NAME test_golden COMMAND test_golden)

# A2 new wire format pins: the C++ encoder byte-matches the shared v2 fixtures that
# core/tests/golden_wire_v2.rs also decodes and field-checks.
add_executable(test_golden_v2 tests/test_golden_v2.cpp)
target_link_libraries(test_golden_v2 PRIVATE kairos_quote_encode)
target_compile_definitions(
  test_golden_v2
  PRIVATE
    KAIROS_QUOTE_V2_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../schema/testdata/quote_v2_golden_envelope.bin"
)
add_test(NAME test_golden_v2 COMMAND test_golden_v2)

add_executable(test_trade tests/test_trade.cpp)
target_link_libraries(test_trade PRIVATE kairos_quote_encode)
target_compile_definitions(
  test_trade
  PRIVATE
    KAIROS_TRADE_GOLDEN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../schema/testdata/trade_golden_envelope.bin"
)
add_test(NAME test_trade COMMAND test_trade)

add_executable(test_seq_epoch tests/test_seq_epoch.cpp)
target_link_libraries(test_seq_epoch PRIVATE kairos_quote_encode)
add_test(NAME test_seq_epoch COMMAND test_seq_epoch)

# The concords feed executable links the broker SDK + Aeron; built only where the SDK is present in
# concords/lib (CI has none, so it builds just the encode lib + test).
set(CONCORDS_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")
set(CONCORDS_LIB "${CONCORDS_LIB_DIR}/concords_sdk.so")
if(EXISTS "${CONCORDS_LIB}")
  include(FetchContent)
  set(AERON_TESTS
      OFF
      CACHE BOOL "" FORCE)
  set(AERON_SYSTEM_TESTS
      OFF
      CACHE BOOL "" FORCE)
  set(AERON_BUILD_SAMPLES
      OFF
      CACHE BOOL "" FORCE)
  set(AERON_BUILD_DOCUMENTATION
      OFF
      CACHE BOOL "" FORCE)
  set(BUILD_AERON_DRIVER
      OFF
      CACHE BOOL "" FORCE)
  set(BUILD_AERON_ARCHIVE_API
      OFF
      CACHE BOOL "" FORCE)
  FetchContent_Declare(
    aeron
    GIT_REPOSITORY https://github.com/real-logic/aeron.git
    GIT_TAG 1.51.0)
  FetchContent_MakeAvailable(aeron)
  FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0)
  FetchContent_MakeAvailable(tomlplusplus)

  add_executable(kairos_sidecar src/main.cpp src/feed.cpp src/publisher.cpp src/control_sub.cpp)
  target_include_directories(
    kairos_sidecar PRIVATE ${aeron_SOURCE_DIR}/aeron-client/src/main/cpp_wrapper
                           ${CONCORDS_LIB_DIR})
  target_link_libraries(kairos_sidecar PRIVATE kairos_quote_encode aeron_static ${CONCORDS_LIB}
                                               tomlplusplus::tomlplusplus pthread)
  set_target_properties(kairos_sidecar PROPERTIES BUILD_RPATH "${CONCORDS_LIB_DIR}"
                                                  INSTALL_RPATH "${CONCORDS_LIB_DIR}")
else()
  message(STATUS "concords SDK not found at ${CONCORDS_LIB} -- building encode lib + test only")
endif()
