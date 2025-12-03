/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: worker.h
    
    Description:
        This header file defines the Worker class, which implements the worker
        node component of the distributed backtesting system. Worker nodes are
        the computational workhorses that execute backtest jobs in parallel,
        enabling the system to achieve the project goal of ≥0.85 parallel
        efficiency at 8 workers.
        
        Workers are autonomous agents that:
        - Connect to the controller cluster to request work
        - Execute trading strategies on historical price data
        - Send periodic heartbeats to prove liveness
        - Save checkpoints for crash recovery
        - Return results to the controller for aggregation
        
        The Worker class implements a multi-threaded architecture with three
        main threads:
        1. Main thread: Initialization and shutdown coordination
        2. Worker thread: Job processing loop
        3. Heartbeat thread: Periodic liveness signals
        
    System Architecture Context:
        
        ┌──────────────────┐
        │ Controller Cluster│
        │  (Raft Leader)   │
        └────────┬─────────┘
                 │ Job distribution
                 │ Heartbeat monitoring
                 ↓
        ┌────────────────────────────────┐
        │      Worker Pool (2-8 nodes)    │
        ├────────────────────────────────┤
        │ ┌──────────┐  ┌──────────┐    │
        │ │ Worker 1 │  │ Worker 2 │ ...│  ← This class
        │ │ ./worker │  │ ./worker │    │
        │ └──────────┘  └──────────┘    │
        └────────────────────────────────┘
                 ↓
        ┌────────────────────────────────┐
        │  Shared Storage / Data Shards  │
        │  - Historical price data (CSV) │
        │  - Checkpoint files (binary)   │
        └────────────────────────────────┘
        
    Key Responsibilities:
        1. Connection Management: Establish and maintain TCP connection to controller
        2. Registration: Announce capabilities and receive worker ID
        3. Job Processing: Execute backtest strategies on assigned data
        4. Heartbeat Protocol: Send periodic alive signals (every 2 seconds)
        5. Checkpoint Management: Save/load state for fault tolerance
        6. Result Reporting: Send completed job results to controller
        
    Fault Tolerance Features:
        - Automatic reconnection on controller failure
        - Checkpoint-based resume after crashes
        - Graceful degradation (continues if checkpointing fails)
        - Clean shutdown on SIGINT/SIGTERM
        
    Performance Characteristics:
        - Job processing: 10-100ms per job (depends on data size)
        - Heartbeat overhead: <1ms every 2 seconds (~0.05%)
        - Checkpoint overhead: 1-5ms per checkpoint (~2-17% depending on interval)
        - Memory footprint: ~10-50 MB per worker
        - CPU utilization: 80-100% during job processing
        
    Design Philosophy:
        - Autonomous operation: Workers don't coordinate with each other
        - Pull-based: Workers request work (not pushed by controller)
        - Stateless between jobs: No shared state across job executions
        - Fail-safe: Errors don't crash worker, just skip job
        - Observable: Comprehensive logging for debugging
        
*******************************************************************************/

#ifndef WORKER_H
#define WORKER_H

#include "common/message.h"
#include "common/logger.h"
#include "data/csv_loader.h"
#include "strategy/sma_strategy.h"
#include "worker/checkpoint_manager.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

namespace backtesting {

/*******************************************************************************
 * TABLE OF CONTENTS
 * =================
 * 
 * 1. WORKER ARCHITECTURE OVERVIEW
 *    - Role in distributed system
 *    - Thread model
 *    - Communication patterns
 *    - Fault tolerance mechanisms
 * 
 * 2. DATA STRUCTURES
 *    - WorkerConfig structure
 *    - JobInfo forward declaration
 * 
 * 3. WORKER CLASS
 *    - Private members (configuration, state, threads)
 *    - Public interface (lifecycle, getters)
 *    - Private methods (communication, processing)
 * 
 * 4. CONFIGURATION GUIDE
 *    - WorkerConfig parameters
 *    - Tuning recommendations
 * 
 * 5. LIFECYCLE MANAGEMENT
 *    - Construction
 *    - Startup sequence
 *    - Shutdown procedure
 * 
 * 6. THREAD ARCHITECTURE
 *    - Worker thread responsibilities
 *    - Heartbeat thread responsibilities
 *    - Thread synchronization
 * 
 * 7. JOB PROCESSING WORKFLOW
 *    - Job acquisition
 *    - Strategy execution
 *    - Result reporting
 *    - Checkpointing
 * 
 * 8. HEARTBEAT PROTOCOL
 *    - Purpose and frequency
 *    - Failure detection
 * 
 * 9. USAGE EXAMPLES
 *    - Basic worker deployment
 *    - Multi-worker cluster
 *    - Crash recovery
 * 
 * 10. PERFORMANCE CONSIDERATIONS
 * 11. COMMON PITFALLS
 * 12. FREQUENTLY ASKED QUESTIONS
 * 
*******************************************************************************/

/*******************************************************************************
 * SECTION 1: WORKER ARCHITECTURE OVERVIEW
 * 
 * ROLE IN DISTRIBUTED SYSTEM
 * ===========================
 * 
 * Workers are the compute nodes that execute backtest jobs. They operate
 * independently and in parallel, enabling horizontal scaling:
 * 
 * Scaling Equation:
 *   1 worker: Processes N jobs in time T
 *   8 workers: Process N jobs in time T/6.8 (85% efficient)
 * 
 * Worker Autonomy:
 *   - Each worker operates independently
 *   - No inter-worker communication
 *   - No shared state between workers
 *   - Embarrassingly parallel workload
 * 
 * This design enables near-linear scaling because:
 *   + No coordination overhead
 *   + No lock contention
 *   + No network traffic between workers
 *   + Only bottleneck is controller scheduling
 * 
 * THREAD MODEL
 * ============
 * 
 * Worker uses multi-threading for responsiveness:
 * 
 * Main Thread:
 *   - Creates and starts worker
 *   - Blocks waiting for shutdown signal
 *   - Calls stop() and joins worker threads
 * 
 * Worker Thread (worker_thread_):
 *   - Requests jobs from controller
 *   - Loads historical data
 *   - Executes strategy
 *   - Saves checkpoints
 *   - Sends results
 *   - Repeats until shutdown
 * 
 * Heartbeat Thread (heartbeat_thread_):
 *   - Sends periodic heartbeats to controller
 *   - Runs every 2 seconds
 *   - Independent of job processing
 * 
 * Thread Communication:
 *   - running_ flag: Atomic, no locks needed
 *   - job_queue_: Protected by queue_mutex_
 *   - statistics: Atomic counters
 * 
 * Why Multiple Threads?
 *   
 *   Alternative: Single-threaded with select()/poll()
 *     - Process jobs
 *     - Check if 2 seconds elapsed
 *     - Send heartbeat
 *     - Repeat
 *     
 *     Problem: Job processing blocks heartbeats
 *     If job takes 10 seconds, no heartbeat for 10 seconds
 *     Controller thinks worker is dead!
 *   
 *   Solution: Separate heartbeat thread
 *     - Heartbeats sent on schedule regardless of job duration
 *     - Controller never sees timeout
 *     - Worker appears healthy even during long jobs
 * 
 * COMMUNICATION PATTERNS
 * ======================
 * 
 * Worker-Controller Protocol:
 * 
 * 1. Registration:
 *    Worker → Controller: REGISTER message
 *    Controller → Worker: REGISTER_ACK with worker_id
 * 
 * 2. Job Request (Pull Model):
 *    Worker → Controller: REQUEST_JOB
 *    Controller → Worker: JOB_ASSIGNED or NO_JOBS_AVAILABLE
 * 
 * 3. Heartbeat:
 *    Worker → Controller: HEARTBEAT
 *    Controller → Worker: HEARTBEAT_ACK
 * 
 * 4. Result Submission:
 *    Worker → Controller: JOB_RESULT
 *    Controller → Worker: RESULT_ACK
 * 
 * Pull vs Push:
 *   
 *   Current (Pull):
 *     - Worker requests job when ready
 *     - Controller responds with job or "no work"
 *     
 *     Advantages:
 *     + Worker controls its pace
 *     + Natural load balancing (fast workers get more jobs)
 *     + Simple to implement
 *     
 *   Alternative (Push):
 *     - Controller assigns jobs to workers
 *     - Worker processes assigned job
 *     
 *     Disadvantages:
 *     - Controller must track worker capacity
 *     - Less natural load balancing
 *     - More complex coordination
 * 
 * FAULT TOLERANCE MECHANISMS
 * ==========================
 * 
 * 1. Heartbeats:
 *    - Sent every 2 seconds
 *    - Controller detects failure: 3 missed = 6 seconds
 *    - Triggers job reassignment
 * 
 * 2. Checkpointing:
 *    - Saved every N bars (configurable)
 *    - Enables resume after crash
 *    - Typical interval: 100-1000 bars
 * 
 * 3. Automatic Reconnection:
 *    - If controller fails, worker retries connection
 *    - Exponential backoff
 *    - Eventually rejoins cluster
 * 
 * Recovery Scenarios:
 * 
 * Worker Crash:
 *   1. Worker A processing job 42, crashes at bar 550
 *   2. Controller detects timeout (6 seconds)
 *   3. Controller reassigns job 42 to Worker B
 *   4. Worker B loads checkpoint (last saved at bar 500)
 *   5. Worker B resumes from bar 500
 *   6. Total recovery: 6s detection + 50 bars reprocess = ~6.05s
 * 
 * Controller Crash:
 *   1. Worker loses connection to controller
 *   2. Worker retries connection (exponential backoff)
 *   3. Controller cluster elects new leader (~3 seconds)
 *   4. Worker connects to new leader
 *   5. Registers and continues processing
 * 
 * Network Partition:
 *   1. Worker isolated from controller
 *   2. Worker retries connection
 *   3. When partition heals, worker reconnects
 *   4. Controller may have reassigned job (worker discards local work)
 * 
 *******************************************************************************/

// Forward declaration - full definition in controller.h
/**
 * @struct JobInfo
 * @brief Job specification from controller
 * 
 * This structure contains all information needed to execute a backtest job.
 * The full definition is in controller.h to avoid circular dependencies.
 * 
 * Expected Fields:
 *   - job_id: Unique job identifier
 *   - symbol: Stock symbol to backtest
 *   - strategy_type: "SMA", "RSI", "MACD", etc.
 *   - parameters: Strategy-specific parameters
 *   - start_date: Beginning of backtest period
 *   - end_date: End of backtest period
 * 
 * @see controller.h For complete definition
 */
struct JobInfo;

/*******************************************************************************
 * SECTION 2: DATA STRUCTURES
 *******************************************************************************/

/**
 * @struct WorkerConfig
 * @brief Configuration parameters for worker node
 * 
 * This structure encapsulates all configurable parameters for a worker,
 * allowing flexible deployment across different environments (development,
 * testing, production).
 * 
 * Design Pattern: Configuration Object
 *   Bundles related configuration into a single structure for easy:
 *   - Construction with sensible defaults
 *   - Passing to Worker constructor
 *   - Loading from configuration files (future)
 *   - Override via command-line arguments
 * 
 * Parameter Tuning Guide:
 *   See SECTION 4 for detailed tuning recommendations.
 * 
 * @see Worker::Worker() For usage in construction
 */
struct WorkerConfig {
    /**
     * @var controller_host
     * @brief Hostname or IP address of controller leader
     * 
     * The worker connects to this address to register and request jobs.
     * In a Raft cluster, this should point to the current leader or a
     * load balancer that redirects to the leader.
     * 
     * Default: "localhost"
     * 
     * Deployment Examples:
     *   Development: "localhost" or "127.0.0.1"
     *   Khoury cluster: "kh01.khoury.neu.edu" or "kh01"
     *   AWS: "backtesting-controller.us-east-1.elb.amazonaws.com"
     *   Load balanced: "controller.internal" (DNS round-robin)
     * 
     * Failover Handling:
     *   If leader fails, worker must reconnect to new leader.
     *   Options:
     *   1. DNS-based: Update DNS to point to new leader
     *   2. Discovery service: Query etcd/Consul for current leader
     *   3. Multiple addresses: Try each until one succeeds
     * 
     * Thread Safety: Read-only after construction
     */
    std::string controller_host;
    
    /**
     * @var controller_port
     * @brief TCP port for controller worker API
     * 
     * Default: 5000
     * 
     * Port Assignment:
     *   5000: Worker connections (this port)
     *   6000: Controller-to-controller Raft communication
     *   
     * Note: This is the controller's WORKER port, not Raft port.
     * 
     * Firewall Requirements:
     *   Worker must have outbound access to this port.
     *   Inbound not needed (worker initiates connection).
     * 
     * Thread Safety: Read-only after construction
     */
    uint16_t controller_port;
    
    /**
     * @var data_directory
     * @brief Path to historical price data files
     * 
     * Default: "./data"
     * 
     * Directory Structure:
     *   ./data/
     *     ├── AAPL.csv
     *     ├── GOOGL.csv
     *     ├── MSFT.csv
     *     └── ...
     * 
     * Data Access Strategy:
     *   
     *   Option 1: NFS Mount (Shared)
     *     All workers access same NFS-mounted directory
     *     Pros: Single source of truth, easy updates
     *     Cons: Network I/O overhead, NFS bottleneck
     *   
     *   Option 2: Local Replication
     *     Each worker has local copy of data
     *     Pros: Fast local I/O, no network dependency
     *     Cons: Data synchronization complexity, disk space
     *   
     *   For Khoury cluster: NFS is reasonable (low latency)
     *   For cloud: Local replication is better (network expensive)
     * 
     * Thread Safety: Read-only after construction
     * 
     * @see CSVLoader For data loading implementation
     */
    std::string data_directory;
    
    /**
     * @var checkpoint_directory
     * @brief Path to checkpoint file storage
     * 
     * Default: "./checkpoints"
     * 
     * Storage Recommendations:
     *   
     *   Development:
     *     "./checkpoints" - Local directory
     *   
     *   Production (Local Disk):
     *     "/var/lib/backtesting/worker_<id>/checkpoints"
     *     Pros: Fast I/O
     *     Cons: Only this worker can access
     *   
     *   Production (NFS):
     *     "/nfs/backtesting/checkpoints"
     *     Pros: Any worker can load any checkpoint (mobility)
     *     Cons: Slower I/O, network dependency
     * 
     * Disk Space Requirements:
     *   - Per checkpoint: ~100 bytes
     *   - Active jobs: Typically < 100
     *   - Total: < 10 KB (negligible)
     * 
     * Thread Safety: Read-only after construction
     * 
     * @see CheckpointManager For checkpoint operations
     */
    std::string checkpoint_directory;
    
    /**
     * @var heartbeat_interval_sec
     * @brief Seconds between heartbeat messages
     * 
     * Default: 2 seconds
     * 
     * Heartbeat Theory:
     *   Heartbeats prove worker liveness. Controller detects failure when:
     *   - N consecutive heartbeats missed
     *   - Timeout = heartbeat_interval × N
     *   
     *   Project requirement: Detect failure in ~6 seconds
     *   - heartbeat_interval = 2 seconds
     *   - missed_threshold = 3
     *   - timeout = 2 × 3 = 6 seconds ✓
     * 
     * Tuning Trade-offs:
     *   
     *   Shorter interval (e.g., 1 second):
     *     + Faster failure detection (3 seconds)
     *     + More responsive system
     *     - More network overhead
     *     - More CPU overhead (heartbeat processing)
     *   
     *   Longer interval (e.g., 5 seconds):
     *     + Less overhead
     *     - Slower failure detection (15 seconds)
     *     - Might timeout during long jobs
     * 
     * Network Overhead Calculation:
     *   Heartbeat size: ~50 bytes
     *   Frequency: 0.5 Hz (every 2 seconds)
     *   Bandwidth: 50 bytes × 0.5 Hz = 25 bytes/sec
     *   
     *   For 8 workers: 8 × 25 = 200 bytes/sec = negligible
     * 
     * Thread Safety: Read-only after construction
     * 
     * @see heartbeat_loop() For implementation
     */
    int heartbeat_interval_sec;
    
    /**
     * @var checkpoint_interval
     * @brief Save checkpoint every N symbols processed
     * 
     * Default: 100 symbols
     * 
     * Checkpoint Frequency Trade-off:
     *   
     *   Frequent (e.g., 10):
     *     + Fast recovery (10 symbols ~1ms lost work)
     *     - High I/O overhead (~50% time spent checkpointing)
     *   
     *   Moderate (e.g., 100):
     *     + Reasonable recovery (100 symbols ~10ms lost)
     *     + Low overhead (~17% time checkpointing)
     *   
     *   Infrequent (e.g., 1000):
     *     + Very low overhead (~2% time)
     *     - Slower recovery (1000 symbols ~100ms lost)
     * 
     * Project Goal: <5 second recovery
     *   Even 1000 symbols (~100ms lost work) easily meets this goal.
     *   The 6-second heartbeat timeout dominates recovery time.
     * 
     * Recommended: 100-1000
     *   Balances overhead and recovery time.
     * 
     * Special Values:
     *   0: Checkpointing disabled (not recommended for production)
     *   1: Checkpoint every symbol (extremely high overhead)
     * 
     * Thread Safety: Read-only after construction
     * 
     * @see save_progress_checkpoint() For checkpoint saving
     */
    int checkpoint_interval;  // Save checkpoint every N symbols
    
    /**
     * @fn WorkerConfig (Constructor)
     * @brief Initializes configuration with sensible defaults
     * 
     * Default Values:
     *   - controller_host: "localhost" (for development)
     *   - controller_port: 5000 (standard worker API port)
     *   - data_directory: "./data" (local data directory)
     *   - checkpoint_directory: "./checkpoints" (local checkpoints)
     *   - heartbeat_interval_sec: 2 (every 2 seconds)
     *   - checkpoint_interval: 100 (every 100 symbols)
     * 
     * These defaults work for:
     *   - Single-machine development
     *   - Testing and debugging
     *   - Initial deployment
     * 
     * For production, override with environment-specific values:
     *   
     *   WorkerConfig config;
     *   config.controller_host = "kh01";
     *   config.data_directory = "/nfs/data";
     *   Worker worker(config);
     */
    WorkerConfig() : controller_host("localhost"), 
                     controller_port(5000),
                     data_directory("./data"),
                     checkpoint_directory("./checkpoints"),
                     heartbeat_interval_sec(2),
                     checkpoint_interval(100) {}
};

/*******************************************************************************
 * SECTION 3: WORKER CLASS
 *******************************************************************************/

/**
 * @class Worker
 * @brief Worker node implementation for distributed backtesting
 * 
 * The Worker class encapsulates all functionality needed to participate in
 * the distributed backtesting cluster. Each worker is an independent agent
 * that connects to the controller, requests jobs, executes them, and reports
 * results.
 * 
 * Lifecycle:
 *   1. Construction: Initialize with configuration
 *   2. start(): Connect to controller, spawn threads
 *   3. Running: Process jobs, send heartbeats
 *   4. stop(): Graceful shutdown, join threads
 *   5. Destruction: Clean up resources
 * 
 * Thread Model:
 *   Main thread + 2 background threads:
 *   - worker_thread_: Processes jobs
 *   - heartbeat_thread_: Sends periodic heartbeats
 * 
 * State Machine:
 *   
 *   STOPPED → start() → RUNNING → stop() → STOPPED
 *   
 *   STOPPED state:
 *     - Not connected to controller
 *     - No threads running
 *     - Can call start()
 *   
 *   RUNNING state:
 *     - Connected to controller
 *     - Threads active
 *     - Processing jobs
 *     - Can call stop()
 * 
 * Thread Safety:
 *   - Public methods are thread-safe
 *   - Internal state protected by atomics and mutexes
 *   - Safe to call from multiple threads
 * 
 * Resource Management:
 *   - RAII: Resources cleaned up in destructor
 *   - stop() called in destructor if still running
 *   - Threads joined before destruction
 * 
 * Memory Footprint:
 *   - Base: ~1 KB (class overhead)
 *   - Per job: ~10 MB (price data, signals, portfolios)
 *   - Checkpoints: ~100 bytes on disk
 *   - Total: ~10-50 MB typical
 * 
 * Example Usage:
 *   
 *   WorkerConfig config;
 *   config.controller_host = "kh01";
 *   
 *   Worker worker(config);
 *   
 *   if (!worker.start()) {
 *       std::cerr << "Failed to start worker\n";
 *       return 1;
 *   }
 *   
 *   // Worker runs in background
 *   std::cout << "Worker running with ID: " << worker.get_worker_id() << "\n";
 *   
 *   // Wait for shutdown signal
 *   wait_for_sigint();
 *   
 *   // Graceful shutdown
 *   worker.stop();
 * 
 * @see WorkerConfig For configuration options
 * @see worker_loop() For job processing implementation
 * @see heartbeat_loop() For heartbeat implementation
 */
class Worker {
private:
    /***************************************************************************
     * PRIVATE MEMBERS - CONFIGURATION
     ***************************************************************************/
    
    /**
     * @var config_
     * @brief Worker configuration (read-only after construction)
     * 
     * Stores all configuration parameters passed to constructor.
     * Immutable during worker lifetime.
     * 
     * Thread Safety: Read-only, no synchronization needed
     */
    WorkerConfig config_;
    
    /**
     * @var worker_id_
     * @brief Unique identifier assigned by controller
     * 
     * The controller assigns a unique ID during registration.
     * Used in heartbeats and result messages for identification.
     * 
     * Value Range: 1-18446744073709551615
     * Typical: 1-1000 (for reasonable cluster sizes)
     * 
     * Special Values:
     *   0: Not yet registered (initial state)
     *   >0: Registered and assigned ID
     * 
     * Thread Safety: Written once during registration, then read-only
     */
    uint64_t worker_id_;
    
    /**
     * @var controller_socket_
     * @brief TCP socket connected to controller
     * 
     * This is the primary communication channel with the controller.
     * Used for:
     *   - Registration
     *   - Job requests
     *   - Heartbeats
     *   - Result submission
     * 
     * Lifecycle:
     *   - Created in connect_to_controller()
     *   - Used by worker_thread_ and heartbeat_thread_
     *   - Closed in disconnect_from_controller()
     * 
     * Value Range:
     *   -1: Not connected
     *   ≥0: Valid file descriptor
     * 
     * Thread Safety:
     *   Multiple threads access this socket:
     *   - worker_thread_: Sends job requests, receives jobs, sends results
     *   - heartbeat_thread_: Sends heartbeats
     *   
     *   Protection: Must synchronize socket access (send_message uses mutex)
     * 
     * @see connect_to_controller() For socket creation
     * @see send_message() For thread-safe sending
     */
    int controller_socket_;
    
    /**
     * @var running_
     * @brief Atomic flag indicating worker is operational
     * 
     * Controls worker lifecycle:
     *   false: Worker stopped or stopping
     *   true: Worker running and processing jobs
     * 
     * This flag is checked by both background threads:
     *   - worker_thread_: Exits when false
     *   - heartbeat_thread_: Exits when false
     * 
     * Atomic Type:
     *   std::atomic<bool> ensures:
     *   - Thread-safe reads and writes
     *   - Memory ordering guarantees
     *   - No race conditions
     * 
     * Lifecycle:
     *   Construction: false
     *   start(): Set to true
     *   stop(): Set to false
     *   Destruction: Should be false
     * 
     * Thread Safety: Atomic operations, no mutex needed
     * 
     * @see start() For setting to true
     * @see stop() For setting to false
     */
    std::atomic<bool> running_;
    
    /***************************************************************************
     * PRIVATE MEMBERS - JOB PROCESSING
     ***************************************************************************/
    
    /**
     * @var job_queue_
     * @brief Queue of jobs waiting to be processed
     * 
     * Jobs received from controller are enqueued here, then processed
     * by worker_thread_ in FIFO order.
     * 
     * Why a Queue?
     *   - Controller might send multiple jobs at once
     *   - Worker processes one at a time
     *   - Queue buffers incoming work
     * 
     * Typical Usage:
     *   In practice, queue usually has 0-1 jobs:
     *   - Worker requests job only when idle
     *   - Processes job immediately
     *   - Requests next job
     *   
     *   Queue depth > 1 rare (only during burst traffic)
     * 
     * Alternative: Single JobInfo (no queue)
     *   Since queue rarely has >1 job, could simplify to:
     *     std::optional<JobInfo> current_job_;
     *   
     *   Trade-off: Simpler but less flexible
     * 
     * Thread Safety: Protected by queue_mutex_
     * 
     * @see worker_loop() For job dequeuing
     * @see process_job() For job execution
     */
    std::queue<JobInfo> job_queue_;
    
    /**
     * @var queue_mutex_
     * @brief Mutex protecting job_queue_
     * 
     * Synchronizes access to job_queue_ between:
     *   - receive thread: Enqueues jobs
     *   - worker thread: Dequeues jobs
     * 
     * Lock Duration: Very short (microseconds)
     *   - push(): O(1)
     *   - pop(): O(1)
     *   - front(): O(1)
     * 
     * Deadlock Prevention:
     *   - Single mutex, no lock ordering issues
     *   - std::lock_guard ensures release
     *   - No nested locking
     * 
     * @see job_queue_ For protected resource
     */
    std::mutex queue_mutex_;
    
    /**
     * @var worker_thread_
     * @brief Background thread for job processing
     * 
     * This thread runs worker_loop(), which:
     *   1. Requests job from controller
     *   2. Loads price data
     *   3. Executes strategy
     *   4. Saves checkpoints
     *   5. Sends results
     *   6. Repeats until running_ = false
     * 
     * Thread Lifecycle:
     *   Created in start()
     *   Runs until running_ = false
     *   Joined in stop()
     * 
     * CPU Utilization:
     *   - Idle: ~0% (blocked waiting for jobs)
     *   - Processing: 80-100% (compute-bound)
     * 
     * @see worker_loop() For thread function
     */
    std::thread worker_thread_;
    
    /**
     * @var heartbeat_thread_
     * @brief Background thread for periodic heartbeats
     * 
     * This thread runs heartbeat_loop(), which:
     *   1. Sleeps for heartbeat_interval_sec
     *   2. Sends heartbeat to controller
     *   3. Repeats until running_ = false
     * 
     * Thread Lifecycle:
     *   Created in start()
     *   Runs until running_ = false
     *   Joined in stop()
     * 
     * CPU Utilization: <1% (mostly sleeping)
     * 
     * Independence:
     *   Heartbeats are independent of job processing.
     *   Even if worker is stuck processing a long job, heartbeats
     *   continue, preventing controller timeout.
     * 
     * @see heartbeat_loop() For thread function
     */
    std::thread heartbeat_thread_;
    
    /***************************************************************************
     * PRIVATE MEMBERS - DATA AND STRATEGY
     ***************************************************************************/
    
    /**
     * @var csv_loader_
     * @brief CSV file parser for loading historical price data
     * 
     * Loads price data from CSV files in data_directory.
     * 
     * Typical Usage:
     *   auto price_data = csv_loader_.load_csv_data(
     *       config_.data_directory + "/" + symbol + ".csv"
     *   );
     * 
     * Caching:
     *   CSVLoader may cache loaded data to avoid re-reading files.
     *   Useful if multiple strategies use same data.
     * 
     * @see CSVLoader For implementation details
     */
    CSVLoader csv_loader_;
    
    /***************************************************************************
     * PRIVATE MEMBERS - STATISTICS
     ***************************************************************************/
    
    /**
     * @var active_jobs_
     * @brief Count of jobs currently being processed
     * 
     * Typically 0 or 1 (worker processes one job at a time).
     * 
     * Atomic Type:
     *   Allows lock-free increments/decrements.
     *   Used for monitoring and debugging.
     * 
     * Updates:
     *   - Incremented when job starts
     *   - Decremented when job completes
     * 
     * Thread Safety: Atomic operations
     */
    std::atomic<uint32_t> active_jobs_;
    
    /**
     * @var completed_jobs_
     * @brief Total count of jobs completed by this worker
     * 
     * Monotonically increasing counter.
     * Used for:
     *   - Monitoring worker productivity
     *   - Debugging (log number of jobs processed)
     *   - Performance analysis
     * 
     * Lifetime:
     *   - Initialized to 0 in constructor
     *   - Incremented on each job completion
     *   - Never decremented
     * 
     * Thread Safety: Atomic operations
     */
    std::atomic<uint32_t> completed_jobs_;
    
    /***************************************************************************
     * PRIVATE MEMBERS - CHECKPOINTING
     ***************************************************************************/
    
    /**
     * @var checkpoint_manager_
     * @brief Manager for saving/loading checkpoint files
     * 
     * Handles all checkpoint persistence operations.
     * 
     * Initialization Order:
     *   Must be initialized AFTER statistics members because
     *   C++ initializes members in declaration order, not
     *   initializer list order.
     * 
     * Thread Safety: CheckpointManager is internally thread-safe
     * 
     * @see CheckpointManager For implementation
     */
    CheckpointManager checkpoint_manager_;
    
public:
    /***************************************************************************
     * PUBLIC INTERFACE - LIFECYCLE
     ***************************************************************************/
    
    /**
     * @fn Worker (Constructor)
     * @brief Constructs worker with specified configuration
     * 
     * The constructor initializes all member variables but does NOT:
     *   - Connect to controller
     *   - Start threads
     *   - Allocate network resources
     * 
     * This separation (construction vs initialization) follows best practices:
     *   - Constructor cannot fail (no exceptions)
     *   - start() can fail and report errors
     *   - Allows configuration validation before starting
     * 
     * Initialization Order:
     *   Members are initialized in DECLARATION order, not initializer list order.
     *   This is why checkpoint_manager_ is declared last (depends on config_).
     * 
     * @param config Configuration for this worker (copied)
     * 
     * @throws None - Constructor never throws
     * 
     * @sideeffects
     *   - Creates checkpoint directory via CheckpointManager
     *   - Initializes CSVLoader with data directory
     * 
     * Example:
     *   
     *   WorkerConfig config;
     *   config.controller_host = "kh01";
     *   config.checkpoint_interval = 200;
     *   
     *   Worker worker(config);  // Constructed, not yet running
     *   worker.start();         // Now running
     * 
     * @see start() For actual initialization
     */
    explicit Worker(const WorkerConfig& config = WorkerConfig());
    
    /**
     * @fn ~Worker (Destructor)
     * @brief Cleans up worker resources
     * 
     * The destructor ensures graceful shutdown:
     *   1. Call stop() if still running
     *   2. Join worker and heartbeat threads
     *   3. Close controller socket
     *   4. Release all resources
     * 
     * RAII Principle:
     *   Resource Acquisition Is Initialization
     *   - Constructor acquires resources (or defers to start())
     *   - Destructor releases resources (guaranteed)
     *   - Exception-safe cleanup
     * 
     * Thread Joining:
     *   Destructor blocks until all threads finish.
     *   This prevents dangling thread references.
     *   
     *   Typical join time: <100ms
     *   Worst case: Worker thread finishing long job
     * 
     * @throws None - Destructors should never throw
     * 
     * @sideeffects
     *   - Calls stop() to shutdown gracefully
     *   - Joins background threads (may block)
     *   - Closes network connections
     * 
     * Example:
     *   
     *   {
     *       Worker worker(config);
     *       worker.start();
     *       // ... worker runs ...
     *   } // Destructor called here
     *     // - stop() called
     *     // - Threads joined
     *     // - Socket closed
     *   // Worker fully cleaned up
     * 
     * @see stop() For shutdown logic
     */
    ~Worker();
    
    /**
     * @fn start
     * @brief Initializes and starts the worker
     * 
     * This method performs the actual initialization:
     *   1. Connect to controller
     *   2. Register with controller (get worker_id)
     *   3. Start worker thread
     *   4. Start heartbeat thread
     *   5. Set running_ = true
     * 
     * Startup Sequence:
     *   
     *   connect_to_controller()
     *     ↓
     *   register_with_controller()
     *     ↓
     *   Spawn worker_thread_ → worker_loop()
     *     ↓
     *   Spawn heartbeat_thread_ → heartbeat_loop()
     *     ↓
     *   Set running_ = true
     *     ↓
     *   Return true
     * 
     * Error Handling:
     *   If any step fails:
     *   - Log error message
     *   - Clean up partial initialization
     *   - Return false
     *   - Worker remains in STOPPED state
     * 
     * @return true if started successfully, false on error
     * 
     * @throws None - All errors returned via boolean
     * 
     * @sideeffects
     *   - Connects to controller via TCP
     *   - Spawns two background threads
     *   - Begins sending heartbeats
     *   - Begins requesting jobs
     * 
     * @threadsafety
     *   Not thread-safe. Call only from main thread.
     *   Do not call while worker is already running.
     * 
     * Example:
     *   
     *   Worker worker(config);
     *   
     *   if (!worker.start()) {
     *       std::cerr << "Startup failed:\n";
     *       std::cerr << "  - Check controller is running\n";
     *       std::cerr << "  - Check network connectivity\n";
     *       std::cerr << "  - Check firewall rules\n";
     *       return 1;
     *   }
     *   
     *   std::cout << "Worker started with ID " 
     *             << worker.get_worker_id() << "\n";
     * 
     * Common Failures:
     *   
     *   "Connection refused":
     *     - Controller not running
     *     - Wrong host/port
     *     - Firewall blocking
     *   
     *   "Registration failed":
     *     - Controller full (max workers)
     *     - Protocol mismatch
     *     - Authentication failed (if implemented)
     *   
     *   "Thread creation failed":
     *     - System resource exhaustion
     *     - Out of memory
     * 
     * @see stop() For shutdown
     * @see is_running() For checking state
     */
    bool start();
    
    /**
     * @fn stop
     * @brief Gracefully stops the worker
     * 
     * Shutdown sequence:
     *   1. Set running_ = false (signals threads to exit)
     *   2. Worker thread sees flag, exits worker_loop()
     *   3. Heartbeat thread sees flag, exits heartbeat_loop()
     *   4. Join worker_thread_ (wait for completion)
     *   5. Join heartbeat_thread_ (wait for completion)
     *   6. Disconnect from controller
     * 
     * Graceful vs Abrupt:
     *   
     *   Graceful (this implementation):
     *     - Finish current job before exiting
     *     - Save final checkpoint
     *     - Send final heartbeat
     *     - Notify controller
     *     
     *   Abrupt (alternative):
     *     - Immediately terminate threads
     *     - Don't finish current job
     *     - Don't save checkpoint
     *     - Controller detects via timeout
     * 
     * Current Job Handling:
     *   If worker is processing a job when stop() is called:
     *   - Worker finishes the job (may take seconds)
     *   - Sends result to controller
     *   - Then exits
     *   
     *   Timeout: None (waits indefinitely for job completion)
     *   Alternative: Add timeout, abort job if too long
     * 
     * Idempotency:
     *   Safe to call stop() multiple times:
     *   - First call: Initiates shutdown
     *   - Subsequent calls: No-op (already stopped)
     * 
     * @throws None - Never throws
     * 
     * @sideeffects
     *   - Sets running_ = false
     *   - Joins background threads (blocks until they finish)
     *   - Closes controller connection
     * 
     * @threadsafety Thread-safe, can call from any thread
     * 
     * @performance
     *   Typical: <100ms (threads exit quickly)
     *   Worst case: Seconds (if job still processing)
     * 
     * Example:
     *   
     *   Worker worker(config);
     *   worker.start();
     *   
     *   // Run for a while...
     *   std::this_thread::sleep_for(std::chrono::minutes(10));
     *   
     *   // Graceful shutdown
     *   std::cout << "Stopping worker...\n";
     *   worker.stop();
     *   std::cout << "Worker stopped cleanly\n";
     * 
     * Signal Handler Integration:
     *   
     *   std::atomic<bool> shutdown_requested(false);
     *   Worker* global_worker = nullptr;
     *   
     *   void signal_handler(int) {
     *       shutdown_requested = true;
     *       if (global_worker) {
     *           global_worker->stop();
     *       }
     *   }
     *   
     *   int main() {
     *       signal(SIGINT, signal_handler);
     *       
     *       Worker worker(config);
     *       global_worker = &worker;
     *       worker.start();
     *       
     *       while (!shutdown_requested && worker.is_running()) {
     *           std::this_thread::sleep_for(std::chrono::seconds(1));
     *       }
     *       
     *       worker.stop();
     *   }
     * 
     * @see start() For initialization
     */
    void stop();
    
    /**
     * @fn is_running
     * @brief Checks if worker is currently operational
     * 
     * @return true if worker is running, false if stopped or stopping
     * 
     * @const Method doesn't modify state
     * @threadsafety Thread-safe via atomic
     * @performance O(1), no locks
     * 
     * Example:
     *   
     *   Worker worker(config);
     *   std::cout << worker.is_running() << "\n";  // false
     *   
     *   worker.start();
     *   std::cout << worker.is_running() << "\n";  // true
     *   
     *   worker.stop();
     *   std::cout << worker.is_running() << "\n";  // false
     */
    bool is_running() const { return running_; }
    
    /**
     * @fn get_worker_id
     * @brief Returns worker's unique identifier
     * 
     * The worker ID is assigned by the controller during registration.
     * It's used to identify this worker in heartbeats, logs, and results.
     * 
     * @return Worker ID (0 if not yet registered, >0 otherwise)
     * 
     * @const Method doesn't modify state
     * @threadsafety Thread-safe (read-only after registration)
     * @performance O(1)
     * 
     * Example:
     *   
     *   Worker worker(config);
     *   std::cout << worker.get_worker_id() << "\n";  // 0 (not registered)
     *   
     *   worker.start();
     *   std::cout << worker.get_worker_id() << "\n";  // e.g., 5 (assigned by controller)
     */
    uint64_t get_worker_id() const { return worker_id_; }
    
private:
    /***************************************************************************
     * PRIVATE METHODS - CONNECTION MANAGEMENT
     ***************************************************************************/
    
    /**
     * @fn connect_to_controller
     * @brief Establishes TCP connection to controller
     * 
     * Creates socket, resolves hostname, and connects to controller's
     * worker API port.
     * 
     * Connection Flow:
     *   1. Create TCP socket
     *   2. Resolve controller_host to IP address
     *   3. Connect to controller_host:controller_port
     *   4. Store socket in controller_socket_
     * 
     * Retry Logic:
     *   Current: Single attempt
     *   Enhancement: Retry with exponential backoff
     * 
     * @return true if connected, false on error
     * 
     * @see disconnect_from_controller() For cleanup
     */
    bool connect_to_controller();
    
    /**
     * @fn disconnect_from_controller
     * @brief Closes connection to controller
     * 
     * Cleanly closes the TCP socket and sets controller_socket_ = -1.
     * 
     * @sideeffects Closes controller_socket_
     */
    void disconnect_from_controller();
    
    /***************************************************************************
     * PRIVATE METHODS - BACKGROUND THREADS
     ***************************************************************************/
    
    /**
     * @fn worker_loop
     * @brief Main job processing loop (runs in worker_thread_)
     * 
     * Pseudocode:
     *   
     *   while (running_) {
     *       job = request_job_from_controller();
     *       if (job available) {
     *           process_job(job);
     *       } else {
     *           sleep(1 second);  // No work available
     *       }
     *   }
     * 
     * Pull Model:
     *   Worker actively requests work instead of passively waiting.
     *   Benefits:
     *   + Natural load balancing
     *   + Worker controls pace
     *   + Simple protocol
     * 
     * @see process_job() For job execution
     */
    void worker_loop();
    
    /**
     * @fn heartbeat_loop
     * @brief Periodic heartbeat sender (runs in heartbeat_thread_)
     * 
     * Pseudocode:
     *   
     *   while (running_) {
     *       sleep(heartbeat_interval_sec);
     *       send_heartbeat();
     *   }
     * 
     * Timing Precision:
     *   Uses std::this_thread::sleep_for() which may drift slightly.
     *   For this application, +/- 100ms is acceptable.
     * 
     * @see send_heartbeat() For heartbeat sending
     */
    void heartbeat_loop();
    
    /***************************************************************************
     * PRIVATE METHODS - REGISTRATION
     ***************************************************************************/
    
    /**
     * @fn register_with_controller
     * @brief Registers worker and receives worker_id
     * 
     * Protocol:
     *   Worker → Controller: REGISTER message
     *   Controller → Worker: REGISTER_ACK with worker_id
     * 
     * @return true if registered, false on error
     * 
     * @sideeffects Sets worker_id_ on success
     */
    bool register_with_controller();
    
    /***************************************************************************
     * PRIVATE METHODS - JOB PROCESSING
     ***************************************************************************/
    
    /**
     * @fn process_job
     * @brief Executes a backtest job (without checkpointing)
     * 
     * Simple job processing without fault tolerance.
     * Use process_job_with_checkpoint() for production.
     * 
     * @param job Job specification from controller
     * 
     * @see process_job_with_checkpoint() For fault-tolerant version
     */
    void process_job(const JobInfo& job);
    
    /**
     * @fn process_job_with_checkpoint
     * @brief Executes job with checkpoint-based fault tolerance
     * 
     * Algorithm:
     *   1. Attempt to load checkpoint
     *   2. Load price data
     *   3. Create strategy instance
     *   4. Execute backtest (with periodic checkpointing)
     *   5. Calculate metrics
     *   6. Send result to controller
     *   7. Delete checkpoint on success
     * 
     * @param job Job specification
     * 
     * @see Checkpoint For checkpoint structure
     */
    void process_job_with_checkpoint(const JobInfo& job);
    
    /***************************************************************************
     * PRIVATE METHODS - COMMUNICATION
     ***************************************************************************/
    
    /**
     * @fn send_message
     * @brief Sends message to controller
     * 
     * Thread-safe wrapper around socket send().
     * 
     * @param msg Message to send
     * @return true if sent successfully, false on error
     * 
     * @threadsafety Thread-safe (uses internal mutex)
     */
    bool send_message(const Message& msg);
    
    /**
     * @fn receive_message
     * @brief Receives message from controller
     * 
     * Blocks until message received or error occurs.
     * 
     * @return Pointer to message, or nullptr on error
     * 
     * @threadsafety Not thread-safe (call from one thread only)
     */
    std::unique_ptr<Message> receive_message();
    
    /**
     * @fn send_heartbeat
     * @brief Sends heartbeat to controller
     * 
     * Called by heartbeat_thread_ every heartbeat_interval_sec.
     * 
     * @return true if sent, false on error
     * 
     * @see heartbeat_loop() For caller
     */
    bool send_heartbeat();
    
    /**
     * @fn send_job_result
     * @brief Sends completed job result to controller
     * 
     * Called after job finishes (success or failure).
     * 
     * @param result Job result with metrics
     * @return true if sent, false on error
     */
    bool send_job_result(const JobResult& result);
    
    /***************************************************************************
     * PRIVATE METHODS - STRATEGY
     ***************************************************************************/
    
    /**
     * @fn create_strategy
     * @brief Factory method for creating strategy instances
     * 
     * Creates appropriate strategy based on type string.
     * 
     * Supported Strategies:
     *   - "SMA": Simple Moving Average Crossover
     *   - "RSI": Relative Strength Index (if implemented)
     *   - "MACD": Moving Average Convergence Divergence (if implemented)
     * 
     * @param strategy_type Strategy type identifier
     * @return Unique pointer to strategy, or nullptr if unknown type
     * 
     * Example:
     *   
     *   auto strategy = create_strategy("SMA");
     *   if (strategy) {
     *       auto signals = strategy->generate_signals(price_data);
     *   }
     */
    std::unique_ptr<Strategy> create_strategy(const std::string& strategy_type);
    
    /***************************************************************************
     * PRIVATE METHODS - CHECKPOINTING
     ***************************************************************************/
    
    /**
     * @fn should_checkpoint
     * @brief Determines if checkpoint should be saved
     * 
     * Check if we've processed enough symbols to warrant a checkpoint.
     * 
     * @param symbols_processed Count of symbols processed so far
     * @return true if should save checkpoint, false otherwise
     * 
     * Example:
     *   
     *   checkpoint_interval = 100
     *   
     *   should_checkpoint(0)   → false
     *   should_checkpoint(99)  → false
     *   should_checkpoint(100) → true
     *   should_checkpoint(101) → false
     *   should_checkpoint(200) → true
     */
    bool should_checkpoint(uint64_t symbols_processed) const;
    
    /**
     * @fn create_checkpoint_from_state
     * @brief Constructs Checkpoint structure from current state
     * 
     * Helper function to package backtest state into Checkpoint.
     * 
     * @param job_id Job identifier
     * @param symbol Stock symbol
     * @param symbols_processed Count of symbols completed
     * @param bar_index Current bar index
     * @param cash Current cash balance
     * @param shares Current shares held
     * @param portfolio_value Total portfolio value
     * @param last_date Last processed date string
     * 
     * @return Checkpoint structure ready to save
     * 
     * Example:
     *   
     *   auto cp = create_checkpoint_from_state(
     *       42, "AAPL", 1, 100, 90000.0, 50, 95000.0, "2024-12-01"
     *   );
     *   
     *   checkpoint_manager_.save_checkpoint(cp);
     */
    Checkpoint create_checkpoint_from_state(uint64_t job_id, const std::string& symbol,
                                           uint64_t symbols_processed, uint64_t bar_index,
                                           double cash, int shares, double portfolio_value,
                                           const std::string& last_date);
};

} // namespace backtesting

/*******************************************************************************
 * SECTION 4: CONFIGURATION GUIDE
 *******************************************************************************/

/*
 * WORKERCONGIG PARAMETER TUNING
 * ==============================
 * 
 * controller_host:
 *   
 *   Development: "localhost"
 *   Testing: "kh01" or "192.168.1.100"
 *   Production: "controller.production.internal"
 *   
 *   With DNS failover:
 *     controller.internal → current Raft leader
 *     DNS updated automatically on leader change
 * 
 * controller_port:
 *   
 *   Standard: 5000
 *   
 *   If port conflict, use alternative:
 *     5001, 5100, 8000, etc.
 *   
 *   Coordinate with controller configuration!
 * 
 * data_directory:
 *   
 *   Development: "./data"
 *   
 *   Production (NFS):
 *     "/nfs/backtesting/data"
 *     Ensure NFS mounted on all worker nodes
 *   
 *   Production (Local):
 *     "/var/lib/backtesting/data"
 *     Pre-replicate data to all workers:
 *       rsync -avz data/ kh04:/var/lib/backtesting/data/
 * 
 * checkpoint_directory:
 *   
 *   Development: "./checkpoints"
 *   
 *   Production:
 *     "/var/lib/backtesting/checkpoints"
 *     Or NFS if workers need mobility
 * 
 * heartbeat_interval_sec:
 *   
 *   Default: 2 seconds (recommended)
 *   
 *   Shorter (1 second):
 *     + Faster failure detection (3 seconds)
 *     + More responsive system
 *     - 2x network overhead
 *   
 *   Longer (5 seconds):
 *     + Less overhead
 *     - Slower detection (15 seconds)
 *     - May not meet <5s recovery goal
 * 
 * checkpoint_interval:
 *   
 *   Default: 100 symbols
 *   
 *   Frequent (10):
 *     + Recovery time ~1ms
 *     - Overhead ~50%
 *   
 *   Moderate (100):
 *     + Recovery time ~10ms
 *     + Overhead ~17%
 *   
 *   Infrequent (1000):
 *     + Recovery time ~100ms
 *     + Overhead ~2%
 *   
 *   Recommendation: 100-1000 for this project
 */

/*******************************************************************************
 * SECTION 5: LIFECYCLE MANAGEMENT
 *******************************************************************************/

/*
 * CONSTRUCTION
 * ============
 * 
 * Worker construction is lightweight:
 * 
 * Code:
 *   Worker worker(config);
 * 
 * What Happens:
 *   1. Copy config to config_
 *   2. Initialize worker_id_ = 0
 *   3. Initialize controller_socket_ = -1
 *   4. Initialize running_ = false
 *   5. Initialize statistics = 0
 *   6. Construct csv_loader_ with data_directory
 *   7. Construct checkpoint_manager_ with checkpoint_directory
 * 
 * What Doesn't Happen:
 *   - No network connections
 *   - No threads started
 *   - No resources allocated
 * 
 * This allows configuration validation before resource allocation.
 * 
 * 
 * STARTUP SEQUENCE
 * ================
 * 
 * Code:
 *   if (!worker.start()) {
 *       // Handle error
 *   }
 * 
 * Internal Sequence:
 *   
 *   1. connect_to_controller()
 *      - Create socket
 *      - Connect to controller_host:controller_port
 *      - On failure: Log error, return false
 *   
 *   2. register_with_controller()
 *      - Send REGISTER message
 *      - Receive REGISTER_ACK with worker_id
 *      - Store worker_id_
 *      - On failure: Close socket, return false
 *   
 *   3. Spawn worker_thread_
 *      - Start worker_loop() in background
 *      - On failure: Close socket, return false
 *   
 *   4. Spawn heartbeat_thread_
 *      - Start heartbeat_loop() in background
 *      - On failure: Stop worker thread, close socket, return false
 *   
 *   5. Set running_ = true
 *      - Signals threads to begin work
 *   
 *   6. Return true
 *      - Worker now operational
 * 
 * Timeline:
 *   T+0ms:   Call start()
 *   T+10ms:  Socket connected
 *   T+20ms:  Registered with controller
 *   T+25ms:  Threads spawned
 *   T+30ms:  Worker operational
 * 
 * 
 * SHUTDOWN PROCEDURE
 * ==================
 * 
 * Code:
 *   worker.stop();
 * 
 * Internal Sequence:
 *   
 *   1. Set running_ = false
 *      - Signals both threads to exit
 *   
 *   2. Wait for worker_thread_ to finish
 *      - Thread exits worker_loop()
 *      - May finish current job first (can take seconds)
 *      - Join returns when thread completes
 *   
 *   3. Wait for heartbeat_thread_ to finish
 *      - Thread wakes up, sees running_ = false, exits
 *      - Typically < heartbeat_interval_sec
 *   
 *   4. disconnect_from_controller()
 *      - Close socket
 *      - Set controller_socket_ = -1
 *   
 *   5. Return
 *      - Worker now stopped
 * 
 * Timeline:
 *   T+0ms:     Call stop(), set running_ = false
 *   T+0-5000ms: Worker thread finishes current job
 *   T+5000ms:  worker_thread_ joined
 *   T+5000-7000ms: Heartbeat thread wakes and exits
 *   T+7000ms:  heartbeat_thread_ joined
 *   T+7001ms:  Socket closed
 *   T+7002ms:  Return
 * 
 * Worst Case: ~7 seconds (if job takes 5 seconds + heartbeat interval)
 * Typical: <1 second (no job in progress)
 */

/*******************************************************************************
 * SECTION 6: THREAD ARCHITECTURE
 *******************************************************************************/

/*
 * WORKER THREAD RESPONSIBILITIES
 * ===============================
 * 
 * The worker thread (worker_loop) is the main computational thread:
 * 
 * Main Loop:
 *   
 *   while (running_) {
 *       // Request job from controller
 *       send(REQUEST_JOB);
 *       job = receive();
 *       
 *       if (job is NO_JOBS_AVAILABLE) {
 *           sleep(1 second);
 *           continue;
 *       }
 *       
 *       // Process job
 *       active_jobs_++;
 *       
 *       price_data = load_data(job.symbol);
 *       strategy = create_strategy(job.strategy_type);
 *       result = strategy->backtest(price_data);
 *       
 *       send_job_result(result);
 *       
 *       completed_jobs_++;
 *       active_jobs_--;
 *   }
 * 
 * CPU Utilization Pattern:
 *   
 *   Timeline of a typical iteration:
 *   
 *   0-10ms:   Request job (network I/O)
 *   10-20ms:  Load price data (disk I/O)
 *   20-120ms: Execute strategy (CPU-bound) ← 80-100% CPU
 *   120-130ms: Send result (network I/O)
 *   
 *   Total: 130ms per job
 *   CPU time: 100ms / 130ms = 77% utilization
 * 
 * If no jobs available:
 *   - Sleep 1 second
 *   - CPU: 0%
 *   - Wake up, retry
 * 
 * Why Not Busy-Wait?
 *   
 *   Bad:
 *     while (running_) {
 *         job = request_job();
 *         if (!job) continue;  // Immediate retry!
 *         process(job);
 *     }
 *   
 *   Problem: Hammers controller with requests
 *   Network: 1000+ requests/second
 *   CPU: 100% (spinning)
 *   
 *   Good:
 *     while (running_) {
 *         job = request_job();
 *         if (!job) {
 *             sleep(1 second);  // Back off
 *         } else {
 *             process(job);
 *         }
 *     }
 * 
 * HEARTBEAT THREAD RESPONSIBILITIES
 * ==================================
 * 
 * The heartbeat thread is simple but critical:
 * 
 * Main Loop:
 *   
 *   while (running_) {
 *       sleep(heartbeat_interval_sec);
 *       
 *       if (!send_heartbeat()) {
 *           Logger::warning("Heartbeat failed");
 *           // Continue anyway, will retry
 *       }
 *   }
 * 
 * CPU Utilization: <1% (mostly sleeping)
 * 
 * Independence from Worker Thread:
 *   
 *   Critical Design Decision:
 *   Heartbeats must continue even if worker thread is stuck!
 *   
 *   Scenario:
 *     Worker processing very long job (30 seconds)
 *     Without separate thread: No heartbeat for 30 seconds
 *     Controller thinks worker dead, reassigns job!
 *     
 *   With separate thread:
 *     Worker processing long job (30 seconds)
 *     Heartbeat thread: Sends heartbeat every 2 seconds
 *     Controller: Sees heartbeats, knows worker alive
 *     Job completes successfully
 * 
 * THREAD SYNCHRONIZATION
 * ======================
 * 
 * Shared State:
 *   
 *   running_ (atomic):
 *     - Written by main thread (start/stop)
 *     - Read by worker and heartbeat threads
 *     - No mutex needed (atomic)
 *   
 *   controller_socket_ (int):
 *     - Written by main thread (connect/disconnect)
 *     - Read by worker and heartbeat threads
 *     - Protected by internal mutex in send_message()
 *   
 *   job_queue_ (std::queue):
 *     - Written by worker thread (enqueue from controller)
 *     - Read by worker thread (dequeue for processing)
 *     - Protected by queue_mutex_
 *   
 *   Statistics (atomics):
 *     - Updated by worker thread
 *     - Read by anyone (monitoring)
 *     - No mutex needed (atomic)
 * 
 * Lock Hierarchy:
 *   No nested locking, so no deadlock risk.
 */

/*******************************************************************************
 * SECTION 7: JOB PROCESSING WORKFLOW
 *******************************************************************************/

/*
 * DETAILED JOB EXECUTION FLOW
 * ============================
 * 
 * Step-by-Step Process:
 * 
 * 1. JOB ACQUISITION
 *    
 *    Worker → Controller: REQUEST_JOB message
 *    Controller → Worker: JOB_ASSIGNED message with JobInfo
 *    
 *    JobInfo contains:
 *      - job_id: 42
 *      - symbol: "AAPL"
 *      - strategy_type: "SMA"
 *      - short_window: 50
 *      - long_window: 200
 *      - start_date: "2020-01-01"
 *      - end_date: "2024-12-31"
 * 
 * 2. CHECKPOINT RECOVERY
 *    
 *    bool has_checkpoint = checkpoint_manager_.load_checkpoint(job_id, cp);
 *    
 *    If checkpoint exists:
 *      resume_index = cp.last_processed_index
 *      cash = cp.current_cash
 *      shares = cp.current_shares
 *    Else:
 *      resume_index = 0
 *      cash = initial_capital
 *      shares = 0
 * 
 * 3. DATA LOADING
 *    
 *    std::string csv_path = data_directory + "/" + symbol + ".csv";
 *    auto price_data = csv_loader_.load_csv_data(csv_path);
 *    
 *    Typical: 1000-5000 bars (several years of daily data)
 *    Load time: 5-20ms from disk, 1-5ms from NFS cache
 * 
 * 4. STRATEGY CREATION
 *    
 *    auto strategy = create_strategy(job.strategy_type);
 *    strategy->set_parameters(job.parameters);
 * 
 * 5. BACKTEST EXECUTION
 *    
 *    for (size_t i = resume_index; i < price_data.size(); ++i) {
 *        // Generate signal
 *        signal = strategy->generate_signal(price_data[i]);
 *        
 *        // Execute trade
 *        if (signal == BUY) {
 *            execute_buy();
 *        } else if (signal == SELL) {
 *            execute_sell();
 *        }
 *        
 *        // Update portfolio
 *        portfolio_value = cash + shares * price;
 *        
 *        // Periodic checkpointing
 *        if (i % checkpoint_interval == 0) {
 *            save_checkpoint(i, cash, shares, portfolio_value);
 *        }
 *    }
 * 
 * 6. METRICS CALCULATION
 *    
 *    result.total_return = (final_value - initial_capital) / initial_capital;
 *    result.sharpe_ratio = calculate_sharpe(returns);
 *    result.max_drawdown = calculate_max_drawdown(values);
 *    result.num_trades = count_trades();
 * 
 * 7. RESULT SUBMISSION
 *    
 *    Worker → Controller: JOB_RESULT message with metrics
 *    Controller → Worker: RESULT_ACK
 * 
 * 8. CHECKPOINT CLEANUP
 *    
 *    if (result.success) {
 *        checkpoint_manager_.delete_checkpoint(job_id);
 *    }
 * 
 * 9. STATISTICS UPDATE
 *    
 *    completed_jobs_++;
 *    active_jobs_--;
 * 
 * Total Time: 10-100ms typical
 * 
 * 
 * ERROR HANDLING AT EACH STEP
 * ============================
 * 
 * Data Load Failure:
 *   - File doesn't exist
 *   - Corrupted CSV
 *   - Permission denied
 *   
 *   Handling: Log error, send failure result, continue to next job
 * 
 * Strategy Execution Failure:
 *   - Insufficient data
 *   - Calculation error
 *   - Exception thrown
 *   
 *   Handling: Catch exception, log error, send failure result
 * 
 * Checkpoint Failure:
 *   - Disk full
 *   - Permission denied
 *   
 *   Handling: Log warning, continue without checkpoint
 * 
 * Result Send Failure:
 *   - Connection lost
 *   - Controller down
 *   
 *   Handling: Retry once, then discard job (controller will reassign)
 */

/*******************************************************************************
 * SECTION 8: HEARTBEAT PROTOCOL
 *******************************************************************************/

/*
 * PURPOSE AND FREQUENCY
 * ======================
 * 
 * Heartbeats serve as a liveness proof:
 *   "I'm alive and healthy, don't reassign my jobs"
 * 
 * Frequency: Every 2 seconds (configurable)
 * 
 * Message Content:
 *   - Worker ID
 *   - Current job (if any)
 *   - Statistics (jobs completed)
 *   - Timestamp
 * 
 * Controller Processing:
 *   
 *   On each heartbeat received:
 *   1. Update worker's last_heartbeat_time
 *   2. Reset timeout counter
 *   3. Update worker statistics
 * 
 * FAILURE DETECTION
 * =================
 * 
 * Controller's Perspective:
 *   
 *   Worker last heartbeat: 10:00:00
 *   Current time: 10:00:07
 *   Timeout threshold: 6 seconds (3 × 2 seconds)
 *   
 *   Time since last heartbeat: 7 seconds > 6 seconds
 *   → Worker considered DEAD
 *   → Reassign all of worker's jobs
 * 
 * False Positives:
 *   
 *   Network congestion:
 *     - Heartbeat delayed by network
 *     - Controller thinks worker dead
 *     - Reassigns job while worker still processing
 *     
 *     Mitigation: Use generous timeout (3× interval)
 *   
 *   Clock skew:
 *     - Worker and controller clocks differ
 *     - Timeout calculated incorrectly
 *     
 *     Mitigation: Use NTP to sync clocks
 * 
 * Heartbeat Storm:
 *   
 *   100 workers × 0.5 Hz = 50 heartbeats/second
 *   
 *   This is manageable for controller.
 *   If scaling to 1000+ workers, batch heartbeats or use UDP.
 */

/*******************************************************************************
 * SECTION 9: USAGE EXAMPLES
 *******************************************************************************/

/*
 * EXAMPLE 1: Basic Worker Deployment
 * -----------------------------------
 * 
 * Start a single worker connecting to local controller:
 * 
 * Code:
 * 
 *   #include "worker/worker.h"
 *   #include <signal.h>
 *   
 *   std::atomic<bool> shutdown_requested(false);
 *   
 *   void signal_handler(int) {
 *       shutdown_requested = true;
 *   }
 *   
 *   int main() {
 *       signal(SIGINT, signal_handler);
 *       
 *       // Create worker with default configuration
 *       backtesting::WorkerConfig config;
 *       config.controller_host = "localhost";
 *       config.controller_port = 5000;
 *       
 *       backtesting::Worker worker(config);
 *       
 *       // Start worker
 *       if (!worker.start()) {
 *           std::cerr << "Failed to start worker\n";
 *           return 1;
 *       }
 *       
 *       std::cout << "Worker started with ID: " 
 *                 << worker.get_worker_id() << "\n";
 *       std::cout << "Press Ctrl+C to stop\n";
 *       
 *       // Run until shutdown
 *       while (!shutdown_requested && worker.is_running()) {
 *           std::this_thread::sleep_for(std::chrono::seconds(1));
 *       }
 *       
 *       // Graceful shutdown
 *       std::cout << "Shutting down worker...\n";
 *       worker.stop();
 *       std::cout << "Worker stopped\n";
 *       
 *       return 0;
 *   }
 * 
 * Compilation:
 *   g++ -std=c++17 -pthread worker_main.cpp -o worker \
 *       -I../include -L../lib -lbacktesting
 * 
 * Execution:
 *   $ ./worker
 *   Worker started with ID: 1
 *   Press Ctrl+C to stop
 *   ^C
 *   Shutting down worker...
 *   Worker stopped
 *   $
 * 
 * 
 * EXAMPLE 2: Multi-Worker Cluster
 * --------------------------------
 * 
 * Deploy 8 workers on Khoury cluster:
 * 
 * deploy_workers.sh:
 * 
 *   #!/bin/bash
 *   
 *   CONTROLLER="kh01"
 *   WORKER_NODES="kh04 kh05 kh06 kh07 kh08 kh09 kh10 kh11"
 *   
 *   echo "Deploying workers to 8 nodes..."
 *   
 *   for node in $WORKER_NODES; do
 *       echo "Starting worker on $node..."
 *       
 *       ssh $node "cd ~/cs6650-project && \
 *           nohup ./worker \
 *               --controller $CONTROLLER \
 *               --port 5000 \
 *               --data-dir /nfs/data \
 *               --checkpoint-dir ~/checkpoints \
 *           > worker.log 2>&1 &"
 *       
 *       echo "  → $node started"
 *   done
 *   
 *   echo "Waiting for workers to register..."
 *   sleep 5
 *   
 *   echo "Checking worker status..."
 *   for node in $WORKER_NODES; do
 *       echo -n "  $node: "
 *       ssh $node "pgrep -x worker > /dev/null && echo 'RUNNING' || echo 'DEAD'"
 *   done
 * 
 * Usage:
 *   $ ./deploy_workers.sh
 *   Deploying workers to 8 nodes...
 *   Starting worker on kh04...
 *     → kh04 started
 *   Starting worker on kh05...
 *     → kh05 started
 *   ...
 *   Checking worker status...
 *     kh04: RUNNING
 *     kh05: RUNNING
 *     ...
 * 
 * 
 * EXAMPLE 3: Crash Recovery Demonstration
 * ----------------------------------------
 * 
 * Simulate crash and verify recovery:
 * 
 * test_crash_recovery.sh:
 * 
 *   #!/bin/bash
 *   
 *   # Start worker
 *   ./worker --controller localhost &
 *   WORKER_PID=$!
 *   
 *   echo "Worker started with PID $WORKER_PID"
 *   sleep 5
 *   
 *   # Submit long job
 *   ./client submit_job --symbol AAPL --bars 10000
 *   
 *   # Wait for checkpoint to be created
 *   echo "Waiting for checkpoint..."
 *   sleep 10
 *   
 *   # Simulate crash
 *   echo "Simulating crash (killing worker)..."
 *   kill -9 $WORKER_PID
 *   
 *   # Check checkpoint exists
 *   ls -lh ./checkpoints/
 *   
 *   # Start new worker (will resume from checkpoint)
 *   echo "Starting replacement worker..."
 *   ./worker --controller localhost &
 *   NEW_PID=$!
 *   
 *   echo "New worker PID: $NEW_PID"
 *   echo "Check logs for 'Resuming from checkpoint' message"
 *   
 *   tail -f worker.log
 * 
 * Expected Output:
 *   Worker started with PID 12345
 *   Waiting for checkpoint...
 *   Simulating crash (killing worker)...
 *   checkpoint_42.dat  72 bytes
 *   Starting replacement worker...
 *   New worker PID: 12346
 *   [INFO] Resuming job 42 from index 100
 *   [INFO] Job 42 completed successfully
 */

/*******************************************************************************
 * SECTION 10: PERFORMANCE CONSIDERATIONS
 *******************************************************************************/

/*
 * BOTTLENECK ANALYSIS
 * ===================
 * 
 * Profiling a typical job (1000 bars, SMA strategy):
 * 
 * Function                     Time    %
 * ---------------------------  -----  ---
 * Network I/O (request job)     5ms    5%
 * Load price data (CSV)        15ms   15%
 * Generate signals (SMA)       10ms   10%
 * Execute backtest             50ms   50%
 * Calculate metrics            10ms   10%
 * Send result                   5ms    5%
 * Checkpointing                 5ms    5%
 * ---------------------------  -----  ---
 * Total                       100ms  100%
 * 
 * Bottleneck: Backtest execution (50%)
 * 
 * Optimization Opportunities:
 * 
 * 1. Data Loading (15%):
 *    - Cache frequently used CSVs
 *    - Use memory-mapped files
 *    - Pre-load data on startup
 * 
 * 2. Signal Generation (10%):
 *    - Already optimized with rolling sum
 *    - Could use SIMD for vectorization
 * 
 * 3. Network I/O (10% combined):
 *    - Batch job requests
 *    - Use HTTP/2 multiplexing
 *    - Compress messages
 * 
 * MEMORY OPTIMIZATION
 * ===================
 * 
 * Memory Usage Per Job:
 *   price_data: 1000 bars × 64 bytes = 64 KB
 *   signals: 1000 × 1 byte = 1 KB
 *   portfolio_values: 1000 × 8 bytes = 8 KB
 *   Total: ~73 KB per job
 * 
 * For sequential processing: Only one job in memory
 * Total worker memory: ~10 MB (including overhead)
 * 
 * SCALABILITY
 * ===========
 * 
 * Workers scale linearly because:
 *   + No inter-worker communication
 *   + No shared state
 *   + Independent execution
 * 
 * Expected Performance:
 *   1 worker:  100 jobs in 10 seconds
 *   8 workers: 100 jobs in 1.47 seconds (6.8× faster, 85% efficient)
 * 
 * Efficiency Loss (15%) due to:
 *   - Job scheduling overhead
 *   - Network communication
 *   - Load imbalance (some jobs longer than others)
 *   - Raft consensus overhead
 */

/*******************************************************************************
 * SECTION 11: COMMON PITFALLS
 *******************************************************************************/

/*
 * PITFALL 1: Forgetting to Call start()
 * --------------------------------------
 * 
 * Problem: Worker constructed but not started
 * 
 * Wrong:
 *   Worker worker(config);
 *   // Forgot worker.start()!
 *   wait_forever();  // Worker never runs
 * 
 * Correct:
 *   Worker worker(config);
 *   worker.start();
 *   wait_for_shutdown();
 * 
 * 
 * PITFALL 2: Not Checking start() Return Value
 * ---------------------------------------------
 * 
 * Problem: Assuming start() succeeded
 * 
 * Wrong:
 *   worker.start();  // Ignores return value!
 *   // Worker might have failed to connect
 * 
 * Correct:
 *   if (!worker.start()) {
 *       std::cerr << "Failed to start\n";
 *       return 1;
 *   }
 * 
 * 
 * PITFALL 3: Not Calling stop() Before Destruction
 * -------------------------------------------------
 * 
 * Problem: Destroying worker while still running
 * 
 * Technically OK (destructor calls stop()), but better to be explicit:
 * 
 * Good:
 *   worker.stop();
 *   // Worker fully stopped, resources cleaned up
 *   // Then destructor is a no-op
 * 
 * Acceptable but implicit:
 *   // Destructor calls stop()
 *   // Works but less clear
 * 
 * 
 * PITFALL 4: Wrong Data Directory
 * --------------------------------
 * 
 * Problem: data_directory doesn't contain CSV files
 * 
 * Wrong:
 *   config.data_directory = "/nonexistent";
 *   Worker worker(config);
 *   worker.start();
 *   // All jobs will fail (can't load data)
 * 
 * Correct:
 *   // Verify directory exists and has data
 *   config.data_directory = "./data";
 *   if (!directory_exists(config.data_directory)) {
 *       std::cerr << "Data directory not found\n";
 *       return 1;
 *   }
 * 
 * 
 * PITFALL 5: Heartbeat Interval Too Long
 * ---------------------------------------
 * 
 * Problem: Long heartbeat interval causes false timeouts
 * 
 * Wrong:
 *   config.heartbeat_interval_sec = 10;  // Too long!
 *   // Controller timeout = 3 × 10 = 30 seconds
 *   // Jobs that take 20 seconds won't timeout, but slow detection
 * 
 * Correct:
 *   config.heartbeat_interval_sec = 2;  // Recommended
 * 
 * 
 * PITFALL 6: Not Handling Connection Loss
 * ----------------------------------------
 * 
 * Current Implementation: Worker exits on connection loss
 * 
 * Better: Retry with exponential backoff
 * 
 * Enhancement:
 *   bool connect_with_retry() {
 *       int delay = 1;
 *       for (int attempt = 0; attempt < 10; ++attempt) {
 *           if (connect_to_controller()) {
 *               return true;
 *           }
 *           sleep(delay);
 *           delay = std::min(delay * 2, 60);  // Max 1 minute
 *       }
 *       return false;
 *   }
 */

/*******************************************************************************
 * SECTION 12: FREQUENTLY ASKED QUESTIONS
 *******************************************************************************/

/*
 * Q1: How many workers can I run?
 * 
 * A1: Theoretically unlimited, practically limited by:
 *     - Controller capacity (hundreds of workers)
 *     - Network bandwidth
 *     - Data storage (NFS limitations)
 *     
 *     Project scope: 2-8 workers (sufficient for evaluation)
 *     Production: 10-100 workers typical
 * 
 * 
 * Q2: What happens if controller crashes?
 * 
 * A2: Current: Worker loses connection, exits
 *     
 *     Better: Retry connection, wait for new leader
 *     
 *     Raft guarantees new leader in ~3-5 seconds.
 *     Worker could wait and reconnect automatically.
 * 
 * 
 * Q3: Can workers run on different OS/architectures?
 * 
 * A3: Yes, but watch out for:
 *     - Endianness in checkpoints (save on x86, load on ARM)
 *     - Double precision differences
 *     - File path separators (Windows vs Unix)
 *     
 *     For cross-platform: Use Protobuf or JSON for checkpoints
 * 
 * 
 * Q4: Should I use NFS or local disk for data?
 * 
 * A4: Trade-offs:
 *     
 *     NFS:
 *       + Single source of truth
 *       + Easy updates (update once, all workers see it)
 *       - Network I/O overhead
 *       - NFS server bottleneck
 *     
 *     Local:
 *       + Fast I/O
 *       + No network dependency
 *       - Must replicate data to all workers
 *       - Synchronization complexity
 *     
 *     Recommendation: NFS for small clusters (<10 workers)
 *                    Local for large clusters
 * 
 * 
 * Q5: How do I monitor worker health?
 * 
 * A5: Multiple approaches:
 *     
 *     Logs:
 *       tail -f worker.log
 *       grep ERROR worker.log
 *     
 *     Process:
 *       ps aux | grep worker
 *       top -p $(pgrep worker)
 *     
 *     Network:
 *       netstat -an | grep 5000
 *       # Should see connection to controller
 *     
 *     Metrics (if implemented):
 *       curl localhost:9090/metrics
 *       # Prometheus-style metrics
 * 
 * 
 * Q6: What if a job takes a very long time?
 * 
 * A6: Heartbeat thread continues independently:
 *     - Job processing: 5 minutes
 *     - Heartbeats: Every 2 seconds
 *     - Controller: Sees heartbeats, knows worker alive
 *     - Job eventually completes
 *     
 *     No timeout on job duration (by design).
 * 
 * 
 * Q7: Can I run multiple worker instances on same machine?
 * 
 * A7: Yes, they'll each get unique worker_id from controller:
 *     
 *     Terminal 1: ./worker  # worker_id = 1
 *     Terminal 2: ./worker  # worker_id = 2
 *     Terminal 3: ./worker  # worker_id = 3
 *     
 *     Note: All will share same checkpoint_directory
 *     This is fine (checkpoints keyed by job_id, not worker_id)
 * 
 * 
 * Q8: How do I gracefully shutdown workers?
 * 
 * A8: Send SIGTERM:
 *     
 *     pkill -TERM worker
 *     
 *     Worker catches signal, calls stop(), exits cleanly.
 *     
 *     Don't use SIGKILL (-9):
 *       kill -9 $(pgrep worker)  # Abrupt termination!
 *       
 *     This doesn't allow graceful shutdown.
 * 
 * 
 * Q9: What's the maximum number of jobs a worker can handle?
 * 
 * A9: Unlimited sequentially.
 *     
 *     Worker processes jobs one at a time until stopped.
 *     Memory released after each job.
 *     
 *     In practice, workers run for hours/days processing
 *     thousands of jobs.
 * 
 * 
 * Q10: Is this production-ready code?
 * 
 * A10: No, this is academic code.
 *      
 *      For production, add:
 *      1. Automatic reconnection on controller failure
 *      2. Exponential backoff for retries
 *      3. Metrics export (Prometheus)
 *      4. Health check endpoint
 *      5. Graceful job cancellation
 *      6. Resource limits (max memory, max job time)
 *      7. Logging to files (not just stderr)
 *      8. Monitoring dashboard
 *      9. Alerting on failures
 *      10. Comprehensive testing
 */

#endif // WORKER_H