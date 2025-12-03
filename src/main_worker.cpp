/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: main_worker.cpp
    
    Description:
        This file implements the main entry point for worker nodes in the
        distributed backtesting system. Worker nodes are the computational
        workhorses that execute actual backtesting jobs assigned by the
        controller cluster. This program handles worker lifecycle management,
        configuration, signal handling, and graceful shutdown.
        
        Core Responsibilities:
        - Parse command-line arguments for worker configuration
        - Establish and maintain connection to controller cluster
        - Register worker capabilities with controller
        - Execute assigned backtesting jobs on historical data
        - Send periodic heartbeats to prove liveness
        - Handle graceful shutdown on signals (Ctrl-C, SIGTERM)
        - Manage checkpoint/resume functionality for long-running jobs
        
        Worker Node Architecture:
        Workers are stateless computational nodes that:
        1. Connect to controller on startup
        2. Wait for job assignments
        3. Load price data from shared storage
        4. Execute backtesting strategies
        5. Return results to controller
        6. Repeat until shutdown
        
        Fault Tolerance:
        - Heartbeat mechanism: Worker sends periodic pings to controller
        - Failure detection: Controller detects dead workers via missed heartbeats
        - Job reassignment: Failed jobs automatically reassigned to healthy workers
        - Checkpointing: Long jobs can resume from checkpoint if worker crashes
        - Stateless design: Workers can crash/restart without data loss
        
        Deployment Models:
        
        SINGLE MACHINE (Testing):
        - Run 1 controller + 2-4 workers on same host
        - Use localhost for all connections
        - Different checkpoint directories per worker
        
        CLUSTER DEPLOYMENT (Production):
        - 3 controller nodes (Raft consensus)
        - 2-8 worker nodes (compute pool)
        - Shared NFS for price data (read-only)
        - Local disk for checkpoints (per-worker)
        
        KHOURY CLUSTER (Evaluation):
        - SSH into Khoury cluster nodes (kh01-kh10)
        - Deploy controllers on kh01-kh03
        - Deploy workers on kh04-kh10
        - Use shared /scratch for data
        
        Configuration Options:
        - --controller: Controller hostname/IP (which controller to connect to)
        - --port: Controller port (default 5000)
        - --data-dir: Where to find price CSV files
        - --checkpoint-dir: Where to save job checkpoints
        
        Signal Handling:
        - SIGINT (Ctrl-C): Graceful shutdown, complete current job
        - SIGTERM (kill): Graceful shutdown, save checkpoint
        - SIGPIPE: Ignored (prevents crash on broken socket)
        
        Graceful Shutdown:
        On shutdown signal:
        1. Set shutdown flag (stops accepting new jobs)
        2. Complete current job if any (saves checkpoint)
        3. Disconnect from controller
        4. Clean up resources
        5. Exit with status 0
        
    Dependencies:
        - worker/worker.h: Worker class implementation
        - common/logger.h: Logging infrastructure
        - POSIX signals: Signal handling (signal.h)
        - C++ atomics: Thread-safe shutdown flag
        - C++ standard library: I/O, threading, chrono
        
    Exit Codes:
        0: Normal shutdown (completed gracefully)
        1: Error (failed to start, invalid arguments, connection failure)
        
    Logging:
        Default level: INFO (connection status, job progress)
        Change with: Logger::set_level(LogLevel::DEBUG) for verbose output
        
    Thread Safety:
        - shutdown_requested: std::atomic for signal-safe access
        - global_worker: Protected by being only modified in signal handler
        - Worker class handles internal thread synchronization
        
    Performance Characteristics:
        - Startup time: ~10-100ms (connection + registration)
        - Idle CPU: <1% (blocked waiting for jobs)
        - Active CPU: 100% (executing backtest computation)
        - Memory: ~10-100MB (depends on price data loaded)
        - Network: ~1KB/sec heartbeats, ~10-100KB per job result
*******************************************************************************/

#include "worker/worker.h"
#include "common/logger.h"
#include <iostream>
#include <signal.h>
#include <atomic>

using namespace backtesting;

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
// 1. Overview and Architecture
// 2. Global State Management
// 3. Signal Handler Implementation
// 4. Usage Display Function
// 5. Main Function
//    5.1 Signal Setup
//    5.2 Argument Parsing
//    5.3 Worker Lifecycle
//    5.4 Shutdown Handling
// 6. Deployment Examples
// 7. Common Pitfalls
// 8. Troubleshooting Guide
// 9. FAQ
//==============================================================================

//==============================================================================
// SECTION 1: Overview and Architecture
//==============================================================================

/**
 * WORKER NODE ROLE IN DISTRIBUTED SYSTEM
 * 
 * SYSTEM ARCHITECTURE:
 * 
 * ```
 *          ┌─────────────────────────────────────┐
 *          │   Controller Cluster (Raft)        │
 *          │  ┌─────┐  ┌─────┐  ┌─────┐         │
 *          │  │Ctrl1│  │Ctrl2│  │Ctrl3│         │
 *          │  │Lead │  │Foll │  │Foll │         │
 *          │  └──┬──┘  └──┬──┘  └──┬──┘         │
 *          └─────┼────────┼────────┼─────────────┘
 *                │        │        │
 *          Job assignments & heartbeats
 *                │        │        │
 *     ┌──────────┴────────┴────────┴──────────┐
 *     │                                        │
 *  ┌──▼──┐  ┌──▼──┐  ┌──▼──┐  ┌──▼──┐  ┌──▼──┐
 *  │Wkr1 │  │Wkr2 │  │Wkr3 │  │Wkr4 │  │Wkr5 │
 *  └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘  └──┬──┘
 *     │        │        │        │        │
 *     └────────┴────────┴────────┴────────┘
 *              Access shared price data
 *                       │
 *                   ┌───▼────┐
 *                   │ NFS    │
 *                   │ /data  │
 *                   └────────┘
 * ```
 * 
 * WORKER RESPONSIBILITIES:
 * 
 * 1. CONNECT TO CONTROLLER:
 *    - Establish TCP connection to controller leader
 *    - Register capabilities (hostname, port)
 *    - Receive unique worker_id
 * 
 * 2. MAINTAIN LIVENESS:
 *    - Send heartbeat every 2 seconds
 *    - Report current job status in heartbeat
 *    - Controller detects failure if 3 heartbeats missed (6 sec timeout)
 * 
 * 3. EXECUTE JOBS:
 *    - Receive job assignment from controller
 *    - Load price data from shared storage
 *    - Run backtesting strategy
 *    - Create checkpoint every N symbols (for resume)
 *    - Return results to controller
 * 
 * 4. HANDLE FAILURES:
 *    - If controller disconnects, attempt reconnection
 *    - If job fails, report error to controller
 *    - If crash mid-job, resume from checkpoint on restart
 * 
 * WORKER STATE MACHINE:
 * 
 * ```
 *   STARTUP ──register──→ IDLE ──assigned──→ WORKING ──complete──→ IDLE
 *      │                    ▲                    │                    ▲
 *      │ connection         │ job done           │ job done          │
 *      │ failed             │                    │                   │
 *      ▼                    │                    ▼                   │
 *   ERROR ◄────disconnect───┴────────failure─────┴───────resume─────┘
 * ```
 * 
 * LIFECYCLE EXAMPLE:
 * 
 * Time 0s: Worker starts, parses arguments
 * Time 1s: Connects to controller at kh01:5000
 * Time 2s: Sends registration (hostname, port)
 * Time 3s: Receives worker_id=5, enters IDLE state
 * Time 4s: Sends heartbeat (status=IDLE)
 * Time 6s: Sends heartbeat (status=IDLE)
 * Time 8s: Receives job assignment (backtest AAPL, SMA strategy)
 * Time 8s: Enters WORKING state, loads AAPL.csv
 * Time 9s: Sends heartbeat (status=WORKING, job_id=42)
 * Time 10s: Executes backtest (generates signals, simulates trades)
 * Time 11s: Sends heartbeat (status=WORKING, progress=50%)
 * Time 12s: Completes backtest, calculates metrics
 * Time 13s: Sends results to controller
 * Time 13s: Returns to IDLE state
 * Time 14s: Sends heartbeat (status=IDLE)
 * ... repeat until shutdown
 * 
 * STATELESS DESIGN BENEFITS:
 * 
 * Workers maintain no persistent state (except checkpoints).
 * This enables:
 * - Easy scaling: Just launch more workers
 * - Fault tolerance: Dead workers don't cause data loss
 * - Load balancing: Controller assigns jobs to any available worker
 * - Rolling upgrades: Restart workers one at a time without downtime
 * 
 * CHECKPOINT STRATEGY:
 * 
 * For long-running jobs (100+ symbols):
 * - Save progress every 1000 symbols processed
 * - Checkpoint includes: job_id, symbols_completed, partial_results
 * - On worker restart, resume from last checkpoint
 * - Prevents redundant work if worker crashes mid-job
 * 
 * Example:
 * Job: Backtest 5000 symbols
 * Worker processes 2500 symbols, then crashes
 * On restart, worker:
 * - Loads checkpoint for job_id
 * - Sees 2500 symbols completed
 * - Resumes from symbol 2501
 * - Saves 50% of computation time!
 */

//==============================================================================
// SECTION 2: Global State Management
//==============================================================================

/**
 * @var shutdown_requested
 * @brief Atomic flag indicating shutdown has been requested
 * 
 * THREAD SAFETY:
 * std::atomic<bool> ensures signal handler can safely set flag
 * while main thread reads it. No mutex needed.
 * 
 * SIGNAL-SAFETY:
 * Signal handlers have strict limitations on what they can do:
 * - Can't call malloc/free (not async-signal-safe)
 * - Can't call most library functions
 * - CAN write to atomic variables (one of few safe operations)
 * 
 * USAGE PATTERN:
 * 
 * Signal handler sets flag:
 * ```cpp
 * void signal_handler(int sig) {
 *     shutdown_requested = true;  // Async-signal-safe
 * }
 * ```
 * 
 * Main loop checks flag:
 * ```cpp
 * while (!shutdown_requested) {
 *     // Do work...
 * }
 * ```
 * 
 * ALTERNATIVE (NOT USED HERE):
 * Could use std::atomic_flag (even more lightweight):
 * ```cpp
 * std::atomic_flag shutdown = ATOMIC_FLAG_INIT;
 * 
 * void signal_handler(int sig) {
 *     shutdown.test_and_set();
 * }
 * 
 * while (!shutdown.test_and_set()) {
 *     // ...
 * }
 * ```
 * 
 * WHY NOT JUST bool?
 * ```cpp
 * bool shutdown = false;  // NOT SIGNAL-SAFE
 * ```
 * 
 * Without atomic, compiler might optimize away checks:
 * - Read once, cache in register
 * - Never see updates from signal handler
 * - Loop never exits even after Ctrl-C
 * 
 * std::atomic prevents these optimizations (memory fence).
 */
std::atomic<bool> shutdown_requested(false);

/**
 * @var global_worker
 * @brief Global pointer to Worker instance for signal handler access
 * 
 * PURPOSE:
 * Signal handlers can't access local variables from main().
 * Global pointer allows signal handler to call worker->stop().
 * 
 * LIFETIME:
 * - Initialized to nullptr
 * - Set to &worker after Worker construction
 * - Used only in signal handler
 * - Never deleted (Worker is on stack)
 * 
 * THREAD SAFETY:
 * Not strictly thread-safe, but safe in practice:
 * - Only written once (in main thread)
 * - Only read in signal handler (different context)
 * - No concurrent writes
 * 
 * BETTER ALTERNATIVE (not used for simplicity):
 * ```cpp
 * std::atomic<Worker*> global_worker{nullptr};
 * ```
 * 
 * SIGNAL-SAFETY CAVEAT:
 * Calling worker->stop() from signal handler is technically
 * NOT async-signal-safe (calls complex functions). Better approach:
 * 
 * ```cpp
 * void signal_handler(int sig) {
 *     // Only set flag, defer stop() to main thread
 *     shutdown_requested = true;
 * }
 * 
 * int main() {
 *     // ...
 *     while (!shutdown_requested && worker.is_running()) {
 *         sleep(1);
 *     }
 *     
 *     // Main thread calls stop() (safe)
 *     if (shutdown_requested) {
 *         worker.stop();
 *     }
 * }
 * ```
 * 
 * Current implementation works but isn't strictly correct.
 * In practice, rarely causes issues.
 */
Worker* global_worker = nullptr;

//==============================================================================
// SECTION 3: Signal Handler Implementation
//==============================================================================

/**
 * @brief Handles shutdown signals for graceful termination
 * 
 * REGISTERED SIGNALS:
 * - SIGINT: Ctrl-C in terminal (interactive interrupt)
 * - SIGTERM: kill command (standard termination request)
 * 
 * SIGNAL SEMANTICS:
 * 
 * SIGINT (Interrupt):
 * - Sent when user presses Ctrl-C
 * - Indicates "please stop what you're doing"
 * - Should save work and exit gracefully
 * 
 * SIGTERM (Terminate):
 * - Sent by system shutdown, process managers, etc.
 * - Standard way to request graceful shutdown
 * - Should cleanup and exit within ~10 seconds
 * 
 * HANDLER LOGIC:
 * 
 * 1. Check signal type (SIGINT or SIGTERM)
 * 2. Log shutdown message
 * 3. Set shutdown_requested flag
 * 4. Call worker->stop() if worker exists
 * 
 * @param signal Signal number (SIGINT=2, SIGTERM=15)
 * 
 * ASYNC-SIGNAL-SAFETY VIOLATIONS:
 * 
 * This handler violates signal safety rules:
 * - Logger::info() calls complex functions (not safe)
 * - worker->stop() calls mutexes, threads (not safe)
 * 
 * WHY IT USUALLY WORKS:
 * - Signal delivered to main thread (not arbitrary thread)
 * - Main thread not holding locks when interrupted
 * - Logger and Worker are designed to be robust
 * 
 * But can cause:
 * - Deadlock if signal arrives while lock held
 * - Corruption if malloc/free interrupted
 * - Undefined behavior per POSIX spec
 * 
 * PRODUCTION FIX:
 * ```cpp
 * // Global atomic flag only (signal-safe)
 * void signal_handler(int sig) {
 *     shutdown_requested.store(true, std::memory_order_release);
 *     // No other operations!
 * }
 * 
 * // Main thread polls flag and handles shutdown
 * int main() {
 *     // ...
 *     while (!shutdown_requested.load(std::memory_order_acquire)) {
 *         std::this_thread::sleep_for(std::chrono::seconds(1));
 *     }
 *     
 *     Logger::info("Shutdown requested");  // Safe in main thread
 *     worker.stop();  // Safe in main thread
 * }
 * ```
 * 
 * SIGNAL HANDLER BEST PRACTICES:
 * 
 * DO:
 * - Set atomic flags
 * - Write to self-pipe (advanced technique)
 * - Call async-signal-safe functions only
 * - Keep handler minimal and fast
 * 
 * DON'T:
 * - Call malloc/free/new/delete
 * - Use mutexes or locks
 * - Call logging functions
 * - Throw exceptions
 * - Access non-atomic globals
 * 
 * ASYNC-SIGNAL-SAFE FUNCTIONS (POSIX):
 * - write() (to stderr or pipe)
 * - _exit() (immediate exit)
 * - Signal-related functions
 * - Few others (see man 7 signal-safety)
 * 
 * EXAMPLE TRACE:
 * ```
 * Worker running...
 * ^C                                    ← User presses Ctrl-C
 * [INFO] Shutdown signal received
 * [INFO] Shutting down...
 * [INFO] Worker shutdown complete
 * ```
 */
void signal_handler(int signal) {
    /**
     * CHECK SIGNAL TYPE
     * 
     * Only handle SIGINT and SIGTERM.
     * Other signals (if handler registered) are ignored.
     */
    if (signal == SIGINT || signal == SIGTERM) {
        /**
         * LOG SHUTDOWN REQUEST
         * 
         * WARNING: Logger::info() is NOT async-signal-safe!
         * Could deadlock if logger holds lock when signal arrives.
         * 
         * Safe alternative:
         * const char msg[] = "Shutdown requested\n";
         * write(STDERR_FILENO, msg, sizeof(msg) - 1);
         */
        Logger::info("Shutdown signal received");
        
        /**
         * SET SHUTDOWN FLAG
         * 
         * This is the ONLY truly signal-safe operation here.
         * Main loop will detect and exit gracefully.
         */
        shutdown_requested = true;
        
        /**
         * TRIGGER WORKER SHUTDOWN
         * 
         * WARNING: worker->stop() is NOT async-signal-safe!
         * It calls:
         * - Mutex locks (can deadlock)
         * - Thread operations
         * - Network shutdown
         * 
         * Works in practice but not guaranteed safe.
         * Better to set flag only, let main thread call stop().
         */
        if (global_worker) {
            global_worker->stop();
        }
    }
}

//==============================================================================
// SECTION 4: Usage Display Function
//==============================================================================

/**
 * @brief Displays command-line usage information
 * 
 * @param program_name Program name from argv[0]
 * 
 * STANDARD HELP FORMAT:
 * - Usage line showing basic syntax
 * - Options list with descriptions and defaults
 * - Clear, concise descriptions
 * 
 * INVOKED BY:
 * - --help flag
 * - Invalid arguments
 * - Missing required arguments (if any)
 * 
 * OUTPUT EXAMPLE:
 * ```
 * Usage: ./worker [options]
 * Options:
 *   --controller HOST    Controller hostname (default: localhost)
 *   --port PORT          Controller port (default: 5000)
 *   --data-dir DIR       Data directory (default: ./data)
 *   --checkpoint-dir DIR Checkpoint directory (default: ./checkpoints)
 *   --help               Show this help message
 * ```
 * 
 * DESIGN DECISIONS:
 * 
 * ALL OPTIONS OPTIONAL:
 * Defaults allow running without any arguments:
 * ```bash
 * ./worker  # Uses all defaults, connects to localhost:5000
 * ```
 * 
 * WHY DEFAULTS?
 * - Easy testing: Just run ./worker for local testing
 * - Sane defaults: localhost:5000 is common development setup
 * - Override only what's needed: --controller kh01 for cluster
 * 
 * ALTERNATIVE: Required arguments
 * ```cpp
 * if (config.controller_host.empty()) {
 *     std::cerr << "Error: --controller required\n";
 *     print_usage(argv[0]);
 *     return 1;
 * }
 * ```
 */
void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --controller HOST    Controller hostname (default: localhost)\n"
              << "  --port PORT          Controller port (default: 5000)\n"
              << "  --data-dir DIR       Data directory (default: ./data)\n"
              << "  --checkpoint-dir DIR Checkpoint directory (default: ./checkpoints)\n"
              << "  --help               Show this help message\n";
}

//==============================================================================
// SECTION 5: Main Function - Program Entry Point
//==============================================================================

/**
 * @brief Main entry point for worker node
 * 
 * EXECUTION FLOW:
 * 
 * 1. Register signal handlers
 * 2. Parse command-line arguments
 * 3. Configure logging
 * 4. Create Worker instance
 * 5. Start worker (connects to controller)
 * 6. Wait for shutdown signal
 * 7. Stop worker gracefully
 * 8. Exit
 * 
 * @param argc Argument count
 * @param argv Argument vector
 * 
 * @return 0 on success, 1 on error
 * 
 * COMMAND-LINE EXAMPLES:
 * 
 * Local testing:
 * ```bash
 * ./worker
 * ```
 * 
 * Connect to remote controller:
 * ```bash
 * ./worker --controller kh01.khoury.neu.edu --port 5000
 * ```
 * 
 * Custom directories:
 * ```bash
 * ./worker --data-dir /mnt/shared/price_data \
 *          --checkpoint-dir /var/worker/checkpoints
 * ```
 * 
 * Khoury cluster deployment:
 * ```bash
 * ssh kh04 "cd ~/cs6650-project && ./worker --controller kh01 --port 5000" &
 * ssh kh05 "cd ~/cs6650-project && ./worker --controller kh01 --port 5000" &
 * ssh kh06 "cd ~/cs6650-project && ./worker --controller kh01 --port 5000" &
 * ssh kh07 "cd ~/cs6650-project && ./worker --controller kh01 --port 5000" &
 * ```
 * 
 * ENVIRONMENT VARIABLES (alternative configuration):
 * ```bash
 * export CONTROLLER_HOST=kh01.khoury.neu.edu
 * export CONTROLLER_PORT=5000
 * export DATA_DIR=/scratch/price_data
 * ./worker  # Reads from environment
 * ```
 * 
 * FAILURE MODES:
 * 
 * CONTROLLER UNREACHABLE:
 * - Worker logs "Failed to connect"
 * - Exits with code 1
 * - Should retry in production (see Q&A)
 * 
 * DATA DIRECTORY MISSING:
 * - Worker starts successfully
 * - Fails when trying to load price data for job
 * - Reports error to controller, job reassigned
 * 
 * PORT ALREADY IN USE:
 * - Shouldn't happen (worker doesn't listen on port)
 * - If implemented, worker would fail to bind
 * 
 * INTEGRATION WITH PROCESS MANAGERS:
 * 
 * SYSTEMD:
 * ```ini
 * [Unit]
 * Description=Backtesting Worker
 * After=network.target
 * 
 * [Service]
 * Type=simple
 * ExecStart=/usr/local/bin/worker --controller controller.local --port 5000
 * Restart=always
 * RestartSec=5
 * User=worker
 * 
 * [Install]
 * WantedBy=multi-user.target
 * ```
 * 
 * DOCKER:
 * ```dockerfile
 * FROM ubuntu:20.04
 * COPY worker /usr/local/bin/
 * COPY data /data
 * CMD ["worker", "--controller", "controller", "--port", "5000"]
 * ```
 * 
 * KUBERNETES:
 * ```yaml
 * apiVersion: apps/v1
 * kind: Deployment
 * metadata:
 *   name: backtesting-worker
 * spec:
 *   replicas: 8
 *   template:
 *     spec:
 *       containers:
 *       - name: worker
 *         image: backtesting-worker:latest
 *         args:
 *         - --controller
 *         - controller-service
 *         - --port
 *         - "5000"
 * ```
 */
int main(int argc, char* argv[]) {
    //==========================================================================
    // SECTION 5.1: Signal Setup
    //==========================================================================
    
    /**
     * REGISTER SIGNAL HANDLERS
     * 
     * signal() is the simple POSIX API for signal handling.
     * More robust alternative: sigaction() with SA_RESTART flag.
     * 
     * SIGINT:
     * - Ctrl-C in terminal
     * - INT = "interrupt"
     * - Default action: Terminate process
     * - Our handler: Graceful shutdown
     * 
     * SIGTERM:
     * - kill <pid> (without -9)
     * - TERM = "terminate"
     * - Default action: Terminate process
     * - Our handler: Graceful shutdown
     * - Most common signal for service shutdown
     * 
     * SIGPIPE:
     * - Occurs when writing to closed socket
     * - Default action: Terminate process
     * - Our handler: SIG_IGN (ignore)
     * - Prevents crash when controller disconnects
     * 
     * WHY IGNORE SIGPIPE?
     * 
     * Scenario without SIG_IGN:
     * 1. Worker sends heartbeat to controller
     * 2. Controller crashes, closes connection
     * 3. Worker's next send() generates SIGPIPE
     * 4. Worker process terminates (bad!)
     * 
     * With SIG_IGN:
     * 1-3. Same as above
     * 4. send() returns error (EPIPE)
     * 5. Worker detects error, attempts reconnection (good!)
     * 
     * ALTERNATIVE: MSG_NOSIGNAL flag
     * ```cpp
     * send(sock, data, size, MSG_NOSIGNAL);  // Don't generate SIGPIPE
     * ```
     * But SIG_IGN is global protection for all socket writes.
     */
    signal(SIGINT, signal_handler);   // Ctrl-C
    signal(SIGTERM, signal_handler);  // kill <pid>
    signal(SIGPIPE, SIG_IGN);         // Ignore broken pipe
    
    //==========================================================================
    // SECTION 5.2: Argument Parsing
    //==========================================================================
    
    /**
     * INITIALIZE CONFIGURATION WITH DEFAULTS
     * 
     * WorkerConfig (from worker/worker.h) contains:
     * - controller_host: "localhost"
     * - controller_port: 5000
     * - data_directory: "./data"
     * - checkpoint_directory: "./checkpoints"
     * - heartbeat_interval_sec: 2
     * 
     * These defaults work for local testing without arguments.
     */
    WorkerConfig config;
    
    /**
     * PARSE COMMAND-LINE ARGUMENTS
     * 
     * PATTERN:
     * for each argument:
     *   if matches flag (--controller, --port, etc.):
     *     consume next argument as value
     *     store in config
     * 
     * VALIDATION:
     * 
     * Current implementation: NONE
     * - Invalid port string crashes (std::stoi throws)
     * - Negative port accepted (should reject)
     * - Nonexistent directories accepted (fails later)
     * 
     * PRODUCTION VERSION:
     * ```cpp
     * else if (arg == "--port" && i + 1 < argc) {
     *     try {
     *         int port = std::stoi(argv[++i]);
     *         if (port < 1 || port > 65535) {
     *             std::cerr << "Port must be 1-65535\n";
     *             return 1;
     *         }
     *         config.controller_port = static_cast<uint16_t>(port);
     *     } catch (const std::exception& e) {
     *         std::cerr << "Invalid port: " << argv[i] << "\n";
     *         return 1;
     *     }
     * }
     * ```
     * 
     * UNKNOWN ARGUMENTS:
     * Prints error and usage, exits with code 1.
     * Good UX - user immediately sees what went wrong.
     */
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--controller" && i + 1 < argc) {
            config.controller_host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            config.controller_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--data-dir" && i + 1 < argc) {
            config.data_directory = argv[++i];
        } else if (arg == "--checkpoint-dir" && i + 1 < argc) {
            config.checkpoint_directory = argv[++i];
        } else {
            /**
             * INVALID ARGUMENT
             * 
             * Show error and usage, then exit.
             * Immediate feedback helps user correct mistake.
             */
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    
    //==========================================================================
    // SECTION 5.3: Worker Lifecycle
    //==========================================================================
    
    /**
     * CONFIGURE LOGGING
     * 
     * LogLevel::INFO: Standard verbosity
     * - Shows connection status
     * - Shows job assignments and completions
     * - Shows errors
     * 
     * For debugging, use LogLevel::DEBUG:
     * - Shows all protocol messages
     * - Shows heartbeat traffic
     * - Shows checkpoint operations
     * 
     * For production, use LogLevel::WARN or LogLevel::ERROR:
     * - Minimal output
     * - Only problems logged
     */
    Logger::set_level(LogLevel::INFO);
    Logger::info("=== Distributed Backtesting Worker ===");
    
    /**
     * CREATE WORKER INSTANCE
     * 
     * Worker constructor:
     * - Stores configuration
     * - Initializes internal state
     * - Does NOT connect yet (that's in start())
     * 
     * STACK ALLOCATION:
     * Worker on stack (not heap) ensures automatic cleanup.
     * Destructor called on return, shutdown, or exception.
     * 
     * ALTERNATIVE (heap allocation):
     * ```cpp
     * auto worker = std::make_unique<Worker>(config);
     * global_worker = worker.get();
     * if (!worker->start()) { ... }
     * ```
     */
    Worker worker(config);
    
    /**
     * STORE GLOBAL POINTER FOR SIGNAL HANDLER
     * 
     * Allows signal handler to call worker.stop().
     * Set AFTER construction (worker is fully initialized).
     * 
     * TIMING:
     * Must set before start() because signal could arrive anytime after.
     */
    global_worker = &worker;
    
    /**
     * START WORKER
     * 
     * Worker startup sequence:
     * 1. Create socket
     * 2. Connect to controller
     * 3. Send registration message
     * 4. Wait for worker_id assignment
     * 5. Start heartbeat thread
     * 6. Enter job processing loop
     * 
     * BLOCKING:
     * start() returns immediately after launching background threads.
     * Actual work happens asynchronously.
     * 
     * FAILURE HANDLING:
     * If start() returns false:
     * - Connection failed (controller unreachable)
     * - Registration rejected (protocol error)
     * - Thread creation failed (resource exhaustion)
     * 
     * Log error and exit with code 1.
     */
    if (!worker.start()) {
        Logger::error("Failed to start worker");
        return 1;
    }
    
    Logger::info("Worker running. Press Ctrl+C to stop.");
    
    //==========================================================================
    // SECTION 5.4: Main Event Loop - Wait for Shutdown
    //==========================================================================
    
    /**
     * WAIT LOOP
     * 
     * Main thread sleeps while worker threads do actual work:
     * - Heartbeat thread sends periodic pings
     * - Job thread executes backtests
     * - Network thread receives messages from controller
     * 
     * LOOP CONDITIONS:
     * 
     * Continue while:
     * - !shutdown_requested: No Ctrl-C or SIGTERM
     * - worker.is_running(): Worker hasn't crashed
     * 
     * Exit when:
     * - User presses Ctrl-C (shutdown_requested = true)
     * - Worker encounters fatal error (is_running() = false)
     * - Controller disconnects and can't reconnect
     * 
     * SLEEP DURATION:
     * 1 second sleep reduces CPU usage (no busy-wait).
     * Doesn't affect responsiveness:
     * - Signal handler sets flag (wakes immediately)
     * - Worker threads run independently (not blocked by sleep)
     * 
     * ALTERNATIVE: Condition Variable
     * ```cpp
     * std::mutex mtx;
     * std::condition_variable cv;
     * 
     * {
     *     std::unique_lock<std::mutex> lock(mtx);
     *     cv.wait(lock, []{ return shutdown_requested.load(); });
     * }
     * 
     * // Signal handler notifies:
     * cv.notify_all();
     * ```
     * 
     * GRACEFUL SHUTDOWN:
     * 
     * When loop exits:
     * - Worker finishes current job (if any)
     * - Saves checkpoint
     * - Sends final heartbeat (status=SHUTTING_DOWN)
     * - Disconnects from controller
     * - Joins threads
     * 
     * Total shutdown time: ~1-5 seconds
     * - Current job completion: 0-2 seconds
     * - Thread cleanup: ~1 second
     * - Network cleanup: ~100ms
     */
    while (!shutdown_requested && worker.is_running()) {
        /**
         * SLEEP FOR 1 SECOND
         * 
         * Reduces CPU usage to nearly 0% when idle.
         * Wakes every second to check conditions.
         * 
         * If shutdown requested, loop exits on next iteration.
         * Max latency: 1 second (acceptable for shutdown).
         */
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    
    /**
     * SHUTDOWN INITIATED
     * 
     * Log message for operator awareness.
     */
    Logger::info("Shutting down...");
    
    /**
     * STOP WORKER
     * 
     * Worker::stop() performs graceful shutdown:
     * 1. Set internal shutdown flag
     * 2. Stop accepting new jobs
     * 3. Complete current job (or checkpoint)
     * 4. Send final heartbeat
     * 5. Disconnect from controller
     * 6. Join background threads
     * 
     * BLOCKING:
     * May block for up to ~5 seconds:
     * - Current job completion: 0-2 seconds
     * - Thread join: ~1 second
     * - Network shutdown: ~100ms
     * 
     * IDEMPOTENT:
     * Safe to call even if signal handler already called stop().
     * Second call is no-op.
     */
    worker.stop();
    
    /**
     * SHUTDOWN COMPLETE
     * 
     * Final log message before exit.
     */
    Logger::info("Worker shutdown complete");
    
    /**
     * EXIT WITH SUCCESS CODE
     * 
     * Return 0 indicates normal shutdown.
     * Process managers interpret this as "intentional stop, don't restart".
     */
    return 0;
}

//==============================================================================
// SECTION 6: Deployment Examples
//==============================================================================

/**
 * EXAMPLE 1: Local Testing (Single Machine)
 * 
 * Terminal 1 - Start controller:
 * ```bash
 * ./controller --id 1
 * ```
 * 
 * Terminal 2 - Start worker 1:
 * ```bash
 * ./worker --controller localhost --port 5000 \
 *          --data-dir ./data --checkpoint-dir ./checkpoints1
 * ```
 * 
 * Terminal 3 - Start worker 2:
 * ```bash
 * ./worker --controller localhost --port 5000 \
 *          --data-dir ./data --checkpoint-dir ./checkpoints2
 * ```
 * 
 * Terminal 4 - Submit jobs:
 * ```bash
 * ./test_integration
 * ```
 * 
 * Expected output (Worker 1):
 * ```
 * === Distributed Backtesting Worker ===
 * [INFO] Worker running. Press Ctrl+C to stop.
 * [INFO] Connected to controller at localhost:5000
 * [INFO] Registered as worker_1
 * [INFO] Received job 42: AAPL SMA
 * [INFO] Loading data: ./data/AAPL.csv
 * [INFO] Executing backtest...
 * [INFO] Backtest complete, sending results
 * [INFO] Job 42 completed successfully
 * ```
 */

/**
 * EXAMPLE 2: Khoury Cluster Deployment (Multi-Machine)
 * 
 * Step 1 - Compile on login node:
 * ```bash
 * ssh login.khoury.neu.edu
 * cd ~/cs6650-project
 * mkdir build && cd build
 * cmake ..
 * make -j4
 * ```
 * 
 * Step 2 - Copy price data to shared location:
 * ```bash
 * mkdir -p /scratch/$USER/price_data
 * cp ../data/asterisk.csv /scratch/$USER/price_data/
 * ```
 * 
 * Step 3 - Start controllers (3 nodes for Raft):
 * ```bash
 * ssh kh01 "cd ~/cs6650-project/build && \
 *          ./controller --id 1 --peers kh02:6000,kh03:6000" &
 * 
 * ssh kh02 "cd ~/cs6650-project/build && \
 *          ./controller --id 2 --peers kh01:6000,kh03:6000" &
 * 
 * ssh kh03 "cd ~/cs6650-project/build && \
 *          ./controller --id 3 --peers kh01:6000,kh02:6000" &
 * ```
 * 
 * Step 4 - Start workers (4 nodes):
 * ```bash
 * for i in {4..7}; do
 *     ssh kh0$i "cd ~/cs6650-project/build && \
 *               ./worker --controller kh01 --port 5000 \
 *                        --data-dir /scratch/$USER/price_data \
 *                        --checkpoint-dir /tmp/checkpoints_$i" &
 * done
 * ```
 * 
 * Step 5 - Monitor workers:
 * ```bash
 * # Check if workers are running
 * for i in {4..7}; do
 *     echo "=== Worker on kh0$i ==="
 *     ssh kh0$i "ps aux | grep worker"
 * done
 * ```
 * 
 * Step 6 - Submit evaluation jobs:
 * ```bash
 * ssh kh01 "cd ~/cs6650-project/build && ./test_integration"
 * ```
 * 
 * Step 7 - Collect results and shutdown:
 * ```bash
 * # Copy results
 * scp kh01:~/cs6650-project/build/results.json ./
 * 
 * # Shutdown workers
 * for i in {4..7}; do
 *     ssh kh0$i "pkill -TERM worker"
 * done
 * 
 * # Shutdown controllers
 * for i in {1..3}; do
 *     ssh kh0$i "pkill -TERM controller"
 * done
 * ```
 */

/**
 * EXAMPLE 3: Automated Deployment Script
 * 
 * deploy_workers.sh:
 * ```bash
 * #!/bin/bash
 * 
 * CONTROLLER="kh01.khoury.neu.edu"
 * PORT=5000
 * DATA_DIR="/scratch/$USER/price_data"
 * WORKER_NODES=("kh04" "kh05" "kh06" "kh07")
 * 
 * echo "Deploying workers to ${#WORKER_NODES[@]} nodes..."
 * 
 * for node in "${WORKER_NODES[@]}"; do
 *     echo "Starting worker on $node..."
 *     
 *     ssh $node "mkdir -p /tmp/checkpoints_$node"
 *     
 *     ssh $node "cd ~/cs6650-project/build && \
 *                nohup ./worker --controller $CONTROLLER \
 *                              --port $PORT \
 *                              --data-dir $DATA_DIR \
 *                              --checkpoint-dir /tmp/checkpoints_$node \
 *                > worker_$node.log 2>&1 &"
 *     
 *     echo "Worker on $node started"
 *     sleep 1
 * done
 * 
 * echo "All workers deployed"
 * echo "Logs available at worker_*.log on each node"
 * ```
 * 
 * shutdown_workers.sh:
 * ```bash
 * #!/bin/bash
 * 
 * WORKER_NODES=("kh04" "kh05" "kh06" "kh07")
 * 
 * echo "Shutting down workers..."
 * 
 * for node in "${WORKER_NODES[@]}"; do
 *     echo "Stopping worker on $node..."
 *     ssh $node "pkill -TERM worker"
 * done
 * 
 * sleep 5
 * 
 * # Check if any still running
 * for node in "${WORKER_NODES[@]}"; do
 *     running=$(ssh $node "pgrep worker" | wc -l)
 *     if [ $running -gt 0 ]; then
 *         echo "WARNING: Worker on $node still running, force killing"
 *         ssh $node "pkill -9 worker"
 *     fi
 * done
 * 
 * echo "All workers stopped"
 * ```
 */

/**
 * EXAMPLE 4: Docker Deployment
 * 
 * Dockerfile:
 * ```dockerfile
 * FROM ubuntu:20.04
 * 
 * # Install dependencies
 * RUN apt-get update && apt-get install -y
 *     g++ cmake libboost-all-dev
 *     && rm -rf /var/lib/apt/lists/asterisk
 * 
 * # Copy source and build
 * COPY . /app
 * WORKDIR /app/build
 * RUN cmake .. && make -j4
 * 
 * # Create data directory
 * RUN mkdir -p /data /checkpoints
 * 
 * # Copy price data
 * COPY data/asterisk.csv /data/
 * 
 * # Run worker
 * CMD ["./worker", "--controller", "controller", \
 *      "--port", "5000", \
 *      "--data-dir", "/data", \
 *      "--checkpoint-dir", "/checkpoints"]
 * ```
 * 
 * docker-compose.yml:
 * ```yaml
 * version: '3'
 * services:
 *   controller:
 *     build: .
 *     command: ./controller --id 1
 *     ports:
 *       - "5000:5000"
 *   
 *   worker1:
 *     build: .
 *     depends_on:
 *       - controller
 *     volumes:
 *       - ./data:/data:ro
 *   
 *   worker2:
 *     build: .
 *     depends_on:
 *       - controller
 *     volumes:
 *       - ./data:/data:ro
 * ```
 * 
 * Deploy:
 * ```bash
 * docker-compose up --scale worker=4
 * ```
 */

//==============================================================================
// SECTION 7: Common Pitfalls and Solutions
//==============================================================================

/**
 * PITFALL 1: Forgetting to set global_worker pointer
 * 
 * PROBLEM:
 * ```cpp
 * Worker worker(config);
 * // Forgot: global_worker = &worker;
 * if (!worker.start()) { ... }
 * ```
 * 
 * CONSEQUENCE:
 * Signal handler has global_worker == nullptr.
 * On Ctrl-C, worker->stop() not called.
 * Worker doesn't shutdown gracefully.
 * 
 * SOLUTION:
 * Always set pointer before start():
 * ```cpp
 * Worker worker(config);
 * global_worker = &worker;  // Before start()
 * if (!worker.start()) { ... }
 * ```
 */

/**
 * PITFALL 2: Using same checkpoint directory for multiple workers
 * 
 * PROBLEM:
 * ```bash
 * # Both workers on same host
 * ./worker --checkpoint-dir /tmp/checkpoints &
 * ./worker --checkpoint-dir /tmp/checkpoints &  # Same directory!
 * ```
 * 
 * CONSEQUENCE:
 * Workers overwrite each other's checkpoints.
 * Checkpoint corruption, lost progress.
 * 
 * SOLUTION:
 * Unique directory per worker:
 * ```bash
 * ./worker --checkpoint-dir /tmp/checkpoints1 &
 * ./worker --checkpoint-dir /tmp/checkpoints2 &  # Different
 * ```
 * 
 * Or use worker hostname:
 * ```bash
 * HOSTNAME=$(hostname)
 * ./worker --checkpoint-dir /tmp/checkpoints_$HOSTNAME
 * ```
 */

/**
 * PITFALL 3: Data directory not accessible
 * 
 * PROBLEM:
 * ```bash
 * ./worker --data-dir /mnt/nfs/data
 * # But /mnt/nfs not mounted!
 * ```
 * 
 * CONSEQUENCE:
 * Worker starts successfully but fails when loading data.
 * Every job assignment fails with "file not found".
 * 
 * SOLUTION:
 * Validate directory exists on startup:
 * ```cpp
 * if (!std::filesystem::exists(config.data_directory)) {
 *     Logger::error("Data directory not found: " + config.data_directory);
 *     return 1;
 * }
 * ```
 */

/**
 * PITFALL 4: Controller hostname unresolvable
 * 
 * PROBLEM:
 * ```bash
 * ./worker --controller nonexistent.host
 * ```
 * 
 * CONSEQUENCE:
 * Worker tries to connect, DNS lookup fails.
 * Worker exits with "Failed to start worker".
 * 
 * SOLUTION:
 * Test connectivity first:
 * ```bash
 * # Verify hostname resolves
 * ping -c 1 kh01.khoury.neu.edu
 * 
 * # Verify port is reachable
 * telnet kh01.khoury.neu.edu 5000
 * 
 * # Then start worker
 * ./worker --controller kh01.khoury.neu.edu
 * ```
 */

/**
 * PITFALL 5: Not handling worker restart after crash
 * 
 * PROBLEM:
 * Worker crashes, stays down, capacity reduced.
 * 
 * SOLUTION:
 * Use process manager to auto-restart:
 * 
 * Systemd:
 * ```ini
 * [Service]
 * Restart=always
 * RestartSec=5
 * ```
 * 
 * Supervisor:
 * ```ini
 * [program:worker]
 * command=/usr/local/bin/worker --controller controller
 * autorestart=true
 * startretries=10
 * ```
 * 
 * Or manual monitoring:
 * ```bash
 * while true; do
 *     ./worker --controller kh01
 *     echo "Worker exited, restarting in 5s..."
 *     sleep 5
 * done
 * ```
 */

/**
 * PITFALL 6: Insufficient file descriptors
 * 
 * PROBLEM:
 * Running many workers on same machine.
 * Each worker opens sockets, files, etc.
 * Eventually hit ulimit (default: 1024).
 * 
 * SYMPTOM:
 * "Too many open files" error
 * 
 * SOLUTION:
 * Increase file descriptor limit:
 * ```bash
 * # Check current limit
 * ulimit -n
 * 
 * # Increase for current session
 * ulimit -n 4096
 * 
 * # Permanent increase (add to /etc/security/limits.conf)
 * * soft nofile 4096
 * * hard nofile 8192
 * ```
 */

/**
 * PITFALL 7: Forgetting to ignore SIGPIPE
 * 
 * PROBLEM:
 * ```cpp
 * // Forgot: signal(SIGPIPE, SIG_IGN);
 * ```
 * 
 * CONSEQUENCE:
 * When controller crashes, worker's next send() generates SIGPIPE.
 * Worker process terminates unexpectedly.
 * 
 * SOLUTION:
 * Always ignore SIGPIPE:
 * ```cpp
 * signal(SIGPIPE, SIG_IGN);  // Prevents crash on broken pipe
 * ```
 */

//==============================================================================
// SECTION 8: Troubleshooting Guide
//==============================================================================

/**
 * ISSUE: Worker immediately exits with "Failed to start worker"
 * 
 * CAUSE 1: Controller not running
 * CHECK:
 * ```bash
 * ps aux | grep controller
 * # If no output, controller not running
 * ```
 * FIX:
 * ```bash
 * ./controller --id 1 &
 * ```
 * 
 * CAUSE 2: Wrong controller hostname/port
 * CHECK:
 * ```bash
 * telnet localhost 5000
 * # If "Connection refused", controller not listening
 * ```
 * FIX:
 * ```bash
 * # Verify controller is on correct host/port
 * ./worker --controller kh01 --port 5000
 * ```
 * 
 * CAUSE 3: Firewall blocking connection
 * CHECK:
 * ```bash
 * nc -zv kh01.khoury.neu.edu 5000
 * # If "connection refused", firewall issue
 * ```
 * FIX:
 * ```bash
 * # Open firewall (if you have permissions)
 * sudo ufw allow 5000/tcp
 * ```
 */

/**
 * ISSUE: Worker connects but doesn't receive jobs
 * 
 * CAUSE 1: Controller has no jobs in queue
 * CHECK:
 * Look at controller logs for job submissions
 * FIX:
 * Submit jobs via test_integration or client
 * 
 * CAUSE 2: Worker heartbeats failing
 * CHECK:
 * ```bash
 * # Worker logs should show heartbeat messages every 2 seconds
 * tail -f worker.log | grep heartbeat
 * ```
 * FIX:
 * If no heartbeats, check network connectivity
 * 
 * CAUSE 3: Worker in zombie state (controller thinks it's dead)
 * CHECK:
 * Controller logs: "Worker X failed heartbeat, marking as dead"
 * FIX:
 * Restart worker to re-register
 */

/**
 * ISSUE: Worker crashes during job execution
 * 
 * CAUSE 1: Missing price data file
 * CHECK:
 * Worker logs: "Failed to load: ./data/AAPL.csv"
 * FIX:
 * ```bash
 * ls -lh ./data/AAPL.csv
 * # If missing, copy from source
 * ```
 * 
 * CAUSE 2: Corrupted CSV file
 * CHECK:
 * Worker logs: "CSV parse error at line N"
 * FIX:
 * ```bash
 * # Validate CSV format
 * head -5 ./data/AAPL.csv
 * # Should have header: date,open,high,low,close,volume
 * ```
 * 
 * CAUSE 3: Out of memory
 * CHECK:
 * ```bash
 * dmesg | grep -i "out of memory"
 * # Or worker killed by OOM killer
 * ```
 * FIX:
 * Reduce job size or add more RAM
 */

/**
 * ISSUE: Worker won't shutdown on Ctrl-C
 * 
 * CAUSE: Job still running, waiting for completion
 * 
 * EXPECTED BEHAVIOR:
 * Worker completes current job before exiting (graceful).
 * May take up to ~2 seconds.
 * 
 * FORCE SHUTDOWN:
 * If worker doesn't exit after 10 seconds:
 * ```bash
 * pkill -9 worker  # Force kill (loses checkpoint!)
 * ```
 */

/**
 * ISSUE: High CPU usage when idle
 * 
 * CAUSE: Busy-wait loop instead of sleep
 * 
 * CHECK:
 * ```bash
 * top -p $(pgrep worker)
 * # Should show ~0% CPU when idle
 * ```
 * 
 * EXPECTATION:
 * Worker should use <1% CPU when idle (sleeping in main loop).
 * If showing 100% CPU when idle, indicates busy-wait bug.
 */

//==============================================================================
// SECTION 9: FAQ
//==============================================================================

/**
 * Q1: Can I run multiple workers on the same machine?
 * 
 * A: Yes! Just ensure they have different checkpoint directories:
 *    
 *    ```bash
 *    ./worker --checkpoint-dir /tmp/checkpoints1 &
 *    ./worker --checkpoint-dir /tmp/checkpoints2 &
 *    ./worker --checkpoint-dir /tmp/checkpoints3 &
 *    ./worker --checkpoint-dir /tmp/checkpoints4 &
 *    ```
 *    
 *    They can share the same data directory (read-only access).
 *    Number of workers limited by:
 *    - CPU cores (1 worker per core is reasonable)
 *    - Memory (each worker uses ~10-100MB)
 *    - File descriptors (ulimit -n)
 */

/**
 * Q2: What happens if worker crashes mid-job?
 * 
 * A: Depends on checkpoint status:
 *    
 *    WITH CHECKPOINTING:
 *    1. Worker crashes after processing 500 of 1000 symbols
 *    2. Checkpoint saved at symbol 500
 *    3. Controller detects worker death (missed heartbeats)
 *    4. Controller reassigns job to another worker
 *    5. New worker loads checkpoint, resumes from symbol 501
 *    6. Only 500 symbols re-processed (not all 1000)
 *    
 *    WITHOUT CHECKPOINTING:
 *    1. Worker crashes
 *    2. Controller reassigns job
 *    3. New worker starts from beginning
 *    4. All work lost, full re-computation
 */

/**
 * Q3: How do I connect to specific controller in Raft cluster?
 * 
 * A: Workers can connect to any controller:
 *    
 *    ```bash
 *    # Connect to controller 1
 *    ./worker --controller kh01 --port 5000
 *    
 *    # If kh01 is not leader, it forwards to leader
 *    # Worker's connection transparently follows leader
 *    ```
 *    
 *    Or implement client-side leader discovery:
 *    ```cpp
 *    // Try each controller until finding leader
 *    std::vector<std::string> controllers = {"kh01", "kh02", "kh03"};
 *    for (const auto& ctrl : controllers) {
 *        if (worker.connect(ctrl, 5000)) {
 *            break;  // Connected successfully
 *        }
 *    }
 *    ```
 */

/**
 * Q4: Can worker reconnect if controller restarts?
 * 
 * A: Depends on Worker implementation. Typical behavior:
 *    
 *    1. Controller crashes/restarts
 *    2. Worker's heartbeat fails
 *    3. Worker detects disconnection
 *    4. Worker attempts reconnection (exponential backoff)
 *    5. Once controller back up, worker re-registers
 *    6. Worker resumes normal operation
 *    
 *    If Worker doesn't auto-reconnect:
 *    - Process manager restarts worker
 *    - Worker registers fresh
 *    - No state lost (stateless design)
 */

/**
 * Q5: How do I monitor worker health?
 * 
 * A: Several approaches:
 *    
 *    LOG MONITORING:
 *    ```bash
 *    tail -f worker.log | grep -E "(ERROR|heartbeat|job)"
 *    ```
 *    
 *    PROCESS MONITORING:
 *    ```bash
 *    while true; do
 *        if ! pgrep -x worker > /dev/null; then
 *            echo "Worker died! Restarting..."
 *            ./worker &
 *        fi
 *        sleep 10
 *    done
 *    ```
 *    
 *    METRICS ENDPOINT (if implemented):
 *    ```bash
 *    curl http://worker:9090/metrics
 *    # Returns: jobs_completed=47, jobs_failed=2, uptime_sec=3600
 *    ```
 *    
 *    CONTROLLER QUERY:
 *    Ask controller about worker status:
 *    ```bash
 *    ./admin_client --query-workers
 *    # Worker 1: ACTIVE, last_heartbeat=2s ago
 *    # Worker 2: ACTIVE, last_heartbeat=1s ago
 *    # Worker 3: DEAD, last_heartbeat=15s ago
 *    ```
 */

/**
 * Q6: What's the maximum job size a worker can handle?
 * 
 * A: Depends on available resources:
 *    
 *    MEMORY:
 *    - Loading 500 symbols × 1000 bars × 50 bytes/bar = 25MB
 *    - Plus strategy state, portfolio history: +10MB
 *    - Total: ~35MB per job
 *    - With 4GB RAM, can handle jobs up to ~1000 symbols
 *    
 *    TIME:
 *    - 1 symbol backtest: ~1-10ms
 *    - 100 symbols: ~1 second
 *    - 1000 symbols: ~10 seconds
 *    - 10,000 symbols: ~100 seconds
 *    
 *    LIMITS:
 *    - Practical max: ~5000 symbols per job
 *    - Above that, split into multiple jobs
 */

/**
 * Q7: Can I run workers on Windows?
 * 
 * A: Code uses POSIX signals, needs modification for Windows:
 *    
 *    Windows equivalent:
 *    ```cpp
 *    #ifdef _WIN32
 *    #include <windows.h>
 *    
 *    BOOL WINAPI ConsoleHandler(DWORD signal) {
 *        if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
 *            shutdown_requested = true;
 *            if (global_worker) {
 *                global_worker->stop();
 *            }
 *            return TRUE;
 *        }
 *        return FALSE;
 *    }
 *    
 *    int main() {
 *        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
 *        // ...
 *    }
 *    #else
 *    // POSIX signal handling (current code)
 *    #endif
 *    ```
 */

/**
 * Q8: How do I debug "worker not receiving jobs"?
 * 
 * A: Step-by-step diagnosis:
 *    
 *    STEP 1: Verify worker connected
 *    Worker logs should show:
 *    ```
 *    [INFO] Connected to controller at localhost:5000
 *    [INFO] Registered as worker_1
 *    ```
 *    If not, worker didn't connect successfully.
 *    
 *    STEP 2: Check heartbeats
 *    Worker logs should show heartbeats every 2 seconds:
 *    ```
 *    [DEBUG] Sending heartbeat (status=IDLE)
 *    ```
 *    If missing, heartbeat thread crashed.
 *    
 *    STEP 3: Verify jobs in controller queue
 *    Controller logs should show:
 *    ```
 *    [INFO] Job 42 submitted: AAPL SMA
 *    [INFO] Job queue size: 5
 *    ```
 *    If empty, no jobs to assign.
 *    
 *    STEP 4: Check worker status on controller
 *    Controller should consider worker ALIVE:
 *    ```
 *    [INFO] Worker 1 status: ALIVE (last heartbeat 1s ago)
 *    ```
 *    If DEAD, worker not sending heartbeats properly.
 *    
 *    STEP 5: Verify job assignment logic
 *    Controller should assign jobs to IDLE workers:
 *    ```
 *    [INFO] Assigning job 42 to worker 1
 *    ```
 *    If not assigning, controller's scheduler may have bug.
 */

/**
 * Q9: How do I scale the worker pool up/down?
 * 
 * A: SCALING UP (add more workers):
 *    
 *    ```bash
 *    # Just start more workers
 *    ./worker --controller kh01 &
 *    ./worker --controller kh01 &
 *    # Controller automatically discovers and uses them
 *    ```
 *    
 *    SCALING DOWN (remove workers):
 *    
 *    GRACEFUL (recommended):
 *    ```bash
 *    # Send SIGTERM, worker completes current job
 *    pkill -TERM worker
 *    ```
 *    
 *    IMMEDIATE (for emergencies):
 *    ```bash
 *    # Send SIGKILL, worker terminates immediately
 *    pkill -9 worker
 *    # Job in progress will be reassigned
 *    ```
 *    
 *    Controller handles both gracefully:
 *    - Detects worker disconnect via heartbeat failure
 *    - Marks worker as DEAD
 *    - Reassigns pending jobs to remaining workers
 */

/**
 * Q10: What's the recommended worker pool size?
 * 
 * A: Depends on workload and hardware:
 *    
 *    RULE OF THUMB:
 *    - CPU-bound jobs: 1 worker per CPU core
 *    - Mixed workload: 1.5-2× workers per core
 *    - I/O-bound jobs: 2-4× workers per core
 *    
 *    EXAMPLE:
 *    8-core machine:
 *    - Pure compute: 8 workers
 *    - Typical backtest: 12-16 workers
 *    - Heavy I/O: 16-32 workers
 *    
 *    BENCHMARKING:
 *    Test different pool sizes, measure throughput:
 *    ```bash
 *    for workers in 4 8 12 16; do
 *        ./deploy_workers.sh $workers
 *        time ./submit_100_jobs.sh
 *    done
 *    ```
 *    
 *    Optimal: Throughput peaks then plateaus
 *    - Too few workers: Underutilized CPUs
 *    - Too many workers: Context switching overhead
 *    - Sweet spot: Typically 1-2× CPU core count
 */

/**
 * Q11: How do I deploy workers behind a load balancer?
 * 
 * A: Workers connect OUT to controller, so load balancer not needed.
 *    Controller acts as coordinator, workers are clients.
 *    
 *    If you want high availability for controller:
 *    ```
 *         Load Balancer (HAProxy)
 *                │
 *        ┌───────┼───────┐
 *        ▼       ▼       ▼
 *      Ctrl1  Ctrl2  Ctrl3
 *        ▲       ▲       ▲
 *        └───────┼───────┘
 *                │
 *             Workers
 *    ```
 *    
 *    Workers connect to load balancer:
 *    ```bash
 *    ./worker --controller loadbalancer.local --port 5000
 *    ```
 *    
 *    Load balancer forwards to current Raft leader.
 */

/**
 * Q12: Can workers run on spot instances (AWS, GCP)?
 * 
 * A: Yes! Workers are designed for this:
 *    
 *    ADVANTAGES:
 *    - Cheap: Spot instances cost 60-80% less
 *    - Stateless: No data loss when instances terminated
 *    - Recoverable: Jobs reassigned automatically
 *    
 *    CONSIDERATIONS:
 *    - Enable checkpointing (resume after interruption)
 *    - Set up auto-scaling to replace terminated instances
 *    - Monitor job completion rate (may vary with spot availability)
 *    
 *    AWS Auto Scaling:
 *    ```bash
 *    aws autoscaling create-auto-scaling-group \
 *      --auto-scaling-group-name worker-pool \
 *      --instance-type c5.2xlarge \
 *      --min-size 2 \
 *      --max-size 10 \
 *      --desired-capacity 4
 *    ```
 *    
 *    Workers join cluster automatically on boot via user-data script.
 */