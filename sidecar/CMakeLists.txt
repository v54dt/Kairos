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

set(CAPNPC_SRC_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/../schema")
set(CAPNPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS "${CMAKE_CURRENT_SOURCE_DIR}/../schema/kairos.capnp")

add_library(kairos_quote_encode src/quote_encode.cpp ${CAPNP_SRCS})
target_include_directories(kairos_quote_encode PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(kairos_quote_encode PUBLIC CapnProto::capnp)

enable_testing()
add_executable(test_encode tests/test_encode.cpp)
target_link_libraries(test_encode PRIVATE kairos_quote_encode)
add_test(NAME test_encode COMMAND test_encode)
