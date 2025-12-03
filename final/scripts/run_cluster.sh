#!/bin/bash

################################################################################
# Project: Distributed Financial Backtesting & Risk Engine
# Authors: Talif Pathan, Mohamed Samir Shafat Khan
# Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
# Date: November 27, 2025
#
# File: run_cluster.sh
#
# Description:
#   This Bash script provides a lightweight local runner for the distributed
#   financial backtesting system. It manages a single controller process and
#   a configurable pool of worker processes on the same host, primarily for:
#
#   - Rapid local development and debugging.
#   - Sanity checks before deploying to a multi-node cluster.
#   - Small-scale functional and performance smoke tests.
#
#   The script exposes a simple CLI:
#     - start  : Launch controller and N worker processes.
#     - stop   : Stop all matching controller and worker processes.
#     - status : Show a concise status summary of running cluster processes.
#
# Core Functionality:
#   - Controller Startup:
#       * Starts ./build/controller with a given port and data directory.
#       * Waits briefly to allow the controller to initialize.
#   - Worker Startup:
#       * Starts NUM_WORKERS instances of ./build/worker.
#       * Each worker connects to the controller host/port and shares a data
#         directory for sample market data.
#   - Cluster Shutdown:
#       * Uses pkill -f pattern matching to terminate controller and worker
#         processes started with matching arguments.
#   - Status Inspection:
#       * Uses ps + grep to list currently running controller and worker
#         processes, printing their PIDs and command line snippets.
#
# Local Architecture (Single-Host Simulation):
#
#   ┌───────────────────────────────────────────────┐
#   │ Local Machine (Development / Testing)        │
#   │                                             │
#   │   Controller (./build/controller)           │
#   │     - Listens on: localhost:${CONTROLLER_PORT:-5000}        │
#   │     - Serves jobs to workers                │
#   │                                             │
#   │   Worker Pool (./build/worker)             │
#   │     - NUM_WORKERS processes (default: 4)   │
#   │     - Connect to controller via TCP        │
#   │     - Read sample data from DATA_DIR       │
#   └───────────────────────────────────────────────┘
#
#   This script abstracts away manual process management, allowing developers
#   to bring up the entire mini-cluster with a single command and tear it down
#   just as quickly.
#
# Usage Workflow:
#
#   1. Build the project:
#        mkdir -p build
#        cd build
#        cmake ..
#        make -j
#
#   2. From the project root:
#        ./scripts/run_cluster.sh start
#
#   3. Check status:
#        ./scripts/run_cluster.sh status
#
#   4. Stop the cluster:
#        ./scripts/run_cluster.sh stop
#
# Dependencies:
#   - Bash (#!/bin/bash)
#   - Standard Unix tools:
#       * ps, grep, awk, pkill, sleep
#   - Compiled binaries:
#       * ./build/controller
#       * ./build/worker
#   - Data directory:
#       * ./data/sample (default, can be overridden in script)
#
# Related Scripts:
#   - deploy_to_khoury.sh : Production-like deployment to multi-node cluster.
#   - fault_test.sh       : Fault injection against controllers/workers.
#   - prepare_submission.sh: Clean project tree for final submission.
#
################################################################################

#===============================================================================
# TABLE OF CONTENTS
#===============================================================================
#
# 1. SCRIPT OVERVIEW AND INTENDED USE
# 2. CONFIGURATION VARIABLES
#    2.1 CONTROLLER_HOST
#    2.2 CONTROLLER_PORT
#    2.3 DATA_DIR
#    2.4 NUM_WORKERS
# 3. FUNCTION BREAKDOWN
#    3.1 start_controller()
#    3.2 start_workers()
#    3.3 stop_cluster()
#    3.4 status_cluster()
# 4. COMMAND DISPATCH (start | stop | status)
# 5. EXAMPLE USAGE
# 6. PREREQUISITES AND DEPENDENCIES
# 7. COMMON PITFALLS AND CAVEATS
# 8. FAQ
# 9. EXTENSIONS AND BEST PRACTICES
#
#===============================================================================

#===============================================================================
# 1. SCRIPT OVERVIEW AND INTENDED USE
#-------------------------------------------------------------------------------
# This script is a convenience wrapper for local development:
#   - It does NOT manage a full Raft controller cluster; it assumes a single
#     controller binary (./build/controller) that exposes the API used by
#     workers.
#   - It is best used on a developer laptop or a single test node to validate
#     behavior before deploying to a real cluster via deploy_to_khoury.sh.
#
# High-level commands:
#   ./run_cluster.sh start   # start controller + workers
#   ./run_cluster.sh stop    # stop all matching processes
#   ./run_cluster.sh status  # show running processes
#
#===============================================================================
# 2. CONFIGURATION VARIABLES
#===============================================================================

# Distributed Backtesting Cluster Runner
# Usage: ./run_cluster.sh [start|stop|status]

#-------------------------------------------------------------------------------
# 2.1 CONTROLLER_HOST
#-------------------------------------------------------------------------------
# Hostname or IP where the controller binary listens. For local development,
# "localhost" is sufficient. If you later split controller and workers across
# machines, you can change this to a specific hostname or IP.
CONTROLLER_HOST="localhost"

#-------------------------------------------------------------------------------
# 2.2 CONTROLLER_PORT
#-------------------------------------------------------------------------------
# TCP port used by the controller for worker communication. This should match
# the port that ./build/controller actually binds to when launched.
CONTROLLER_PORT="5000"

#-------------------------------------------------------------------------------
# 2.3 DATA_DIR
#-------------------------------------------------------------------------------
# Local path to sample data used by both controller and workers. This directory
# should contain the CSV files or other input needed for running backtests.
# Keeping it relative to the project root (./data/sample) makes the script
# portable.
DATA_DIR="./data/sample"

#-------------------------------------------------------------------------------
# 2.4 NUM_WORKERS
#-------------------------------------------------------------------------------
# Number of worker processes to launch on this host. For local testing, keep
# this modest (e.g., 2–8). Large numbers may oversubscribe CPU cores and hurt
# performance or responsiveness.
NUM_WORKERS=4

#===============================================================================
# 3. FUNCTION BREAKDOWN
#===============================================================================

#-------------------------------------------------------------------------------
# 3.1 start_controller()
#-------------------------------------------------------------------------------
# Responsibilities:
#   - Start the controller binary in the background.
#   - Pass the configured port and data directory.
#   - Capture its PID for immediate feedback.
#   - Sleep briefly to give it time to initialize before workers start.
#
# Notes:
#   - The PID is only printed, not persisted. For scripting around this, you
#     might want to extend the script to write the PID to a file.
start_controller() {
    echo "Starting controller..."
    ./build/controller --port $CONTROLLER_PORT --data-dir $DATA_DIR &
    CONTROLLER_PID=$!
    echo "Controller started (PID: $CONTROLLER_PID)"
    # Give the controller a couple of seconds to bind the port and load
    # initial state; this reduces connection errors when workers start.
    sleep 2
}

#-------------------------------------------------------------------------------
# 3.2 start_workers()
#-------------------------------------------------------------------------------
# Responsibilities:
#   - Start NUM_WORKERS worker processes in the background.
#   - Each worker:
#       * Connects to the configured controller host/port.
#       * Uses the shared DATA_DIR for reading sample data.
#   - Print PID for each worker so that developers can inspect or attach
#     debuggers if needed.
#
# Implementation:
#   - Uses a simple for-loop from 1 to NUM_WORKERS.
#   - Sleeps 0.5 seconds between worker launches to avoid thundering herd
#     against the controller on startup.
start_workers() {
    echo "Starting $NUM_WORKERS workers..."
    for i in $(seq 1 $NUM_WORKERS); do
        ./build/worker --controller $CONTROLLER_HOST --port $CONTROLLER_PORT --data-dir $DATA_DIR &
        WORKER_PID=$!
        echo "Worker $i started (PID: $WORKER_PID)"
        sleep 0.5
    done
}

#-------------------------------------------------------------------------------
# 3.3 stop_cluster()
#-------------------------------------------------------------------------------
# Responsibilities:
#   - Attempt to terminate all controller and worker processes started with
#     matching argument patterns.
#
# Implementation Details:
#   - pkill -f "controller --port"
#       * Matches any process whose command line contains the substring
#         "controller --port".
#   - pkill -f "worker --controller"
#       * Matches any process whose command line contains the substring
#         "worker --controller".
#
# Caveats:
#   - pkill -f is pattern-based; if your system has other, unrelated processes
#     containing these substrings, they may be killed as well. See Section 7
#     (Common Pitfalls).
stop_cluster() {
    echo "Stopping cluster..."
    pkill -f "controller --port"
    pkill -f "worker --controller"
    echo "Cluster stopped"
}

#-------------------------------------------------------------------------------
# 3.4 status_cluster()
#-------------------------------------------------------------------------------
# Responsibilities:
#   - Show a concise snapshot of all processes that look like controllers or
#     workers managed by this script.
#
# Implementation Details:
#   - Uses ps aux and grep with a trick:
#       grep "[c]ontroller --port"
#     This avoids matching the grep process itself because the regex does not
#     literally contain "controller".
#   - awk prints:
#       PID and first few command tokens for readability.
#
# This is not a full monitoring solution; it is just a simple status helper
# for developers.
status_cluster() {
    echo "=== Cluster Status ==="
    echo "Controllers:"
    ps aux | grep "[c]ontroller --port" | awk '{print "  PID " $2 ": " $11 " " $12 " " $13}'
    echo "Workers:"
    ps aux | grep "[w]orker --controller" | awk '{print "  PID " $2 ": " $11 " " $12 " " $13}'
}

#===============================================================================
# 4. COMMAND DISPATCH (start | stop | status)
#===============================================================================
# This case statement is the primary user-facing interface:
#   - $1 is the first command-line argument.
#   - Depending on its value, we call the appropriate function(s).
#
# Behavior:
#   - start  : start_controller, then start_workers, then print final message.
#   - stop   : stop_cluster.
#   - status : status_cluster.
#   - other  : print usage and exit with code 1.
case "$1" in
    start)
        start_controller
        start_workers
        echo "Cluster started"
        ;;
    stop)
        stop_cluster
        ;;
    status)
        status_cluster
        ;;
    *)
        echo "Usage: $0 {start|stop|status}"
        exit 1
        ;;
esac

#===============================================================================
# 5. EXAMPLE USAGE
#===============================================================================
#
# Basic Local Run:
#   # From project root
#   ./scripts/run_cluster.sh start
#
#   # Verify that processes are running
#   ./scripts/run_cluster.sh status
#
#   # Run your client, tests, or manual experiments here
#   ./build/client --controller localhost --port 5000
#
#   # When done
#   ./scripts/run_cluster.sh stop
#
# Changing the Number of Workers:
#   - Edit NUM_WORKERS at the top of this script:
#       NUM_WORKERS=8
#   - Then:
#       ./scripts/run_cluster.sh start
#
# Running with a Different Port:
#   - Edit CONTROLLER_PORT:
#       CONTROLLER_PORT="6000"
#   - Ensure your controller and workers support this port and no conflict
#     exists with other services.

#===============================================================================
# 6. PREREQUISITES AND DEPENDENCIES
#===============================================================================
#
# - Binaries:
#     ./build/controller
#     ./build/worker
#
#   These must be compiled and located exactly where referenced, or you’ll see
#   "No such file or directory" errors on start.
#
# - Data directory:
#     ./data/sample
#
#   Ensure this path exists and contains valid input data; otherwise, the
#   controller/workers may start but immediately fail due to missing data.
#
# - Tools:
#     ps, grep, awk, pkill, sleep, seq
#
#   Typically available by default on most Linux distributions.

#===============================================================================
# 7. COMMON PITFALLS AND CAVEATS
#===============================================================================
#
# - pkill Pattern Collisions:
#     pkill -f "controller --port" may kill any process with that substring in
#     its command line. On a shared machine, ensure other processes are not
#     using that pattern. You can tighten the pattern (e.g., include the path
#     "./build/controller") if needed.
#
# - Missing Binaries:
#     If ./build/controller or ./build/worker do not exist or are not
#     executable, the script will fail silently apart from the shell error.
#     Always run "ls ./build" or rebuild if you suspect an issue.
#
# - Hard-Coded Paths:
#     The script assumes it is run from the project root (or with paths
#     adjusted accordingly). If you move it or change the directory layout,
#     update DATA_DIR and the binary paths.
#
# - No Persistent PID Tracking:
#     This script does not store PIDs persistently. It relies on pkill patterns
#     and ps/grep for status. If you need precise lifetime management, consider
#     writing PIDs to files when starting processes.
#
# - Single Controller Only:
#     This runner is not a full Raft cluster launcher; it starts one controller
#     instance. For multi-controller setups, use or extend deploy_to_khoury.sh.

#===============================================================================
# 8. FAQ
#===============================================================================
#
# Q1: Can I run the controller and workers on different machines with this script?
# A1: As written, this script assumes everything runs on the same host. To
#     use multiple machines, you would typically:
#       - Run the controller on one machine manually or via another script.
#       - Modify CONTROLLER_HOST to point to that machine.
#       - Run this script (start) on the worker machines, or write a variant
#         that starts workers only.
#
# Q2: How do I change the number of workers dynamically?
# A2: Currently, NUM_WORKERS is a constant in the script. You can:
#       - Edit the script and set NUM_WORKERS=desired_value, or
#       - Extend the script to accept a flag, e.g.,
#           ./run_cluster.sh start 8
#         and parse $2 to override NUM_WORKERS.
#
# Q3: Why is there a sleep after starting the controller?
# A3: The sleep allows the controller to fully initialize and bind its port
#     before workers attempt to connect. Without this, workers may fail if
#     they attempt to connect too early.
#
# Q4: How do I see logs from controller and workers?
# A4: Log behavior depends on how the binaries are implemented. This script
#     runs them in the background but does not redirect their output. Common
#     patterns:
#       - Logs printed to stdout/stderr (visible via `ps` and `journalctl`).
#       - Logs written to files (e.g., controller.log, worker.log) controlled
#         by the C++ code. Check your implementation and adjust the script
#         to redirect output if desired.
#
# Q5: Can I chain this script with fault_test.sh?
# A5: Yes. A typical workflow:
#       1) ./run_cluster.sh start
#       2) Run some workload.
#       3) ./fault_test.sh --kill-controller=1 --after=30
#       4) ./run_cluster.sh status
#
# Q6: What happens if I rerun "start" while the cluster is already running?
# A6: The script does not check for existing instances. It will attempt to
#     start another controller (likely failing if the port is in use) and more
#     workers. Always run:
#       ./run_cluster.sh status
#     or
#       ./run_cluster.sh stop
#     before starting again.
#
#===============================================================================
# 9. EXTENSIONS AND BEST PRACTICES
#===============================================================================
#
# - Add Logging:
#     Redirect stdout/stderr to log files:
#       ./build/controller ... >> controller.log 2>&1 &
#
# - Add Health Checks:
#     Before starting workers, poll the controller port to ensure it is live
#     (e.g., using nc or curl, depending on your API).
#
# - Parameterize via Environment:
#     Allow overrides:
#       CONTROLLER_PORT=${CONTROLLER_PORT:-5000}
#       NUM_WORKERS=${NUM_WORKERS:-4}
#
# - Integrate with CI:
#     Use this script in CI to spin up a local cluster, run integration tests,
#     then stop the cluster in a teardown phase.
#
# End of run_cluster.sh
#===============================================================================
