cmake_minimum_required(VERSION 3.25)
project(reservations_cpp_experiment LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

set(REFLECTCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(REFLECTCPP_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  reflectcpp
  GIT_REPOSITORY https://github.com/munich-data-engineering/reflect-cpp.git
  GIT_TAG v0.26.2
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(reflectcpp)

find_package(couchbase_cxx_client QUIET)
if(NOT couchbase_cxx_client_FOUND)
  message(STATUS "couchbase_cxx_client not found via find_package; fetching via CPM")
  file(DOWNLOAD
    https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.40.8/CPM.cmake
    ${CMAKE_BINARY_DIR}/cmake/CPM.cmake
    EXPECTED_HASH SHA256=78ba32abdf798bc616bab7c73aac32a17bbd7b06ad9e26a6add69de8f3ae4791
  )
  include(${CMAKE_BINARY_DIR}/cmake/CPM.cmake)
  CPMAddPackage(
    NAME couchbase_cxx_client
    GIT_TAG 1.5.1
    VERSION 1.5.1
    GITHUB_REPOSITORY "couchbase/couchbase-cxx-client"
    OPTIONS
      "COUCHBASE_CXX_CLIENT_STATIC_BORINGSSL ON"
      "COUCHBASE_CXX_CLIENT_BUILD_STATIC ON"
      "COUCHBASE_CXX_CLIENT_BUILD_SHARED OFF"
      "COUCHBASE_CXX_CLIENT_BUILD_TESTS OFF"
      "COUCHBASE_CXX_CLIENT_BUILD_EXAMPLES OFF"
      "COUCHBASE_CXX_CLIENT_BUILD_TOOLS OFF"
  )
endif()

find_package(CURL REQUIRED)
find_package(fmt REQUIRED)

if(TARGET couchbase_cxx_client::couchbase_cxx_client)
  set(RESERVATIONS_COUCHBASE_TARGET couchbase_cxx_client::couchbase_cxx_client)
elseif(TARGET couchbase_cxx_client::couchbase_cxx_client_static)
  set(RESERVATIONS_COUCHBASE_TARGET couchbase_cxx_client::couchbase_cxx_client_static)
else()
  message(FATAL_ERROR "Couchbase C++ SDK CMake target not found")
endif()

add_library(reservations_core
  src/platform/couchbase_client.cpp
  src/seatmap/service.cpp
  src/seatmap/synthetic.cpp
  src/seatmap/seed.cpp
)
add_library(reservations::core ALIAS reservations_core)

target_include_directories(reservations_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(reservations_core
  PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

target_link_libraries(reservations_core
  PUBLIC
    reflectcpp
    ${RESERVATIONS_COUCHBASE_TARGET}
    CURL::libcurl
    fmt::fmt
)

add_executable(load apps/load/main.cpp)
target_link_libraries(load PRIVATE reservations::core)
target_compile_options(load PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(exercise apps/exercise/main.cpp)
target_link_libraries(exercise PRIVATE reservations::core)
target_compile_options(exercise PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
