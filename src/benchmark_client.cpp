/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: benchmark_client.cpp
    
    Description:
        This file implements a command-line benchmark client for performance
        testing and evaluation of the distributed backtesting system. It serves
        as the primary tool for measuring scalability metrics required for the
        project evaluation, including parallel efficiency, throughput, and
        speedup calculations.
        
        The benchmark client programmatically submits multiple backtest jobs
        to the controller cluster, measures execution times, and generates
        performance reports in CSV format for analysis and visualization.
        
    Key Responsibilities:
        1. Command-line Interface: Parse arguments for flexible configuration
        2. Job Submission: Generate and submit N backtest jobs to controller
        3. Performance Measurement: Time job execution and calculate metrics
        4. Results Collection: Gather completed results from controller
        5. Report Generation: Export metrics to CSV for plotting
        6. Scalability Testing: Support varying worker counts for speedup tests
        
    Project Evaluation Context:
        This tool is critical for demonstrating the project goals:
        - Goal: Achieve ≥0.85 parallel efficiency at 8 workers
        - Measurement: 8 workers should be ≥6.8× faster than 1 worker
        - Validation: Run benchmarks with 1, 2, 4, 8 workers
        - Deliverable: Generate speedup plots for final report
        
    Integration Points:
        - Controller: Connects via TCP to submit jobs and receive results
        - Data Files: Loads historical price data from data_dir
        - Output: Writes benchmark_results.csv for Excel/Python plotting
        - Test Suite: Used by automated testing framework
        
    Current Status:
        This is a SKELETON IMPLEMENTATION providing the command-line interface
        and configuration structure. The actual job submission and timing logic
        needs to be implemented by connecting to a running controller instance.
        
        See test_integration.cpp for examples of programmatic job submission.
        
    Design Philosophy:
        This client follows Unix philosophy:
        - Do one thing well: Benchmark the system
        - Text output: CSV for universal compatibility
        - Composability: Can be scripted for automated testing
        - Configurability: Command-line flags for all parameters
        
*******************************************************************************/

#include "controller/controller.h"
#include "common/logger.h"
#include <iostream>
#include <chrono>
#include <fstream>
#include <vector>

using namespace backtesting;

/*******************************************************************************
 * TABLE OF CONTENTS
 * =================
 * 
 * 1. OVERVIEW AND ARCHITECTURE
 *    - Purpose and context
 *    - Integration with distributed system
 *    - Benchmarking methodology
 * 
 * 2. DATA STRUCTURES
 *    - BenchmarkConfig structure
 * 
 * 3. UTILITY FUNCTIONS
 *    - print_usage() - Help text display
 * 
 * 4. MAIN FUNCTION
 *    - Command-line argument parsing
 *    - Configuration setup
 *    - Job submission workflow (to be implemented)
 * 
 * 5. IMPLEMENTATION GUIDE
 *    - How to complete the implementation
 *    - Socket connection example
 *    - Job submission protocol
 *    - Results collection
 *    - Metrics calculation
 * 
 * 6. USAGE EXAMPLES
 *    - Basic benchmarking
 *    - Scalability testing
 *    - Automated test scripts
 * 
 * 7. PERFORMANCE EVALUATION
 *    - Speedup calculation
 *    - Parallel efficiency formula
 *    - Statistical significance
 * 
 * 8. COMMON PITFALLS
 * 9. FREQUENTLY ASKED QUESTIONS
 * 
*******************************************************************************/

/*******************************************************************************
 * SECTION 1: OVERVIEW AND ARCHITECTURE
 * 
 * BENCHMARKING IN DISTRIBUTED SYSTEMS
 * ====================================
 * 
 * Purpose of This Tool:
 *   
 *   The benchmark client is the evaluation harness for the distributed
 *   backtesting system. It answers critical questions:
 *   
 *   1. Scalability: How does performance scale with worker count?
 *   2. Efficiency: Is the system efficiently utilizing resources?
 *   3. Throughput: How many jobs/second can the system handle?
 *   4. Latency: What's the end-to-end time per job?
 *   5. Reliability: Does fault tolerance impact performance?
 * 
 * Integration with Distributed System:
 *   
 *   Architecture:
 *   
 *   ┌─────────────────┐
 *   │ Benchmark Client│ ← This program
 *   └────────┬────────┘
 *            │ TCP Socket
 *            ↓
 *   ┌─────────────────┐
 *   │ Controller (Raft)│
 *   │   Leader Node   │
 *   └────────┬────────┘
 *            │ Job Distribution
 *            ↓
 *   ┌────────────────────────────┐
 *   │ Worker Pool (2-8 nodes)    │
 *   │ ┌──────┐ ┌──────┐ ┌──────┐ │
 *   │ │Worker│ │Worker│ │Worker│ │
 *   │ │  1   │ │  2   │ │  3   │ │
 *   │ └──────┘ └──────┘ └──────┘ │
 *   └────────────────────────────┘
 * 
 * Benchmarking Methodology:
 *   
 *   Standard Approach:
 *   1. Warm-up: Run 1-2 jobs to initialize system (caches, connections)
 *   2. Baseline: Measure with 1 worker (T_1)
 *   3. Scale up: Measure with 2, 4, 8 workers (T_2, T_4, T_8)
 *   4. Calculate speedup: S_n = T_1 / T_n
 *   5. Calculate efficiency: E_n = S_n / n
 *   6. Repeat: Multiple runs for statistical significance
 * 
 * Metrics Definitions:
 *   
 *   Speedup:
 *     S(n) = T(1) / T(n)
 *     Where T(n) = execution time with n workers
 *     
 *     Ideal: S(8) = 8 (perfect linear scaling)
 *     Project Goal: S(8) ≥ 6.8 (85% efficient)
 *   
 *   Parallel Efficiency:
 *     E(n) = S(n) / n = T(1) / (n × T(n))
 *     
 *     Ideal: E(n) = 1.0 (100% efficient)
 *     Project Goal: E(8) ≥ 0.85 (85% efficient)
 *   
 *   Throughput:
 *     θ(n) = num_jobs / T(n)
 *     Measured in jobs per second
 *   
 *   Latency:
 *     L = T(n) / num_jobs
 *     Average time per job
 * 
 * Why These Metrics Matter:
 *   
 *   Speedup tells us: "How much faster with more resources?"
 *   - S(8) = 8: Adding 8 workers gives 8× speedup (perfect)
 *   - S(8) = 4: Adding 8 workers only gives 4× speedup (wasteful)
 *   - S(8) = 6.8: Adding 8 workers gives 6.8× speedup (good!)
 *   
 *   Efficiency tells us: "How well are we using resources?"
 *   - E(8) = 1.0: All workers fully utilized (perfect)
 *   - E(8) = 0.5: Workers idle 50% of time (wasteful)
 *   - E(8) = 0.85: Workers utilized 85% of time (acceptable)
 *   
 *   High efficiency means:
 *   + Less wasted compute resources
 *   + Lower cloud costs (pay for what you use)
 *   + Better return on infrastructure investment
 * 
 * Expected Results (Project Goals):
 *   
 *   Workers  Time (s)  Speedup  Efficiency
 *   -------  --------  -------  ----------
 *   1        80.0      1.0      100%
 *   2        40.0      2.0      100%
 *   4        20.0      4.0      100%
 *   8        11.8      6.8      85%
 *   
 *   The 85% efficiency at 8 workers accounts for:
 *   - Job scheduling overhead
 *   - Network communication
 *   - Load imbalance (some jobs finish before others)
 *   - Raft consensus overhead
 * 
 *******************************************************************************/

/**
 * @struct BenchmarkConfig
 * @brief Configuration parameters for benchmark execution
 * 
 * This structure encapsulates all configurable parameters for the benchmark
 * client, allowing flexible testing scenarios without recompilation.
 * 
 * Design Pattern: Configuration Object
 *   Rather than passing individual parameters everywhere, we bundle related
 *   configuration into a single structure. This makes it easy to:
 *   - Add new parameters without changing function signatures
 *   - Save/load configurations from files
 *   - Pass configuration through function call chains
 * 
 * Usage Pattern:
 *   1. Create default configuration
 *   2. Override via command-line arguments
 *   3. Validate parameters
 *   4. Pass to benchmark execution functions
 * 
 * @see main() For configuration parsing
 */
struct BenchmarkConfig {
    /**
     * @var controller_host
     * @brief Hostname or IP address of the Raft controller leader
     * 
     * This is where the benchmark client will connect to submit jobs.
     * In a distributed deployment, this could be a load balancer or
     * DNS name that resolves to the current Raft leader.
     * 
     * Default: "localhost"
     * 
     * Examples:
     *   - Local testing: "localhost" or "127.0.0.1"
     *   - Khoury cluster: "kh01.khoury.neu.edu"
     *   - AWS deployment: "backtesting-controller.us-east-1.elb.amazonaws.com"
     * 
     * Security Note:
     *   In production, use TLS and authentication. This implementation
     *   assumes trusted network environment (Khoury cluster).
     */
    std::string controller_host = "localhost";
    
    /**
     * @var controller_port
     * @brief TCP port where controller accepts client connections
     * 
     * Standard port for the controller's client API. Workers use a
     * different port (typically 5001) for job submission.
     * 
     * Default: 5000
     * 
     * Port Assignment Strategy:
     *   5000: Client connections (this benchmark, web UI)
     *   5001: Worker registration and job requests
     *   5002-5010: Controller cluster Raft communication
     * 
     * Firewall Note:
     *   Ensure this port is open in firewall rules. On Khoury cluster,
     *   ports 5000-5100 should be accessible for development.
     */
    uint16_t controller_port = 5000;
    
    /**
     * @var data_dir
     * @brief Directory containing historical price data files
     * 
     * The benchmark client needs to know where data files are located
     * to include in job specifications. Workers will load data from
     * their local copies (via NFS or pre-distribution).
     * 
     * Default: "./data/sample"
     * 
     * Directory Structure:
     *   ./data/sample/
     *     ├── AAPL.csv       (Apple historical prices)
     *     ├── GOOGL.csv      (Google historical prices)
     *     ├── MSFT.csv       (Microsoft historical prices)
     *     └── ...
     * 
     * CSV Format:
     *   Date,Open,High,Low,Close,Volume
     *   2020-01-02,300.35,302.50,298.80,301.20,32500000
     *   2020-01-03,301.50,305.00,300.00,304.80,28700000
     *   ...
     * 
     * Data Acquisition:
     *   Price data can be downloaded from:
     *   - Yahoo Finance API
     *   - Alpha Vantage
     *   - Quandl
     *   - NYSE/NASDAQ historical data
     */
    std::string data_dir = "./data/sample";
    
    /**
     * @var num_jobs
     * @brief Number of backtest jobs to submit in this benchmark run
     * 
     * This controls the workload size. More jobs give better statistical
     * significance but take longer to execute.
     * 
     * Default: 10
     * 
     * Guidance for Different Test Types:
     *   
     *   Quick Sanity Check:
     *     num_jobs = 1-5 (< 1 minute)
     *     Purpose: Verify system is working
     *     
     *   Standard Benchmark:
     *     num_jobs = 10-50 (1-5 minutes)
     *     Purpose: Measure scalability
     *     
     *   Stress Test:
     *     num_jobs = 100-1000 (10-60 minutes)
     *     Purpose: Find bottlenecks, test stability
     *     
     *   Load Test:
     *     num_jobs = 1000+ (hours)
     *     Purpose: Continuous load, memory leaks, crash recovery
     * 
     * Job Distribution:
     *   Jobs are distributed across symbols in a round-robin fashion:
     *   Job 1: AAPL, Job 2: GOOGL, Job 3: MSFT, ..., Job 6: AAPL, ...
     */
    int num_jobs = 10;
    
    /**
     * @var symbols
     * @brief List of stock symbols to backtest
     * 
     * The benchmark cycles through these symbols when creating jobs.
     * This allows testing multiple securities to simulate realistic
     * workloads (users typically backtest portfolios, not single stocks).
     * 
     * Default: {"AAPL", "GOOGL", "MSFT", "AMZN", "TSLA"}
     * 
     * Symbol Selection Considerations:
     *   
     *   Diverse Market Caps:
     *     - Large cap: AAPL, MSFT (stable, high liquidity)
     *     - Mid cap: TSLA (volatile, growth)
     *     - Mix provides realistic workload
     *     
     *   Data Availability:
     *     - Ensure all symbols have data in data_dir
     *     - Check date ranges match across symbols
     *     
     *   Performance Testing:
     *     - More symbols = more variety
     *     - But jobs per symbol decreases
     *     - For 10 jobs, 5 symbols = 2 jobs each
     * 
     * Customization:
     *   Can be extended to hundreds of symbols for stress testing:
     *   --symbols-file sp500.txt (load from file)
     */
    std::vector<std::string> symbols = {"AAPL", "GOOGL", "MSFT", "AMZN", "TSLA"};
    
    /**
     * @var output_file
     * @brief Path to CSV file for benchmark results
     * 
     * All benchmark metrics are written to this file in CSV format,
     * which can be imported into Excel, Python (pandas), or R for
     * analysis and visualization.
     * 
     * Default: "benchmark_results.csv"
     * 
     * CSV Format:
     *   workers,total_time_ms,num_jobs,avg_time_per_job_ms,throughput_jobs_per_sec,speedup,efficiency
     *   1,80000,100,800.0,1.25,1.00,1.00
     *   2,40000,100,400.0,2.50,2.00,1.00
     *   4,20000,100,200.0,5.00,4.00,1.00
     *   8,11800,100,118.0,8.47,6.78,0.85
     * 
     * Columns Explained:
     *   - workers: Number of worker nodes
     *   - total_time_ms: Total execution time in milliseconds
     *   - num_jobs: Number of jobs in benchmark
     *   - avg_time_per_job_ms: Average latency per job
     *   - throughput_jobs_per_sec: Jobs completed per second
     *   - speedup: Speedup relative to 1 worker
     *   - efficiency: Parallel efficiency (speedup / workers)
     * 
     * Analysis Examples:
     *   
     *   Python (matplotlib):
     *     import pandas as pd
     *     import matplotlib.pyplot as plt
     *     
     *     df = pd.read_csv('benchmark_results.csv')
     *     plt.plot(df['workers'], df['speedup'], marker='o')
     *     plt.plot(df['workers'], df['workers'], linestyle='--', label='Ideal')
     *     plt.xlabel('Number of Workers')
     *     plt.ylabel('Speedup')
     *     plt.legend()
     *     plt.savefig('speedup_plot.png')
     *   
     *   Excel:
     *     1. Import CSV
     *     2. Create line chart: workers (X) vs speedup (Y)
     *     3. Add ideal speedup line: Y = X
     *     4. Calculate efficiency: =speedup/workers
     */
    std::string output_file = "benchmark_results.csv";
};

/*******************************************************************************
 * SECTION 2: UTILITY FUNCTIONS
 *******************************************************************************/

/**
 * @fn print_usage
 * @brief Displays command-line usage information
 * 
 * This function prints help text explaining all available command-line
 * options. It's called when user provides --help flag or when invalid
 * arguments are detected.
 * 
 * Design Pattern: Self-Documenting CLI
 *   Good command-line tools document themselves. Users should never need
 *   to read source code or external documentation to understand basic usage.
 * 
 * Best Practices Implemented:
 *   1. Show program name from argv[0]
 *   2. Group related options
 *   3. Show default values
 *   4. Provide examples (could be added)
 *   5. Exit code convention: 0 for help, 1 for errors
 * 
 * @param program Program name from argv[0]
 * 
 * @sideeffects Writes to stdout
 * 
 * @performance O(1) - Just prints text
 * 
 * Example Output:
 *   
 *   $ ./benchmark_client --help
 *   Usage: ./benchmark_client [options]
 *   Options:
 *     --controller HOST   Controller hostname (default: localhost)
 *     --port PORT         Controller port (default: 5000)
 *     --jobs N            Number of jobs to submit (default: 10)
 *     --output FILE       Output CSV file (default: benchmark_results.csv)
 *     --help              Show this help
 * 
 * Enhancement Opportunities:
 *   
 *   Add Examples Section:
 *     Examples:
 *       # Basic benchmark with 10 jobs
 *       ./benchmark_client
 *       
 *       # Benchmark with 100 jobs, custom output
 *       ./benchmark_client --jobs 100 --output results.csv
 *       
 *       # Connect to remote controller
 *       ./benchmark_client --controller kh01 --port 5000
 *   
 *   Add Version Info:
 *     Version: 1.0.0
 *     Build date: 2024-11-25
 *     Project: CS6650 Distributed Backtesting Engine
 *   
 *   Add Environment Variables:
 *     Environment:
 *       BENCHMARK_CONTROLLER - Default controller host
 *       BENCHMARK_PORT       - Default controller port
 */
void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "Options:\n"
              << "  --controller HOST   Controller hostname (default: localhost)\n"
              << "  --port PORT         Controller port (default: 5000)\n"
              << "  --jobs N            Number of jobs to submit (default: 10)\n"
              << "  --output FILE       Output CSV file (default: benchmark_results.csv)\n"
              << "  --help              Show this help\n";
}

/*******************************************************************************
 * SECTION 3: MAIN FUNCTION
 *******************************************************************************/

/**
 * @fn main
 * @brief Entry point for benchmark client
 * 
 * The main function orchestrates the benchmark workflow:
 * 1. Parse command-line arguments
 * 2. Initialize configuration
 * 3. Connect to controller
 * 4. Submit jobs
 * 5. Wait for results
 * 6. Calculate metrics
 * 7. Write CSV output
 * 
 * Current Implementation Status:
 *   This is a SKELETON providing the command-line interface. The actual
 *   job submission and timing logic needs to be implemented. See SECTION 4
 *   for implementation guidance.
 * 
 * Command-Line Argument Parsing:
 *   
 *   Uses simple iterative parsing with error checking. For more complex
 *   CLIs, consider libraries like:
 *   - GNU getopt (C standard)
 *   - boost::program_options (C++)
 *   - CLI11 (modern C++)
 *   - argparse (Python-style)
 *   
 *   Trade-offs:
 *   + Current: No dependencies, simple, sufficient for this project
 *   - Current: Manual error checking, no automatic validation
 *   + Library: Automatic validation, better error messages
 *   - Library: Additional dependency, learning curve
 * 
 * Error Handling Strategy:
 *   
 *   Exit Codes:
 *   0 = Success or --help
 *   1 = Invalid arguments
 *   2 = Connection failure
 *   3 = Benchmark execution error
 *   
 *   This follows Unix conventions and enables scripting:
 *   
 *   #!/bin/bash
 *   if ./benchmark_client --jobs 100; then
 *       echo "Benchmark succeeded"
 *       python3 plot_results.py
 *   else
 *       echo "Benchmark failed with code $?"
 *       exit 1
 *   fi
 * 
 * @param argc Argument count
 * @param argv Argument vector
 * 
 * @return 0 on success, non-zero on error
 * 
 * Example Usage:
 *   
 *   Basic (all defaults):
 *     ./benchmark_client
 *     → Connects to localhost:5000
 *     → Submits 10 jobs
 *     → Writes benchmark_results.csv
 *   
 *   Custom configuration:
 *     ./benchmark_client --controller kh01 --port 5000 --jobs 100 --output run1.csv
 *     → Connects to kh01:5000
 *     → Submits 100 jobs
 *     → Writes run1.csv
 *   
 *   Help:
 *     ./benchmark_client --help
 *     → Prints usage and exits
 * 
 * Workflow (To Be Implemented):
 *   
 *   1. SETUP PHASE
 *      - Validate configuration
 *      - Connect to controller
 *      - Check controller health
 *      - Verify data files exist
 *   
 *   2. WARMUP PHASE (Optional but recommended)
 *      - Submit 1-2 warmup jobs
 *      - Discard warmup results
 *      - Purpose: Prime caches, establish connections
 *   
 *   3. BENCHMARK PHASE
 *      - Record start time
 *      - Submit N jobs to controller
 *      - Jobs distributed to workers automatically
 *      - Poll for completion
 *      - Record end time
 *   
 *   4. RESULTS COLLECTION
 *      - Retrieve all job results
 *      - Validate: success == true for all
 *      - Extract performance metrics
 *   
 *   5. ANALYSIS PHASE
 *      - Calculate total_time = end - start
 *      - Calculate speedup (if baseline exists)
 *      - Calculate efficiency
 *      - Calculate throughput
 *   
 *   6. OUTPUT PHASE
 *      - Write CSV with metrics
 *      - Print summary to console
 *      - Return success/failure
 * 
 * @see BenchmarkConfig For configuration parameters
 * @see SECTION 4 For implementation guidance
 */
int main(int argc, char* argv[]) {
    // Initialize default configuration
    // These values can be overridden by command-line arguments
    BenchmarkConfig config;
    
    // COMMAND-LINE ARGUMENT PARSING
    // ==============================
    
    // Iterate through arguments, starting from index 1 (skip program name)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        // Help flag - print usage and exit successfully
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;  // Exit code 0 = success (help is not an error)
        }
        
        // Controller hostname
        else if (arg == "--controller" && i + 1 < argc) {
            config.controller_host = argv[++i];  // Consume next argument
        }
        
        // Controller port
        else if (arg == "--port" && i + 1 < argc) {
            config.controller_port = std::stoi(argv[++i]);
            // Note: Should add error handling for invalid numbers
            // Enhancement: Validate port range (1-65535)
        }
        
        // Number of jobs
        else if (arg == "--jobs" && i + 1 < argc) {
            config.num_jobs = std::stoi(argv[++i]);
            // Enhancement: Validate num_jobs > 0
        }
        
        // Output filename
        else if (arg == "--output" && i + 1 < argc) {
            config.output_file = argv[++i];
            // Enhancement: Validate file path is writable
        }
        
        // Unknown argument
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;  // Exit code 1 = error
        }
    }
    
    // LOGGING INITIALIZATION
    // ======================
    
    // Set log level to INFO (hide DEBUG messages, show INFO/WARNING/ERROR)
    Logger::set_level(LogLevel::INFO);
    
    // Print banner
    Logger::info("=== Benchmark Client ===");
    
    // CONFIGURATION DISPLAY
    // =====================
    
    // Print configuration for user verification
    // This helps catch mistakes (e.g., wrong controller address)
    std::cout << "Benchmark configuration:\n"
              << "  Controller: " << config.controller_host << ":" << config.controller_port << "\n"
              << "  Jobs to submit: " << config.num_jobs << "\n"
              << "  Output: " << config.output_file << "\n";
    
    // TODO: Add symbols list to output
    // std::cout << "  Symbols: ";
    // for (const auto& sym : config.symbols) std::cout << sym << " ";
    // std::cout << "\n";
    
    // IMPLEMENTATION PLACEHOLDER
    // ==========================
    
    // NOTE: This is where the actual implementation would go
    // The following code would:
    // 1. Connect to controller via socket
    // 2. Submit N jobs with different parameters
    // 3. Wait for all results
    // 4. Calculate metrics
    // 5. Write to output file
    //
    // See SECTION 4 below for detailed implementation guidance
    
    std::cout << "\nNote: For actual benchmarking, integrate with running controller instance\n";
    std::cout << "See test_integration.cpp for example of programmatic job submission\n";
    
    // Placeholder success return
    // In full implementation, return non-zero if benchmark fails
    return 0;
}

/*******************************************************************************
 * SECTION 4: IMPLEMENTATION GUIDE
 * 
 * This section provides detailed guidance for completing the benchmark client
 * implementation. It includes example code, protocol details, and best practices.
 * 
 *******************************************************************************/

/*
 * COMPLETE IMPLEMENTATION EXAMPLE
 * ================================
 * 
 * Here's how to implement the full benchmark workflow:
 * 
 * Code Structure:
 * 
 * int main(int argc, char* argv[]) {
 *     // ... (argument parsing as above)
 *     
 *     // STEP 1: Connect to controller
 *     // ==============================
 *     
 *     int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
 *     if (socket_fd < 0) {
 *         std::cerr << "Failed to create socket\n";
 *         return 2;
 *     }
 *     
 *     struct sockaddr_in server_addr;
 *     server_addr.sin_family = AF_INET;
 *     server_addr.sin_port = htons(config.controller_port);
 *     
 *     // Resolve hostname to IP
 *     struct hostent* host = gethostbyname(config.controller_host.c_str());
 *     if (!host) {
 *         std::cerr << "Failed to resolve hostname: " << config.controller_host << "\n";
 *         return 2;
 *     }
 *     
 *     memcpy(&server_addr.sin_addr, host->h_addr, host->h_length);
 *     
 *     // Connect
 *     if (connect(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
 *         std::cerr << "Failed to connect to controller\n";
 *         return 2;
 *     }
 *     
 *     Logger::info("Connected to controller at " + config.controller_host + 
 *                 ":" + std::to_string(config.controller_port));
 *     
 *     // STEP 2: Submit jobs
 *     // ===================
 *     
 *     std::vector<uint64_t> job_ids;
 *     auto start_time = std::chrono::steady_clock::now();
 *     
 *     for (int i = 0; i < config.num_jobs; ++i) {
 *         // Create job specification
 *         JobParams params;
 *         params.symbol = config.symbols[i % config.symbols.size()];
 *         params.strategy_type = StrategyType::SMA;
 *         params.short_window = 50;
 *         params.long_window = 200;
 *         params.initial_capital = 100000.0;
 *         params.start_date = "2020-01-01";
 *         params.end_date = "2024-12-31";
 *         
 *         // Serialize job request
 *         auto job_msg = serialize_job_request(params);
 *         
 *         // Send to controller
 *         ssize_t sent = send(socket_fd, job_msg.data(), job_msg.size(), 0);
 *         if (sent != static_cast<ssize_t>(job_msg.size())) {
 *             std::cerr << "Failed to send job " << i << "\n";
 *             close(socket_fd);
 *             return 3;
 *         }
 *         
 *         // Receive job ID
 *         uint64_t job_id;
 *         ssize_t received = recv(socket_fd, &job_id, sizeof(job_id), 0);
 *         if (received != sizeof(job_id)) {
 *             std::cerr << "Failed to receive job ID\n";
 *             close(socket_fd);
 *             return 3;
 *         }
 *         
 *         job_ids.push_back(job_id);
 *         
 *         if ((i + 1) % 10 == 0) {
 *             Logger::info("Submitted " + std::to_string(i + 1) + "/" + 
 *                         std::to_string(config.num_jobs) + " jobs");
 *         }
 *     }
 *     
 *     Logger::info("All jobs submitted, waiting for results...");
 *     
 *     // STEP 3: Wait for results
 *     // ========================
 *     
 *     std::vector<JobResult> results;
 *     int completed = 0;
 *     
 *     while (completed < config.num_jobs) {
 *         // Poll for result
 *         uint8_t buffer[8192];
 *         ssize_t received = recv(socket_fd, buffer, sizeof(buffer), 0);
 *         
 *         if (received <= 0) {
 *             std::cerr << "Connection lost while waiting for results\n";
 *             close(socket_fd);
 *             return 3;
 *         }
 *         
 *         // Deserialize result
 *         JobResult result = deserialize_job_result(buffer, received);
 *         
 *         if (!result.success) {
 *             std::cerr << "Job " << result.job_id << " failed: " 
 *                      << result.error_message << "\n";
 *         }
 *         
 *         results.push_back(result);
 *         completed++;
 *         
 *         if (completed % 10 == 0) {
 *             Logger::info("Received " + std::to_string(completed) + "/" +
 *                         std::to_string(config.num_jobs) + " results");
 *         }
 *     }
 *     
 *     auto end_time = std::chrono::steady_clock::now();
 *     auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
 *         end_time - start_time
 *     );
 *     
 *     close(socket_fd);
 *     
 *     Logger::info("All results received");
 *     
 *     // STEP 4: Calculate metrics
 *     // =========================
 *     
 *     double total_time_ms = duration.count();
 *     double avg_time_per_job_ms = total_time_ms / config.num_jobs;
 *     double throughput = config.num_jobs / (total_time_ms / 1000.0);
 *     
 *     // For speedup calculation, you'd need a baseline from 1-worker run
 *     // This could be:
 *     // - Read from previous benchmark_results.csv
 *     // - Hard-coded baseline
 *     // - Calculated if this IS the 1-worker run
 *     
 *     // Example: Read baseline from previous run
 *     double baseline_time_ms = 80000.0;  // Would read from CSV
 *     double speedup = baseline_time_ms / total_time_ms;
 *     
 *     // Assuming we know worker count (would be passed via --workers flag)
 *     int num_workers = 8;  // Would come from config
 *     double efficiency = speedup / num_workers;
 *     
 *     // STEP 5: Write CSV
 *     // =================
 *     
 *     std::ofstream out(config.output_file, std::ios::app);  // Append mode
 *     if (!out) {
 *         std::cerr << "Failed to open output file: " << config.output_file << "\n";
 *         return 3;
 *     }
 *     
 *     // Write header if file is empty
 *     out.seekp(0, std::ios::end);
 *     if (out.tellp() == 0) {
 *         out << "workers,total_time_ms,num_jobs,avg_time_per_job_ms,"
 *             << "throughput_jobs_per_sec,speedup,efficiency\n";
 *     }
 *     
 *     // Write data
 *     out << num_workers << ","
 *         << total_time_ms << ","
 *         << config.num_jobs << ","
 *         << avg_time_per_job_ms << ","
 *         << throughput << ","
 *         << speedup << ","
 *         << efficiency << "\n";
 *     
 *     out.close();
 *     
 *     // STEP 6: Print summary
 *     // =====================
 *     
 *     std::cout << "\n=== Benchmark Results ===\n";
 *     std::cout << "Workers:              " << num_workers << "\n";
 *     std::cout << "Total time:           " << total_time_ms << " ms\n";
 *     std::cout << "Jobs completed:       " << config.num_jobs << "\n";
 *     std::cout << "Avg time per job:     " << avg_time_per_job_ms << " ms\n";
 *     std::cout << "Throughput:           " << throughput << " jobs/sec\n";
 *     std::cout << "Speedup:              " << speedup << "x\n";
 *     std::cout << "Parallel efficiency:  " << (efficiency * 100) << "%\n";
 *     std::cout << "\nResults written to: " << config.output_file << "\n";
 *     
 *     // Check if we met project goals
 *     if (num_workers == 8 && efficiency >= 0.85) {
 *         std::cout << "\n✓ Project goal achieved: E(8) >= 0.85\n";
 *     } else if (num_workers == 8) {
 *         std::cout << "\n✗ Project goal not met: E(8) = " << efficiency 
 *                  << " < 0.85\n";
 *     }
 *     
 *     return 0;
 * }
 * 
 * 
 * JOB SUBMISSION PROTOCOL
 * =======================
 * 
 * The controller expects messages in this format:
 * 
 * Job Request Message:
 *   [4 bytes] message_type = 0x01 (SUBMIT_JOB)
 *   [8 bytes] client_id
 *   [variable] job_params (serialized)
 * 
 * job_params serialization:
 *   [4 bytes] symbol_length
 *   [N bytes] symbol string
 *   [1 byte]  strategy_type (0=SMA, 1=RSI, 2=MACD)
 *   [4 bytes] short_window
 *   [4 bytes] long_window
 *   [8 bytes] initial_capital (double)
 *   [4 bytes] start_date_length
 *   [N bytes] start_date string
 *   [4 bytes] end_date_length
 *   [N bytes] end_date string
 * 
 * Job Response Message:
 *   [4 bytes] message_type = 0x02 (JOB_ACCEPTED)
 *   [8 bytes] job_id
 * 
 * Result Message:
 *   [4 bytes] message_type = 0x03 (JOB_RESULT)
 *   [8 bytes] job_id
 *   [1 byte]  success (0/1)
 *   [8 bytes] final_portfolio_value
 *   [8 bytes] total_return
 *   [8 bytes] sharpe_ratio
 *   [8 bytes] max_drawdown
 *   [4 bytes] num_trades
 *   [4 bytes] winning_trades
 *   [4 bytes] losing_trades
 *   [4 bytes] error_message_length
 *   [N bytes] error_message (if length > 0)
 * 
 * See common/message.h for serialization helpers.
 * 
 * 
 * RESULTS COLLECTION STRATEGIES
 * ==============================
 * 
 * Option 1: Blocking Wait (Simplest)
 *   
 *   Submit all jobs, then wait for all results
 *   
 *   Pros:
 *   + Simple implementation
 *   + Easy to measure total time
 *   
 *   Cons:
 *   - No progress updates
 *   - Can't detect partial failures
 *   
 *   Best for: Quick benchmarks (< 100 jobs)
 * 
 * Option 2: Polling (Recommended)
 *   
 *   Submit jobs, periodically poll for results
 *   
 *   Pros:
 *   + Progress updates
 *   + Can detect slow jobs
 *   + Can abort if too many failures
 *   
 *   Cons:
 *   - Slightly more complex
 *   - Need timeout logic
 *   
 *   Best for: Production benchmarks
 * 
 * Option 3: Async/Callback
 *   
 *   Use asynchronous I/O or callback functions
 *   
 *   Pros:
 *   + Non-blocking
 *   + Efficient for many concurrent jobs
 *   
 *   Cons:
 *   - Most complex
 *   - Requires threading or async library
 *   
 *   Best for: Stress testing (1000+ jobs)
 * 
 * 
 * METRICS CALCULATION
 * ===================
 * 
 * Speedup:
 *   
 *   Definition: How much faster with n workers vs 1 worker
 *   
 *   Formula:
 *     S(n) = T(1) / T(n)
 *   
 *   Where:
 *     T(1) = baseline time with 1 worker
 *     T(n) = time with n workers
 *   
 *   Example:
 *     T(1) = 80 seconds
 *     T(8) = 12 seconds
 *     S(8) = 80 / 12 = 6.67x
 *   
 *   Implementation:
 *     // Read baseline from previous run
 *     double baseline = read_baseline_from_csv(config.output_file);
 *     double speedup = baseline / total_time_ms;
 * 
 * Efficiency:
 *   
 *   Definition: How well resources are utilized
 *   
 *   Formula:
 *     E(n) = S(n) / n = T(1) / (n × T(n))
 *   
 *   Where:
 *     S(n) = speedup
 *     n = number of workers
 *   
 *   Example:
 *     S(8) = 6.67
 *     E(8) = 6.67 / 8 = 0.83 = 83%
 *   
 *   Implementation:
 *     double efficiency = speedup / num_workers;
 * 
 * Throughput:
 *   
 *   Definition: Jobs completed per unit time
 *   
 *   Formula:
 *     θ = N / T
 *   
 *   Where:
 *     N = number of jobs
 *     T = total time (seconds)
 *   
 *   Example:
 *     N = 100 jobs
 *     T = 12 seconds
 *     θ = 100 / 12 = 8.33 jobs/sec
 *   
 *   Implementation:
 *     double throughput = config.num_jobs / (total_time_ms / 1000.0);
 * 
 * Latency:
 *   
 *   Definition: Average time per job
 *   
 *   Formula:
 *     L = T / N
 *   
 *   Where:
 *     T = total time
 *     N = number of jobs
 *   
 *   Example:
 *     T = 12 seconds
 *     N = 100 jobs
 *     L = 12 / 100 = 0.12 seconds per job
 *   
 *   Implementation:
 *     double latency_ms = total_time_ms / config.num_jobs;
 * 
 * Statistical Significance:
 *   
 *   Single runs can have variance due to:
 *   - Network jitter
 *   - CPU scheduling
 *   - Cache effects
 *   - Background processes
 *   
 *   Best Practice: Run multiple times and report:
 *   - Mean (average)
 *   - Standard deviation (variability)
 *   - Confidence interval (95% CI)
 *   
 *   Example:
 *     Run 5 times with 8 workers:
 *     Times: [11.8, 12.1, 11.9, 12.0, 11.7] seconds
 *     Mean: 11.9 seconds
 *     Std Dev: 0.15 seconds
 *     95% CI: 11.9 ± 0.19 seconds
 *   
 *   Implementation:
 *     for (int run = 0; run < 5; ++run) {
 *         auto time = run_benchmark(config);
 *         times.push_back(time);
 *     }
 *     double mean = calculate_mean(times);
 *     double std_dev = calculate_std_dev(times);
 */

/*******************************************************************************
 * SECTION 5: USAGE EXAMPLES
 *******************************************************************************/

/*
 * EXAMPLE 1: Basic Single Run
 * ----------------------------
 * 
 * Run benchmark with default configuration:
 * 
 * Command:
 *   $ ./benchmark_client
 * 
 * Output:
 *   === Benchmark Client ===
 *   Benchmark configuration:
 *     Controller: localhost:5000
 *     Jobs to submit: 10
 *     Output: benchmark_results.csv
 *   
 *   Connected to controller
 *   Submitted 10/10 jobs
 *   Received 10/10 results
 *   
 *   === Benchmark Results ===
 *   Total time: 1250 ms
 *   Jobs completed: 10
 *   Throughput: 8.0 jobs/sec
 *   
 *   Results written to: benchmark_results.csv
 * 
 * CSV Output:
 *   workers,total_time_ms,num_jobs,avg_time_per_job_ms,throughput_jobs_per_sec
 *   1,1250,10,125.0,8.0
 * 
 * 
 * EXAMPLE 2: Scalability Testing
 * -------------------------------
 * 
 * Run benchmarks with different worker counts to measure speedup:
 * 
 * Script (benchmark_scaling.sh):
 *   #!/bin/bash
 *   
 *   # Clear previous results
 *   rm -f scaling_results.csv
 *   
 *   # Test with 1, 2, 4, 8 workers
 *   for workers in 1 2 4 8; do
 *       echo "Testing with $workers workers..."
 *       
 *       # Reconfigure cluster with $workers workers
 *       ./configure_cluster.sh $workers
 *       
 *       # Wait for workers to start
 *       sleep 5
 *       
 *       # Run benchmark
 *       ./benchmark_client \
 *           --controller kh01 \
 *           --jobs 100 \
 *           --output scaling_results.csv
 *       
 *       echo "Completed $workers workers"
 *       echo "---"
 *   done
 *   
 *   echo "Generating plots..."
 *   python3 plot_scalability.py scaling_results.csv
 * 
 * Expected Results:
 *   Testing with 1 workers...
 *   Total time: 80000 ms
 *   Speedup: 1.00x
 *   Efficiency: 100%
 *   ---
 *   Testing with 2 workers...
 *   Total time: 40000 ms
 *   Speedup: 2.00x
 *   Efficiency: 100%
 *   ---
 *   Testing with 4 workers...
 *   Total time: 20000 ms
 *   Speedup: 4.00x
 *   Efficiency: 100%
 *   ---
 *   Testing with 8 workers...
 *   Total time: 11800 ms
 *   Speedup: 6.78x
 *   Efficiency: 85%
 *   ✓ Project goal achieved!
 *   ---
 * 
 * 
 * EXAMPLE 3: Stress Testing
 * --------------------------
 * 
 * Test system stability with large workload:
 * 
 * Command:
 *   $ ./benchmark_client --jobs 1000 --output stress_test.csv
 * 
 * Monitoring:
 *   # In separate terminal, watch system metrics
 *   $ watch -n 1 'ps aux | grep -E "(controller|worker)"'
 *   
 *   # Watch network traffic
 *   $ iftop -i eth0
 *   
 *   # Watch disk I/O (for checkpoints)
 *   $ iostat -x 1
 * 
 * What to Look For:
 *   - Memory leaks: Worker memory usage should be stable
 *   - CPU utilization: Should be high (80-100%) during execution
 *   - Network saturation: Should not exceed network capacity
 *   - Checkpoint I/O: Should not bottleneck
 *   - Error rate: Should be 0% (all jobs succeed)
 * 
 * Red Flags:
 *   ✗ Memory usage growing linearly with jobs (leak!)
 *   ✗ CPU usage < 50% (not enough work, load imbalance)
 *   ✗ Network packet drops (congestion)
 *   ✗ Jobs failing (bugs in strategy, checkpointing)
 *   ✗ Throughput decreasing over time (degradation)
 * 
 * 
 * EXAMPLE 4: Automated Regression Testing
 * ----------------------------------------
 * 
 * Integrate benchmark into CI/CD pipeline:
 * 
 * .github/workflows/benchmark.yml:
 *   name: Performance Benchmark
 *   
 *   on:
 *     push:
 *       branches: [main]
 *   
 *   jobs:
 *     benchmark:
 *       runs-on: ubuntu-latest
 *       
 *       steps:
 *       - uses: actions/checkout@v2
 *       
 *       - name: Build project
 *         run: |
 *           mkdir build && cd build
 *           cmake ..
 *           make -j4
 *       
 *       - name: Start cluster
 *         run: |
 *           ./scripts/start_cluster.sh
 *           sleep 10
 *       
 *       - name: Run benchmark
 *         run: |
 *           ./build/benchmark_client \
 *             --controller localhost \
 *             --jobs 50 \
 *             --output benchmark.csv
 *       
 *       - name: Check performance regression
 *         run: |
 *           python3 scripts/check_regression.py \
 *             --current benchmark.csv \
 *             --baseline baseline.csv \
 *             --threshold 0.90
 *       
 *       - name: Upload results
 *         uses: actions/upload-artifact@v2
 *         with:
 *           name: benchmark-results
 *           path: benchmark.csv
 * 
 * scripts/check_regression.py:
 *   import pandas as pd
 *   import sys
 *   
 *   current = pd.read_csv('benchmark.csv')
 *   baseline = pd.read_csv('baseline.csv')
 *   
 *   # Check if performance dropped more than 10%
 *   current_throughput = current['throughput_jobs_per_sec'].iloc[-1]
 *   baseline_throughput = baseline['throughput_jobs_per_sec'].iloc[-1]
 *   
 *   ratio = current_throughput / baseline_throughput
 *   
 *   if ratio < 0.90:
 *       print(f"Performance regression detected!")
 *       print(f"   Current: {current_throughput:.2f} jobs/sec")
 *       print(f"   Baseline: {baseline_throughput:.2f} jobs/sec")
 *       print(f"   Ratio: {ratio:.2%}")
 *       sys.exit(1)
 *   else:
 *       print(f"Performance acceptable")
 *       print(f"   Current: {current_throughput:.2f} jobs/sec")
 *       print(f"   Baseline: {baseline_throughput:.2f} jobs/sec")
 *       print(f"   Ratio: {ratio:.2%}")
 *       sys.exit(0)
 * 
 * 
 * EXAMPLE 5: Generating Report Plots
 * -----------------------------------
 * 
 * Create visualizations for final project report:
 * 
 * plot_results.py:
 *   import pandas as pd
 *   import matplotlib.pyplot as plt
 *   
 *   # Read benchmark data
 *   df = pd.read_csv('benchmark_results.csv')
 *   
 *   # Create figure with subplots
 *   fig, axes = plt.subplots(2, 2, figsize=(12, 10))
 *   
 *   # Plot 1: Speedup
 *   axes[0, 0].plot(df['workers'], df['speedup'], marker='o', label='Actual')
 *   axes[0, 0].plot(df['workers'], df['workers'], '--', label='Ideal')
 *   axes[0, 0].set_xlabel('Number of Workers')
 *   axes[0, 0].set_ylabel('Speedup')
 *   axes[0, 0].set_title('Speedup vs Workers')
 *   axes[0, 0].legend()
 *   axes[0, 0].grid(True)
 *   
 *   # Plot 2: Efficiency
 *   axes[0, 1].plot(df['workers'], df['efficiency'], marker='o')
 *   axes[0, 1].axhline(y=0.85, color='r', linestyle='--', label='Goal (85%)')
 *   axes[0, 1].set_xlabel('Number of Workers')
 *   axes[0, 1].set_ylabel('Parallel Efficiency')
 *   axes[0, 1].set_title('Efficiency vs Workers')
 *   axes[0, 1].legend()
 *   axes[0, 1].grid(True)
 *   
 *   # Plot 3: Throughput
 *   axes[1, 0].plot(df['workers'], df['throughput_jobs_per_sec'], marker='o')
 *   axes[1, 0].set_xlabel('Number of Workers')
 *   axes[1, 0].set_ylabel('Throughput (jobs/sec)')
 *   axes[1, 0].set_title('Throughput vs Workers')
 *   axes[1, 0].grid(True)
 *   
 *   # Plot 4: Execution Time
 *   axes[1, 1].plot(df['workers'], df['total_time_ms'] / 1000, marker='o')
 *   axes[1, 1].set_xlabel('Number of Workers')
 *   axes[1, 1].set_ylabel('Execution Time (seconds)')
 *   axes[1, 1].set_title('Execution Time vs Workers')
 *   axes[1, 1].grid(True)
 *   
 *   plt.tight_layout()
 *   plt.savefig('benchmark_results.png', dpi=300)
 *   print('Plots saved to benchmark_results.png')
 * 
 * Usage:
 *   $ python3 plot_results.py
 *   Plots saved to benchmark_results.png
 * 
 * Include in Report:
 *   - Add benchmark_results.png to final report
 *   - Discuss speedup curve shape
 *   - Explain why efficiency < 100% at 8 workers
 *   - Compare to theoretical maximum
 */

/*******************************************************************************
 * SECTION 6: PERFORMANCE EVALUATION
 *******************************************************************************/

/*
 * SPEEDUP CALCULATION
 * ===================
 * 
 * Theoretical Background:
 *   
 *   Amdahl's Law:
 *     S(n) = 1 / (P + (1-P)/n)
 *     
 *     Where:
 *       S(n) = speedup with n processors
 *       P = fraction of serial code
 *       (1-P) = fraction of parallel code
 *   
 *   For our system:
 *     - Parallel: Strategy execution (99%)
 *     - Serial: Job scheduling, Raft consensus (1%)
 *     
 *     S(8) = 1 / (0.01 + 0.99/8) = 1 / (0.01 + 0.124) = 7.46
 *   
 *   Our goal of S(8) = 6.8 is realistic given:
 *     - Network overhead
 *     - Load imbalance
 *     - Synchronization costs
 * 
 * Measurement Procedure:
 *   
 *   1. Establish Baseline (1 Worker):
 *      - Run benchmark with 1 worker, 100 jobs
 *      - Record time: T_1 = 80 seconds
 *      - This is your baseline for speedup calculations
 *   
 *   2. Scale to 2 Workers:
 *      - Run same workload (100 jobs)
 *      - Record time: T_2 = 40 seconds
 *      - Calculate speedup: S_2 = T_1 / T_2 = 80/40 = 2.0
 *   
 *   3. Scale to 4 Workers:
 *      - Run same workload
 *      - Record time: T_4 = 20 seconds
 *      - Calculate speedup: S_4 = T_1 / T_4 = 80/20 = 4.0
 *   
 *   4. Scale to 8 Workers:
 *      - Run same workload
 *      - Record time: T_8 = 11.8 seconds
 *      - Calculate speedup: S_8 = T_1 / T_8 = 80/11.8 = 6.78
 * 
 * Interpreting Results:
 *   
 *   Perfect Scaling (Rarely Achieved):
 *     Workers: 1    2    4    8
 *     Speedup: 1.0  2.0  4.0  8.0
 *     
 *     This means zero overhead - impossible in practice
 *   
 *   Good Scaling (Project Goal):
 *     Workers: 1    2    4    8
 *     Speedup: 1.0  2.0  3.9  6.8
 *     
 *     85% efficiency at 8 workers - achievable with good design
 *   
 *   Poor Scaling (Needs Improvement):
 *     Workers: 1    2    4    8
 *     Speedup: 1.0  1.8  3.0  4.5
 *     
 *     56% efficiency at 8 workers - significant overhead
 *   
 *   Sublinear Scaling (Problem):
 *     Workers: 1    2    4    8
 *     Speedup: 1.0  1.5  2.5  3.5
 *     
 *     44% efficiency - investigate bottlenecks
 * 
 * Common Bottlenecks:
 *   
 *   1. Serial Section Too Large:
 *      Symptom: Speedup plateaus early
 *      Cause: Controller becomes bottleneck
 *      Solution: Optimize job scheduling, use batching
 *   
 *   2. Network Saturation:
 *      Symptom: Speedup degrades at high worker count
 *      Cause: Network bandwidth exhausted
 *      Solution: Reduce message sizes, batch results
 *   
 *   3. Load Imbalance:
 *      Symptom: Some workers finish early and idle
 *      Cause: Unequal job sizes
 *      Solution: Dynamic load balancing, work stealing
 *   
 *   4. Synchronization Overhead:
 *      Symptom: Throughput doesn't scale linearly
 *      Cause: Excessive locking, Raft coordination
 *      Solution: Reduce critical sections, async operations
 * 
 * 
 * PARALLEL EFFICIENCY FORMULA
 * ===========================
 * 
 * Definition:
 *   E(n) = S(n) / n
 *   
 *   Measures how effectively workers are utilized
 *   Range: 0 to 1 (or 0% to 100%)
 * 
 * Interpretation:
 *   
 *   E = 1.0 (100%):
 *     Perfect efficiency - all workers fully utilized
 *     Example: 8 workers give 8× speedup
 *     
 *   E = 0.85 (85%):
 *     Good efficiency - minor overhead
 *     Example: 8 workers give 6.8× speedup
 *     
 *   E = 0.50 (50%):
 *     Poor efficiency - half the resources wasted
 *     Example: 8 workers give 4× speedup
 *     
 *   E < 0.50:
 *     Very poor - more workers actually hurt
 *     Consider reducing worker count
 * 
 * Why Efficiency Matters:
 *   
 *   Cost Analysis:
 *     - 8 workers @ $1/hour each = $8/hour
 *     - If E = 1.0: Getting 8x work for 8x cost (fair)
 *     - If E = 0.85: Getting 6.8x work for 8x cost (acceptable)
 *     - If E = 0.50: Getting 4x work for 8x cost (wasteful!)
 *   
 *   Cloud Economics:
 *     AWS c5.xlarge: $0.17/hour
 *     8 instances: $1.36/hour
 *     
 *     At E = 0.85:
 *       Effective cost: $1.36 / 6.8 = $0.20 per "1x" work
 *       Still profitable
 *     
 *     At E = 0.50:
 *       Effective cost: $1.36 / 4 = $0.34 per "1x" work
 *       Better to use 4 workers at higher efficiency
 * 
 * Efficiency vs Worker Count:
 *   
 *   Typically, efficiency decreases as workers increase:
 *   
 *   Workers  Speedup  Efficiency
 *   -------  -------  ----------
 *   1        1.0      100%
 *   2        2.0      100%
 *   4        3.9      98%
 *   8        6.8      85%
 *   16       10.2     64%
 *   32       14.4     45%
 *   
 *   Sweet spot: 8 workers (85% efficiency)
 *   Beyond 16: Diminishing returns
 * 
 * 
 * STATISTICAL SIGNIFICANCE
 * ========================
 * 
 * Why Multiple Runs Matter:
 *   
 *   Single run time: 11.8 seconds
 *   
 *   Is this representative? Consider sources of variance:
 *   - Network latency fluctuations
 *   - CPU scheduling variations
 *   - Cache effects (hot vs cold)
 *   - Background processes
 *   - I/O contention
 *   
 *   Solution: Run multiple times, calculate statistics
 * 
 * Statistical Measures:
 *   
 *   Mean (Average):
 *     μ = Σ(x_i) / n
 *     
 *     Example: [11.8, 12.1, 11.9, 12.0, 11.7]
 *     μ = (11.8 + 12.1 + 11.9 + 12.0 + 11.7) / 5 = 11.9 seconds
 *   
 *   Standard Deviation:
 *     σ = sqrt(Σ(x_i - μ)² / (n-1))
 *     
 *     Measures spread of data
 *     Low σ = consistent, high σ = variable
 *     
 *     Example: σ = 0.15 seconds
 *   
 *   Coefficient of Variation:
 *     CV = σ / μ
 *     
 *     Relative variability (%)
 *     
 *     Example: CV = 0.15 / 11.9 = 0.013 = 1.3%
 *     
 *     Good: CV < 5%
 *     Acceptable: CV < 10%
 *     Poor: CV > 10% (investigate cause)
 *   
 *   Confidence Interval (95%):
 *     CI = μ ± 1.96 * (σ / sqrt(n))
 *     
 *     Range where true mean likely falls
 *     
 *     Example (n=5):
 *       CI = 11.9 ± 1.96 * (0.15 / sqrt(5))
 *          = 11.9 ± 0.13
 *          = [11.77, 12.03]
 *     
 *     Interpretation: "We're 95% confident the true mean
 *     is between 11.77 and 12.03 seconds"
 * 
 * Recommended Practice:
 *   
 *   1. Run each configuration 5 times minimum
 *   2. Discard first run (warmup)
 *   3. Calculate mean and std dev
 *   4. Report: "11.9 ± 0.15 seconds (mean ± std dev, n=5)"
 *   5. Check CV < 10% (if higher, investigate)
 *   6. Use mean for speedup calculations
 * 
 * Reporting in Final Report:
 *   
 *   Table Format:
 *     Workers  Mean (s)  Std Dev  CV     Speedup  Efficiency
 *     -------  --------  -------  -----  -------  ----------
 *     1        80.2      1.2      1.5%   1.00     100%
 *     2        40.1      0.8      2.0%   2.00     100%
 *     4        20.3      0.6      3.0%   3.95     99%
 *     8        11.9      0.5      4.2%   6.74     84%
 *   
 *   This shows:
 *   - Consistent results (CV < 5%)
 *   - Clear speedup trend
 *   - Meets project goals
 */

/*******************************************************************************
 * SECTION 7: COMMON PITFALLS
 *******************************************************************************/

/*
 * PITFALL 1: Not Establishing Baseline
 * -------------------------------------
 * 
 * Problem:
 *   Running with 8 workers first, no 1-worker baseline
 * 
 * Wrong Approach:
 *   $ ./benchmark_client --workers 8
 *   Total time: 12 seconds
 *   
 *   # Can't calculate speedup - no baseline!
 * 
 * Correct Approach:
 *   # First, establish baseline
 *   $ ./benchmark_client --workers 1 --output baseline.csv
 *   Total time: 80 seconds
 *   
 *   # Then test scalability
 *   $ ./benchmark_client --workers 8 --output results.csv
 *   Total time: 12 seconds
 *   Speedup: 6.67x (relative to baseline)
 * 
 * 
 * PITFALL 2: Inconsistent Workload
 * ---------------------------------
 * 
 * Problem:
 *   Using different num_jobs for different worker counts
 * 
 * Wrong:
 *   1 worker:  --jobs 10
 *   8 workers: --jobs 100
 *   
 *   Can't compare - different workloads!
 * 
 * Correct:
 *   1 worker:  --jobs 100
 *   8 workers: --jobs 100
 *   
 *   Same workload, valid comparison
 * 
 * 
 * PITFALL 3: Cold Start Effects
 * ------------------------------
 * 
 * Problem:
 *   First run is slower due to cold caches
 * 
 * Example:
 *   Run 1: 15 seconds (cold start)
 *   Run 2: 12 seconds (warm)
 *   Run 3: 12 seconds (warm)
 *   
 *   If you only do one run, results are skewed
 * 
 * Solution:
 *   # Warmup run (discard results)
 *   $ ./benchmark_client --jobs 5
 *   
 *   # Actual benchmark runs
 *   $ ./benchmark_client --jobs 100  # Use this
 * 
 * 
 * PITFALL 4: Background Load
 * ---------------------------
 * 
 * Problem:
 *   Running benchmark while other processes compete for resources
 * 
 * Check:
 *   $ top
 *   # Look for other CPU-intensive processes
 *   
 *   $ iostat
 *   # Check for disk I/O contention
 * 
 * Solution:
 *   - Stop unnecessary services
 *   - Run during off-peak hours
 *   - Use dedicated benchmark cluster
 *   - Nice other processes: nice -n 19 other_process
 * 
 * 
 * PITFALL 5: Network Effects
 * ---------------------------
 * 
 * Problem:
 *   Network congestion causing false bottlenecks
 * 
 * Symptoms:
 *   - Performance degrades at high worker counts
 *   - Packet loss in iftop
 *   - High retransmit rates
 * 
 * Check:
 *   $ iftop -i eth0
 *   $ netstat -s | grep retrans
 * 
 * Solution:
 *   - Use dedicated network for cluster
 *   - Reduce message sizes
 *   - Batch results
 * 
 * 
 * PITFALL 6: Wrong Metrics
 * -------------------------
 * 
 * Problem:
 *   Using wall-clock time instead of CPU time
 * 
 * Wrong:
 *   # Includes time waiting for I/O, network
 *   total_time = wall_clock_end - wall_clock_start
 * 
 * Correct:
 *   # For distributed systems, wall-clock IS correct
 *   # User cares about end-to-end latency
 *   total_time = wall_clock_end - wall_clock_start
 * 
 * Note: For single-machine, CPU time matters. For distributed, wall-clock.
 * 
 * 
 * PITFALL 7: Not Checking for Errors
 * -----------------------------------
 * 
 * Problem:
 *   Some jobs failed but benchmark reports success
 * 
 * Wrong:
 *   # Didn't check job results
 *   auto results = collect_results();
 *   // Assume all succeeded
 * 
 * Correct:
 *   auto results = collect_results();
 *   int failed = 0;
 *   for (const auto& r : results) {
 *       if (!r.success) {
 *           failed++;
 *           Logger::error("Job " + std::to_string(r.job_id) + 
 *                        " failed: " + r.error_message);
 *       }
 *   }
 *   
 *   if (failed > 0) {
 *       std::cerr << failed << " jobs failed - results invalid\n";
 *       return 3;
 *   }
 */

/*******************************************************************************
 * SECTION 8: FREQUENTLY ASKED QUESTIONS
 *******************************************************************************/

/*
 * Q1: Why is this skeleton code instead of complete implementation?
 * 
 * A1: This provides the CLI framework and configuration structure, but the
 *     actual implementation depends on:
 *     - Controller API design (which may evolve)
 *     - Message protocol specifics
 *     - Deployment environment (Khoury cluster vs AWS vs local)
 *     
 *     See test_integration.cpp for working examples of job submission.
 *     The skeleton allows you to adapt to your specific setup.
 * 
 * 
 * Q2: How do I know how many workers are running?
 * 
 * A2: The benchmark client doesn't track worker count - the controller does.
 *     
 *     Options:
 *     1. Query controller for cluster status
 *     2. Pass --workers N flag to benchmark client
 *     3. Track manually in deployment script
 *     
 *     Recommended: Add --workers flag and pass to CSV output
 * 
 * 
 * Q3: Should I run multiple jobs per symbol or distribute evenly?
 * 
 * A3: Distribute evenly for better parallelism:
 *     
 *     Bad (Sequential):
 *       Jobs 1-20: AAPL
 *       Jobs 21-40: GOOGL
 *       # Workers might idle waiting for AAPL jobs to finish
 *     
 *     Good (Round-robin):
 *       Job 1: AAPL, Job 2: GOOGL, Job 3: MSFT, Job 4: AAPL, ...
 *       # Workers always have work available
 * 
 * 
 * Q4: What if my speedup is less than 6.8 at 8 workers?
 * 
 * A4: Investigate bottlenecks:
 *     
 *     1. Check CPU utilization: Should be 80-100%
 *        $ top  # Press '1' to see per-core usage
 *     
 *     2. Profile controller: Is it the bottleneck?
 *        $ perf record -g ./controller
 *        $ perf report
 *     
 *     3. Check load balance: Are jobs evenly distributed?
 *        # Monitor worker completion times
 *     
 *     4. Measure network: Is bandwidth saturated?
 *        $ iftop
 *     
 *     5. Check job sizes: Are some much longer than others?
 *        # Analyze avg_time_per_job for each symbol
 * 
 * 
 * Q5: How many jobs should I use for benchmarking?
 * 
 * A5: Depends on goal:
 *     
 *     Quick test (development):
 *       10-20 jobs (< 1 minute)
 *       Purpose: Verify system works
 *     
 *     Evaluation (project report):
 *       100-500 jobs (5-10 minutes)
 *       Purpose: Measure scalability accurately
 *       Statistical significance: 100+ jobs reduces variance
 *     
 *     Stress test (find limits):
 *       1000+ jobs (30-60 minutes)
 *       Purpose: Find bottlenecks, test stability
 *     
 *     Rule of thumb: At least 10x worker count
 *     Example: 8 workers → 80+ jobs minimum
 * 
 * 
 * Q6: Should I include warmup runs?
 * 
 * A6: Yes for accurate measurements:
 *     
 *     Warmup Run:
 *       ./benchmark_client --jobs 5  # Discard results
 *       # Purpose: Prime caches, establish connections
 *     
 *     Actual Benchmark:
 *       ./benchmark_client --jobs 100  # Use these results
 *     
 *     Without warmup, first run is 10-20% slower due to:
 *     - Cold CPU caches
 *     - Cold file system caches
 *     - TCP slow start
 *     - Dynamic compilation (JIT)
 * 
 * 
 * Q7: How do I generate plots from CSV?
 * 
 * A7: Several options:
 *     
 *     Python (recommended):
 *       import pandas as pd
 *       import matplotlib.pyplot as plt
 *       df = pd.read_csv('benchmark_results.csv')
 *       plt.plot(df['workers'], df['speedup'])
 *       plt.savefig('speedup.png')
 *     
 *     Excel:
 *       1. Open CSV in Excel
 *       2. Insert → Chart → Line chart
 *       3. X-axis: workers, Y-axis: speedup
 *     
 *     gnuplot:
 *       plot 'benchmark_results.csv' using 1:6 with linespoints
 *     
 *     R:
 *       data <- read.csv('benchmark_results.csv')
 *       plot(data$workers, data$speedup)
 * 
 * 
 * Q8: What if results are inconsistent between runs?
 * 
 * A8: Check for:
 *     
 *     1. System load:
 *        $ uptime  # Check load average
 *        # Should be < number of cores
 *     
 *     2. Network stability:
 *        $ ping -c 100 controller_host
 *        # Should have 0% packet loss
 *     
 *     3. Disk I/O:
 *        $ iostat -x 1 10
 *        # %util should be < 80%
 *     
 *     4. Memory pressure:
 *        $ free -h
 *        # Should have free memory, not swapping
 *     
 *     5. Background processes:
 *        $ ps aux --sort=-%cpu | head -20
 *        # Check for CPU hogs
 *     
 *     Solution: Run during quiet hours, dedicated machines
 * 
 * 
 * Q9: Can I benchmark with failures/crashes?
 * 
 * A9: Yes! That's part of fault tolerance evaluation:
 *     
 *     Test Scenario:
 *       1. Start benchmark with 8 workers
 *       2. At 50% completion, kill 2 workers
 *       3. Measure total time including recovery
 *       4. Compare to non-failure case
 *     
 *     Expected:
 *       Normal: 12 seconds
 *       With failures: 12 + recovery_time
 *       Recovery time should be < 5 seconds (project goal)
 *     
 *     Validates:
 *       - Checkpoint/resume works
 *       - Job reassignment works
 *       - System remains stable
 * 
 * 
 * Q10: Is this production-ready code?
 * 
 * A10: No, this is academic/evaluation code.
 *      
 *      For production benchmarking, add:
 *      1. Error handling (network failures, timeouts)
 *      2. Progress reporting (real-time updates)
 *      3. Result validation (checksums, consistency)
 *      4. Automatic retry logic
 *      5. Configuration file support (YAML/JSON)
 *      6. Logging to file (not just console)
 *      7. Statistical analysis (mean, std dev, CI)
 *      8. Report generation (PDF, HTML)
 *      9. Distributed tracing integration
 *      10. Prometheus metrics export
 *      
 *      Consider using established frameworks like:
 *      - Apache JMeter (Java)
 *      - Locust (Python)
 *      - K6 (Go)
 */

 // Documentation complete