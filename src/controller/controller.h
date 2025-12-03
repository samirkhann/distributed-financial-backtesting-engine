/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: controller.h
    
    Description:
        This header file defines the Controller class and supporting data
        structures for the distributed financial backtesting system. The
        Controller serves as the central coordinator that manages worker nodes,
        distributes backtest jobs, monitors system health, and aggregates
        computation results across the distributed cluster.
        
        Architectural Role:
        The Controller is the brain of the distributed system, responsible for:
        
        1. WORKER MANAGEMENT: Registration, health monitoring, failure detection
        2. JOB DISTRIBUTION: Scheduling, load balancing, fault tolerance
        3. RESULT AGGREGATION: Collecting and storing backtest results
        4. NETWORK COORDINATION: TCP server accepting worker connections
        5. FAULT TOLERANCE: Detecting failures, reassigning jobs
        
    System Architecture:
        
        ┌──────────────────────────────────────────────────┐
        │              CONTROLLER NODE                      │
        │                                                   │
        │  ┌─────────────┐  ┌──────────────┐              │
        │  │ TCP Server  │→│ Worker Registry│              │
        │  │ (Port 5000) │  └──────────────┘              │
        │  └─────────────┘         ↓                       │
        │         ↓         ┌──────────────┐              │
        │  ┌─────────────┐ │ Job Scheduler │              │
        │  │   Workers   │→│ (Load Balance)│              │
        │  │   (2-8)     │  └──────────────┘              │
        │  └─────────────┘         ↓                       │
        │         ↓         ┌──────────────┐              │
        │  ┌─────────────┐ │   Heartbeat   │              │
        │  │ Connections │→│   Monitor     │              │
        │  └─────────────┘  └──────────────┘              │
        └──────────────────────────────────────────────────┘
                     ↕ Network (TCP)
        ┌────────────────────────────────────────┐
        │         WORKER NODES (Khoury Cluster)  │
        │  Worker 1    Worker 2    ...   Worker 8│
        └────────────────────────────────────────┘
        
    Concurrency Model:
        Multi-threaded server with fine-grained locking:
        
        BACKGROUND THREADS (4 + N):
        1. Accept Thread: Listens for new worker connections
        2. Scheduler Thread: Distributes jobs using load balancing
        3. Heartbeat Thread: Monitors worker health (every 2 seconds)
        4. Worker Threads: One per connected worker (N threads)
        
        SYNCHRONIZATION PRIMITIVES:
        - workers_mutex_: Protects worker registry and socket tracking
        - jobs_mutex_: Protects job queues (pending, active, completed)
        - scheduler_cv_: Condition variable for efficient scheduler waking
        - Atomic counters: next_job_id_, next_worker_id_, statistics
        
        LOCK-FREE OPERATIONS:
        - running_ flag: Read/write without lock (bool is atomic on all platforms)
        - Atomic counters: Lock-free increment (std::atomic)
        - Condition variables: Avoid busy-waiting
        
    Design Patterns:
        
        1. MULTI-THREADED SERVER
           - Accept thread for new connections
           - Thread pool for worker handling
           - Background threads for monitoring
        
        2. PRODUCER-CONSUMER
           - Clients produce jobs → pending queue
           - Scheduler consumes jobs → assigns to workers
           - Workers produce results → completed queue
        
        3. OBSERVER PATTERN
           - Heartbeat monitor observes worker health
           - Removes failed workers
           - Triggers job reassignment
        
        4. STRATEGY PATTERN
           - Pluggable worker selection algorithms
           - Round-robin vs. least-loaded
           - Easy to add new strategies
        
        5. RAII (Resource Acquisition Is Initialization)
           - std::lock_guard for automatic mutex unlocking
           - Destructor calls stop() for clean shutdown
           - Smart pointers for message ownership
        
    Fault Tolerance:
        
        WORKER FAILURE DETECTION:
        - Heartbeat-based: Worker sends heartbeat every 2 seconds
        - Timeout detection: Controller expects heartbeat within 6 seconds
        - Failure response: Mark worker dead, reassign jobs
        
        JOB REASSIGNMENT:
        - Active jobs on failed worker → pending queue
        - Scheduler automatically reassigns to healthy workers
        - No jobs lost (unless all workers fail)
        
        RECOVERY CAPABILITIES:
        - New workers can join anytime (automatic registration)
        - Jobs wait in queue until workers available
        - Graceful degradation (system continues with reduced capacity)
        
    Extensibility for Raft:
        
        This class is designed to be extended by RaftController:
        - Virtual methods: start(), stop(), submit_job()
        - Protected members: Allow subclass access
        - Same interface: Raft version drop-in replacement
        
        Future RaftController will add:
        - Leader election
        - Log replication
        - Cluster consensus
        - Controller fault tolerance
        
    Performance Characteristics:
        - Job submission: O(log n) insertion to queue
        - Job assignment: O(n) worker selection (n = worker count)
        - Worker lookup: O(log n) in map (n = worker count)
        - Heartbeat check: O(n) iteration over workers
        - Memory: O(jobs + workers) - bounded by system capacity
        
    Thread Safety Guarantees:
        - All public methods are thread-safe
        - Multiple threads can submit jobs concurrently
        - Safe to call from signal handlers (for stop() only)
        - No data races (verified with ThreadSanitizer)
        
    Resource Management:
        - Sockets: Closed in destructor and stop()
        - Threads: Joined before destruction
        - Memory: RAII containers, no manual new/delete
        - No resource leaks (verified with Valgrind)
        
    Integration Points:
        - common/message.h: Protocol definitions
        - common/logger.h: Structured logging
        - worker/worker.h: Worker-side counterpart
        - client/client.h: Client submits jobs
        
    Related Files:
        - controller.cpp: Implementation of all methods
        - raft_controller.h: Raft-enabled fault-tolerant extension
        - tests/controller_test.cpp: Unit and integration tests

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. HEADER GUARD & PREPROCESSOR
// 2. STANDARD LIBRARY INCLUDES
// 3. SYSTEM INCLUDES (POSIX Sockets)
// 4. NAMESPACE DECLARATION
// 5. DATA STRUCTURES
//    5.1 WorkerInfo: Worker Node Metadata
//    5.2 JobInfo: Job State Tracking
//    5.3 ControllerConfig: Configuration Parameters
// 6. CONTROLLER CLASS
//    6.1 Class Overview & Design Philosophy
//    6.2 Protected Members (Raft Extension Points)
//        6.2.1 Configuration
//        6.2.2 Network State
//        6.2.3 Job Management State
//        6.2.4 Worker Management State
//        6.2.5 Thread Management
//        6.2.6 Synchronization Primitives
//        6.2.7 Statistics
//    6.3 Public Interface
//        6.3.1 Lifecycle Management
//        6.3.2 Job Submission
//        6.3.3 Result Retrieval
//        6.3.4 Statistics & Monitoring
//    6.4 Private Implementation Methods
//        6.4.1 Network Operations
//        6.4.2 Worker Management
//        6.4.3 Job Scheduling
//        6.4.4 Message Handling
//        6.4.5 Cleanup
// 7. USAGE EXAMPLES
// 8. THREAD SAFETY ANALYSIS
// 9. COMMON PITFALLS & SOLUTIONS
// 10. FAQ
// 11. BEST PRACTICES
// 12. PERFORMANCE CONSIDERATIONS
// 13. RAFT INTEGRATION GUIDE
// 14. TESTING STRATEGIES
// 15. TROUBLESHOOTING GUIDE
//
//==============================================================================

//==============================================================================
// SECTION 1: HEADER GUARD & PREPROCESSOR
//==============================================================================

#ifndef CONTROLLER_H
#define CONTROLLER_H

//==============================================================================
// SECTION 2: STANDARD LIBRARY INCLUDES
//==============================================================================
//
// Standard C++ library headers for data structures and concurrency.
//
//------------------------------------------------------------------------------

#include "common/message.h"  // Message protocol definitions
#include "common/logger.h"   // Logging utilities

// <string> - std::string for hostnames, paths
#include <string>

// <queue> - std::queue for pending job queue (FIFO)
// Producer-consumer pattern: submit_job produces, scheduler consumes
#include <queue>

// <map> - std::map for O(log n) lookups
// Used for: workers_, active_jobs_, completed_jobs_
// Ordered container: Enables iteration in deterministic order
#include <map>

// <set> - std::set for worker socket tracking
// Used for: worker_sockets_ (O(log n) insert/remove/find)
#include <set>

// <mutex> - std::mutex for thread synchronization
// Protects: workers_ map, jobs queues
#include <mutex>

// <condition_variable> - std::condition_variable for efficient waiting
// Used by scheduler thread to avoid busy-waiting
#include <condition_variable>

// <thread> - std::thread for background threads
// Accept, scheduler, heartbeat, and per-worker threads
#include <thread>

// <atomic> - std::atomic for lock-free counters
// Used for: running_, next_job_id_, next_worker_id_, statistics
#include <atomic>

// <chrono> - std::chrono for timestamps and timeouts
// Heartbeat monitoring, job assignment times, timeout calculations
#include <chrono>

//==============================================================================
// SECTION 3: SYSTEM INCLUDES (POSIX SOCKETS)
//==============================================================================
//
// POSIX socket API for network communication.
// Platform: Linux/Unix (not portable to Windows)
//
//------------------------------------------------------------------------------

// <sys/socket.h> - Socket API
// Functions: socket(), bind(), listen(), accept(), send(), recv(), shutdown()
#include <sys/socket.h>

// <netinet/in.h> - Internet address structures
// Types: sockaddr_in, in_addr
// Constants: AF_INET, SOCK_STREAM, INADDR_ANY
#include <netinet/in.h>

// <arpa/inet.h> - Internet address manipulation
// Functions: inet_ntop(), inet_pton(), htons(), ntohs()
#include <arpa/inet.h>

//==============================================================================
// SECTION 4: NAMESPACE DECLARATION
//==============================================================================

namespace backtesting {

//==============================================================================
// SECTION 5: DATA STRUCTURES
//==============================================================================
//
// Supporting types for controller implementation.
// All are POD-style structs with public fields and default constructors.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 5.1 WORKERINFO: WORKER NODE METADATA
//------------------------------------------------------------------------------
//
// struct WorkerInfo
//
// PURPOSE:
// Tracks state and metadata for each registered worker node in the cluster.
//
// LIFECYCLE:
// 1. Worker connects → Controller creates WorkerInfo
// 2. Worker registers → Fields populated (hostname, port, etc.)
// 3. Worker sends heartbeats → last_heartbeat updated
// 4. Worker fails → is_alive = false
// 5. Worker disconnects → Removed from workers_ map
//
// THREAD SAFETY:
// - Stored in workers_ map (protected by workers_mutex_)
// - Modified by multiple threads (accept, heartbeat, scheduler)
// - Always accessed with lock held
//
// MEMORY LAYOUT:
// Approximate size: 80-120 bytes per worker
// - Fixed fields: ~64 bytes
// - hostname: ~8-40 bytes (depends on length)
//
// USAGE:
// - Controller maintains map<uint64_t, WorkerInfo> workers_
// - Key: worker_id (unique identifier)
// - Value: WorkerInfo (all metadata)
//
// FIELD DESCRIPTIONS:
//
// worker_id:
//   - Unique identifier assigned by controller
//   - Incremented from 1 (next_worker_id_++)
//   - Never reused (even after worker disconnect)
//   - Used for: Logging, job assignment tracking, message routing
//
// socket_fd:
//   - TCP socket file descriptor for communication
//   - Obtained from accept() system call
//   - Used for: send(), recv() operations
//   - -1 indicates invalid/closed socket
//
// hostname:
//   - Worker's hostname or IP address
//   - Provided by worker in WORKER_REGISTER message
//   - Examples: "kh05.ccs.neu.edu", "192.168.1.105"
//   - Used for: Logging, potential reconnection (not implemented)
//
// port:
//   - TCP port worker listens on (for future use)
//   - Range: 1-65535
//   - Currently: Not used (controller only sends to existing connection)
//   - Future: Could enable controller-initiated reconnection
//
// active_jobs:
//   - Number of jobs currently executing on this worker
//   - Updated via: Heartbeat messages from worker
//   - Used for: Load balancing (least-loaded strategy)
//   - Decrements: When worker reports job completion
//
// completed_jobs:
//   - Total jobs completed by this worker since registration
//   - Updated via: Heartbeat messages
//   - Monotonically increasing (never decreases)
//   - Used for: Statistics, monitoring worker productivity
//
// last_heartbeat:
//   - Timestamp of most recent heartbeat message
//   - Type: std::chrono::steady_clock::time_point (monotonic time)
//   - Updated: Every 2 seconds when heartbeat received
//   - Used for: Failure detection (if now - last_heartbeat > 6s → failed)
//
// is_alive:
//   - Worker is alive and accepting jobs
//   - true: Worker healthy (heartbeats arriving)
//   - false: Worker failed (timeout or disconnected)
//   - Used for: Worker selection (only assign to alive workers)
//

// Worker information
struct WorkerInfo {
    // Unique worker identifier (assigned by controller during registration)
    // Range: 1 to UINT64_MAX (never exhausted in practice)
    uint64_t worker_id;
    
    // TCP socket file descriptor for communication with this worker
    // -1 indicates invalid/closed socket
    int socket_fd;
    
    // Worker's hostname or IP address (for logging/debugging)
    // Examples: "kh05.ccs.neu.edu", "192.168.1.105", "worker-3"
    std::string hostname;
    
    // TCP port worker listens on (reserved for future use)
    // Currently: Not used (controller only sends on existing connection)
    uint16_t port;
    
    // Number of jobs currently executing on this worker
    // Updated via heartbeat messages
    // Used for load balancing (assign to worker with fewest active jobs)
    uint32_t active_jobs;
    
    // Total number of jobs completed by this worker since registration
    // Monotonically increasing
    // Used for statistics and monitoring
    uint32_t completed_jobs;
    
    // Timestamp of most recent heartbeat message
    // Used for failure detection (timeout = 6 seconds)
    // steady_clock: Monotonic time (not affected by system clock changes)
    std::chrono::steady_clock::time_point last_heartbeat;
    
    // Worker is alive and accepting jobs
    // true: Heartbeats arriving, can assign jobs
    // false: Timeout or disconnected, do not assign jobs
    bool is_alive;
    
    // Default constructor: Initialize to safe defaults
    // - worker_id = 0 (invalid, must be set during registration)
    // - socket_fd = -1 (invalid socket)
    // - port = 0 (invalid port)
    // - Counters = 0
    // - is_alive = false (not alive until registered and heartbeat received)
    WorkerInfo() : worker_id(0), socket_fd(-1), port(0), 
                   active_jobs(0), completed_jobs(0), is_alive(false) {}
};

//------------------------------------------------------------------------------
// 5.2 JOBINFO: JOB STATE TRACKING
//------------------------------------------------------------------------------
//
// struct JobInfo
//
// PURPOSE:
// Tracks complete lifecycle of a backtest job from submission to completion.
//
// JOB STATE MACHINE:
//
//   ┌──────────┐ submit_job()  ┌─────────┐ scheduler  ┌────────┐
//   │ Created  │──────────────→│ Pending │───────────→│ Active │
//   └──────────┘                └─────────┘            └────────┘
//                                                           │
//                                                           │ Worker completes
//                                                           ↓
//                                                      ┌───────────┐
//                                                      │ Completed │
//                                                      └───────────┘
//
// STATE TRANSITIONS:
// 1. Created: JobInfo constructed in submit_job()
// 2. Pending: Added to pending_jobs_ queue
// 3. Active: Assigned to worker, moved to active_jobs_ map
// 4. Completed: Result received, moved to completed_jobs_ map
//
// FAILURE HANDLING:
// - Worker fails: Job returns to pending (reassignment)
// - Job fails: Marked failed, moved to completed with error
//
// FIELD DESCRIPTIONS:
//
// job_id:
//   - Globally unique job identifier
//   - Assigned by controller (next_job_id_++)
//   - Used for: Tracking, logging, result correlation
//   - Persists: From submission through completion
//
// params:
//   - JobParams struct with all backtest parameters
//   - Includes: symbol, strategy, dates, windows, capital
//   - Immutable: Set at submission, never changed
//
// assigned_worker_id:
//   - Which worker is executing this job
//   - 0: Not assigned (in pending queue)
//   - >0: Assigned to specific worker
//   - Used for: Tracking, reassignment on failure
//
// completed:
//   - Job has finished (success or failure)
//   - true: Result available in result field
//   - false: Job still in progress or pending
//
// failed:
//   - Job failed to complete successfully
//   - true: Check result.error_message for details
//   - false: Job succeeded or still in progress
//   - Subtlety: completed && !failed = success
//
// result:
//   - JobResult struct with backtest metrics
//   - Valid only if completed = true
//   - Contains: returns, Sharpe, drawdown, trades, etc.
//
// assigned_time:
//   - When job was assigned to worker
//   - Used for: Timeout detection, performance metrics
//   - Type: steady_clock (monotonic, not affected by clock changes)
//
// USAGE PATTERN:
//
//   // Create job
//   JobInfo job;
//   job.job_id = next_job_id_++;
//   job.params = params;
//   pending_jobs_.push(job);  // State: Pending
//   
//   // Assign to worker
//   job.assigned_worker_id = worker_id;
//   job.assigned_time = now();
//   active_jobs_[job.job_id] = job;  // State: Active
//   
//   // Complete job
//   job.completed = true;
//   job.result = result_from_worker;
//   completed_jobs_[job.job_id] = job;  // State: Completed
//

// Job information
struct JobInfo {
    // Unique job identifier (assigned by controller)
    // Used for tracking across entire lifecycle
    uint64_t job_id;
    
    // Backtest parameters (symbol, strategy, dates, etc.)
    // Immutable after creation
    JobParams params;
    
    // Worker this job is assigned to
    // 0 = not assigned (in pending queue)
    // >0 = assigned to specific worker
    uint64_t assigned_worker_id;
    
    // Job has finished (success or failure)
    // true = result is valid
    // false = job still in progress or pending
    bool completed;
    
    // Job failed to complete
    // true = completed with error
    // false = succeeded or in progress
    bool failed;
    
    // Backtest computation results
    // Valid only if completed = true
    JobResult result;
    
    // When job was assigned to worker
    // Used for timeout detection and performance measurement
    std::chrono::steady_clock::time_point assigned_time;
    
    // Default constructor: Initialize to safe defaults
    JobInfo() : job_id(0), assigned_worker_id(0), 
                completed(false), failed(false) {}
};

//------------------------------------------------------------------------------
// 5.3 CONTROLLERCONFIG: CONFIGURATION PARAMETERS
//------------------------------------------------------------------------------
//
// struct ControllerConfig
//
// PURPOSE:
// Configuration parameters for controller initialization.
//
// FIELD DESCRIPTIONS:
//
// listen_port:
//   - TCP port to listen on for worker connections
//   - Default: 5000
//   - Range: 1-65535 (avoid 0-1023 without root)
//   - Considerations: Choose port not used by other services
//
// max_workers:
//   - Maximum number of workers (not enforced, just documentation)
//   - Default: 16
//   - Actual: Limited by system resources (file descriptors, memory)
//
// heartbeat_timeout_sec:
//   - Worker failure detection timeout (seconds)
//   - Default: 6 (3 missed 2-second heartbeats)
//   - Shorter: Faster failure detection, more false positives
//   - Longer: Slower detection, fewer false positives
//
// data_directory:
//   - Path to shared data directory (for CSV files)
//   - Default: "./data"
//   - Used by: Workers (not controller directly)
//   - Could be: NFS mount point on Khoury cluster
//
// USAGE:
//   ControllerConfig config;
//   config.listen_port = 6000;
//   config.heartbeat_timeout_sec = 10;
//   Controller controller(config);
//

// Controller configuration
struct ControllerConfig {
    // TCP port to listen on for worker connections
    // Default: 5000 (configurable)
    uint16_t listen_port;
    
    // Maximum expected workers (not enforced, just hint)
    // Default: 16
    int max_workers;
    
    // Heartbeat timeout in seconds (failure detection)
    // Default: 6 seconds (3 missed 2-second heartbeats)
    int heartbeat_timeout_sec;
    
    // Directory containing historical price data (CSV files)
    // Default: "./data" (relative to working directory)
    std::string data_directory;
    
    // Default constructor with sensible defaults
    ControllerConfig() : listen_port(5000), max_workers(16), 
                         heartbeat_timeout_sec(6), data_directory("./data") {}
};

//==============================================================================
// SECTION 6: CONTROLLER CLASS
//==============================================================================

//------------------------------------------------------------------------------
// 6.1 CLASS OVERVIEW & DESIGN PHILOSOPHY
//------------------------------------------------------------------------------
//
// class Controller
//
// OVERVIEW:
// Central coordinator for distributed backtesting system. Manages worker
// registration, job distribution, health monitoring, and result aggregation.
//
// DESIGN PHILOSOPHY:
//
// 1. SEPARATION OF CONCERNS:
//    - Network layer: Accept connections, send/receive messages
//    - Job scheduling: Distribute work using load balancing
//    - Health monitoring: Detect failures via heartbeats
//    - Result aggregation: Collect and store results
//
// 2. THREAD SAFETY:
//    - Fine-grained locking (separate mutexes for workers and jobs)
//    - Lock-free operations where possible (atomic counters)
//    - Condition variables for efficient synchronization
//
// 3. FAULT TOLERANCE:
//    - Heartbeat-based failure detection
//    - Automatic job reassignment
//    - Graceful degradation (system continues with fewer workers)
//
// 4. EXTENSIBILITY:
//    - Virtual methods for Raft extension
//    - Protected members for subclass access
//    - Strategy pattern for worker selection
//
// 5. RESOURCE MANAGEMENT:
//    - RAII for all resources (sockets, threads, containers)
//    - Explicit cleanup in destructor and stop()
//    - No memory leaks (verified with Valgrind)
//
// INHERITANCE HIERARCHY:
//
//   Controller (this class)
//       ↑
//       │ (future extension)
//       │
//   RaftController (adds leader election, log replication)
//
// Protected members allow RaftController to:
// - Access job queues directly
// - Override scheduling decisions
// - Replicate state across controller cluster
//
// CONCURRENCY DESIGN:
//
// Thread Model:
//   Main Thread: Calls start(), submit_job(), get_job_result()
//   Accept Thread: Accepts new connections
//   Scheduler Thread: Distributes jobs
//   Heartbeat Thread: Monitors worker health
//   Worker Threads: Handle individual workers (one per worker)
//
// Synchronization:
//   - workers_mutex_: Protects worker registry
//   - jobs_mutex_: Protects job queues
//   - scheduler_cv_: Wakes scheduler when work available
//   - No condition variable for workers (mutex sufficient)
//
// Lock Acquisition Order (to prevent deadlock):
//   1. workers_mutex_
//   2. jobs_mutex_
//   Never acquire in reverse order!
//
// MEMORY MANAGEMENT:
//
// Owned Resources:
//   - server_socket_: TCP server socket (closed in stop())
//   - worker sockets: Closed in stop() and handle_worker_connection()
//   - Threads: Joined in stop()
//   - STL containers: Automatic cleanup
//
// Resource Limits:
//   - File descriptors: Typically 1024 (ulimit -n)
//   - Memory: Limited by available RAM
//   - Threads: Limited by system (typically 10,000+)
//
// For 8 workers:
//   - Sockets: 1 (server) + 8 (workers) = 9
//   - Threads: 3 (background) + 8 (workers) = 11
//   - Memory: ~100KB (negligible)
//
//------------------------------------------------------------------------------

// Controller node
class Controller {
protected:
    //==========================================================================
    // SECTION 6.2: PROTECTED MEMBERS (RAFT EXTENSION POINTS)
    //==========================================================================
    //
    // Protected members allow RaftController to extend functionality:
    // - Access internal state for replication
    // - Override scheduling decisions
    // - Maintain Raft log entries
    //
    // DESIGN RATIONALE:
    // - Protected (not private): Subclass can access
    // - Not public: Implementation details, not for external use
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // 6.2.1 CONFIGURATION
    //--------------------------------------------------------------------------
    
    // Controller configuration (immutable after construction)
    // Contains: port, timeouts, paths
    // Protected: RaftController may need to read configuration
    ControllerConfig config_;
    
    //--------------------------------------------------------------------------
    // 6.2.2 NETWORK STATE
    //--------------------------------------------------------------------------
    
    // TCP server socket (listens for worker connections)
    // Created in: setup_server_socket()
    // Used by: accept_connections() thread
    // Closed in: stop() method
    // -1 indicates: Not created or closed
    //
    // THREAD SAFETY:
    // - Only accept thread reads (no lock needed)
    // - Main thread writes during setup/shutdown (no concurrent access)
    int server_socket_;
    
    // Controller is running (threads active)
    // Atomic: Safe to read from multiple threads without lock
    // Set to: true in start(), false in stop()
    // Checked by: All background threads in their loops
    //
    // WHY ATOMIC?
    // - Read by multiple threads (no lock needed)
    // - Write is rare (only during start/stop)
    // - Alternative: Use mutex, but atomic is more efficient
    //
    // NOTE: std::atomic<bool> is lock-free on all modern platforms
    std::atomic<bool> running_;
    
    //--------------------------------------------------------------------------
    // 6.2.3 JOB MANAGEMENT STATE
    //--------------------------------------------------------------------------
    //
    // Job lifecycle: pending → active → completed
    //
    // THREAD SAFETY: All protected by jobs_mutex_
    //
    
    // Queue of jobs waiting for worker assignment
    // FIFO: First submitted, first assigned
    // Producer: submit_job() adds to queue
    // Consumer: scheduler_loop() removes and assigns
    //
    // WHY QUEUE (not priority_queue or vector)?
    // - FIFO is fair (jobs processed in order)
    // - Simple and efficient for basic scheduling
    // - RaftController could replace with priority queue
    std::queue<JobInfo> pending_jobs_;
    
    // Map of jobs currently executing on workers
    // Key: job_id
    // Value: JobInfo (includes assigned_worker_id, assigned_time)
    //
    // WHY MAP (not unordered_map)?
    // - Ordered iteration (useful for debugging)
    // - O(log n) lookup (sufficient for expected job counts)
    // - Deterministic behavior (easier testing)
    //
    // Lifecycle:
    // - Added: When scheduler assigns to worker
    // - Removed: When worker returns result
    std::map<uint64_t, JobInfo> active_jobs_;
    
    // Map of finished jobs (success or failure)
    // Key: job_id
    // Value: JobInfo (includes result)
    //
    // Lifecycle:
    // - Added: When worker returns result
    // - Removed: Never (persists until controller shutdown)
    //
    // MEMORY CONSIDERATION:
    // - Unbounded growth (could be GBs for 100,000+ jobs)
    // - Future: Implement cleanup policy (remove after N days)
    std::map<uint64_t, JobInfo> completed_jobs_;
    
    // Next job ID to assign (incremented for each job)
    // Atomic: Thread-safe increment without lock
    // Starts at: 1 (0 reserved for invalid)
    //
    // WHY ATOMIC?
    // - Incremented by submit_job() (may be called from multiple threads)
    // - No lock needed (fetch_add is lock-free)
    // - Guaranteed unique IDs
    std::atomic<uint64_t> next_job_id_;
    
    //--------------------------------------------------------------------------
    // 6.2.4 WORKER MANAGEMENT STATE
    //--------------------------------------------------------------------------
    //
    // Worker tracking: registration, health, socket mapping
    //
    // THREAD SAFETY: All protected by workers_mutex_
    //
    
    // Map of registered workers
    // Key: worker_id
    // Value: WorkerInfo (all metadata)
    //
    // Modified by:
    // - register_worker(): Add new worker
    // - remove_worker(): Remove failed/disconnected worker
    // - handle_worker_connection(): Update heartbeat, job counts
    //
    // WHY MAP (not vector)?
    // - Workers identified by ID (not index)
    // - IDs are not contiguous (worker 5 may disconnect, leaving gap)
    // - O(log n) lookup by ID
    std::map<uint64_t, WorkerInfo> workers_;
    
    // Next worker ID to assign (incremented for each registration)
    // Atomic: Thread-safe increment
    // Starts at: 1 (0 reserved for "not assigned")
    std::atomic<uint64_t> next_worker_id_;
    
    // Set of active worker socket file descriptors
    // Used for: Quick socket validation and cleanup
    //
    // WHY SEPARATE FROM workers_ MAP?
    // - Fast O(log n) lookup by socket FD
    // - workers_ map indexed by worker_id (not socket)
    // - Cleanup: Easier to iterate and close all sockets
    std::set<int> worker_sockets_;
    
    //--------------------------------------------------------------------------
    // 6.2.5 THREAD MANAGEMENT
    //--------------------------------------------------------------------------
    //
    // Background threads for asynchronous operations
    //
    
    // Accept thread: Listens for new worker connections
    // Lifecycle: Started in start(), joined in stop()
    // Blocks on: accept() system call
    // Exits when: Server socket closed
    std::thread accept_thread_;
    
    // Scheduler thread: Distributes jobs to workers
    // Lifecycle: Started in start(), joined in stop()
    // Waits on: scheduler_cv_ condition variable
    // Wakes when: New job submitted or worker registers
    std::thread scheduler_thread_;
    
    // Heartbeat monitor thread: Detects worker failures
    // Lifecycle: Started in start(), joined in stop()
    // Sleeps: 2 seconds between checks
    // Checks: Worker last_heartbeat timestamps
    std::thread heartbeat_thread_;
    
    // Worker connection threads (one per worker)
    // Key: socket file descriptor
    // Value: Thread handling that worker
    //
    // Lifecycle:
    // - Created: When worker connects (in accept_connections)
    // - Detached: Runs independently
    // - Exits: When worker disconnects or controller stops
    //
    // WHY DETACHED?
    // - Don't need to join (cleans up automatically)
    // - Simplifies shutdown (no tracking needed)
    // - Worker count varies dynamically
    std::map<int, std::thread> worker_threads_;
    
    //--------------------------------------------------------------------------
    // 6.2.6 SYNCHRONIZATION PRIMITIVES
    //--------------------------------------------------------------------------
    //
    // Mutexes and condition variables for thread coordination
    //
    
    // Protects job queues and maps
    // Locks: pending_jobs_, active_jobs_, completed_jobs_
    // Acquired by: submit_job(), scheduler_loop(), handle_worker_connection()
    //
    // LOCK DURATION:
    // - Submit job: ~1-10 μs (queue push)
    // - Scheduler: ~10-100 μs (queue pop, map insert)
    // - Result handling: ~10-50 μs (map operations)
    //
    // CONTENTION:
    // - Low: submit_job() is fast
    // - Medium: Scheduler and result handler may conflict
    // - Mitigation: Fine-grained locking (lock, operate, unlock quickly)
    mutable std::mutex jobs_mutex_;
    
    // Protects worker registry and sockets
    // Locks: workers_, worker_sockets_
    // Acquired by: register_worker(), remove_worker(), heartbeat checker
    //
    // LOCK DURATION:
    // - Register: ~10-50 μs (map insert)
    // - Remove: ~10-100 μs (map erase, iteration)
    // - Heartbeat: ~5-20 μs (map update)
    //
    // CONTENTION:
    // - Low: Worker registration is infrequent
    // - Low: Heartbeat updates are simple
    mutable std::mutex workers_mutex_;
    
    // Condition variable for scheduler thread
    // Woken when:
    // - New job submitted (submit_job calls notify_one)
    // - Worker registers (register_worker calls notify_one)
    // - Worker removed (remove_worker calls notify_one)
    //
    // Used with: jobs_mutex_
    // Wait pattern: wait_for(lock, timeout, predicate)
    //
    // WHY CONDITION VARIABLE (not busy-wait)?
    // - Efficient: Thread sleeps until work available
    // - CPU-friendly: No spinning
    // - Responsive: Wakes immediately on notification
    std::condition_variable scheduler_cv_;
    
    //--------------------------------------------------------------------------
    // 6.2.7 STATISTICS
    //--------------------------------------------------------------------------
    //
    // Performance and monitoring counters
    //
    
    // Total number of jobs completed successfully
    // Atomic: Incremented in handle_worker_connection()
    // Used for: Monitoring, reporting
    std::atomic<uint64_t> total_jobs_completed_;
    
    // Total number of jobs that failed
    // Atomic: Incremented in handle_worker_connection()
    // Used for: Error rate monitoring
    std::atomic<uint64_t> total_jobs_failed_;
    
public:
    //==========================================================================
    // SECTION 6.3: PUBLIC INTERFACE
    //==========================================================================
    //
    // Public methods available to users of Controller class.
    // All are thread-safe and can be called from multiple threads.
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // 6.3.1 LIFECYCLE MANAGEMENT
    //--------------------------------------------------------------------------
    
    // Constructor: Initializes controller with configuration
    // Parameters:
    //   - config: Configuration (port, timeouts, etc.)
    //   - Default: ControllerConfig() with sensible defaults
    //
    // Note: explicit prevents implicit conversions
    // Example: Controller c(config); ✅
    //          Controller c = config; (prevented by explicit)
    //
    // EXCEPTION SAFETY:
    // - Nothrow: Constructor should not throw
    // - All members have default constructors
    //
    // THREAD SAFETY:
    // - Not thread-safe: Don't construct from multiple threads
    // - After construction: Safe to use from multiple threads
    explicit Controller(const ControllerConfig& config = ControllerConfig());
    
    // Destructor: Stops controller and cleans up resources
    // Virtual: Allows polymorphic deletion (RaftController)
    //
    // Cleanup order:
    // 1. Call stop() (idempotent)
    // 2. Threads joined
    // 3. Sockets closed
    // 4. Containers cleared (automatic)
    //
    // EXCEPTION SAFETY:
    // - Nothrow: Destructors must not throw
    // - stop() catches all exceptions
    virtual ~Controller();
    
    // Start controller (setup network, spawn threads)
    // Virtual: RaftController overrides to add Raft initialization
    //
    // RETURNS:
    //   - true: Successfully started
    //   - false: Failed to start (check logs)
    //
    // ERRORS:
    //   - Port in use: Another process using the port
    //   - Permission denied: Port < 1024 requires root
    //   - Network error: Interface unavailable
    //
    // THREAD SAFETY:
    // - Not thread-safe: Call only from main thread
    // - Idempotent: Multiple calls return false (already running)
    //
    // EXAMPLE:
    //   if (!controller.start()) {
    //       Logger::error("Startup failed");
    //       return 1;
    //   }
    virtual bool start();
    
    // Stop controller (graceful shutdown)
    // Virtual: RaftController overrides to add Raft cleanup
    //
    // BEHAVIOR:
    // - Stops all threads
    // - Closes all connections
    // - Cleans up resources
    // - Idempotent: Safe to call multiple times
    //
    // BLOCKING:
    // - Waits for all threads to exit
    // - May take up to heartbeat_timeout_sec
    //
    // THREAD SAFETY:
    // - Safe to call from any thread (including signal handler)
    // - Blocks until all threads joined
    //
    // EXAMPLE:
    //   controller.stop();
    //   // Controller fully stopped, safe to destroy
    virtual void stop();
    
    // Check if controller is running
    // RETURNS: true if start() called and stop() not yet called
    //
    // THREAD SAFETY:
    // - Safe to call from any thread
    // - Atomic read of running_ flag
    bool is_running() const { return running_; }
    
    //--------------------------------------------------------------------------
    // 6.3.2 JOB SUBMISSION
    //--------------------------------------------------------------------------
    
    // Submit backtest job for execution
    // Virtual: RaftController overrides to replicate in Raft log
    //
    // PARAMETERS:
    //   - params: Backtest parameters (symbol, strategy, dates, etc.)
    //
    // RETURNS:
    //   - uint64_t: Job ID (use with get_job_result)
    //
    // BEHAVIOR:
    // - Assigns unique job ID
    // - Adds to pending queue
    // - Wakes scheduler thread
    // - Returns immediately (non-blocking)
    //
    // THREAD SAFETY:
    // - Thread-safe: Can call from multiple threads
    // - Locks jobs_mutex_ briefly
    //
    // EXAMPLE:
    //   JobParams params;
    //   params.symbol = "AAPL";
    //   params.strategy_type = "SMA";
    //   params.start_date = "2020-01-01";
    //   params.end_date = "2024-12-31";
    //   
    //   uint64_t job_id = controller.submit_job(params);
    //   Logger::info("Submitted job " + std::to_string(job_id));
    virtual uint64_t submit_job(const JobParams& params);
    
    //--------------------------------------------------------------------------
    // 6.3.3 RESULT RETRIEVAL
    //--------------------------------------------------------------------------
    
    // Get job result (blocking until available or timeout)
    //
    // PARAMETERS:
    //   - job_id: Job identifier (from submit_job)
    //   - result: Output parameter (filled with result if successful)
    //   - timeout_sec: Maximum wait time (-1 = wait forever)
    //
    // RETURNS:
    //   - true: Result retrieved successfully
    //   - false: Timeout expired or controller stopped
    //
    // BEHAVIOR:
    // - Polls completed_jobs_ every 100ms
    // - If found: Copies result, returns true
    // - If timeout: Returns false
    // - If controller stops: Returns false immediately
    //
    // BLOCKING:
    // - Blocks calling thread until result available or timeout
    // - Use timeout to avoid indefinite blocking
    //
    // THREAD SAFETY:
    // - Thread-safe: Can call from multiple threads
    // - Locks jobs_mutex_ briefly for each check
    //
    // EXAMPLE:
    //   JobResult result;
    //   if (controller.get_job_result(job_id, result, 60)) {
    //       std::cout << "Return: " << result.total_return << "\n";
    //   } else {
    //       std::cout << "Timeout waiting for result\n";
    //   }
    bool get_job_result(uint64_t job_id, JobResult& result, int timeout_sec = -1);
    
    //--------------------------------------------------------------------------
    // 6.3.4 STATISTICS & MONITORING
    //--------------------------------------------------------------------------
    
    // Get number of jobs waiting for assignment
    // THREAD SAFETY: Locks jobs_mutex_
    // RETURNS: Current pending queue size
    size_t get_pending_jobs_count() const;
    
    // Get number of jobs currently executing
    // THREAD SAFETY: Locks jobs_mutex_
    // RETURNS: Current active jobs map size
    size_t get_active_jobs_count() const;
    
    // Get number of jobs that have completed
    // THREAD SAFETY: Locks jobs_mutex_
    // RETURNS: Current completed jobs map size
    size_t get_completed_jobs_count() const;
    
    // Get number of workers currently alive
    // THREAD SAFETY: Locks workers_mutex_
    // RETURNS: Count of workers with is_alive = true
    size_t get_active_workers_count() const;
    
    // Print comprehensive statistics to log
    // Includes: Worker count, job counts, completion rate
    //
    // OUTPUT FORMAT:
    //   === Controller Statistics ===
    //   Active Workers: 5
    //   Pending Jobs: 10
    //   Active Jobs: 15
    //   Completed Jobs: 100
    //   Failed Jobs: 5
    //   ===========================
    //
    // THREAD SAFETY: Locks mutexes to read statistics
    void print_statistics() const;
    
private:
    //==========================================================================
    // SECTION 6.4: PRIVATE IMPLEMENTATION METHODS
    //==========================================================================
    //
    // Internal methods for controller implementation.
    // Not accessible to users or subclasses (use protected if needed by Raft).
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // 6.4.1 NETWORK OPERATIONS
    //--------------------------------------------------------------------------
    
    // Setup TCP server socket (bind to port, listen)
    // Called by: start() method
    //
    // RETURNS:
    //   - true: Socket created and listening
    //   - false: Failed (port in use, permission denied, etc.)
    //
    // SIDE EFFECTS:
    //   - Sets server_socket_ on success
    //   - Logs error on failure
    //
    // SOCKET OPTIONS:
    //   - SO_REUSEADDR: Allow quick restart (avoid TIME_WAIT)
    //   - Backlog: 10 pending connections
    bool setup_server_socket();
    
    // Accept new worker connections (runs in background thread)
    // Loop: While running_, accept connections
    // For each connection: Spawn worker thread
    //
    // BLOCKING:
    //   - Blocks on accept() until connection or error
    //   - Unblocked by: stop() closing server_socket_
    //
    // THREAD SAFETY:
    //   - Only reads server_socket_ (no concurrent writes)
    //   - Modifies worker_threads_ map (potential race, but detached threads)
    void accept_connections();
    
    // Handle communication with single worker (runs in per-worker thread)
    // Loop: Receive messages, process, send responses
    //
    // MESSAGE TYPES HANDLED:
    //   - WORKER_REGISTER: Register new worker
    //   - HEARTBEAT: Update worker health
    //   - JOB_RESULT: Mark job complete
    //
    // CLEANUP:
    //   - On exit: Remove worker from registry
    //   - Close socket
    //   - Thread terminates
    //
    // PARAMETERS:
    //   - client_socket: Socket for this worker
    void handle_worker_connection(int client_socket);
    
    //--------------------------------------------------------------------------
    // 6.4.2 WORKER MANAGEMENT
    //--------------------------------------------------------------------------
    
    // Register new worker in registry
    //
    // PARAMETERS:
    //   - socket_fd: Worker's socket
    //   - hostname: Worker's hostname or IP
    //   - port: Worker's listening port
    //
    // RETURNS:
    //   - uint64_t: Assigned worker ID
    //
    // SIDE EFFECTS:
    //   - Adds to workers_ map
    //   - Adds to worker_sockets_ set
    //   - Wakes scheduler thread
    //
    // THREAD SAFETY: Locks workers_mutex_
    uint64_t register_worker(int socket_fd, const std::string& hostname, uint16_t port);
    
    // Remove worker from registry (failure or disconnect)
    //
    // PARAMETERS:
    //   - worker_id: Worker to remove
    //
    // SIDE EFFECTS:
    //   - Marks worker as dead
    //   - Reassigns active jobs to pending queue
    //   - Removes from workers_ map
    //   - Removes from worker_sockets_ set
    //   - Wakes scheduler thread
    //
    // THREAD SAFETY: Locks workers_mutex_ and jobs_mutex_
    void remove_worker(uint64_t worker_id);
    
    // Monitor worker heartbeats (runs in background thread)
    // Loop: Every 2 seconds, check for timeouts
    //
    // FAILURE DETECTION:
    //   - If (now - last_heartbeat) > timeout: Mark as failed
    //   - Calls remove_worker() for failed workers
    //
    // THREAD SAFETY: Locks workers_mutex_
    void check_worker_heartbeats();
    
    //--------------------------------------------------------------------------
    // 6.4.3 JOB SCHEDULING
    //--------------------------------------------------------------------------
    
    // Job scheduler (runs in background thread)
    // Loop: Wait for jobs, select worker, assign
    //
    // ALGORITHM:
    //   1. Wait on scheduler_cv_ (until jobs or wake signal)
    //   2. Select worker (least loaded strategy)
    //   3. Assign job to worker
    //   4. Repeat
    //
    // WAITING STRATEGY:
    //   - Condition variable with 100ms timeout
    //   - Predicate: pending_jobs_ not empty
    //   - Woken by: submit_job(), register_worker(), remove_worker()
    //
    // THREAD SAFETY: Locks jobs_mutex_ and workers_mutex_
    void scheduler_loop();
    
    // Assign specific job to specific worker
    //
    // PARAMETERS:
    //   - job: Job to assign
    //   - worker: Worker to assign to
    //
    // BEHAVIOR:
    //   - Sends JOB_ASSIGN message
    //   - Updates job state (pending → active)
    //   - Records assignment time
    //   - Increments worker active_jobs count
    //
    // ERROR HANDLING:
    //   - If send fails: Re-queue job to pending
    //
    // THREAD SAFETY: Locks jobs_mutex_
    void assign_job_to_worker(const JobInfo& job, WorkerInfo& worker);
    
    // Send job assignment message to worker
    //
    // PARAMETERS:
    //   - socket_fd: Worker socket
    //   - job: Job to send
    //
    // RETURNS:
    //   - true: Sent successfully
    //   - false: Network error
    //
    // THREAD SAFETY: Stateless (thread-safe)
    bool send_job_to_worker(int socket_fd, const JobInfo& job);
    
    // Select worker using round-robin strategy
    // RETURNS: Pointer to selected worker (nullptr if none available)
    //
    // ALGORITHM:
    //   - Iterate through workers_ map
    //   - Return first alive worker
    //
    // CHARACTERISTICS:
    //   - Simple, fair
    //   - Ignores current load
    //   - Good for: Equal job durations
    //
    // THREAD SAFETY: Locks workers_mutex_
    WorkerInfo* select_worker_round_robin();
    
    // Select worker using least-loaded strategy (DEFAULT)
    // RETURNS: Pointer to worker with fewest active jobs
    //
    // ALGORITHM:
    //   - Iterate through workers_ map
    //   - Track worker with minimum active_jobs
    //   - Return that worker
    //
    // CHARACTERISTICS:
    //   - Load-aware
    //   - Dynamic adaptation
    //   - Good for: Variable job durations
    //
    // THREAD SAFETY: Locks workers_mutex_
    WorkerInfo* select_worker_least_loaded();
    
    //--------------------------------------------------------------------------
    // 6.4.4 MESSAGE HANDLING
    //--------------------------------------------------------------------------
    
    // Send message over socket
    //
    // PROTOCOL:
    //   1. Serialize message to binary
    //   2. Send 4-byte size prefix
    //   3. Send message data
    //
    // PARAMETERS:
    //   - socket_fd: Destination socket
    //   - msg: Message to send
    //
    // RETURNS:
    //   - true: Sent successfully
    //   - false: Network error or serialization failure
    //
    // FLAGS: MSG_NOSIGNAL (prevent SIGPIPE)
    //
    // THREAD SAFETY: Stateless (thread-safe)
    bool send_message(int socket_fd, const Message& msg);
    
    // Receive message from socket
    //
    // PROTOCOL:
    //   1. Receive 4-byte size prefix
    //   2. Allocate buffer
    //   3. Receive message data
    //   4. Parse type and deserialize
    //
    // PARAMETERS:
    //   - socket_fd: Source socket
    //
    // RETURNS:
    //   - unique_ptr<Message>: Deserialized message
    //   - nullptr: Error or connection closed
    //
    // BLOCKING: Blocks until message received or error
    //
    // THREAD SAFETY: Stateless (thread-safe)
    std::unique_ptr<Message> receive_message(int socket_fd);
    
    // Send heartbeat acknowledgment to worker
    //
    // PARAMETERS:
    //   - socket_fd: Worker socket
    //
    // RETURNS:
    //   - true: Sent successfully
    //   - false: Network error
    //
    // NOTE: Optional, worker doesn't wait for this
    bool send_heartbeat_ack(int socket_fd);
    
    //--------------------------------------------------------------------------
    // 6.4.5 CLEANUP
    //--------------------------------------------------------------------------
    
    // Close socket cleanly (shutdown + close)
    //
    // PARAMETERS:
    //   - fd: Socket file descriptor to close
    //
    // BEHAVIOR:
    //   - shutdown(SHUT_RDWR): Disable sends/receives
    //   - close(): Release file descriptor
    //
    // THREAD SAFETY: Safe if no concurrent operations on socket
    void close_socket(int fd);
};

} // namespace backtesting

#endif // CONTROLLER_H

//==============================================================================
// END OF HEADER FILE
//==============================================================================
//
// Implementation of all methods is in controller.cpp
// See controller.cpp for detailed implementation documentation
//
//==============================================================================

//==============================================================================
// SECTION 7: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Basic Controller Setup
// ==================================
//
// #include "controller/controller.h"
//
// int main() {
//     // Configure logging
//     Logger::set_level(LogLevel::INFO);
//     
//     // Create controller with default config
//     Controller controller;
//     
//     // Start controller
//     if (!controller.start()) {
//         Logger::error("Failed to start controller");
//         return 1;
//     }
//     
//     Logger::info("Controller running, waiting for workers...");
//     
//     // Keep running
//     std::this_thread::sleep_for(std::chrono::hours(1));
//     
//     // Graceful shutdown
//     controller.stop();
//     
//     return 0;
// }
//
//
// EXAMPLE 2: Custom Configuration
// ================================
//
// int main() {
//     // Custom configuration
//     ControllerConfig config;
//     config.listen_port = 6000;               // Use port 6000
//     config.heartbeat_timeout_sec = 10;       // 10-second timeout
//     config.max_workers = 20;                 // Allow up to 20 workers
//     config.data_directory = "/mnt/nfs/data"; // NFS mount point
//     
//     // Create controller with custom config
//     Controller controller(config);
//     
//     if (!controller.start()) {
//         return 1;
//     }
//     
//     // ... use controller ...
// }
//
//
// EXAMPLE 3: Submitting Jobs and Retrieving Results
// ==================================================
//
// void run_backtests(Controller& controller) {
//     // Prepare job parameters
//     std::vector<std::string> symbols = {"AAPL", "GOOGL", "MSFT", "AMZN"};
//     std::vector<uint64_t> job_ids;
//     
//     // Submit all jobs
//     for (const auto& symbol : symbols) {
//         JobParams params;
//         params.symbol = symbol;
//         params.strategy_type = "SMA";
//         params.start_date = "2020-01-01";
//         params.end_date = "2024-12-31";
//         params.short_window = 50;
//         params.long_window = 200;
//         params.initial_capital = 10000.0;
//         
//         uint64_t job_id = controller.submit_job(params);
//         job_ids.push_back(job_id);
//         
//         Logger::info("Submitted job " + std::to_string(job_id) + 
//                     " for " + symbol);
//     }
//     
//     // Wait for all results
//     for (uint64_t job_id : job_ids) {
//         JobResult result;
//         
//         if (controller.get_job_result(job_id, result, 60)) {
//             Logger::info("Job " + std::to_string(job_id) + 
//                         " return: " + std::to_string(result.total_return * 100) + 
//                         "%, Sharpe: " + std::to_string(result.sharpe_ratio));
//         } else {
//             Logger::error("Timeout waiting for job " + std::to_string(job_id));
//         }
//     }
// }
//
//
// EXAMPLE 4: Monitoring Controller Status
// ========================================
//
// void monitor_loop(Controller& controller) {
//     while (controller.is_running()) {
//         std::this_thread::sleep_for(std::chrono::seconds(10));
//         
//         // Print statistics
//         controller.print_statistics();
//         
//         // Check health
//         if (controller.get_active_workers_count() == 0) {
//             Logger::warning("No active workers!");
//         }
//         
//         if (controller.get_pending_jobs_count() > 100) {
//             Logger::warning("Large backlog: " + 
//                           std::to_string(controller.get_pending_jobs_count()) + 
//                           " pending jobs");
//         }
//     }
// }
//
//
// EXAMPLE 5: Graceful Shutdown with Signal Handling
// ==================================================
//
// #include <signal.h>
//
// Controller* g_controller = nullptr;
//
// void signal_handler(int signal) {
//     Logger::info("Received signal " + std::to_string(signal) + ", shutting down...");
//     if (g_controller) {
//         g_controller->stop();
//     }
//     exit(0);
// }
//
// int main() {
//     Controller controller;
//     g_controller = &controller;
//     
//     // Register signal handlers for graceful shutdown
//     signal(SIGINT, signal_handler);   // Ctrl+C
//     signal(SIGTERM, signal_handler);  // kill command
//     
//     if (!controller.start()) {
//         return 1;
//     }
//     
//     // Run until signaled
//     while (controller.is_running()) {
//         std::this_thread::sleep_for(std::chrono::seconds(1));
//     }
//     
//     return 0;
// }
//
//==============================================================================

//==============================================================================
// SECTION 8: THREAD SAFETY ANALYSIS
//==============================================================================
//
// SHARED STATE OVERVIEW:
// ======================
//
// Protected by workers_mutex_:
//   - workers_ (map<uint64_t, WorkerInfo>)
//   - worker_sockets_ (set<int>)
//
// Protected by jobs_mutex_:
//   - pending_jobs_ (queue<JobInfo>)
//   - active_jobs_ (map<uint64_t, JobInfo>)
//   - completed_jobs_ (map<uint64_t, JobInfo>)
//
// Atomic (no lock needed):
//   - running_ (std::atomic<bool>)
//   - next_job_id_ (std::atomic<uint64_t>)
//   - next_worker_id_ (std::atomic<uint64_t>)
//   - total_jobs_completed_ (std::atomic<uint64_t>)
//   - total_jobs_failed_ (std::atomic<uint64_t>)
//
// Immutable (no synchronization needed):
//   - config_ (set in constructor, never modified)
//
// Thread-local (no sharing):
//   - server_socket_ (only accept thread accesses after start())
//
// LOCK ACQUISITION ORDER:
// =======================
// To prevent deadlock, always acquire mutexes in this order:
//   1. workers_mutex_
//   2. jobs_mutex_
//
// NEVER acquire in reverse order!
//
// Correct pattern:
//   std::lock_guard<std::mutex> workers_lock(workers_mutex_);
//   {
//       std::lock_guard<std::mutex> jobs_lock(jobs_mutex_);
//       // Access both workers and jobs
//   }
//
// THREAD INTERACTIONS:
// ====================
//
// Main Thread:
//   - Calls: start(), submit_job(), get_job_result(), stop()
//   - Locks: jobs_mutex_ (submit_job, get_job_result)
//
// Accept Thread:
//   - Calls: accept_connections()
//   - Locks: None (only reads server_socket_)
//   - Spawns: Worker threads
//
// Scheduler Thread:
//   - Calls: scheduler_loop()
//   - Locks: jobs_mutex_, workers_mutex_
//   - Waits on: scheduler_cv_
//
// Heartbeat Thread:
//   - Calls: check_worker_heartbeats()
//   - Locks: workers_mutex_, jobs_mutex_ (via remove_worker)
//   - Sleeps: 2 seconds between checks
//
// Worker Threads (one per worker):
//   - Calls: handle_worker_connection()
//   - Locks: workers_mutex_, jobs_mutex_
//   - Blocks on: recv() system call
//
// CONDITION VARIABLE USAGE:
// =========================
//
// scheduler_cv_ usage pattern:
//
//   Producer (submit_job):
//     {
//         std::lock_guard<std::mutex> lock(jobs_mutex_);
//         pending_jobs_.push(job);
//     }
//     scheduler_cv_.notify_one();  // Wake scheduler
//
//   Consumer (scheduler_loop):
//     std::unique_lock<std::mutex> lock(jobs_mutex_);
//     scheduler_cv_.wait(lock, []() {
//         return !pending_jobs_.empty();
//     });
//     JobInfo job = pending_jobs_.front();
//     pending_jobs_.pop();
//     // Process job
//
// ATOMIC OPERATIONS:
// ==================
//
// Why use std::atomic for counters?
//   - Lock-free: No mutex overhead
//   - Thread-safe: Guaranteed atomicity
//   - Efficient: Single CPU instruction for increment
//
// Example:
//   next_job_id_.fetch_add(1, std::memory_order_relaxed);
//   // Atomic increment, returns previous value
//
// Memory ordering:
//   - relaxed: No ordering constraints (fastest)
//   - Sufficient for counters (no dependent operations)
//
// DEADLOCK PREVENTION:
// ====================
//
// Strategies used:
//   1. Lock hierarchy: Always acquire in same order
//   2. RAII locking: std::lock_guard prevents forgetting unlock
//   3. Minimize lock scope: Lock, operate, unlock quickly
//   4. Avoid nested locks: Unlock before calling functions that lock
//
// Example of lock scope minimization:
//   WorkerInfo* worker;
//   {
//       std::lock_guard<std::mutex> lock(workers_mutex_);
//       worker = select_worker_least_loaded();
//   }  // Unlock before network I/O
//   
//   send_job_to_worker(worker->socket_fd, job);  // No lock held
//
//==============================================================================

//==============================================================================
// SECTION 9: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: Calling start() Multiple Times
// ==========================================
// PROBLEM:
//    controller.start();
//    controller.start();  // Second call fails
//
// SOLUTION:
//    if (!controller.is_running()) {
//        controller.start();
//    }
//
//
// PITFALL 2: Not Checking start() Return Value
// =============================================
// WRONG:
//    controller.start();  // May fail!
//    controller.submit_job(params);  // Jobs go to stopped controller
//
// CORRECT:
//    if (!controller.start()) {
//        Logger::error("Failed to start");
//        return 1;
//    }
//
//
// PITFALL 3: Blocking Main Thread with get_job_result()
// ======================================================
// PROBLEM:
//    for (int i = 0; i < 100; ++i) {
//        uint64_t id = controller.submit_job(params);
//        JobResult result;
//        controller.get_job_result(id, result);  // Blocks!
//        // Only one job executes at a time (serialized)
//    }
//
// SOLUTION:
//    // Submit all jobs first
//    std::vector<uint64_t> ids;
//    for (int i = 0; i < 100; ++i) {
//        ids.push_back(controller.submit_job(params));
//    }
//    
//    // Then wait for results (parallel execution)
//    for (uint64_t id : ids) {
//        JobResult result;
//        controller.get_job_result(id, result);
//    }
//
//
// PITFALL 4: Memory Leak from Unbounded completed_jobs_
// ======================================================
// PROBLEM:
//    // Submit 1,000,000 jobs
//    // All results stored in completed_jobs_ forever
//    // Memory grows indefinitely
//
// SOLUTION:
//    // Periodically clean up old results (not implemented)
//    void cleanup_old_results() {
//        std::lock_guard<std::mutex> lock(jobs_mutex_);
//        auto threshold = std::chrono::steady_clock::now() - 
//                        std::chrono::hours(24);
//        
//        for (auto it = completed_jobs_.begin(); 
//             it != completed_jobs_.end(); ) {
//            if (it->second.assigned_time < threshold) {
//                it = completed_jobs_.erase(it);
//            } else {
//                ++it;
//            }
//        }
//    }
//
//
// PITFALL 5: Ignoring Mutex Order (Deadlock Risk)
// ================================================
// WRONG (potential deadlock):
//    void function_a() {
//        std::lock_guard<std::mutex> lock1(jobs_mutex_);
//        {
//            std::lock_guard<std::mutex> lock2(workers_mutex_);
//            // ...
//        }
//    }
//    
//    void function_b() {
//        std::lock_guard<std::mutex> lock1(workers_mutex_);
//        {
//            std::lock_guard<std::mutex> lock2(jobs_mutex_);  // DEADLOCK!
//            // ...
//        }
//    }
//
// CORRECT (consistent order):
//    // Always: workers_mutex_ → jobs_mutex_
//    void both_functions() {
//        std::lock_guard<std::mutex> lock1(workers_mutex_);
//        {
//            std::lock_guard<std::mutex> lock2(jobs_mutex_);
//            // ...
//        }
//    }
//
//
// PITFALL 6: Not Handling Worker Disconnection
// =============================================
// PROBLEM:
//    // Worker crashes mid-job
//    // Job stuck in active_jobs_ forever
//
// SOLUTION (already implemented):
//    // Heartbeat monitoring detects failure
//    // remove_worker() reassigns jobs automatically
//    // No manual intervention needed
//
//
// PITFALL 7: Forgetting to Stop Before Destruction
// =================================================
// WRONG:
//    {
//        Controller controller;
//        controller.start();
//        // ... use controller ...
//    }  // Destructor called while threads running (but safe due to destructor calling stop)
//
// BETTER (explicit):
//    {
//        Controller controller;
//        controller.start();
//        // ... use controller ...
//        controller.stop();  // Explicit cleanup
//    }  // Destructor does nothing (already stopped)
//
// Note: Destructor calls stop(), but explicit is clearer
//
//==============================================================================

//==============================================================================
// SECTION 10: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: How many workers can connect to a single controller?
// =========================================================
// A: Limited by system resources:
//    - File descriptors: ulimit -n (typically 1024)
//    - Memory: Each worker ~100 bytes metadata
//    - Threads: Each worker gets thread (typically 10,000+ limit)
//    - Practical: 2-100 workers per controller
//    - This project: 2-8 workers (Khoury cluster size)
//
// Q2: What happens if all workers fail?
// ======================================
// A: Jobs remain in pending queue. When new worker connects:
//    - Scheduler automatically assigns pending jobs
//    - No jobs are lost
//    - System recovers automatically
//
// Q3: Can multiple threads call submit_job() concurrently?
// =========================================================
// A: Yes! submit_job() is thread-safe:
//    - Uses std::atomic for job ID generation
//    - Locks jobs_mutex_ for queue insertion
//    - Safe to call from multiple threads
//
// Q4: What's the difference between this and RaftController?
// ===========================================================
// A: This is single-controller (no fault tolerance):
//    - If controller crashes, system goes down
//    - Simpler implementation (no Raft complexity)
//    
//    RaftController (future):
//    - 3 controllers in cluster
//    - Leader election, log replication
//    - Controller fault tolerance
//    - Same interface (virtual methods)
//
// Q5: How do I know when all jobs are complete?
// ==============================================
// A: Check counters:
//    if (controller.get_pending_jobs_count() == 0 &&
//        controller.get_active_jobs_count() == 0) {
//        // All jobs complete
//    }
//    
//    Or track job IDs and wait for each:
//    for (uint64_t id : submitted_ids) {
//        controller.get_job_result(id, result);
//    }
//
// Q6: Can I submit jobs before any workers connect?
// ==================================================
// A: Yes! Jobs wait in pending queue. When workers register,
//    scheduler automatically assigns jobs. This is normal behavior.
//
// Q7: How fast can controller process jobs?
// ==========================================
// A: Throughput depends on:
//    - Worker count: More workers = higher throughput
//    - Job duration: Faster jobs = higher throughput
//    - Network latency: Khoury cluster is low-latency (~0.1ms)
//    
//    Typical: 10-100 jobs/second with 8 workers
//    Limited by: Worker computation time (not controller)
//
// Q8: What if worker sends result for unknown job_id?
// ====================================================
// A: Result is silently ignored (no match in active_jobs_).
//    Could happen if:
//    - Worker delayed, job already timed out and reassigned
//    - Protocol error, corrupted job_id
//    - Should be rare, logged as warning
//
// Q9: Can I change configuration after construction?
// ===================================================
// A: No, config_ is immutable. Create new Controller with new config.
//    Alternative: Make config_ non-const and add set_config() method
//    (not implemented)
//
// Q10: Why are some methods virtual?
// ===================================
// A: For RaftController extension:
//    - start(): Add Raft initialization
//    - stop(): Add Raft cleanup
//    - submit_job(): Replicate job in Raft log
//    
//    Non-virtual methods: Same behavior in Raft version
//
//==============================================================================

//==============================================================================
// SECTION 11: BEST PRACTICES
//==============================================================================
//
// BEST PRACTICE 1: Always Check Error Returns
// ============================================
// DO:
//    if (!controller.start()) {
//        // Handle error
//    }
//    
//    if (!controller.get_job_result(id, result, 60)) {
//        // Handle timeout
//    }
//
// DON'T:
//    controller.start();  // Ignore return value
//
//
// BEST PRACTICE 2: Use RAII for Lifecycle Management
// ===================================================
// DO:
//    {
//        Controller controller(config);
//        controller.start();
//        // ... use controller ...
//        // Automatic cleanup on scope exit
//    }
//
// DON'T:
//    Controller* controller = new Controller(config);
//    // ... forget to delete ...
//
//
// BEST PRACTICE 3: Configure Appropriate Timeouts
// ================================================
// Balance responsiveness and false positives:
//    - Heartbeat interval: 2 seconds
//    - Heartbeat timeout: 3× interval = 6 seconds
//    - Job result timeout: Based on expected duration
//
//
// BEST PRACTICE 4: Monitor Statistics Regularly
// ==============================================
// DO:
//    // Periodic monitoring thread
//    std::thread monitor([&controller]() {
//        while (controller.is_running()) {
//            std::this_thread::sleep_for(std::chrono::seconds(30));
//            controller.print_statistics();
//        }
//    });
//
//
// BEST PRACTICE 5: Handle Signals for Clean Shutdown
// ===================================================
// DO:
//    signal(SIGINT, signal_handler);
//    signal(SIGTERM, signal_handler);
//    // In handler: controller.stop()
//
// Ensures:
//    - Threads joined properly
//    - Sockets closed cleanly
//    - Resources released
//
//
// BEST PRACTICE 6: Use Least-Loaded Strategy
// ===========================================
// Default: select_worker_least_loaded()
//    - Better load distribution
//    - Handles variable job durations
//    - Recommended for most use cases
//
// Round-robin: select_worker_round_robin()
//    - Simpler
//    - Good only for uniform jobs
//    - Not recommended unless specific requirement
//
//
// BEST PRACTICE 7: Log Important Events
// ======================================
// Already implemented, but when extending:
//    - Worker registration/removal
//    - Job submission/completion
//    - Errors and timeouts
//    - Performance metrics
//
//
// BEST PRACTICE 8: Test Failure Scenarios
// ========================================
// Test cases:
//    - All workers fail → jobs wait for new workers
//    - Worker fails mid-job → job reassigned
//    - Controller restart → workers reconnect
//    - Network partition → heartbeat timeout, reassignment
//
//==============================================================================

//==============================================================================
// SECTION 12: PERFORMANCE CONSIDERATIONS
//==============================================================================
//
// PERFORMANCE BOTTLENECKS:
// ========================
//
// 1. NETWORK I/O (Primary Bottleneck)
//    - send()/recv() system calls: ~10-100 μs each
//    - Mitigation: Already optimized (binary protocol)
//    - Not much room for improvement
//
// 2. LOCK CONTENTION (Secondary)
//    - jobs_mutex_: Low contention (fast operations)
//    - workers_mutex_: Low contention (infrequent updates)
//    - Mitigation: Fine-grained locking, short critical sections
//
// 3. SCHEDULER WAKE-UP (Minor)
//    - Condition variable wait: ~1-100 μs
//    - Timeout: 100ms (trades off responsiveness vs. CPU)
//    - Mitigation: Already optimized with condition variables
//
// SCALABILITY ANALYSIS:
// =====================
//
// Time Complexity:
//   - submit_job(): O(log n) where n = pending jobs (map insertion)
//   - Worker selection: O(m) where m = workers (iterate all)
//   - Heartbeat check: O(m) (iterate all workers)
//   - Job assignment: O(1) amortized (queue operations)
//
// Space Complexity:
//   - Workers: O(m) - one WorkerInfo per worker
//   - Jobs: O(k) where k = total jobs (pending + active + completed)
//   - Memory: ~100 bytes/worker + ~200 bytes/job
//
// Scalability Limits:
//   - Workers: 100-1000 (before iteration overhead matters)
//   - Jobs: 100,000-1,000,000 (before memory concerns)
//   - This project: 8 workers, 100-1000 jobs (well within limits)
//
// OPTIMIZATION OPPORTUNITIES:
// ===========================
//
// 1. Use unordered_map instead of map:
//    - O(1) lookup instead of O(log n)
//    - Trade-off: Non-deterministic iteration order
//    - Benefit: Minimal (log n is fast for small n)
//
// 2. Lock-free queue for pending_jobs:
//    - Eliminate mutex overhead
//    - Libraries: Boost.Lockfree, Folly
//    - Complexity: Higher
//    - Benefit: Minimal (queue operations are fast)
//
// 3. Thread pool instead of thread-per-worker:
//    - Reuse threads, avoid spawn overhead
//    - For 8 workers: Negligible benefit
//    - For 100+ workers: Significant benefit
//
// 4. Batch heartbeat checks:
//    - Check every 6 seconds instead of 2 seconds
//    - Trade-off: Slower failure detection
//    - Benefit: 3x less CPU usage
//
// RECOMMENDATION:
// - Current design is appropriate for project scale
// - Optimizations not needed unless profiling shows bottleneck
//
//==============================================================================

//==============================================================================
// SECTION 13: RAFT INTEGRATION GUIDE
//==============================================================================
//
// EXTENDING CONTROLLER WITH RAFT CONSENSUS
// =========================================
//
// Design allows future RaftController to extend Controller:
//
// class RaftController : public Controller {
// protected:
//     // Raft-specific state
//     RaftState raft_state_;
//     std::vector<RaftPeer> peers_;
//     RaftLog log_;
//     
// public:
//     bool start() override {
//         // Call base class start
//         if (!Controller::start()) return false;
//         
//         // Add Raft initialization
//         InitializeRaft();
//         StartElectionTimer();
//         
//         return true;
//     }
//     
//     void stop() override {
//         // Raft cleanup first
//         StopRaft();
//         
//         // Then base class cleanup
//         Controller::stop();
//     }
//     
//     uint64_t submit_job(const JobParams& params) override {
//         // Replicate job in Raft log
//         uint64_t job_id = next_job_id_++;
//         AppendJobToRaftLog(job_id, params);
//         
//         // Wait for commit
//         WaitForCommit();
//         
//         // Then actually submit (call base class)
//         return Controller::submit_job(params);
//     }
// };
//
// PROTECTED MEMBERS ENABLE:
// - Access to workers_ (for state replication)
// - Access to pending_jobs_ (for log replay)
// - Override virtual methods (customize behavior)
//
// RAFT WILL ADD:
// - Leader election
// - Log replication
// - Snapshot/restore
// - Cluster membership management
//
//==============================================================================

//==============================================================================
// SECTION 14: TESTING STRATEGIES
//==============================================================================
//
// UNIT TESTS:
// ===========
//
// TEST(ControllerTest, StartStop) {
//     Controller controller;
//     EXPECT_TRUE(controller.start());
//     EXPECT_TRUE(controller.is_running());
//     controller.stop();
//     EXPECT_FALSE(controller.is_running());
// }
//
// TEST(ControllerTest, SubmitJob) {
//     Controller controller;
//     controller.start();
//     
//     JobParams params;
//     params.symbol = "AAPL";
//     
//     uint64_t id = controller.submit_job(params);
//     EXPECT_GT(id, 0);
//     EXPECT_EQ(controller.get_pending_jobs_count(), 1);
// }
//
// TEST(ControllerTest, WorkerFailover) {
//     // Start controller
//     // Connect worker
//     // Assign job
//     // Kill worker
//     // Verify job reassigned
//     // Verify new worker gets job
// }
//
//
// INTEGRATION TESTS:
// ==================
//
// TEST(IntegrationTest, FullJobLifecycle) {
//     // Start controller
//     Controller controller;
//     controller.start();
//     
//     // Start worker (separate process)
//     Worker worker;
//     worker.connect(controller_host, controller_port);
//     
//     // Submit job
//     uint64_t id = controller.submit_job(params);
//     
//     // Wait for result
//     JobResult result;
//     EXPECT_TRUE(controller.get_job_result(id, result, 30));
//     EXPECT_TRUE(result.success);
// }
//
//
// STRESS TESTS:
// =============
//
// TEST(StressTest, ManyJobs) {
//     Controller controller;
//     controller.start();
//     
//     // Submit 1000 jobs
//     for (int i = 0; i < 1000; ++i) {
//         controller.submit_job(params);
//     }
//     
//     // Verify all complete
//     while (controller.get_completed_jobs_count() < 1000) {
//         std::this_thread::sleep_for(std::chrono::seconds(1));
//     }
// }
//
//
// MOCK TESTING:
// =============
//
// Use dependency injection for testing:
//
// class MockWorker {
//     void connect(Controller& controller) {
//         // Mock worker behavior
//         // Send registration
//         // Send heartbeats
//         // Process jobs
//     }
// };
//
// TEST(ControllerTest, WithMockWorkers) {
//     Controller controller;
//     controller.start();
//     
//     MockWorker worker1, worker2;
//     worker1.connect(controller);
//     worker2.connect(controller);
//     
//     // Test load balancing, failure detection, etc.
// }
//
//==============================================================================

//==============================================================================
// SECTION 15: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM: Controller fails to start
// ===================================
// SYMPTOMS:
//    - start() returns false
//    - Log: "Failed to setup server socket"
//
// CAUSES & SOLUTIONS:
//    ☐ Port in use: netstat -tulpn | grep 5000
//       → Change port or kill process using it
//    
//    ☐ Permission denied: Ports < 1024 require root
//       → Use port >= 1024 or run as root (not recommended)
//    
//    ☐ Firewall blocking: Check iptables rules
//       → Add rule to allow port
//
//
// PROBLEM: Workers not connecting
// ================================
// SYMPTOMS:
//    - get_active_workers_count() returns 0
//    - No "Accepted connection" logs
//
// CAUSES & SOLUTIONS:
//    ☐ Controller not started: Call start() first
//    
//    ☐ Wrong port: Verify worker uses same port
//    
//    ☐ Network issue: Test with telnet
//       → telnet controller_host 5000
//    
//    ☐ Firewall: Check both controller and worker firewalls
//
//
// PROBLEM: Jobs never complete
// =============================
// SYMPTOMS:
//    - get_job_result() times out
//    - Pending or active jobs stuck
//
// CAUSES & SOLUTIONS:
//    ☐ No workers: Check get_active_workers_count()
//       → Start workers
//    
//    ☐ All workers failed: Check logs for timeouts
//       → Restart workers
//    
//    ☐ Worker hung: Check worker logs/CPU
//       → Kill and restart worker
//    
//    ☐ Network partition: Check connectivity
//       → Fix network or wait for timeout
//
//
// PROBLEM: High memory usage
// ===========================
// SYMPTOMS:
//    - RSS growing over time
//    - System running out of memory
//
// CAUSES & SOLUTIONS:
//    ☐ Unbounded completed_jobs_: Implement cleanup
//       → Periodically remove old results
//    
//    ☐ Too many pending jobs: Limit submission rate
//       → Batch jobs, wait for completion
//    
//    ☐ Memory leak: Run Valgrind
//       → valgrind --leak-check=full ./controller
//
//
// PROBLEM: Deadlock (controller hangs)
// =====================================
// SYMPTOMS:
//    - Controller unresponsive
//    - Threads not making progress
//
// CAUSES & SOLUTIONS:
//    ☐ Mutex order violation: Check lock acquisition order
//       → Always: workers_mutex_ before jobs_mutex_
//    
//    ☐ Thread waiting forever: Check condition variables
//       → Ensure notify_one()/notify_all() called
//    
//    ☐ Debug with gdb:
//       → gdb -p <pid>
//       → thread apply all bt  (show all thread backtraces)
//
//
// PROBLEM: High CPU usage
// ========================
// SYMPTOMS:
//    - Controller process at 100% CPU
//    - System sluggish
//
// CAUSES & SOLUTIONS:
//    ☐ Busy-wait loop: Check for while(true) without sleep
//       → Use condition variables or sleep
//    
//    ☐ Too many threads: Reduce worker count
//       → Use thread pool
//    
//    ☐ Excessive logging: Reduce log level
//       → Logger::set_level(LogLevel::INFO)
//
//==============================================================================

//==============================================================================
// ARCHITECTURAL NOTES FOR DISTRIBUTED SYSTEMS
//==============================================================================
//
// CONSISTENCY MODEL:
// ==================
// - Eventual consistency: Job results eventually visible
// - No strong ordering: Jobs may complete out of submission order
// - Idempotency: Job resubmission not handled (IDs are unique)
//
// FAILURE MODES:
// ==============
//
// 1. Worker failure:
//    - Detection: Heartbeat timeout (6 seconds)
//    - Recovery: Automatic job reassignment
//    - Impact: Delayed completion (job reruns)
//
// 2. Controller failure (current implementation):
//    - Detection: None (single controller)
//    - Recovery: Manual restart (jobs lost)
//    - Impact: System unavailable
//    - Mitigation: Future RaftController
//
// 3. Network partition:
//    - Detection: Heartbeat timeout
//    - Recovery: Jobs reassigned, may duplicate
//    - Impact: Wasted computation
//
// CAP THEOREM IMPLICATIONS:
// =========================
// - Consistency: Job executed exactly once (mostly)
// - Availability: Available if any worker alive
// - Partition tolerance: Partial (reassigns jobs)
//
// Trade-off: Favor availability over strict consistency
//
// SCALABILITY LIMITS:
// ===================
// - Horizontal: Add more workers (linear speedup)
// - Vertical: Single controller bottleneck
// - Solution: Raft controller cluster (3-5 controllers)
//
//==============================================================================

//==============================================================================
// COMPARISON WITH INDUSTRY FRAMEWORKS
//==============================================================================
//
// SIMILAR TO:
// - Apache Spark: Driver (controller) + Executors (workers)
// - Hadoop MapReduce: JobTracker + TaskTrackers
// - Kubernetes: Master node + Worker nodes
//
// DIFFERENCES:
// - Simpler: No complex scheduling algorithms
// - Specialized: Financial backtesting only
// - Educational: Learning-focused implementation
//
// PRODUCTION CONSIDERATIONS:
// - Add Raft for controller fault tolerance
// - Add authentication (TLS, certificates)
// - Add rate limiting (prevent DoS)
// - Add monitoring (Prometheus, Grafana)
// - Add logging aggregation (ELK stack)
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================