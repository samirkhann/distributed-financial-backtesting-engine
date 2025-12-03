/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: logger.h
    
    Description:
        This header file defines the Logger class interface and implementation
        for the distributed financial backtesting system. The Logger provides
        thread-safe, level-filtered logging capabilities with millisecond-precision
        timestamps essential for debugging distributed systems.
        
        Core Features:
        - Thread-safe logging using std::mutex for multi-threaded controllers/workers
        - Configurable log levels (DEBUG, INFO, WARNING, ERROR)
        - Millisecond-precision timestamps for event correlation across nodes
        - Zero-overhead filtering (messages below threshold never formatted)
        - Header-only implementation (all methods inline for performance)
        
    Design Philosophy:
        In distributed systems with 3 controller nodes + 8 worker nodes running
        concurrent operations (Raft consensus, job scheduling, heartbeats, 
        backtesting computations), logging is critical for:
        
        1. DEBUGGING: Trace Raft leader elections and log replication
        2. MONITORING: Track job assignments and worker health
        3. PERFORMANCE: Identify bottlenecks in distributed computations
        4. FAILURE ANALYSIS: Reconstruct event sequences during crashes
        5. EVALUATION: Measure system metrics (speedup, fault tolerance)
        
    Thread Safety Model:
        - Static mutex protects all logging operations
        - Lock acquired ONLY after level check (fast path for filtered messages)
        - RAII-style locking via std::lock_guard (automatic cleanup)
        - Safe for use from any thread in controller/worker processes
        
    Integration Points:
        - Controller: Raft consensus events, worker failure detection
        - Worker: Job execution, heartbeat transmission, checkpoint creation
        - Network: Connection establishment, message transmission errors
        - Strategy: Backtest computation progress, result calculation
        
    Performance Characteristics:
        - Disabled log check: ~8-10 nanoseconds (integer comparison)
        - Enabled log message: ~25-50 microseconds (lock + format + output)
        - Timestamp generation: ~2-5 microseconds (system call)
        - String concatenation: ~1-10 microseconds depending on message size
        
    Header-Only Design Rationale:
        All methods are defined inline in the header for:
        - Simplicity: No separate .cpp file needed (logger.cpp only defines statics)
        - Compiler optimization: Inline functions can be optimized at call site
        - Template-style convenience: Easy to include anywhere
        - Trade-off: Slightly longer compilation time, but negligible for this project
        
    Related Files:
        - common/logger.cpp: Static member definitions (current_level_, mutex_)
        - All distributed system components: Include this header for logging
        
    Typical Usage:
        #include "common/logger.h"
        using namespace backtesting;
        
        Logger::set_level(LogLevel::INFO);  // Configure verbosity
        Logger::info("Controller started, waiting for workers...");
        Logger::debug("Detailed diagnostic info");  // Filtered if level=INFO
        Logger::error("Failed to connect to controller at kh01:5000");

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. HEADER GUARD & PREPROCESSOR
// 2. INCLUDE DEPENDENCIES
// 3. NAMESPACE DECLARATION
// 4. LOG LEVEL ENUMERATION
//    4.1 LogLevel Design & Hierarchy
//    4.2 Severity Ordering
// 5. LOGGER CLASS DECLARATION
//    5.1 Private Static Members
//    5.2 Private Helper Methods
//        5.2.1 Timestamp Generation (get_timestamp)
//        5.2.2 Level-to-String Conversion (level_to_string)
//    5.3 Public Interface
//        5.3.1 Configuration (set_level)
//        5.3.2 Core Logging (log)
//        5.3.3 Convenience Methods (debug, info, warning, error)
// 6. USAGE EXAMPLES
//    6.1 Basic Logging
//    6.2 Multi-threaded Scenarios
//    6.3 Distributed System Integration
// 7. COMMON PITFALLS & SOLUTIONS
// 8. FAQ
// 9. BEST PRACTICES
// 10. PERFORMANCE OPTIMIZATION GUIDE
// 11. TESTING STRATEGIES
// 12. TROUBLESHOOTING CHECKLIST
//
//==============================================================================

//==============================================================================
// SECTION 1: HEADER GUARD & PREPROCESSOR
//==============================================================================
//
// #ifndef LOGGER_H / #define LOGGER_H / #endif
//
// HEADER GUARD PURPOSE:
// Prevents multiple inclusion of this header file during compilation, which
// would cause:
// - Redefinition errors for classes, enums, and inline functions
// - Increased compilation time
// - Potential ODR (One Definition Rule) violations
//
// HOW IT WORKS:
// 1. First inclusion: LOGGER_H undefined -> define it, process content
// 2. Second inclusion: LOGGER_H already defined -> skip entire file
// 3. Result: Class defined exactly once per translation unit
//
// MODERN ALTERNATIVE: #pragma once
// Many compilers support #pragma once as a simpler alternative:
//   #pragma once  // Non-standard but widely supported
//
// However, #ifndef/#define/#endif is:
// - C++ standard compliant (guaranteed portable)
// - Preferred for academic/evaluation projects
// - Explicitly shows intent to reviewers
//
// NAMING CONVENTION:
// - LOGGER_H matches filename (logger.h)
// - Uppercase with underscores (standard C++ practice)
// - Alternative: BACKTESTING_COMMON_LOGGER_H (prevents conflicts in large projects)
//
//==============================================================================

#ifndef LOGGER_H
#define LOGGER_H

//==============================================================================
// SECTION 2: INCLUDE DEPENDENCIES
//==============================================================================
//
// This section imports Standard Library components required by Logger.
// Each include is carefully chosen to minimize compilation overhead.
//
//------------------------------------------------------------------------------

// <string> - std::string for log messages
// ========================================
// Required for:
// - Message parameter type: log(LogLevel level, const std::string& message)
// - Return type of helper methods: get_timestamp(), level_to_string()
//
// Memory considerations:
// - std::string uses Small String Optimization (SSO) for short messages
// - Typical SSO threshold: 15-23 characters (no heap allocation)
// - Longer messages: Single heap allocation + automatic memory management
//
// Alternative considered: std::string_view (C++17)
// - Would be more efficient (no copy for temporaries)
// - Not used here because we need to own the timestamp string
// - Could be added as overload: log(LogLevel level, std::string_view message)
//
#include <string>

// <iostream> - std::cout for output stream
// =========================================
// Required for:
// - Output target: std::cout << formatted_message << std::endl
//
// Thread safety note:
// - std::cout operations are NOT atomic
// - Multiple threads can interleave output: "Hel" "Wor" "lo " "ld"
// - This is why we use mutex_ to protect the entire output operation
//
// Output buffering:
// - std::cout is line-buffered (flushes on \n) or full-buffered
// - std::endl flushes immediately (slower but ensures visibility)
// - Trade-off: We use std::endl for real-time visibility despite performance cost
//
// Alternative considered: std::ofstream for file output
// - Could be added as future enhancement
// - Would require additional configuration (log file path)
// - For now, stdout is sufficient for Khoury cluster evaluation
//
#include <iostream>

// <sstream> - std::stringstream for string building
// ==================================================
// Required for:
// - Timestamp formatting: Build "[2025-11-20 14:32:15.123]" string
// - Alternative to manual string concatenation (more readable)
//
// Performance note:
// - std::stringstream is slower than raw char buffer manipulation
// - But difference is negligible (~1-2 microseconds) for our use case
// - Code clarity outweighs micro-optimization here
//
// Example usage in get_timestamp():
//   std::stringstream ss;
//   ss << std::put_time(...) << '.' << ms.count();
//   return ss.str();  // Builds final string
//
#include <sstream>

// <mutex> - std::mutex and std::lock_guard for thread safety
// ===========================================================
// Required for:
// - Thread synchronization: static std::mutex mutex_;
// - RAII locking: std::lock_guard<std::mutex> lock(mutex_);
//
// Why std::mutex (not std::recursive_mutex)?
// - std::mutex: Same thread cannot lock twice (deadlock if attempted)
// - std::recursive_mutex: Same thread CAN lock multiple times
// - We use std::mutex because:
//   * Faster (no recursion tracking overhead)
//   * Safer (exposes design flaws if we accidentally nest locks)
//   * Sufficient (Logger methods never call each other with lock held)
//
// Why std::lock_guard (not std::unique_lock)?
// - std::lock_guard: Simple RAII, always locks on construction
// - std::unique_lock: More flexible (can defer locking, transfer ownership)
// - We use std::lock_guard because:
//   * Simpler and faster
//   * We always want to lock immediately
//   * No need for deferred locking or manual unlock
//
// Thread safety guarantee:
// - Only ONE thread can execute inside the critical section at a time
// - All other threads block on mutex acquisition
// - Automatic unlock when lock_guard destructor runs (even with exceptions)
//
#include <mutex>

// <chrono> - std::chrono for high-precision timestamps
// =====================================================
// Required for:
// - Current time: std::chrono::system_clock::now()
// - Time conversion: to_time_t(), duration_cast()
// - Millisecond precision: duration_cast<milliseconds>(...)
//
// Why std::chrono (not time() or gettimeofday())?
// - Type-safe (no raw integers or void*)
// - Cross-platform (works on Linux Khoury cluster, macOS, Windows)
// - High-resolution (microsecond or better on modern systems)
// - Modern C++11 standard
//
// Precision levels:
// - std::chrono::system_clock: Wall-clock time (what humans see)
// - std::chrono::steady_clock: Monotonic time (for intervals, unaffected by clock changes)
// - std::chrono::high_resolution_clock: Highest available precision
//
// We use system_clock because:
// - Need actual wall-clock time for log timestamps
// - Must correlate events across multiple machines (Khoury cluster nodes)
// - Millisecond precision sufficient for debugging distributed systems
//
// Timezone handling:
// - system_clock::now() returns UTC time
// - std::localtime() converts to local timezone (EST for Khoury cluster)
// - For production: Consider UTC timestamps for global coordination
//
#include <chrono>

// <iomanip> - std::put_time for timestamp formatting
// ===================================================
// Required for:
// - Time formatting: std::put_time(localtime(&time), "%Y-%m-%d %H:%M:%S")
// - Output manipulators: std::setfill('0'), std::setw(3)
//
// Format specifiers used:
// - %Y: 4-digit year (2025)
// - %m: 2-digit month (01-12)
// - %d: 2-digit day (01-31)
// - %H: 2-digit hour, 24-hour format (00-23)
// - %M: 2-digit minute (00-59)
// - %S: 2-digit second (00-59)
//
// Example output: [2025-11-20 14:32:15.123]
//                  ^^^^    ^^    ^^       ^^^
//                  Year  Month  Day       Milliseconds (added separately)
//
// std::setfill('0') + std::setw(3):
// - Ensures milliseconds always 3 digits: "001", "010", "123"
// - Without: "1", "10", "123" (inconsistent width, breaks parsing)
//
// Alternative considered: strftime()
// - C-style function, less type-safe
// - std::put_time is the C++ wrapper, preferred in modern code
//
#include <iomanip>

//==============================================================================
// SECTION 3: NAMESPACE DECLARATION
//==============================================================================
//
// namespace backtesting { ... }
//
// PURPOSE:
// Encapsulates all distributed backtesting system components to prevent
// naming conflicts with:
// - Standard library (e.g., std::)
// - Third-party libraries (e.g., boost::, nlohmann::)
// - User code in other projects
//
// DESIGN RATIONALE:
// Without namespace:
//   class Logger { ... };  // Global namespace
//   // Conflict if another library also defines "Logger"
//
// With namespace:
//   namespace backtesting {
//       class Logger { ... };
//   }
//   // Full name: backtesting::Logger (no conflict)
//
// USAGE PATTERNS:
//
// 1. Explicit namespace qualification (verbose but clear):
//    backtesting::Logger::info("Message");
//
// 2. Namespace alias (shorter, still explicit):
//    namespace bt = backtesting;
//    bt::Logger::info("Message");
//
// 3. Using declaration (import specific name):
//    using backtesting::Logger;
//    Logger::info("Message");
//
// 4. Using directive (import entire namespace, use sparingly):
//    using namespace backtesting;
//    Logger::info("Message");
//
// BEST PRACTICE FOR THIS PROJECT:
// - In .cpp files: "using namespace backtesting;" at top (file scope)
// - In .h files: Avoid "using namespace" (can pollute user's namespace)
// - In local scopes: Use explicit backtesting:: prefix
//
// NAMESPACE ORGANIZATION:
//   backtesting::           (root namespace)
//   ├── Logger              (this class)
//   ├── LogLevel            (this enum)
//   ├── RaftController      (controller/raft_controller.h)
//   ├── Worker              (worker/worker.h)
//   ├── Strategy            (strategy/strategy.h)
//   └── ...
//
//==============================================================================

namespace backtesting {

//==============================================================================
// SECTION 4: LOG LEVEL ENUMERATION
//==============================================================================
//
// enum class LogLevel { ... }
//
// OVERVIEW:
// Defines the severity/importance hierarchy for log messages, enabling
// runtime filtering to control verbosity and reduce log volume.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.1 LOGLEVEL DESIGN & HIERARCHY
//------------------------------------------------------------------------------
//
// enum class vs. enum (unscoped)
// ==============================
//
// MODERN C++11 ENUM CLASS (used here):
//   enum class LogLevel {
//       DEBUG = 0,
//       INFO = 1
//   };
//   Usage: LogLevel::DEBUG, LogLevel::INFO
//
// OLD C-STYLE ENUM (avoid):
//   enum LogLevel {
//       DEBUG = 0,
//       INFO = 1
//   };
//   Usage: DEBUG, INFO (pollutes namespace!)
//
// Why enum class is better:
// 1. SCOPED: Must write LogLevel::DEBUG (prevents naming conflicts)
// 2. STRONGLY TYPED: Cannot accidentally convert to int without cast
// 3. NO IMPLICIT CONVERSIONS: LogLevel level = 1; // ERROR (good!)
// 4. MODERN C++ BEST PRACTICE: Preferred since C++11
//
// Underlying Type:
// - Implicitly int by default
// - Could be explicit: enum class LogLevel : uint8_t { ... } (saves memory)
// - int is fine here (only 4 values, performance not critical)
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.2 SEVERITY ORDERING & VALUES
//------------------------------------------------------------------------------
//
// Severity increases from DEBUG (0) to ERROR (3):
//
//   DEBUG (0)    - Most verbose, detailed diagnostics
//   INFO (1)     - Standard operational messages
//   WARNING (2)  - Unexpected but non-critical conditions
//   ERROR (3)    - Critical failures requiring attention
//
// NUMERIC VALUES RATIONALE:
// - Explicit values (= 0, = 1, ...) for clarity (though implicit works too)
// - Sequential integers enable simple comparison: level < current_level_
// - Lower number = more verbose (standard convention in logging frameworks)
//
// COMPARISON EXAMPLES:
//   if (LogLevel::DEBUG < LogLevel::INFO)    // TRUE (0 < 1)
//   if (LogLevel::ERROR > LogLevel::WARNING) // TRUE (3 > 2)
//
// This ordering enables the core filtering logic in log():
//   if (level < current_level_) return;  // Skip less severe messages
//
//   Example: current_level_ = INFO (1)
//   - DEBUG (0) < INFO (1) -> Skip
//   - INFO (1) < INFO (1) -> FALSE, process
//   - WARNING (2) < INFO (1) -> FALSE, process
//   - ERROR (3) < INFO (1) -> FALSE, process
//
// ALTERNATIVE DESIGNS CONSIDERED:
//
// 1. Bitflags (used by some logging systems):
//    enum class LogLevel {
//        DEBUG = 1 << 0,    // 0b0001
//        INFO = 1 << 1,     // 0b0010
//        WARNING = 1 << 2,  // 0b0100
//        ERROR = 1 << 3     // 0b1000
//    };
//    Allows: enable_mask = DEBUG | INFO | ERROR; (flexible but complex)
//    NOT NEEDED: Sequential levels are simpler for this project
//
// 2. String-based levels:
//    enum class LogLevel { DEBUG, INFO, WARNING, ERROR };
//    // No numeric values
//    Requires: Custom comparison operator
//    NOT IDEAL: Integer comparison is simpler and faster
//
//------------------------------------------------------------------------------

enum class LogLevel {
    // DEBUG: Detailed diagnostic information for development/troubleshooting
    // =====================================================================
    // Use cases:
    // - Raft consensus: "Sending AppendEntries to follower 2, log index 1234"
    // - Worker: "Loaded CSV row 12500 for symbol AAPL"
    // - Network: "Received 1024 bytes from 192.168.1.10:5000"
    //
    // Characteristics:
    // - Very verbose (can generate 1000s of messages/second)
    // - Should be DISABLED in production (performance impact)
    // - Essential for debugging complex distributed system issues
    //
    // When to use:
    // - Development: Always enable during initial implementation
    // - Debugging: Enable for specific failing component
    // - Production: Disable unless actively troubleshooting
    //
    DEBUG = 0,
    
    // INFO: General informational messages about system operation
    // ===========================================================
    // Use cases:
    // - System lifecycle: "Controller started, listening on port 5000"
    // - Job tracking: "Worker 3 started processing job 42 (symbol: AAPL)"
    // - State changes: "Raft node 1 elected as leader, term 5"
    //
    // Characteristics:
    // - Moderate verbosity (10-100 messages/second typical)
    // - DEFAULT level for production systems
    // - Provides operational visibility without excessive noise
    //
    // When to use:
    // - Production: Default setting for normal operations
    // - Monitoring: Key events that should appear in dashboards
    // - Auditing: Important state transitions and completions
    //
    INFO = 1,
    
    // WARNING: Unexpected conditions that don't prevent operation
    // ============================================================
    // Use cases:
    // - Performance: "Heartbeat response delayed by 500ms"
    // - Resources: "Worker queue length exceeded 100 pending jobs"
    // - Retries: "Raft log replication retry attempt 2/5"
    //
    // Characteristics:
    // - Low volume (should be rare, <10 messages/minute)
    // - Indicates potential problems but system continues
    // - Should be investigated but not urgent
    //
    // When to use:
    // - Degraded performance: Slower than expected but functional
    // - Resource limits: Approaching but not exceeding limits
    // - Recoverable errors: Retry succeeded but took multiple attempts
    //
    // WARNING vs. ERROR distinction:
    // - WARNING: "Tried to connect 3 times, succeeded on retry"
    // - ERROR: "Failed to connect after 5 retries, giving up"
    //
    WARNING = 2,
    
    // ERROR: Critical failures that prevent normal operation
    // =======================================================
    // Use cases:
    // - Fatal failures: "Raft cluster lost quorum (only 1/3 nodes alive)"
    // - Data loss: "Worker 3 crashed mid-job, checkpoint corrupted"
    // - Resource exhaustion: "Out of memory, cannot allocate job buffer"
    //
    // Characteristics:
    // - Very low volume (should be extremely rare)
    // - Always logged regardless of configuration
    // - Requires immediate attention/investigation
    //
    // When to use:
    // - Unrecoverable errors: Cannot proceed without intervention
    // - Data corruption: Integrity violations detected
    // - System failures: Critical components unavailable
    //
    // Response to ERROR logs:
    // - Alert operations team (if in production)
    // - Trigger failover (if configured)
    // - Generate incident report
    // - May terminate process (FATAL equivalent)
    //
    ERROR = 3
};

//------------------------------------------------------------------------------
// ENUM CLASS USAGE TIPS:
//------------------------------------------------------------------------------
//
// 1. ALWAYS use scope prefix:
//    LogLevel::INFO
//    INFO (won't compile with enum class)
//
// 2. Comparing levels (in application code):
//    if (severity >= LogLevel::WARNING) {
//        AlertOperations();
//    }
//
// 3. Converting to integer (if needed, though usually avoid):
//    int level_value = static_cast<int>(LogLevel::INFO);  // 1
//
// 4. Converting from integer (dangerous, validate first):
//    int user_input = 2;
//    if (user_input >= 0 && user_input <= 3) {
//        LogLevel level = static_cast<LogLevel>(user_input);
//    }
//
// 5. Switching on log level (in application code):
//    switch (current_level) {
//        case LogLevel::DEBUG:   /* ... */ break;
//        case LogLevel::INFO:    /* ... */ break;
//        case LogLevel::WARNING: /* ... */ break;
//        case LogLevel::ERROR:   /* ... */ break;
//    }
//
//------------------------------------------------------------------------------

//==============================================================================
// SECTION 5: LOGGER CLASS DECLARATION
//==============================================================================
//
// class Logger { ... };
//
// OVERVIEW:
// Thread-safe logging utility with level-based filtering, designed for
// distributed systems with multiple concurrent threads (controller Raft
// operations, worker computations, heartbeat threads, network I/O).
//
// DESIGN PATTERN: Static-only class (no instances)
// ================================================
// All members are static -> Logger is effectively a namespace with access control
//
// Benefits:
// - Global access: Logger::info("...") from anywhere
// - Single configuration: One current_level_ for entire process
// - No initialization: No need to pass Logger instance around
// - Zero overhead: No vtables, no dynamic dispatch
//
// Alternative considered: Singleton pattern
// - Would require: Logger::instance().info("...")
// - More boilerplate: GetInstance(), private constructor, delete copy/move
// - NOT NEEDED: Static-only design is simpler for this use case
//
// THREAD SAFETY MODEL:
// ===================
// - Static mutex_ protects all shared mutable state
// - Lock acquired ONLY in critical sections (after level check)
// - RAII-style locking prevents deadlocks from early returns/exceptions
//
// HEADER-ONLY IMPLEMENTATION:
// ===========================
// All method bodies defined inline in this header for:
// - Convenience: Single file to include
// - Optimization: Compiler can inline at call sites
// - Simplicity: No .cpp/.h coordination needed (except static member definitions)
//
//==============================================================================

class Logger {
private:
    //==========================================================================
    // SECTION 5.1: PRIVATE STATIC MEMBERS
    //==========================================================================
    //
    // These members maintain global logging state across all threads/calls.
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // static LogLevel current_level_;
    //--------------------------------------------------------------------------
    //
    // DECLARED HERE, DEFINED IN logger.cpp:
    //   LogLevel Logger::current_level_ = LogLevel::INFO;
    //
    // PURPOSE:
    // Minimum severity level for messages to be output. Messages with level
    // below this threshold are silently discarded (fast path).
    //
    // THREAD SAFETY:
    // - READ: Safe without lock (atomic read of enum value)
    // - WRITE: Should use set_level() which could add lock (not implemented here)
    //
    // PERFORMANCE NOTE:
    // This check happens BEFORE mutex acquisition:
    //   if (level < current_level_) return;  // No lock needed!
    //
    // This is critical for performance when many messages are filtered:
    // - 1000 DEBUG calls/sec with level=INFO: 1000 × 8ns = 8μs overhead
    // - vs. locking every time: 1000 × 1μs = 1ms overhead (125x slower)
    //
    // VISIBILITY:
    // Static members are shared across:
    // - All threads in current process
    // - All translation units (linked together)
    // - NOT shared across separate processes (each controller/worker has own)
    //
    // MODIFICATION:
    // Only via set_level() public method:
    //   Logger::set_level(LogLevel::DEBUG);  // Enable verbose logging
    //
    // DEFAULT VALUE (defined in logger.cpp):
    //   LogLevel::INFO (balance of visibility and performance)
    //
    //--------------------------------------------------------------------------
    
    static LogLevel current_level_;
    
    //--------------------------------------------------------------------------
    // static std::mutex mutex_;
    //--------------------------------------------------------------------------
    //
    // DECLARED HERE, DEFINED IN logger.cpp:
    //   std::mutex Logger::mutex_;
    //
    // PURPOSE:
    // Protects concurrent access to logging operations (formatting + output)
    // across multiple threads.
    //
    // PROTECTED RESOURCES:
    // - std::cout output stream (NOT thread-safe for concurrent writes)
    // - String formatting operations (to prevent interleaved output)
    //
    // LOCKING STRATEGY:
    // 1. Check level (no lock): if (level < current_level_) return;
    // 2. Acquire lock: std::lock_guard<std::mutex> lock(mutex_);
    // 3. Format message: timestamp + level + message
    // 4. Write to output: std::cout << formatted << std::endl;
    // 5. Release lock: automatic (lock_guard destructor)
    //
    // CONTENTION SCENARIOS:
    //
    // Controller Node (5 threads logging simultaneously):
    //   Thread 1: Logger::info("Raft leader elected")
    //   Thread 2: Logger::debug("Heartbeat sent to worker 3")
    //   Thread 3: Logger::info("Job 42 assigned to worker 1")
    //   Thread 4: Logger::debug("Log entry 1234 replicated")
    //   Thread 5: Logger::warning("Worker 5 heartbeat timeout")
    //
    //   -> Threads 2-5 block on mutex while Thread 1 holds lock
    //   -> Typical lock hold time: 25-50 microseconds
    //   -> Acceptable for this project's load (<1000 logs/sec)
    //
    // PERFORMANCE CONSIDERATIONS:
    // - Mutex overhead: ~200-500 nanoseconds (lock + unlock)
    // - Critical section: ~25-50 microseconds (format + write)
    // - Total: ~25-50 microseconds per log message
    //
    // DEADLOCK PREVENTION:
    // - Never call Logger methods while holding mutex
    // - Use std::lock_guard for RAII (automatic unlock on exception/return)
    // - Keep critical section minimal (no file I/O, no network calls)
    //
    // ALTERNATIVE CONSIDERED: Lock-free logging
    // - Could use lock-free queue + background writer thread
    // - Would improve throughput (1000x faster for high-volume logging)
    // - NOT NEEDED: Overhead acceptable for academic project scope
    // - ADDED COMPLEXITY: Thread management, queue sizing, shutdown coordination
    //
    //--------------------------------------------------------------------------
    
    static std::mutex mutex_;
    
    //==========================================================================
    // SECTION 5.2: PRIVATE HELPER METHODS
    //==========================================================================
    //
    // These utility functions support the public logging interface.
    // All are static (no instance needed) and inline (defined in header).
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // 5.2.1 TIMESTAMP GENERATION: get_timestamp()
    //--------------------------------------------------------------------------
    //
    // static std::string get_timestamp()
    //
    // PURPOSE:
    // Generates millisecond-precision timestamps for log messages, enabling
    // precise event correlation across distributed system components.
    //
    // RETURN VALUE:
    // String formatted as: "YYYY-MM-DD HH:MM:SS.mmm"
    // Example: "2025-11-20 14:32:15.123"
    //          ^^^^^^^  ^^ ^^^^^^^^  ^^^
    //          Date     Time         Milliseconds
    //
    // IMPLEMENTATION BREAKDOWN:
    //
    // Step 1: Capture current time with millisecond precision
    //   auto now = std::chrono::system_clock::now();
    //   - system_clock: Wall-clock time (not monotonic)
    //   - Precision: Platform-dependent, typically nanoseconds
    //   - Type: std::chrono::time_point<std::chrono::system_clock>
    //
    // Step 2: Convert to time_t for formatting (seconds since epoch)
    //   auto time = std::chrono::system_clock::to_time_t(now);
    //   - time_t: C-style timestamp (seconds since Jan 1, 1970 UTC)
    //   - Required by std::localtime() and std::put_time()
    //   - Loses sub-second precision (restored separately)
    //
    // Step 3: Extract milliseconds component
    //   auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    //                 now.time_since_epoch()) % 1000;
    //   - now.time_since_epoch(): Total duration since epoch
    //   - duration_cast<milliseconds>: Convert to milliseconds
    //   - % 1000: Extract fractional seconds (0-999)
    //   Example: 1637419935123 ms -> 123 ms (fractional part)
    //
    // Step 4: Format date/time using std::put_time
    //   std::stringstream ss;
    //   ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
    //   - std::localtime(&time): Convert UTC to local timezone (EST for Khoury)
    //   - std::put_time(..., format): Format according to strftime specifiers
    //   - "%Y-%m-%d %H:%M:%S": ISO 8601-like format (sortable lexicographically)
    //
    // Step 5: Append milliseconds with zero-padding
    //   ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
    //   - std::setfill('0'): Pad with zeros (not spaces)
    //   - std::setw(3): Total width of 3 characters
    //   - ms.count(): Extract integer value from duration
    //   Example: 7 -> "007", 89 -> "089", 123 -> "123"
    //
    // Step 6: Return formatted string
    //   return ss.str();
    //   - Builds final std::string from stringstream
    //   - Return value optimization (RVO) avoids copy
    //
    // WHY MILLISECOND PRECISION?
    // - Event correlation: Match logs across different nodes
    //   Example: "2025-11-20 14:32:15.123 Controller assigned job"
    //            "2025-11-20 14:32:15.125 Worker received job"
    //            -> 2ms network latency
    //
    // - Debugging: Identify race conditions and timing issues
    //   Example: "Leader election started at 14:32:15.100"
    //            "Follower voted at 14:32:15.105"
    //            -> 5ms response time (expected < 50ms)
    //
    // - Performance analysis: Measure operation durations
    //   Example: "Backtest started: 14:32:15.000"
    //            "Backtest completed: 14:32:17.456"
    //            -> 2456ms duration
    //
    // THREAD SAFETY:
    // - std::localtime() is NOT thread-safe (uses static buffer)
    // - However, called AFTER mutex is acquired in log()
    // - So thread safety is provided by outer mutex
    //
    // ALTERNATIVE: Thread-safe timestamp generation
    //   // Use localtime_r() on POSIX systems:
    //   struct tm time_info;
    //   localtime_r(&time, &time_info);
    //   ss << std::put_time(&time_info, "%Y-%m-%d %H:%M:%S");
    //
    //   // Or use C++20 std::chrono::format (not available in C++17):
    //   ss << std::format("{:%Y-%m-%d %H:%M:%S}", now);
    //
    // PERFORMANCE IMPACT:
    // - Timestamp generation: ~2-5 microseconds
    // - Dominated by system call overhead (clock_gettime)
    // - String formatting: ~1-2 microseconds
    // - Total: ~3-7 microseconds per timestamp
    // - Acceptable overhead for logging (< 10% of total log time)
    //
    // TIMEZONE CONSIDERATIONS:
    // For distributed system across timezones:
    // - Option 1: UTC timestamps (use std::gmtime instead of std::localtime)
    // - Option 2: Include timezone offset: "2025-11-20 14:32:15.123 EST"
    // - Current: Local time is fine for Khoury cluster (all nodes in same zone)
    //
    // MONOTONICITY WARNING:
    // system_clock can go backwards (NTP adjustments, DST changes):
    // - Log timestamp: 14:32:15.123
    // - NTP adjusts clock -1 second
    // - Next log: 14:32:14.500 (goes backwards!)
    //
    // If monotonicity is critical, use steady_clock for intervals:
    //   auto start = std::chrono::steady_clock::now();
    //   DoWork();
    //   auto end = std::chrono::steady_clock::now();
    //   auto duration = end - start;  // Guaranteed monotonic
    //
    //--------------------------------------------------------------------------
    
    static std::string get_timestamp() {
        // Capture current time with maximum available precision
        // system_clock: Wall-clock time, can be adjusted by NTP/user
        // Alternative: steady_clock (monotonic, but doesn't map to wall time)
        auto now = std::chrono::system_clock::now();
        
        // Convert to time_t (seconds since Unix epoch: Jan 1, 1970 00:00:00 UTC)
        // Required for compatibility with C time formatting functions
        // Loses sub-second precision (restored below)
        auto time = std::chrono::system_clock::to_time_t(now);
        
        // Extract millisecond component (fractional seconds)
        // time_since_epoch(): duration since epoch (nanosecond precision)
        // duration_cast<milliseconds>: truncate to milliseconds
        // % 1000: Extract only the fractional part (0-999 ms)
        //
        // Example calculation:
        //   now = 1637419935123 ms since epoch (arbitrary example)
        //   now % 1000 = 123 ms (the fractional part we want)
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        // Build timestamp string using stringstream (simpler than C-style buffers)
        std::stringstream ss;
        
        // Format date and time (without sub-second precision)
        // std::localtime: Convert UTC time_t to local timezone (EST for Boston/Khoury)
        // std::put_time: Stream manipulator for formatted time output
        // Format: "YYYY-MM-DD HH:MM:SS" (ISO 8601-like, lexicographically sortable)
        //
        // WARNING: std::localtime() uses static buffer (not thread-safe)
        // Safe here because called AFTER acquiring mutex in log()
        //
        // Alternative for thread safety: localtime_r() on POSIX
        //   struct tm time_info;
        //   localtime_r(&time, &time_info);
        //   ss << std::put_time(&time_info, "%Y-%m-%d %H:%M:%S");
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        
        // Append milliseconds with zero-padding
        // '.' : Decimal point separator
        // std::setfill('0'): Pad with zeros (not spaces)
        // std::setw(3): Minimum width 3 characters
        // ms.count(): Extract integer value from duration
        //
        // Examples:
        //   ms = 7   -> "007"
        //   ms = 89  -> "089"
        //   ms = 123 -> "123"
        ss << '.' << std::setfill('0') << std::setw(3) << ms.count();
        
        // Convert stringstream to std::string and return
        // Compiler optimization (RVO) typically avoids copy
        // Final format: "2025-11-20 14:32:15.123"
        return ss.str();
    }
    
    //--------------------------------------------------------------------------
    // 5.2.2 LEVEL-TO-STRING CONVERSION: level_to_string()
    //--------------------------------------------------------------------------
    //
    // static std::string level_to_string(LogLevel level)
    //
    // PURPOSE:
    // Converts LogLevel enum value to human-readable string for log output.
    //
    // PARAMETER:
    // - level: The log level to convert (DEBUG, INFO, WARNING, ERROR)
    //
    // RETURN VALUE:
    // - "DEBUG", "INFO", "WARN", "ERROR", or "UNKNOWN" (for invalid levels)
    //
    // IMPLEMENTATION:
    // Uses switch statement for exhaustive enum value handling with
    // compile-time verification (compiler warns if case missing).
    //
    // OUTPUT FORMATTING CHOICES:
    //
    // "WARN" instead of "WARNING":
    // - Shorter (consistent 5-character width with DEBUG, ERROR)
    // - Aligns nicely in log output:
    //   [2025-11-20 14:32:15.123] [DEBUG] Message
    //   [2025-11-20 14:32:15.124] [INFO ] Message
    //   [2025-11-20 14:32:15.125] [WARN ] Message
    //   [2025-11-20 14:32:15.126] [ERROR] Message
    //   ^^^^^^^^^^^^^^^^^^^^^  ^^^^^^^
    //   Timestamp              Level (aligned)
    //
    // - Could use right-padding to make all 5 chars:
    //   case LogLevel::INFO: return "INFO ";  // Trailing space
    //   But: Not implemented here (minor visual improvement)
    //
    // "UNKNOWN" default case:
    // - Defensive programming: Handles invalid enum values
    // - Can occur if:
    //   * Memory corruption (extremely rare)
    //   * Casting from invalid integer: LogLevel(99)
    //   * Future enum values added but this function not updated
    //
    // - Alternative: Could assert() or throw exception
    //   But: Logging should be robust, not crash on bad input
    //
    // WHY NOT ARRAY-BASED LOOKUP?
    // Alternative implementation:
    //   const char* level_names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    //   return level_names[static_cast<int>(level)];
    //
    // Problems:
    // - No bounds checking (undefined behavior if level > 3)
    // - Requires enum values to be contiguous 0, 1, 2, 3
    // - Switch statement is safer with compile-time checking
    //
    // THREAD SAFETY:
    // - Read-only operation (no shared mutable state)
    // - Returns temporary std::string (copy per call)
    // - Safe to call from multiple threads without lock
    //
    // PERFORMANCE:
    // - Switch statement: ~1-2 CPU cycles (branch prediction)
    // - String construction: ~5-10 nanoseconds (SSO for short strings)
    // - Total: Negligible compared to timestamp generation (~3μs)
    //
    // RETURN VALUE OPTIMIZATION:
    // Modern compilers apply RVO (Return Value Optimization):
    //   std::string result = level_to_string(level);
    //   // No copy! String constructed directly in result's memory
    //
    //--------------------------------------------------------------------------
    
    static std::string level_to_string(LogLevel level) {
        // Switch on enum value (exhaustive handling)
        // Compiler typically generates jump table (O(1) lookup)
        switch (level) {
            // DEBUG (0): Most verbose diagnostics
            case LogLevel::DEBUG:
                return "DEBUG";  // 5 characters
            
            // INFO (1): Standard operational messages
            case LogLevel::INFO:
                return "INFO";   // 4 characters (could pad to "INFO " for alignment)
            
            // WARNING (2): Unexpected but non-critical conditions
            // Shortened to "WARN" for consistent width
            case LogLevel::WARNING:
                return "WARN";   // 4 characters (matches INFO width)
            
            // ERROR (3): Critical failures
            case LogLevel::ERROR:
                return "ERROR";  // 5 characters (matches DEBUG width)
            
            // Default: Invalid enum value (should never happen in normal operation)
            // Defensive programming: Handle corruption or future enum additions
            default:
                return "UNKNOWN";  // 7 characters (stands out as abnormal)
        }
        
        // Note: Unreachable code (all enum values handled + default case)
        // Some compilers may warn; suppress with -Wno-unreachable-code if needed
    }
    
public:
    //==========================================================================
    // SECTION 5.3: PUBLIC INTERFACE
    //==========================================================================
    //
    // Public static methods providing logging functionality.
    // All methods are inline (defined in header) and static (no instance needed).
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // 5.3.1 CONFIGURATION: set_level()
    //--------------------------------------------------------------------------
    //
    // static void set_level(LogLevel level)
    //
    // PURPOSE:
    // Configures the minimum log level threshold. Messages with severity
    // below this level are silently discarded.
    //
    // PARAMETER:
    // - level: New threshold (DEBUG, INFO, WARNING, or ERROR)
    //
    // THREAD SAFETY:
    // - CURRENTLY NOT THREAD-SAFE (no lock protection)
    // - Assignment to static variable is atomic on most platforms
    // - However, could race with concurrent log() calls reading current_level_
    //
    // POTENTIAL RACE CONDITION:
    //   Thread 1: Logger::set_level(LogLevel::DEBUG);  // Writing
    //   Thread 2: if (level < current_level_) return;  // Reading (in log())
    //   -> Rare but possible torn read (reading while writing)
    //
    // MITIGATION OPTIONS:
    //
    // 1. Add mutex protection (safest but slower):
    //    static void set_level(LogLevel level) {
    //        std::lock_guard<std::mutex> lock(mutex_);
    //        current_level_ = level;
    //    }
    //
    // 2. Use std::atomic<LogLevel> (lock-free):
    //    static std::atomic<LogLevel> current_level_;
    //    static void set_level(LogLevel level) {
    //        current_level_.store(level, std::memory_order_relaxed);
    //    }
    //    // In log(): if (level < current_level_.load(std::memory_order_relaxed))
    //
    // 3. Accept the risk (current approach):
    //    - Log level typically changed only at startup
    //    - Worst case: One log message incorrectly filtered during level change
    //    - Acceptable for academic project
    //
    // USAGE PATTERNS:
    //
    // 1. At program startup (before any threads created):
    //    int main() {
    //        Logger::set_level(LogLevel::DEBUG);  // Safe (single-threaded)
    //        StartSystem();
    //    }
    //
    // 2. From command-line argument:
    //    if (arg == "--debug") {
    //        Logger::set_level(LogLevel::DEBUG);
    //    } else if (arg == "--quiet") {
    //        Logger::set_level(LogLevel::ERROR);
    //    }
    //
    // 3. Dynamic adjustment during runtime (use with caution):
    //    // Temporarily enable DEBUG logging
    //    LogLevel old_level = Logger::get_level();  // Need getter (not implemented)
    //    Logger::set_level(LogLevel::DEBUG);
    //    TroublesomeFunction();
    //    Logger::set_level(old_level);  // Restore
    //
    // 4. Signal handler (advanced, requires atomic operations):
    //    void handle_sigusr1(int sig) {
    //        Logger::set_level(LogLevel::DEBUG);  // Increase verbosity
    //    }
    //    signal(SIGUSR1, handle_sigusr1);
    //    // Later: kill -USR1 <pid> to enable DEBUG logging
    //
    // BEST PRACTICES:
    // - Set level once at startup (avoids race conditions)
    // - Use environment variable or config file:
    //   const char* level_str = getenv("LOG_LEVEL");
    //   if (level_str == "DEBUG") Logger::set_level(LogLevel::DEBUG);
    //
    // - For multiple log levels per component, consider:
    //   Logger::set_level("controller", LogLevel::DEBUG);
    //   Logger::set_level("worker", LogLevel::INFO);
    //   (Requires more complex implementation with level map)
    //
    //--------------------------------------------------------------------------
    
    static void set_level(LogLevel level) {
        // Direct assignment to static member
        // WARNING: Not thread-safe (no mutex protection)
        // Acceptable risk: Level typically set once at startup
        //
        // For production code, consider:
        //   std::lock_guard<std::mutex> lock(mutex_);
        //   current_level_ = level;
        current_level_ = level;
    }
    
    //--------------------------------------------------------------------------
    // 5.3.2 CORE LOGGING: log()
    //--------------------------------------------------------------------------
    //
    // static void log(LogLevel level, const std::string& message)
    //
    // PURPOSE:
    // Core logging method that filters, formats, and outputs log messages.
    // All convenience methods (debug, info, warning, error) delegate to this.
    //
    // PARAMETERS:
    // - level: Severity of this message (DEBUG, INFO, WARNING, ERROR)
    // - message: Log message content (passed by const reference, no copy)
    //
    // IMPLEMENTATION FLOW:
    //
    // 1. FAST PATH: Level check without lock
    //    if (level < current_level_) return;
    //    - Filters out disabled messages immediately
    //    - No mutex overhead for disabled logs (~8ns)
    //    - Critical for performance when DEBUG disabled but called frequently
    //
    // 2. SLOW PATH: Format and output (lock held)
    //    std::lock_guard<std::mutex> lock(mutex_);
    //    - Acquire exclusive lock (blocks other threads)
    //    - Critical section: Only one thread at a time
    //
    // 3. Format output string
    //    std::cout << "[timestamp] [LEVEL] message\n";
    //    - get_timestamp(): "2025-11-20 14:32:15.123"
    //    - level_to_string(): "DEBUG", "INFO", "WARN", "ERROR"
    //    - message: User-provided content
    //
    // 4. Flush output
    //    std::endl: Inserts newline + flushes stream
    //    - Ensures message visible immediately (important for debugging)
    //    - Trade-off: Slower than '\n' but prevents buffering issues
    //
    // 5. Automatic unlock
    //    lock_guard destructor releases mutex
    //    - Happens even if exception thrown (RAII)
    //
    // OUTPUT FORMAT:
    // [2025-11-20 14:32:15.123] [INFO] Controller started on port 5000
    // ^^^^^^^^^^^^^^^^^^^^^^^^^  ^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^
    // Timestamp (24 chars)       Level  Message
    //
    // Why this format?
    // - Timestamp first: Easy to sort/grep by time
    // - Level in brackets: Visually distinct, easy to filter (grep "[ERROR]")
    // - Message last: Variable length, doesn't disturb alignment
    //
    // THREAD SAFETY ANALYSIS:
    //
    // Scenario: 3 threads logging simultaneously
    //   Thread A: Logger::info("Message A")
    //   Thread B: Logger::info("Message B")
    //   Thread C: Logger::info("Message C")
    //
    // Without mutex:
    //   Output: [2025-11-[2025-11-20 14:32:15.124] [INFO] Message B
    //           20 14:32:15.123] [INFO] Message A
    //           [2025-11-20 14:32:15.125] [INFO] Message C
    //   -> CORRUPTED! Interleaved characters
    //
    // With mutex:
    //   Output: [2025-11-20 14:32:15.123] [INFO] Message A
    //           [2025-11-20 14:32:15.124] [INFO] Message B
    //           [2025-11-20 14:32:15.125] [INFO] Message C
    //   -> CORRECT! Sequential, readable
    //
    // PERFORMANCE BREAKDOWN:
    // - Level check: ~8 ns (no lock)
    // - Mutex acquire: ~200-500 ns
    // - get_timestamp(): ~3-7 μs
    // - level_to_string(): ~10 ns
    // - String formatting: ~1-5 μs
    // - std::cout write: ~10-20 μs
    // - std::endl flush: ~5-10 μs (forces write to kernel)
    // - Mutex release: ~200-500 ns
    // TOTAL: ~25-50 μs per enabled log message
    //
    // OPTIMIZATION OPPORTUNITIES (if needed):
    //
    // 1. Lazy formatting (only format if level enabled):
    //    Already done via fast path check
    //
    // 2. Use '\n' instead of std::endl (avoid flush):
    //    std::cout << formatted << '\n';  // 2-5μs faster
    //    Trade-off: Messages may be buffered, not immediately visible
    //
    // 3. Asynchronous logging (background thread):
    //    - Main thread: Push message to lock-free queue
    //    - Background thread: Pop and write
    //    - 10-100x faster for high-volume logging
    //    - Complexity: Thread management, queue sizing, shutdown
    //
    // 4. Binary logging (not human-readable):
    //    - Write structured binary format
    //    - Post-process to text later
    //    - 10x faster but harder to debug
    //
    // EXCEPTION SAFETY:
    // - std::lock_guard: RAII ensures unlock even if exception thrown
    // - Example: If std::cout throws (disk full, broken pipe)
    //            Mutex still released, no deadlock
    //
    // COMMON MISTAKES TO AVOID:
    //
    // 1. Calling log() while holding mutex (deadlock):
    //    void SomeFunction() {
    //           std::lock_guard<std::mutex> lock(Logger::mutex_);
    //           Logger::log(LogLevel::INFO, "Message");  // DEADLOCK!
    //       }
    //
    // 2. Expensive computation before level check:
    //    Logger::debug(ExpensiveToString());  // Computed even if disabled!
    //    if (Logger::is_debug_enabled()) {  // Need getter for this
    //           Logger::debug(ExpensiveToString());
    //       }
    //
    // 3. Logging in signal handler (async-signal-unsafe):
    //    void signal_handler(int sig) {
    //           Logger::error("Signal received");  // UNDEFINED BEHAVIOR
    //       }
    //    -> mutex operations not async-signal-safe
    //    -> Use write() system call directly in signal handlers
    //
    //--------------------------------------------------------------------------
    
    static void log(LogLevel level, const std::string& message) {
        // FAST PATH: Filter messages below threshold WITHOUT acquiring lock
        // This is critical for performance when many messages are disabled
        //
        // Example: current_level_ = INFO (1)
        //   - log(DEBUG, "..."):   DEBUG (0) < INFO (1) -> return (filtered)
        //   - log(INFO, "..."):    INFO (1) < INFO (1)  -> FALSE, continue
        //   - log(WARNING, "..."): WARNING (2) < INFO (1) -> FALSE, continue
        //   - log(ERROR, "..."):   ERROR (3) < INFO (1) -> FALSE, continue
        //
        // Performance benefit:
        //   Without fast path: 1000 DEBUG calls = 1000 mutex locks = ~1ms
        //   With fast path:    1000 DEBUG calls = 1000 integer compares = ~8μs
        //   -> 125x faster filtering
        //
        // Thread safety note:
        //   Reading current_level_ without lock is technically a race
        //   But: enum read is atomic on all modern platforms
        //        Worst case: One message incorrectly filtered during level change
        //        Acceptable risk for the performance benefit
        if (level < current_level_) return;
        
        // SLOW PATH: Acquire lock and perform logging
        // Beyond this point, only ONE thread executes at a time
        //
        // std::lock_guard: RAII-style mutex wrapper
        // - Constructor: Acquires mutex (blocks if already held)
        // - Destructor: Releases mutex automatically
        // - Exception-safe: Unlocks even if formatting/output throws
        //
        // Alternative: std::unique_lock (more flexible, but unnecessary here)
        //   std::unique_lock<std::mutex> lock(mutex_);
        //   // Can manually unlock: lock.unlock();
        //   // Can transfer ownership: std::unique_lock lock2 = std::move(lock);
        //
        // lock_guard is simpler and faster (no extra state tracking)
        std::lock_guard<std::mutex> lock(mutex_);
        
        // FORMAT AND OUTPUT (protected by mutex, only one thread at a time)
        //
        // Output components:
        // 1. Timestamp in brackets: "[2025-11-20 14:32:15.123]"
        // 2. Space separator
        // 3. Level in brackets: "[INFO]"
        // 4. Space separator
        // 5. User message: "Controller started"
        // 6. Newline + flush: std::endl
        //
        // Example output:
        //   [2025-11-20 14:32:15.123] [INFO] Controller started on port 5000
        //
        // Why std::cout (not fprintf or write)?
        // - C++ stream: Type-safe, no format string vulnerabilities
        // - Consistent with C++ ecosystem
        // - Easy to redirect (could be std::ofstream for file logging)
        //
        // Why std::endl (not just '\n')?
        // - std::endl = '\n' + flush
        // - Ensures message visible immediately (important for debugging)
        // - Trade-off: ~5μs slower than '\n' due to flush
        // - For high-performance logging, could use '\n' and rely on auto-flush
        //
        // Thread safety of std::cout:
        // - Individual operator<< calls are NOT atomic
        // - Without mutex, output from multiple threads would interleave
        // - Example without mutex:
        //     Thread 1: std::cout << "[2025-11-20 14:32:15.123]" << " [INFO]"
        //     Thread 2: std::cout << "[2025-11-20 14:32:15.124]"
        //     Output:   "[2025-11-20 14:32:15.123][2025-11-20 14:32:15.124] [INFO]"
        // - With mutex: Entire output sequence is atomic
        std::cout << "[" << get_timestamp() << "] "
                  << "[" << level_to_string(level) << "] "
                  << message << std::endl;
        
        // MUTEX AUTOMATICALLY RELEASED HERE
        // lock_guard destructor called when scope exits
        // Even if std::cout throws exception, mutex released (RAII guarantee)
    }
    
    //--------------------------------------------------------------------------
    // 5.3.3 CONVENIENCE METHODS: debug(), info(), warning(), error()
    //--------------------------------------------------------------------------
    //
    // These methods provide syntactic sugar for logging at specific levels.
    // All delegate to log() with appropriate LogLevel constant.
    //
    // DESIGN RATIONALE:
    //
    // Without convenience methods (verbose):
    //   Logger::log(LogLevel::INFO, "Controller started");
    //   Logger::log(LogLevel::ERROR, "Connection failed");
    //
    // With convenience methods (concise):
    //   Logger::info("Controller started");
    //   Logger::error("Connection failed");
    //
    // Benefits:
    // - Shorter, more readable code
    // - Less typing (reduces developer cognitive load)
    // - Follows logging framework conventions (log4j, Python logging, etc.)
    //
    // Implementation:
    // - Simple wrappers: Just call log() with hardcoded level
    // - Inline: Compiler typically optimizes away the extra function call
    // - No overhead: Direct translation to log(level, message)
    //
    // NAMING CONVENTIONS:
    //
    // Option 1: lowercase (current): debug(), info(), warning(), error()
    // - Pros: Concise, reads as verb ("log this at debug level")
    // - Cons: Not capitalized like class name (Logger)
    //
    // Option 2: Capitalized: Debug(), Info(), Warning(), Error()
    // - Pros: Consistent with class name capitalization
    // - Cons: Looks like constructor call
    //
    // Option 3: ALL_CAPS: DEBUG(), INFO(), WARNING(), ERROR()
    // - Pros: Visually distinct (stands out in code)
    // - Cons: ALL_CAPS typically reserved for macros
    //
    // We use lowercase (Option 1) following C++ standard library conventions:
    // - std::cout (not std::Cout or std::COUT)
    // - std::vector::push_back() (not PushBack())
    //
    // ALTERNATIVE DESIGN: Variadic templates for formatting
    //
    // Could provide printf-style or format-style logging:
    //
    //   // Printf-style:
    //   template<typename... Args>
    //   static void info(const char* format, Args&&... args) {
    //       char buffer[1024];
    //       snprintf(buffer, sizeof(buffer), format, std::forward<Args>(args)...);
    //       log(LogLevel::INFO, buffer);
    //   }
    //   // Usage: Logger::info("Worker %d completed job %s", worker_id, job_name);
    //
    //   // Format-style (C++20 std::format):
    //   template<typename... Args>
    //   static void info(std::format_string<Args...> fmt, Args&&... args) {
    //       log(LogLevel::INFO, std::format(fmt, std::forward<Args>(args)...));
    //   }
    //   // Usage: Logger::info("Worker {} completed job {}", worker_id, job_name);
    //
    // Not implemented here for simplicity, but would be useful extensions.
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // debug() - Log DEBUG-level message
    //--------------------------------------------------------------------------
    //
    // PURPOSE: Detailed diagnostic information for development/troubleshooting
    //
    // WHEN TO USE:
    // - Function entry/exit: ">> EnteringprocessBacktest()"
    // - Loop iterations: "Processing symbol 123/500: AAPL"
    // - State transitions: "Raft state changed: Follower -> Candidate"
    // - Network details: "Received 1024 bytes from 192.168.1.10:5000"
    //
    // WHEN TO AVOID:
    // - Production (excessive volume, performance impact)
    // - Inside hot loops (1M iterations -> 1M log calls)
    // - Before expensive computations (unless guarded by level check)
    //
    // EXAMPLE USAGE:
    //   void ProcessJob(const Job& job) {
    //       Logger::debug("Processing job " + job.id + " for symbol " + job.symbol);
    //       
    //       for (const auto& data_point : job.data) {
    //           // DON'T log every iteration (too verbose)
    //           ProcessDataPoint(data_point);
    //       }
    //       
    //       Logger::debug("Completed job " + job.id + ", PnL: " + 
    //                    std::to_string(result.pnl));
    //   }
    //
    // FILTERING:
    // - Enabled: When current_level_ = DEBUG
    // - Disabled: When current_level_ = INFO, WARNING, or ERROR
    //
    // PERFORMANCE:
    // - If disabled: ~8 ns (fast path check)
    // - If enabled: ~25-50 μs (full logging)
    //
    //--------------------------------------------------------------------------
    
    static void debug(const std::string& message) {
        // Delegate to core log() method with DEBUG level
        // Compiler typically inlines this (zero overhead)
        log(LogLevel::DEBUG, message);
    }
    
    //--------------------------------------------------------------------------
    // info() - Log INFO-level message
    //--------------------------------------------------------------------------
    //
    // PURPOSE: General informational messages about system operation
    //
    // WHEN TO USE:
    // - System lifecycle: "Controller started, listening on port 5000"
    // - Job tracking: "Worker 3 completed job 42, duration: 1250ms"
    // - State changes: "Raft node elected as leader, term: 5"
    // - Periodic status: "Processed 1000 symbols, 500 remaining"
    //
    // WHEN TO AVOID:
    // - Excessive detail (use DEBUG instead)
    // - Inside loops (aggregate instead)
    //
    // EXAMPLE USAGE:
    //   void StartController() {
    //       Logger::info("Controller starting on port " + std::to_string(port));
    //       
    //       InitializeRaft();
    //       Logger::info("Raft cluster initialized, ID: " + node_id);
    //       
    //       StartAcceptingConnections();
    //       Logger::info("Controller ready, waiting for workers");
    //   }
    //
    // DEFAULT LEVEL:
    // - Recommended for production systems
    // - Balance of visibility and performance
    // - Provides operational insights without excessive noise
    //
    //--------------------------------------------------------------------------
    
    static void info(const std::string& message) {
        log(LogLevel::INFO, message);
    }
    
    //--------------------------------------------------------------------------
    // warning() - Log WARNING-level message
    //--------------------------------------------------------------------------
    //
    // PURPOSE: Unexpected conditions that don't prevent operation
    //
    // WHEN TO USE:
    // - Performance degradation: "Heartbeat response delayed by 500ms"
    // - Resource limits: "Worker queue length exceeded 100 jobs"
    // - Retries: "Connection attempt 3/5 failed, retrying..."
    // - Recoverable errors: "Checkpoint corrupted, recreating from scratch"
    //
    // WHEN TO AVOID:
    // - Expected slow operations (not warnings, just INFO)
    // - Unrecoverable failures (use ERROR instead)
    //
    // EXAMPLE USAGE:
    //   void SendHeartbeat() {
    //       auto start = std::chrono::steady_clock::now();
    //       bool success = SendHeartbeatPacket();
    //       auto duration = std::chrono::steady_clock::now() - start;
    //       
    //       if (duration > std::chrono::milliseconds(500)) {
    //           Logger::warning("Heartbeat delayed: " + 
    //                         std::to_string(duration.count()) + "ms");
    //       }
    //       
    //       if (!success) {
    //           Logger::warning("Heartbeat failed, will retry in 2 seconds");
    //       }
    //   }
    //
    // RESPONSE:
    // - Investigate: Check for systemic issues
    // - Monitor: Track frequency (occasional = ok, frequent = problem)
    // - Alert: Consider paging if warnings persist
    //
    //--------------------------------------------------------------------------
    
    static void warning(const std::string& message) {
        log(LogLevel::WARNING, message);
    }
    
    //--------------------------------------------------------------------------
    // error() - Log ERROR-level message
    //--------------------------------------------------------------------------
    //
    // PURPOSE: Critical failures that prevent normal operation
    //
    // WHEN TO USE:
    // - Fatal errors: "Cannot bind to port 5000, address already in use"
    // - Data loss: "Worker crashed, job 42 results lost"
    // - System failures: "Raft cluster lost quorum, cannot accept jobs"
    // - Resource exhaustion: "Out of memory, cannot allocate job buffer"
    //
    // WHEN TO AVOID:
    // - Recoverable issues (use WARNING instead)
    // - Expected error cases (e.g., "File not found" might be INFO)
    //
    // EXAMPLE USAGE:
    //   bool ConnectToController() {
    //       for (int attempt = 1; attempt <= 5; ++attempt) {
    //           if (TryConnect()) {
    //               Logger::info("Connected to controller on attempt " + 
    //                          std::to_string(attempt));
    //               return true;
    //           }
    //           Logger::warning("Connection attempt " + std::to_string(attempt) + 
    //                         " failed, retrying...");
    //           std::this_thread::sleep_for(std::chrono::seconds(1));
    //       }
    //       
    //       Logger::error("Failed to connect to controller after 5 attempts");
    //       return false;
    //   }
    //
    // RESPONSE:
    // - Alert: Page on-call engineer immediately
    // - Failover: Trigger automatic recovery if configured
    // - Incident: Create incident report for post-mortem
    // - Terminate: May exit program after logging error
    //
    // ERROR vs. EXCEPTION:
    // - Logger::error() just logs, doesn't stop execution
    // - To stop execution: throw after logging
    //   Logger::error("Fatal error occurred");
    //   throw std::runtime_error("Fatal error");
    //
    //--------------------------------------------------------------------------
    
    static void error(const std::string& message) {
        log(LogLevel::ERROR, message);
    }
};

} // namespace backtesting

#endif // LOGGER_H

//==============================================================================
// SECTION 6: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Basic Controller Logging
// ====================================
//
// File: controller/main.cpp
//
// #include "common/logger.h"
// using namespace backtesting;
//
// int main(int argc, char* argv[]) {
//     // Configure log level from command line
//     if (argc > 1 && std::string(argv[1]) == "--debug") {
//         Logger::set_level(LogLevel::DEBUG);
//     } else {
//         Logger::set_level(LogLevel::INFO);
//     }
//     
//     Logger::info("Controller starting, version 1.0.0");
//     Logger::debug("Command line args: " + std::to_string(argc));
//     
//     try {
//         InitializeRaftCluster();
//         Logger::info("Raft cluster initialized");
//         
//         StartAcceptingConnections();
//         Logger::info("Listening on port 5000");
//         
//         RunEventLoop();
//     } catch (const std::exception& e) {
//         Logger::error("Fatal exception: " + std::string(e.what()));
//         return 1;
//     }
//     
//     Logger::info("Controller shutting down");
//     return 0;
// }
//
//
// EXAMPLE 2: Worker Job Execution
// ================================
//
// File: worker/job_executor.cpp
//
// void JobExecutor::ExecuteBacktest(const Job& job) {
//     Logger::info("[Worker " + std::to_string(worker_id_) + "] " +
//                  "Starting job " + job.id + " for symbol " + job.symbol);
//     
//     auto start = std::chrono::high_resolution_clock::now();
//     
//     try {
//         // Load data
//         Logger::debug("Loading price data for " + job.symbol);
//         auto data = data_loader_.Load(job.symbol, job.start_date, job.end_date);
//         Logger::debug("Loaded " + std::to_string(data.size()) + " data points");
//         
//         // Execute strategy
//         Logger::debug("Running strategy: " + job.strategy_name);
//         auto result = strategy_->Backtest(data, job.parameters);
//         
//         // Calculate duration
//         auto end = std::chrono::high_resolution_clock::now();
//         auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
//             end - start).count();
//         
//         Logger::info("[Worker " + std::to_string(worker_id_) + "] " +
//                      "Completed job " + job.id + " in " + 
//                      std::to_string(duration_ms) + "ms, " +
//                      "PnL: $" + std::to_string(result.pnl));
//         
//         SendResultToController(result);
//         
//     } catch (const DataLoadException& e) {
//         Logger::error("Failed to load data for job " + job.id + ": " + e.what());
//         SendErrorToController(job.id, "DATA_LOAD_FAILED");
//     } catch (const std::exception& e) {
//         Logger::error("Job " + job.id + " failed with exception: " + e.what());
//         SendErrorToController(job.id, "EXECUTION_FAILED");
//     }
// }
//
//
// EXAMPLE 3: Raft Consensus Logging
// ==================================
//
// File: controller/raft_controller.cpp
//
// void RaftController::StartElection() {
//     current_term_++;
//     voted_for_ = node_id_;
//     state_ = RaftState::CANDIDATE;
//     
//     Logger::info("[Raft] Node " + node_id_ + " starting election for term " + 
//                  std::to_string(current_term_));
//     Logger::debug("[Raft] Sending RequestVote to " + 
//                   std::to_string(peers_.size()) + " peers");
//     
//     int votes_received = 1;  // Vote for self
//     
//     for (const auto& peer : peers_) {
//         Logger::debug("[Raft] Sending RequestVote to peer " + peer.id);
//         
//         if (SendRequestVote(peer, current_term_, last_log_index_)) {
//             votes_received++;
//             Logger::debug("[Raft] Received vote from peer " + peer.id);
//         } else {
//             Logger::warning("[Raft] Peer " + peer.id + " rejected vote request");
//         }
//     }
//     
//     int majority = (peers_.size() + 1) / 2 + 1;
//     if (votes_received >= majority) {
//         state_ = RaftState::LEADER;
//         Logger::info("[Raft] Node " + node_id_ + " elected as LEADER for term " + 
//                      std::to_string(current_term_) + " with " + 
//                      std::to_string(votes_received) + "/" + 
//                      std::to_string(peers_.size() + 1) + " votes");
//         BecomeLeader();
//     } else {
//         Logger::warning("[Raft] Election failed, received only " + 
//                        std::to_string(votes_received) + "/" + 
//                        std::to_string(majority) + " votes");
//         state_ = RaftState::FOLLOWER;
//     }
// }
//
//
// EXAMPLE 4: Multi-threaded Worker Heartbeats
// ============================================
//
// File: worker/heartbeat_manager.cpp
//
// void HeartbeatManager::Run() {
//     std::thread heartbeat_thread([this]() {
//         Logger::info("[Heartbeat] Thread started, interval: 2s");
//         
//         while (running_.load()) {
//             std::this_thread::sleep_for(std::chrono::seconds(2));
//             
//             Logger::debug("[Heartbeat] Sending heartbeat to controller");
//             
//             try {
//                 auto start = std::chrono::steady_clock::now();
//                 bool success = SendHeartbeat();
//                 auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
//                     std::chrono::steady_clock::now() - start);
//                 
//                 if (success) {
//                     Logger::debug("[Heartbeat] Successfully sent in " + 
//                                  std::to_string(duration.count()) + "ms");
//                     
//                     if (duration.count() > 500) {
//                         Logger::warning("[Heartbeat] Slow response: " + 
//                                       std::to_string(duration.count()) + "ms");
//                     }
//                 } else {
//                     Logger::error("[Heartbeat] Failed to send, controller may be down");
//                 }
//             } catch (const NetworkException& e) {
//                 Logger::error("[Heartbeat] Network exception: " + 
//                              std::string(e.what()));
//             }
//         }
//         
//         Logger::info("[Heartbeat] Thread stopping");
//     });
//     
//     heartbeat_thread.detach();
// }
//
//
// EXAMPLE 5: Checkpointing with Logging
// ======================================
//
// File: worker/checkpoint_manager.cpp
//
// void CheckpointManager::SaveCheckpoint(const JobState& state) {
//     Logger::debug("[Checkpoint] Starting checkpoint for job " + state.job_id + 
//                   " at symbol " + std::to_string(state.current_symbol_index));
//     
//     try {
//         std::string filename = "checkpoint_" + state.job_id + ".dat";
//         std::ofstream file(filename, std::ios::binary);
//         
//         if (!file.is_open()) {
//             Logger::error("[Checkpoint] Failed to open file: " + filename);
//             return;
//         }
//         
//         // Write checkpoint data
//         file.write(reinterpret_cast<const char*>(&state), sizeof(state));
//         file.flush();
//         
//         Logger::info("[Checkpoint] Saved job " + state.job_id + 
//                      " progress: " + std::to_string(state.current_symbol_index) + 
//                      "/" + std::to_string(state.total_symbols));
//         
//     } catch (const std::exception& e) {
//         Logger::error("[Checkpoint] Exception during save: " + 
//                      std::string(e.what()));
//     }
// }
//
//==============================================================================

//==============================================================================
// SECTION 7: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: Expensive Computation in Log Message
// ================================================
// WRONG (always executes, even if DEBUG disabled):
//    Logger::debug("Data: " + ExpensiveToString(data));
//    // ExpensiveToString() called even when DEBUG filtered!
//
// CORRECT (guard with level check):
//    // Option 1: Add is_level_enabled() method (not implemented):
//    if (Logger::is_debug_enabled()) {
//        Logger::debug("Data: " + ExpensiveToString(data));
//    }
//    
//    // Option 2: Use lambda + lazy evaluation (requires template changes):
//    Logger::debug([&]() { return "Data: " + ExpensiveToString(data); });
//
//
// PITFALL 2: Logging Inside Tight Loops
// ======================================
// WRONG (massive overhead, even if disabled):
//    for (int i = 0; i < 1000000; ++i) {
//        Logger::debug("Processing iteration " + std::to_string(i));
//        // 1M log calls, even at 8ns each = 8ms overhead
//    }
//
// CORRECT (aggregate logging):
//    Logger::debug("Starting loop with " + std::to_string(count) + " iterations");
//    for (int i = 0; i < 1000000; ++i) {
//        // No logging inside loop
//        ProcessItem(i);
//    }
//    Logger::debug("Completed loop");
//
//
// PITFALL 3: Forgetting Static Member Definitions
// ================================================
// WRONG (linker error):
//    // Only logger.h exists, no logger.cpp
//    // Error: undefined reference to `backtesting::Logger::current_level_'
//
// CORRECT (logger.cpp must define statics):
//    // logger.cpp:
//    LogLevel Logger::current_level_ = LogLevel::INFO;
//    std::mutex Logger::mutex_;
//
//
// PITFALL 4: Calling Logger from Signal Handler
// ==============================================
// WRONG (undefined behavior):
//    void signal_handler(int signum) {
//        Logger::error("Caught signal " + std::to_string(signum));
//        // Mutex operations are async-signal-unsafe!
//        exit(1);
//    }
//
// CORRECT (use write() directly):
//    void signal_handler(int signum) {
//        const char msg[] = "Caught signal\n";
//        write(STDERR_FILENO, msg, sizeof(msg) - 1);
//        // Only async-signal-safe functions allowed in handler
//        _exit(1);
//    }
//
//
// PITFALL 5: Assuming set_level() is Thread-Safe
// ===============================================
// WRONG (potential race condition):
//    // Thread 1: Logger::set_level(LogLevel::DEBUG);
//    // Thread 2: Logger::log(LogLevel::INFO, "...");
//    // Race: Thread 2 reads current_level_ while Thread 1 writes
//
// CORRECT (set level before threading):
//    int main() {
//        Logger::set_level(LogLevel::INFO);  // Before spawning threads
//        StartWorkerThreads();  // Now safe
//    }
//
//
// PITFALL 6: Not Flushing Before Crash
// =====================================
// WRONG (logs buffered, lost on crash):
//    Logger::info("About to perform risky operation");
//    DangerousOperation();  // Crashes before std::endl flushes
//
// CORRECT (explicitly flush critical logs):
//    Logger::info("About to perform risky operation");
//    std::cout.flush();  // Force write to kernel
//    DangerousOperation();
//
//
// PITFALL 7: Logging Sensitive Information
// =========================================
// WRONG (security risk):
//    Logger::debug("API key: " + api_key);
//    Logger::info("User password: " + password);
//
// CORRECT (redact sensitive data):
//    Logger::debug("API key: " + api_key.substr(0, 4) + "****");
//    Logger::info("User authenticated: " + username);  // No password
//
//==============================================================================

//==============================================================================
// SECTION 8: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Why are methods defined in the header (not .cpp)?
// ======================================================
// A: Header-only implementation for:
//    - Simplicity: Single file to include
//    - Inlining: Compiler can optimize at call site
//    - Trade-off: Longer compilation time (negligible for this project)
//    Note: Static members MUST be defined in .cpp to avoid linker errors
//
// Q2: Can I log from multiple threads simultaneously?
// ===================================================
// A: Yes! Logger is thread-safe:
//    - std::mutex protects critical sections
//    - Multiple threads can call log() concurrently
//    - Output is sequenced (no interleaved characters)
//
// Q3: What's the performance overhead of logging?
// ================================================
// A: Overhead per log call:
//    - Disabled message (DEBUG when level=INFO): ~8 ns
//    - Enabled message: ~25-50 μs
//    - For typical workload (100 logs/sec): ~5ms/sec = 0.5% CPU
//    - Acceptable for academic project, may need optimization for production
//
// Q4: Can I log to a file instead of stdout?
// ===========================================
// A: Not currently supported, but easy to add:
//    // Add to Logger class:
//    static std::ofstream log_file_;
//    static void set_output(const std::string& filename) {
//        log_file_.open(filename, std::ios::app);
//    }
//    // In log() method, change std::cout to log_file_
//
// Q5: How do I filter logs by component (controller vs. worker)?
// ===============================================================
// A: Current design has single global level. Options:
//    - Prefix messages: Logger::info("[Controller] ...");
//    - Separate log files per component
//    - Extend Logger to support per-component levels (requires refactoring)
//
// Q6: Why LogLevel::WARNING (not WARN)?
// =====================================
// A: Enum value is WARNING, but displays as "WARN":
//    - Full word in enum for clarity
//    - Abbreviated output for consistent column alignment
//    - Both conventions are common in logging frameworks
//
// Q7: Can I change log level at runtime?
// =======================================
// A: Yes, call Logger::set_level() anytime:
//    Logger::set_level(LogLevel::DEBUG);  // Enable verbose logging
//    But be aware: Not thread-safe (best to set once at startup)
//
// Q8: What if I want millisecond precision in logs?
// ==================================================
// A: Already implemented! Timestamps include milliseconds:
//    [2025-11-20 14:32:15.123]
//                           ^^^ milliseconds
//
// Q9: How do I aggregate logs from multiple nodes?
// =================================================
// A: Options for distributed logging:
//    - Collect files: scp logs from each node, merge by timestamp
//    - Centralized: Forward logs to syslog/ELK stack (requires implementation)
//    - Shared filesystem: All nodes write to NFS with unique filenames
//
// Q10: Why not use existing library (spdlog, glog, log4cpp)?
// ===========================================================
// A: For academic project:
//    - Learning: Understand logging implementation
//    - Simplicity: No external dependencies
//    - Control: Customize for distributed systems needs
//    For production: spdlog recommended (async, fast, feature-rich)
//
//==============================================================================

//==============================================================================
// SECTION 9: BEST PRACTICES FOR DISTRIBUTED SYSTEMS
//==============================================================================
//
// BEST PRACTICE 1: Structured, Grep-able Log Format
// ==================================================
// Always use consistent prefixes for easy searching:
//
//    Logger::info("[RAFT::ELECTION] Node 2 starting election, term=5");
//    Logger::info("[WORKER::JOB] Received job_id=42 symbol=AAPL");
//    Logger::debug("[NETWORK::SEND] Sending 1024 bytes to " + peer_addr);
//
// Benefits:
//    grep "[RAFT::" *.log       # All Raft events
//    grep "job_id=42" *.log     # Track specific job across nodes
//    grep "\[ERROR\]" *.log     # All errors (across all components)
//
//
// BEST PRACTICE 2: Include Node/Thread Identifiers
// =================================================
// In distributed systems, identify WHERE log came from:
//
//    std::string node_prefix = "[Controller-1] ";
//    Logger::info(node_prefix + "Started election");
//    
//    std::string thread_prefix = "[Thread-" + GetThreadId() + "] ";
//    Logger::debug(thread_prefix + "Processing message");
//
// Enables: Correlate events across nodes and threads
//
//
// BEST PRACTICE 3: Log State Transitions
// =======================================
// Distributed systems have complex state machines:
//
//    Logger::info("[Raft] State: Follower -> Candidate (term " + 
//                 std::to_string(term) + ")");
//    Logger::info("[Raft] State: Candidate -> Leader");
//    Logger::info("[Worker] State: Idle -> Executing (job " + job_id + ")");
//
// Enables: Reconstruct state machine execution from logs
//
//
// BEST PRACTICE 4: Log with Context
// ==================================
// Include relevant contextual information:
//
//    BAD:  Logger::error("Connection failed");
//    GOOD: Logger::error("[Worker-3] Connection to controller failed: " +
//                          "host=" + controller_host + " port=" + port +
//                          " error=" + strerror(errno) + " attempt=3/5");
//
// Enables: Debug without needing to reproduce issue
//
//
// BEST PRACTICE 5: Use DEBUG for Development, INFO for Production
// ================================================================
// During development:
//    Logger::set_level(LogLevel::DEBUG);  // See everything
//
// In production/evaluation:
//    Logger::set_level(LogLevel::INFO);   // Balanced visibility
//
// For troubleshooting:
//    Logger::set_level(LogLevel::DEBUG);  // Temporary, then back to INFO
//
//
// BEST PRACTICE 6: Measure Timing with Logs
// ==========================================
// Use logs to measure distributed system performance:
//
//    auto start = std::chrono::high_resolution_clock::now();
//    Logger::info("[Job-42] Started backtest");
//    
//    PerformBacktest();
//    
//    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
//        std::chrono::high_resolution_clock::now() - start).count();
//    Logger::info("[Job-42] Completed in " + std::to_string(duration) + "ms");
//
// Then grep logs to analyze:
//    grep "Completed in" worker.log | awk '{print $8}' | sort -n
//
//
// BEST PRACTICE 7: Log Network Events
// ====================================
// Network is primary failure mode in distributed systems:
//
//    Logger::debug("[Network] Connecting to " + peer + ":" + port);
//    Logger::info("[Network] Connected to " + peer);
//    Logger::error("[Network] Connection lost to " + peer + ": " + error);
//    Logger::info("[Network] Reconnected to " + peer + " after " + retries);
//
//
// BEST PRACTICE 8: Don't Log in Hot Paths
// ========================================
// Identify CPU-intensive code paths and minimize logging:
//
//    GOOD:
//    Logger::info("Processing 1M records");
//    for (int i = 0; i < 1000000; ++i) {
//        ProcessRecord(i);  // No logging inside loop
//    }
//    Logger::info("Completed processing");
//    
//    BAD:
//    for (int i = 0; i < 1000000; ++i) {
//        Logger::debug("Record " + std::to_string(i));  // 1M log calls!
//        ProcessRecord(i);
//    }
//
//==============================================================================

//==============================================================================
// SECTION 10: PERFORMANCE OPTIMIZATION GUIDE
//==============================================================================
//
// OPTIMIZATION 1: Fast Path for Disabled Logs
// ============================================
// Already implemented: Level check before mutex
//    if (level < current_level_) return;  // No lock needed
//
// Impact: 125x faster for disabled logs (8ns vs 1μs)
//
//
// OPTIMIZATION 2: Avoid String Concatenation for Disabled Logs
// =============================================================
// Problem:
//    Logger::debug("Value: " + ExpensiveToString(value));
//    // ExpensiveToString() always called, even if DEBUG disabled!
//
// Solution: Guard with level check
//    if (current_level_ <= LogLevel::DEBUG) {
//        Logger::debug("Value: " + ExpensiveToString(value));
//    }
//
// Or: Add to Logger class
//    static bool is_debug_enabled() {
//        return current_level_ <= LogLevel::DEBUG;
//    }
//
//
// OPTIMIZATION 3: Use '\n' Instead of std::endl for High-Volume Logs
// ===================================================================
// Current: std::cout << message << std::endl;  // Flushes every time
// Faster:  std::cout << message << '\n';       // Buffered, periodic flush
//
// Trade-off: Messages may not appear immediately (buffering delay)
// When to use: High-frequency DEBUG logs where immediate visibility not critical
//
//
// OPTIMIZATION 4: Consider Asynchronous Logging
// ==============================================
// For very high throughput (>10,000 logs/sec):
//
// Architecture:
//    Main threads -> Lock-free queue -> Background writer thread -> Output
//
// Benefits:
//    - Main threads: Just push to queue (~100 ns)
//    - Background thread: Batch writes (amortize I/O cost)
//    - 10-100x throughput improvement
//
// Complexity:
//    - Thread management (start/stop)
//    - Queue sizing (prevent overflow)
//    - Shutdown coordination (flush pending logs)
//
// Libraries: spdlog (async mode), NanoLog
//
//
// OPTIMIZATION 5: Binary Logging for Maximum Performance
// =======================================================
// For extreme performance requirements:
//
//    // Write binary log entries
//    struct LogEntry {
//        uint64_t timestamp_ns;
//        uint32_t level;
//        uint32_t message_id;  // String table lookup
//        uint8_t data[256];
//    };
//    write(log_fd, &entry, sizeof(entry));
//
// Post-process to human-readable text:
//    ./log_parser binary.log > readable.log
//
// Trade-off: Cannot read logs in real-time (must post-process)
//
//==============================================================================

//==============================================================================
// SECTION 11: TESTING STRATEGIES
//==============================================================================
//
// TEST 1: Verify Log Levels Filter Correctly
// ===========================================
//    TEST(LoggerTest, LevelFiltering) {
//        testing::internal::CaptureStdout();
//        
//        Logger::set_level(LogLevel::INFO);
//        Logger::debug("Should not appear");
//        Logger::info("Should appear");
//        
//        std::string output = testing::internal::GetCapturedStdout();
//        
//        ASSERT_EQ(output.find("Should not appear"), std::string::npos);
//        ASSERT_NE(output.find("Should appear"), std::string::npos);
//    }
//
//
// TEST 2: Verify Thread Safety
// =============================
//    TEST(LoggerTest, ConcurrentLogging) {
//        constexpr int NUM_THREADS = 10;
//        constexpr int LOGS_PER_THREAD = 1000;
//        
//        std::vector<std::thread> threads;
//        for (int i = 0; i < NUM_THREADS; ++i) {
//            threads.emplace_back([i]() {
//                for (int j = 0; j < LOGS_PER_THREAD; ++j) {
//                    Logger::info("Thread " + std::to_string(i) + 
//                                " message " + std::to_string(j));
//                }
//            });
//        }
//        
//        for (auto& t : threads) {
//            t.join();
//        }
//        
//        // Manually verify no corrupted output
//        // Automated: Parse log file, count entries, check integrity
//    }
//
//
// TEST 3: Verify Timestamp Precision
// ===================================
//    TEST(LoggerTest, TimestampPrecision) {
//        testing::internal::CaptureStdout();
//        
//        Logger::info("Message 1");
//        std::this_thread::sleep_for(std::chrono::milliseconds(50));
//        Logger::info("Message 2");
//        
//        std::string output = testing::internal::GetCapturedStdout();
//        
//        // Parse timestamps, verify >=50ms difference
//        // Regex: \[(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3})\]
//    }
//
//
// TEST 4: Verify Output Format
// =============================
//    TEST(LoggerTest, OutputFormat) {
//        testing::internal::CaptureStdout();
//        Logger::info("Test message");
//        std::string output = testing::internal::GetCapturedStdout();
//        
//        // Expected format: [YYYY-MM-DD HH:MM:SS.mmm] [INFO] Test message\n
//        ASSERT_TRUE(std::regex_match(output, 
//            std::regex(R"(\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] \[INFO\] Test message\n)")));
//    }
//
//==============================================================================

//==============================================================================
// SECTION 12: TROUBLESHOOTING CHECKLIST
//==============================================================================
//
// PROBLEM: Linker error "undefined reference to Logger::current_level_"
// ======================================================================
// SOLUTION:
//    ☐ Verify logger.cpp defines static members:
//       LogLevel Logger::current_level_ = LogLevel::INFO;
//       std::mutex Logger::mutex_;
//    ☐ Ensure logger.cpp is compiled and linked:
//       g++ -o program main.cpp logger.cpp
//    ☐ Check CMakeLists.txt includes logger.cpp:
//       add_executable(program main.cpp logger.cpp)
//
//
// PROBLEM: Log messages not appearing
// ====================================
// SOLUTION:
//    ☐ Check log level: Logger::set_level(LogLevel::DEBUG);
//    ☐ Verify message level >= current_level_
//    ☐ Check stdout is not redirected/suppressed
//    ☐ Try explicit flush: std::cout.flush();
//    ☐ Verify logger.cpp is linked (see above)
//
//
// PROBLEM: Garbled/interleaved log output
// ========================================
// SOLUTION:
//    ☐ Verify mutex is used in log() method
//    ☐ Check std::lock_guard scope (should cover entire output operation)
//    ☐ Ensure separate log files for multiple processes
//    ☐ Use file locking if sharing log file across processes
//
//
// PROBLEM: Program hangs during logging
// ======================================
// SOLUTION:
//    ☐ Check for deadlock: Same thread locking mutex twice
//    ☐ Verify no Logger calls inside Logger (while holding lock)
//    ☐ Use gdb backtrace: gdb -p <pid>, then "bt" and "thread apply all bt"
//    ☐ Consider std::recursive_mutex if nested locking unavoidable
//
//
// PROBLEM: Disk space exhausted
// ==============================
// SOLUTION:
//    ☐ Reduce log level: Logger::set_level(LogLevel::INFO);
//    ☐ Implement log rotation (delete old logs)
//    ☐ Remove DEBUG logs from tight loops
//    ☐ Check quota: quota -v
//    ☐ Compress old logs: gzip *.log
//
//
// PROBLEM: Performance degradation
// =================================
// SOLUTION:
//    ☐ Profile logging overhead: Time with vs. without logging
//    ☐ Reduce DEBUG logging in hot paths
//    ☐ Guard expensive string operations with level check
//    ☐ Consider asynchronous logging for high throughput
//    ☐ Use '\n' instead of std::endl (avoid flush overhead)
//
//==============================================================================

//==============================================================================
// END OF DOCUMENTATION
//==============================================================================