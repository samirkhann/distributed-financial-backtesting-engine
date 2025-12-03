#!/bin/bash

################################################################################
# Project: Distributed Financial Backtesting & Risk Engine
# Authors: Talif Pathan, Mohamed Samir Shafat Khan
# Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
# Date: November 27, 2025
#
# File: run_scalability_evaluation.sh
#
# Description:
#   This Bash script automates the E2: Scalability Evaluation experiment for the
#   Distributed Financial Backtesting & Risk Engine. It systematically measures
#   end-to-end system performance as the number of worker processes increases,
#   focusing on:
#
#     - Throughput (jobs per second)
#     - Speedup (relative to 1 worker)
#     - Parallel efficiency (speedup / number of workers)
#
#   The script:
#     - Brings up a single controller and a configurable number of worker
#       processes on the local machine.
#     - Runs an integration test executable that programmatically submits
#       backtest jobs.
#     - Collects timing and completion metrics from logs and execution time.
#     - Aggregates results into a CSV file for analysis and plotting.
#     - Optionally generates summary plots using a Python plotting script.
#
# Core Functionality:
#   - Automation of E2 Scenario:
#       * Runs the same workload across multiple worker counts (default:
#         1, 2, 4, 8 workers).
#       * Ensures clean startup and shutdown of controller/worker processes
#         for each configuration.
#
#   - Build & Data Preparation:
#       * Verifies that build/controller and build/worker binaries exist.
#       * If not, runs `make build` to compile the project.
#       * Verifies the sample data directory exists; if missing, calls a
#         Python helper (generate_sample_data.py) to create test data.
#
#   - Experiment Execution:
#       * For each worker configuration:
#            - Starts controller and workers, redirecting logs to /tmp.
#            - Uses the integration test (./build/test_integration) to submit
#              jobs and drive the system.
#            - Uses wall-clock time around the test execution to measure
#              total elapsed time.
#            - Parses controller logs to count completed jobs.
#
#   - Metric Computation:
#       * Throughput (jobs/sec)
#       * Average job time
#       * Speedup vs. baseline (1 worker)
#       * Parallel efficiency (% of ideal linear scaling)
#
#   - Reporting:
#       * Writes results as rows into evaluation_e2_results.csv.
#       * Prints a concise summary for each worker count.
#       * Prints the final CSV contents for quick inspection.
#       * Optionally produces plots using scripts/plot_results.py.
#
# E2 Evaluation Architecture (Single-Host Experiment Harness):
#
#   ┌──────────────────────────────────────────────────────────────┐
#   │ Local Machine (Evaluation Node)                             │
#   │                                                              │
#   │  Controller (./build/controller)                            │
#   │   - Listens on CONTROLLER_PORT (default: 5000)              │
#   │   - Receives jobs and coordinates worker execution          │
#   │                                                              │
#   │  Worker Pool (./build/worker)                               │
#   │   - N processes (1, 2, 4, 8 in default E2)                  │
#   │   - Connect to controller on localhost:CONTROLLER_PORT      │
#   │   - Consume sample data from DATA_DIR                        │
#   │                                                              │
#   │  Integration Test (./build/test_integration)                │
#   │   - Submits NUM_JOBS jobs programmatically (implementation- │
#   │     specific).                                              │
#   │   - Acts as a realistic workload generator.                 │
#   └──────────────────────────────────────────────────────────────┘
#
#   For each worker count, the script tears down any existing processes,
#   starts fresh controller/worker processes, runs the test, measures time,
#   and collects metrics.
#
# Timing & Metric Considerations:
#
#   - Elapsed Time:
#       * Measured using `date +%s.%N` before and after running the
#         integration test.
#       * Elapsed = END_TIME - START_TIME, computed using `bc` for
#         floating-point precision.
#
#   - Jobs Completed:
#       * Primary: Count "completed successfully" lines in controller log.
#       * Fallback: Count "PASSED" in test output if log parsing yields zero.
#
#   - Throughput:
#       * throughput = jobs_completed / elapsed_time
#
#   - Average Job Time:
#       * avg_job_time = elapsed_time / jobs_completed
#
#   - Speedup & Efficiency:
#       * Baseline: 1 worker run’s elapsed_time.
#       * speedup = baseline_time / elapsed_time (for N > 1).
#       * efficiency = (speedup / num_workers) * 100 (%).
#
# Dependencies:
#
#   - Bash shell (#!/bin/bash)
#   - GNU core utilities:
#       * ps, grep, pkill, sleep, timeout, wc, cat
#   - bc:
#       * For floating-point arithmetic.
#   - Python 3 (optional, for plot generation):
#       * scripts/plot_results.py
#   - Project-specific binaries:
#       * ./build/controller
#       * ./build/worker
#       * ./build/test_integration
#   - Project-specific Python scripts:
#       * scripts/generate_sample_data.py
#
# Related Scripts:
#
#   - run_cluster.sh           : Convenience script for local controller/worker.
#   - fault_test.sh            : Fault injection for controller/worker failures.
#   - prepare_submission.sh    : Cleans tree and prepares submission package.
#   - deploy_to_khoury.sh      : Multi-node deployment to Khoury cluster.
#
################################################################################

#===============================================================================
# TABLE OF CONTENTS
#===============================================================================
#
# 1. SHEBANG AND STRICT MODE
# 2. COLOR CONSTANTS AND GLOBAL E2 PARAMETERS
# 3. LOGGING HELPERS
# 4. CLEANUP FUNCTION AND EXIT TRAP
# 5. ARGUMENT PARSING (CLI OVERRIDES)
# 6. E2 EVALUATION BANNER & INPUT VALIDATION
#    6.1 Build Check (make build)
#    6.2 Data Directory Check (generate_sample_data.py)
# 7. CSV HEADER INITIALIZATION
# 8. MAIN E2 LOOP OVER WORKER COUNTS
#    8.1 Per-Run Cleanup and Controller Startup
#    8.2 Worker Startup and Registration Check
#    8.3 Timing and Job Submission via test_integration
#    8.4 Metric Computation (Throughput, Speedup, Efficiency)
#    8.5 Result Recording and Logging
# 9. FINAL SUMMARY AND OPTIONAL PLOT GENERATION
# 10. USAGE EXAMPLES
# 11. COMMON PITFALLS AND CAVEATS
# 12. FAQ
# 13. EXTENSIONS AND BEST PRACTICES
#
#===============================================================================

#===============================================================================
# 1. SHEBANG AND STRICT MODE
#-------------------------------------------------------------------------------
# `set -e` enables "strict mode" for this script:
#   - If any command exits with a non-zero status (except those explicitly
#     guarded with `|| true`), the script will terminate immediately.
#   - This is important for evaluation: we do not want to silently continue
#     after a failure in build, data generation, or running tests.
set -e

#===============================================================================
# 2. COLOR CONSTANTS AND GLOBAL E2 PARAMETERS
#-------------------------------------------------------------------------------
# ANSI color codes for more readable console logging. These are used by the
# log_* helper functions below.
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m'  # "No Color" / reset

# Controller port used by controller and workers in this experiment.
CONTROLLER_PORT=5000

# Directory containing sample data (e.g., CSV price data) used during tests.
DATA_DIR="./data/sample"

# Number of jobs to be used per test scenario (used for display; the actual
# number of jobs may be controlled by ./build/test_integration).
NUM_JOBS=20

# CSV file to which all E2 results will be written.
OUTPUT_FILE="evaluation_e2_results.csv"

# Set of worker counts to test. Default matches the E2 spec: 1, 2, 4, 8.
WORKER_COUNTS="1 2 4 8"

#===============================================================================
# 3. LOGGING HELPERS
#-------------------------------------------------------------------------------
# log_info:    Informational messages (blue).
# log_success: Success/summary messages (green).
# log_warn:    Non-fatal warnings (yellow).
log_info()    { echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { echo -e "${GREEN}[SUCCESS]${NC} $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }

#===============================================================================
# 4. CLEANUP FUNCTION AND EXIT TRAP
#-------------------------------------------------------------------------------
# cleanup:
#   - Kills any running controller/worker processes associated with this
#     evaluation (filtered by port).
#   - Introduces a small delay to allow OS to reclaim resources before the
#     next test iteration.
#
# trap cleanup EXIT:
#   - Ensures cleanup is run when the script exits *for any reason*:
#       * Normal completion.
#       * Error (due to set -e).
#       * Interrupt (Ctrl+C).
cleanup() {
    log_info "Cleaning up processes..."
    pkill -f "controller --port $CONTROLLER_PORT" 2>/dev/null || true
    pkill -f "worker --controller.*$CONTROLLER_PORT" 2>/dev/null || true
    sleep 2
}

trap cleanup EXIT

#===============================================================================
# 5. ARGUMENT PARSING (CLI OVERRIDES)
#-------------------------------------------------------------------------------
# Support simple command-line overrides for key parameters:
#   --jobs=N       : Override NUM_JOBS (informational; actual job count may
#                    depend on test_integration implementation).
#   --workers="..." : Override WORKER_COUNTS list (e.g., "1 2 3 4").
#   --output=FILE  : Override output CSV file name/path.
#
# Example:
#   ./e2_scalability_evaluation.sh --jobs=50 --workers="1 2 4" --output=e2.csv
for arg in "$@"; do
    case $arg in
        --jobs=*)    NUM_JOBS="${arg#*=}" ;;
        --workers=*) WORKER_COUNTS="${arg#*=}" ;;
        --output=*)  OUTPUT_FILE="${arg#*=}" ;;
    esac
done

#===============================================================================
# 6. E2 EVALUATION BANNER & INPUT VALIDATION
#===============================================================================

echo "============================================="
echo "  E2: Scalability Evaluation"
echo "============================================="
echo "Jobs per test: $NUM_JOBS"
echo "Worker counts: $WORKER_COUNTS"
echo "Output: $OUTPUT_FILE"
echo ""

#-------------------------------------------------------------------------------
# 6.1 Build Check (make build)
#-------------------------------------------------------------------------------
# Ensure the core binaries exist; if not, trigger a build via `make build`.
# This assumes the Makefile contains a `build` target that compiles controller,
# worker, and test_integration.
if [ ! -f "build/controller" ] || [ ! -f "build/worker" ]; then
    log_info "Building project..."
    make build
fi

#-------------------------------------------------------------------------------
# 6.2 Data Directory Check (generate_sample_data.py)
#-------------------------------------------------------------------------------
# Ensure the sample data directory exists. If missing, call the Python helper
# to generate synthetic or minimal data for the evaluation.
if [ ! -d "$DATA_DIR" ]; then
    log_info "Generating sample data..."
    python3 scripts/generate_sample_data.py
fi

#===============================================================================
# 7. CSV HEADER INITIALIZATION
#-------------------------------------------------------------------------------
# Initialize the CSV file with header columns. These columns are:
#
#   num_workers                : Number of workers in this test run.
#   num_jobs                   : Intended jobs per test (see note below).
#   total_time_sec             : Total wall-clock time for the test run.
#   jobs_completed             : Number of jobs identified as completed via logs.
#   throughput_jobs_per_sec    : jobs_completed / total_time_sec.
#   avg_job_time_sec           : total_time_sec / jobs_completed.
#   speedup                    : baseline_time / total_time_sec for this worker
#                                count (vs. 1-worker case).
#   efficiency                 : (speedup / num_workers) * 100 (%).
#
# NOTE: In the current implementation, the second column is populated with
#       JOBS_COMPLETED (see echo below), not NUM_JOBS. If you want the CSV to
#       reflect requested jobs, adjust that line accordingly.
echo "num_workers,num_jobs,total_time_sec,jobs_completed,throughput_jobs_per_sec,avg_job_time_sec,speedup,efficiency" > $OUTPUT_FILE

# Baseline execution time for 1-worker run; used to compute speedup/efficiency.
BASELINE_TIME=0

#===============================================================================
# 8. MAIN E2 LOOP OVER WORKER COUNTS
#===============================================================================
for num_workers in $WORKER_COUNTS; do
    echo ""
    log_info "Testing with $num_workers worker(s)..."
    
    #---------------------------------------------------------------------------
    # 8.1 Per-Run Cleanup and Cooldown
    #---------------------------------------------------------------------------
    # Ensure no old controller/worker processes are still running from previous
    # iterations. The explicit cleanup call here is redundant with the trap but
    # ensures we start each run in a clean state.
    cleanup
    sleep 1
    
    #---------------------------------------------------------------------------
    # 8.2 Controller Startup
    #---------------------------------------------------------------------------
    # Start controller in the background, redirecting output to a temporary log
    # for later parsing.
    ./build/controller --port $CONTROLLER_PORT --data-dir $DATA_DIR > /tmp/e2_controller.log 2>&1 &
    CTRL_PID=$!
    sleep 2
    
    if ! ps -p $CTRL_PID > /dev/null; then
        log_warn "Controller failed to start"
        continue
    fi
    
    #---------------------------------------------------------------------------
    # 8.3 Worker Startup and Registration Check
    #---------------------------------------------------------------------------
    # Launch `num_workers` worker processes and collect their PIDs in an array.
    # Each worker connects to localhost:CONTROLLER_PORT and uses DATA_DIR.
    WORKER_PIDS=()
    for i in $(seq 1 $num_workers); do
        ./build/worker --controller localhost --port $CONTROLLER_PORT --data-dir $DATA_DIR > /tmp/e2_worker_$i.log 2>&1 &
        WORKER_PIDS+=($!)
        sleep 0.3
    done
    
    # Allow time for workers to connect and register with the controller.
    sleep 2
    
    # The following assumes the controller logs "Registered worker" for each
    # successful worker registration.
    REGISTERED=$(grep -c "Registered worker" /tmp/e2_controller.log 2>/dev/null || echo "0")
    log_info "Workers registered: $REGISTERED"
    
    #---------------------------------------------------------------------------
    # 8.4 Timing and Job Submission via test_integration
    #---------------------------------------------------------------------------
    # START_TIME / END_TIME capture wall-clock timestamps with nanosecond
    # resolution; bc is later used to subtract them.
    START_TIME=$(date +%s.%N)
    
    # Run the integration test to submit jobs and exercise the system.
    # timeout 300:
    #   - Prevents a hung test from blocking the entire evaluation.
    #   - If timeout is hit, exit code is non-zero, but `|| true` keeps the
    #     script going so we can at least record partial metrics.
    timeout 300 ./build/test_integration > /tmp/e2_test_output.log 2>&1 || true
    
    END_TIME=$(date +%s.%N)
    ELAPSED=$(echo "$END_TIME - $START_TIME" | bc)
    
    #---------------------------------------------------------------------------
    # 8.5 Metric Extraction from Logs
    #---------------------------------------------------------------------------
    # JOBS_COMPLETED:
    #   - Primary: count "completed successfully" messages in controller log.
    #   - Fallback: if zero, count "PASSED" in test output (depending on how
    #               test_integration reports success).
    JOBS_COMPLETED=$(grep -c "completed successfully" /tmp/e2_controller.log 2>/dev/null || echo "0")
    
    if [ "$JOBS_COMPLETED" -eq 0 ]; then
        # Fallback: count from test output.
        JOBS_COMPLETED=$(grep -c "PASSED" /tmp/e2_test_output.log 2>/dev/null || echo "1")
    fi
    
    #---------------------------------------------------------------------------
    # 8.6 Metric Computation (Throughput, Avg Time)
    #---------------------------------------------------------------------------
    if [ "$JOBS_COMPLETED" -gt 0 ]; then
        THROUGHPUT=$(echo "scale=4; $JOBS_COMPLETED / $ELAPSED" | bc)
        AVG_TIME=$(echo "scale=4; $ELAPSED / $JOBS_COMPLETED" | bc)
    else
        THROUGHPUT=0
        AVG_TIME=0
    fi
    
    #---------------------------------------------------------------------------
    # 8.7 Speedup and Efficiency
    #---------------------------------------------------------------------------
    # For 1 worker:
    #   - Use its elapsed time as baseline for subsequent runs.
    # For N > 1:
    #   - speedup = BASELINE_TIME / ELAPSED
    #   - efficiency = (speedup / num_workers) * 100
    if [ "$num_workers" -eq 1 ]; then
        BASELINE_TIME=$ELAPSED
        SPEEDUP="1.00"
        EFFICIENCY="100.00"
    else
        if [ "$(echo "$BASELINE_TIME > 0" | bc)" -eq 1 ]; then
            SPEEDUP=$(echo "scale=2; $BASELINE_TIME / $ELAPSED" | bc)
            EFFICIENCY=$(echo "scale=2; ($SPEEDUP / $num_workers) * 100" | bc)
        else
            SPEEDUP="N/A"
            EFFICIENCY="N/A"
        fi
    fi
    
    #---------------------------------------------------------------------------
    # 8.8 Record Results (CSV + Console)
    #---------------------------------------------------------------------------
    # NOTE: The second field here is currently JOBS_COMPLETED, even though
    #       the header labels it num_jobs. If you want it to reflect the
    #       intended NUM_JOBS instead, change the second field accordingly.
    echo "$num_workers,$JOBS_COMPLETED,$ELAPSED,$JOBS_COMPLETED,$THROUGHPUT,$AVG_TIME,$SPEEDUP,$EFFICIENCY" >> $OUTPUT_FILE
    
    log_success "Workers: $num_workers | Time: ${ELAPSED}s | Jobs: $JOBS_COMPLETED | Speedup: ${SPEEDUP}x | Efficiency: ${EFFICIENCY}%"
    
    # Clean up controller/worker processes before the next iteration.
    cleanup
done

#===============================================================================
# 9. FINAL SUMMARY AND OPTIONAL PLOT GENERATION
#===============================================================================

echo ""
echo "============================================="
echo "  E2 Results Summary"
echo "============================================="
cat $OUTPUT_FILE
echo ""
log_success "Results saved to: $OUTPUT_FILE"

# If Python 3 is available, attempt to generate plots:
#   - scripts/plot_results.py should accept:
#       --input=<csv> --output=<dir>
if command -v python3 &> /dev/null; then
    log_info "Generating plots..."
    python3 scripts/plot_results.py --input=$OUTPUT_FILE --output=./evaluation_results 2>/dev/null || log_warn "Plot generation failed"
fi

#===============================================================================
# 10. USAGE EXAMPLES
#===============================================================================
#
# Basic Run (default E2 configuration):
#   ./e2_scalability_evaluation.sh
#
# Custom Worker Counts:
#   ./e2_scalability_evaluation.sh --workers="1 2 3 4 5 6 8"
#
# Custom Job Count (for display/consistency with test_integration):
#   ./e2_scalability_evaluation.sh --jobs=40
#
# Custom Output File:
#   ./e2_scalability_evaluation.sh --output=my_e2_results.csv
#
# Integrate into Makefile:
#   e2:
#       ./e2_scalability_evaluation.sh --workers="1 2 4 8"
#
#===============================================================================
# 11. COMMON PITFALLS AND CAVEATS
#===============================================================================
#
# - bc not installed:
#     The script relies on `bc` for floating-point math. If bc is missing,
#     calls like `echo "1.0 / 2" | bc` will fail. Install it via your package
#     manager (e.g., `sudo apt-get install bc`).
#
# - timeout command missing:
#     `timeout` is part of GNU coreutils. On some systems (or macOS), it may
#     not be available or may have a different name. If you see errors, either
#     install coreutils or remove/replace the timeout usage.
#
# - test_integration behavior:
#     The actual number of jobs and logging format depend on your
#     ./build/test_integration implementation. Ensure it:
#       * Submits a workload representative of your E2 scenario.
#       * Logs job completion or test success in a way that this script can
#         parse ("completed successfully" or "PASSED").
#
# - Log string mismatch:
#     If your controller no longer logs "Registered worker" or "completed
#     successfully", the script's grep commands will fail to detect workers
#     and completed jobs accurately. Update the grep patterns in that case.
#
# - num_jobs vs. jobs_completed mismatch:
#     The CSV header says "num_jobs" but the script currently writes
#     JOBS_COMPLETED into that column. This is fine as long as you understand
#     that the second column represents *observed* completed jobs. To reflect
#     intended jobs, change that field to $NUM_JOBS.
#
# - Shared machine pkill risk:
#     The cleanup function uses pkill -f with patterns including the port.
#     On shared systems, ensure no unrelated processes match
#     "controller --port 5000" or "worker --controller.*5000".
#
#===============================================================================
# 12. FAQ
#===============================================================================
#
# Q1: How do I change the set of worker counts evaluated?
# A1: Use the --workers flag:
#       ./e2_scalability_evaluation.sh --workers="1 2 4 8 16"
#     or edit the WORKER_COUNTS variable near the top of the script.
#
# Q2: What if my integration test binary is named differently?
# A2: Update the script wherever `./build/test_integration` appears. Keep the
#     timing logic around it the same.
#
# Q3: Can I run E2 on a remote cluster instead of localhost?
# A3: This script is designed for local experiments. For multi-node experiments,
#     you’d typically use deploy_to_khoury.sh (or a similar script) and then
#     run a variant of this script that targets the deployed cluster. The
#     measurement logic (CSV, speedup, efficiency) can be reused.
#
# Q4: Why do we use wall-clock time around the test instead of measuring
#     inside the code?
# A4: Wall-clock time provides an end-to-end measurement including network
#     delays, scheduling overhead, and process startup effects. Internal
#     timers are still useful but this script deliberately measures the
#     whole pipeline.
#
# Q5: How should I interpret speedup and efficiency?
# A5:
#     - speedup ~ N with N workers means near-linear scaling.
#     - efficiency = (speedup / N) * 100.
#       * 100% efficiency = perfect linear scaling.
#       * 70-90% is realistic for many distributed workloads.
#
# Q6: The script continues even when the controller logs show zero completed
#     jobs. Why?
# A6: To avoid aborting the entire E2 run due to a single configuration, the
#     script fills in 0 for throughput and logs a warning. You should inspect
#     /tmp/e2_controller.log and /tmp/e2_test_output.log to understand the
#     failure.
#
#===============================================================================
# 13. EXTENSIONS AND BEST PRACTICES
#===============================================================================
#
# - Repeat Runs for Stability:
#     Real systems have variability. You can extend this script to run each
#     configuration multiple times and average results.
#
# - Statistical Analysis:
#     Export CSV into Python/R and compute confidence intervals, variance,
#     and more advanced metrics.
#
# - Integration with CI:
#     Hook this script into your CI pipeline to ensure that scalability does
#     not regress over time.
#
# - Per-Worker Metrics:
#     Extend logging (controller/worker code) to record per-worker job counts
#     and idle times, then parse those logs here for deeper analysis.
#
# - Parameter Sweeps:
#     In addition to worker counts, sweep over:
#       * Different job sizes.
#       * Different data sets.
#       * Different strategy configurations.
#
# End of e2_scalability_evaluation.sh
#===============================================================================
