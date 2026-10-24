cmake_minimum_required(VERSION 3.16)

project(LowLatencyOrderBook VERSION 2.0.0 LANGUAGES CXX)

option(ORDERBOOK_BUILD_DEMO "Build the orderbook_demo executable" ON)
option(ORDERBOOK_BUILD_TESTS "Build the Catch2 unit tests" ON)
option(ORDERBOOK_BUILD_BENCHMARKS "Build the orderbook_bench benchmark" ON)
option(ORDERBOOK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(ORDERBOOK_SANITIZE "" CACHE STRING
    "Sanitizers for all targets: empty, 'address,undefined' or 'thread'")

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# Common warning / sanitizer settings, applied per target.
add_library(orderbook_options INTERFACE)
if(MSVC)
  target_compile_options(orderbook_options INTERFACE /W4)
  if(ORDERBOOK_WARNINGS_AS_ERRORS)
    target_compile_options(orderbook_options INTERFACE /WX)
  endif()
else()
  target_compile_options(orderbook_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
  if(ORDERBOOK_WARNINGS_AS_ERRORS)
    target_compile_options(orderbook_options INTERFACE -Werror)
  endif()
  if(ORDERBOOK_SANITIZE)
    target_compile_options(orderbook_options INTERFACE
      -fsanitize=${ORDERBOOK_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(orderbook_options INTERFACE -fsanitize=${ORDERBOOK_SANITIZE})
  endif()
endif()

# ---- Library ---------------------------------------------------------------
add_library(orderbook
  src/OrderBook.cpp
  src/StopOrderScheduler.cpp
  src/MatchingEngine.cpp
  src/AsyncMatchingEngine.cpp)
add_library(orderbook::orderbook ALIAS orderbook)
target_include_directories(orderbook PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(orderbook PUBLIC cxx_std_17)
target_link_libraries(orderbook PUBLIC Threads::Threads PRIVATE orderbook_options)

# ---- Demo executable ---------------------------------------------------------
if(ORDERBOOK_BUILD_DEMO)
  add_executable(orderbook_demo apps/main.cpp)
  target_link_libraries(orderbook_demo PRIVATE orderbook orderbook_options)
endif()

# ---- Tests ---------------------------------------------------------------------
if(ORDERBOOK_BUILD_TESTS)
  enable_testing()
  add_executable(orderbook_tests
    tests/test_main.cpp
    tests/test_price_conversion.cpp
    tests/test_order_book.cpp
    tests/test_stop_orders.cpp
    tests/test_randomized.cpp
    tests/test_concurrency.cpp)
  target_include_directories(orderbook_tests SYSTEM PRIVATE third_party/catch2)
  target_include_directories(orderbook_tests PRIVATE tests)
  target_link_libraries(orderbook_tests PRIVATE orderbook orderbook_options)
  add_test(NAME orderbook_tests COMMAND orderbook_tests)
endif()

# ---- Benchmarks ----------------------------------------------------------------
if(ORDERBOOK_BUILD_BENCHMARKS)
  add_executable(orderbook_bench bench/bench_orderbook.cpp)
  target_link_libraries(orderbook_bench PRIVATE orderbook orderbook_options)
endif()
