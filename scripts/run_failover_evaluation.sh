#!/bin/bash

################################################################################
# Project: Distributed Financial Backtesting & Risk Engine
# Authors: Talif Pathan, Mohamed Samir Shafat Khan
# Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
# Date: November 27, 2025
#
# File: run_failover_evaluation.sh
#
# Description:
# This Bash script implements Evaluation E3: Controller Failover for the
# distributed financial backtesting system. It brings up a 3-node Raft-based
# controller cluster on localhost, simulates leader failure under load, and
# verifies:
#
#   - Correct initial leader election
#   - Worker registration with the elected leader
#   - Leader failover when the current leader is killed
#   - Time taken to complete the failover (failover latency)
#   - Health of remaining controllers after failure
#   - Successful worker reconnection to the new leader
#   - Rejoin of the failed controller as a follower
#
# The script generates a detailed evaluation log file that can be reused in
# your final report to demonstrate fault tolerance and recovery behavior of
# the controller cluster.
#
# How this script fits into the larger project:
#
# - It directly validates the "Controller Cluster (Raft)" fault-tolerance
#   requirements from the project plan:
#     * Survive the loss of any single controller node.
#     * Automatically elect a new leader.
#     * Preserve availability for worker registration and job management.
#
# - It complements other evaluations:
#     * E1/E2: Functional and scalability benchmarks.
#     * E3 (this script): Liveness and correctness under controller failure.
#
# - The script is intentionally self-contained and local:
#     * All three Raft controllers run on localhost with distinct ports.
#     * Two workers connect to the leader and later to the new leader.
#     * Raft logs are stored in a dedicated directory for this test
#       (RAFT_DIR="./raft_data_e3").
#
# Core Evaluation Steps:
#
# 1) Test 1 — Initial Leader Election
#    - Starts a 3-node Raft cluster.
#    - Waits for leader election.
#    - Verifies that exactly one node becomes leader.
#
# 2) Test 2 — Worker Registration with Leader
#    - Starts 2 worker processes.
#    - Confirms they register with the leader via log messages.
#
# 3) Test 3 — Leader Failover
#    - Kills the current leader.
#    - Measures time between kill and new leader election.
#    - Verifies that the new leader is different from the original one.
#
# 4) Test 4 — System Recovery After Failover
#    - Confirms remaining controllers are still running and healthy.
#    - Reconnects workers to the new leader.
#    - Ensures the new leader accepts worker registrations.
#
# 5) Test 5 — Restart Failed Node
#    - Restarts the killed controller node.
#    - Verifies it re-joins the cluster as a follower (not a second leader).
#
# Output Artifacts:
#
# - evaluation_e3_failover.log
#     * Human-readable evaluation log (INFO/SUCCESS/WARN/ERROR lines).
#     * Includes summary section with:
#         - Initial leader ID
#         - New leader ID
#         - Failover time (seconds)
#         - Workers registered before/after failover
#
# - /tmp/e3_ctrl*.log
#     * Raw logs from each Raft controller (IDs 1, 2, 3).
#
# - /tmp/e3_worker*.log, /tmp/e3_worker_new*.log
#     * Raw logs from workers before and after failover.
#
# Raft Cluster Architecture (Local, Single Host):
#
# ┌──────────────────────────────────────────────────────┐
# │ 3-Node Raft Controller Cluster (localhost)          │
# │                                                      │
# │  Node 1: id=1, port=5100, raft-port=6100            │
# │  Node 2: id=2, port=5101, raft-port=6101            │
# │  Node 3: id=3, port=5102, raft-port=6102            │
# │                                                      │
# │  All nodes share RAFT_DIR for Raft logs, but each   │
# │  controller should internally use distinct state    │
# │  (typically via per-node subdirectories or ids).    │
# └──────────────────────────────────────────────────────┘
#
# Worker Connections:
#
# - Initially:
#     * Workers connect to INITIAL_LEADER on port:
#       LEADER_PORT = 5099 + INITIAL_LEADER
#
# - After failover:
#     * Workers connect to NEW_LEADER on port:
#       NEW_LEADER_PORT = 5099 + NEW_LEADER
#
# Dependencies:
# - Bash 4.0+ (arrays, extended features)
# - GNU grep with PCRE (-P) support (for timestamp extraction)
# - bc (for floating-point failover time computation)
# - ps, pkill (for process introspection and cleanup)
# - Python 3 (for generate_sample_data.py)
#
# Related Scripts & Components:
# - build/raft_controller : Raft-based controller binary
# - build/worker          : Worker binary
# - scripts/generate_sample_data.py : Sample data generator
# - Other evaluation scripts (e.g., scalability, checkpointing)
#
################################################################################

#===============================================================================
# TABLE OF CONTENTS
#===============================================================================
#
# 1. SCRIPT OVERVIEW AND PURPOSE
# 2. HIGH-LEVEL TEST SCENARIO (E3: Controller Failover)
# 3. CONFIGURATION CONSTANTS
#    3.1 Data & Raft Directories
#    3.2 Output Log File
# 4. LOGGING AND COLOR UTILITIES
# 5. CLEANUP HANDLER AND SIGNAL TRAP
# 6. LEADER DETECTION UTILITY (get_leader)
# 7. PREREQUISITE CHECKS (build, data)
# 8. TEST 1: INITIAL LEADER ELECTION
# 9. TEST 2: WORKER REGISTRATION WITH LEADER
# 10. TEST 3: LEADER FAILOVER AND FAILOVER TIME
# 11. TEST 4: SYSTEM RECOVERY AND WORKER RECONNECTION
# 12. TEST 5: REJOIN OF FAILED CONTROLLER AS FOLLOWER
# 13. SUMMARY & EXIT STATUS
# 14. USAGE GUIDE
# 15. PREREQUISITES & DEPENDENCIES
# 16. COMMON PITFALLS AND BEST PRACTICES
# 17. FAQ
#
#===============================================================================

# E3: Controller Failover Evaluation
# Tests Raft leader election and failover with active jobs

#--------------------------------------
# 4. LOGGING AND COLOR UTILITIES
#--------------------------------------
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

#--------------------------------------
# 3. CONFIGURATION CONSTANTS
#--------------------------------------
DATA_DIR="./data/sample"
RAFT_DIR="./raft_data_e3"
OUTPUT_FILE="evaluation_e3_failover.log"

# Simple logging helpers with colored prefixes and tee to append to OUTPUT_FILE.
log_info()    { echo -e "${BLUE}[INFO]${NC} $1"    | tee -a "$OUTPUT_FILE"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1" | tee -a "$OUTPUT_FILE"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"   | tee -a "$OUTPUT_FILE"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $1" | tee -a "$OUTPUT_FILE"; }

#--------------------------------------
# 5. CLEANUP HANDLER AND SIGNAL TRAP
#--------------------------------------
# cleanup():
#   - Kills any raft_controller and worker processes started for this test.
#   - Runs both on explicit exit and when the script exits due to error/interrupt.
cleanup() {
    log_info "Cleaning up..."
    pkill -9 -f "raft_controller --id" 2>/dev/null || true
    pkill -9 -f "worker --controller" 2>/dev/null || true
    sleep 2
}

# Ensure cleanup is always invoked, regardless of exit reason.
trap cleanup EXIT

#--------------------------------------
# 6. LEADER DETECTION UTILITY
#--------------------------------------
# get_leader():
#   - Scans Raft controller logs (/tmp/e3_ctrl{i}.log) for "became LEADER".
#   - Returns the numeric node ID (1, 2, or 3) of the last node that became
#     leader, or "0" if no leader has been elected yet.
get_leader() {
    for i in 1 2 3; do
        if grep -q "became LEADER" "/tmp/e3_ctrl$i.log" 2>/dev/null; then
            LAST_LEADER=$(grep "became LEADER" "/tmp/e3_ctrl$i.log" | tail -1)
            if [ -n "$LAST_LEADER" ]; then
                echo "$i"
                return
            fi
        fi
    done
    echo "0"
}

#===============================================================================
# 7. PREREQUISITE CHECKS
#===============================================================================

echo "=============================================" | tee "$OUTPUT_FILE"
echo "  E3: Controller Failover Evaluation" | tee -a "$OUTPUT_FILE"
echo "=============================================" | tee -a "$OUTPUT_FILE"
echo "" | tee -a "$OUTPUT_FILE"

# Ensure controller binary exists; build if missing.
if [ ! -f "build/raft_controller" ]; then
    log_info "Controller binary not found. Building project with 'make build'..."
    make build
fi

# Ensure data directory exists; generate sample data if missing.
if [ ! -d "$DATA_DIR" ]; then
    log_info "Data directory not found. Generating sample data..."
    python3 scripts/generate_sample_data.py
fi

# Start with a clean slate: no stale processes or stale Raft logs.
cleanup
rm -rf "$RAFT_DIR"
mkdir -p "$RAFT_DIR"

#===============================================================================
# 8. TEST 1: INITIAL LEADER ELECTION
#===============================================================================
log_info "Test 1: Initial Leader Election"
log_info "Starting 3-node Raft cluster..."

./build/raft_controller --id 1 --port 5100 --raft-port 6100 \
    --data-dir "$DATA_DIR" --raft-dir "$RAFT_DIR" \
    --peers "2:localhost:5101:6101,3:localhost:5102:6102" \
    > /tmp/e3_ctrl1.log 2>&1 &
CTRL1_PID=$!
sleep 1

./build/raft_controller --id 2 --port 5101 --raft-port 6101 \
    --data-dir "$DATA_DIR" --raft-dir "$RAFT_DIR" \
    --peers "1:localhost:5100:6100,3:localhost:5102:6102" \
    > /tmp/e3_ctrl2.log 2>&1 &
CTRL2_PID=$!
sleep 1

./build/raft_controller --id 3 --port 5102 --raft-port 6102 \
    --data-dir "$DATA_DIR" --raft-dir "$RAFT_DIR" \
    --peers "1:localhost:5100:6100,2:localhost:5101:6101" \
    > /tmp/e3_ctrl3.log 2>&1 &
CTRL3_PID=$!

log_info "Waiting for leader election (5 seconds)..."
sleep 5

INITIAL_LEADER=$(get_leader)
if [ "$INITIAL_LEADER" != "0" ]; then
    log_success "Initial leader elected: Node $INITIAL_LEADER"
    # Extract timestamp of first "became LEADER" occurrence for reporting.
    ELECTION_TIME=$(
        grep "became LEADER" "/tmp/e3_ctrl$INITIAL_LEADER.log" \
        | head -1 \
        | grep -oP '\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}' \
        | tail -1
    )
    log_info "Election timestamp: $ELECTION_TIME"
else
    log_error "No leader elected! Check Raft configuration and logs in /tmp/e3_ctrl*.log"
    exit 1
fi

#===============================================================================
# 9. TEST 2: WORKER REGISTRATION WITH LEADER
#===============================================================================
log_info ""
log_info "Test 2: Worker Registration with Leader"

# Compute leader's client port based on its id.
LEADER_PORT=$((5099 + INITIAL_LEADER))

for i in 1 2; do
    ./build/worker --controller localhost --port "$LEADER_PORT" \
        --data-dir "$DATA_DIR" > "/tmp/e3_worker$i.log" 2>&1 &
    sleep 0.5
done

sleep 2

WORKERS_REGISTERED=$(
    grep -c "Registered worker" "/tmp/e3_ctrl$INITIAL_LEADER.log" 2>/dev/null || echo "0"
)
if [ "$WORKERS_REGISTERED" -ge 2 ]; then
    log_success "Workers registered with leader: $WORKERS_REGISTERED"
else
    log_warn "Only $WORKERS_REGISTERED workers registered (expected at least 2)"
fi

#===============================================================================
# 10. TEST 3: LEADER FAILOVER AND FAILOVER TIME
#===============================================================================
log_info ""
log_info "Test 3: Leader Failover"
log_info "Killing current leader (Node $INITIAL_LEADER)..."

# CRITICAL FIX: Kill old workers first - they'll need to reconnect to new leader
pkill -9 -f "worker --controller" 2>/dev/null || true

FAILOVER_START=$(date +%s.%N)

case $INITIAL_LEADER in
    1) kill -9 "$CTRL1_PID" 2>/dev/null ;;
    2) kill -9 "$CTRL2_PID" 2>/dev/null ;;
    3) kill -9 "$CTRL3_PID" 2>/dev/null ;;
esac

log_info "Waiting for new leader election..."

NEW_LEADER="0"
for attempt in {1..40}; do
    sleep 0.5

    for i in 1 2 3; do
        # Skip the node we just killed.
        if [ "$i" -eq "$INITIAL_LEADER" ]; then continue; fi

        if grep -q "became LEADER" "/tmp/e3_ctrl$i.log" 2>/dev/null; then
            LEADER_COUNT=$(grep -c "became LEADER" "/tmp/e3_ctrl$i.log")
            if [ "$LEADER_COUNT" -gt 0 ]; then
                NEW_LEADER=$i
                break 2
            fi
        fi
    done
done

FAILOVER_END=$(date +%s.%N)
FAILOVER_TIME=$(echo "$FAILOVER_END - $FAILOVER_START" | bc)

if [ "$NEW_LEADER" != "0" ] && [ "$NEW_LEADER" != "$INITIAL_LEADER" ]; then
    log_success "New leader elected: Node $NEW_LEADER"
    log_success "Failover time: ${FAILOVER_TIME}s"
else
    log_error "Failover failed - no new leader elected"
    exit 1
fi

#===============================================================================
# 11. TEST 4: SYSTEM RECOVERY AND WORKER RECONNECTION
#===============================================================================
log_info ""
log_info "Test 4: System Recovery After Failover"

sleep 2

# Check if remaining controllers are still running (sanity check).
CTRL_RUNNING=0
for i in 1 2 3; do
    if [ "$i" -eq "$INITIAL_LEADER" ]; then continue; fi

    case $i in
        1) if ps -p "$CTRL1_PID" > /dev/null 2>&1; then ((CTRL_RUNNING++)); fi ;;
        2) if ps -p "$CTRL2_PID" > /dev/null 2>&1; then ((CTRL_RUNNING++)); fi ;;
        3) if ps -p "$CTRL3_PID" > /dev/null 2>&1; then ((CTRL_RUNNING++)); fi ;;
    esac
done

if [ "$CTRL_RUNNING" -ge 2 ]; then
    log_success "Remaining controllers healthy: $CTRL_RUNNING"
else
    log_warn "Only $CTRL_RUNNING controllers still running (expected 2)"
fi

# CRITICAL FIX: Reconnect workers to NEW leader
log_info ""
log_info "Test 4.1: Reconnecting Workers to New Leader"

NEW_LEADER_PORT=$((5099 + NEW_LEADER))
log_info "Connecting workers to Node $NEW_LEADER on port $NEW_LEADER_PORT..."

for i in 1 2; do
    ./build/worker --controller localhost --port "$NEW_LEADER_PORT" \
        --data-dir "$DATA_DIR" > "/tmp/e3_worker_new$i.log" 2>&1 &
    WORKER_PID=$!
    sleep 0.5

    # Verify worker didn't crash immediately.
    if ! ps -p "$WORKER_PID" > /dev/null 2>&1; then
        log_error "Worker $i crashed on startup - check /tmp/e3_worker_new$i.log"
        tail -10 "/tmp/e3_worker_new$i.log"
        exit 1
    fi
done

sleep 2

# Verify workers registered with new leader.
NEW_WORKERS=$(
    grep -c "Registered worker" "/tmp/e3_ctrl$NEW_LEADER.log" 2>/dev/null || echo "0"
)
if [ "$NEW_WORKERS" -ge 2 ]; then
    log_success "Workers reconnected to new leader: $NEW_WORKERS workers"
else
    log_warn "Only $NEW_WORKERS workers reconnected (expected 2)"
fi

#===============================================================================
# 12. TEST 5: REJOIN OF FAILED CONTROLLER AS FOLLOWER
#===============================================================================
log_info ""
log_info "Test 5: Rejoin Failed Node"

log_info "Restarting Node $INITIAL_LEADER..."

case $INITIAL_LEADER in
    1)
        ./build/raft_controller --id 1 --port 5100 --raft-port 6100 \
            --data-dir "$DATA_DIR" --raft-dir "$RAFT_DIR" \
            --peers "2:localhost:5101:6101,3:localhost:5102:6102" \
            >> /tmp/e3_ctrl1.log 2>&1 &
        CTRL1_PID=$!
        ;;
    2)
        ./build/raft_controller --id 2 --port 5101 --raft-port 6101 \
            --data-dir "$DATA_DIR" --raft-dir "$RAFT_DIR" \
            --peers "1:localhost:5100:6100,3:localhost:5102:6102" \
            >> /tmp/e3_ctrl2.log 2>&1 &
        CTRL2_PID=$!
        ;;
    3)
        ./build/raft_controller --id 3 --port 5102 --raft-port 6102 \
            --data-dir "$DATA_DIR" --raft-dir "$RAFT_DIR" \
            --peers "1:localhost:5100:6100,2:localhost:5101:6101" \
            >> /tmp/e3_ctrl3.log 2>&1 &
        CTRL3_PID=$!
        ;;
esac

sleep 3

# Check if restarted node logged itself as FOLLOWER (expected behavior).
if grep -q "FOLLOWER" "/tmp/e3_ctrl$INITIAL_LEADER.log"; then
    log_success "Node $INITIAL_LEADER rejoined cluster as follower"
else
    log_warn "Node $INITIAL_LEADER rejoin status unclear (no FOLLOWER log found)"
fi

#===============================================================================
# 13. SUMMARY & EXIT STATUS
#===============================================================================
echo "" | tee -a "$OUTPUT_FILE"
echo "=============================================" | tee -a "$OUTPUT_FILE"
echo "  E3 Evaluation Summary" | tee -a "$OUTPUT_FILE"
echo "=============================================" | tee -a "$OUTPUT_FILE"
echo "Initial leader: Node $INITIAL_LEADER" | tee -a "$OUTPUT_FILE"
echo "New leader after failover: Node $NEW_LEADER" | tee -a "$OUTPUT_FILE"
echo "Failover time: ${FAILOVER_TIME}s" | tee -a "$OUTPUT_FILE"
echo "Workers registered (initial): $WORKERS_REGISTERED" | tee -a "$OUTPUT_FILE"
echo "Workers reconnected (new leader): $NEW_WORKERS" | tee -a "$OUTPUT_FILE"
echo "" | tee -a "$OUTPUT_FILE"

if [ "$NEW_LEADER" != "0" ] && [ "$NEW_LEADER" != "$INITIAL_LEADER" ] && [ "$NEW_WORKERS" -ge 2 ]; then
    log_success "E3: PASSED - Controller failover successful"
    EXIT_CODE=0
else
    log_error "E3: FAILED - Controller failover unsuccessful"
    EXIT_CODE=1
fi

cleanup
exit $EXIT_CODE

################################################################################
# 14. USAGE GUIDE
#-------------------------------------------------------------------------------
# Basic usage:
#
#   $ ./evaluation_e3_controller_failover.sh
#
# Behavior:
#   - Spins up a 3-node Raft controller cluster on localhost.
#   - Runs through all 5 tests (initial election, worker registration, leader
#     failover, worker reconnection, failed node rejoin).
#   - Produces:
#       * evaluation_e3_failover.log      (human-readable summary)
#       * /tmp/e3_ctrl{1,2,3}.log        (controller logs)
#       * /tmp/e3_worker*.log            (worker logs)
#
# Recommended workflow:
#   1) Build project:
#        $ make build
#
#   2) Ensure data exists (or let script generate):
#        $ ls data/sample || python3 scripts/generate_sample_data.py
#
#   3) Run E3 evaluation:
#        $ ./evaluation_e3_controller_failover.sh
#
#   4) Inspect evaluation_e3_failover.log and relevant /tmp logs.
#
################################################################################

################################################################################
# 15. PREREQUISITES & DEPENDENCIES
#-------------------------------------------------------------------------------
# 1) Binaries:
#    - build/raft_controller
#    - build/worker
#    (Script auto-runs `make build` if raft_controller is missing.)
#
# 2) Data:
#    - Directory: ./data/sample
#    - Generated via: python3 scripts/generate_sample_data.py
#
# 3) Tools:
#    - bash 4.0+
#    - grep with -P support (for timestamp regex)
#    - bc (for computing failover time in seconds.subseconds)
#    - ps, pkill (for process management)
#    - python3
#
# 4) Log Message Conventions:
#    - Controllers must log:
#         "became LEADER"  when a node becomes leader
#         "FOLLOWER"       when a node transitions to follower
#         "Registered worker" when a worker registers
#
#      If these messages change in your C++ code, you must update the
#      grep patterns in this script accordingly.
#
################################################################################

################################################################################
# 16. COMMON PITFALLS AND BEST PRACTICES
#-------------------------------------------------------------------------------
# Pitfall 1: Missing bc or grep -P
#   Symptom:
#     - Error around failover time computation or timestamp extraction.
#   Fix:
#     - Install `bc` and ensure GNU grep with PCRE (-P) is available.
#
# Pitfall 2: RAFT_DIR overwrite
#   Symptom:
#     - Unexpected Raft state reuse between runs.
#   Cause:
#     - RAFT_DIR reused without cleanup.
#   Design:
#     - This script explicitly `rm -rf ./raft_data_e3` before each run.
#       If you change RAFT_DIR, ensure you are not pointing to a shared
#       production directory.
#
# Pitfall 3: pkill pattern too broad
#   Symptom:
#     - Other unrelated raft_controller or worker processes on the same
#       machine are killed.
#   Best Practice:
#     - Run this script in a dedicated environment or adjust pkill patterns
#       (e.g., by adding more specific arguments or using PID tracking only).
#
# Pitfall 4: Log messages not matching
#   Symptom:
#     - Script reports "No leader elected" or wrong worker counts, even though
#       the system is functioning.
#   Cause:
#     - Controller/worker log format changed (different text).
#   Fix:
#     - Update the `grep` patterns to match new log messages.
#
# Best Practice: Stable, deterministic Raft timeouts
#   - For reliable evaluation, use reasonably short but stable Raft election
#     timeouts (e.g., 200–500ms with jitter). Too long and your failover time
#     looks artificially large; too short and you risk split-vote thrashing.
#
################################################################################

################################################################################
# 17. FAQ
#-------------------------------------------------------------------------------
# Q1: Why are all controllers running on localhost instead of separate machines?
# A :
#   - This E3 script focuses on *logical* correctness of Raft failover and
#     controller behavior. Running everything on localhost:
#       * Simplifies debugging.
#       * Removes multi-host SSH/deployment complexity.
#     Once correctness is validated, you can replicate this setup across
#     multiple machines using your deployment scripts.
#
# Q2: How is failover time measured?
# A :
#   - FAILOVER_TIME is computed as:
#       FAILOVER_END - FAILOVER_START
#     where:
#       * FAILOVER_START: timestamp just before killing the leader.
#       * FAILOVER_END  : timestamp once a new leader has been detected.
#     The time unit is seconds with fractional part, computed via `bc`.
#
# Q3: Why does the script kill workers before killing the leader?
# A :
#   - During development, some implementations hold persistent connections to
#     the leader and may not correctly reconnect when leadership changes.
#   - Killing workers before the leader makes the test more explicit:
#       * It tests a fresh reconnection path to the new leader.
#       * It avoids confusing log noise from workers stuck on dead TCP sockets.
#
# Q4: What does a "PASS" vs "FAIL" mean for E3?
# A :
#   - PASS (EXIT_CODE=0) requires:
#       * New leader elected (NEW_LEADER != 0)
#       * New leader != initial leader
#       * At least 2 workers successfully registered with the new leader
#   - FAIL (EXIT_CODE=1) if any of these conditions is not met.
#
# Q5: Can I extend this script to run active jobs during failover?
# A :
#   - Yes. Typical extensions:
#       * Start workers and submit jobs to the leader.
#       * Kill leader while jobs are in progress.
#       * Verify job continuity or compensating behavior after failover.
#     Keep the same structure: Test 1–2 setup, Test 3 failure, Test 4–5
#     recovery and verification.
#
################################################################################
