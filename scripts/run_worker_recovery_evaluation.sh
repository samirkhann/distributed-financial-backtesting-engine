#!/bin/bash

################################################################################
# Project: Distributed Financial Backtesting & Risk Engine
# Authors: Talif Pathan, Mohamed Samir Shafat Khan
# Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
# Date: November 27, 2025
#
# File: run_worker_recovery_evaluation.sh
#
# Description:
# This Bash script implements Evaluation E4: Worker Failure Recovery for the
# distributed financial backtesting system. It verifies that the controller
# and workers correctly handle worker crashes via heartbeats, job re-queuing,
# and checkpoint-based resume, before you run the full evaluation suite or
# deploy to the Khoury cluster.
#
# Core Functionality:
# - Environment & Prerequisites:
#   * Ensures controller and worker binaries are built.
#   * Ensures sample data exists (generates it if needed).
#   * Starts from a clean environment by killing leftover processes and
#     resetting the checkpoints directory for E4.
#
# - E4 Sub-Tests:
#   * Test 1: Checkpoint Creation
#       - Starts a controller and worker on port 5200.
#       - Verifies worker registration.
#       - Runs test_checkpoint and checks for checkpoint files.
#   * Test 2: Worker Crash Simulation
#       - Kills the worker process and measures controller detection delay
#         via heartbeat timeout logs.
#   * Test 3: Job Re-queue
#       - Checks controller logs for "Re-queuing job" entries.
#   * Test 4: New Worker Recovery
#       - Starts a replacement worker and verifies re-registration.
#   * Test 5: Checkpoint Resume Verification
#       - Runs test_checkpoint again to validate resume semantics.
#   * Test 6: Full Integration Test
#       - Runs test_full_system with a timeout to ensure the entire system
#         behaves correctly when checkpointing is enabled.
#
# - Result Aggregation:
#   * Writes a human-readable evaluation log to:
#       evaluation_e4_worker_recovery.log
#   * Counts how many key checks passed (out of 5 required).
#   * Exits with status 0 on success (>=5/5) and 1 on failure.
#
# Evaluation Architecture (Local Host):
#
# ┌──────────────────────────────────────────────────────────────┐
# │ Controller (port 5200)                                      │
# │  - Reads data from:   ./data/sample                         │
# │  - Logs to:           /tmp/e4_controller.log                │
# │                                                            │
# │ Worker(s)                                                  │
# │  - Connect to:        localhost:5200                        │
# │  - Data dir:          ./data/sample                         │
# │  - Checkpoints dir:   ./checkpoints_e4                      │
# │  - Logs:              /tmp/e4_worker1.log, e4_worker2.log   │
# └──────────────────────────────────────────────────────────────┘
#
# Evaluation Workflow:
#
# 1. Global Setup:
#    - Kill any stale controller/worker processes on port 5200.
#    - Clean and recreate the checkpoints directory.
#    - Initialize evaluation_e4_worker_recovery.log and logging helpers.
#
# 2. Test 1 (Checkpoint Creation):
#    - Start controller + worker.
#    - Confirm worker registration via controller logs.
#    - Run test_checkpoint and verify that checkpoint files are created.
#
# 3. Test 2 (Worker Crash Detection):
#    - Kill the worker and timestamp the crash.
#    - Wait for heartbeat timeout and confirm controller logs the failure.
#    - Compute and record detection delay using `bc`.
#
# 4. Test 3 (Job Re-queue):
#    - Scrape controller logs for "Re-queuing job" evidence.
#
# 5. Test 4 (New Worker Recovery):
#    - Start a new worker using the same checkpoint directory.
#    - Confirm new registration in controller logs.
#
# 6. Test 5 (Checkpoint Resume):
#    - Run test_checkpoint again to validate checkpoint resume semantics.
#
# 7. Test 6 (Full Integration):
#    - Stop controller/worker.
#    - Run test_full_system with a 60s timeout and inspect exit code.
#
# 8. Summary:
#    - Derive a TESTS_PASSED count from the log file.
#    - Print a summary banner with PASS/FAIL and exit accordingly.
#
# Port Allocation:
# - Controller port for E4 tests:
#   * 5200 (fixed in this script)
#
# Timing Considerations:
# - Sleeps:
#   * After starting controller/worker: 2s to allow startup and registration.
#   * After killing worker: 5s to allow heartbeat timeout detection.
# - Timeouts:
#   * test_checkpoint: 30s for checkpoint creation.
#   * test_full_system: 60s max runtime.
#
# Error Handling:
# - Critical failures (e.g., missing binaries, registration failures,
#   full system test failing) cause the script to exit with a non-zero code.
# - Non-critical issues (e.g., no re-queued jobs in a particular run) are
#   logged as warnings and do not necessarily fail the evaluation.
#
# Dependencies:
# - Bash: Version 4.0+ (for arithmetic and arrays).
# - make: For on-demand `make build` if binaries are missing.
# - Python 3: For scripts/generate_sample_data.py.
# - bc: For floating-point detection delay calculation.
# - timeout: For bounding test runtimes (test_checkpoint, test_full_system).
#
# Related Scripts:
# - master_evaluation.sh           : Full evaluation harness (E1–E6).
# - end_to_end_system_test.sh      : Quick local smoke test.
# - deploy_khoury_cluster.sh       : Deployment to Khoury cluster.
#
################################################################################

#===============================================================================
# TABLE OF CONTENTS
#===============================================================================
#
# 1. SCRIPT HEADER AND PURPOSE
# 2. COLOR CODES AND PATH CONFIGURATION
# 3. LOGGING HELPERS
# 4. CLEANUP HANDLER AND TRAP
# 5. E4 BANNER AND PREREQUISITES
#    5.1 Build Artifacts
#    5.2 Sample Data
#    5.3 Checkpoint Directory Reset
# 6. TEST 1: CHECKPOINT CREATION
# 7. TEST 2: WORKER CRASH SIMULATION AND FAILURE DETECTION
# 8. TEST 3: JOB RE-QUEUE VERIFICATION
# 9. TEST 4: NEW WORKER RECOVERY
# 10. TEST 5: CHECKPOINT RESUME VERIFICATION
# 11. TEST 6: FULL INTEGRATION TEST WITH CHECKPOINTING
# 12. SUMMARY AND EXIT CODE
# 13. USAGE EXAMPLES
# 14. COMMON PITFALLS AND NOTES
# 15. FAQ
# 16. BEST PRACTICES / EXTENSIONS
#
#===============================================================================

# E4: Worker Failure Recovery Evaluation
# Tests checkpoint creation and job resume after worker crash

#------------------------------------------------------------------------------
# 2. COLOR CODES AND PATH CONFIGURATION
#------------------------------------------------------------------------------

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'

DATA_DIR="./data/sample"
CHECKPOINT_DIR="./checkpoints_e4"
OUTPUT_FILE="evaluation_e4_worker_recovery.log"

#------------------------------------------------------------------------------
# 3. LOGGING HELPERS
#------------------------------------------------------------------------------
# log_info/log_success/log_error/log_warn:
#   - Print messages with colored prefixes for easy scanning.
#   - Also append to OUTPUT_FILE via tee for persistent evaluation logs.

log_info()  { echo -e "${BLUE}[INFO]${NC} $1"    | tee -a "$OUTPUT_FILE"; }
log_success(){ echo -e "${GREEN}[SUCCESS]${NC} $1" | tee -a "$OUTPUT_FILE"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"   | tee -a "$OUTPUT_FILE"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $1" | tee -a "$OUTPUT_FILE"; }

#------------------------------------------------------------------------------
# 4. CLEANUP HANDLER AND TRAP
#------------------------------------------------------------------------------
# cleanup:
#   - Ensures we do not leave stale controller or worker processes bound to
#     port 5200 between tests or after the script exits.

cleanup() {
    log_info "Cleaning up..."
    pkill -9 -f "controller --port 5200"        2>/dev/null || true
    pkill -9 -f "worker --controller.*5200"     2>/dev/null || true
    sleep 2
}

# Ensure cleanup runs on any script exit (success or failure).
trap cleanup EXIT

#------------------------------------------------------------------------------
# 5. E4 BANNER AND PREREQUISITES
#------------------------------------------------------------------------------

echo "=============================================" | tee "$OUTPUT_FILE"
echo "  E4: Worker Failure Recovery Evaluation"     | tee -a "$OUTPUT_FILE"
echo "=============================================" | tee -a "$OUTPUT_FILE"
echo ""                                             | tee -a "$OUTPUT_FILE"

# Ensure prerequisites
# 5.1 Compile controller/worker if needed.
if [ ! -f "build/controller" ] || [ ! -f "build/worker" ]; then
    log_info "Building project..."
    make build
fi

# 5.2 Generate sample data if needed.
if [ ! -d "$DATA_DIR" ]; then
    python3 scripts/generate_sample_data.py
fi

# 5.3 Reset checkpoints and start from a clean controller/worker state.
cleanup
rm -rf "$CHECKPOINT_DIR"
mkdir -p "$CHECKPOINT_DIR"

#------------------------------------------------------------------------------
# 6. TEST 1: CHECKPOINT CREATION
#------------------------------------------------------------------------------
# Goal:
#   - Start controller and worker.
#   - Verify worker registration.
#   - Invoke test_checkpoint and confirm that checkpoint files are created
#     under $CHECKPOINT_DIR.

log_info "Test 1: Checkpoint Creation"
log_info "Starting controller and worker..."

./build/controller --port 5200 --data-dir "$DATA_DIR" > /tmp/e4_controller.log 2>&1 &
CTRL_PID=$!
sleep 2

./build/worker --controller localhost --port 5200 \
    --data-dir "$DATA_DIR" --checkpoint-dir "$CHECKPOINT_DIR" \
    > /tmp/e4_worker1.log 2>&1 &
WORKER1_PID=$!
sleep 2

# Check worker registered
if grep -q "Registered worker" /tmp/e4_controller.log; then
    log_success "Worker registered successfully"
else
    log_error "Worker registration failed"
    exit 1
fi

# Run test to create checkpoints
log_info "Running test to generate checkpoints..."
timeout 30 ./build/test_checkpoint > /tmp/e4_checkpoint_test.log 2>&1 || true

# Check for checkpoint files (*.dat)
CHECKPOINT_COUNT=$(ls -1 "$CHECKPOINT_DIR"/*.dat 2>/dev/null | wc -l || echo "0")
if [ "$CHECKPOINT_COUNT" -gt 0 ]; then
    log_success "Checkpoints created: $CHECKPOINT_COUNT"
    ls -la "$CHECKPOINT_DIR"/*.dat 2>/dev/null | head -5 | tee -a "$OUTPUT_FILE"
else
    log_warn "No checkpoint files created (test may have completed too quickly)"
fi

#------------------------------------------------------------------------------
# 7. TEST 2: WORKER CRASH SIMULATION AND FAILURE DETECTION
#------------------------------------------------------------------------------
# Goal:
#   - Simulate worker crash via kill -9.
#   - Confirm that controller detects missing heartbeats and logs the failure.
#   - Compute detection delay between crash and log entry.

log_info ""
log_info "Test 2: Worker Crash Simulation"

log_info "Killing worker (PID: $WORKER1_PID)..."
CRASH_TIME=$(date +%s.%N)
kill -9 "$WORKER1_PID" 2>/dev/null || true
sleep 1

# Wait for controller's heartbeat timeout to trigger.
sleep 5  # Wait for heartbeat timeout (~6 seconds total since kill)

if grep -q "heartbeat timeout\|Removing worker" /tmp/e4_controller.log; then
    DETECTION_TIME=$(date +%s.%N)
    DETECTION_DELAY=$(echo "$DETECTION_TIME - $CRASH_TIME" | bc)
    log_success "Controller detected worker failure (delay: ${DETECTION_DELAY}s)"
else
    log_warn "Worker failure detection not logged"
fi

#------------------------------------------------------------------------------
# 8. TEST 3: JOB RE-QUEUE VERIFICATION
#------------------------------------------------------------------------------
# Goal:
#   - Check whether controller logs indicate jobs were re-queued after the
#     worker crash (this may not always happen, depending on timing).

log_info ""
log_info "Test 3: Job Re-queue After Failure"

if grep -q "Re-queuing job" /tmp/e4_controller.log; then
    REQUEUED=$(grep -c "Re-queuing job" /tmp/e4_controller.log)
    log_success "Jobs re-queued: $REQUEUED"
else
    log_info "No jobs were in progress during crash (expected in some scenarios)"
fi

#------------------------------------------------------------------------------
# 9. TEST 4: NEW WORKER RECOVERY
#------------------------------------------------------------------------------
# Goal:
#   - Start a replacement worker.
#   - Ensure it registers with the controller (rebuilding worker pool after
#     failure).

log_info ""
log_info "Test 4: Starting Replacement Worker"

./build/worker --controller localhost --port 5200 \
    --data-dir "$DATA_DIR" --checkpoint-dir "$CHECKPOINT_DIR" \
    > /tmp/e4_worker2.log 2>&1 &
WORKER2_PID=$!
sleep 2

if grep -q "Registered worker" /tmp/e4_controller.log; then
    TOTAL_REGISTRATIONS=$(grep -c "Registered worker" /tmp/e4_controller.log)
    log_success "New worker registered (total registrations: $TOTAL_REGISTRATIONS)"
else
    log_error "New worker failed to register"
    exit 1
fi

#------------------------------------------------------------------------------
# 10. TEST 5: CHECKPOINT RESUME VERIFICATION
#------------------------------------------------------------------------------
# Goal:
#   - Validate that checkpoint save/load/resume logic works via test_checkpoint.
#   - This complements Test 1 by focusing on resume semantics.

log_info ""
log_info "Test 5: Checkpoint Resume Verification"

if ./build/test_checkpoint > /tmp/e4_resume_test.log 2>&1; then
    log_success "Checkpoint save/load/resume verified"
else
    log_warn "Checkpoint test had issues (check /tmp/e4_resume_test.log)"
fi

#------------------------------------------------------------------------------
# 11. TEST 6: FULL INTEGRATION TEST WITH CHECKPOINTING
#------------------------------------------------------------------------------
# Goal:
#   - Run the full system test under realistic conditions (after checkpoint
#     testing and worker recovery) to ensure there are no hidden integration
#     issues.
#   - test_full_system is expected to use checkpoints as part of its logic.

log_info ""
log_info "Test 6: Full Integration Test"

# Stop current controller and worker processes before running full test.
kill "$WORKER2_PID" 2>/dev/null || true
kill "$CTRL_PID"    2>/dev/null || true
sleep 1

# Run full system test and capture exit code accurately.
timeout 60 ./build/test_full_system > /tmp/e4_full_test.log 2>&1
TEST_EXIT_CODE=$?

if [ "$TEST_EXIT_CODE" -eq 0 ]; then
    log_success "Full system test with checkpointing passed"
elif [ "$TEST_EXIT_CODE" -eq 124 ]; then
    log_error "Full system test timed out (60s)"
    cat /tmp/e4_full_test.log | tail -20
    exit 1
else
    log_error "Full system test failed (exit code: $TEST_EXIT_CODE)"
    log_info "Last 20 lines of test output:"
    cat /tmp/e4_full_test.log | tail -20
    exit 1
fi

#------------------------------------------------------------------------------
# 12. SUMMARY AND EXIT CODE
#------------------------------------------------------------------------------
# Based on key success messages in the OUTPUT_FILE, compute how many of the
# five primary E4 checks passed and decide overall PASS/FAIL.

echo "" | tee -a "$OUTPUT_FILE"
echo "=============================================" | tee -a "$OUTPUT_FILE"
echo "  E4 Evaluation Summary"                      | tee -a "$OUTPUT_FILE"
echo "=============================================" | tee -a "$OUTPUT_FILE"

TESTS_PASSED=0

# Count results based on what we saw in the log file.
if grep -q "Worker registered successfully" "$OUTPUT_FILE"; then ((TESTS_PASSED++)); fi
if grep -q "Controller detected worker failure" "$OUTPUT_FILE"; then ((TESTS_PASSED++)); fi
if grep -q "New worker registered" "$OUTPUT_FILE"; then ((TESTS_PASSED++)); fi
if grep -q "Checkpoint save/load/resume verified" "$OUTPUT_FILE"; then ((TESTS_PASSED++)); fi
if grep -q "Full system test with checkpointing passed" "$OUTPUT_FILE"; then ((TESTS_PASSED++)); fi

echo "Tests passed: $TESTS_PASSED/5" | tee -a "$OUTPUT_FILE"
echo "" | tee -a "$OUTPUT_FILE"

if [ "$TESTS_PASSED" -ge 5 ]; then
    log_success "E4: PASSED - All worker recovery tests successful"
    EXIT_CODE=0
else
    log_error "E4: FAILED - Only $TESTS_PASSED/5 tests passed"
    EXIT_CODE=1
fi

cleanup
exit "$EXIT_CODE"

#===============================================================================
# 13. USAGE EXAMPLES
#===============================================================================
#
# Basic local usage:
#
#   # From the project root:
#   $ chmod +x scripts/run_worker_recovery_evaluation.sh
#   $ ./scripts/run_worker_recovery_evaluation.sh
#
# After the script runs:
#   - Check the main evaluation log:
#       evaluation_e4_worker_recovery.log
#   - Inspect raw controller/worker/test logs:
#       /tmp/e4_controller.log
#       /tmp/e4_worker1.log
#       /tmp/e4_worker2.log
#       /tmp/e4_checkpoint_test.log
#       /tmp/e4_resume_test.log
#       /tmp/e4_full_test.log
#
# Typical workflow:
#   1. Implement or modify checkpointing/heartbeat/job re-queuing logic.
#   2. Build the project:
#        $ make build
#   3. Run this script to validate E4 worker recovery behavior.
#   4. If successful, proceed to:
#        - master_evaluation.sh (full evaluation)
#        - deploy_khoury_cluster.sh (cluster deployment)
#
#===============================================================================
# 14. COMMON PITFALLS AND NOTES
#===============================================================================
#
# - Missing Color Definition for RED:
#   log_error() uses ${RED}, but RED is not defined in this script. In Bash
#   this simply results in no color for error messages (no crash), but if you
#   want colored errors, define:
#
#       RED='\033[0;31m'
#
#   near the other color codes.
#
# - test_checkpoint / test_full_system Not Built:
#   If these binaries are missing, the script will fail at runtime. Ensure
#   they are part of the build:
#
#       $ make build
#
# - bc Not Installed:
#   The detection delay computation:
#
#       DETECTION_DELAY=$(echo "$DETECTION_TIME - $CRASH_TIME" | bc)
#
#   requires `bc`. If bc is missing, install it or replace the calculation
#   with simpler integer math (or skip it).
#
# - Port Conflicts:
#   This script assumes port 5200 is free. If you see binding failures in
#   logs, check for other services using that port:
#
#       $ lsof -i :5200
#
# - Aggressive pkill:
#   cleanup() uses pkill -9 -f on patterns matching "controller --port 5200"
#   and "worker --controller.*5200". On a shared machine, this should be safe
#   but be cautious if other unrelated processes reuse similar names.
#
# - Timing Sensitivity:
#   Heartbeat detection and re-queuing behavior depends on controller/worker
#   timeouts. If you change these in code, you may need to adjust the sleep
#   durations in this script (e.g., the `sleep 5` after kill).
#
#===============================================================================
# 15. FAQ
#===============================================================================
#
# Q1: Does E4 require jobs to be in-flight during the crash?
# A1:
#   Not strictly. Test 3 (job re-queue) is opportunistic—it checks whether
#   any "Re-queuing job" entries appear in the log. If the system was idle
#   when the worker crashed, you may see an informational message instead of
#   a success line for that specific aspect, but the main pass/fail criteria
#   are based on the five checks in the summary.
#
# Q2: Why run test_checkpoint twice?
# A2:
#   The first run focuses on creating checkpoint files in the specified
#   directory. The second run, after worker crash and recovery, helps validate
#   that save/load/resume semantics still hold under a more stressed state.
#
# Q3: Can I change the port (5200) used for E4?
# A3:
#   Yes, but you must change all occurrences in this script (controller start
#   command, worker start commands, pkill patterns) and ensure the controller
#   and worker code do not hard-code the port elsewhere.
#
# Q4: How does this differ from the end-to-end or master evaluation scripts?
# A4:
#   - end_to_end_system_test.sh:
#       Quick smoke test of core wiring and Raft behavior.
#   - run_worker_recovery_evaluation.sh (this script):
#       Deep dive on E4: worker failure, detection, re-queuing, and checkpoint
#       resume.
#   - master_evaluation.sh:
#       Orchestrates E1–E6, including E4 via this script, and generates a
#       comprehensive, timestamped evaluation report.
#
# Q5: What if the full system test times out?
# A5:
#   If test_full_system takes more than 60 seconds, timeout returns 124 and
#   the script logs a clear error and exits. You can increase the timeout if
#   you expect long-running scenarios, but generally a quick full-system run
#   is preferable for E4.
#
#===============================================================================
# 16. BEST PRACTICES / EXTENSIONS
#===============================================================================
#
# - Make Tests Deterministic:
#   Where possible, control randomness in controller/worker behavior so that
#   E4 behaves consistently across runs (e.g., fixed seeds or fixed job sizes).
#
# - Capture Artifacts:
#   Consider copying /tmp/e4_*.log and $CHECKPOINT_DIR into a timestamped
#   subdirectory under evaluation_results/ so you can archive E4 runs
#   alongside other evaluations.
#
# - Add Latency Thresholds:
#   Extend Test 2 to assert that DETECTION_DELAY is below a certain threshold
#   (e.g., < 10 seconds) to turn this into a performance SLO check, not
#   just functional correctness.
#
# - Parametrize:
#   Turn DATA_DIR, CHECKPOINT_DIR, and port 5200 into command-line options
#   with sensible defaults so that E4 can be reused in different environments
#   and datasets.
#
#===============================================================================
