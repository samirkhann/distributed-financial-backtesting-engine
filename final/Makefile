# Makefile for Distributed Backtesting Engine
# Wrapper around CMake with evaluation targets

BUILD_DIR = build
DATA_DIR = data/sample
RAFT_DIR = raft_data
CHECKPOINT_DIR = checkpoints
RESULTS_DIR = evaluation_results

.PHONY: all build clean test run-single run-raft benchmark help
.PHONY: eval eval-all eval-e1 eval-e2 eval-e3 eval-e4 eval-e5 eval-e6 plot

all: build

# Build the project
build:
	@echo "Building project..."
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake .. && make -j4
	@echo "Build complete!"

# Clean build artifacts
clean:
	@echo "Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@echo "Clean complete!"

# Clean everything including data and logs
clean-all: clean
	@echo "Cleaning data, logs, and checkpoints..."
	@rm -rf $(DATA_DIR) $(RAFT_DIR) $(CHECKPOINT_DIR) $(RESULTS_DIR)
	@rm -f *.log evaluation_*.csv evaluation_*.log
	@echo "Full clean complete!"

# Run tests
test: build
	@echo "Running tests..."
	@cd $(BUILD_DIR) && ctest --output-on-failure

# Generate sample data
data:
	@echo "Generating sample data..."
	@python3 scripts/generate_sample_data.py
	@echo "Sample data generated in $(DATA_DIR)"

# Run single-node system
run-single: build data
	@echo "Starting single-node system..."
	@chmod +x scripts/run_cluster.sh
	@./scripts/run_cluster.sh start

# Run Raft cluster
run-raft: build data
	@echo "Starting Raft cluster..."
	@chmod +x scripts/run_raft_cluster.sh
	@./scripts/run_raft_cluster.sh start

# Stop cluster
stop:
	@echo "Stopping cluster..."
	@pkill -f "controller --port" 2>/dev/null || true
	@pkill -f "raft_controller --id" 2>/dev/null || true
	@pkill -f "worker --controller" 2>/dev/null || true
	@echo "Cluster stopped"

# Status check
status:
	@echo "=== System Status ==="
	@echo "Controllers:"
	@ps aux | grep "[c]ontroller" | awk '{print "  PID", $$2, ":", $$11, $$12, $$13}' || echo "  None running"
	@echo "Workers:"
	@ps aux | grep "[w]orker --controller" | awk '{print "  PID", $$2}' || echo "  None running"

#######################################
# Evaluation Targets
#######################################

# Run all evaluations
eval-all: build data
	@echo "Running all evaluations..."
	@mkdir -p $(RESULTS_DIR)
	@chmod +x scripts/run_all_evaluations.sh
	@./scripts/run_all_evaluations.sh

eval: eval-all

# E1: Correctness
eval-e1: build
	@echo "=== E1: Correctness Evaluation ==="
	@cd $(BUILD_DIR) && ./test_csv_loader && ./test_sma_strategy && \
	 ./test_checkpoint && ./test_raft && ./test_integration
	@echo "E1: PASSED"

# E2: Scalability
eval-e2: build data
	@echo "=== E2: Scalability Evaluation ==="
	@mkdir -p $(RESULTS_DIR)
	@chmod +x scripts/run_scalability_evaluation.sh
	@./scripts/run_scalability_evaluation.sh --workers="1 2 4" --output=evaluation_e2_results.csv
	@cp evaluation_e2_results.csv $(RESULTS_DIR)/

# E3: Controller Failover
eval-e3: build data
	@echo "=== E3: Controller Failover Evaluation ==="
	@mkdir -p $(RESULTS_DIR)
	@chmod +x scripts/run_failover_evaluation.sh
	@./scripts/run_failover_evaluation.sh
	@cp evaluation_e3_*.log $(RESULTS_DIR)/ 2>/dev/null || true

# E4: Worker Recovery
eval-e4: build data
	@echo "=== E4: Worker Recovery Evaluation ==="
	@mkdir -p $(RESULTS_DIR)
	@chmod +x scripts/run_worker_recovery_evaluation.sh
	@./scripts/run_worker_recovery_evaluation.sh
	@cp evaluation_e4_*.log $(RESULTS_DIR)/ 2>/dev/null || true

# E5: Checkpoint Persistence
eval-e5: build
	@echo "=== E5: Checkpoint Persistence Evaluation ==="
	@cd $(BUILD_DIR) && ./test_checkpoint
	@echo "E5: PASSED"

# E6: Concurrent Jobs
eval-e6: build data
	@echo "=== E6: Concurrent Jobs Evaluation ==="
	@cd $(BUILD_DIR) && ./test_full_system
	@echo "E6: PASSED"

# Generate plots from results
plot:
	@echo "Generating plots..."
	@mkdir -p $(RESULTS_DIR)
	@python3 scripts/plot_evaluation_results.py --input=evaluation_e2_results.csv --output=$(RESULTS_DIR)
	@echo "Plots saved in $(RESULTS_DIR)/"

# Legacy benchmark target
benchmark: build data
	@echo "Running benchmark..."
	@chmod +x scripts/run_benchmark.sh
	@./scripts/run_benchmark.sh --workers=1,2,4,8 --output=benchmark_results.csv
	@echo "Benchmark complete! Results in benchmark_results.csv"

# Quick validation before deployment
validate: build data
	@echo "Running system validation..."
	@chmod +x scripts/validate_system.sh
	@./scripts/validate_system.sh

# End-to-end test
e2e: build data
	@echo "Running end-to-end test..."
	@chmod +x scripts/run_e2e_test.sh
	@./scripts/run_e2e_test.sh

# Collect all results for submission
collect-results:
	@echo "Collecting evaluation results..."
	@mkdir -p $(RESULTS_DIR)
	@cp evaluation_*.csv evaluation_*.log $(RESULTS_DIR)/ 2>/dev/null || true
	@cp *.log $(RESULTS_DIR)/ 2>/dev/null || true
	@chmod +x scripts/collect_evaluation_results.sh 2>/dev/null || true
	@./scripts/collect_evaluation_results.sh 2>/dev/null || true
	@echo "Results collected in $(RESULTS_DIR)/"

# Prepare final submission
submission: clean-all build test eval-all collect-results
	@echo "Preparing submission package..."
	@chmod +x scripts/prepare_submission.sh 2>/dev/null || true
	@./scripts/prepare_submission.sh 2>/dev/null || true
	@echo "Submission package ready!"

# Help
help:
	@echo "Distributed Backtesting Engine - Makefile"
	@echo ""
	@echo "Build & Run:"
	@echo "  make build       - Build the project"
	@echo "  make clean       - Clean build directory"
	@echo "  make clean-all   - Clean everything"
	@echo "  make test        - Run all unit tests"
	@echo "  make data        - Generate sample data"
	@echo "  make run-single  - Run single-node cluster"
	@echo "  make run-raft    - Run 3-node Raft cluster"
	@echo "  make stop        - Stop all processes"
	@echo "  make status      - Show running processes"
	@echo ""
	@echo "Evaluation:"
	@echo "  make eval-all    - Run ALL evaluations (E1-E6)"
	@echo "  make eval-e1     - E1: Correctness tests"
	@echo "  make eval-e2     - E2: Scalability tests"
	@echo "  make eval-e3     - E3: Controller failover"
	@echo "  make eval-e4     - E4: Worker recovery"
	@echo "  make eval-e5     - E5: Checkpoint persistence"
	@echo "  make eval-e6     - E6: Concurrent jobs"
	@echo "  make plot        - Generate result plots"
	@echo ""
	@echo "Utilities:"
	@echo "  make validate    - Pre-deployment validation"
	@echo "  make e2e         - End-to-end test"
	@echo "  make collect-results - Gather all results"
	@echo "  make submission  - Prepare final submission"
	@echo "  make help        - Show this message"
	@echo ""
	@echo "Quick Start:"
	@echo "  make build data eval-all plot"