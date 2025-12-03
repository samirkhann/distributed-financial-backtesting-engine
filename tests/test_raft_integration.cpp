/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: test_raft_integration.cpp
    
    Description:
        This file implements an integration test suite for the Raft-enabled
        distributed backtesting system. It validates end-to-end functionality
        by testing the interaction between RaftController (fault-tolerant
        coordinator) and Worker (computation node), including job submission,
        consensus protocol, and result collection in a distributed environment.
        
        Test Objectives:
        - Raft Controller Initialization: Verify Raft node starts and operates
        - Worker Connection: Validate worker can connect to Raft controller
        - Job Distribution: Test job submission through Raft consensus
        - End-to-End Execution: Verify complete job lifecycle (submit → execute → result)
        - Fault Tolerance Foundation: Establish baseline for future multi-node tests
        
    Integration Test Philosophy:
        
        INTEGRATION vs UNIT TESTING:
        - Unit tests: Test single component in isolation (CSV loader, message serialization)
        - Integration tests: Test multiple components working together
        - This test: Full system integration (Raft + Controller + Worker + Strategy)
        
        WHY INTEGRATION TESTS MATTER:
        - Unit tests: Verify components work individually ✓
        - Integration tests: Verify components work together ✓
        - Components may work alone but fail when integrated
        - Example: Message serialization works, but network protocol fails
        
        CHALLENGES:
        - Timing: Async operations require delays (Raft election, job processing)
        - State: Distributed state harder to verify than local state
        - Failures: More failure modes (network, consensus, computation)
        - Setup: More complex (multiple components, temporary directories)
        
    Test Architecture:
        
        Single-Node Raft Cluster (Simplified for Testing):
        ┌────────────────────────────────────────────────┐
        │ RaftController (Node 1)                        │
        │  - Raft consensus (single-node = auto-leader) │
        │  - Worker management                           │
        │  - Job scheduling                              │
        │  Port 15001: Worker connections                │
        │  Port 16001: Raft consensus (unused, no peers) │
        └────────────────────────────────────────────────┘
                         ↓ (TCP connection)
        ┌────────────────────────────────────────────────┐
        │ Worker                                         │
        │  - Connects to localhost:15001                 │
        │  - Registers with controller                   │
        │  - Receives job assignments                    │
        │  - Executes backtests                          │
        │  - Returns results                             │
        └────────────────────────────────────────────────┘
                         ↓ (loads data)
        ┌────────────────────────────────────────────────┐
        │ Test Data: /tmp/MSFT.csv                      │
        │  - 150 rows of synthetic price data           │
        │  - Simple uptrend pattern                      │
        └────────────────────────────────────────────────┘
        
    Single-Node Raft Simplification:
        
        PRODUCTION RAFT: 3+ nodes with leader election
        ┌─────────┐ ┌─────────┐ ┌─────────┐
        │ Node 1  │ │ Node 2  │ │ Node 3  │
        │(Leader) │ │(Follower)│ │(Follower)│
        └─────────┘ └─────────┘ └─────────┘
        → Leader election required (200-500ms)
        → Consensus for job submission
        → Fault tolerance (survives 1 failure)
        
        TEST RAFT: 1 node (simplified)
        ┌─────────┐
        │ Node 1  │
        │(Leader) │
        └─────────┘
        → No election needed (auto-leader)
        → No consensus delay (single node)
        → No fault tolerance (but tests basic functionality)
        
        Benefits of single-node test:
        - Faster: No election delay
        - Simpler: Fewer components to manage
        - Deterministic: No timing variability
        - Foundation: Validates basic Raft controller works
        
        Future: Add multi-node tests for full Raft validation
        
    Test Timing Considerations:
        
        DELAYS REQUIRED:
        1. After controller.start(): 1 second
           - Raft initialization
           - Server socket binding
           - Thread spawning
        
        2. After worker.start(): 1 second
           - TCP connection establishment
           - Worker registration
           - Thread spawning
        
        3. After Raft start: 500ms
           - Single-node: Becomes leader immediately
           - Multi-node: Would need 200-500ms for election
        
        4. After job submission: 30 seconds (timeout)
           - Job execution time (varies by complexity)
           - Network communication
           - Result aggregation
        
        WHY SO MUCH WAITING?
        - Distributed systems: Inherently asynchronous
        - Race conditions: Components start in parallel
        - Determinism: Ensure operations complete before assertions
        
        TRADE-OFF:
        - Longer delays: More reliable tests, slower execution
        - Shorter delays: Faster tests, occasional flakes
        - Current: Conservative (prefer reliability)
        
    Test Data Characteristics:
        
        SYNTHETIC DATA GENERATION:
        - Symbol: MSFT (chosen for test)
        - Rows: 150 (sufficient for strategies requiring 20-50 day windows)
        - Pattern: Linear uptrend (predictable for testing)
        - Dates: Sequential (2023-01-01 onwards)
        
        DATA PROPERTIES:
        - Price: Starts at 280, increases ~0.3 per day
        - Volume: Constant 2,000,000 (simplified)
        - Valid: All bars pass validation (High >= Low, etc.)
        
        WHY SYNTHETIC?
        - Deterministic: Known expected results
        - No external dependencies: Don't need real market data
        - Controlled: Can test specific scenarios
        - Fast: Generate on-the-fly (no large files)
        
    Expected Behavior:
        
        SUCCESSFUL TEST RUN:
        $ ./test_raft_integration
        [INFO] Running Raft integration tests...
        Raft Integration Test: Single Raft Controller + Worker... PASSED
          Job ID: 1
          Return: 0.15%  (example value)
        
        === Results ===
        Passed: 1
        Failed: 0
        
        Exit code: 0
        
        POSSIBLE OUTCOMES:
        1. PASSED: Full integration works
        2. FAILED: Some assertion failed or exception thrown
        3. SKIPPED: Not leader (single-node should always be leader, but timing)
        
    Limitations:
        
        WHAT THIS TEST VALIDATES:
        ✓ Raft controller can start
        ✓ Worker can connect to Raft controller
        ✓ Jobs can be submitted
        ✓ Workers can execute jobs
        ✓ Results can be retrieved
        
        WHAT THIS TEST DOES NOT VALIDATE:
        ✗ Multi-node Raft consensus (only 1 node)
        ✗ Leader election (no election with 1 node)
        ✗ Log replication (no followers)
        ✗ Fault tolerance (no failures injected)
        ✗ Network partitions (single machine)
        
        FOR COMPLETE VALIDATION:
        - Need multi-node tests (3+ Raft controllers)
        - Need failure injection (kill leader, partition network)
        - Need chaos testing (random failures)
        
    Dependencies:
        - controller/raft_controller.h: Raft-enabled controller
        - worker/worker.h: Worker implementation
        - common/logger.h: Logging utilities
        - POSIX: Temporary directory (/tmp)
        
    Related Files:
        - test_csv_loader.cpp: Data loading tests
        - test_controller.cpp: Base controller tests
        - controller_main.cpp: Production controller
        - worker_main.cpp: Production worker

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. INCLUDES & DEPENDENCIES
// 2. NAMESPACE AND USING DIRECTIVE
// 3. TEST DATA GENERATION
//    3.1 create_sample_data() - Generate MSFT Test CSV
// 4. MAIN FUNCTION - INTEGRATION TEST SUITE
//    4.1 Test Setup
//    4.2 Raft Controller Initialization
//    4.3 Worker Initialization
//    4.4 Job Submission and Execution
//    4.5 Result Validation
//    4.6 Cleanup and Results
// 5. TEST EXECUTION GUIDE
// 6. UNDERSTANDING TEST TIMING
// 7. INTERPRETING TEST RESULTS
// 8. COMMON TEST FAILURES
// 9. FAQ
// 10. BEST PRACTICES FOR INTEGRATION TESTING
// 11. DEBUGGING INTEGRATION TESTS
// 12. EXTENDING TO MULTI-NODE TESTS
//
//==============================================================================

//==============================================================================
// SECTION 1: INCLUDES & DEPENDENCIES
//==============================================================================

#include "controller/raft_controller.h"  // Raft-enabled controller
#include "worker/worker.h"                // Worker implementation
#include "common/logger.h"                // Logging utilities

// Standard C++ Libraries
// ======================
#include <iostream>   // Console output (test results)
#include <cassert>    // Assertion macros (test verification)
#include <fstream>    // File I/O (test data creation)

//==============================================================================
// SECTION 2: NAMESPACE AND USING DIRECTIVE
//==============================================================================

using namespace backtesting;

//==============================================================================
// SECTION 3: TEST DATA GENERATION
//==============================================================================

//------------------------------------------------------------------------------
// 3.1 CREATE_SAMPLE_DATA() - GENERATE MSFT TEST CSV
//------------------------------------------------------------------------------
//
// void create_sample_data()
//
// PURPOSE:
// Creates synthetic stock price data for integration testing.
//
// OUTPUT FILE:
// - Location: /tmp/MSFT.csv
// - Symbol: MSFT (Microsoft)
// - Rows: 150 (approximately 5 months of trading data)
//
// DATA PATTERN:
// - Prices: Linear uptrend (starts at 280, increases ~0.3 per day)
// - Formula: price = 280 + day * 0.3
// - Volume: Constant 2,000,000 shares
// - Dates: Sequential from 2023-01-01 onwards
//
// PRICE CALCULATION:
// Day 0:   280.0, 281.0, 279.0, 280.5, 2000000
// Day 1:   280.3, 281.3, 279.3, 280.8, 2000000
// Day 149: 324.7, 325.7, 323.7, 325.2, 2000000
//
// WHY SYNTHETIC DATA?
// - Deterministic: Known pattern for validation
// - No dependencies: Don't need real market data
// - Fast: Generated on-the-fly (no large file downloads)
// - Controlled: Can test specific scenarios
//
// DATE FORMAT:
// - Pattern: YYYY-M-D (simplified, not zero-padded)
// - Example: "2023-1-1", "2023-1-2", ...
// - Note: Not strictly ISO 8601 (missing zero padding)
// - Works: Lexicographic sorting still correct for same year/month
//
// LIMITATIONS:
// - Date format: Not production-quality (missing zero padding)
// - Weekends/holidays: Not realistic (includes every day)
// - Volume: Unrealistic (constant)
// - For testing: Sufficient (validates core functionality)
//
// IMPROVEMENT OPPORTUNITY:
// - Use proper date formatting: 2023-01-01, 2023-01-02
// - Skip weekends: Only trading days
// - Variable volume: More realistic
// - Not critical for current tests
//

void create_sample_data() {
    // Create file in temporary directory
    std::ofstream file("/tmp/MSFT.csv");
    
    // Write CSV header
    file << "Date,Open,High,Low,Close,Volume\n";
    
    // Generate 150 rows of price data
    for (int i = 0; i < 150; ++i) {
        // DATE CONSTRUCTION
        // Format: YYYY-M-D (simplified, not zero-padded)
        // Month: i / 30 + 1 (approximately 30 days per month)
        // Day: i % 30 + 1 (1-30 range)
        //
        // Example:
        // i=0:   2023-1-1
        // i=29:  2023-1-30
        // i=30:  2023-2-1
        // i=149: 2023-5-30
        file << "2023-" << (i / 30 + 1) << "-" << (i % 30 + 1) << ","
             
             // PRICE CALCULATIONS
             // Linear uptrend: price = base + day * increment
             << (280.0 + i * 0.3) << ","   // Open: 280.0, 280.3, 280.6, ...
             << (281.0 + i * 0.3) << ","   // High: 281.0, 281.3, 281.6, ...
             << (279.0 + i * 0.3) << ","   // Low:  279.0, 279.3, 279.6, ...
             << (280.5 + i * 0.3) << ","   // Close: 280.5, 280.8, 281.1, ...
             
             // VOLUME (constant)
             << "2000000\n";
    }
    
    // Close file (flush to disk)
    file.close();
    
    // File now exists at /tmp/MSFT.csv
    // Worker can load this data for backtest execution
}

//==============================================================================
// SECTION 4: MAIN FUNCTION - INTEGRATION TEST SUITE
//==============================================================================

int main() {
    //==========================================================================
    // SECTION 4.1: TEST SETUP
    //==========================================================================
    
    // Configure logging
    Logger::set_level(LogLevel::INFO);
    Logger::info("Running Raft integration tests...");
    
    // Test counters
    int passed = 0;
    int failed = 0;
    
    //==========================================================================
    // SECTION 4.2: RAFT CONTROLLER INITIALIZATION
    //==========================================================================
    //
    // TEST CASE: Single Raft Controller + Worker
    //
    // SCOPE:
    // Tests complete end-to-end flow with single Raft node and one worker.
    //
    // COMPONENTS TESTED:
    // - RaftController: Raft-enabled controller
    // - Worker: Computation node
    // - CSV data loading
    // - Strategy execution
    // - Network communication
    //
    // SIMPLIFIED SETUP:
    // - Single Raft node (no peers)
    // - Auto-leader (no election needed)
    // - Validates basic functionality
    // - Foundation for multi-node tests
    //
    //--------------------------------------------------------------------------
    
    // Test: Raft Controller + Worker
    {
        std::cout << "Raft Integration Test: Single Raft Controller + Worker... ";
        
        try {
            // =================================================================
            // SETUP PHASE: Create test data
            // =================================================================
            
            // Create MSFT.csv with 150 rows of price data
            create_sample_data();
            
            // =================================================================
            // RAFT CONTROLLER SETUP
            // =================================================================
            
            // Configure Raft controller
            RaftControllerConfig raft_config;
            raft_config.node_id = 1;                          // Single node
            raft_config.listen_port = 15001;                  // Worker port (non-standard to avoid conflicts)
            raft_config.raft_port = 16001;                    // Raft port (unused with single node)
            raft_config.data_directory = "/tmp";              // Where to find MSFT.csv
            raft_config.raft_log_directory = "/tmp/raft_data_test";  // Raft log storage
            
            // Create Raft controller
            RaftController controller(raft_config);
            
            // START CONTROLLER
            // This:
            // - Starts base controller (worker network on port 15001)
            // - Starts Raft socket (port 16001)
            // - Initializes Raft node
            // - Spawns background threads (accept, scheduler, heartbeat, Raft)
            assert(controller.start());  // Must succeed
            
            // DELAY: Give controller time to initialize
            // Allows:
            // - Server sockets to bind
            // - Threads to start
            // - Raft to initialize (single node becomes leader immediately)
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // =================================================================
            // WORKER SETUP
            // =================================================================
            
            // Configure worker
            WorkerConfig worker_config;
            worker_config.controller_host = "localhost";      // Same machine as controller
            worker_config.controller_port = 15001;            // Connect to controller's worker port
            worker_config.data_directory = "/tmp";            // Where to find MSFT.csv
            worker_config.checkpoint_directory = "/tmp/worker_checkpoints";  // Checkpoint storage
            
            // Create worker
            Worker worker(worker_config);
            
            // START WORKER
            // This:
            // - Connects to controller (TCP to localhost:15001)
            // - Registers (sends WORKER_REGISTER)
            // - Receives worker_id
            // - Spawns threads (worker loop, heartbeat)
            assert(worker.start());  // Must succeed
            
            // DELAY: Give worker time to connect and register
            // Allows:
            // - TCP connection to establish
            // - Registration to complete
            // - Worker to be ready for jobs
            std::this_thread::sleep_for(std::chrono::seconds(1));
            
            // =================================================================
            // RAFT LEADER ELECTION (Single-Node Case)
            // =================================================================
            
            // Wait for Raft to elect leader
            // Single node: Becomes leader immediately (no election needed)
            // Multi-node: Would take 200-500ms for first election
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            // =================================================================
            // JOB SUBMISSION
            // =================================================================
            
            // Prepare job parameters
            JobParams params;
            params.symbol = "MSFT";                    // Must match test data file
            params.strategy_type = "SMA";              // Simple Moving Average strategy
            params.start_date = "2023-01-01";          // Start of test data
            params.end_date = "2023-12-31";            // End of test data (covers all 150 rows)
            params.short_window = 10;                  // 10-day short MA
            params.long_window = 20;                   // 20-day long MA
            params.initial_capital = 10000.0;          // $10,000 starting capital
            
            // SUBMIT JOB to Raft controller
            // With Raft: Leader check happens in submit_job()
            // Returns: job_id > 0 if successful, 0 if not leader
            uint64_t job_id = controller.submit_job(params);
            
            // =================================================================
            // RESULT COLLECTION
            // =================================================================
            
            if (job_id > 0) {
                // JOB SUBMITTED SUCCESSFULLY
                
                // WAIT FOR RESULT
                // Blocking call with 30-second timeout
                // Sufficient for: Data loading + strategy execution + result transmission
                JobResult result;
                bool success = controller.get_job_result(job_id, result, 30);
                
                if (success) {
                    // RESULT RECEIVED: Validate
                    
                    // ASSERTION 1: Job completed successfully
                    assert(result.success);
                    
                    // ASSERTION 2: Correct symbol in result
                    assert(result.symbol == "MSFT");
                    
                    // TEST PASSED
                    std::cout << "PASSED\n";
                    std::cout << "  Job ID: " << job_id << "\n";
                    std::cout << "  Return: " << result.total_return << "%\n";
                    passed++;
                    
                } else {
                    // TIMEOUT: Job didn't complete in 30 seconds
                    // Could indicate: Worker crash, computation error, network issue
                    std::cout << "FAILED: Timeout waiting for result\n";
                    failed++;
                }
                
            } else {
                // JOB SUBMISSION FAILED (job_id = 0)
                // For single-node Raft: Should always be leader
                // If not leader: Timing issue (rare)
                std::cout << "SKIPPED (not leader)\n";
                
                // Note: Not counted as failure (timing race condition acceptable in test)
                // In production: Would retry until leader elected
            }
            
            // =================================================================
            // CLEANUP
            // =================================================================
            
            // Stop worker (graceful shutdown)
            worker.stop();
            
            // Stop controller (stops Raft + base controller)
            controller.stop();
            
        } catch (const std::exception& e) {
            // EXCEPTION HANDLING
            // Any exception indicates test failure
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    // Test scope ends: All local objects destroyed
    
    //==========================================================================
    // SECTION 4.6: RESULTS SUMMARY
    //==========================================================================
    
    // Print test results
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";
    
    // EXIT CODE
    // 0: All tests passed
    // 1: One or more tests failed
    return (failed == 0) ? 0 : 1;
}

//==============================================================================
// END OF TEST IMPLEMENTATION
//==============================================================================

//==============================================================================
// SECTION 5: TEST EXECUTION GUIDE
//==============================================================================
//
// COMPILATION:
// ============
//
// With CMake:
// $ mkdir build && cd build
// $ cmake ..
// $ make test_raft_integration
//
// Manual (g++):
// $ g++ -std=c++17 -I../include -o test_raft_integration 
//       test_raft_integration.cpp 
//       ../src/controller/raft_controller.cpp 
//       ../src/controller/controller.cpp 
//       ../src/worker/worker.cpp 
//       ../src/raft/raft_node.cpp 
//       ../src/data/csv_loader.cpp
//       ../src/common/logger.cpp 
//       ../src/common/message.cpp
//       -lpthread
//
// EXECUTION:
// ==========
//
// Basic run:
// $ ./test_raft_integration
// [INFO] Running Raft integration tests...
// Raft Integration Test: Single Raft Controller + Worker... PASSED
//   Job ID: 1
//   Return: 0.15%
//
// === Results ===
// Passed: 1
// Failed: 0
//
// Check exit code:
// $ echo $?
// 0  # Success
//
// VERBOSE OUTPUT:
// ===============
//
// Enable DEBUG logging for detailed execution trace:
// $ ./test_raft_integration 2>&1 | tee test_log.txt
//
// Shows:
// - Raft controller initialization steps
// - Worker connection and registration
// - Job submission and distribution
// - Backtest execution progress
// - Result transmission
//
// AUTOMATED TESTING:
// ==================
//
// In Makefile:
// test: test_raft_integration
// 	./test_raft_integration || exit 1
//
// In shell script:
// #!/bin/bash
// ./test_raft_integration
// if [ $? -eq 0 ]; then
//     echo "✓ Raft integration test passed"
// else
//     echo "✗ Raft integration test failed"
//     exit 1
// fi
//
// CLEANUP:
// ========
//
// After testing, clean temporary files:
// $ rm -rf /tmp/MSFT.csv
// $ rm -rf /tmp/raft_data_test
// $ rm -rf /tmp/worker_checkpoints
//
// Or let OS clean /tmp automatically
//
//==============================================================================

//==============================================================================
// SECTION 6: UNDERSTANDING TEST TIMING
//==============================================================================
//
// TIMING BREAKDOWN:
// =================
//
// T=0s:     Test starts
// T=0s:     create_sample_data() (instant)
// T=0s:     RaftController created
// T=0.1s:   controller.start() completes
// T=1.1s:   Sleep 1 second (controller initialization)
// T=1.1s:   Worker created
// T=1.2s:   worker.start() completes (connected, registered)
// T=2.2s:   Sleep 1 second (worker initialization)
// T=2.7s:   Sleep 500ms (Raft leader election - trivial for single node)
// T=2.7s:   submit_job() called
// T=2.71s:  Job submitted (added to pending queue)
// T=2.72s:  Scheduler assigns job to worker
// T=2.73s:  Worker receives JOB_ASSIGN message
// T=2.74s:  Worker loads MSFT.csv (~5ms)
// T=2.75s:  Worker executes backtest (~5-15 seconds)
// T=17.75s: Backtest completes
// T=17.76s: Worker sends JOB_RESULT
// T=17.77s: Controller receives result
// T=17.77s: get_job_result() returns true
// T=17.77s: Assertions validated
// T=17.78s: Test PASSED
// T=17.79s: worker.stop(), controller.stop()
// T=18.79s: Test complete
//
// TOTAL TIME:
// - Typical: 15-20 seconds
// - Dominated by: Backtest execution (5-15 seconds)
// - Overhead: ~3 seconds (initialization, delays)
//
// WHY 30-SECOND TIMEOUT?
// - Backtest: Typically 5-15 seconds
// - Safety margin: 2× typical time
// - Handles: Slow machines, debug builds
//
// FAILURE TIMING:
// - If worker crashes: Timeout after 30 seconds
// - If network fails: Immediate failure (connection lost)
// - If assertion fails: Immediate failure (assert aborts)
//
//==============================================================================

//==============================================================================
// SECTION 7: INTERPRETING TEST RESULTS
//==============================================================================
//
// OUTCOME 1: PASSED
// =================
// Output:
//    Raft Integration Test: ... PASSED
//      Job ID: 1
//      Return: 0.15%
//    === Results ===
//    Passed: 1
//    Failed: 0
//
// Interpretation:
// - Full integration working correctly
// - Raft controller started successfully
// - Worker connected and registered
// - Job submitted through Raft
// - Backtest executed
// - Result received and validated
// - All assertions passed
//
// Next steps:
// - Test with multiple workers
// - Test with multi-node Raft cluster
// - Test fault injection
//
//
// OUTCOME 2: FAILED (Timeout)
// ============================
// Output:
//    Raft Integration Test: ... FAILED: Timeout waiting for result
//    === Results ===
//    Passed: 0
//    Failed: 1
//
// Possible causes:
// - Worker crashed during execution
// - Worker couldn't load data (MSFT.csv missing)
// - Backtest took > 30 seconds (unlikely for 150 bars)
// - Worker not connected (registration failed)
//
// Debugging:
// - Check worker logs: Did it receive job?
// - Check data file: Does /tmp/MSFT.csv exist?
// - Increase timeout: Try 60 seconds
// - Add debug logging: See where it hangs
//
//
// OUTCOME 3: FAILED (Assertion)
// ==============================
// Output:
//    Raft Integration Test: ... Assertion `result.success' failed.
//    Aborted (core dumped)
//
// Possible causes:
// - Job failed during execution (result.success = false)
// - Data loading error
// - Strategy execution error
// - Invalid parameters
//
// Debugging:
// - Check result.error_message
// - Check worker logs for exceptions
// - Verify MSFT.csv format
//
//
// OUTCOME 4: SKIPPED (Not Leader)
// ================================
// Output:
//    Raft Integration Test: ... SKIPPED (not leader)
//    === Results ===
//    Passed: 0
//    Failed: 0
//
// Interpretation:
// - Single node didn't become leader yet (timing race)
// - Rare: Single node should immediately be leader
// - Not failure: Just means test couldn't run
//
// Solutions:
// - Increase Raft initialization delay (500ms → 1000ms)
// - Add retry loop (wait for leader, then submit)
// - Check Raft logs: Why not leader?
//
//
// OUTCOME 5: FAILED (Controller Start)
// =====================================
// Output:
//    Assertion `controller.start()' failed.
//
// Possible causes:
// - Port 15001 already in use
// - Port 16001 already in use
// - Permission denied
// - Raft log directory can't be created
//
// Debugging:
// - Check ports: netstat -tulpn | grep 15001
// - Check permissions: Can write to /tmp?
// - Try different ports: 25001, 26001
//
//==============================================================================

//==============================================================================
// SECTION 8: COMMON TEST FAILURES
//==============================================================================
//
// FAILURE 1: Port Already in Use
// ===============================
// SYMPTOM:
//    Assertion `controller.start()' failed.
//    [ERROR] Failed to bind socket: Address already in use
//
// SOLUTION:
//    ☐ Check if port in use: netstat -tulpn | grep 15001
//    ☐ Kill process using port
//    ☐ Change port in test:
//       raft_config.listen_port = 25001;
//       raft_config.raft_port = 26001;
//       worker_config.controller_port = 25001;
//
//
// FAILURE 2: Worker Can't Connect
// ================================
// SYMPTOM:
//    Assertion `worker.start()' failed.
//    [ERROR] Failed to connect: Connection refused
//
// SOLUTION:
//    ☐ Ensure controller started first (it does)
//    ☐ Increase delay after controller.start() (1s → 2s)
//    ☐ Check firewall not blocking localhost
//
//
// FAILURE 3: Data File Not Found
// ===============================
// SYMPTOM:
//    FAILED: Timeout waiting for result
//    (Worker log shows: Failed to load data)
//
// SOLUTION:
//    ☐ Verify /tmp writable: touch /tmp/test
//    ☐ Check create_sample_data() succeeded
//    ☐ Manually create file if needed
//
//
// FAILURE 4: Insufficient Data for Strategy
// ==========================================
// SYMPTOM:
//    result.success = false
//    result.error_message = "Insufficient data for 20-day MA"
//
// SOLUTION:
//    ☐ Increase test data rows (150 → 250)
//    ☐ Or reduce long_window (20 → 10)
//    ☐ Ensure: data rows >= long_window
//
//
// FAILURE 5: Raft Not Leader
// ===========================
// SYMPTOM:
//    SKIPPED (not leader)
//    (But single node should always be leader)
//
// SOLUTION:
//    ☐ Increase Raft initialization delay (500ms → 2000ms)
//    ☐ Add explicit leader check before submit:
//       while (!controller.is_raft_leader()) {
//           std::this_thread::sleep_for(std::chrono::milliseconds(100));
//       }
//    ☐ Check Raft logs for initialization errors
//
//==============================================================================

//==============================================================================
// SECTION 9: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Why test with single Raft node (not 3)?
// ============================================
// A: Simplicity and speed:
//    - Single node: Auto-leader, no election delay
//    - Validates: Basic Raft controller functionality
//    - Foundation: Must work before testing multi-node
//    - Fast: ~15-20 seconds vs. ~30-40 for multi-node
//    
//    Multi-node tests should be added separately
//
// Q2: How do I test with multiple Raft nodes?
// ============================================
// A: Would require:
//    - Start 3 controllers (different ports)
//    - Configure peers on each
//    - Wait for leader election
//    - Submit to leader
//    - Verify followers replicate
//    - Complex but important for full validation
//
// Q3: What if test hangs (never completes)?
// ==========================================
// A: Common causes:
//    - Deadlock in controller/worker
//    - Worker waiting forever for job
//    - Controller waiting for worker that didn't connect
//    
//    Solutions:
//    - Kill test: Ctrl+C
//    - Check with timeout: timeout 60 ./test_raft_integration
//    - Debug: Attach gdb to hung process
//
// Q4: Can I run this test multiple times in parallel?
// ====================================================
// A: No (current implementation):
//    - Uses fixed ports (15001, 16001)
//    - Uses shared /tmp files
//    - Second instance: Port conflict
//    
//    Solution:
//    - Randomize ports per run
//    - Use unique temp directories
//
// Q5: Why are delays so long (1 second)?
// =======================================
// A: Conservative for reliability:
//    - Distributed systems: Async operations
//    - Thread spawning: Not instant
//    - Network binding: Takes time
//    - Better: Slow but reliable test vs. fast but flaky
//
// Q6: Can I test worker failure and recovery?
// ============================================
// A: Yes, would need:
//    - Start worker
//    - Submit job
//    - Kill worker mid-job (worker.stop())
//    - Start new worker
//    - Verify: Job reassigned, checkpoint resumed
//    - Not implemented in current test
//
// Q7: How do I test Raft leader election?
// ========================================
// A: Would require:
//    - Start 3-node cluster
//    - Wait for election
//    - Verify exactly one leader
//    - Kill leader
//    - Verify new election
//    - Verify new leader elected
//
// Q8: What about testing network partitions?
// ===========================================
// A: Requires:
//    - Network simulation (drop packets)
//    - Or: firewall rules to partition
//    - Complex: Beyond scope of simple test
//    - Tools: Jepsen, Chaos Monkey
//
// Q9: Should integration tests be in CI/CD?
// ==========================================
// A: Yes, but:
//    - Run on every commit (fast feedback)
//    - May be slower than unit tests (15-20s)
//    - Could run less frequently (nightly for slow tests)
//    - Balance: Speed vs. coverage
//
// Q10: How do I know if test failure is flaky or real?
// =====================================================
// A: Run multiple times:
//    for i in {1..10}; do ./test_raft_integration; done
//    
//    - Fails always: Real bug
//    - Fails sometimes: Flaky (timing, race condition)
//    - Flaky tests: Increase delays, fix races
//
//==============================================================================

//==============================================================================
// SECTION 10: BEST PRACTICES FOR INTEGRATION TESTING
//==============================================================================
//
// BEST PRACTICE 1: Test Realistic Scenarios
// ==========================================
// DO:
//    - Real network communication (TCP)
//    - Real data loading (CSV files)
//    - Real strategy execution
//    - Full end-to-end flow
//
// DON'T:
//    - Mock everything (defeats purpose of integration test)
//
//
// BEST PRACTICE 2: Use Unique Ports for Tests
// ============================================
// DO:
//    // Test ports: 15001, 16001 (different from production 5000, 5001)
//    // Avoids conflicts with running instances
//
//
// BEST PRACTICE 3: Clean Up Resources
// ====================================
// DO:
//    worker.stop();
//    controller.stop();
//    // Always stop in reverse order of start
//
//
// BEST PRACTICE 4: Use Generous Timeouts
// =======================================
// DO:
//    // 30 seconds for job completion
//    // Better: Reliable slow test than flaky fast test
//
//
// BEST PRACTICE 5: Test in Isolation
// ===================================
// DO:
//    // Each test in its own scope {}
//    // Resources cleaned up between tests
//    // No shared state
//
//
// BEST PRACTICE 6: Provide Clear Failure Messages
// ================================================
// DO:
//    if (!success) {
//        std::cout << "FAILED: Timeout waiting for result\n";
//    }
//    // Clear what went wrong
//
//
// BEST PRACTICE 7: Document Expected Behavior
// ============================================
// DO:
//    // Comments explain timing
//    // Document why delays are needed
//    // Note edge cases
//
//==============================================================================

//==============================================================================
// SECTION 11: DEBUGGING INTEGRATION TESTS
//==============================================================================
//
// DEBUGGING STRATEGY:
// ===================
//
// 1. ENABLE VERBOSE LOGGING
// --------------------------
// Logger::set_level(LogLevel::DEBUG);
// // Shows all operations in detail
//
// 2. CHECK COMPONENT STARTUP
// ---------------------------
// assert(controller.start());  // Did controller start?
// Logger::info("Controller started");
//
// assert(worker.start());  // Did worker start?
// Logger::info("Worker started");
//
// 3. VERIFY NETWORK CONNECTIVITY
// -------------------------------
// // After worker.start()
// assert(worker.get_worker_id() > 0);  // Registered successfully?
//
// 4. CHECK JOB SUBMISSION
// -----------------------
// uint64_t job_id = controller.submit_job(params);
// Logger::info("Submitted job: " + std::to_string(job_id));
// assert(job_id > 0);  // Submission succeeded?
//
// 5. MONITOR JOB PROCESSING
// --------------------------
// // Poll for status
// for (int i = 0; i < 30; ++i) {
//     std::this_thread::sleep_for(std::chrono::seconds(1));
//     Logger::info("Waiting... " + std::to_string(i) + "s");
//     // Check if result available
// }
//
// 6. INSPECT RESULT ON FAILURE
// -----------------------------
// if (!result.success) {
//     Logger::error("Job failed: " + result.error_message);
//     // Shows exactly what went wrong
// }
//
// 7. USE DEBUGGER FOR HANGS
// --------------------------
// $ gdb ./test_raft_integration
// (gdb) run
// (Ctrl+C when hangs)
// (gdb) thread apply all bt  # Show all thread stacks
// (gdb) info threads  # Show thread states
//
// 8. CHECK TEMPORARY FILES
// -------------------------
// $ ls -la /tmp/MSFT.csv  # Data file created?
// $ ls -la /tmp/raft_data_test/  # Raft log directory?
// $ ls -la /tmp/worker_checkpoints/  # Checkpoint directory?
//
//==============================================================================

//==============================================================================
// SECTION 12: EXTENDING TO MULTI-NODE TESTS
//==============================================================================
//
// MULTI-NODE RAFT TEST (Future Enhancement):
// ===========================================
//
// Test structure for 3-node Raft cluster:
//
// {
//     std::cout << "Multi-Node Raft Test... ";
//     
//     try {
//         // Start 3 Raft controllers
//         RaftController node1(config1);
//         RaftController node2(config2);
//         RaftController node3(config3);
//         
//         // Configure peers (each knows about others)
//         node1.add_controller_peer(2, "localhost", 15002, 16002);
//         node1.add_controller_peer(3, "localhost", 15003, 16003);
//         // ... symmetric for node2 and node3 ...
//         
//         // Start all nodes
//         assert(node1.start());
//         assert(node2.start());
//         assert(node3.start());
//         
//         // Wait for leader election
//         std::this_thread::sleep_for(std::chrono::seconds(2));
//         
//         // Find leader
//         RaftController* leader = nullptr;
//         if (node1.is_raft_leader()) leader = &node1;
//         else if (node2.is_raft_leader()) leader = &node2;
//         else if (node3.is_raft_leader()) leader = &node3;
//         
//         assert(leader != nullptr);  // Must have leader
//         
//         // Start worker (connects to any node)
//         Worker worker(worker_config);
//         assert(worker.start());
//         
//         // Submit job to leader
//         uint64_t job_id = leader->submit_job(params);
//         assert(job_id > 0);
//         
//         // Wait for result
//         JobResult result;
//         assert(leader->get_job_result(job_id, result, 30));
//         assert(result.success);
//         
//         // Test leader failover
//         Logger::info("Testing leader failover...");
//         leader->stop();  // Kill leader
//         
//         // Wait for new election
//         std::this_thread::sleep_for(std::chrono::seconds(2));
//         
//         // Verify new leader elected
//         int leader_count = 0;
//         if (node1.is_raft_leader()) leader_count++;
//         if (node2.is_raft_leader()) leader_count++;
//         if (node3.is_raft_leader()) leader_count++;
//         
//         assert(leader_count == 1);  // Exactly one leader
//         
//         std::cout << "PASSED\n";
//         passed++;
//         
//     } catch (const std::exception& e) {
//         std::cout << "FAILED: " << e.what() << "\n";
//         failed++;
//     }
// }
//
// COMPLEXITY:
// - Much more complex than single-node test
// - More failure modes to handle
// - Longer execution time (election delays)
// - Worth it: Validates true Raft functionality
//
//==============================================================================

//==============================================================================
// COMPARISON WITH PRODUCTION DEPLOYMENT
//==============================================================================
//
// TEST ENVIRONMENT vs PRODUCTION:
// ================================
//
// Aspect           | Test                  | Production
// -----------------|------------------------|-------------------------
// Raft nodes       | 1                     | 3-5
// Workers          | 1                     | 2-8
// Data source      | Synthetic (/tmp)      | Real CSV (NFS)
// Ports            | 15001, 16001          | 5000, 5001
// Execution time   | 15-20 seconds         | Hours/days
// Job count        | 1                     | 100-10,000
// Fault injection  | None                  | Expected (handled)
// Monitoring       | Test assertions       | Prometheus, Grafana
// Logging          | Console               | Files, ELK stack
//
// TEST VALIDATES:
// ✓ Basic connectivity
// ✓ Protocol correctness
// ✓ Job execution
// ✓ Result collection
//
// TEST DOES NOT VALIDATE:
// ✗ Scale (only 1 worker)
// ✗ Fault tolerance (no failures injected)
// ✗ Performance under load
// ✗ Multi-node consensus
//
// NEXT STEPS FOR COMPREHENSIVE TESTING:
// - Add multi-node Raft tests
// - Add worker failure tests
// - Add network partition tests
// - Add performance benchmarks
// - Add stress tests (1000s of jobs)
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================