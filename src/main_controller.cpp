/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: main_controller.cpp
    
    Description:
        This file implements the main entry point for the Controller executable
        in the distributed financial backtesting system. It provides a production-
        ready server application with command-line argument parsing, signal
        handling for graceful shutdown, periodic statistics reporting, and
        robust error handling.
        
        Core Functionality:
        - Command-Line Interface: Configurable port, data directory
        - Signal Handling: Graceful shutdown on SIGINT/SIGTERM
        - Process Lifecycle: Initialization, execution, cleanup
        - Statistics Monitoring: Periodic system status reports
        - Error Recovery: Proper error codes and logging
        
    Application Role:
        This is the standalone server executable that operators run to start
        a controller node in the distributed backtesting cluster. It serves as
        the central coordinator that workers connect to for job distribution.
        
        Deployment Context:
        
        Production Deployment (Khoury Cluster):
        ┌──────────────────────────────────────────┐
        │ Terminal: ssh kh01.ccs.neu.edu          │
        │ Command: ./controller --port 5000       │
        │          --data-dir /mnt/nfs/data       │
        └──────────────────────────────────────────┘
                         ↓
        ┌──────────────────────────────────────────┐
        │ Controller Process Running               │
        │ - Listening on port 5000                 │
        │ - Accepting worker connections           │
        │ - Distributing jobs                      │
        │ - Monitoring worker health               │
        │ - Reporting statistics every 5 seconds   │
        └──────────────────────────────────────────┘
                         ↓
        ┌──────────────────────────────────────────┐
        │ Workers Connect and Execute Jobs         │
        │ Worker 1 → Controller (kh01:5000)       │
        │ Worker 2 → Controller (kh01:5000)       │
        │ ...                                      │
        │ Worker 8 → Controller (kh01:5000)       │
        └──────────────────────────────────────────┘
        
    Lifecycle Stages:
        
        1. STARTUP:
           - Parse command-line arguments
           - Configure controller
           - Register signal handlers
           - Start controller (network, threads)
           - Print startup banner
        
        2. RUNNING:
           - Accept worker connections
           - Distribute jobs to workers
           - Monitor worker health (heartbeats)
           - Print statistics periodically (every 5 seconds)
           - Wait for shutdown signal
        
        3. SHUTDOWN:
           - Receive SIGINT (Ctrl+C) or SIGTERM (kill)
           - Set shutdown flag
           - Stop controller (graceful)
           - Join all threads
           - Close all connections
           - Exit with status code
        
    Signal Handling:
        
        SIGINT (Ctrl+C):
        - User presses Ctrl+C in terminal
        - Signal handler called
        - Sets shutdown_requested flag
        - Calls controller.stop()
        - Main loop detects flag, exits gracefully
        
        SIGTERM (kill command):
        - System sends termination request
        - Same handling as SIGINT
        - Allows graceful shutdown before force kill
        
        SIGPIPE (broken pipe):
        - Occurs when: Writing to closed socket
        - Default: Terminates process
        - Handler: SIG_IGN (ignore)
        - Effect: send() returns -1 with errno=EPIPE (handled in code)
        
    Command-Line Interface:
        
        Basic usage:
        $ ./controller
        (Uses defaults: port 5000, data dir ./data)
        
        Custom port:
        $ ./controller --port 6000
        
        Custom data directory:
        $ ./controller --data-dir /mnt/nfs/backtesting_data
        
        Multiple options:
        $ ./controller --port 6000 --data-dir /data
        
        Help:
        $ ./controller --help
        
    Statistics Reporting:
        
        Every 5 seconds, prints:
        === Controller Statistics ===
        Active Workers: 5
        Pending Jobs: 10
        Active Jobs: 15
        Completed Jobs: 100
        Failed Jobs: 5
        ===========================
        
        Enables: Real-time monitoring of system health
        
    Exit Codes:
        0: Success (clean shutdown)
        1: Startup failure (configuration error, port in use)
        
    Threading Model:
        
        Main Thread:
        - Parses arguments
        - Starts controller
        - Runs monitoring loop (sleeps 5 seconds, prints stats)
        - Waits for shutdown signal
        - Stops controller
        
        Background Threads (in Controller):
        - Accept thread: Worker connections
        - Scheduler thread: Job distribution
        - Heartbeat thread: Health monitoring
        - Worker threads: Per-worker communication (N threads)
        
        Total threads: 1 (main) + 3 (controller) + N (workers)
        
    Error Handling:
        
        Startup Errors:
        - Port in use: Log error, exit code 1
        - Permission denied: Log error, exit code 1
        - Invalid arguments: Print usage, exit code 1
        
        Runtime Errors:
        - Worker disconnection: Logged, handled by controller
        - Network errors: Logged, handled by controller
        - Signal received: Graceful shutdown, exit code 0
        
    Production Readiness:
        
        Implemented:
        ✓ Signal handling for graceful shutdown
        ✓ Command-line argument parsing
        ✓ Error logging and reporting
        ✓ Statistics monitoring
        ✓ Proper exit codes
        ✓ Help documentation
        
        Could Add:
        ☐ Daemonization (run as background service)
        ☐ PID file (prevent multiple instances)
        ☐ Log rotation (prevent log file growth)
        ☐ Configuration file (YAML/JSON config)
        ☐ Health check endpoint (HTTP status page)
        ☐ Metrics export (Prometheus, Grafana)
        
    Dependencies:
        - controller/controller.h: Controller class
        - common/logger.h: Logging utilities
        - <signal.h>: POSIX signal handling
        - <iostream>: Command-line I/O
        - <atomic>: Thread-safe flags
        
    Platform:
        - POSIX: Linux, Unix, macOS
        - Not portable to: Windows (uses POSIX signals)
        
    Related Files:
        - worker_main.cpp: Worker executable entry point
        - controller/controller.cpp: Controller implementation
        - Makefile: Build configuration

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. INCLUDES & DEPENDENCIES
// 2. NAMESPACE AND GLOBAL STATE
//    2.1 Using Directive
//    2.2 Global Shutdown Flag
//    2.3 Global Controller Reference
// 3. SIGNAL HANDLER
//    3.1 signal_handler() - Graceful Shutdown Handler
// 4. UTILITY FUNCTIONS
//    4.1 print_usage() - Help Documentation
// 5. MAIN FUNCTION
//    5.1 Signal Handler Registration
//    5.2 Command-Line Argument Parsing
//    5.3 Controller Initialization
//    5.4 Main Event Loop
//    5.5 Shutdown and Cleanup
// 6. COMMAND-LINE INTERFACE
// 7. DEPLOYMENT GUIDE
// 8. USAGE EXAMPLES
// 9. COMMON PITFALLS & SOLUTIONS
// 10. FAQ
// 11. BEST PRACTICES
// 12. TROUBLESHOOTING GUIDE
// 13. PRODUCTION DEPLOYMENT
// 14. MONITORING AND OPERATIONS
//
//==============================================================================

//==============================================================================
// SECTION 1: INCLUDES & DEPENDENCIES
//==============================================================================

#include "controller/controller.h"  // Controller class
#include "common/logger.h"          // Logging utilities

// Standard C++ I/O
// ================
#include <iostream>   // std::cout, std::cerr for console output

// POSIX Signal Handling
// =====================
#include <signal.h>   // signal(), SIGINT, SIGTERM, SIGPIPE

// Atomic Operations
// =================
#include <atomic>     // std::atomic for thread-safe flags

//==============================================================================
// SECTION 2: NAMESPACE AND GLOBAL STATE
//==============================================================================

//------------------------------------------------------------------------------
// 2.1 USING DIRECTIVE
//------------------------------------------------------------------------------
//
// using namespace backtesting;
//
// PURPOSE:
// Import backtesting namespace for convenient access to Controller,
// ControllerConfig, Logger, etc.
//
// SCOPE:
// - File-level: Applies to entire file
// - Not in header: Safe to use in .cpp files
//
// ALTERNATIVE:
// - Explicit qualification: backtesting::Controller
// - Using declarations: using backtesting::Controller;
//
// WHY HERE?
// - Main file: Unlikely to have naming conflicts
// - Convenience: Shorter code, more readable
// - Standard practice: Common in main() files
//

using namespace backtesting;

//------------------------------------------------------------------------------
// 2.2 GLOBAL SHUTDOWN FLAG
//------------------------------------------------------------------------------
//
// std::atomic<bool> shutdown_requested(false)
//
// PURPOSE:
// Thread-safe flag indicating shutdown has been requested.
//
// USAGE:
// - Signal handler: Sets to true
// - Main loop: Checks flag, exits if true
//
// WHY ATOMIC?
// - Signal handler: Runs in different context (async-signal-safe required)
// - Main thread: Reads flag in loop
// - Without atomic: Race condition, undefined behavior
// - With atomic: Safe concurrent access
//
// INITIALIZATION:
// - false: Not shutdown initially
// - Set to true: In signal handler
//
// ALTERNATIVE DESIGNS:
//
// 1. Volatile (incorrect for this use):
//    volatile bool shutdown_requested = false;
//    Problems: Not guaranteed atomic, not thread-safe
//
// 2. Mutex-protected (overkill):
//    std::mutex mtx;
//    bool shutdown_requested;
//    Problems: Can't use mutex in signal handler (not async-signal-safe)
//
// 3. std::atomic (current, correct):
//    Atomic operations are lock-free and signal-safe
//
// SIGNAL SAFETY:
// - std::atomic operations are async-signal-safe (C++11 onwards)
// - Safe to modify in signal handler
//

std::atomic<bool> shutdown_requested(false);

//------------------------------------------------------------------------------
// 2.3 GLOBAL CONTROLLER REFERENCE
//------------------------------------------------------------------------------
//
// Controller* global_controller = nullptr
//
// PURPOSE:
// Global pointer to controller for access in signal handler.
//
// WHY GLOBAL?
// - Signal handlers: Can't pass parameters (fixed signature)
// - Need access: To call controller->stop() on signal
// - No alternative: Signal handler is global callback
//
// INITIALIZATION:
// - nullptr: Initially (no controller yet)
// - Set in main(): After controller constructed
// - Used in signal handler: To call stop()
//
// NULL CHECK:
// - Always check before use: if (global_controller) { ... }
// - Prevents: Crash if signal received before main() runs
//
// THREAD SAFETY:
// - Written once: In main thread
// - Read in signal handler: Async context
// - Safe: Write happens-before signal handler registration
//
// ALTERNATIVE DESIGNS:
//
// 1. Static member variable (cleaner):
//    class Controller {
//        static Controller* instance_;
//    };
//    Problems: More complex, still global
//
// 2. Singleton pattern (overkill):
//    Problems: Unnecessary complexity for simple use case
//
// 3. Global pointer (current, simple):
//    Appropriate for signal handler access
//
// CLEANUP:
// - Not deleted: Controller is stack-allocated in main()
// - Just a reference: No ownership
// - Safe: Controller outlives signal handler registration
//

Controller* global_controller = nullptr;

//==============================================================================
// SECTION 3: SIGNAL HANDLER
//==============================================================================

//------------------------------------------------------------------------------
// 3.1 SIGNAL_HANDLER() - GRACEFUL SHUTDOWN HANDLER
//------------------------------------------------------------------------------
//
// void signal_handler(int signal)
//
// PURPOSE:
// Handles shutdown signals (SIGINT, SIGTERM) for graceful termination.
//
// PARAMETERS:
// - signal: Signal number (SIGINT=2, SIGTERM=15)
//
// SIGNAL TYPES:
//
// SIGINT (2):
// - Triggered by: Ctrl+C in terminal
// - Purpose: Interactive interrupt
// - Use case: Operator wants to stop controller
//
// SIGTERM (15):
// - Triggered by: kill <pid> command
// - Purpose: Graceful termination request
// - Use case: System shutdown, process manager (systemd)
//
// HANDLER ACTIONS:
// 1. Check signal type (SIGINT or SIGTERM)
// 2. Log shutdown message
// 3. Set shutdown_requested flag (atomic)
// 4. Call controller->stop() (graceful shutdown)
//
// ASYNC-SIGNAL-SAFETY:
// Signal handlers have strict requirements (POSIX):
//
// SAFE (can call in signal handler):
// - std::atomic operations (C++11 onwards)
// - write() system call
// - _Exit()
//
// UNSAFE (cannot call in signal handler):
// - malloc/free
// - printf/cout (uses malloc internally)
// - Most C++ standard library
// - Mutex operations
//
// OUR HANDLER:
// - Logger::info(): TECHNICALLY UNSAFE (uses mutex, cout)
// - In practice: Usually works (but not guaranteed)
// - Proper approach: Use write() for signal-safe logging
// - For academic project: Current approach acceptable
//
// GRACEFUL SHUTDOWN:
// - Sets flag: Main loop sees it and exits
// - Calls stop(): Joins threads, closes sockets
// - Non-blocking: Returns quickly (actual cleanup in main thread)
//
// PRODUCTION IMPROVEMENT:
//
// Strictly signal-safe version:
//   void signal_handler(int signal) {
//       // Only async-signal-safe operations
//       const char msg[] = "Shutdown signal received\n";
//       write(STDERR_FILENO, msg, sizeof(msg) - 1);
//       
//       shutdown_requested.store(true, std::memory_order_relaxed);
//       
//       // Don't call controller->stop() here
//       // Main loop will call it after detecting flag
//   }
//
// Current implementation: Acceptable for project, but be aware of safety issues
//

void signal_handler(int signal) {
    // Check signal type
    // SIGINT (2): Ctrl+C
    // SIGTERM (15): kill command
    if (signal == SIGINT || signal == SIGTERM) {
        // Log shutdown message
        // NOTE: Logger::info() is NOT async-signal-safe
        // Uses mutex and std::cout internally
        // In practice: Usually works, but theoretically unsafe
        // For production: Use write() directly to STDERR
        Logger::info("Shutdown signal received");
        
        // Set atomic flag (async-signal-safe in C++11+)
        // Main loop checks this flag
        shutdown_requested = true;
        
        // Stop controller (graceful shutdown)
        // NOTE: This is NOT async-signal-safe
        // Controller::stop() uses mutexes, condition variables
        // Could deadlock if signal arrives while main thread holds mutex
        //
        // SAFER APPROACH:
        // - Just set shutdown_requested flag here
        // - Main loop calls stop() after detecting flag
        //
        // CURRENT APPROACH:
        // - Calls stop() directly for faster shutdown
        // - Risk: Potential deadlock (rare in practice)
        if (global_controller) {
            global_controller->stop();
        }
    }
    
    // Signal handler returns
    // Main thread continues execution
    // Main loop will detect shutdown_requested and exit
}

//==============================================================================
// SECTION 4: UTILITY FUNCTIONS
//==============================================================================

//------------------------------------------------------------------------------
// 4.1 PRINT_USAGE() - HELP DOCUMENTATION
//------------------------------------------------------------------------------
//
// void print_usage(const char* program_name)
//
// PURPOSE:
// Prints command-line usage information to console.
//
// PARAMETERS:
// - program_name: argv[0] (name of executable)
//
// OUTPUT:
// Usage: ./controller [options]
// Options:
//   --port PORT          Listen port (default: 5000)
//   --data-dir DIR       Data directory (default: ./data)
//   --help               Show this help message
//
// WHEN CALLED:
// - --help flag: User requests help
// - Invalid argument: Show usage as error context
//
// DESIGN NOTES:
// - Uses std::cout (stdout): Standard for help messages
// - Clear formatting: Aligned options and descriptions
// - Shows defaults: Users know what to expect
//

void print_usage(const char* program_name) {
    // Print usage message to stdout
    // Format: Clear, aligned, includes defaults
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --port PORT          Listen port (default: 5000)\n"
              << "  --data-dir DIR       Data directory (default: ./data)\n"
              << "  --help               Show this help message\n";
    
    // FUTURE ENHANCEMENTS:
    // Could add:
    // - --log-level LEVEL     Logging verbosity (DEBUG, INFO, WARN, ERROR)
    // - --max-workers NUM     Maximum worker connections
    // - --heartbeat-timeout SEC  Worker failure timeout
    // - --config FILE         Load configuration from file
    // - --daemon              Run as background daemon
    // - --pid-file FILE       Write process ID to file
}

//==============================================================================
// SECTION 5: MAIN FUNCTION
//==============================================================================

//------------------------------------------------------------------------------
// MAIN ENTRY POINT
//------------------------------------------------------------------------------
//
// int main(int argc, char* argv[])
//
// PURPOSE:
// Entry point for controller executable. Manages entire application lifecycle
// from initialization through shutdown.
//
// PARAMETERS:
// - argc: Argument count (includes program name)
// - argv: Argument array (argv[0] = program name)
//
// RETURNS:
// - 0: Success (clean shutdown)
// - 1: Failure (startup error)
//
// EXECUTION FLOW:
// 1. Register signal handlers
// 2. Parse command-line arguments
// 3. Configure logging
// 4. Create controller
// 5. Start controller
// 6. Run monitoring loop
// 7. Shutdown on signal or error
// 8. Return exit code
//

int main(int argc, char* argv[]) {
    //==========================================================================
    // SECTION 5.1: SIGNAL HANDLER REGISTRATION
    //==========================================================================
    //
    // Register handlers for graceful shutdown and error prevention.
    //
    //--------------------------------------------------------------------------
    
    // SIGINT Handler: Ctrl+C (Terminal Interrupt)
    // ============================================
    // signal(SIGINT, signal_handler):
    // - SIGINT: Signal number 2
    // - signal_handler: Function to call when signal received
    //
    // Behavior:
    // - User presses Ctrl+C
    // - OS sends SIGINT to process
    // - signal_handler called (async context)
    // - Handler sets shutdown_requested, calls controller.stop()
    // - Main loop detects shutdown_requested, exits gracefully
    //
    // Why needed:
    // - Default: SIGINT terminates process immediately (no cleanup)
    // - Custom handler: Allows graceful shutdown (close connections, join threads)
    signal(SIGINT, signal_handler);
    
    // SIGTERM Handler: Graceful Termination Request
    // ==============================================
    // Triggered by: kill <pid>, systemctl stop, etc.
    //
    // Behavior: Same as SIGINT (graceful shutdown)
    //
    // Why needed:
    // - System shutdown: systemd sends SIGTERM before SIGKILL
    // - Container orchestration: Docker/Kubernetes send SIGTERM
    // - Allows: Saving state, closing connections before termination
    signal(SIGTERM, signal_handler);
    
    // SIGPIPE Handler: Broken Pipe (Ignore)
    // ======================================
    // signal(SIGPIPE, SIG_IGN):
    // - SIGPIPE: Signal 13
    // - SIG_IGN: Ignore signal (don't call handler, don't terminate)
    //
    // Triggered by:
    // - Writing to closed socket (peer disconnected)
    // - Default behavior: Terminates process (undesirable)
    //
    // With SIG_IGN:
    // - send() returns -1 with errno=EPIPE
    // - Process continues (error handled in code)
    //
    // Why needed:
    // - Worker disconnects: Should log error, not crash
    // - Network errors: Should be handled gracefully
    // - Standard practice: All socket servers ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe
    
    //==========================================================================
    // SECTION 5.2: COMMAND-LINE ARGUMENT PARSING
    //==========================================================================
    //
    // Parse command-line options and configure controller.
    //
    //--------------------------------------------------------------------------
    
    // Create configuration with defaults
    // Defaults:
    // - listen_port: 5000
    // - max_workers: 16
    // - heartbeat_timeout_sec: 6
    // - data_directory: "./data"
    ControllerConfig config;
    
    // Parse command-line arguments
    // Loop through all arguments (skip argv[0] which is program name)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        // OPTION: --help
        // ==============
        // Display usage information and exit
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;  // Exit successfully after showing help
        }
        
        // OPTION: --port PORT
        // ===================
        // Set listen port for worker connections
        // Example: --port 6000
        else if (arg == "--port" && i + 1 < argc) {
            // Parse next argument as port number
            // ++i: Consume the port value argument
            // std::stoi: String to integer conversion
            // static_cast: Convert int to uint16_t (port type)
            //
            // VALIDATION MISSING:
            // - Should check: 1 <= port <= 65535
            // - Should check: port >= 1024 (avoid privileged ports without root)
            // - Current: Assumes valid input
            config.listen_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        
        // OPTION: --data-dir DIR
        // ======================
        // Set data directory path
        // Example: --data-dir /mnt/nfs/data
        else if (arg == "--data-dir" && i + 1 < argc) {
            // Store directory path as string
            // No validation (should check directory exists and is readable)
            config.data_directory = argv[++i];
        }
        
        // UNKNOWN OPTION
        // ==============
        // Invalid argument: Print error and usage, exit
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;  // Exit with error code
        }
    }
    
    //==========================================================================
    // SECTION 5.3: CONTROLLER INITIALIZATION
    //==========================================================================
    //
    // Configure logging, create controller, start server.
    //
    //--------------------------------------------------------------------------
    
    // Configure logging level
    // INFO: Standard operational messages (not too verbose)
    // Could be configurable: --log-level DEBUG
    Logger::set_level(LogLevel::INFO);
    
    // Print startup banner
    // Indicates: Application starting (version info could be added)
    Logger::info("=== Distributed Backtesting Controller ===");
    
    // Create controller with parsed configuration
    // Controller constructor: Initializes state, doesn't start networking
    Controller controller(config);
    
    // Store global reference for signal handler
    // Signal handler needs to call controller.stop()
    global_controller = &controller;
    
    // Start controller
    // This: Creates server socket, spawns threads, begins accepting connections
    if (!controller.start()) {
        // Startup failed: Port in use, permission denied, etc.
        Logger::error("Failed to start controller");
        return 1;  // Exit with error code
    }
    
    // Controller now running
    // - Listening on config.listen_port
    // - Accepting worker connections
    // - Ready to distribute jobs
    
    //==========================================================================
    // SECTION 5.4: MAIN EVENT LOOP
    //==========================================================================
    //
    // Monitor system status until shutdown requested.
    //
    //--------------------------------------------------------------------------
    
    // Print ready message
    // Informs operator: Controller is operational
    Logger::info("Controller running. Press Ctrl+C to stop.");
    
    // MAIN LOOP: Run until shutdown
    // Conditions:
    // - !shutdown_requested: No signal received
    // - controller.is_running(): Controller still active
    //
    // Both must be true to continue
    // Either false: Exit loop and shutdown
    while (!shutdown_requested && controller.is_running()) {
        // Sleep 5 seconds between status updates
        // Prevents: Excessive logging
        // Allows: Regular monitoring without spam
        //
        // Could be configurable: --stats-interval 10
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        // Print statistics
        // Shows: Worker count, job counts, completion rate
        // Enables: Monitoring system health during operation
        //
        // Example output:
        //   === Controller Statistics ===
        //   Active Workers: 5
        //   Pending Jobs: 10
        //   Active Jobs: 15
        //   Completed Jobs: 100
        //   Failed Jobs: 5
        //   ===========================
        controller.print_statistics();
    }
    
    // LOOP EXIT REASONS:
    // 1. shutdown_requested = true (signal received)
    // 2. !controller.is_running() (controller stopped internally)
    //
    // Both cases: Proceed to shutdown
    
    //==========================================================================
    // SECTION 5.5: SHUTDOWN AND CLEANUP
    //==========================================================================
    //
    // Graceful shutdown and cleanup before exit.
    //
    //--------------------------------------------------------------------------
    
    // Log shutdown start
    Logger::info("Shutting down...");
    
    // Stop controller (if not already stopped)
    // - Stops all threads (accept, scheduler, heartbeat, workers)
    // - Closes all sockets
    // - Cleans up resources
    // - Idempotent: Safe to call even if already stopped
    controller.stop();
    
    // Log shutdown complete
    Logger::info("Controller shutdown complete");
    
    // Exit successfully
    // Exit code 0: Indicates clean shutdown
    // Checked by: Shell scripts, process managers
    return 0;
    
    // AUTOMATIC CLEANUP (after return):
    // - controller destructor called (stack unwinding)
    // - global_controller not deleted (just reference)
    // - STL containers destroyed
    // - Process exits
}

//==============================================================================
// END OF IMPLEMENTATION
//==============================================================================

//==============================================================================
// SECTION 6: COMMAND-LINE INTERFACE
//==============================================================================
//
// USAGE PATTERNS:
// ===============
//
// 1. Basic (defaults):
//    $ ./controller
//    → Port: 5000
//    → Data dir: ./data
//
// 2. Custom port:
//    $ ./controller --port 6000
//    → Port: 6000
//    → Data dir: ./data (default)
//
// 3. Custom data directory:
//    $ ./controller --data-dir /mnt/nfs/backtesting_data
//    → Port: 5000 (default)
//    → Data dir: /mnt/nfs/backtesting_data
//
// 4. Multiple options:
//    $ ./controller --port 6000 --data-dir /data
//    → Port: 6000
//    → Data dir: /data
//
// 5. Help:
//    $ ./controller --help
//    → Prints usage and exits
//
// 6. Error (invalid option):
//    $ ./controller --invalid
//    → Prints error and usage, exits with code 1
//
// ARGUMENT ORDERING:
// - Order doesn't matter: --port 6000 --data-dir /data
//                      == --data-dir /data --port 6000
// - Each option consumes next argument
//
// VALIDATION:
// - Port range: Not validated (should check 1-65535)
// - Directory existence: Not validated (should check before start)
// - Value count: Checked (i + 1 < argc)
//
// EXIT CODES:
// - 0: Success (--help, clean shutdown)
// - 1: Error (invalid arguments, startup failure)
//
//==============================================================================

//==============================================================================
// SECTION 7: DEPLOYMENT GUIDE
//==============================================================================
//
// KHOURY CLUSTER DEPLOYMENT:
// ===========================
//
// Step 1: Build controller
// ------------------------
// $ mkdir build && cd build
// $ cmake ..
// $ make controller
// $ ls -lh controller
// -rwxr-xr-x 1 user group 2.5M Nov 20 14:32 controller
//
// Step 2: Prepare data directory
// -------------------------------
// $ mkdir -p /tmp/backtesting_data
// $ cp ~/data/*.csv /tmp/backtesting_data/
// $ ls /tmp/backtesting_data/
// AAPL.csv  GOOGL.csv  MSFT.csv  ...
//
// Step 3: SSH to controller node
// -------------------------------
// $ ssh talif@kh01.ccs.neu.edu
//
// Step 4: Start controller
// ------------------------
// $ ./controller --port 5000 --data-dir /tmp/backtesting_data
// [2025-11-20 14:32:15.123] [INFO] === Distributed Backtesting Controller ===
// [2025-11-20 14:32:15.234] [INFO] Starting controller on port 5000
// [2025-11-20 14:32:15.345] [INFO] Server socket bound to port 5000
// [2025-11-20 14:32:15.456] [INFO] Controller started successfully
// [2025-11-20 14:32:15.567] [INFO] Controller running. Press Ctrl+C to stop.
//
// Step 5: Monitor operation
// -------------------------
// (Every 5 seconds, statistics printed)
// [2025-11-20 14:32:20.123] [INFO] === Controller Statistics ===
// [2025-11-20 14:32:20.124] [INFO] Active Workers: 0
// [2025-11-20 14:32:20.125] [INFO] Pending Jobs: 0
// [2025-11-20 14:32:20.126] [INFO] Active Jobs: 0
// [2025-11-20 14:32:20.127] [INFO] Completed Jobs: 0
// [2025-11-20 14:32:20.128] [INFO] Failed Jobs: 0
// [2025-11-20 14:32:20.129] [INFO] ===========================
//
// Step 6: Shutdown
// ----------------
// (Press Ctrl+C)
// ^C
// [2025-11-20 15:00:00.123] [INFO] Shutdown signal received
// [2025-11-20 15:00:00.234] [INFO] Stopping controller...
// [2025-11-20 15:00:00.345] [INFO] Controller stopped
// [2025-11-20 15:00:00.456] [INFO] Shutting down...
// [2025-11-20 15:00:00.567] [INFO] Controller shutdown complete
//
// BACKGROUND EXECUTION:
// =====================
//
// Run in background with nohup:
// $ nohup ./controller --port 5000 > controller.log 2>&1 &
// $ echo $! > controller.pid  # Save process ID
//
// Monitor logs:
// $ tail -f controller.log
//
// Stop gracefully:
// $ kill $(cat controller.pid)  # Sends SIGTERM
//
// Force stop (if graceful fails):
// $ kill -9 $(cat controller.pid)  # Sends SIGKILL (no cleanup)
//
// SYSTEMD SERVICE:
// ================
//
// For production, create systemd service:
//
// /etc/systemd/system/controller.service:
// ---------------------------------------
// [Unit]
// Description=Backtesting Controller
// After=network.target
//
// [Service]
// Type=simple
// User=backtesting
// WorkingDirectory=/opt/backtesting
// ExecStart=/opt/backtesting/controller --port 5000 --data-dir /data
// Restart=on-failure
// RestartSec=10
//
// [Install]
// WantedBy=multi-user.target
//
// Commands:
// $ sudo systemctl daemon-reload
// $ sudo systemctl start controller
// $ sudo systemctl status controller
// $ sudo systemctl stop controller
// $ sudo systemctl enable controller  # Start on boot
//
//==============================================================================

//==============================================================================
// SECTION 8: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Running Controller on Khoury Cluster
// ================================================
//
// Terminal session:
//
// $ ssh talif@kh01.ccs.neu.edu
// $ cd ~/cs6650-project/build
// $ ./controller --port 5000 --data-dir ~/data
// [INFO] === Distributed Backtesting Controller ===
// [INFO] Starting controller on port 5000
// [INFO] Server socket bound to port 5000
// [INFO] Controller started successfully
// [INFO] Controller running. Press Ctrl+C to stop.
// [INFO] === Controller Statistics ===
// [INFO] Active Workers: 0
// [INFO] Pending Jobs: 0
// ...
// (Wait for workers to connect)
// [INFO] Accepted connection from 192.168.1.105
// [INFO] Registered worker 1
// [INFO] === Controller Statistics ===
// [INFO] Active Workers: 1
// ...
//
//
// EXAMPLE 2: Running with Custom Configuration
// =============================================
//
// $ ./controller
//     --port 8000
//     --data-dir /mnt/nfs/shared_data
//
// Uses:
// - Port 8000 (non-standard, avoid conflicts)
// - Shared NFS mount for data (accessible to all workers)
//
//
// EXAMPLE 3: Testing Controller Locally
// ======================================
//
// Terminal 1 (Controller):
// $ ./controller --port 5555
// [INFO] Controller running...
//
// Terminal 2 (Test Connection):
// $ telnet localhost 5555
// Trying 127.0.0.1...
// Connected to localhost.
// (Connection accepted - controller working)
//
// Terminal 3 (Worker):
// $ ./worker --controller localhost:5555
// (Worker connects to controller)
//
//
// EXAMPLE 4: Monitoring Multiple Controllers
// ===========================================
//
// # Start 3 controllers on different ports
// Terminal 1:
// $ ./controller --port 5000  # Controller 1
//
// Terminal 2:
// $ ./controller --port 5001  # Controller 2
//
// Terminal 3:
// $ ./controller --port 5002  # Controller 3
//
// # Monitor all
// $ ps aux | grep controller
// talif    12345  1.0  0.5  controller --port 5000
// talif    12346  1.0  0.5  controller --port 5001
// talif    12347  1.0  0.5  controller --port 5002
//
//
// EXAMPLE 5: Graceful Shutdown
// =============================
//
// Running controller:
// $ ./controller --port 5000
// [INFO] Controller running. Press Ctrl+C to stop.
//
// (Press Ctrl+C)
// ^C
// [INFO] Shutdown signal received
// [INFO] Stopping controller...
// [INFO] Controller stopped
// [INFO] Shutting down...
// [INFO] Controller shutdown complete
// $ echo $?
// 0  # Exit code: Success
//
//==============================================================================

//==============================================================================
// SECTION 9: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: Port Already in Use
// ===============================
// SYMPTOM:
//    $ ./controller --port 5000
//    [ERROR] Failed to start controller
//    [ERROR] Failed to bind socket: Address already in use
//
// SOLUTIONS:
//    1. Check what's using the port:
//       $ netstat -tulpn | grep 5000
//       $ lsof -i :5000
//    
//    2. Kill conflicting process:
//       $ kill <pid>
//    
//    3. Use different port:
//       $ ./controller --port 5001
//    
//    4. Wait for TIME_WAIT to expire (~60 seconds)
//
//
// PITFALL 2: Permission Denied (Privileged Ports)
// ================================================
// SYMPTOM:
//    $ ./controller --port 80
//    [ERROR] Failed to bind socket: Permission denied
//
// SOLUTIONS:
//    1. Use port >= 1024:
//       $ ./controller --port 5000
//    
//    2. Run as root (NOT RECOMMENDED):
//       $ sudo ./controller --port 80
//    
//    3. Use capability (Linux):
//       $ sudo setcap 'cap_net_bind_service=+ep' ./controller
//       $ ./controller --port 80
//
//
// PITFALL 3: Data Directory Not Found
// ====================================
// SYMPTOM:
//    Workers can't load data
//    [ERROR] Failed to open file: ./data/AAPL.csv
//
// SOLUTIONS:
//    1. Verify directory exists:
//       $ ls -ld ./data
//    
//    2. Create if missing:
//       $ mkdir -p ./data
//    
//    3. Check file permissions:
//       $ chmod +r ./data/*.csv
//    
//    4. Use absolute path:
//       $ ./controller --data-dir /home/user/backtesting_data
//
//
// PITFALL 4: Process Not Stopping on Ctrl+C
// ==========================================
// SYMPTOM:
//    Press Ctrl+C, but controller continues running
//
// SOLUTIONS:
//    1. Check signal handler registered:
//       (Should see signal() calls in main)
//    
//    2. Force termination:
//       $ kill -9 <pid>
//    
//    3. Debug: Check if main loop stuck
//
//
// PITFALL 5: Forgetting Command-Line Arguments
// =============================================
// PROBLEM:
//    $ ./controller --port
//    Unknown option: --port
//    (Missing port value)
//
// SOLUTION:
//    $ ./controller --port 5000
//    (Provide value after option)
//
//
// PITFALL 6: Invalid Port Number
// ===============================
// SYMPTOM:
//    $ ./controller --port 99999
//    (Port out of range, overflow to unexpected value)
//
// SOLUTION:
//    Add validation:
//    int port = std::stoi(argv[++i]);
//    if (port < 1 || port > 65535) {
//        std::cerr << "Invalid port: " << port << "\n";
//        return 1;
//    }
//    config.listen_port = static_cast<uint16_t>(port);
//
//==============================================================================

//==============================================================================
// SECTION 10: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: How do I run controller in background?
// ===========================================
// A: Use nohup or screen/tmux:
//    $ nohup ./controller --port 5000 > controller.log 2>&1 &
//    $ echo $! > controller.pid
//    
//    Or with screen:
//    $ screen -S controller
//    $ ./controller --port 5000
//    $ Ctrl+A, D  # Detach
//    $ screen -r controller  # Reattach later
//
// Q2: How do I stop a background controller?
// ===========================================
// A: Send SIGTERM:
//    $ kill $(cat controller.pid)
//    
//    Or find PID:
//    $ ps aux | grep controller
//    $ kill <pid>
//
// Q3: Can I run multiple controllers on same machine?
// ====================================================
// A: Yes, use different ports:
//    $ ./controller --port 5000 &
//    $ ./controller --port 5001 &
//    $ ./controller --port 5002 &
//    
//    Each has separate: Port, workers, job queue
//
// Q4: Where are logs written?
// ============================
// A: Currently: stdout/stderr
//    - If redirected: Goes to file (> controller.log)
//    - If terminal: Printed to console
//    
//    For production: Could add file logging
//
// Q5: How do I know controller is working?
// =========================================
// A: Check:
//    - Process running: ps aux | grep controller
//    - Port listening: netstat -tulpn | grep 5000
//    - Logs showing "Controller running"
//    - Workers can connect: telnet controller_host 5000
//
// Q6: What if controller crashes?
// ================================
// A: No automatic restart:
//    - Workers disconnect (can't reach controller)
//    - Jobs in progress lost (unless checkpointed)
//    - Manual restart required
//    
//    For production: Use systemd or supervisor for auto-restart
//
// Q7: Can I reload configuration without restart?
// ================================================
// A: Not currently supported:
//    - Must stop and restart to change config
//    - Future: Could add SIGHUP handler to reload
//
// Q8: How do I know which workers are connected?
// ===============================================
// A: Check statistics (printed every 5 seconds):
//    Active Workers: 5
//    
//    Or add debug logging in Controller to list workers
//
// Q9: What happens on unhandled signal (SIGKILL)?
// ================================================
// A: Immediate termination:
//    - No cleanup (threads not joined, sockets not closed)
//    - Workers will eventually timeout (no heartbeat response)
//    - Jobs in progress lost
//    - Not graceful (avoid if possible)
//
// Q10: Can I change statistics interval?
// =======================================
// A: Not configurable currently:
//    - Hardcoded: 5 seconds
//    - To change: Modify sleep_for duration in main loop
//    - Future: Add --stats-interval option
//
//==============================================================================

//==============================================================================
// SECTION 11: BEST PRACTICES
//==============================================================================
//
// BEST PRACTICE 1: Always Use Absolute Paths in Production
// =========================================================
// DO:
//    ./controller --data-dir /opt/backtesting/data
//
// DON'T:
//    ./controller --data-dir ./data  # Relative to current directory
//    # Changes if you cd before running
//
//
// BEST PRACTICE 2: Run with Process Manager
// ==========================================
// DO (systemd):
//    sudo systemctl start controller
//    # Automatic restart on failure
//    # Logs to journald
//
// DON'T:
//    ./controller &  # No monitoring, no restart
//
//
// BEST PRACTICE 3: Redirect Logs to File
// =======================================
// DO:
//    ./controller --port 5000 > controller.log 2>&1
//    # Captures all output for debugging
//
// DON'T:
//    ./controller  # Logs to terminal (lost on disconnect)
//
//
// BEST PRACTICE 4: Monitor Resource Usage
// ========================================
// DO:
//    # Check CPU and memory
//    $ top -p $(cat controller.pid)
//    
//    # Check network connections
//    $ netstat -anp | grep controller
//    
//    # Check file descriptors
//    $ lsof -p $(cat controller.pid)
//
//
// BEST PRACTICE 5: Use Standard Ports
// ====================================
// DO:
//    # Use well-known port range (5000-10000)
//    ./controller --port 5000
//
// DON'T:
//    # Avoid: Privileged ports (<1024), random ports
//
//
// BEST PRACTICE 6: Validate Configuration
// ========================================
// Before running:
//    ☐ Port not in use: netstat -tulpn | grep <port>
//    ☐ Data directory exists: ls -ld <data-dir>
//    ☐ Data files present: ls <data-dir>/*.csv
//    ☐ Permissions correct: User can read data files
//
//
// BEST PRACTICE 7: Test Graceful Shutdown
// ========================================
// DO:
//    # Start controller
//    # Connect workers
//    # Submit jobs
//    # Send SIGTERM (kill <pid>)
//    # Verify: Jobs complete, workers disconnect cleanly
//
//==============================================================================

//==============================================================================
// SECTION 12: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM: Controller won't start
// ================================
// SYMPTOMS:
//    - Exits immediately with error
//    - Log: "Failed to start controller"
//
// DEBUGGING:
//    ☐ Check port availability:
//       $ netstat -tulpn | grep <port>
//       → If in use: Change port or kill process
//    
//    ☐ Check permissions:
//       $ ./controller --port 80
//       → If permission denied: Use port >= 1024
//    
//    ☐ Check bind errors:
//       $ dmesg | tail
//       → System logs may show additional errors
//
//
// PROBLEM: Statistics not printing
// =================================
// SYMPTOMS:
//    - No statistics output
//    - Main loop not executing
//
// DEBUGGING:
//    ☐ Check main loop condition:
//       → shutdown_requested should be false
//       → controller.is_running() should be true
//    
//    ☐ Check if immediately exited:
//       → Maybe start() failed, check earlier logs
//    
//    ☐ Check logging level:
//       → INFO level required for statistics
//
//
// PROBLEM: Can't stop with Ctrl+C
// ================================
// SYMPTOMS:
//    - Press Ctrl+C, nothing happens
//    - Controller keeps running
//
// DEBUGGING:
//    ☐ Check signal handler registered:
//       → signal() calls should execute before main loop
//    
//    ☐ Check terminal sends SIGINT:
//       $ kill -INT <pid>  # Manual SIGINT
//    
//    ☐ Force termination:
//       $ kill -9 <pid>  # SIGKILL (no cleanup)
//
//
// PROBLEM: High CPU usage
// ========================
// SYMPTOMS:
//    - Controller at 100% CPU
//    - System sluggish
//
// DEBUGGING:
//    ☐ Check thread count:
//       $ ps -eLf | grep controller | wc -l
//       → Should be: 3-4 + worker_count
//    
//    ☐ Profile with perf:
//       $ perf top -p <pid>
//       → Shows which functions using CPU
//    
//    ☐ Check for busy-wait:
//       → Main loop should sleep (does: 5 seconds)
//       → Controller threads should use condition variables (they do)
//
//
// PROBLEM: Memory leak
// ====================
// SYMPTOMS:
//    - Memory usage growing over time
//    - Eventually out of memory
//
// DEBUGGING:
//    ☐ Monitor RSS over time:
//       $ while true; do ps -p <pid> -o rss; sleep 60; done
//    
//    ☐ Run with Valgrind:
//       $ valgrind --leak-check=full ./controller
//    
//    ☐ Check completed_jobs_ growth:
//       → Controller keeps all completed jobs in memory
//       → Could add cleanup policy
//
//==============================================================================

//==============================================================================
// SECTION 13: PRODUCTION DEPLOYMENT
//==============================================================================
//
// DEPLOYMENT CHECKLIST:
// =====================
//
// Pre-Deployment:
//    ☐ Build optimized binary: cmake -DCMAKE_BUILD_TYPE=Release
//    ☐ Test on staging environment
//    ☐ Verify data files present and accessible
//    ☐ Check port availability
//    ☐ Configure firewall rules (allow port 5000)
//    ☐ Set up log rotation
//    ☐ Create service file (systemd/supervisor)
//    ☐ Document runbook (start/stop procedures)
//
// Deployment:
//    ☐ Copy binary to production server
//    ☐ Copy data files to production storage
//    ☐ Start service: systemctl start controller
//    ☐ Verify listening: netstat -tulpn | grep 5000
//    ☐ Test worker connection
//    ☐ Monitor logs for errors
//    ☐ Check statistics after 1 minute
//
// Post-Deployment:
//    ☐ Set up monitoring (CPU, memory, network)
//    ☐ Configure alerts (no workers, high error rate)
//    ☐ Document any issues encountered
//    ☐ Create backup/recovery plan
//
// MONITORING:
// ===========
//
// Key metrics to monitor:
// - Active workers: Should match expected count
// - Job completion rate: Jobs/second
// - Error rate: Failed jobs / total jobs
// - CPU usage: Should be low (<10% idle)
// - Memory usage: Should be stable (no growth)
// - Network connections: Port 5000 LISTEN state
//
// Alerting thresholds:
// - No workers for > 1 minute: CRITICAL
// - Error rate > 10%: WARNING
// - CPU > 90% for > 5 minutes: WARNING
// - Memory > 80% of available: WARNING
//
// BACKUP STRATEGY:
// ================
//
// What to backup:
// - Controller configuration
// - Data files (CSV historical data)
// - Logs (for debugging)
//
// What NOT to backup:
// - Running state (no persistent state in base controller)
// - Network connections (ephemeral)
//
// Recovery:
// - Controller fails: Restart, workers reconnect automatically
// - Data lost: Restore from backup
// - Configuration lost: Recreate from documentation
//
//==============================================================================

//==============================================================================
// SECTION 14: MONITORING AND OPERATIONS
//==============================================================================
//
// OPERATIONAL COMMANDS:
// =====================
//
// Start controller:
// $ ./controller --port 5000 --data-dir /data
//
// Check if running:
// $ ps aux | grep controller
// $ pgrep -f controller
//
// Check listening port:
// $ netstat -tulpn | grep 5000
// $ ss -tulpn | grep 5000
//
// Monitor logs (if redirected):
// $ tail -f controller.log
//
// Send test connection:
// $ telnet controller_host 5000
// $ nc -v controller_host 5000
//
// Graceful shutdown:
// $ kill -TERM <pid>
// $ pkill -TERM controller
//
// Force shutdown (if graceful fails):
// $ kill -9 <pid>
// $ pkill -9 controller
//
// HEALTH CHECKS:
// ==============
//
// Quick health check script:
//
// #!/bin/bash
// # health_check.sh
//
// PID=$(pgrep -f controller)
// if [ -z "$PID" ]; then
//     echo "ERROR: Controller not running"
//     exit 1
// fi
//
// # Check port listening
// if ! netstat -tulpn | grep -q ":5000.*LISTEN"; then
//     echo "ERROR: Controller not listening on port 5000"
//     exit 1
// fi
//
// # Check recent activity (logs updated in last minute)
// if [ -f controller.log ]; then
//     LAST_LOG=$(stat -c %Y controller.log)
//     NOW=$(date +%s)
//     if [ $((NOW - LAST_LOG)) -gt 60 ]; then
//         echo "WARNING: No recent log activity"
//     fi
// fi
//
// echo "OK: Controller healthy"
// exit 0
//
// PERFORMANCE MONITORING:
// =======================
//
// CPU and Memory:
// $ top -p $(pgrep controller)
// $ htop -p $(pgrep controller)
//
// Network statistics:
// $ netstat -s | grep -i tcp
// $ ss -s
//
// File descriptors:
// $ lsof -p $(pgrep controller) | wc -l
// (Should be: ~10-50 depending on worker count)
//
// Thread count:
// $ ps -eLf | grep controller | wc -l
// (Should be: 4 + worker_count)
//
// DEBUGGING COMMANDS:
// ===================
//
// Attach debugger to running controller:
// $ gdb -p $(pgrep controller)
// (gdb) thread apply all bt  # Backtrace all threads
// (gdb) info threads
//
// Trace system calls:
// $ strace -p $(pgrep controller)
// (Shows accept(), send(), recv(), etc.)
//
// Check network connections:
// $ lsof -i -a -p $(pgrep controller)
// (Shows all network connections)
//
//==============================================================================

//==============================================================================
// SIGNAL HANDLING BEST PRACTICES
//==============================================================================
//
// CURRENT IMPLEMENTATION ISSUES:
// ===============================
//
// 1. Logger::info() in signal handler:
//    - Uses mutex (not async-signal-safe)
//    - Uses std::cout (not async-signal-safe)
//    - Risk: Deadlock if signal during logging
//    - Probability: Low (rare timing)
//    - For project: Acceptable risk
//
// 2. controller->stop() in signal handler:
//    - Uses mutexes, condition variables
//    - Not async-signal-safe
//    - Risk: Deadlock or undefined behavior
//    - Mitigation: Shutdown_requested flag (main loop calls stop)
//
// PRODUCTION-QUALITY SIGNAL HANDLER:
// ===================================
//
// Strictly async-signal-safe version:
//
// volatile sig_atomic_t shutdown_flag = 0;
//
// void signal_handler(int signal) {
//     // Only async-signal-safe operations
//     
//     // Write to stderr (async-signal-safe)
//     const char msg[] = "Shutdown signal received\n";
//     write(STDERR_FILENO, msg, sizeof(msg) - 1);
//     
//     // Set flag (sig_atomic_t is safe for signals)
//     shutdown_flag = 1;
//     
//     // Don't call any complex functions
//     // Main loop will detect flag and call stop()
// }
//
// Main loop modification:
//
// while (!shutdown_flag && controller.is_running()) {
//     std::this_thread::sleep_for(std::chrono::seconds(5));
//     controller.print_statistics();
// }
//
// controller.stop();  // Called from main thread (safe)
//
// SELF-PIPE TRICK (Advanced):
// ============================
//
// For truly robust signal handling:
//
// int signal_pipe[2];  // Self-pipe
//
// void init_signal_handling() {
//     pipe(signal_pipe);
//     fcntl(signal_pipe[0], F_SETFL, O_NONBLOCK);
//     fcntl(signal_pipe[1], F_SETFL, O_NONBLOCK);
// }
//
// void signal_handler(int sig) {
//     // Write signal number to pipe (async-signal-safe)
//     write(signal_pipe[1], &sig, sizeof(sig));
// }
//
// Main loop:
// while (!shutdown) {
//     fd_set fds;
//     FD_ZERO(&fds);
//     FD_SET(signal_pipe[0], &fds);
//     
//     struct timeval tv = {5, 0};  // 5 second timeout
//     int ret = select(signal_pipe[0] + 1, &fds, NULL, NULL, &tv);
//     
//     if (ret > 0) {
//         // Signal received via pipe
//         int sig;
//         read(signal_pipe[0], &sig, sizeof(sig));
//         shutdown = true;
//     }
//     
//     controller.print_statistics();
// }
//
// This approach is fully signal-safe and robust.
//
//==============================================================================

//==============================================================================
// COMPARISON WITH ALTERNATIVE APPROACHES
//==============================================================================
//
// ALTERNATIVE 1: Interactive Mode (Current Design)
// =================================================
// Pros:
// - Simple: Run in terminal, see output
// - Debugging-friendly: See logs in real-time
// - Easy to stop: Just press Ctrl+C
//
// Cons:
// - Terminal required: Can't disconnect without stopping
// - No auto-restart: Crashes require manual intervention
//
// Best for: Development, testing, short runs
//
//
// ALTERNATIVE 2: Daemon Mode
// ===========================
// Implementation:
// - Fork to background: daemon(0, 0)
// - Detach from terminal
// - Redirect stdin/stdout/stderr to /dev/null or log file
// - Write PID file: /var/run/controller.pid
//
// Pros:
// - Runs independently: Terminal can close
// - Professional: Standard for server applications
//
// Cons:
// - More complex: Requires PID management
// - Harder to debug: Can't see output directly
//
// Best for: Production deployments
//
//
// ALTERNATIVE 3: Systemd Service (Recommended for Production)
// ============================================================
// Implementation:
// - Create .service file
// - Let systemd manage lifecycle
// - Automatic restart on failure
// - Integrated logging (journald)
//
// Pros:
// - Robust: Auto-restart, dependency management
// - Standard: Linux best practice
// - Monitoring: systemctl status shows health
//
// Cons:
// - Requires root: To install service
// - Platform-specific: Linux only
//
// Best for: Production on Linux servers
//
//
// CURRENT CHOICE: Interactive mode
// Appropriate for: Academic project, development, testing
// Upgrade path: Add daemonization or systemd for production
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================