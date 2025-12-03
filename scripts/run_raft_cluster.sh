#!/bin/bash

################################################################################
# Project: Distributed Financial Backtesting & Risk Engine
# Authors: Talif Pathan, Mohamed Samir Shafat Khan
# Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
# Date: November 27, 2025
#
# File: run_raft_cluster.sh
#
# Description:
# This Bash script launches a complete *local* instance of the distributed
# financial backtesting system on a single machine. It starts a 3-node Raft
# controller cluster and a configurable pool of worker processes, all using
# locally compiled binaries and local data directories.
#
# The script is designed as a lightweight, developer-friendly harness for:
# - Rapid local iteration on Raft logic, worker logic, and protocol changes
# - Smoke testing end-to-end job submission and result aggregation
# - Sanity-checking fault handling (manual process kills, restarts, etc.)
#
# Compared to the cluster deployment script (`deploy_to_khoury.sh`), this
# script:
# - Runs everything on *localhost* (no SSH, no remote copying)
# - Uses a shared data directory on the local filesystem
# - Targets fast feedback cycles during development
#
# Core Functionality:
# - Start a 3-node Raft controller cluster on localhost
# - Start a configurable number of local worker processes
# - Stop all controllers and workers using pattern-based `pkill`
# - Display basic cluster status and recent controller log activity
#
# Local Test Architecture:
#
# Localhost (Single Machine):
#
# ┌─────────────────────────────────────────────────────────────┐
# │ Controller Cluster (3 processes - Raft consensus)          │
# │                                                             │
# │  ID=1: raft_controller --port 5000 --raft-port 6000         │
# │  ID=2: raft_controller --port 5001 --raft-port 6001         │
# │  ID=3: raft_controller --port 5002 --raft-port 6002         │
# │                                                             │
# │ Responsibilities:                                           │
# │ - Raft leader election & log replication                    │
# │ - Job queue & scheduling                                    │
# │ - Worker registration & heartbeats                          │
# │ - Aggregation of backtest results                           │
# └─────────────────────────────────────────────────────────────┘
#                              ↓
# ┌─────────────────────────────────────────────────────────────┐
# │ Worker Pool (N processes - Parallel computation)           │
# │                                                             │
# │  worker 1 .. worker N                                       │
# │   - Connect initially to controller at localhost:5000       │
# │   - Follow redirect to Raft leader if necessary             │
# │   - Load CSV data from ./data/sample                        │
# │   - Run strategies & report results                         │
# └─────────────────────────────────────────────────────────────┘
#
# All processes share:
# - Data directory:       ./data/sample
# - Raft log directory:   ./raft_data
# - Checkpoint directory: ./checkpoints
#
# Local Testing Workflow:
#
# 1. Build the project (from repo root):
#      $ mkdir -p build
#      $ cd build && cmake .. && make
#    Ensure the `raft_controller` and `worker` binaries are placed in ./build.
#
# 2. Prepare sample data:
#      - Place CSV files under ./data/sample
#      - Ensure the path matches the DATA_DIR configured in this script
#
# 3. Start the local Raft cluster:
#      $ ./run_raft_cluster.sh start
#
# 4. Monitor logs:
#      $ tail -f controller1.log
#      $ tail -f worker1.log
#
# 5. Submit jobs (via your client or controller API) and observe behavior.
#
# 6. Stop the cluster:
#      $ ./run_raft_cluster.sh stop
#
# Port Allocation Strategy:
#
# Controller Ports (per process):
# - External API / worker communication:
#   - Controller 1: 5000
#   - Controller 2: 5001
#   - Controller 3: 5002
#
# - Raft consensus ports (internal Raft RPC):
#   - Controller 1: 6000
#   - Controller 2: 6001
#   - Controller 3: 6002
#
# Rationale:
# - Keeping worker/API ports unique (5000–5002) makes it trivial to point a
#   test client at a specific controller if necessary.
# - Using a dedicated Raft port per controller isolates Raft traffic from
#   client/worker traffic and avoids accidental conflicts.
#
# Timing Considerations:
#
# - After starting each controller, the script sleeps briefly to:
#   * Avoid port binding race conditions
#   * Give each node time to initialize before the next joins
#
# - After all controllers are started, the script waits an additional 3 seconds:
#   * Allows Raft to complete leader election
#   * Ensures workers that connect subsequently will see a stable leader
#
# - Workers are started with a small stagger (0.5 seconds):
#   * Reduces thundering-herd behavior on controller startup
#   * Simplifies debugging of connection issues in logs
#
# Error Handling Philosophy:
#
# - This script intentionally does *not* use `set -e` so that:
#   * A failed controller start does not abort the entire script silently
#   * Developers can inspect logs and process lists even in partial failure
#
# - Instead, failure detection is done manually by:
#   * Inspecting controllerN.log / workerN.log
#   * Running `./run_raft_cluster.sh status`
#
# Log Files:
#
# - Controllers:
#     controller1.log  → Controller ID 1
#     controller2.log  → Controller ID 2
#     controller3.log  → Controller ID 3
#
# - Workers:
#     worker1.log      → First worker process
#     worker2.log      → Second worker process
#     ...
#
# - Log files are appended to across runs. For a clean test, delete them:
#     $ rm controller*.log worker*.log
#
# Dependencies:
#
# - Bash 4.0+ (tested with modern Bash)
# - `ps`, `grep`, `awk`, `tail`, `pkill`
# - Compiled binaries:
#     - ./build/raft_controller
#     - ./build/worker
# - Local data directory:
#     - ./data/sample (contains CSV market data)
#
# Related Scripts:
#
# - deploy_to_khoury.sh:
#     Full multi-node deployment to Khoury Linux cluster.
#
# - stop_khoury_cluster.sh:
#     Graceful stop for the remote cluster deployment.
#
# - run_raft_cluster.sh (this file):
#     Local development/testing harness.
#
################################################################################

#===============================================================================
# TABLE OF CONTENTS
#===============================================================================
#
# 1. SCRIPT PURPOSE & CONTEXT
#    - See header "Description" and "Local Test Architecture"
#
# 2. USAGE SUMMARY
#    - ./run_raft_cluster.sh start
#    - ./run_raft_cluster.sh stop
#    - ./run_raft_cluster.sh status
#
# 3. CONFIGURATION
#    3.1 Data, Raft, and Checkpoint Directories
#    3.2 Controller IDs, Ports, and Peer Topology
#    3.3 Worker Count and Default Controller Host
#
# 4. FUNCTION-BY-FUNCTION WALKTHROUGH
#    4.1 start_controllers()
#    4.2 start_workers()
#    4.3 stop_cluster()
#    4.4 status_cluster()
#
# 5. CONTROL FLOW (ARGUMENT DISPATCH)
#
# 6. EXAMPLE WORKFLOWS
#
# 7. COMMON PITFALLS & TROUBLESHOOTING
#
# 8. FAQ
#
# 9. EXTENSION POINTS & BEST PRACTICES
#
#===============================================================================

#===============================================================================
# 3. CONFIGURATION
#===============================================================================
#
# The following variables define local directories, controller IDs/ports,
# Raft topology, and worker behavior. Adjust them carefully if needed.
#

#------------------------------------------------------------------------------
# 3.1 Data, Raft, and Checkpoint Directories
#------------------------------------------------------------------------------

# Directory containing sample market data (CSV files).
# For local testing, this is typically a small subset of real data.
DATA_DIR="./data/sample"

# Directory where Raft controllers will store their logs and snapshots.
# All controllers share this base directory, but should use unique subpaths
# internally (handled by the C++ implementation).
RAFT_DIR="./raft_data"

# Directory where workers will persist progress so they can resume from
# checkpoints after failures or restarts.
CHECKPOINT_DIR="./checkpoints"

# Number of worker processes to launch on localhost by default.
# Adjust this based on your machine's CPU core count and workload size.
NUM_WORKERS=4

#------------------------------------------------------------------------------
# 3.2 Controller IDs, Ports, and Peer Topology
#------------------------------------------------------------------------------
#
# Each controller has:
# - A unique numeric ID
# - A unique API/worker port (CONTROLLER_X_PORT)
# - A unique Raft consensus port (CONTROLLER_X_RAFT_PORT)
#
# The `--peers` argument encodes all *other* controllers in the cluster in
# the format:
#     "<peerId>:<host>:<apiPort>:<raftPort>,<peerId2>:<host2>:<apiPort2>:<raftPort2>"

# Controller configurations
CONTROLLER_1_ID=1
CONTROLLER_1_PORT=5000
CONTROLLER_1_RAFT_PORT=6000

CONTROLLER_2_ID=2
CONTROLLER_2_PORT=5001
CONTROLLER_2_RAFT_PORT=6001

CONTROLLER_3_ID=3
CONTROLLER_3_PORT=5002
CONTROLLER_3_RAFT_PORT=6002

# For localhost testing, all controllers run on the same machine.
# In a distributed setting, this would be replaced with distinct hostnames.
CONTROLLER_HOST="localhost"

# Build peer strings (each controller sees the other two as peers).
# These must match the Raft configuration expected by the C++ controller code.
PEERS_FOR_1="2:localhost:5001:6001,3:localhost:5002:6002"
PEERS_FOR_2="1:localhost:5000:6000,3:localhost:5002:6002"
PEERS_FOR_3="1:localhost:5000:6000,2:localhost:5001:6001"

#===============================================================================
# 4. FUNCTION-BY-FUNCTION WALKTHROUGH
#===============================================================================

#------------------------------------------------------------------------------
# 4.1 start_controllers()
#------------------------------------------------------------------------------
#
# Responsibilities:
# - Ensure required directories exist
# - Start all 3 Raft controller processes
# - Wire each controller with the correct peer topology
# - Stagger startup to avoid port races
# - Give Raft time to elect a leader
#
# Notes:
# - Each controller is started in the background and its stdout/stderr is
#   redirected to a dedicated controllerN.log file.
# - `$!` is used immediately after each background start to print the PID.
# - This function does *not* validate that the binary exists; if it is
#   missing or not executable, the failure will appear in the logs.
#
start_controllers() {
    echo "Starting 3-node Raft controller cluster..."
    
    # Create required directories if they do not already exist.
    # This avoids failures when controllers attempt to write data/checkpoints.
    mkdir -p "$DATA_DIR" "$RAFT_DIR" "$CHECKPOINT_DIR"
    
    #-------------------------------
    # Controller 1
    #-------------------------------
    ./build/raft_controller --id "$CONTROLLER_1_ID" \
        --port "$CONTROLLER_1_PORT" \
        --raft-port "$CONTROLLER_1_RAFT_PORT" \
        --data-dir "$DATA_DIR" \
        --raft-dir "$RAFT_DIR" \
        --peers "$PEERS_FOR_1" > controller1.log 2>&1 &
    echo "Controller 1 started (PID: $!)"
    # Short delay to allow initialization before starting the next node.
    sleep 1
    
    #-------------------------------
    # Controller 2
    #-------------------------------
    ./build/raft_controller --id "$CONTROLLER_2_ID" \
        --port "$CONTROLLER_2_PORT" \
        --raft-port "$CONTROLLER_2_RAFT_PORT" \
        --data-dir "$DATA_DIR" \
        --raft-dir "$RAFT_DIR" \
        --peers "$PEERS_FOR_2" > controller2.log 2>&1 &
    echo "Controller 2 started (PID: $!)"
    sleep 1
    
    #-------------------------------
    # Controller 3
    #-------------------------------
    ./build/raft_controller --id "$CONTROLLER_3_ID" \
        --port "$CONTROLLER_3_PORT" \
        --raft-port "$CONTROLLER_3_RAFT_PORT" \
        --data-dir "$DATA_DIR" \
        --raft-dir "$RAFT_DIR" \
        --peers "$PEERS_FOR_3" > controller3.log 2>&1 &
    echo "Controller 3 started (PID: $!)"
    sleep 2
    
    echo "Waiting for leader election..."
    # Give Raft some breathing room to perform term negotiation and elect a leader.
    sleep 3
}

#------------------------------------------------------------------------------
# 4.2 start_workers()
#------------------------------------------------------------------------------
#
# Responsibilities:
# - Launch NUM_WORKERS worker processes on localhost
# - Point each worker at the first controller's public port
# - Stagger worker startup slightly to reduce connection storms
#
# Notes:
# - The C++ worker binary is expected to handle:
#   * Initial contact with controller at CONTROLLER_1_PORT
#   * Redirect to the current leader if the contacted node is not leader
#   * Internal retry/backoff behavior on temporary failures
#
start_workers() {
    echo "Starting $NUM_WORKERS workers..."
    
    # Connect workers to first controller (which will redirect to leader if
    # needed, based on your Raft/controller implementation).
    for i in $(seq 1 "$NUM_WORKERS"); do
        ./build/worker --controller "$CONTROLLER_HOST" \
            --port "$CONTROLLER_1_PORT" \
            --data-dir "$DATA_DIR" > "worker$i.log" 2>&1 &
        echo "Worker $i started (PID: $!)"
        # Small delay avoids overwhelming the controller with simultaneous connects.
        sleep 0.5
    done
}

#------------------------------------------------------------------------------
# 4.3 stop_cluster()
#------------------------------------------------------------------------------
#
# Responsibilities:
# - Terminate all running controller and worker processes started by this script
#
# Implementation Details:
# - Uses `pkill -f` with patterns that match the command-line invocation:
#     "raft_controller --id"
#     "worker --controller"
#
# Caution:
# - `pkill -f` matches against the full command line. If you happen to have
#   other processes on the same host that contain these substrings, they may
#   be terminated as well.
# - On a dedicated dev box this is usually acceptable; in a shared environment,
#   consider tightening the patterns or using PID files instead.
#
stop_cluster() {
    echo "Stopping Raft cluster..."
    pkill -f "raft_controller --id"
    pkill -f "worker --controller"
    echo "Cluster stopped"
}

#------------------------------------------------------------------------------
# 4.4 status_cluster()
#------------------------------------------------------------------------------
#
# Responsibilities:
# - Display a quick snapshot of:
#   * Running controller processes (with IDs inferred from arguments)
#   * Running worker processes
#   * Tail of each controller log (last 3 lines for quick debugging)
#
# Notes:
# - The controller ID is pulled out of the `ps` line via `awk '{print "  Node " $14 " PID " $2}'`
#   which assumes a specific argument ordering. If you change controller
#   invocation flags significantly, adjust the field index accordingly.
# - For more detailed troubleshooting, you should open logs directly.
#
status_cluster() {
    echo "=== Raft Cluster Status ==="
    echo "Controllers:"
    # The [r] pattern trick prevents grep from matching its own process.
    ps aux | grep "[r]aft_controller --id" | awk '{print "  Node " $14 " PID " $2}'
    echo "Workers:"
    ps aux | grep "[w]orker --controller" | awk '{print "  PID " $2}'
    echo ""
    echo "Recent controller logs:"
    echo "Controller 1 (last 3 lines):"
    tail -n 3 controller1.log 2>/dev/null || echo "  No log file"
    echo "Controller 2 (last 3 lines):"
    tail -n 3 controller2.log 2>/dev/null || echo "  No log file"
    echo "Controller 3 (last 3 lines):"
    tail -n 3 controller3.log 2>/dev/null || echo "  No log file"
}

#===============================================================================
# 5. CONTROL FLOW (ARGUMENT DISPATCH)
#===============================================================================
#
# The main entry point interprets the first CLI argument and dispatches:
#   start  → start_controllers + start_workers
#   stop   → stop_cluster
#   status → status_cluster
#
# Any other value (or missing argument) triggers a usage message and exit code 1.
#

case "$1" in
    start)
        start_controllers
        start_workers
        echo ""
        echo "=== Raft Cluster Started ==="
        echo "Controllers listening on ports: 5000, 5001, 5002"
        echo "Raft communication on ports: 6000, 6001, 6002"
        echo "Workers: $NUM_WORKERS"
        echo ""
        echo "Check logs:"
        echo "  tail -f controller1.log"
        echo "  tail -f worker1.log"
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
# 6. EXAMPLE WORKFLOWS
#===============================================================================
#
# 6.1 Fresh Local Test Run
# ------------------------
# $ rm -f controller*.log worker*.log
# $ ./run_raft_cluster.sh start
#   (wait for logs to show a leader and worker registrations)
# $ ./run_raft_cluster.sh status
#   (verify controllers and workers are running)
#
# Submit a backtest job using your client, then:
#
# $ tail -f controller1.log
# $ tail -f worker1.log
#
# When done:
# $ ./run_raft_cluster.sh stop
#
# 6.2 Changing the Number of Workers
# ----------------------------------
# - Edit NUM_WORKERS at the top of this script:
#     NUM_WORKERS=8
#
# - Restart cluster:
#     $ ./run_raft_cluster.sh stop
#     $ ./run_raft_cluster.sh start
#
# 6.3 Simulating Failures
# -----------------------
# - Kill a worker manually:
#     $ pkill -f "worker --controller"  # or kill a specific PID from status
#
# - Observe controller logs for:
#     - Heartbeat timeouts
#     - Task reassignment or checkpoint-based recovery
#
# - Kill the leader controller and verify a new leader is elected:
#     $ ./run_raft_cluster.sh status  # identify leader via logs
#     $ kill <leader_pid>
#     (watch controller logs for new leader election)
#
#===============================================================================
# 7. COMMON PITFALLS & TROUBLESHOOTING
#===============================================================================
#
# Pitfall 1: Binaries Not Found
# -----------------------------
# Symptom:
# - Log files are empty or contain "No such file or directory".
#
# Check:
# - Ensure ./build/raft_controller and ./build/worker exist and are executable:
#     $ ls -l ./build/raft_controller ./build/worker
#
# Pitfall 2: Port Already in Use
# ------------------------------
# Symptom:
# - Controllers fail to start; logs show bind errors for ports 5000–5002 or
#   6000–6002.
#
# Check:
# - Look for stale processes:
#     $ ./run_raft_cluster.sh stop
#     $ ps aux | grep raft_controller
#
# - Or find the process holding the port (Linux):
#     $ sudo lsof -i:5000
#
# Pitfall 3: `pkill` Stopping Unrelated Processes
# -----------------------------------------------
# Symptom:
# - Unexpected processes are terminated when running `stop`.
#
# Explanation:
# - `pkill -f` matches by substring on the *entire* command line.
#
# Mitigation:
# - Use this on a dedicated dev environment.
# - Or tighten patterns (e.g., include absolute path) if you share host:
#     pkill -f "$(pwd)/build/raft_controller --id"
#
# Pitfall 4: Workers Fail to Connect
# ----------------------------------
# Symptom:
# - workerN.log shows repeated connection failures to controller.
#
# Check:
# - Ensure Controller 1 is running and listening on CONTROLLER_1_PORT (5000).
# - Ensure no firewall rules are blocking local connections (rare on localhost).
#
# Pitfall 5: Stale Raft or Checkpoint Data
# ----------------------------------------
# Symptom:
# - Unexpected behavior after code changes (old state influencing new logic).
#
# Mitigation:
# - For a clean run, wipe state:
#     $ rm -rf ./raft_data ./checkpoints
#     $ mkdir -p ./raft_data ./checkpoints
#
#===============================================================================
# 8. FAQ
#===============================================================================
#
# Q1: Does this script rebuild the C++ project for me?
# ----------------------------------------------------
# A: No. This script assumes you have already built the binaries and placed
#    them in ./build. Keeping build logic separate makes this script simpler
#    and avoids hidden side effects.
#
# Q2: Can I run more than 3 controllers locally?
# ----------------------------------------------
# A: The Raft implementation in your project is designed for 3 controllers,
#    which is the minimal odd number for fault tolerance (can tolerate 1
#    controller failure). You *could* extend this script to 5 controllers,
#    but then you must:
#    - Update peer strings accordingly
#    - Ensure Raft implementation supports dynamic cluster sizes
#
# Q3: How do workers find the current leader?
# -------------------------------------------
# A: Workers initially connect to CONTROLLER_1_PORT (5000). If that controller
#    is not the leader, it should respond with a redirect or leader hint as
#    defined by your protocol. The worker then reconnects to the proper leader.
#
# Q4: Why do controllers have different API ports (5000, 5001, 5002)?
# -------------------------------------------------------------------
# A: This makes it straightforward to:
#    - Direct a client at a specific controller during debugging
#    - Run all nodes on a single host without port collisions
#
# Q5: Can I change the data directory?
# ------------------------------------
# A: Yes. Update DATA_DIR at the top of this script and ensure that:
#    - The directory exists
#    - It contains the CSV files expected by the project
#
# Q6: Is it safe to run this script on the Khoury cluster?
# --------------------------------------------------------
# A: Technically yes (it only uses local processes), but the intent is to use
#    this for local development on your own machine. For cluster-scale tests,
#    use the dedicated deployment script (`deploy_to_khoury.sh`).
#
#===============================================================================
# 9. EXTENSION POINTS & BEST PRACTICES
#===============================================================================
#
# - PID Files:
#   For more precise process control, consider writing PIDs to files instead of
#   relying on pattern-based `pkill`. This becomes especially important on
#   shared machines.
#
# - Environment Profiles:
#   You can wrap this script with simple env-specific wrappers (dev, perf-test)
#   that:
#     - Adjust NUM_WORKERS
#     - Change DATA_DIR to different datasets
#
# - Health Checks:
#   Add simple curl-based or custom client-based checks after startup to verify:
#     - Leader is elected
#     - Basic API endpoints respond
#
# - Log Rotation:
#   For long-running tests, consider adding log rotation or truncation logic so
#   controllerN.log and workerN.log do not grow without bound.
#
# - Automation:
#   This script plays well with CI pipelines: you can:
#     - Start the cluster in a job stage
#     - Run integration tests
#     - Stop the cluster in a teardown step
#
# End of run_raft_cluster.sh
#===============================================================================
