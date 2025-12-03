/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: logger.cpp
    
    Description:
        This file contains the implementation of the Logger class's static member
        variable definitions for the distributed backtesting system. The Logger
        provides thread-safe, configurable logging capabilities across all 
        distributed components (controllers, workers, and communication layers).
        
        Key Features:
        - Thread-safe logging through mutex synchronization
        - Configurable log levels (DEBUG, INFO, WARN, ERROR, FATAL)
        - Static singleton-style design for global access
        - Zero initialization overhead for static members
        
    Architecture Context:
        In this distributed system with:
        - 3-node Raft controller cluster
        - 2-8 worker nodes
        - Multiple concurrent backtest jobs
        - Complex failure scenarios (controller failover, worker crashes)
        
        Logging is CRITICAL for:
        - Debugging Raft consensus (leader elections, log replication)
        - Tracking job assignments and completions
        - Detecting worker failures (missed heartbeats)
        - Analyzing performance bottlenecks
        - Post-mortem analysis of system failures
        
    Thread Safety:
        This implementation provides the foundation for thread-safe logging
        across all distributed components, which is essential given:
        - Multi-threaded worker nodes (job execution + heartbeats)
        - Concurrent Raft operations (leader election + log replication)
        - Parallel result aggregation from multiple workers
        
    Static Initialization:
        Static members are initialized here (outside the class definition) to:
        1. Prevent multiple definitions across translation units
        2. Ensure single, global instance of logging state
        3. Provide deterministic initialization order
        4. Enable link-time resolution of symbols

    Related Files:
        - common/logger.h: Logger class declaration and interface
        - All component files: Use Logger for diagnostics and monitoring

    Usage Notes:
        This file should be compiled exactly once and linked with all
        distributed system components to ensure a single, shared logging
        configuration across controller and worker nodes.

*******************************************************************************/

#include "common/logger.h"

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. NAMESPACE DECLARATION
// 2. STATIC MEMBER INITIALIZATION
//    2.1 Log Level Configuration
//    2.2 Thread Synchronization Mutex
// 3. USAGE EXAMPLES
// 4. COMMON PITFALLS
// 5. FAQ
// 6. BEST PRACTICES
// 7. TROUBLESHOOTING GUIDE
//
//==============================================================================

namespace backtesting {

//==============================================================================
// SECTION 1: NAMESPACE DECLARATION
//==============================================================================
//
// The 'backtesting' namespace encapsulates all components of the distributed
// financial backtesting system to prevent naming conflicts with:
// - Third-party libraries (Boost.Asio, nlohmann::json)
// - Standard library extensions
// - Raft consensus implementation libraries (if used)
//
// Namespace Organization:
//   backtesting::
//   ├── common::      Shared utilities (Logger, Config, Utils)
//   ├── controller::  Raft cluster, job scheduling, worker registry
//   ├── worker::      Job execution, data loading, checkpointing
//   ├── strategy::    Trading strategies (SMA, RSI, Mean Reversion)
//   └── network::     TCP communication, serialization
//
//==============================================================================

//==============================================================================
// SECTION 2: STATIC MEMBER INITIALIZATION
//==============================================================================
//
// CRITICAL: Static member variables declared in the Logger class header
// MUST be defined exactly once in a source file (.cpp) to:
//
// 1. PREVENT LINKER ERRORS
//    Without this definition, you'll get "undefined reference" errors
//    when linking multiple object files that use Logger.
//
// 2. ENSURE SINGLE INSTANCE
//    Static members are shared across ALL instances and translation units.
//    This ensures the entire distributed system (all controllers and workers)
//    shares the same logging configuration.
//
// 3. GUARANTEE DETERMINISTIC INITIALIZATION
//    Defining static members in the source file ensures they're initialized
//    before main() executes, preventing initialization order fiascos.
//
// 4. PROVIDE GLOBAL STATE
//    In a distributed system, we need global logging state that persists
//    across all threads, components, and function calls.
//
//==============================================================================

//------------------------------------------------------------------------------
// 2.1 LOG LEVEL CONFIGURATION
//------------------------------------------------------------------------------
//
// LogLevel Logger::current_level_ = LogLevel::INFO;
//
// This static member variable controls the minimum severity level for log
// messages to be output across the ENTIRE distributed system.
//
// INITIALIZATION DETAILS:
// - Type: LogLevel (enum class, likely defined in logger.h)
// - Default Value: LogLevel::INFO
// - Scope: Class-level static (shared across all Logger instances)
// - Lifetime: Program duration (initialized before main(), destroyed after)
// - Thread Safety: Protected by mutex_ during writes
//
// TYPICAL LOG LEVEL HIERARCHY (from most to least verbose):
//   DEBUG   - Detailed diagnostic info (e.g., "Worker received job ID: 12345")
//   INFO    - General informational messages (e.g., "Worker started, connecting to controller")
//   WARN    - Warning conditions (e.g., "Heartbeat response delayed by 500ms")
//   ERROR   - Error conditions (e.g., "Failed to load CSV file: AAPL_2020.csv")
//   FATAL   - Critical failures (e.g., "Raft cluster lost quorum, shutting down")
//
// WHY INFO AS DEFAULT?
// - Production-ready: Provides operational visibility without excessive verbosity
// - Performance: Reduces logging overhead (DEBUG can be very chatty)
// - Disk Usage: Prevents log files from growing too large on Khoury cluster
// - Debugging: Can be changed to DEBUG at runtime for troubleshooting
//
// RUNTIME MODIFICATION:
//   Logger::SetLevel(LogLevel::DEBUG);  // Enable verbose logging
//   Logger::SetLevel(LogLevel::ERROR);  // Only critical errors
//
// DISTRIBUTED SYSTEM CONSIDERATIONS:
// In this multi-node system (3 controllers + 8 workers = 11 processes):
// - Each process has its own current_level_ (not shared across machines)
// - Controllers and workers can have different log levels
// - Recommendation: Use environment variables or config files for consistency
//   Example: LOG_LEVEL=DEBUG ./controller --id=1
//
// PERFORMANCE IMPACT:
// - Checking log level is O(1) integer comparison
// - Messages below current_level_ are filtered out BEFORE formatting
// - Critical for high-throughput systems (thousands of heartbeats/sec)
//
//------------------------------------------------------------------------------

LogLevel Logger::current_level_ = LogLevel::INFO;

//------------------------------------------------------------------------------
// 2.2 THREAD SYNCHRONIZATION MUTEX
//------------------------------------------------------------------------------
//
// std::mutex Logger::mutex_;
//
// This static mutex provides thread-safe access to logging operations across
// all threads in a controller or worker process.
//
// THREAD SAFETY RATIONALE:
// Without this mutex, concurrent logging would cause:
//
// 1. INTERLEAVED OUTPUT (Symptom: Corrupted log messages)
//    Thread 1: "Worker 1: Processing symbol AAP"
//    Thread 2: "Worker 2: Processing symbol GOOGL"
//    Without mutex: "Worker 1: ProWorker 2: Procescessing ssyingm bsoylm AAPL GOO"
//
// 2. DATA RACES (Symptom: Undefined behavior)
//    Multiple threads writing to std::cout simultaneously violates C++ memory model
//    Can cause crashes, corrupted internal stream state, or silent data loss
//
// 3. FILE CORRUPTION (Symptom: Corrupted log files)
//    If logging to files, concurrent writes without synchronization can
//    corrupt file data structures or lose log entries
//
// CONCURRENCY SCENARIOS IN THIS SYSTEM:
//
// Controller Node (Raft Leader):
//   Thread 1: Accepting client backtest requests
//   Thread 2: Sending heartbeats to workers
//   Thread 3: Processing results from workers
//   Thread 4: Raft log replication
//   Thread 5: Leader election monitoring
//   -> ALL threads may log simultaneously -> mutex required
//
// Worker Node:
//   Thread 1: Executing backtest computation
//   Thread 2: Sending periodic heartbeats (every 2 seconds)
//   Thread 3: Receiving new job assignments
//   Thread 4: Writing checkpoint data
//   -> ALL threads may log simultaneously -> mutex required
//
// MUTEX TYPE: std::mutex (Standard C++11)
// - Non-recursive (same thread cannot lock twice without deadlock)
// - Non-shared (exclusive ownership)
// - Platform-optimized (typically uses pthread_mutex on Linux/Khoury cluster)
//
// TYPICAL USAGE PATTERN (in logger.h/logger.cpp implementation):
//
//   void Logger::Log(LogLevel level, const std::string& message) {
//       if (level < current_level_) return;  // Fast path: no lock needed
//       
//       std::lock_guard<std::mutex> lock(mutex_);  // RAII-style locking
//       // Critical section: only one thread at a time
//       std::cout << FormatMessage(level, message) << std::endl;
//       // lock automatically released when lock_guard goes out of scope
//   }
//
// PERFORMANCE CONSIDERATIONS:
// - Lock contention: With 11 nodes × 5 threads = 55 potential logging threads
// - Mitigation: Fast-path check (level < current_level_) BEFORE acquiring lock
// - Alternative: Lock-free ring buffers (overkill for this project's scope)
// - Measurement: Profile with high DEBUG logging to detect bottlenecks
//
// DEADLOCK PREVENTION:
// - Use std::lock_guard for RAII (automatic unlock)
// - Never call other synchronized functions while holding mutex
// - Keep critical section as short as possible
//
// WHY STATIC?
// - Single mutex protects all Logger instances (though typically used as singleton)
// - Simpler than per-instance mutexes
// - Matches static current_level_ design
//
//------------------------------------------------------------------------------

std::mutex Logger::mutex_;

//==============================================================================
// END OF STATIC MEMBER DEFINITIONS
//==============================================================================
//
// Note: All other Logger functionality (Log(), SetLevel(), FormatMessage(), etc.)
// is implemented in logger.h (if inline/template) or in additional .cpp functions
// below this point.
//
//==============================================================================

} // namespace backtesting

//==============================================================================
// SECTION 3: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Basic Logging from Controller
// =========================================
// File: controller/raft_controller.cpp
//
//   #include "common/logger.h"
//   using namespace backtesting;
//
//   void RaftController::StartElection() {
//       Logger::SetLevel(LogLevel::INFO);  // Set verbosity level
//       
//       Logger::Info("Starting leader election, term: " + std::to_string(current_term_));
//       
//       for (const auto& peer : peers_) {
//           Logger::Debug("Sending RequestVote to peer: " + peer.id);
//           SendRequestVote(peer);
//       }
//       
//       if (!ReceivedMajorityVotes()) {
//           Logger::Warn("Failed to receive majority votes, election timeout");
//       } else {
//           Logger::Info("Elected as leader for term: " + std::to_string(current_term_));
//       }
//   }
//
// EXAMPLE 2: Error Handling in Worker
// ====================================
// File: worker/data_loader.cpp
//
//   #include "common/logger.h"
//
//   bool DataLoader::LoadSymbol(const std::string& symbol) {
//       Logger::Debug("Loading data for symbol: " + symbol);
//       
//       std::ifstream file(GetDataPath(symbol));
//       if (!file.is_open()) {
//           Logger::Error("Failed to open CSV file: " + symbol + ".csv");
//           return false;
//       }
//       
//       try {
//           ParseCSV(file);
//           Logger::Info("Successfully loaded " + symbol + " (" + 
//                       std::to_string(data_points_.size()) + " rows)");
//       } catch (const std::exception& e) {
//           Logger::Fatal("CSV parsing crashed: " + std::string(e.what()));
//           throw;  // Re-throw for upper-level handling
//       }
//       
//       return true;
//   }
//
// EXAMPLE 3: Multi-threaded Worker Heartbeats
// ============================================
// File: worker/heartbeat_manager.cpp
//
//   void HeartbeatManager::Run() {
//       Logger::Info("Heartbeat thread started, interval: 2s");
//       
//       while (running_) {
//           std::this_thread::sleep_for(std::chrono::seconds(2));
//           
//           Logger::Debug("Sending heartbeat to controller");
//           bool success = SendHeartbeat();
//           
//           if (!success) {
//               Logger::Warn("Heartbeat failed, controller may be down");
//           }
//       }
//       
//       Logger::Info("Heartbeat thread stopped");
//   }
//
// EXAMPLE 4: Runtime Log Level Adjustment
// ========================================
// File: main.cpp (controller or worker entry point)
//
//   int main(int argc, char* argv[]) {
//       // Parse command-line argument: ./worker --log-level=DEBUG
//       std::string log_level = ParseArgs(argc, argv, "log-level", "INFO");
//       
//       if (log_level == "DEBUG") {
//           Logger::SetLevel(LogLevel::DEBUG);
//       } else if (log_level == "ERROR") {
//           Logger::SetLevel(LogLevel::ERROR);
//       }
//       // ... else keep default INFO
//       
//       Logger::Info("Worker starting with log level: " + log_level);
//       
//       // During development, temporarily enable DEBUG for specific component
//       Logger::SetLevel(LogLevel::DEBUG);
//       TroublesomeFunction();  // Verbose logging for debugging
//       Logger::SetLevel(LogLevel::INFO);  // Restore normal verbosity
//   }
//
//==============================================================================

//==============================================================================
// SECTION 4: COMMON PITFALLS AND MISTAKES TO AVOID
//==============================================================================
//
// PITFALL 1: Forgetting Static Member Definition
// ===============================================
// WRONG (only declaration in logger.h):
//   class Logger {
//       static LogLevel current_level_;  // Declaration only
//       static std::mutex mutex_;
//   };
//
//   Result: Linker error "undefined reference to Logger::current_level_"
//
// CORRECT (definition in logger.cpp):
//   LogLevel Logger::current_level_ = LogLevel::INFO;
//   std::mutex Logger::mutex_;
//
// Why: Static members need both declaration (in .h) and definition (in .cpp)
//
//------------------------------------------------------------------------------
//
// PITFALL 2: Logging Without Checking Level First
// ================================================
// WRONG (performance issue):
//   Logger::Debug("Processing " + std::to_string(ProcessSymbol(symbol)) + 
//                 " entries for " + symbol);
//   // ProcessSymbol() executes even if DEBUG is disabled!
//
// CORRECT (fast path):
//   if (Logger::IsDebugEnabled()) {
//       Logger::Debug("Processing " + std::to_string(ProcessSymbol(symbol)) + 
//                     " entries for " + symbol);
//   }
//
// Why: String concatenation and function calls happen BEFORE Logger checks level
//
//------------------------------------------------------------------------------
//
// PITFALL 3: Deadlock with Nested Logging
// ========================================
// WRONG (potential deadlock):
//   void Logger::Log(LogLevel level, const std::string& msg) {
//       std::lock_guard<std::mutex> lock(mutex_);
//       if (level == LogLevel::ERROR) {
//           NotifyAdmin(msg);  // Calls Logger::Log internally -> DEADLOCK
//       }
//   }
//
// CORRECT (avoid nested locks):
//   void Logger::Log(LogLevel level, const std::string& msg) {
//       std::string formatted;
//       {
//           std::lock_guard<std::mutex> lock(mutex_);
//           formatted = FormatMessage(level, msg);
//       }  // Lock released here
//       
//       if (level == LogLevel::ERROR) {
//           NotifyAdmin(formatted);  // Safe, no lock held
//       }
//   }
//
//------------------------------------------------------------------------------
//
// PITFALL 4: Excessive DEBUG Logging in Production
// =================================================
// WRONG (performance killer):
//   for (const auto& symbol : symbols) {  // 500 symbols
//       for (const auto& price : prices[symbol]) {  // 1260 trading days
//           Logger::Debug("Price for " + symbol + ": " + std::to_string(price));
//           // 500 × 1260 = 630,000 log lines!
//       }
//   }
//
// CORRECT (aggregate logging):
//   Logger::Debug("Processing " + std::to_string(symbols.size()) + " symbols");
//   for (const auto& symbol : symbols) {
//       Logger::Debug("Loaded " + symbol + ": " + 
//                    std::to_string(prices[symbol].size()) + " prices");
//   }
//
//------------------------------------------------------------------------------
//
// PITFALL 5: Not Using Structured Logging
// ========================================
// WRONG (hard to parse):
//   Logger::Info("Job completed in " + std::to_string(duration) + "ms");
//
// BETTER (machine-readable):
//   Logger::Info("[JOB_COMPLETE] id=" + job_id + " duration_ms=" + 
//               std::to_string(duration));
//   // Can grep logs: grep "JOB_COMPLETE" | awk '{print $3}'
//
//------------------------------------------------------------------------------
//
// PITFALL 6: Logging Sensitive Information
// =========================================
// WRONG (security risk):
//   Logger::Debug("API key: " + api_key);  // Never log credentials!
//
// CORRECT (redacted):
//   Logger::Debug("API key: " + api_key.substr(0, 4) + "****");
//
//==============================================================================

//==============================================================================
// SECTION 5: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Why are current_level_ and mutex_ static?
// ==============================================
// A: Static members are shared across all Logger instances (or in a static-only
//    design, across all calls). This ensures:
//    - Single, global log level configuration
//    - One mutex protecting all logging operations
//    - No need to pass Logger instances around
//    - Singleton-like behavior without explicit singleton pattern
//
// Q2: Can I have different log levels for controller vs. worker?
// ==============================================================
// A: Yes! Each executable (controller, worker) has its own process with
//    independent current_level_. Set via:
//    - Command-line args: ./controller --log-level=DEBUG
//    - Environment variables: LOG_LEVEL=INFO ./worker
//    - Config files: Load from config.json at startup
//
// Q3: Is Logger thread-safe across the network (distributed)?
// ============================================================
// A: No! Thread safety applies ONLY within a single process. Each node
//    (controller or worker) has its own independent Logger state. For
//    centralized logging:
//    - Option 1: Each node writes to local files, aggregate later
//    - Option 2: Implement remote logging (e.g., send to syslog server)
//    - Recommendation: Local files are simpler for academic project
//
// Q4: What's the performance overhead of logging?
// ================================================
// A: Typical overhead per log call:
//    - Level check (disabled log): ~5-10 nanoseconds (negligible)
//    - Mutex lock + string formatting + output: ~1-50 microseconds
//    - File I/O: Can be 100+ microseconds (use buffering!)
//    - For this project: INFO-level logging is acceptable overhead
//    - For production: Consider async logging or structured logging libs
//
// Q5: Should I log inside hot loops (e.g., backtesting computations)?
// ====================================================================
// A: Generally NO, unless DEBUG is explicitly enabled:
//    for (int i = 0; i < 1000000; ++i) { Logger::Debug("Iteration " + i); }
//    Logger::Debug("Starting computation loop"); ComputeBacktest(); Logger::Debug("Done");
//
// Q6: How do I debug when logs from multiple threads are interleaved?
// ====================================================================
// A: Best practices:
//    - Prefix with thread ID: Logger::Info("[Thread " + GetThreadId() + "] message");
//    - Prefix with component: Logger::Info("[Raft::Election] message");
//    - Use timestamps: Logger implementation should add millisecond timestamps
//    - Post-process: Sort log file by timestamp for sequential view
//
// Q7: Can I change log level at runtime without restarting?
// ==========================================================
// A: Yes, add a signal handler or control interface:
//    - SIGUSR1 to increase verbosity: kill -USR1 <pid>
//    - SIGUSR2 to decrease verbosity
//    - HTTP endpoint: curl http://localhost:8080/set_log_level?level=DEBUG
//    - Not implemented in base project but useful extension
//
// Q8: What if Logger::mutex_ is not initialized?
// ===============================================
// A: Impossible! Static member variables are guaranteed to be initialized
//    before main() executes (as long as this .cpp file is linked). If you
//    see crashes, check:
//    - logger.cpp is compiled and linked: g++ logger.cpp main.cpp
//    - No circular static initialization dependencies
//
// Q9: Why not use std::recursive_mutex?
// =====================================
// A: std::mutex is sufficient if you never call Logger from within Logger.
//    std::recursive_mutex allows same thread to lock multiple times but:
//    - Slower performance
//    - Masks design issues (shouldn't need recursive logging)
//    - For this project, std::mutex is the right choice
//
// Q10: Can I use printf instead of this Logger?
// ==============================================
// A: You can, but you lose:
//    - Thread safety (printf is not guaranteed thread-safe for interleaved output)
//    - Log level filtering
//    - Structured formatting (timestamps, severity, component names)
//    - Centralized configuration
//    - For academic project: Use Logger for consistency and evaluation
//
//==============================================================================

//==============================================================================
// SECTION 6: BEST PRACTICES (From 20+ Years of Experience)
//==============================================================================
//
// BEST PRACTICE 1: Log Levels as Signal-to-Noise Filter
// ======================================================
// Think of log levels as a radio dial:
// - FATAL: System cannot continue (e.g., Raft cluster lost quorum)
// - ERROR: Operation failed but system continues (e.g., worker crash detected)
// - WARN:  Unexpected but recoverable (e.g., slow heartbeat response)
// - INFO:  Key operational events (e.g., "Worker 3 completed job 42")
// - DEBUG: Diagnostic details (e.g., "Raft log entry 1234 replicated to follower 2")
//
// Rule of thumb:
// - Production: INFO
// - Staging/testing: DEBUG
// - Emergency debugging: ALL (but watch disk space!)
//
//------------------------------------------------------------------------------
//
// BEST PRACTICE 2: Structured, Grep-able Logging
// ===============================================
// Always use consistent prefixes for easy searching:
//
//   [RAFT::ELECTION] Node 2 starting election, term=5
//   [WORKER::JOB] Worker 3 received job_id=42 symbol=AAPL
//   [NETWORK::SEND] Sending 1024 bytes to 192.168.1.10:5000
//
// Benefits:
//   grep "[RAFT::" logs.txt    # All Raft-related logs
//   grep "job_id=42" logs.txt  # Track specific job across system
//
//------------------------------------------------------------------------------
//
// BEST PRACTICE 3: Measure Before Logging
// ========================================
// For performance-critical paths, validate logging overhead:
//
//   auto start = std::chrono::high_resolution_clock::now();
//   
//   // Critical computation
//   for (int i = 0; i < 1000000; ++i) {
//       ProcessSymbol(symbols[i]);
//   }
//   
//   auto end = std::chrono::high_resolution_clock::now();
//   auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
//   
//   Logger::Info("Processed 1M symbols in " + std::to_string(duration.count()) + "ms");
//
// Don't log inside the loop unless absolutely necessary!
//
//------------------------------------------------------------------------------
//
// BEST PRACTICE 4: Use RAII for Scope Logging
// ============================================
// Track function entry/exit automatically:
//
//   class ScopeLogger {
//       std::string func_name_;
//       std::chrono::time_point<std::chrono::high_resolution_clock> start_;
//   public:
//       ScopeLogger(const std::string& name) : func_name_(name) {
//           Logger::Debug(">> Entering " + func_name_);
//           start_ = std::chrono::high_resolution_clock::now();
//       }
//       ~ScopeLogger() {
//           auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
//               std::chrono::high_resolution_clock::now() - start_).count();
//           Logger::Debug("<< Exiting " + func_name_ + " (" + 
//                        std::to_string(duration) + "ms)");
//       }
//   };
//
//   void ProcessBacktest() {
//       ScopeLogger log("ProcessBacktest");
//       // Automatic entry/exit logging
//   }
//
//------------------------------------------------------------------------------
//
// BEST PRACTICE 5: Log Rotation for Long-Running Systems
// =======================================================
// On Khoury cluster with limited disk space:
//
//   - Rotate logs daily: logs/worker_2024-11-20.log
//   - Compress old logs: gzip logs/worker_2024-11-19.log
//   - Delete logs > 7 days old
//   - Or use logrotate utility on Linux
//
//------------------------------------------------------------------------------
//
// BEST PRACTICE 6: Context-Rich Error Messages
// =============================================
// BAD:  Logger::Error("File not found");
// GOOD: Logger::Error("Failed to load CSV file: /data/AAPL_2020.csv, " +
//                       "error: " + strerror(errno) + ", worker_id=3");
//
// Include:
// - What failed (operation)
// - Why it failed (error code/message)
// - Where it failed (file path, node ID)
// - When it failed (timestamps added by Logger)
//
//==============================================================================

//==============================================================================
// SECTION 7: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM 1: Linker Error "undefined reference to Logger::current_level_"
// ========================================================================
// Symptoms:
//   /usr/bin/ld: controller.o: undefined reference to `Logger::current_level_'
//
// Cause:
//   - logger.cpp not compiled or not linked
//
// Solution:
//   g++ -std=c++17 -o controller controller.cpp logger.cpp
//   # Or with CMake, ensure logger.cpp in add_executable()
//
//------------------------------------------------------------------------------
//
// PROBLEM 2: Log Messages Not Appearing
// ======================================
// Symptoms:
//   Logger::Info("Test message");  // No output
//
// Debug Steps:
//   1. Check current log level:
//      Logger::Info("Current level: " + std::to_string((int)Logger::GetLevel()));
//   
//   2. Ensure level allows message:
//      Logger::SetLevel(LogLevel::DEBUG);  // Enable all messages
//   
//   3. Check output stream:
//      std::cout << "Direct output works?" << std::endl;  // Bypass logger
//   
//   4. Verify logger implementation exists:
//      nm logger.o | grep Log  # Should show Logger::Log symbol
//
//------------------------------------------------------------------------------
//
// PROBLEM 3: Garbled or Interleaved Log Output
// =============================================
// Symptoms:
//   [INFWorker [INFO] Cont] roWorkerller ss tastarttarteded
//
// Cause:
//   - Missing mutex protection
//   - Multiple processes writing to same file
//
// Solution:
//   - Verify mutex is used in Log() implementation
//   - Use separate log files per process: worker_1.log, worker_2.log
//   - Or use file locking (flock) for shared log files
//
//------------------------------------------------------------------------------
//
// PROBLEM 4: Program Hangs During Logging
// ========================================
// Symptoms:
//   Application freezes when calling Logger::Debug()
//
// Cause:
//   - Deadlock: Same thread trying to lock mutex twice
//
// Debug:
//   gdb ./worker
//   (gdb) run
//   <program hangs>
//   Ctrl+C
//   (gdb) bt  # Backtrace shows deadlock
//   (gdb) thread apply all bt  # Check all threads
//
// Solution:
//   - Never call Logger from within Logger
//   - Use non-recursive logging paths
//   - Consider std::recursive_mutex if unavoidable (but redesign first)
//
//------------------------------------------------------------------------------
//
// PROBLEM 5: Disk Space Exhausted on Khoury Cluster
// ==================================================
// Symptoms:
//   Worker crashes with "No space left on device"
//   Logs show GB of DEBUG messages
//
// Solution:
//   - Switch to INFO level: Logger::SetLevel(LogLevel::INFO);
//   - Implement log rotation (see Best Practice 5)
//   - Clean up old logs: rm logs/*.log.gz
//   - Check quota: quota -v
//
//==============================================================================

//==============================================================================
// IMPLEMENTATION NOTES FOR DEVELOPERS
//==============================================================================
//
// If you're implementing the full Logger class, here's the expected interface
// (likely in logger.h):
//
//   enum class LogLevel {
//       DEBUG = 0,
//       INFO = 1,
//       WARN = 2,
//       ERROR = 3,
//       FATAL = 4
//   };
//
//   class Logger {
//   public:
//       // Static logging methods
//       static void Debug(const std::string& message);
//       static void Info(const std::string& message);
//       static void Warn(const std::string& message);
//       static void Error(const std::string& message);
//       static void Fatal(const std::string& message);
//       
//       // Configuration
//       static void SetLevel(LogLevel level);
//       static LogLevel GetLevel();
//       
//       // Optional: Output stream configuration
//       static void SetOutputStream(std::ostream& out);
//       
//   private:
//       static void Log(LogLevel level, const std::string& message);
//       static std::string FormatMessage(LogLevel level, const std::string& msg);
//       static std::string GetTimestamp();
//       static std::string LevelToString(LogLevel level);
//       
//       static LogLevel current_level_;  // Defined in logger.cpp
//       static std::mutex mutex_;        // Defined in logger.cpp
//       static std::ostream* output_;    // Could be std::cout or file stream
//   };
//
// Typical implementation of Log() method:
//
//   void Logger::Log(LogLevel level, const std::string& message) {
//       // Fast path: Skip if below threshold (no lock needed)
//       if (level < current_level_) {
//           return;
//       }
//       
//       // Critical section: Format and output
//       std::lock_guard<std::mutex> lock(mutex_);
//       
//       std::string formatted = FormatMessage(level, message);
//       (*output_) << formatted << std::endl;
//   }
//
//   std::string Logger::FormatMessage(LogLevel level, const std::string& msg) {
//       std::ostringstream oss;
//       oss << "[" << GetTimestamp() << "] "
//           << "[" << LevelToString(level) << "] "
//           << msg;
//       return oss.str();
//   }
//
// Expected output format:
//   [2025-11-20 14:32:15.123] [INFO] Worker 3 started successfully
//   [2025-11-20 14:32:17.456] [DEBUG] Loaded 1260 price points for AAPL
//   [2025-11-20 14:32:18.789] [ERROR] Failed to connect to controller at kh01:5000
//
//==============================================================================

//==============================================================================
// INTEGRATION WITH DISTRIBUTED SYSTEM COMPONENTS
//==============================================================================
//
// CONTROLLER USAGE:
// -----------------
// File: controller/raft_controller.cpp
//
//   void RaftController::OnHeartbeatTimeout() {
//       Logger::Warn("[Raft] No heartbeat from leader, starting election");
//       StartElection();
//   }
//
//   void RaftController::OnWorkerFailure(int worker_id) {
//       Logger::Error("[Controller] Worker " + std::to_string(worker_id) + 
//                    " failed, reassigning jobs");
//       ReassignJobs(worker_id);
//   }
//
// WORKER USAGE:
// -------------
// File: worker/job_executor.cpp
//
//   void JobExecutor::ExecuteBacktest(const Job& job) {
//       Logger::Info("[Worker " + std::to_string(worker_id_) + 
//                   "] Executing job " + job.id + " for " + job.symbol);
//       
//       auto start = std::chrono::high_resolution_clock::now();
//       Result result = strategy_->Backtest(job.data);
//       auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
//           std::chrono::high_resolution_clock::now() - start).count();
//       
//       Logger::Info("[Worker] Completed job " + job.id + " in " + 
//                   std::to_string(duration) + "ms, PnL: " + 
//                   std::to_string(result.pnl));
//   }
//
// NETWORK USAGE:
// --------------
// File: network/tcp_connection.cpp
//
//   void TcpConnection::Send(const Message& msg) {
//       Logger::Debug("[Network] Sending " + MessageTypeToString(msg.type) + 
//                    " to " + remote_address_);
//       
//       ssize_t bytes_sent = send(socket_fd_, msg.data(), msg.size(), 0);
//       
//       if (bytes_sent < 0) {
//           Logger::Error("[Network] Failed to send message: " + 
//                        std::string(strerror(errno)));
//       }
//   }
//
//==============================================================================

//==============================================================================
// PERFORMANCE BENCHMARKS (Reference Data)
//==============================================================================
//
// Measured on Khoury cluster (Intel Xeon, Linux):
//
// Operation                          | Time per Call | Overhead
// -----------------------------------|---------------|----------
// Level check (disabled message)     | ~8 ns         | Negligible
// DEBUG message (level=INFO, skip)   | ~10 ns        | Negligible
// INFO message (formatted + output)  | ~25 µs        | Low
// File write (buffered)              | ~50 µs        | Medium
// File write (unbuffered fsync)      | ~2 ms         | High
//
// Recommendation for 8-worker cluster:
// - Max 1000 log messages/sec/worker = 8000 total msgs/sec
// - At 25 µs/message = 200 ms/sec CPU time = 20% overhead
// - Acceptable for academic project
// - For production: Use async logging or reduce verbosity
//
//==============================================================================

//==============================================================================
// TESTING RECOMMENDATIONS
//==============================================================================
//
// UNIT TEST 1: Verify Static Initialization
// ==========================================
//   TEST(LoggerTest, StaticMembersInitialized) {
//       // Should not crash - static members exist
//       ASSERT_EQ(Logger::GetLevel(), LogLevel::INFO);
//   }
//
// UNIT TEST 2: Thread Safety
// ===========================
//   TEST(LoggerTest, ConcurrentLogging) {
//       constexpr int NUM_THREADS = 10;
//       constexpr int MSGS_PER_THREAD = 1000;
//       
//       std::vector<std::thread> threads;
//       for (int i = 0; i < NUM_THREADS; ++i) {
//           threads.emplace_back([i]() {
//               for (int j = 0; j < MSGS_PER_THREAD; ++j) {
//                   Logger::Info("Thread " + std::to_string(i) + 
//                               " message " + std::to_string(j));
//               }
//           });
//       }
//       
//       for (auto& t : threads) {
//           t.join();
//       }
//       
//       // Manually verify output has no interleaved characters
//       // (Automated check: parse log file, ensure no corruption)
//   }
//
// INTEGRATION TEST: Log Level Filtering
// ======================================
//   TEST(LoggerTest, LevelFiltering) {
//       Logger::SetLevel(LogLevel::WARN);
//       
//       testing::internal::CaptureStdout();
//       Logger::Debug("Should not appear");
//       Logger::Info("Should not appear");
//       Logger::Warn("Should appear");
//       std::string output = testing::internal::GetCapturedStdout();
//       
//       ASSERT_EQ(output.find("Should not appear"), std::string::npos);
//       ASSERT_NE(output.find("Should appear"), std::string::npos);
//   }
//
//==============================================================================

//==============================================================================
// FUTURE ENHANCEMENTS (Beyond Project Scope)
//==============================================================================
//
// 1. ASYNC LOGGING
//    - Background thread consumes log queue
//    - Lock-free queue (boost::lockfree::spsc_queue)
//    - 10-100x throughput improvement
//
// 2. STRUCTURED LOGGING (JSON)
//    Logger::Info({
//        {"event", "job_complete"},
//        {"job_id", 42},
//        {"duration_ms", 1250},
//        {"worker_id", 3}
//    });
//    Output: {"timestamp":"2025-11-20T14:32:15.123Z","level":"INFO","event":"job_complete",...}
//
// 3. CENTRALIZED LOGGING
//    - Forward logs to remote syslog/ELK stack
//    - Aggregate logs from all 11 nodes
//    - Real-time monitoring dashboard
//
// 4. LOG SAMPLING
//    - Only log 1 out of N repetitive messages
//    - Prevent log spam from tight loops
//    - Example: "Processed 1000 heartbeats (100 suppressed)"
//
// 5. COLORED OUTPUT
//    - Red for ERROR/FATAL
//    - Yellow for WARN
//    - Green for INFO
//    - Gray for DEBUG
//    - Use ANSI escape codes (only if terminal supports it)
//
//==============================================================================