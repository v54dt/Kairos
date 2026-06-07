cmake_minimum_required(VERSION 3.24)
project(kairos_exec CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Pure-logic self-tests (no broker SDK, no network) for the TW market + fee model. The execution
# engine (concords order backend, UDS quote consumer) is added in later phases.
enable_testing()
add_executable(test_market_fees tests/test_market_fees.cpp)
target_include_directories(test_market_fees PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME test_market_fees COMMAND test_market_fees)

add_executable(test_pricing tests/test_pricing.cpp)
target_include_directories(test_pricing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME test_pricing COMMAND test_pricing)
