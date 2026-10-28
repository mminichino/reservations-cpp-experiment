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

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCOUCHBASE REQUIRED libcouchbase)
find_package(CURL REQUIRED)
find_package(fmt REQUIRED)

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
    ${LIBCOUCHBASE_INCLUDE_DIRS}
)

target_compile_options(reservations_core
  PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
)

target_link_libraries(reservations_core
  PUBLIC
    reflectcpp
    CURL::libcurl
    fmt::fmt
    ${LIBCOUCHBASE_LIBRARIES}
)

target_link_directories(reservations_core PUBLIC ${LIBCOUCHBASE_LIBRARY_DIRS})

add_executable(load apps/load/main.cpp)
target_link_libraries(load PRIVATE reservations::core)
target_compile_options(load PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

add_executable(exercise apps/exercise/main.cpp)
target_link_libraries(exercise PRIVATE reservations::core)
target_compile_options(exercise PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
