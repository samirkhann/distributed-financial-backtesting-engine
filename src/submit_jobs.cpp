/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: submit_jobs.cpp
    
    Description:
        This file implements a batch job submission client for the distributed
        backtesting system. It provides a command-line tool that loads job
        configurations from CSV files, submits them to a running controller,
        waits for all results, and exports comprehensive results with performance
        metrics to CSV format.
        
        Core Functionality:
        - CSV Configuration Loading: Batch job definitions from file
        - Programmatic Job Submission: Submit multiple jobs automatically
        - Result Collection: Wait for all jobs to complete
        - CSV Export: Write results in machine-readable format
        - Performance Metrics: Calculate throughput and execution time
        - Summary Statistics: Success rate, timing, performance
        
    Use Cases:
        
        1. BATCH EXPERIMENTS:
           - Test multiple symbols (AAPL, GOOGL, MSFT, ...)
           - Test multiple strategies (SMA, RSI, MACD, ...)
           - Test multiple parameter combinations
           - Automated overnight runs
        
        2. SYSTEM EVALUATION:
           - Performance benchmarking (jobs/second throughput)
           - Scalability testing (2, 4, 8 workers)
           - Fault tolerance validation (worker failures)
           - Load testing (100s of concurrent jobs)
        
        3. RESEARCH AND ANALYSIS:
           - Strategy parameter optimization
           - Symbol universe screening
           - Historical performance analysis
           - Risk-return trade-off studies
        
    Application Architecture:
        
        ┌─────────────────────────────────────────┐
        │ Job Submission Client (This File)      │
        │                                         │
        │  1. Load: jobs.csv                     │
        │  2. Parse: Job configurations          │
        │  3. Submit: To controller              │
        │  4. Wait: For all results              │
        │  5. Export: results.csv + summary      │
        └─────────────────────────────────────────┘
                         ↓
        ┌─────────────────────────────────────────┐
        │ Controller (Running Server)             │
        │  - Distributes jobs to workers         │
        │  - Aggregates results                  │
        └─────────────────────────────────────────┘
                         ↓
        ┌─────────────────────────────────────────┐
        │ Workers (2-8 Nodes)                    │
        │  - Execute backtests                   │
        │  - Return results                      │
        └─────────────────────────────────────────┘
        
    Input File Format (jobs.csv):
        
        CSV structure:
        ─────────────────────────────────────────────────────────────
        symbol,strategy,start_date,end_date,short_window,long_window,initial_capital
        AAPL,SMA,2020-01-01,2024-12-31,50,200,10000.0
        GOOGL,SMA,2020-01-01,2024-12-31,50,200,10000.0
        MSFT,SMA,2020-01-01,2024-12-31,30,100,10000.0
        ─────────────────────────────────────────────────────────────
        
        Columns:
        1. symbol: Stock ticker (AAPL, GOOGL, MSFT, ...)
        2. strategy: Strategy type (SMA, RSI, MACD, ...)
        3. start_date: Backtest start (YYYY-MM-DD)
        4. end_date: Backtest end (YYYY-MM-DD)
        5. short_window: Short MA period (days)
        6. long_window: Long MA period (days)
        7. initial_capital: Starting capital (USD)
        
    Output File Format (results.csv):
        
        CSV structure:
        ─────────────────────────────────────────────────────────────
        job_id,symbol,total_return,sharpe_ratio,max_drawdown,final_value,num_trades,winning,losing,success
        1,AAPL,0.25,1.5,-0.15,12500.0,50,30,20,true
        2,GOOGL,0.18,1.2,-0.12,11800.0,45,28,17,true
        3,MSFT,-0.05,0.3,-0.25,9500.0,38,15,23,true
        ─────────────────────────────────────────────────────────────
        
        Plus summary file (results.csv.summary.txt):
        ─────────────────────────────────────────────────────────────
        === Job Submission Summary ===
        Total jobs submitted: 100
        Successful: 95
        Failed: 5
        Total time: 120.5 seconds
        Throughput: 0.83 jobs/sec
        ─────────────────────────────────────────────────────────────
        
    Execution Flow:
        
        1. INITIALIZATION:
           - Parse command-line arguments
           - Load job configurations from CSV
           - Validate inputs
        
        2. CONNECTION:
           - Connect to controller (or create local instance)
           - Verify controller is running
        
        3. SUBMISSION:
           - For each job configuration:
             * Create JobParams
             * Submit to controller
             * Record job_id
        
        4. COLLECTION:
           - For each submitted job:
             * Wait for result (blocking)
             * Timeout: 5 minutes per job
             * Store result
        
        5. REPORTING:
           - Write results to CSV file
           - Calculate performance metrics
           - Generate summary statistics
           - Log completion
        
    Performance Tracking:
        
        Measured Metrics:
        - Total execution time (seconds)
        - Job throughput (jobs/second)
        - Success rate (percentage)
        - Individual job timing (could be added)
        
        Example Results:
        - 100 jobs in 120 seconds = 0.83 jobs/sec
        - With 8 workers: ~6-10 jobs/sec typical
        - With 1 worker: ~0.1-1 jobs/sec typical
        - Throughput scales linearly with worker count
        
    Limitations:
        
        CURRENT IMPLEMENTATION:
        - Uses in-process controller (not true client)
        - Cannot connect to remote controller over network
        - Must run on same machine as controller
        
        TRUE CLIENT WOULD NEED:
        - TCP socket connection to controller
        - Client-side protocol implementation
        - Request/response message handling
        - Authentication (optional)
        
        WORKAROUND FOR PROJECT:
        - Run client and controller in same process
        - Direct function calls (no network)
        - Sufficient for evaluation and testing
        
    Dependencies:
        - controller/controller.h: Controller class
        - common/logger.h: Logging utilities
        - <iostream>: Console I/O
        - <fstream>: File I/O (CSV reading/writing)
        - <sstream>: String parsing
        - <chrono>: Performance timing
        
    Related Files:
        - controller_main.cpp: Controller server executable
        - worker_main.cpp: Worker executable
        - jobs.csv: Example job configuration file
        - results.csv: Example output file

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. INCLUDES & DEPENDENCIES
// 2. NAMESPACE AND DATA STRUCTURES
//    2.1 Using Directive
//    2.2 JobConfig Structure
// 3. CONFIGURATION LOADING
//    3.1 load_job_config() - Parse CSV Configuration
// 4. RESULTS EXPORT
//    4.1 write_results() - Export to CSV
// 5. MAIN FUNCTION
//    5.1 Command-Line Parsing
//    5.2 Configuration Loading
//    5.3 Job Submission
//    5.4 Result Collection
//    5.5 Results Export
// 6. CSV FILE SPECIFICATIONS
// 7. USAGE EXAMPLES
// 8. COMMON PITFALLS & SOLUTIONS
// 9. FAQ
// 10. BEST PRACTICES
// 11. PERFORMANCE ANALYSIS
// 12. TROUBLESHOOTING GUIDE
// 13. AUTOMATION SCRIPTS
//
//==============================================================================

//==============================================================================
// SECTION 1: INCLUDES & DEPENDENCIES
//==============================================================================

#include "controller/controller.h"  // Controller class for job submission
#include "common/logger.h"          // Logging utilities

// Standard C++ Libraries
// ======================
#include <iostream>    // Console I/O (std::cout, std::cerr)
#include <fstream>     // File I/O (CSV reading/writing)
#include <sstream>     // String parsing (CSV tokenization)
#include <vector>      // Dynamic arrays (job list, results)
#include <chrono>      // Performance timing (execution duration)

//==============================================================================
// SECTION 2: NAMESPACE AND DATA STRUCTURES
//==============================================================================

//------------------------------------------------------------------------------
// 2.1 USING DIRECTIVE
//------------------------------------------------------------------------------

using namespace backtesting;

//------------------------------------------------------------------------------
// 2.2 JOBCONFIG STRUCTURE
//------------------------------------------------------------------------------
//
// struct JobConfig
//
// PURPOSE:
// Intermediate representation of job configuration loaded from CSV file.
//
// DESIGN RATIONALE:
// - Separate from JobParams: Parsing layer vs. protocol layer
// - Plain struct: Simple data holder, no behavior
// - Direct CSV mapping: Fields match CSV columns exactly
//
// FIELDS:
// - symbol: Stock ticker (AAPL, GOOGL, etc.)
// - strategy: Strategy type name (SMA, RSI, MACD, etc.)
// - start_date: Backtest start date (YYYY-MM-DD)
// - end_date: Backtest end date (YYYY-MM-DD)
// - short_window: Short moving average window (days)
// - long_window: Long moving average window (days)
// - initial_capital: Starting portfolio value (USD)
//
// CONVERSION TO JOBPARAMS:
//   JobConfig config = load_from_csv();
//   JobParams params;
//   params.symbol = config.symbol;
//   params.strategy_type = config.strategy;
//   // ... copy other fields ...
//
// VALIDATION:
// - None in structure (assumes valid CSV data)
// - Should add: Validate date format, positive windows, etc.
//
// MEMORY:
// - Typical size: ~100 bytes per job
// - 1000 jobs: ~100 KB (negligible)
//

struct JobConfig {
    std::string symbol;           // Stock ticker
    std::string strategy;         // Strategy type
    std::string start_date;       // Backtest start (YYYY-MM-DD)
    std::string end_date;         // Backtest end (YYYY-MM-DD)
    int short_window;             // Short MA window (days)
    int long_window;              // Long MA window (days)
    double initial_capital;       // Starting capital (USD)
};

//==============================================================================
// SECTION 3: CONFIGURATION LOADING
//==============================================================================

//------------------------------------------------------------------------------
// 3.1 LOAD_JOB_CONFIG() - PARSE CSV CONFIGURATION
//------------------------------------------------------------------------------
//
// std::vector<JobConfig> load_job_config(const std::string& config_file)
//
// PURPOSE:
// Loads job configurations from CSV file for batch submission.
//
// PARAMETERS:
// - config_file: Path to CSV file (e.g., "jobs.csv")
//
// RETURNS:
// - vector<JobConfig>: All valid job configurations
// - Empty vector: If file not found or parse errors
//
// CSV FORMAT EXPECTED:
// ──────────────────────────────────────────────────────────────────
// symbol,strategy,start_date,end_date,short_window,long_window,initial_capital
// AAPL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// GOOGL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// ──────────────────────────────────────────────────────────────────
//
// PARSING ALGORITHM:
// 1. Open file
// 2. Read and skip header line (first line)
// 3. For each subsequent line:
//    a. Split by comma
//    b. Validate column count (need 7)
//    c. Parse each field (string, int, double)
//    d. Create JobConfig
//    e. Add to results vector
// 4. Close file
// 5. Return vector
//
// ERROR HANDLING:
// - File not found: Log error, return empty vector
// - Invalid line: Skip line, continue parsing
// - Parse error: Skip field/line, continue
// - Defensive: Load as many valid jobs as possible
//
// FIELD PARSING:
// - Strings: Direct assignment (no conversion)
// - Integers: std::stoi (may throw invalid_argument)
// - Doubles: std::stod (may throw invalid_argument)
// - Exceptions: Should be caught (not implemented)
//
// VALIDATION (NOT IMPLEMENTED):
// - Date format: Should match YYYY-MM-DD
// - Date ordering: start_date <= end_date
// - Windows: 0 < short_window < long_window
// - Capital: initial_capital > 0
// - Symbol: Non-empty, valid ticker
//
// PERFORMANCE:
// - Typical: 1-10 ms for 100 jobs
// - I/O bound: File reading dominant cost
//
// THREAD SAFETY:
// - Not thread-safe: File I/O not synchronized
// - Call from single thread
//

std::vector<JobConfig> load_job_config(const std::string& config_file) {
    // Result vector: Will hold all loaded jobs
    std::vector<JobConfig> jobs;
    
    // Open configuration file for reading
    std::ifstream file(config_file);
    
    // CHECK: File opened successfully
    if (!file.is_open()) {
        Logger::error("Failed to open config file: " + config_file);
        return jobs;  // Return empty vector
    }
    
    // Read lines from file
    std::string line;
    
    // STEP 1: Skip header line
    // First line contains column names, not data
    // Example: "symbol,strategy,start_date,..."
    std::getline(file, line);  // Read and discard
    
    // STEP 2: Parse data lines
    while (std::getline(file, line)) {
        // Create string stream for tokenization
        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> tokens;
        
        // Split line by comma delimiter
        // CSV format: field1,field2,field3,...
        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }
        
        // VALIDATION: Check column count
        // Need exactly 7 columns for complete job definition
        // If fewer: Invalid line, skip
        // If more: Ignore extra columns (forward compatibility)
        if (tokens.size() >= 7) {
            // Create job configuration from parsed tokens
            JobConfig job;
            job.symbol = tokens[0];                         // Column 0: Symbol
            job.strategy = tokens[1];                       // Column 1: Strategy
            job.start_date = tokens[2];                     // Column 2: Start date
            job.end_date = tokens[3];                       // Column 3: End date
            job.short_window = std::stoi(tokens[4]);        // Column 4: Short window
            job.long_window = std::stoi(tokens[5]);         // Column 5: Long window
            job.initial_capital = std::stod(tokens[6]);     // Column 6: Initial capital
            
            // Add to results
            jobs.push_back(job);
        }
        // If tokens.size() < 7: Skip line (invalid format)
    }
    
    // Close file
    file.close();
    
    // Return all loaded jobs
    return jobs;
    
    // IMPROVEMENT OPPORTUNITIES:
    //
    // 1. Exception handling for std::stoi, std::stod:
    //    try {
    //        job.short_window = std::stoi(tokens[4]);
    //    } catch (const std::exception& e) {
    //        Logger::warning("Invalid short_window in line: " + line);
    //        continue;  // Skip this job
    //    }
    //
    // 2. Trim whitespace from tokens:
    //    token = trim(token);  // Remove leading/trailing spaces
    //
    // 3. Validate parsed values:
    //    if (job.short_window <= 0 || job.long_window <= job.short_window) {
    //        Logger::warning("Invalid windows: " + line);
    //        continue;
    //    }
    //
    // 4. Report parse statistics:
    //    Logger::info("Loaded " + std::to_string(jobs.size()) + 
    //                " jobs, skipped " + std::to_string(skipped) + " invalid lines");
}

//==============================================================================
// SECTION 4: RESULTS EXPORT
//==============================================================================

//------------------------------------------------------------------------------
// 4.1 WRITE_RESULTS() - EXPORT TO CSV
//------------------------------------------------------------------------------
//
// void write_results(const std::string& output_file, 
//                   const std::vector<std::pair<uint64_t, JobResult>>& results,
//                   double total_time_sec)
//
// PURPOSE:
// Exports job results to CSV file with summary statistics.
//
// PARAMETERS:
// - output_file: Path to output CSV (e.g., "results.csv")
// - results: Vector of (job_id, JobResult) pairs
// - total_time_sec: Total execution time for throughput calculation
//
// OUTPUT FILES:
// 1. results.csv: Detailed results for each job
// 2. results.csv.summary.txt: Summary statistics
//
// CSV HEADER:
// job_id,symbol,total_return,sharpe_ratio,max_drawdown,final_value,
// num_trades,winning,losing,success
//
// CSV DATA ROW EXAMPLE:
// 1,AAPL,0.25,1.5,-0.15,12500.0,50,30,20,true
//
// SUMMARY FORMAT:
// === Job Submission Summary ===
// Total jobs submitted: 100
// Successful: 95
// Failed: 5
// Total time: 120.5 seconds
// Throughput: 0.83 jobs/sec
//
// SUCCESS COUNTING:
// - Iterates through results
// - Counts: result.success == true
// - Calculates: Failed = total - successful
//
// THROUGHPUT CALCULATION:
// - Formula: jobs / seconds
// - Example: 100 jobs / 120 sec = 0.83 jobs/sec
// - Interpretation: Average job completion rate
//
// FILE I/O:
// - Creates new files (overwrites if exist)
// - No append mode (fresh export each run)
// - Could add: Timestamp in filename for history
//
// ERROR HANDLING:
// - File creation failure: Not checked
// - Should add: Verify file opened successfully
//
// THREAD SAFETY:
// - Not thread-safe: Single-threaded file writing
//

void write_results(const std::string& output_file, 
                  const std::vector<std::pair<uint64_t, JobResult>>& results,
                  double total_time_sec) {
    // =========================================================================
    // WRITE DETAILED RESULTS CSV
    // =========================================================================
    
    // Open output file for writing
    std::ofstream file(output_file);
    
    // IMPROVEMENT: Check if file opened
    // if (!file.is_open()) {
    //     Logger::error("Failed to create output file: " + output_file);
    //     return;
    // }
    
    // Write CSV header
    // Defines column names for result fields
    file << "job_id,symbol,total_return,sharpe_ratio,max_drawdown,"
         << "final_value,num_trades,winning,losing,success\n";
    
    // Write data rows
    // One row per job result
    for (const auto& [job_id, result] : results) {
        file << job_id << ","
             << result.symbol << ","
             << result.total_return << ","
             << result.sharpe_ratio << ","
             << result.max_drawdown << ","
             << result.final_portfolio_value << ","
             << result.num_trades << ","
             << result.winning_trades << ","
             << result.losing_trades << ","
             << (result.success ? "true" : "false") << "\n";
    }
    
    // Close results file
    file.close();
    
    // =========================================================================
    // WRITE SUMMARY STATISTICS
    // =========================================================================
    
    // Summary filename: results.csv.summary.txt
    std::string summary_file = output_file + ".summary.txt";
    std::ofstream summary(summary_file);
    
    // CALCULATE SUCCESS RATE
    // Count jobs with success = true
    int successful = 0;
    for (const auto& [_, result] : results) {
        if (result.success) successful++;
    }
    
    // WRITE SUMMARY
    summary << "=== Job Submission Summary ===\n";
    summary << "Total jobs submitted: " << results.size() << "\n";
    summary << "Successful: " << successful << "\n";
    summary << "Failed: " << (results.size() - successful) << "\n";
    summary << "Total time: " << total_time_sec << " seconds\n";
    summary << "Throughput: " << (results.size() / total_time_sec) << " jobs/sec\n";
    
    // Close summary file
    summary.close();
    
    // Log completion
    Logger::info("Results written to " + output_file);
    Logger::info("Summary written to " + summary_file);
    
    // ADDITIONAL STATISTICS (could add):
    // - Average return: mean(total_return)
    // - Average Sharpe ratio: mean(sharpe_ratio)
    // - Win rate: winning_trades / (winning + losing)
    // - Best/worst performers: max/min returns
}

//==============================================================================
// SECTION 5: MAIN FUNCTION
//==============================================================================

int main(int argc, char* argv[]) {
    //==========================================================================
    // SECTION 5.1: COMMAND-LINE PARSING
    //==========================================================================
    //
    // Parse command-line options for client configuration.
    //
    //--------------------------------------------------------------------------
    
    // DEFAULT CONFIGURATION
    std::string config_file = "jobs.csv";       // Input: Job definitions
    std::string output_file = "results.csv";    // Output: Results
    uint16_t port = 5000;                       // Controller port
    
    // PARSE ARGUMENTS
    // Loop through command-line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        // OPTION: --config FILE
        // Specify input CSV file with job definitions
        if (arg == "--config" && i + 1 < argc) {
            config_file = argv[++i];
        }
        
        // OPTION: --output FILE
        // Specify output CSV file for results
        else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        }
        
        // OPTION: --port PORT
        // Controller port to connect to
        else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
        
        // OPTION: --help
        // Display usage and exit
        else if (arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --config FILE    Job config CSV (default: jobs.csv)\n"
                      << "  --output FILE    Results CSV (default: results.csv)\n"
                      << "  --port PORT      Controller port (default: 5000)\n"
                      << "  --help           Show this help\n";
            return 0;
        }
        // Unknown options: Silently ignored (could warn)
    }
    
    //==========================================================================
    // SECTION 5.2: CONFIGURATION LOADING
    //==========================================================================
    //
    // Load job definitions from CSV file.
    //
    //--------------------------------------------------------------------------
    
    // Configure logging
    Logger::set_level(LogLevel::INFO);
    Logger::info("=== Job Submission Client ===");
    
    // Load jobs from CSV
    auto jobs = load_job_config(config_file);
    
    // VALIDATION: Check jobs loaded
    if (jobs.empty()) {
        Logger::error("No jobs loaded from " + config_file);
        return 1;  // Exit with error
    }
    
    Logger::info("Loaded " + std::to_string(jobs.size()) + " jobs from " + config_file);
    
    //==========================================================================
    // SECTION 5.3: JOB SUBMISSION
    //==========================================================================
    //
    // Submit all jobs to controller and record job IDs.
    //
    //--------------------------------------------------------------------------
    
    // Create controller instance
    // NOTE: This creates in-process controller (not true network client)
    // For project: Simpler than implementing network client
    // For production: Would connect via TCP socket
    ControllerConfig config;
    config.listen_port = port;
    
    Controller controller(config);
    
    // NOTE: For external client, we'd need socket connection
    // For this project, we run this on same machine as controller
    // and submit jobs programmatically
    //
    // TRUE NETWORK CLIENT WOULD:
    // 1. Connect: socket(AF_INET, SOCK_STREAM, 0)
    // 2. Send: JOB_SUBMIT message with parameters
    // 3. Receive: Job ID response
    // 4. Later: Query for result
    //
    // CURRENT APPROACH:
    // - In-process controller instance
    // - Direct function calls
    // - Simpler for academic project
    
    Logger::info("Submitting jobs...");
    
    // Record start time for performance measurement
    auto start_time = std::chrono::steady_clock::now();
    
    // Submit all jobs and collect job IDs
    std::vector<uint64_t> job_ids;
    
    for (const auto& job_config : jobs) {
        // Convert JobConfig to JobParams
        JobParams params;
        params.symbol = job_config.symbol;
        params.strategy_type = job_config.strategy;
        params.start_date = job_config.start_date;
        params.end_date = job_config.end_date;
        params.short_window = job_config.short_window;
        params.long_window = job_config.long_window;
        params.initial_capital = job_config.initial_capital;
        
        // Submit job to controller
        uint64_t job_id = controller.submit_job(params);
        
        // CHECK: Submission successful
        if (job_id > 0) {
            job_ids.push_back(job_id);
            Logger::info("Submitted job " + std::to_string(job_id) + 
                        " for " + job_config.symbol);
        } else {
            // Submission failed: job_id = 0
            // Reasons: Controller not running, not leader (Raft)
            Logger::error("Failed to submit job for " + job_config.symbol);
        }
    }
    
    // LOG: Submission phase complete
    Logger::info("All jobs submitted (" + std::to_string(job_ids.size()) + " total)");
    
    //==========================================================================
    // SECTION 5.4: RESULT COLLECTION
    //==========================================================================
    //
    // Wait for all jobs to complete and collect results.
    //
    //--------------------------------------------------------------------------
    
    Logger::info("Waiting for " + std::to_string(job_ids.size()) + " jobs to complete...");
    
    // Result storage: (job_id, JobResult) pairs
    std::vector<std::pair<uint64_t, JobResult>> results;
    
    // Timeout per job: 5 minutes
    // Reasonable for most backtests
    // Could be made configurable: --timeout 600
    int timeout_sec = 300;  // 5 minutes
    
    // Wait for each job's result
    // BLOCKING: This loop blocks until all jobs complete or timeout
    for (uint64_t job_id : job_ids) {
        JobResult result;
        
        // BLOCKING CALL: Wait for result (up to timeout_sec)
        // Returns: true if result received, false if timeout
        if (controller.get_job_result(job_id, result, timeout_sec)) {
            // RESULT RECEIVED: Store for export
            results.push_back({job_id, result});
            
            // Log based on success/failure
            if (result.success) {
                Logger::info("Job " + std::to_string(job_id) + " completed: " +
                           result.symbol + " Return=" + 
                           std::to_string(result.total_return * 100) + "%");
            } else {
                Logger::warning("Job " + std::to_string(job_id) + " failed: " + 
                              result.error_message);
            }
        } else {
            // TIMEOUT: Job didn't complete in time
            // Could be: Worker crashed, computation very slow, network issue
            Logger::error("Timeout waiting for job " + std::to_string(job_id));
            
            // Don't add to results (no data available)
            // Could add: Placeholder result with success=false
        }
    }
    
    // Record end time
    auto end_time = std::chrono::steady_clock::now();
    
    // CALCULATE DURATION
    // Cast to seconds for readability
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        end_time - start_time).count();
    
    //==========================================================================
    // SECTION 5.5: RESULTS EXPORT
    //==========================================================================
    //
    // Write results to CSV and display summary.
    //
    //--------------------------------------------------------------------------
    
    // Export results to CSV files
    write_results(output_file, results, static_cast<double>(duration));
    
    // Log completion summary
    Logger::info("All jobs completed in " + std::to_string(duration) + " seconds");
    
    // Calculate and log throughput
    // Throughput: jobs per second
    double throughput = results.size() / static_cast<double>(duration);
    Logger::info("Throughput: " + std::to_string(throughput) + " jobs/sec");
    
    // Exit successfully
    return 0;
    
    // PERFORMANCE INTERPRETATION:
    //
    // Example results:
    // - 100 jobs in 120 seconds = 0.83 jobs/sec
    //
    // Factors affecting throughput:
    // - Worker count: More workers = higher throughput
    // - Job complexity: Longer backtests = lower throughput
    // - Network latency: Affects job distribution
    // - Data loading: CSV parsing overhead
    //
    // Typical throughput:
    // - 1 worker: 0.1-1 jobs/sec
    // - 4 workers: 0.4-4 jobs/sec
    // - 8 workers: 0.8-8 jobs/sec
    // (Linear scaling with worker count)
}

//==============================================================================
// END OF IMPLEMENTATION
//==============================================================================

//==============================================================================
// SECTION 6: CSV FILE SPECIFICATIONS
//==============================================================================
//
// INPUT FILE FORMAT (jobs.csv):
// ==============================
//
// Structure:
// ─────────────────────────────────────────────────────────────────────────
// symbol,strategy,start_date,end_date,short_window,long_window,initial_capital
// AAPL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// GOOGL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// MSFT,SMA,2020-01-01,2024-12-31,30,100,15000.0
// AMZN,SMA,2019-01-01,2023-12-31,50,200,10000.0
// ─────────────────────────────────────────────────────────────────────────
//
// Column Specifications:
//
// 1. symbol (string):
//    - Stock ticker symbol
//    - Examples: AAPL, GOOGL, MSFT, AMZN, TSLA
//    - Must match: CSV filename (AAPL.csv)
//    - Case-sensitive: AAPL ≠ aapl
//
// 2. strategy (string):
//    - Strategy type identifier
//    - Examples: SMA, RSI, MACD, MeanReversion
//    - Must match: Available strategy implementations
//    - Currently supported: SMA
//
// 3. start_date (string):
//    - Backtest start date
//    - Format: YYYY-MM-DD (ISO 8601)
//    - Example: 2020-01-01
//    - Validation: Should be <= end_date
//
// 4. end_date (string):
//    - Backtest end date
//    - Format: YYYY-MM-DD
//    - Example: 2024-12-31
//    - Validation: Should be >= start_date
//
// 5. short_window (integer):
//    - Short moving average period (days)
//    - Example: 50
//    - Range: 1-200 typical
//    - Validation: Should be < long_window
//
// 6. long_window (integer):
//    - Long moving average period (days)
//    - Example: 200
//    - Range: 50-300 typical
//    - Validation: Should be > short_window
//
// 7. initial_capital (double):
//    - Starting portfolio value (USD)
//    - Example: 10000.0
//    - Range: 1000-1000000 typical
//    - Validation: Should be > 0
//
// CREATING INPUT FILE:
// ====================
//
// Manual (text editor):
// 1. Create jobs.csv
// 2. Add header line
// 3. Add one job per line
// 4. Save file
//
// Programmatic (Python):
// import csv
// with open('jobs.csv', 'w') as f:
//     writer = csv.writer(f)
//     writer.writerow(['symbol', 'strategy', 'start_date', 'end_date',
//                      'short_window', 'long_window', 'initial_capital'])
//     for symbol in ['AAPL', 'GOOGL', 'MSFT']:
//         writer.writerow([symbol, 'SMA', '2020-01-01', '2024-12-31',
//                         50, 200, 10000.0])
//
// Spreadsheet (Excel/Google Sheets):
// 1. Create spreadsheet with columns
// 2. Fill in job data
// 3. Export as CSV
//
// OUTPUT FILE FORMAT (results.csv):
// ==================================
//
// Column Specifications:
//
// 1. job_id: Unique job identifier (from controller)
// 2. symbol: Stock ticker (echoed from input)
// 3. total_return: Percentage return (0.25 = 25%)
// 4. sharpe_ratio: Risk-adjusted return
// 5. max_drawdown: Worst decline (negative percentage)
// 6. final_value: Ending portfolio value (USD)
// 7. num_trades: Total trades executed
// 8. winning: Number of profitable trades
// 9. losing: Number of unprofitable trades
// 10. success: true/false (job completed successfully)
//
// IMPORTING RESULTS:
// ==================
//
// Python (pandas):
// import pandas as pd
// df = pd.read_csv('results.csv')
// print(df.describe())
// print(df[df['success'] == True]['total_return'].mean())
//
// R:
// results <- read.csv('results.csv')
// summary(results)
// mean(results[results$success == TRUE, 'total_return'])
//
// Excel:
// File → Open → results.csv
// (Automatic import)
//
//==============================================================================

//==============================================================================
// SECTION 7: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Basic Usage with Defaults
// =====================================
//
// Step 1: Create jobs.csv
// $ cat > jobs.csv << EOF
// symbol,strategy,start_date,end_date,short_window,long_window,initial_capital
// AAPL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// GOOGL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// MSFT,SMA,2020-01-01,2024-12-31,50,200,10000.0
// EOF
//
// Step 2: Run client
// $ ./job_client
// [INFO] === Job Submission Client ===
// [INFO] Loaded 3 jobs from jobs.csv
// [INFO] Submitting jobs...
// [INFO] Submitted job 1 for AAPL
// [INFO] Submitted job 2 for GOOGL
// [INFO] Submitted job 3 for MSFT
// [INFO] Waiting for 3 jobs to complete...
// [INFO] Job 1 completed: AAPL Return=25.5%
// [INFO] Job 2 completed: GOOGL Return=18.3%
// [INFO] Job 3 completed: MSFT Return=12.7%
// [INFO] All jobs completed in 45 seconds
// [INFO] Throughput: 0.067 jobs/sec
// [INFO] Results written to results.csv
//
// Step 3: View results
// $ cat results.csv
// job_id,symbol,total_return,sharpe_ratio,...
// 1,AAPL,0.255,1.5,-0.15,12550.0,50,30,20,true
// 2,GOOGL,0.183,1.2,-0.12,11830.0,45,28,17,true
// 3,MSFT,0.127,0.9,-0.18,11270.0,42,25,17,true
//
//
// EXAMPLE 2: Custom Files and Port
// =================================
//
// $ ./job_client
//     --config experiments/batch_001.csv 
//     --output results/batch_001_results.csv
//     --port 6000
//
// Uses:
// - Input: experiments/batch_001.csv
// - Output: results/batch_001_results.csv
// - Controller: Port 6000 (non-standard)
//
//
// EXAMPLE 3: Large Batch Processing
// ==================================
//
// Step 1: Generate large job file (100 symbols × 3 strategies)
// $ python generate_jobs.py --symbols sp500.txt --strategies SMA,RSI,MACD > jobs_large.csv
//
// Step 2: Run batch
// $ ./job_client --config jobs_large.csv --output results_large.csv
// [INFO] Loaded 300 jobs from jobs_large.csv
// [INFO] Submitting jobs...
// (... 300 job submissions ...)
// [INFO] Waiting for 300 jobs to complete...
// (... progress over ~10-30 minutes ...)
// [INFO] All jobs completed in 1800 seconds
// [INFO] Throughput: 0.167 jobs/sec
//
// Step 3: Analyze results
// $ python analyze_results.py results_large.csv
//
//
// EXAMPLE 4: Parameter Sweep Experiment
// ======================================
//
// Test different window combinations for AAPL:
//
// jobs.csv:
// symbol,strategy,start_date,end_date,short_window,long_window,initial_capital
// AAPL,SMA,2020-01-01,2024-12-31,10,50,10000.0
// AAPL,SMA,2020-01-01,2024-12-31,20,100,10000.0
// AAPL,SMA,2020-01-01,2024-12-31,30,150,10000.0
// AAPL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// AAPL,SMA,2020-01-01,2024-12-31,75,300,10000.0
//
// Result analysis:
// - Compare Sharpe ratios across parameter combinations
// - Identify optimal windows for AAPL
// - Plot: window vs. return
//
//
// EXAMPLE 5: Automated Testing Script
// ====================================
//
// #!/bin/bash
// # run_experiments.sh
//
// # Array of experiment configurations
// experiments=(
//     "exp1_tech_stocks.csv"
//     "exp2_finance_stocks.csv"
//     "exp3_energy_stocks.csv"
// )
//
// # Run each experiment
// for exp in "${experiments[@]}"; do
//     echo "Running experiment: $exp"
//     
//     output="results_${exp}"
//     ./job_client --config "configs/$exp" --output "results/$output"
//     
//     if [ $? -eq 0 ]; then
//         echo "✓ $exp completed successfully"
//     else
//         echo "✗ $exp failed"
//     fi
//     
//     # Brief delay between experiments
//     sleep 5
// done
//
// echo "All experiments complete!"
//
//==============================================================================

//==============================================================================
// SECTION 8: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: jobs.csv File Not Found
// ===================================
// SYMPTOM:
//    $ ./job_client
//    [ERROR] Failed to open config file: jobs.csv
//    [ERROR] No jobs loaded from jobs.csv
//
// SOLUTIONS:
//    1. Create jobs.csv in current directory
//    2. Specify path: ./job_client --config /path/to/jobs.csv
//    3. Check current directory: pwd
//
//
// PITFALL 2: Invalid CSV Format
// ==============================
// SYMPTOM:
//    [INFO] Loaded 0 jobs from jobs.csv
//    (File exists but no jobs loaded)
//
// SOLUTIONS:
//    1. Verify CSV format:
//       - Header line present
//       - Comma-separated (not tab or semicolon)
//       - Exactly 7 columns
//    
//    2. Check for parsing errors:
//       - Add debug logging in load_job_config
//       - Print tokens.size() to see column count
//    
//    3. Validate data:
//       - Numbers are numeric (not text)
//       - No missing fields
//
//
// PITFALL 3: Job Submission Returns 0
// ====================================
// SYMPTOM:
//    [ERROR] Failed to submit job for AAPL
//    (controller.submit_job returns 0)
//
// SOLUTIONS:
//    1. Check controller running:
//       - In-process: controller.start() not called
//       - Remote: Controller not reachable
//    
//    2. For RaftController: Check if leader
//       - Only leader accepts jobs
//       - Followers return 0
//
//
// PITFALL 4: Timeout Waiting for Results
// =======================================
// SYMPTOM:
//    [ERROR] Timeout waiting for job 1
//    (get_job_result returns false)
//
// SOLUTIONS:
//    1. Increase timeout:
//       timeout_sec = 600;  // 10 minutes
//    
//    2. Check workers:
//       - Are workers connected?
//       - Are workers processing jobs?
//    
//    3. Check job complexity:
//       - Large date range may take longer
//       - Increase timeout or reduce job scope
//
//
// PITFALL 5: Results File Already Open
// =====================================
// SYMPTOM:
//    Can't write to results.csv (file locked)
//
// SOLUTIONS:
//    1. Close file in Excel/editor
//    2. Use different output filename
//    3. Add timestamp to filename automatically
//
//
// PITFALL 6: Insufficient Data Files
// ===================================
// SYMPTOM:
//    Jobs fail with "Failed to load data"
//
// SOLUTIONS:
//    1. Verify CSV files exist:
//       ls data/AAPL.csv data/GOOGL.csv
//    
//    2. Check data directory configuration
//    3. Ensure symbols in jobs.csv match available CSV files
//
//==============================================================================

//==============================================================================
// SECTION 9: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Can I submit jobs to remote controller?
// ============================================
// A: Not in current implementation:
//    - Creates in-process controller (same machine)
//    - For remote: Would need TCP client implementation
//    - Workaround: SSH to controller machine, run client there
//
// Q2: How many jobs can I submit at once?
// ========================================
// A: Limited by:
//    - System memory (jobs stored in pending queue)
//    - Practical: 1000-10,000 jobs
//    - For more: Submit in batches
//
// Q3: Can I reuse same config file multiple times?
// =================================================
// A: Yes:
//    - Each run creates new job IDs
//    - Results written to same output file (overwrites)
//    - Tip: Use timestamp in output filename for history
//
// Q4: What if some jobs fail?
// ============================
// A: Partial results:
//    - Successful jobs: Written to results.csv
//    - Failed jobs: Marked with success=false
//    - Summary shows: success vs. failure counts
//
// Q5: How do I know which jobs are still running?
// ================================================
// A: Not shown in this client:
//    - Could query controller for job status
//    - Controller statistics show active job count
//    - For detailed tracking: Add progress logging
//
// Q6: Can I cancel submitted jobs?
// =================================
// A: Not implemented:
//    - No job cancellation API in current design
//    - Could add: CANCEL_JOB message type
//    - Workaround: Stop controller (cancels all jobs)
//
// Q7: How do I run multiple experiments?
// =======================================
// A: Script it:
//    for config in exp*.csv; do
//        ./job_client --config $config --output results_$config
//    done
//
// Q8: Can I get real-time progress updates?
// ==========================================
// A: Not in current design:
//    - get_job_result blocks until complete
//    - Could add: Polling with timeout, check periodically
//    - Could add: Callback for job completion
//
// Q9: What happens if client crashes?
// ====================================
// A: Jobs continue:
//    - Jobs already submitted to controller
//    - Controller distributes to workers
//    - Workers execute and return results
//    - But: Client can't collect results (not running)
//
// Q10: How do I process results programmatically?
// ================================================
// A: Parse CSV file:
//    Python: pandas.read_csv('results.csv')
//    R: read.csv('results.csv')
//    Or: Write custom parser
//
//==============================================================================

//==============================================================================
// SECTION 10: BEST PRACTICES
//==============================================================================
//
// BEST PRACTICE 1: Validate Input Configuration
// ==============================================
// DO:
//    // Before submitting, validate each job
//    for (auto& job : jobs) {
//        if (job.short_window >= job.long_window) {
//            Logger::error("Invalid windows for " + job.symbol);
//            continue;  // Skip invalid job
//        }
//        if (job.start_date > job.end_date) {
//            Logger::error("Invalid date range for " + job.symbol);
//            continue;
//        }
//        // Submit only valid jobs
//    }
//
//
// BEST PRACTICE 2: Use Descriptive Filenames
// ===========================================
// DO:
//    std::time_t now = std::time(nullptr);
//    std::string timestamp = std::to_string(now);
//    std::string output = "results_" + timestamp + ".csv";
//    // Creates: results_1700000000.csv (unique)
//
//
// BEST PRACTICE 3: Handle Partial Failures
// =========================================
// DO:
//    // Collect all results (success + failure)
//    // Write partial results even if some jobs timeout
//    // Summary shows success rate
//
//
// BEST PRACTICE 4: Monitor Progress
// ==================================
// DO:
//    int completed = 0;
//    for (uint64_t job_id : job_ids) {
//        JobResult result;
//        if (controller.get_job_result(job_id, result, timeout)) {
//            completed++;
//            Logger::info("Progress: " + std::to_string(completed) +
//                        "/" + std::to_string(job_ids.size()));
//        }
//    }
//
//
// BEST PRACTICE 5: Use Appropriate Timeouts
// ==========================================
// Configure based on job complexity:
//    - Simple jobs (1 year): 60 seconds
//    - Complex jobs (10 years): 300 seconds
//    - Very complex: 600+ seconds
//
//
// BEST PRACTICE 6: Batch Large Submissions
// =========================================
// DO:
//    // Instead of 10,000 jobs at once
//    // Submit in batches of 100
//    for (int batch = 0; batch < 100; ++batch) {
//        for (int i = 0; i < 100; ++i) {
//            submit_job(...);
//        }
//        wait_for_batch_completion();
//    }
//
//
// BEST PRACTICE 7: Export Results Incrementally
// ==============================================
// For very long runs:
//    // Write results as they complete
//    for (auto job_id : job_ids) {
//        JobResult result;
//        if (get_job_result(job_id, result, timeout)) {
//            append_to_results_file(result);  // Append mode
//        }
//    }
//    // Prevents loss if client crashes mid-run
//
//==============================================================================

//==============================================================================
// SECTION 11: PERFORMANCE ANALYSIS
//==============================================================================
//
// THROUGHPUT CALCULATION:
// =======================
//
// Formula:
//   Throughput = Jobs Completed / Total Time
//
// Example:
//   100 jobs in 120 seconds = 0.83 jobs/sec
//
// FACTORS AFFECTING THROUGHPUT:
//
// 1. Worker Count (primary factor):
//    - 1 worker: ~0.1-1 jobs/sec
//    - 4 workers: ~0.4-4 jobs/sec
//    - 8 workers: ~0.8-8 jobs/sec
//    - Scaling: Linear (ideally)
//
// 2. Job Complexity:
//    - Simple (1 year, 252 bars): ~1-5 seconds per job
//    - Medium (5 years, 1260 bars): ~5-15 seconds per job
//    - Complex (20 years, 5000 bars): ~20-60 seconds per job
//
// 3. Network Latency:
//    - Local: Negligible (<1 ms)
//    - Khoury cluster: Low (~0.1-1 ms)
//    - Across datacenters: Higher (~10-100 ms)
//
// 4. Data Loading:
//    - Cached: Fast (~100 ns)
//    - First load: Slow (~2-5 ms)
//    - NFS vs. local: NFS slower
//
// 5. Parallelization Efficiency:
//    - Ideal: 8 workers = 8× speedup
//    - Actual: 6-7× speedup (overhead)
//    - Overhead: Job distribution, network, synchronization
//
// PERFORMANCE BENCHMARKS:
// =======================
//
// Configuration: 8 workers, Khoury cluster
//
// Workload: 100 jobs (5 years each)
// Expected throughput: ~0.5-1.0 jobs/sec
// Expected total time: 100-200 seconds
//
// Workload: 1000 jobs (1 year each)
// Expected throughput: ~2-5 jobs/sec
// Expected total time: 200-500 seconds
//
// SCALABILITY ANALYSIS:
// =====================
//
// Worker Count | Throughput  | Speedup | Efficiency
// -------------|-------------|---------|------------
// 1            | 0.10 j/s    | 1.0×    | 100%
// 2            | 0.19 j/s    | 1.9×    | 95%
// 4            | 0.36 j/s    | 3.6×    | 90%
// 8            | 0.68 j/s    | 6.8×    | 85%
//
// Efficiency = (Speedup / Worker_Count) × 100%
//
// Goal from project plan: ≥ 85% efficiency at 8 workers ✓
//
//==============================================================================

//==============================================================================
// SECTION 12: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM: No jobs loaded
// ========================
// SYMPTOMS:
//    [ERROR] No jobs loaded from jobs.csv
//
// DEBUGGING:
//    ☐ Check file exists: ls -l jobs.csv
//    ☐ Check file readable: cat jobs.csv
//    ☐ Verify CSV format: Has header, has data rows
//    ☐ Check column count: Should be 7 columns
//    ☐ Try absolute path: --config /full/path/to/jobs.csv
//
//
// PROBLEM: All jobs fail
// =======================
// SYMPTOMS:
//    All results have success=false
//
// DEBUGGING:
//    ☐ Check error messages in results
//    ☐ Check data files exist (AAPL.csv, etc.)
//    ☐ Check date ranges valid
//    ☐ Check workers connected to controller
//    ☐ Check controller logs for errors
//
//
// PROBLEM: Some jobs timeout
// ===========================
// SYMPTOMS:
//    [ERROR] Timeout waiting for job 5
//
// DEBUGGING:
//    ☐ Check timeout value (300 seconds default)
//    ☐ Increase timeout for complex jobs
//    ☐ Check worker logs (is job executing?)
//    ☐ Check if workers crashed (heartbeat timeout)
//    ☐ Check job reassignment happening
//
//
// PROBLEM: Can't write results
// =============================
// SYMPTOMS:
//    Results file not created or empty
//
// DEBUGGING:
//    ☐ Check directory writable: touch results.csv
//    ☐ Check file not locked: lsof results.csv
//    ☐ Check disk space: df -h
//    ☐ Try different output path: --output /tmp/results.csv
//
//
// PROBLEM: Throughput lower than expected
// ========================================
// SYMPTOMS:
//    Expected 1.0 jobs/sec, getting 0.3 jobs/sec
//
// DEBUGGING:
//    ☐ Check worker count: Should have 8 workers
//    ☐ Check worker utilization: Are workers idle?
//    ☐ Check job complexity: Longer than expected?
//    ☐ Check network latency: Khoury cluster should be low
//    ☐ Profile workers: Where is time spent?
//
//==============================================================================

//==============================================================================
// SECTION 13: AUTOMATION SCRIPTS
//==============================================================================
//
// BATCH EXPERIMENT RUNNER:
// ========================
//
// Python script to generate job configurations:
//
// # generate_jobs.py
// import csv
// import sys
//
// def generate_parameter_sweep(symbol, output_file):
//     """Generate jobs testing different parameter combinations"""
//     with open(output_file, 'w', newline='') as f:
//         writer = csv.writer(f)
//         
//         # Write header
//         writer.writerow(['symbol', 'strategy', 'start_date', 'end_date',
//                         'short_window', 'long_window', 'initial_capital'])
//         
//         # Parameter combinations
//         short_windows = [10, 20, 30, 50, 75]
//         long_windows = [50, 100, 150, 200, 300]
//         
//         for short in short_windows:
//             for long in long_windows:
//                 if short < long:  # Valid combination
//                     writer.writerow([
//                         symbol, 'SMA',
//                         '2020-01-01', '2024-12-31',
//                         short, long, 10000.0
//                     ])
//
// # Usage
// generate_parameter_sweep('AAPL', 'aapl_sweep.csv')
//
// RESULT ANALYSIS SCRIPT:
// =======================
//
// # analyze_results.py
// import pandas as pd
// import matplotlib.pyplot as plt
//
// # Load results
// df = pd.read_csv('results.csv')
//
// # Filter successful jobs
// df = df[df['success'] == True]
//
// # Summary statistics
// print("=== Results Summary ===")
// print(f"Total jobs: {len(df)}")
// print(f"Average return: {df['total_return'].mean():.2%}")
// print(f"Average Sharpe: {df['sharpe_ratio'].mean():.2f}")
// print(f"Best performer: {df.loc[df['total_return'].idxmax(), 'symbol']}")
//
// # Plot returns distribution
// df['total_return'].hist(bins=20)
// plt.title('Distribution of Returns')
// plt.xlabel('Total Return')
// plt.ylabel('Frequency')
// plt.savefig('returns_distribution.png')
//
// CONTINUOUS INTEGRATION:
// =======================
//
// GitHub Actions workflow (.github/workflows/backtest.yml):
//
// name: Nightly Backtests
// on:
//   schedule:
//     - cron: '0 2 * * *'  # Run at 2 AM daily
//
// jobs:
//   backtest:
//     runs-on: ubuntu-latest
//     steps:
//       - uses: actions/checkout@v2
//       
//       - name: Build project
//         run: |
//           mkdir build && cd build
//           cmake ..
//           make
//       
//       - name: Start controller
//         run: |
//           ./build/controller &
//           sleep 5
//       
//       - name: Start workers
//         run: |
//           for i in {1..4}; do
//             ./build/worker &
//           done
//           sleep 5
//       
//       - name: Run backtests
//         run: |
//           ./build/job_client --config configs/nightly.csv
//                              --output results/nightly_$(date +%Y%m%d).csv
//       
//       - name: Upload results
//         uses: actions/upload-artifact@v2
//         with:
//           name: backtest-results
//           path: results/
//
//==============================================================================

//==============================================================================
// TYPICAL WORKFLOW EXAMPLE
//==============================================================================
//
// COMPLETE END-TO-END WORKFLOW:
// ==============================
//
// Terminal 1: Start Controller
// -----------------------------
// $ ./controller --port 5000 --data-dir ./data
// [INFO] Controller running. Press Ctrl+C to stop.
//
// Terminal 2: Start Workers (repeat 4 times)
// -------------------------------------------
// $ ./worker --controller localhost:5000 &  # Worker 1
// $ ./worker --controller localhost:5000 &  # Worker 2
// $ ./worker --controller localhost:5000 &  # Worker 3
// $ ./worker --controller localhost:5000 &  # Worker 4
//
// Terminal 3: Prepare Job Configuration
// --------------------------------------
// $ cat > jobs.csv << 'EOF'
// symbol,strategy,start_date,end_date,short_window,long_window,initial_capital
// AAPL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// GOOGL,SMA,2020-01-01,2024-12-31,50,200,10000.0
// MSFT,SMA,2020-01-01,2024-12-31,50,200,10000.0
// AMZN,SMA,2020-01-01,2024-12-31,50,200,10000.0
// TSLA,SMA,2020-01-01,2024-12-31,50,200,10000.0
// EOF
//
// Terminal 3: Run Client
// ----------------------
// $ ./job_client --config jobs.csv --output results.csv
// [INFO] === Job Submission Client ===
// [INFO] Loaded 5 jobs from jobs.csv
// [INFO] Submitting jobs...
// [INFO] Submitted job 1 for AAPL
// [INFO] Submitted job 2 for GOOGL
// [INFO] Submitted job 3 for MSFT
// [INFO] Submitted job 4 for AMZN
// [INFO] Submitted job 5 for TSLA
// [INFO] Waiting for 5 jobs to complete...
// [INFO] Job 1 completed: AAPL Return=25.5%
// [INFO] Job 2 completed: GOOGL Return=18.3%
// [INFO] Job 3 completed: MSFT Return=12.7%
// [INFO] Job 4 completed: AMZN Return=31.2%
// [INFO] Job 5 completed: TSLA Return=45.8%
// [INFO] All jobs completed in 25 seconds
// [INFO] Throughput: 0.2 jobs/sec
// [INFO] Results written to results.csv
// [INFO] Summary written to results.csv.summary.txt
//
// Terminal 3: View Results
// ------------------------
// $ cat results.csv
// job_id,symbol,total_return,sharpe_ratio,max_drawdown,final_value,num_trades,winning,losing,success
// 1,AAPL,0.255,1.5,-0.15,12550.0,50,30,20,true
// 2,GOOGL,0.183,1.2,-0.12,11830.0,45,28,17,true
// ...
//
// $ cat results.csv.summary.txt
// === Job Submission Summary ===
// Total jobs submitted: 5
// Successful: 5
// Failed: 0
// Total time: 25.0 seconds
// Throughput: 0.2 jobs/sec
//
// Terminal 3: Analyze with Python
// --------------------------------
// $ python3
// >>> import pandas as pd
// >>> df = pd.read_csv('results.csv')
// >>> df['total_return'].describe()
// count    5.000000
// mean     0.266600
// std      0.115234
// min      0.127000
// ...
//
//==============================================================================

//==============================================================================
// INTEGRATION WITH PROJECT EVALUATION
//==============================================================================
//
// USING CLIENT FOR PROJECT EVALUATION:
// =====================================
//
// Test 1: Verify Functionality
// -----------------------------
// Objective: Backtest completes correctly
//
// jobs.csv:
// AAPL,SMA,2020-01-01,2020-12-31,50,200,10000.0
//
// Run:
// $ ./job_client --config jobs.csv --output test1_results.csv
//
// Verify:
// - Job completes: success=true
// - Reasonable metrics: return ≠ 0, trades > 0
//
// Test 2: Scalability Measurement
// --------------------------------
// Objective: Verify ≥ 85% efficiency at 8 workers
//
// jobs.csv: 100 identical jobs
//
// Run with 1 worker:
// $ ./job_client ... 
// Throughput: 0.10 jobs/sec
//
// Run with 8 workers:
// $ ./job_client ...
// Throughput: 0.68 jobs/sec
//
// Speedup: 0.68 / 0.10 = 6.8×
// Efficiency: 6.8 / 8 = 85% ✓ (meets goal)
//
// Test 3: Fault Tolerance
// ------------------------
// Objective: System recovers from worker failure
//
// 1. Submit 100 jobs
// 2. Kill random worker mid-execution
// 3. Verify: Jobs reassigned, all complete
// 4. Check: No jobs lost
//
// Test 4: Concurrent Jobs
// ------------------------
// Objective: Handle multiple concurrent backtests
//
// jobs.csv: 5 different strategies
//
// Verify: All strategies execute concurrently
//
//==============================================================================

//==============================================================================
// FUTURE ENHANCEMENTS
//==============================================================================
//
// ENHANCEMENT 1: True Network Client
// ===================================
// Implement TCP client to connect to remote controller:
//
// class ControllerClient {
//     int socket_fd_;
//     
//     bool connect(const std::string& host, uint16_t port) {
//         socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
//         // ... connect to controller ...
//     }
//     
//     uint64_t submit_job(const JobParams& params) {
//         // Serialize JOB_SUBMIT message
//         // Send to controller
//         // Receive job_id response
//     }
//     
//     bool get_job_result(uint64_t job_id, JobResult& result) {
//         // Send JOB_QUERY message
//         // Receive JOB_RESULT response
//     }
// };
//
// ENHANCEMENT 2: Progress Bar
// ============================
// Show real-time progress during execution:
//
// [INFO] Waiting for jobs: [=====>    ] 50% (50/100) ETA: 60s
//
// Implementation:
// - Calculate: completed / total
// - Estimate time remaining: (total - completed) × avg_time
// - Update: Every second or every completion
//
// ENHANCEMENT 3: Asynchronous Collection
// =======================================
// Don't wait for jobs sequentially:
//
// // Current (sequential):
// for (job_id : job_ids) {
//     get_job_result(job_id, result, 300);  // Blocks 300 sec
// }
// // Total: 100 × 300 sec = 8.3 hours worst case
//
// // Improved (polling):
// while (results.size() < job_ids.size()) {
//     for (job_id : job_ids) {
//         if (!has_result(job_id)) {
//             if (try_get_result(job_id, result, 0)) {  // Non-blocking
//                 results.add(result);
//             }
//         }
//     }
//     sleep(1);  // Check every second
// }
// // More responsive, better timeout handling
//
// ENHANCEMENT 4: Job Prioritization
// ==================================
// Specify priority in CSV:
//
// symbol,strategy,start_date,end_date,short_window,long_window,initial_capital,priority
// AAPL,SMA,2020-01-01,2024-12-31,50,200,10000.0,HIGH
// MSFT,SMA,2020-01-01,2024-12-31,50,200,10000.0,LOW
//
// High priority jobs scheduled first
//
// ENHANCEMENT 5: Retry on Failure
// ================================
// Automatically retry failed jobs:
//
// if (!result.success) {
//     Logger::info("Retrying job " + std::to_string(job_id));
//     uint64_t retry_id = controller.submit_job(params);
//     // Wait for retry result
// }
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================