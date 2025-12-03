cmake_minimum_required(VERSION 3.15)
project(DistributedBacktesting VERSION 2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -O3")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -DDEBUG")

# Find required packages
find_package(Threads REQUIRED)

# Include directories
include_directories(${PROJECT_SOURCE_DIR}/src)

# Common library
add_library(common STATIC
    src/common/message.cpp
    src/common/logger.cpp
)
target_link_libraries(common Threads::Threads)

# Data library
add_library(data STATIC
    src/data/csv_loader.cpp
)
target_link_libraries(data common)

# Strategy library
add_library(strategy STATIC
    src/strategy/sma_strategy.cpp
    src/strategy/strategy_with_checkpoint.cpp
)
target_link_libraries(strategy common data checkpoint)

# Raft library
add_library(raft STATIC
    src/raft/raft_node.cpp
    src/raft/raft_log.cpp
)
target_link_libraries(raft common)

# Checkpoint library
add_library(checkpoint STATIC
    src/worker/checkpoint_manager.cpp
)
target_link_libraries(checkpoint common)

# Controller library (original single-node)
add_library(controller_lib STATIC
    src/controller/controller.cpp
)
target_link_libraries(controller_lib common strategy data)

# Raft-enabled controller library
add_library(raft_controller_lib STATIC
    src/controller/raft_controller.cpp
)
target_link_libraries(raft_controller_lib controller_lib raft)

# Worker library
add_library(worker_lib STATIC
    src/worker/worker.cpp
)
target_link_libraries(worker_lib common strategy data checkpoint)

# Controller executable (single-node)
add_executable(controller src/main_controller.cpp)
target_link_libraries(controller controller_lib)

# Raft Controller executable (3-node cluster)
add_executable(raft_controller src/main_raft_controller.cpp)
target_link_libraries(raft_controller raft_controller_lib)

# Worker executable
add_executable(worker src/main_worker.cpp)
target_link_libraries(worker worker_lib)

# Job submission client
add_executable(submit_jobs src/submit_jobs.cpp)
target_link_libraries(submit_jobs controller_lib)

# Benchmark client
add_executable(benchmark_client src/benchmark_client.cpp)
target_link_libraries(benchmark_client controller_lib)

# Evaluation client (if exists)
if(EXISTS "${PROJECT_SOURCE_DIR}/src/evaluation_client.cpp")
    add_executable(evaluation_client src/evaluation_client.cpp)
    target_link_libraries(evaluation_client common)
endif()

# Tests
enable_testing()

add_executable(test_csv_loader tests/test_csv_loader.cpp)
target_link_libraries(test_csv_loader data)
add_test(NAME TestCSVLoader COMMAND test_csv_loader)

add_executable(test_sma_strategy tests/test_sma_strategy.cpp)
target_link_libraries(test_sma_strategy strategy)
add_test(NAME TestSMAStrategy COMMAND test_sma_strategy)

add_executable(test_integration tests/test_integration.cpp)
target_link_libraries(test_integration controller_lib worker_lib)
add_test(NAME TestIntegration COMMAND test_integration)

add_executable(test_raft tests/test_raft.cpp)
target_link_libraries(test_raft raft)
add_test(NAME TestRaft COMMAND test_raft)

add_executable(test_checkpoint tests/test_checkpoint.cpp)
target_link_libraries(test_checkpoint checkpoint)
add_test(NAME TestCheckpoint COMMAND test_checkpoint)

add_executable(test_raft_integration tests/test_raft_integration.cpp)
target_link_libraries(test_raft_integration raft_controller_lib worker_lib)
add_test(NAME TestRaftIntegration COMMAND test_raft_integration)

add_executable(test_full_system tests/test_full_system.cpp)
target_link_libraries(test_full_system raft_controller_lib worker_lib)
add_test(NAME TestFullSystem COMMAND test_full_system)

# Installation
install(TARGETS controller raft_controller worker DESTINATION bin)

# Print build info
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")