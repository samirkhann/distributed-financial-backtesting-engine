/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: worker.cpp
    
    Description:
        This file implements the Worker class, which serves as the distributed
        computation node in the financial backtesting system. Workers connect
        to the controller, receive job assignments, execute backtesting
        computations, and return results while maintaining health through
        periodic heartbeats.
        
        Core Responsibilities:
        - Controller Connection: TCP client connecting to controller server
        - Worker Registration: Register with controller, receive worker_id
        - Job Execution: Execute backtest strategies on assigned jobs
        - Result Reporting: Send computation results back to controller
        - Health Monitoring: Periodic heartbeat messages (every 2 seconds)
        - Fault Tolerance: Checkpoint support for long-running jobs
        - Data Management: Load historical price data from CSV files
        
    Worker Architecture:
        
        Worker Node (This Component)
        ├── Network Layer: TCP connection to controller
        ├── Job Processor: Execute backtest computations
        ├── Data Loader: CSV file parsing and caching
        ├── Checkpoint Manager: Save/resume progress
        └── Heartbeat: Periodic health status reporting
        
        Interaction Flow:
        
        1. STARTUP:
           Worker → Controller: Connect (TCP)
           Worker → Controller: WORKER_REGISTER
           Controller → Worker: WORKER_REGISTER_ACK (assigns worker_id)
        
        2. OPERATION:
           Controller → Worker: JOB_ASSIGN (backtest parameters)
           Worker: Execute backtest (may take seconds to minutes)
           Worker → Controller: JOB_RESULT (metrics, PnL, statistics)
           Worker → Controller: HEARTBEAT (every 2 seconds)
           Controller → Worker: HEARTBEAT_ACK (optional)
        
        3. SHUTDOWN:
           Worker: stop() called
           Worker: Close connection
           Worker: Threads joined
           Worker: Exit
        
    Concurrency Model:
        
        THREADS (3 total):
        1. Main Thread: Calls start(), stop(), configuration
        2. Worker Thread: Receives jobs, executes backtests
        3. Heartbeat Thread: Sends periodic heartbeats
        
        SYNCHRONIZATION:
        - active_jobs_: Atomic counter (lock-free)
        - completed_jobs_: Atomic counter
        - running_: Atomic flag
        - controller_socket_: Protected by single ownership (one writer)
        
        THREAD COMMUNICATION:
        - Worker thread: Blocks on recv() (waits for job)
        - Heartbeat thread: Sleeps between heartbeats
        - No shared data structures: Minimal synchronization needed
        
    Network Protocol:
        
        Message Exchange Pattern:
        
        Registration:
        Worker → Controller: WORKER_REGISTER {hostname, port}
        Controller → Worker: WORKER_REGISTER_ACK {worker_id}
        
        Job Assignment:
        Controller → Worker: JOB_ASSIGN {job_id, params}
        Worker processes job (seconds to minutes)
        Worker → Controller: JOB_RESULT {metrics}
        
        Health Monitoring:
        Worker → Controller: HEARTBEAT {worker_id, active_jobs, completed_jobs}
        Controller → Worker: HEARTBEAT_ACK (optional)
        (Repeats every 2 seconds while running)
        
    Fault Tolerance:
        
        CHECKPOINT INTEGRATION:
        - Long jobs: Checkpoint progress periodically
        - Worker crash: New worker resumes from checkpoint
        - Minimal work lost: Only since last checkpoint
        
        NETWORK FAILURE RECOVERY:
        - Connection lost: Worker detects in receive_message()
        - Worker exits: Operator restarts
        - Controller detects: Heartbeat timeout, reassigns jobs
        
        DATA LOADING:
        - CSV caching: Avoid redundant file I/O
        - Shared data: Multiple jobs can reuse loaded data
        
    Job Execution Pipeline:
        
        1. RECEIVE:
           - receive_message() blocks until JOB_ASSIGN arrives
           - Parse job parameters (symbol, strategy, dates, windows)
        
        2. LOAD DATA:
           - csv_loader_.load(symbol): Load historical prices
           - Check cache first: Avoid file I/O if already loaded
           - Filter by date range: Extract relevant time period
        
        3. VALIDATE:
           - Check sufficient data: Need enough bars for strategy
           - Typical: Need 200+ bars for 200-day moving average
        
        4. EXECUTE STRATEGY:
           - Create strategy instance (SMA, RSI, etc.)
           - Set parameters (windows, capital)
           - Generate signals (buy/sell decisions)
           - Simulate trading (track portfolio, calculate metrics)
           - Checkpoint progress: Save state periodically
        
        5. CALCULATE METRICS:
           - Total return: (final - initial) / initial
           - Sharpe ratio: risk-adjusted return
           - Max drawdown: worst peak-to-trough decline
           - Trade statistics: count, winners, losers
        
        6. SEND RESULT:
           - Create JobResultMessage
           - Serialize and send to controller
           - Log completion
        
    Performance Characteristics:
        
        Operation                    | Time          | Frequency
        -----------------------------|---------------|------------------
        Controller connection        | 1-10 ms       | Once at startup
        Worker registration          | 1-5 ms        | Once at startup
        Job receive                  | <1 ms         | Per job
        Data loading (uncached)      | 2-5 ms        | Per unique symbol
        Data loading (cached)        | <1 μs         | Per repeat symbol
        Job execution (5 years)      | 5-30 seconds  | Per job
        Job execution (1 year)       | 1-5 seconds   | Per job
        Checkpoint save              | 1-5 ms        | Every 100 symbols
        Result sending               | 1-3 ms        | Per job
        Heartbeat                    | 1-2 ms        | Every 2 seconds
        
    Memory Usage:
        
        Per worker process:
        - Code + libraries: ~5-10 MB
        - CSV cache (10 symbols): ~1 MB
        - Active job state: ~100 KB
        - Checkpoint state: ~10-50 KB
        - Network buffers: ~64 KB
        - Total: ~10-20 MB typical
        
        Scalable: Can run many workers on single machine
        
    Dependencies:
        - worker/worker.h: Class declaration
        - controller/controller.h: JobInfo definition
        - strategy/strategy_with_checkpoint.h: Strategy implementation
        - common/message.h: Protocol definitions
        - data/csv_loader.h: Data loading
        - worker/checkpoint_manager.h: Checkpoint persistence
        - POSIX: Socket API (socket, connect, send, recv)
        
    Related Files:
        - worker.h: Class and structure definitions
        - worker_main.cpp: Worker executable entry point
        - controller.cpp: Controller counterpart
        - strategy_with_checkpoint.cpp: Strategy implementations

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. INCLUDES & DEPENDENCIES
// 2. NAMESPACE DECLARATION
// 3. CONSTRUCTOR & DESTRUCTOR
//    3.1 Worker Constructor - Initialization
//    3.2 Worker Destructor - Cleanup
// 4. LIFECYCLE MANAGEMENT
//    4.1 start() - Start Worker
//    4.2 stop() - Stop Worker
// 5. CONTROLLER CONNECTION
//    5.1 connect_to_controller() - TCP Connection Setup
//    5.2 disconnect_from_controller() - Close Connection
//    5.3 register_with_controller() - Worker Registration
// 6. WORKER THREADS
//    6.1 worker_loop() - Job Processing Thread
//    6.2 heartbeat_loop() - Health Monitoring Thread
//    6.3 send_heartbeat() - Heartbeat Transmission
// 7. CHECKPOINT SUPPORT
//    7.1 should_checkpoint() - Checkpoint Decision
//    7.2 create_checkpoint_from_state() - Create Checkpoint
// 8. JOB PROCESSING
//    8.1 process_job_with_checkpoint() - Execute with Checkpoints
//    8.2 process_job() - Legacy Interface
//    8.3 create_strategy() - Strategy Factory
// 9. NETWORK COMMUNICATION
//    9.1 send_job_result() - Send Result to Controller
//    9.2 send_message() - Generic Message Send
//    9.3 receive_message() - Generic Message Receive
// 10. USAGE EXAMPLES
// 11. COMMON PITFALLS & SOLUTIONS
// 12. FAQ
// 13. BEST PRACTICES
// 14. TROUBLESHOOTING GUIDE
// 15. PERFORMANCE OPTIMIZATION
//
//==============================================================================

//==============================================================================
// SECTION 1: INCLUDES & DEPENDENCIES
//==============================================================================

#include "worker/worker.h"
#include "controller/controller.h"           // JobInfo definition
#include "strategy/strategy_with_checkpoint.h"  // Checkpoint-aware strategies
#include "common/message.h"                  // Message protocol, byte order helpers

// POSIX Socket API
// ================
#include <sys/socket.h>   // socket(), connect(), send(), recv()
#include <netinet/in.h>   // sockaddr_in, htons()
#include <arpa/inet.h>    // inet_ntop()
#include <unistd.h>       // close(), getpid()
#include <netdb.h>        // gethostbyname()

// C Standard Library
// ==================
#include <cerrno>         // errno
#include <cstring>        // strerror(), memcpy(), memset()

//==============================================================================
// SECTION 2: NAMESPACE DECLARATION
//==============================================================================

namespace backtesting {

//==============================================================================
// SECTION 3: CONSTRUCTOR & DESTRUCTOR
//==============================================================================

//------------------------------------------------------------------------------
// 3.1 WORKER CONSTRUCTOR - INITIALIZATION
//------------------------------------------------------------------------------
//
// Worker::Worker(const WorkerConfig& config)
//
// PURPOSE:
// Initializes worker with configuration but does NOT start networking.
// Actual startup happens in start() method (two-phase initialization).
//
// PARAMETERS:
// - config: Worker configuration (controller address, ports, directories)
//
// INITIALIZATION LIST:
// - config_: Copy configuration
// - worker_id_: 0 (invalid, assigned during registration)
// - controller_socket_: -1 (invalid socket, created in connect)
// - running_: false (not started)
// - active_jobs_: 0 (atomic counter)
// - completed_jobs_: 0 (atomic counter)
// - checkpoint_manager_: Initialize with checkpoint directory
//
// CSV LOADER CONFIGURATION:
// - Sets data directory for CSV file loading
// - Enables: Locating symbol data files (AAPL.csv, etc.)
//
// TWO-PHASE INITIALIZATION RATIONALE:
// - Constructor can't return errors (exceptions only)
// - start() can fail gracefully (return false)
// - Allows configuration before committing resources
//

Worker::Worker(const WorkerConfig& config)
    : config_(config),                          // Store configuration
      worker_id_(0),                            // Invalid ID (assigned by controller)
      controller_socket_(-1),                   // Invalid socket (created in connect)
      running_(false),                          // Not started yet
      active_jobs_(0),                          // No active jobs
      completed_jobs_(0),                       // No completed jobs
      checkpoint_manager_(config.checkpoint_directory) {  // Initialize checkpoint manager
    
    // Configure CSV loader with data directory
    // This tells loader where to find AAPL.csv, GOOGL.csv, etc.
    csv_loader_.set_data_directory(config.data_directory);
    
    // Constructor complete
    // Worker initialized but not connected/registered
    // Call start() to begin operation
}

//------------------------------------------------------------------------------
// 3.2 WORKER DESTRUCTOR - CLEANUP
//------------------------------------------------------------------------------
//
// Worker::~Worker()
//
// PURPOSE:
// Ensures clean shutdown of worker.
//
// CLEANUP:
// - Calls stop() for graceful shutdown
// - Joins threads
// - Closes socket
// - Checkpoint manager destroyed (automatic)
//

Worker::~Worker() {
    // Ensure worker stopped before destruction
    stop();
    
    // Automatic cleanup:
    // - csv_loader_ destroyed (cache cleared)
    // - checkpoint_manager_ destroyed (files closed)
    // - Threads already joined in stop()
}

//==============================================================================
// SECTION 4: LIFECYCLE MANAGEMENT
//==============================================================================

//------------------------------------------------------------------------------
// 4.1 START() - START WORKER
//------------------------------------------------------------------------------
//
// bool Worker::start()
//
// PURPOSE:
// Starts worker by connecting to controller, registering, and spawning threads.
//
// STARTUP SEQUENCE:
// 1. Check not already running
// 2. Connect to controller (TCP)
// 3. Register with controller (get worker_id)
// 4. Set running_ = true
// 5. Spawn worker thread (job processing)
// 6. Spawn heartbeat thread (health monitoring)
//
// RETURNS:
// - true: Successfully started
// - false: Connection or registration failed
//
// ERROR HANDLING:
// - Connect fails: Return false
// - Register fails: Disconnect and return false
// - Thread spawn fails: undefined (std::thread throws)
//
// THREAD LIFECYCLE:
// - worker_thread_: Receives and processes jobs
// - heartbeat_thread_: Sends periodic heartbeats
//

bool Worker::start() {
    // GUARD: Prevent double-start
    if (running_) {
        Logger::warning("Worker already running");
        return false;
    }
    
    Logger::info("Starting worker...");
    
    // PHASE 1: Connect to controller
    // Creates TCP socket and establishes connection
    if (!connect_to_controller()) {
        Logger::error("Failed to connect to controller");
        return false;
    }
    
    // PHASE 2: Register with controller
    // Sends WORKER_REGISTER, receives worker_id
    if (!register_with_controller()) {
        Logger::error("Failed to register with controller");
        disconnect_from_controller();  // Cleanup connection
        return false;
    }
    
    // PHASE 3: Enable threads
    running_ = true;
    
    // PHASE 4: Spawn worker thread
    // Main job processing loop
    worker_thread_ = std::thread(&Worker::worker_loop, this);
    
    // PHASE 5: Spawn heartbeat thread
    // Periodic health check messages
    heartbeat_thread_ = std::thread(&Worker::heartbeat_loop, this);
    
    Logger::info("Worker " + std::to_string(worker_id_) + " started successfully");
    return true;
    
    // AFTER START:
    // - Connected to controller
    // - Registered with assigned worker_id
    // - Worker thread waiting for jobs
    // - Heartbeat thread sending health updates
    // - Ready to process backtest jobs
}

//------------------------------------------------------------------------------
// 4.2 STOP() - STOP WORKER
//------------------------------------------------------------------------------
//
// void Worker::stop()
//
// PURPOSE:
// Gracefully stops worker by halting threads and closing connection.
//
// SHUTDOWN SEQUENCE:
// 1. Check if running
// 2. Set running_ = false (signal threads)
// 3. Join worker thread
// 4. Join heartbeat thread
// 5. Disconnect from controller
//
// THREAD TERMINATION:
// - worker_thread_: Exits when recv() fails or running_ false
// - heartbeat_thread_: Exits when sees running_ false
//
// IDEMPOTENCY:
// - Safe to call multiple times
// - First call: Stops threads
// - Subsequent calls: Return immediately
//

void Worker::stop() {
    // GUARD: Check if already stopped
    if (!running_) return;
    
    Logger::info("Stopping worker...");
    
    // PHASE 1: Signal threads to stop
    running_ = false;
    
    // PHASE 2: Join worker thread
    // Wait for it to exit (may be blocked in recv)
    // recv() will fail when socket closed
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
    
    // PHASE 3: Join heartbeat thread
    // Will exit on next loop iteration (checks running_)
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    
    // PHASE 4: Close controller connection
    // Shutdown and close socket
    disconnect_from_controller();
    
    Logger::info("Worker stopped");
    
    // After this:
    // - Both threads exited
    // - Socket closed
    // - Safe to destroy object
}

//==============================================================================
// SECTION 5: CONTROLLER CONNECTION
//==============================================================================

//------------------------------------------------------------------------------
// 5.1 CONNECT_TO_CONTROLLER() - TCP CONNECTION SETUP
//------------------------------------------------------------------------------
//
// bool Worker::connect_to_controller()
//
// PURPOSE:
// Establishes TCP connection to controller server.
//
// CONNECTION PROCESS:
// 1. Create socket (AF_INET, SOCK_STREAM)
// 2. Resolve controller hostname (DNS lookup)
// 3. Prepare server address structure
// 4. Connect to controller
//
// HOSTNAME RESOLUTION:
// - gethostbyname(): Converts hostname to IP
// - Supports: Hostnames ("kh01.ccs.neu.edu") and IPs ("192.168.1.1")
// - Blocking: May take time if DNS slow
//
// ERROR CASES:
// - Socket creation fails: Return false
// - Hostname resolution fails: Return false (DNS error)
// - Connection refused: Return false (controller not running)
// - Network unreachable: Return false
//
// RETURNS:
// - true: Successfully connected
// - false: Connection failed (check logs for errno)
//

bool Worker::connect_to_controller() {
    Logger::info("Connecting to controller at " + config_.controller_host + 
                ":" + std::to_string(config_.controller_port));
    
    // STEP 1: Create socket
    // AF_INET: IPv4
    // SOCK_STREAM: TCP (reliable, connection-oriented)
    controller_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (controller_socket_ < 0) {
        Logger::error("Failed to create socket: " + std::string(strerror(errno)));
        return false;
    }
    
    // STEP 2: Resolve controller hostname
    // gethostbyname: DNS lookup (blocking)
    // Returns: hostent structure with IP addresses
    struct hostent* host = gethostbyname(config_.controller_host.c_str());
    if (!host) {
        Logger::error("Failed to resolve hostname: " + config_.controller_host);
        close(controller_socket_);
        return false;
    }
    
    // STEP 3: Prepare server address
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.controller_port);
    
    // Copy IP address from hostent
    std::memcpy(&server_addr.sin_addr, host->h_addr, host->h_length);
    
    // STEP 4: Connect to controller
    if (connect(controller_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        Logger::error("Failed to connect: " + std::string(strerror(errno)));
        close(controller_socket_);
        return false;
    }
    
    Logger::info("Connected to controller");
    return true;
}

//------------------------------------------------------------------------------
// 5.2 DISCONNECT_FROM_CONTROLLER() - CLOSE CONNECTION
//------------------------------------------------------------------------------

void Worker::disconnect_from_controller() {
    if (controller_socket_ >= 0) {
        // shutdown: Disable sends and receives
        shutdown(controller_socket_, SHUT_RDWR);
        
        // close: Release file descriptor
        close(controller_socket_);
        
        // Mark as invalid
        controller_socket_ = -1;
    }
}

//------------------------------------------------------------------------------
// 5.3 REGISTER_WITH_CONTROLLER() - WORKER REGISTRATION
//------------------------------------------------------------------------------
//
// bool Worker::register_with_controller()
//
// PURPOSE:
// Registers worker with controller and receives assigned worker_id.
//
// REGISTRATION PROTOCOL:
// 1. Send WORKER_REGISTER message (hostname, port)
// 2. Wait for WORKER_REGISTER_ACK
// 3. Extract worker_id from response
//
// WORKER IDENTIFICATION:
// - Hostname: Uses process ID as unique identifier
// - Pattern: "worker_<pid>"
// - Example: "worker_12345"
// - Purpose: Debugging, logging
//
// PORT:
// - Set to 0: Worker doesn't listen (client only)
// - Future: Could listen for controller reconnection
//
// WORKER_ID ASSIGNMENT:
// - Controller assigns unique ID
// - Stored in: response.id field
// - Used for: Heartbeat messages, logging
//
// RETURNS:
// - true: Successfully registered
// - false: Registration failed
//

bool Worker::register_with_controller() {
    // Create registration message
    WorkerRegisterMessage msg;
    msg.worker_hostname = "worker_" + std::to_string(getpid());  // Unique ID based on PID
    msg.worker_port = 0;  // Worker doesn't listen (client only)
    
    // Send registration
    if (!send_message(msg)) {
        Logger::error("Failed to send registration message");
        return false;
    }
    
    // Wait for acknowledgment
    auto response = receive_message();
    
    // Validate response
    if (!response || response->type != MessageType::WORKER_REGISTER_ACK) {
        Logger::error("Failed to receive registration ACK");
        return false;
    }
    
    // Extract assigned worker_id
    // Controller sends worker_id in response.id field
    worker_id_ = response->id;
    
    Logger::info("Registered with controller, worker_id=" + std::to_string(worker_id_));
    
    return true;
}

//==============================================================================
// SECTION 6: WORKER THREADS
//==============================================================================

//------------------------------------------------------------------------------
// 6.1 WORKER_LOOP() - JOB PROCESSING THREAD
//------------------------------------------------------------------------------
//
// void Worker::worker_loop()
//
// PURPOSE:
// Main worker thread that receives jobs from controller and executes them.
//
// LOOP LOGIC:
// 1. Block on receive_message() (wait for job)
// 2. When JOB_ASSIGN arrives: Process job
// 3. Send result back to controller
// 4. Repeat until stopped
//
// MESSAGE HANDLING:
// - JOB_ASSIGN: Execute backtest
// - Other types: Ignored (or could log warning)
//
// JOB PROCESSING:
// - Increment active_jobs (for load balancing)
// - Execute backtest (may take seconds to minutes)
// - Decrement active_jobs
// - Increment completed_jobs
//
// ERROR HANDLING:
// - receive_message() returns nullptr: Connection lost
// - Exception during processing: Caught, logged, continue
//
// THREAD LIFECYCLE:
// - Started by: start() method
// - Runs until: running_ = false or connection lost
// - Joined by: stop() method
//

void Worker::worker_loop() {
    Logger::info("Worker loop started");
    
    // Job processing loop
    while (running_) {
        try {
            // BLOCKING CALL: Wait for message from controller
            auto msg = receive_message();
            
            // CHECK: Message received successfully
            if (!msg) {
                // Connection lost or error
                if (running_) {
                    Logger::error("Lost connection to controller");
                    running_ = false;  // Signal shutdown
                }
                break;  // Exit loop
            }
            
            // DISPATCH: Handle message based on type
            if (msg->type == MessageType::JOB_ASSIGN) {
                // JOB ASSIGNMENT RECEIVED
                
                // Downcast to specific message type
                auto job_msg = dynamic_cast<JobAssignMessage*>(msg.get());
                if (job_msg) {
                    Logger::info("Received job " + std::to_string(job_msg->job_id));
                    
                    // Create JobInfo structure
                    JobInfo job;
                    job.job_id = job_msg->job_id;
                    job.params = job_msg->params;
                    
                    // UPDATE STATISTICS: Increment active job count
                    // Used by: Load balancing (controller prefers less-loaded workers)
                    active_jobs_++;
                    
                    // PROCESS JOB: Execute backtest
                    // This may take seconds to minutes
                    // Blocks this thread (but that's okay - one job at a time)
                    process_job_with_checkpoint(job);
                    
                    // UPDATE STATISTICS: Job complete
                    active_jobs_--;
                    completed_jobs_++;
                }
            }
            // Other message types: Ignored (HEARTBEAT_ACK, etc.)
            
        } catch (const std::exception& e) {
            // Exception during job processing
            // Log error but continue (don't crash on one bad job)
            Logger::error("Exception in worker loop: " + std::string(e.what()));
        }
    }
    
    Logger::info("Worker loop stopped");
    
    // Thread exits when:
    // - running_ = false (stop() called)
    // - Connection lost (receive_message returned nullptr)
}

//------------------------------------------------------------------------------
// 6.2 HEARTBEAT_LOOP() - HEALTH MONITORING THREAD
//------------------------------------------------------------------------------
//
// void Worker::heartbeat_loop()
//
// PURPOSE:
// Background thread that sends periodic heartbeat messages to controller.
//
// HEARTBEAT PURPOSE:
// - Failure detection: Controller knows worker is alive
// - Load balancing: Reports active_jobs count
// - Statistics: Reports completed_jobs count
//
// FREQUENCY:
// - Configured: heartbeat_interval_sec (default 2 seconds)
// - Must be: Less than controller's heartbeat_timeout_sec (6 seconds)
// - Allows: Controller to detect failure in 6 seconds (3 missed heartbeats)
//
// LOOP LOGIC:
// 1. Sleep for heartbeat_interval_sec
// 2. Send heartbeat
// 3. Repeat until stopped
//
// ERROR HANDLING:
// - send_heartbeat() fails: Log warning, continue
// - Non-fatal: One missed heartbeat is okay
// - Multiple misses: Controller marks worker as failed
//
// THREAD LIFECYCLE:
// - Started by: start() method
// - Runs until: running_ = false
// - Joined by: stop() method
//

void Worker::heartbeat_loop() {
    // Heartbeat loop: Send periodic health updates
    while (running_) {
        // Sleep between heartbeats
        // Default: 2 seconds (configurable)
        // Must be << controller timeout (6 seconds)
        std::this_thread::sleep_for(std::chrono::seconds(config_.heartbeat_interval_sec));
        
        // Send heartbeat to controller
        if (!send_heartbeat()) {
            // Heartbeat failed: Network error
            // Log warning but continue
            // Controller will detect failure after 3 missed heartbeats (6 seconds)
            Logger::warning("Failed to send heartbeat");
        }
    }
    
    // Thread exits when running_ = false
}

//------------------------------------------------------------------------------
// 6.3 SEND_HEARTBEAT() - HEARTBEAT TRANSMISSION
//------------------------------------------------------------------------------
//
// bool Worker::send_heartbeat()
//
// PURPOSE:
// Creates and sends heartbeat message to controller.
//
// MESSAGE CONTENT:
// - worker_id: Our assigned ID (from registration)
// - active_jobs: Current number of executing jobs
// - completed_jobs: Total jobs completed since startup
//
// RETURNS:
// - true: Heartbeat sent successfully
// - false: Network error
//

bool Worker::send_heartbeat() {
    // Create heartbeat message
    HeartbeatMessage msg;
    msg.worker_id = worker_id_;           // Our ID
    msg.active_jobs = active_jobs_;       // Current load (for load balancing)
    msg.completed_jobs = completed_jobs_; // Statistics
    
    // Send message
    return send_message(msg);
}

//==============================================================================
// SECTION 7: CHECKPOINT SUPPORT
//==============================================================================

//------------------------------------------------------------------------------
// 7.1 SHOULD_CHECKPOINT() - CHECKPOINT DECISION
//------------------------------------------------------------------------------
//
// bool Worker::should_checkpoint(uint64_t symbols_processed) const
//
// PURPOSE:
// Determines if checkpoint should be saved based on progress.
//
// ALGORITHM:
// - Checkpoint if: symbols_processed is multiple of checkpoint_interval
// - Example: interval=100, checkpoint at 100, 200, 300, ...
//
// PARAMETERS:
// - symbols_processed: Number of symbols processed so far
//
// RETURNS:
// - true: Time to save checkpoint
// - false: Continue without checkpointing
//

bool Worker::should_checkpoint(uint64_t symbols_processed) const {
    // Check if checkpoint interval reached
    // symbols_processed > 0: Don't checkpoint at start (no progress yet)
    // symbols_processed % interval == 0: Multiple of interval
    //
    // Example: interval = 100
    // - symbols_processed = 0: false (no progress)
    // - symbols_processed = 50: false (not multiple)
    // - symbols_processed = 100: true (checkpoint!)
    // - symbols_processed = 150: false
    // - symbols_processed = 200: true (checkpoint!)
    return (symbols_processed > 0 && symbols_processed % config_.checkpoint_interval == 0);
}

//------------------------------------------------------------------------------
// 7.2 CREATE_CHECKPOINT_FROM_STATE() - CREATE CHECKPOINT
//------------------------------------------------------------------------------
//
// Checkpoint Worker::create_checkpoint_from_state(...)
//
// PURPOSE:
// Creates checkpoint structure from current execution state.
//
// PARAMETERS:
// - job_id: Job identifier
// - symbol: Stock ticker
// - symbols_processed: Number of symbols completed
// - bar_index: Current index in price data array
// - cash: Current cash in portfolio
// - shares: Current share position
// - portfolio_value: Current total portfolio value
// - last_date: Last processed date
//
// RETURNS:
// - Checkpoint: Populated checkpoint structure
//
// CHECKPOINT FIELDS:
// - job_id, symbol: Identification
// - symbols_processed, last_processed_index: Resume point
// - current_cash, current_shares, portfolio_value: Portfolio state
// - last_date_processed: Validation
// - valid: true (checkpoint is valid)
//

Checkpoint Worker::create_checkpoint_from_state(uint64_t job_id, const std::string& symbol,
                                                uint64_t symbols_processed, uint64_t bar_index,
                                                double cash, int shares, double portfolio_value,
                                                const std::string& last_date) {
    // Populate checkpoint structure
    Checkpoint cp;
    cp.job_id = job_id;
    cp.symbol = symbol;
    cp.symbols_processed = symbols_processed;
    cp.last_processed_index = bar_index;
    cp.current_cash = cash;
    cp.current_shares = shares;
    cp.portfolio_value = portfolio_value;
    cp.last_date_processed = last_date;
    cp.valid = true;  // Mark as valid checkpoint
    
    return cp;
}

//==============================================================================
// SECTION 8: JOB PROCESSING
//==============================================================================

//------------------------------------------------------------------------------
// 8.1 PROCESS_JOB_WITH_CHECKPOINT() - EXECUTE WITH CHECKPOINTS
//------------------------------------------------------------------------------
//
// void Worker::process_job_with_checkpoint(const JobInfo& job)
//
// PURPOSE:
// Executes backtest job with checkpoint support for fault tolerance.
//
// EXECUTION FLOW:
// 1. Load historical price data (CSV)
// 2. Validate data sufficiency
// 3. Create checkpoint-aware strategy
// 4. Execute backtest (with automatic checkpointing)
// 5. Send result to controller
//
// DATA LOADING:
// - csv_loader_.load(): Load symbol data
// - Caching: Reuses data if already loaded
// - Date filtering: Extract relevant time period
//
// STRATEGY CREATION:
// - SMAStrategyWithCheckpoint: SMA with checkpoint support
// - set_parameters(): Configure from job params
// - set_job_id(): For checkpoint file naming
//
// CHECKPOINT INTEGRATION:
// - strategy->backtest_with_checkpoint(): Automatic checkpointing
// - Periodic saves: Every checkpoint_interval symbols
// - Resume capability: If checkpoint exists, resumes from saved state
//
// ERROR HANDLING:
// - Data load fails: Send error result
// - No data in range: Send error result
// - Backtest exception: Caught, send error result
//
// RESULT TRANSMISSION:
// - Always sends result (success or failure)
// - Controller needs to know job completed
//

void Worker::process_job_with_checkpoint(const JobInfo& job) {
    Logger::info("Processing job " + std::to_string(job.job_id) + 
                " for symbol " + job.params.symbol);
    
    // Initialize result structure
    JobResult result;
    result.job_id = job.job_id;
    result.symbol = job.params.symbol;
    // Other fields filled by strategy execution
    
    try {
        // =====================================================================
        // STEP 1: LOAD PRICE DATA
        // =====================================================================
        
        // Load symbol data from CSV
        // csv_loader_ caches data (avoids repeated file I/O)
        auto data = csv_loader_.load(job.params.symbol);
        
        // VALIDATION 1: Data loaded successfully
        if (!data || data->empty()) {
            result.success = false;
            result.error_message = "Failed to load data for " + job.params.symbol;
            send_job_result(result);
            return;
        }
        
        // STEP 2: Filter by date range
        // Extract only bars in requested time period
        auto price_data = data->get_range(job.params.start_date, job.params.end_date);
        
        // VALIDATION 2: Data exists in date range
        if (price_data.empty()) {
            result.success = false;
            result.error_message = "No data in date range";
            send_job_result(result);
            return;
        }
        
        Logger::info("Loaded " + std::to_string(price_data.size()) + 
                    " bars for " + job.params.symbol);
        
        // =====================================================================
        // STEP 3: CREATE AND CONFIGURE STRATEGY
        // =====================================================================
        
        // Create checkpoint-aware SMA strategy
        // Uses CheckpointManager for fault tolerance
        auto strategy = std::make_unique<SMAStrategyWithCheckpoint>(&checkpoint_manager_);
        
        // Configure strategy with job parameters
        // Sets: short_window, long_window, initial_capital, etc.
        strategy->set_parameters(job.params);
        
        // Set job ID for checkpoint file naming
        // Creates checkpoints as: checkpoint_<job_id>.dat
        strategy->set_job_id(job.job_id);
        
        // =====================================================================
        // STEP 4: EXECUTE BACKTEST WITH CHECKPOINT SUPPORT
        // =====================================================================
        
        // Run backtest
        // - Checks for existing checkpoint (resume if exists)
        // - Executes strategy with periodic checkpoint saving
        // - Deletes checkpoint on completion
        result = strategy->backtest_with_checkpoint(price_data);
        result.job_id = job.job_id;  // Ensure job_id is set
        
        // Log completion with key metrics
        Logger::info("Job " + std::to_string(job.job_id) + " completed: " +
                    "Return=" + std::to_string(result.total_return * 100) + "%, " +
                    "Sharpe=" + std::to_string(result.sharpe_ratio) + ", " +
                    "MaxDD=" + std::to_string(result.max_drawdown * 100) + "%");
        
    } catch (const std::exception& e) {
        // EXCEPTION HANDLING
        // Backtest threw exception (data error, computation error, etc.)
        
        result.success = false;
        result.error_message = std::string("Exception: ") + e.what();
        
        Logger::error("Job " + std::to_string(job.job_id) + " failed: " + e.what());
    }
    
    // =========================================================================
    // STEP 5: SEND RESULT TO CONTROLLER
    // =========================================================================
    
    // Always send result (even if failed)
    // Controller needs to know job completed
    send_job_result(result);
}

//------------------------------------------------------------------------------
// 8.2 PROCESS_JOB() - LEGACY INTERFACE
//------------------------------------------------------------------------------

void Worker::process_job(const JobInfo& job) {
    // Delegate to checkpoint version
    // Maintains compatibility if code calls process_job directly
    process_job_with_checkpoint(job);
}

//------------------------------------------------------------------------------
// 8.3 CREATE_STRATEGY() - STRATEGY FACTORY
//------------------------------------------------------------------------------
//
// std::unique_ptr<Strategy> Worker::create_strategy(const std::string& strategy_type)
//
// PURPOSE:
// Factory method for creating strategy instances based on type name.
//
// PARAMETERS:
// - strategy_type: Strategy name (e.g., "SMA", "RSI")
//
// RETURNS:
// - unique_ptr<Strategy>: Created strategy
// - nullptr: Unknown strategy type
//
// SUPPORTED STRATEGIES:
// - "SMA": Simple Moving Average crossover
// - Future: "RSI", "MACD", "MeanReversion", etc.
//
// PATTERN:
// Factory pattern for polymorphic object creation
//

std::unique_ptr<Strategy> Worker::create_strategy(const std::string& strategy_type) {
    // Factory: Create strategy based on type name
    if (strategy_type == "SMA") {
        return std::make_unique<SMAStrategy>();
    }
    
    // Unknown strategy: Return nullptr
    // Caller should check for nullptr
    return nullptr;
}

//==============================================================================
// SECTION 9: NETWORK COMMUNICATION
//==============================================================================

//------------------------------------------------------------------------------
// 9.1 SEND_JOB_RESULT() - SEND RESULT TO CONTROLLER
//------------------------------------------------------------------------------

bool Worker::send_job_result(const JobResult& result) {
    // Create result message
    JobResultMessage msg;
    msg.result = result;
    
    // Send to controller
    return send_message(msg);
}

//------------------------------------------------------------------------------
// 9.2 SEND_MESSAGE() - GENERIC MESSAGE SEND
//------------------------------------------------------------------------------
//
// bool Worker::send_message(const Message& msg)
//
// PURPOSE:
// Serializes and sends message to controller with size prefix.
//
// PROTOCOL:
// [4-byte size (network byte order)][message data]
//
// FLAGS:
// - MSG_NOSIGNAL: Prevent SIGPIPE on broken connection
//
// RETURNS:
// - true: Message sent successfully
// - false: Network error
//

bool Worker::send_message(const Message& msg) {
    try {
        // Serialize message to binary
        auto data = msg.serialize();
        
        // Send size prefix (4 bytes, network byte order)
        uint32_t size = hton32(static_cast<uint32_t>(data.size()));
        ssize_t sent = send(controller_socket_, &size, sizeof(size), MSG_NOSIGNAL);
        if (sent != sizeof(size)) {
            return false;
        }
        
        // Send message data
        sent = send(controller_socket_, data.data(), data.size(), MSG_NOSIGNAL);
        return sent == static_cast<ssize_t>(data.size());
        
    } catch (const std::exception& e) {
        Logger::error("Failed to send message: " + std::string(e.what()));
        return false;
    }
}

//------------------------------------------------------------------------------
// 9.3 RECEIVE_MESSAGE() - GENERIC MESSAGE RECEIVE
//------------------------------------------------------------------------------
//
// std::unique_ptr<Message> Worker::receive_message()
//
// PURPOSE:
// Receives and deserializes message from controller with size prefix.
//
// PROTOCOL:
// [4-byte size][message data]
//
// SIZE VALIDATION:
// - Must be > 0 and < 1MB
//
// MESSAGE DISPATCH:
// - Reads message type (first byte)
// - Calls appropriate deserialize function
//
// RETURNS:
// - unique_ptr<Message>: Deserialized message
// - nullptr: Error or connection closed
//
// MESSAGE TYPES HANDLED:
// - JOB_ASSIGN: Job assignment from controller
// - HEARTBEAT_ACK: Heartbeat acknowledgment (optional)
// - WORKER_REGISTER_ACK: Registration confirmation
//
// SPECIAL HANDLING:
// - HEARTBEAT_ACK and WORKER_REGISTER_ACK:
//   * No dedicated message classes (just base Message)
//   * Manually extract id field from binary
//   * Used for: worker_id assignment, correlation
//

std::unique_ptr<Message> Worker::receive_message() {
    try {
        // STEP 1: Receive size prefix (4 bytes)
        uint32_t size;
        ssize_t received = recv(controller_socket_, &size, sizeof(size), MSG_WAITALL);
        if (received != sizeof(size)) {
            return nullptr;  // Connection closed or error
        }
        
        // Convert from network byte order
        size = ntoh32(size);
        
        // STEP 2: Validate size
        if (size == 0 || size > 1024 * 1024) {  // Max 1MB
            Logger::error("Invalid message size: " + std::to_string(size));
            return nullptr;
        }
        
        // STEP 3: Receive message data
        std::vector<uint8_t> data(size);
        received = recv(controller_socket_, data.data(), size, MSG_WAITALL);
        if (received != static_cast<ssize_t>(size)) {
            return nullptr;  // Incomplete data
        }
        
        // STEP 4: Parse message type
        if (data.empty()) return nullptr;
        
        MessageType type = static_cast<MessageType>(data[0]);
        
        // STEP 5: Deserialize based on type
        switch (type) {
            case MessageType::JOB_ASSIGN:
                // Full deserialize: Has dedicated message class
                return JobAssignMessage::deserialize(data.data(), data.size());
                
            case MessageType::HEARTBEAT_ACK: {
                // SPECIAL HANDLING: No dedicated class
                // Manually extract id field for correlation
                if (data.size() >= 13) {
                    auto msg = std::make_unique<Message>(MessageType::HEARTBEAT_ACK);
                    
                    // Extract id field (bytes 1-8 in header)
                    const uint8_t* ptr = data.data() + 1;  // Skip type byte
                    std::memcpy(&msg->id, ptr, 8);         // Copy 8 bytes
                    msg->id = ntoh64(msg->id);             // Convert byte order
                    
                    return msg;
                }
                // Size too small: Return basic message
                return std::make_unique<Message>(MessageType::HEARTBEAT_ACK);
            }
            
            case MessageType::WORKER_REGISTER_ACK: {
                // SPECIAL HANDLING: Extract worker_id from id field
                if (data.size() >= 13) {
                    auto msg = std::make_unique<Message>(MessageType::WORKER_REGISTER_ACK);
                    
                    // Extract id field (contains assigned worker_id)
                    const uint8_t* ptr = data.data() + 1;
                    std::memcpy(&msg->id, ptr, 8);
                    msg->id = ntoh64(msg->id);
                    
                    return msg;
                } else {
                    return std::make_unique<Message>(MessageType::WORKER_REGISTER_ACK);
                }
            }
            
            default:
                // Unknown message type
                Logger::warning("Unknown message type: " + std::to_string(static_cast<int>(type)));
                return nullptr;
        }
        
    } catch (const std::exception& e) {
        Logger::error("Failed to receive message: " + std::string(e.what()));
        return nullptr;
    }
}

} // namespace backtesting

//==============================================================================
// END OF IMPLEMENTATION
//==============================================================================

//==============================================================================
// SECTION 10: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Starting Worker on Khoury Cluster
// =============================================
//
// #include "worker/worker.h"
//
// int main() {
//     // Configure worker
//     WorkerConfig config;
//     config.controller_host = "kh01.ccs.neu.edu";
//     config.controller_port = 5000;
//     config.data_directory = "./data";
//     config.checkpoint_directory = "/tmp/checkpoints";
//     config.heartbeat_interval_sec = 2;
//     config.checkpoint_interval = 100;
//     
//     // Create worker
//     Worker worker(config);
//     
//     // Start worker (connects, registers, begins processing)
//     if (!worker.start()) {
//         Logger::error("Failed to start worker");
//         return 1;
//     }
//     
//     // Keep running until stopped
//     while (worker.is_running()) {
//         std::this_thread::sleep_for(std::chrono::seconds(1));
//     }
//     
//     return 0;
// }
//
//
// EXAMPLE 2: Multiple Workers on Same Machine
// ============================================
//
// // Start 4 workers on same machine, all connecting to same controller
// for (int i = 0; i < 4; ++i) {
//     pid_t pid = fork();
//     
//     if (pid == 0) {
//         // Child process: Run worker
//         WorkerConfig config;
//         config.controller_host = "kh01.ccs.neu.edu";
//         config.controller_port = 5000;
//         config.data_directory = "./data";  // Shared data directory
//         config.checkpoint_directory = "/tmp/checkpoints";
//         
//         Worker worker(config);
//         worker.start();
//         
//         // Run until stopped
//         while (worker.is_running()) {
//             sleep(1);
//         }
//         
//         exit(0);
//     }
// }
// // Parent: 4 workers now running
//
//
// EXAMPLE 3: Worker with Custom Configuration
// ============================================
//
// WorkerConfig config;
// config.controller_host = "controller.local";
// config.controller_port = 6000;  // Custom port
// config.data_directory = "/mnt/nfs/data";  // NFS shared storage
// config.checkpoint_directory = "/tmp/checkpoints";
// config.heartbeat_interval_sec = 1;  // More frequent (faster failure detection)
// config.checkpoint_interval = 50;  // More frequent checkpoints
//
// Worker worker(config);
// worker.start();
//
//
// EXAMPLE 4: Monitoring Worker Status
// ====================================
//
// void monitor_worker(Worker& worker) {
//     while (worker.is_running()) {
//         std::this_thread::sleep_for(std::chrono::seconds(10));
//         
//         Logger::info("Worker Status:");
//         Logger::info("  Worker ID: " + std::to_string(worker.get_worker_id()));
//         Logger::info("  Active jobs: " + std::to_string(worker.get_active_jobs()));
//         Logger::info("  Completed jobs: " + std::to_string(worker.get_completed_jobs()));
//     }
// }
//
//==============================================================================

//==============================================================================
// SECTION 11: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: Controller Not Running
// ==================================
// SYMPTOM:
//    worker.start() returns false
//    Log: "Failed to connect: Connection refused"
//
// SOLUTIONS:
//    1. Start controller first:
//       $ ./controller --port 5000
//    
//    2. Verify controller listening:
//       $ netstat -tulpn | grep 5000
//    
//    3. Check hostname/port:
//       $ telnet kh01.ccs.neu.edu 5000
//
//
// PITFALL 2: Wrong Hostname
// ==========================
// SYMPTOM:
//    Log: "Failed to resolve hostname: invalid_host"
//
// SOLUTIONS:
//    1. Use IP address instead:
//       config.controller_host = "192.168.1.1";
//    
//    2. Verify DNS:
//       $ nslookup kh01.ccs.neu.edu
//    
//    3. Check /etc/hosts:
//       127.0.0.1 localhost
//
//
// PITFALL 3: Data Files Not Found
// ================================
// SYMPTOM:
//    Job fails: "Failed to load data for AAPL"
//
// SOLUTIONS:
//    1. Verify files exist:
//       $ ls ./data/AAPL.csv
//    
//    2. Check data_directory config:
//       config.data_directory = "/correct/path";
//    
//    3. Use absolute paths:
//       config.data_directory = "/home/user/data";
//
//
// PITFALL 4: Connection Lost During Job
// ======================================
// SYMPTOM:
//    Worker exits mid-job
//    Log: "Lost connection to controller"
//
// SOLUTIONS:
//    1. Check network stability
//    2. Check controller still running
//    3. Restart worker (will resume from checkpoint)
//    4. Controller will reassign job
//
//
// PITFALL 5: No Checkpointing
// ============================
// SYMPTOM:
//    Worker crashes, must restart entire job
//
// SOLUTIONS:
//    1. Ensure checkpoint_manager_ initialized:
//       (Already done in constructor)
//    
//    2. Use SMAStrategyWithCheckpoint:
//       (Already used in process_job_with_checkpoint)
//    
//    3. Verify checkpoint directory writable:
//       $ ls -ld /tmp/checkpoints
//
//
// PITFALL 6: Worker ID Not Set
// =============================
// SYMPTOM:
//    worker_id_ = 0 (invalid)
//    Heartbeats fail or ignored
//
// SOLUTIONS:
//    1. Ensure registration succeeds:
//       (Checked in start() return value)
//    
//    2. Verify WORKER_REGISTER_ACK received:
//       (Checked in register_with_controller)
//
//==============================================================================

//==============================================================================
// SECTION 12: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Can multiple workers run on same machine?
// ==============================================
// A: Yes:
//    - Each worker is separate process
//    - All connect to same controller
//    - Each has unique worker_id (assigned by controller)
//    - Can use same data_directory (read-only access)
//    - Should use separate checkpoint_directory or unique IDs
//
// Q2: What happens if worker crashes mid-job?
// ============================================
// A: Fault tolerance with checkpoints:
//    1. Controller detects: Heartbeat timeout (6 seconds)
//    2. Controller reassigns: Job to different worker
//    3. New worker: Loads checkpoint, resumes from saved state
//    4. Result: Job completes with minimal wasted work
//
// Q3: How many jobs can worker process concurrently?
// ===================================================
// A: One at a time (sequential):
//    - worker_loop() processes one job before receiving next
//    - Could be extended: Thread pool for parallel jobs
//    - Current design: Simpler, one job per worker
//
// Q4: Can worker reconnect if controller restarts?
// =================================================
// A: No (current implementation):
//    - Worker detects connection lost, exits
//    - Must manually restart worker
//    - Future: Could add reconnection logic
//
// Q5: How does worker know which controller is leader (Raft)?
// ============================================================
// A: Doesn't need to know:
//    - Workers connect to any controller
//    - Controller configuration provides address
//    - For Raft: Could provide multiple addresses, try each
//
// Q6: Where are checkpoints stored?
// ==================================
// A: Configured location:
//    - Default: /tmp/checkpoints
//    - Configurable: checkpoint_directory in WorkerConfig
//    - One file per job: checkpoint_<job_id>.dat
//
// Q7: Can worker use cached data from previous jobs?
// ===================================================
// A: Yes:
//    - csv_loader_ maintains cache
//    - Symbol loaded once: Cached for subsequent jobs
//    - Memory efficient: One copy per symbol
//    - Persists: Until worker shutdown
//
// Q8: What if data not available for symbol?
// ===========================================
// A: Graceful failure:
//    - csv_loader_.load() throws exception
//    - Caught in process_job_with_checkpoint
//    - Sends error result to controller
//    - Job marked as failed
//
// Q9: How fast can worker process jobs?
// ======================================
// A: Depends on job complexity:
//    - Simple (1 year): 1-5 seconds
//    - Medium (5 years): 5-15 seconds
//    - Complex (20 years): 20-60 seconds
//    - Throughput: ~0.1-1 jobs/sec per worker
//
// Q10: Can worker be stopped gracefully?
// =======================================
// A: Yes:
//    - Call stop() method
//    - Sets running_ = false
//    - Threads exit gracefully
//    - Current job: Finishes (not interrupted)
//    - Connection: Closed cleanly
//
//==============================================================================

//==============================================================================
// SECTION 13: BEST PRACTICES
//==============================================================================
//
// BEST PRACTICE 1: Always Check start() Return Value
// ===================================================
// DO:
//    if (!worker.start()) {
//        Logger::error("Worker startup failed");
//        return 1;
//    }
//
// DON'T:
//    worker.start();  // Ignore return value
//
//
// BEST PRACTICE 2: Use Shared Data Directory for Multiple Workers
// ================================================================
// DO:
//    // All workers on same machine share data
//    config.data_directory = "/mnt/nfs/data";  // Shared NFS
//    
//    // Or local:
//    config.data_directory = "/opt/backtesting/data";
//    
//    // Benefit: One copy of CSV files, all workers read
//
//
// BEST PRACTICE 3: Use Separate Checkpoint Directories
// =====================================================
// DO:
//    // Worker 1:
//    config.checkpoint_directory = "/tmp/checkpoints/worker1";
//    
//    // Worker 2:
//    config.checkpoint_directory = "/tmp/checkpoints/worker2";
//    
//    // Or: Use shared directory (job_id makes checkpoints unique)
//    config.checkpoint_directory = "/tmp/checkpoints";  // OK
//
//
// BEST PRACTICE 4: Monitor Worker Health
// =======================================
// Check periodically:
//    - Worker still running: is_running()
//    - Active jobs: get_active_jobs()
//    - Completed jobs: get_completed_jobs()
//    - Connection status: Check receive_message() success
//
//
// BEST PRACTICE 5: Handle Exceptions Gracefully
// ==============================================
// Job processing exceptions shouldn't crash worker:
//    - Caught in process_job_with_checkpoint
//    - Error result sent to controller
//    - Worker continues to next job
//
//
// BEST PRACTICE 6: Configure Appropriate Heartbeat Interval
// ==========================================================
// Relationship:
//    heartbeat_interval < (controller_timeout / 3)
//    2 seconds < (6 seconds / 3) = 2 seconds ✓
//
// Ensures: Controller receives heartbeats reliably
//
//
// BEST PRACTICE 7: Pre-load Frequently Used Symbols
// ==================================================
// At startup:
//    for (auto& symbol : common_symbols) {
//        csv_loader_.load(symbol);  // Cache
//    }
//    // Future jobs: Use cached data (faster)
//
//==============================================================================

//==============================================================================
// SECTION 14: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM: Worker won't start
// ============================
// SYMPTOMS:
//    - start() returns false
//    - Worker exits immediately
//
// DEBUGGING:
//    ☐ Check controller running:
//       $ ps aux | grep controller
//    
//    ☐ Check network connectivity:
//       $ ping controller_host
//       $ telnet controller_host 5000
//    
//    ☐ Check logs for specific error:
//       [ERROR] Failed to connect: Connection refused
//       [ERROR] Failed to resolve hostname: ...
//
//
// PROBLEM: Worker loses connection
// =================================
// SYMPTOMS:
//    - Worker exits during operation
//    - Log: "Lost connection to controller"
//
// DEBUGGING:
//    ☐ Check controller still running:
//       $ ps aux | grep controller
//    
//    ☐ Check network stability:
//       $ ping -c 100 controller_host
//    
//    ☐ Check firewall not blocking:
//       $ iptables -L
//
//
// PROBLEM: Jobs failing with data errors
// =======================================
// SYMPTOMS:
//    - All jobs fail: "Failed to load data"
//    - Or: "No data in date range"
//
// DEBUGGING:
//    ☐ Verify CSV files exist:
//       $ ls data/*.csv
//    
//    ☐ Check data_directory config:
//       Logger::info("Data dir: " + config.data_directory);
//    
//    ☐ Verify date ranges in CSV files:
//       $ head data/AAPL.csv
//       $ tail data/AAPL.csv
//    
//    ☐ Check requested dates match available data:
//       start_date, end_date within CSV date range
//
//
// PROBLEM: Worker high CPU usage
// ===============================
// SYMPTOMS:
//    - Worker process at 100% CPU
//
// DEBUGGING:
//    ☐ Check if executing job:
//       $ ps -p <pid> -o %cpu,state
//       → High CPU during job execution is normal
//    
//    ☐ Check for busy-wait:
//       → Should sleep in loops (heartbeat_loop does)
//    
//    ☐ Profile with perf:
//       $ perf top -p <pid>
//
//
// PROBLEM: Checkpoint not working
// ================================
// SYMPTOMS:
//    - Worker crash, job restarts from beginning
//
// DEBUGGING:
//    ☐ Check checkpoint directory exists:
//       $ ls -ld /tmp/checkpoints
//    
//    ☐ Check checkpoint files created:
//       $ ls /tmp/checkpoints/checkpoint_*.dat
//    
//    ☐ Check using checkpoint strategy:
//       → SMAStrategyWithCheckpoint (yes)
//    
//    ☐ Check checkpoint_interval:
//       → Should be reasonable (100 default)
//
//==============================================================================

//==============================================================================
// SECTION 15: PERFORMANCE OPTIMIZATION
//==============================================================================
//
// OPTIMIZATION 1: Pre-load Common Symbols
// ========================================
// Load frequently-used symbols at startup:
//
// void Worker::preload_data(const std::vector<std::string>& symbols) {
//     for (const auto& symbol : symbols) {
//         try {
//             csv_loader_.load(symbol);
//             Logger::info("Preloaded: " + symbol);
//         } catch (...) {
//             Logger::warning("Failed to preload: " + symbol);
//         }
//     }
// }
//
// Call at startup (after start()):
// worker.preload_data({"AAPL", "GOOGL", "MSFT", "AMZN"});
//
// Benefit: First job for each symbol faster (no file I/O delay)
//
//
// OPTIMIZATION 2: Parallel Job Processing
// ========================================
// Process multiple jobs concurrently (not implemented):
//
// void Worker::worker_loop_parallel() {
//     // Thread pool for parallel job execution
//     ThreadPool pool(4);  // 4 concurrent jobs
//     
//     while (running_) {
//         auto msg = receive_message();
//         if (msg && msg->type == MessageType::JOB_ASSIGN) {
//             // Submit to thread pool (async)
//             pool.submit([this, job]() {
//                 process_job_with_checkpoint(job);
//             });
//         }
//     }
// }
//
// Benefit: Higher throughput per worker
// Complexity: Requires thread pool, synchronization
//
//
// OPTIMIZATION 3: Asynchronous Heartbeats
// ========================================
// Current: Heartbeat may block if send() slow
// Could improve: Non-blocking send or timeout
//
// struct timeval timeout = {1, 0};  // 1 second timeout
// setsockopt(controller_socket_, SOL_SOCKET, SO_SNDTIMEO, 
//           &timeout, sizeof(timeout));
//
// Benefit: Heartbeat thread never blocks indefinitely
//
//
// OPTIMIZATION 4: Connection Pooling (for Raft)
// ==============================================
// If using multiple controllers:
// - Maintain connections to all controllers
// - Failover: Switch to backup if primary fails
// - Not needed: For single controller deployment
//
//==============================================================================

//==============================================================================
// INTEGRATION WITH DISTRIBUTED SYSTEM
//==============================================================================
//
// WORKER IN DISTRIBUTED ARCHITECTURE:
// ====================================
//
//        ┌────────────────────────────────────────┐
//        │ Controller Cluster (Raft)              │
//        │ ┌──────────┐ ┌──────────┐ ┌──────────┐│
//        │ │ Leader   │ │ Follower │ │ Follower ││
//        │ │ (Active) │ │          │ │          ││
//        │ └──────────┘ └──────────┘ └──────────┘│
//        └────────────────────────────────────────┘
//                         ↓
//        ┌────────────────────────────────────────┐
//        │ Worker Pool (2-8 Nodes)                │
//        │ ┌────────┐ ┌────────┐ ┌────────┐      │
//        │ │Worker 1│ │Worker 2│ │Worker 3│ ...  │
//        │ │ (Busy) │ │ (Idle) │ │ (Busy) │      │
//        │ └────────┘ └────────┘ └────────┘      │
//        └────────────────────────────────────────┘
//                         ↓
//        ┌────────────────────────────────────────┐
//        │ Shared Data Storage (NFS or Local)     │
//        │ AAPL.csv, GOOGL.csv, MSFT.csv, ...     │
//        └────────────────────────────────────────┘
//
// WORKER RESPONSIBILITIES:
// - Compute: Execute backtest algorithms
// - Report: Send results to controller
// - Survive: Use checkpoints for fault tolerance
//
// CONTROLLER RESPONSIBILITIES:
// - Coordinate: Distribute jobs to workers
// - Monitor: Track worker health via heartbeats
// - Aggregate: Collect results for clients
//
// SHARED STORAGE:
// - NFS: All workers access same CSV files
// - Local: Each worker has local copy (pre-distributed)
// - Performance: Local faster, NFS easier to manage
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================