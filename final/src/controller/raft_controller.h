/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: raft_controller.h
    
    Description:
        This header file defines the Raft-enabled controller infrastructure for 
        the distributed backtesting system. It provides fault-tolerant job 
        scheduling and coordination through Raft consensus protocol implementation.
        
        Key Components:
        - NetworkRaftNode: Extends base Raft implementation with TCP networking
        - RaftControllerConfig: Configuration parameters for controller setup
        - ControllerPeer: Metadata for peer controller nodes in cluster
        - RaftController: Main controller class integrating Raft consensus with
          job scheduling, worker management, and distributed coordination
        
        The controller cluster (3+ nodes) uses Raft to:
        1. Elect a leader for job scheduling decisions
        2. Replicate job queue state across all controllers
        3. Provide automatic failover when leader crashes
        4. Ensure consistency of worker assignments and job status
        
        Architecture:
        - Leader handles all job submissions and worker coordination
        - Followers replicate state and stand ready for leader election
        - Network layer manages both worker connections (port 5000) and
          inter-controller Raft traffic (port 6000)
        
    Dependencies:
        - controller/controller.h: Base Controller class
        - raft/raft_node.h: Core Raft consensus algorithm
        - raft/raft_messages.h: Raft RPC message definitions
        - POSIX sockets for TCP networking
        - Standard C++ threading and synchronization primitives
        
    Thread Safety:
        - All network operations protected by raft_network_mutex_
        - Raft state machine is internally synchronized
        - Job submission requires leadership check before proceeding
*******************************************************************************/

#ifndef RAFT_CONTROLLER_H
#define RAFT_CONTROLLER_H

#include "controller/controller.h"
#include "raft/raft_node.h"
#include "raft/raft_messages.h"
#include <memory>
#include <map>

namespace backtesting {

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
// 1. NetworkRaftNode Class
// 2. RaftControllerConfig Structure
// 3. ControllerPeer Structure  
// 4. RaftController Class
//    4.1 Public Interface
//    4.2 Private Implementation
// 5. Usage Examples
// 6. Common Pitfalls
// 7. FAQ
//==============================================================================

//==============================================================================
// SECTION 1: NetworkRaftNode Class
//==============================================================================

/**
 * @class NetworkRaftNode
 * @brief Network-enabled extension of the base Raft consensus node
 * 
 * OVERVIEW:
 * NetworkRaftNode bridges the abstract Raft algorithm with real TCP networking.
 * The base RaftNode class defines the consensus logic (leader election, log
 * replication) but delegates actual message transmission to subclasses. This
 * class implements those network operations using POSIX sockets.
 * 
 * RESPONSIBILITIES:
 * - Serialize Raft RPC messages (RequestVote, AppendEntries) to wire format
 * - Send messages over TCP to peer controller nodes
 * - Deserialize responses and return them to Raft state machine
 * - Maintain connection map to peer controllers
 * 
 * DESIGN RATIONALE:
 * The separation between consensus logic (RaftNode) and networking (this class)
 * follows the single-responsibility principle. It allows the core Raft 
 * implementation to remain transport-agnostic and testable without real sockets.
 * 
 * THREADING MODEL:
 * Multiple threads may attempt to send Raft messages concurrently:
 * - Leader sending AppendEntries heartbeats to all followers
 * - Candidate sending RequestVote to all peers during election
 * Therefore, peer_sockets_ access is protected by network_mutex_.
 * 
 * LIFETIME:
 * This object is owned by RaftController and lives for the controller's entire
 * lifetime. Socket connections are established lazily on first use and persist
 * until controller shutdown.
 */
class NetworkRaftNode : public raft::RaftNode {
private:
    /**
     * @brief Map of peer node IDs to their connected socket file descriptors
     * 
     * Key: Raft node ID (e.g., 1, 2, 3 for 3-node cluster)
     * Value: TCP socket FD for sending messages to that peer
     * 
     * IMPORTANT: This is a pointer to RaftController's peer_sockets_ map,
     * not owned by this class. RaftController manages socket lifecycle.
     * 
     * NULL sockets (value -1) indicate disconnected peers. The send methods
     * will attempt reconnection before giving up.
     */
    std::map<uint64_t, int>* peer_sockets_;
    
    /**
     * @brief Mutex protecting concurrent access to peer_sockets_ map
     * 
     * CRITICAL SECTION: Any read/write to peer_sockets_ must hold this lock
     * to prevent race conditions when multiple Raft threads send messages.
     * 
     * Shared with RaftController to coordinate with connection management.
     */
    std::mutex* network_mutex_;
    
public:
    /**
     * @brief Constructs a network-enabled Raft node
     * 
     * @param config Raft configuration (node ID, peer list, timeouts)
     * @param sockets Pointer to socket map managed by RaftController
     * @param mtx Pointer to mutex protecting the socket map
     * 
     * USAGE NOTE: Always pass valid pointers that outlive this object.
     * Typically called from RaftController constructor which owns these resources.
     * 
     * @throws std::invalid_argument if config contains invalid peer addresses
     */
    NetworkRaftNode(const raft::RaftConfig& config,
                   std::map<uint64_t, int>* sockets,
                   std::mutex* mtx)
        : raft::RaftNode(config), peer_sockets_(sockets), network_mutex_(mtx) {}
    
    /**
     * @brief Sends RequestVote RPC to peer and waits for response
     * 
     * PROTOCOL FLOW:
     * 1. Serialize req to byte buffer (term, candidateId, lastLogIndex, etc.)
     * 2. Look up socket for peer_id, reconnect if necessary
     * 3. Send message with length prefix over TCP
     * 4. Block waiting for response (with timeout)
     * 5. Deserialize response into resp
     * 
     * @param peer_id Target node's Raft ID
     * @param req Vote request containing candidate's log state
     * @param resp [out] Vote response (voteGranted, term)
     * 
     * @return true if RPC succeeded (response received), false on network error
     * 
     * TIMEOUT BEHAVIOR:
     * If peer doesn't respond within RPC timeout (~500ms), returns false.
     * Raft will retry the RPC after randomized backoff.
     * 
     * ERROR HANDLING:
     * - Socket closed: Attempts reconnection once, then fails
     * - Serialization error: Returns false, logs error
     * - Timeout: Returns false (peer may be down or partitioned)
     * 
     * THREAD SAFETY: Safe to call concurrently from multiple threads
     */
    bool send_request_vote(uint64_t peer_id, const raft::RequestVoteRequest& req,
                          raft::RequestVoteResponse& resp) override;
    
    /**
     * @brief Sends AppendEntries RPC to peer and waits for response
     * 
     * DUAL PURPOSE:
     * 1. Heartbeat: Empty entries, prevents follower timeout
     * 2. Log Replication: Contains new log entries to replicate
     * 
     * PROTOCOL FLOW:
     * Same as send_request_vote but serializes:
     * - Leader's term, prevLogIndex, prevLogTerm (for consistency check)
     * - Array of log entries to append
     * - Leader's commit index (so follower can apply committed entries)
     * 
     * @param peer_id Target follower's Raft ID
     * @param req AppendEntries request with leader state
     * @param resp [out] Response (success, term, matchIndex for leader tracking)
     * 
     * @return true if RPC succeeded, false on network/timeout error
     * 
     * FREQUENCY:
     * Leader calls this frequently (~100ms heartbeat interval) so network
     * errors are common. Don't log every failure; Raft handles retries.
     * 
     * PERFORMANCE NOTE:
     * For large log entries (e.g., 1000-entry batch), serialization can take
     * milliseconds. Consider batching limits to avoid blocking Raft thread.
     */
    bool send_append_entries(uint64_t peer_id, const raft::AppendEntriesRequest& req,
                            raft::AppendEntriesResponse& resp) override;
};

//==============================================================================
// SECTION 2: RaftControllerConfig Structure
//==============================================================================

/**
 * @struct RaftControllerConfig
 * @brief Configuration parameters for initializing a Raft controller node
 * 
 * USAGE PATTERN:
 * ```cpp
 * RaftControllerConfig config;
 * config.node_id = 1;  // This controller's ID (must be unique)
 * config.listen_port = 5000;  // Port for worker connections
 * config.raft_port = 6000;    // Port for inter-controller Raft traffic
 * config.data_directory = "/mnt/shared/data";  // Price data location
 * config.raft_log_directory = "/var/raft/node1";  // Persistent Raft state
 * 
 * RaftController controller(config);
 * ```
 * 
 * PERSISTENCE REQUIREMENTS:
 * - raft_log_directory MUST survive restarts (use disk, not tmpfs)
 * - data_directory should be on shared NFS for workers to access
 * 
 * PORT ALLOCATION:
 * Ensure no port conflicts if running multiple controllers on same host
 * (useful for local testing). Production deployment has one controller per host.
 */
struct RaftControllerConfig {
    /**
     * @brief Unique identifier for this controller in the Raft cluster
     * 
     * CONSTRAINTS:
     * - Must be > 0 (0 is reserved for "no leader")
     * - Must be unique across all controllers
     * - Typically sequential: 1, 2, 3 for 3-node cluster
     * 
     * EXAMPLE: For controllers on kh01, kh02, kh03, assign IDs 1, 2, 3
     */
    uint64_t node_id;
    
    /**
     * @brief TCP port for accepting worker connections and client job submissions
     * 
     * DEFAULT: 5000
     * 
     * PROTOCOL: Custom backtesting protocol (not Raft)
     * Workers connect here to register, receive jobs, send heartbeats
     */
    uint16_t listen_port;
    
    /**
     * @brief TCP port for Raft consensus protocol (inter-controller communication)
     * 
     * DEFAULT: 6000
     * 
     * PROTOCOL: Raft RPCs (RequestVote, AppendEntries)
     * Only other controllers connect here, never workers or clients
     * 
     * SECURITY NOTE: In production, use firewall rules to restrict this port
     * to only cluster nodes' IPs
     */
    uint16_t raft_port;
    
    /**
     * @brief Directory containing historical price data (CSV files)
     * 
     * DEFAULT: "./data"
     * 
     * STRUCTURE: Expects CSV files named like "AAPL.csv", "GOOGL.csv"
     * Each CSV has columns: date, open, high, low, close, volume
     * 
     * DEPLOYMENT: Should be NFS-mounted location accessible by all workers
     * so they can load data shards without network transfer
     */
    std::string data_directory;
    
    /**
     * @brief Directory for persistent Raft state (log, snapshots, metadata)
     * 
     * DEFAULT: "./raft_data"
     * 
     * CONTENTS:
     * - raft.log: Serialized log entries (jobs, worker assignments)
     * - state: Current term, voted_for (survives crashes)
     * - snapshot: Periodic compacted state (reduces log size)
     * 
     * CRITICAL: Must be on durable storage (not /tmp or RAM disk)
     * Otherwise, controller loses state on reboot and cluster may split-brain
     * 
     * BACKUP: Consider periodic backup of this directory for disaster recovery
     */
    std::string raft_log_directory;
    
    /**
     * @brief Timeout (seconds) for detecting worker failures via missed heartbeats
     * 
     * DEFAULT: 6 seconds (3 heartbeats at 2-second interval)
     * 
     * TUNING:
     * - Lower: Faster failure detection, but more false positives on network hiccups
     * - Higher: Tolerates temporary slowdowns, but delays job reassignment
     * 
     * RECOMMENDED: 2-3x the worker heartbeat interval
     * With 2-second heartbeats, 6 seconds allows 3 missed heartbeats
     */
    int heartbeat_timeout_sec;
    
    /**
     * @brief Default constructor initializes all fields to sensible defaults
     * 
     * Useful for quick testing without specifying every parameter:
     * ```cpp
     * RaftControllerConfig config;  // Uses all defaults
     * config.node_id = 2;  // Override only what's needed
     * ```
     */
    RaftControllerConfig()
        : node_id(1), listen_port(5000), raft_port(6000),
          data_directory("./data"), raft_log_directory("./raft_data"),
          heartbeat_timeout_sec(6) {}
};

//==============================================================================
// SECTION 3: ControllerPeer Structure
//==============================================================================

/**
 * @struct ControllerPeer
 * @brief Metadata describing a peer controller node in the cluster
 * 
 * PURPOSE:
 * When a controller starts, it needs to know how to connect to peer controllers
 * for Raft consensus. This structure captures each peer's network location.
 * 
 * USAGE:
 * ```cpp
 * controller.add_controller_peer(
 *     2,                // node_id
 *     "kh02.khoury.neu.edu",  // hostname
 *     5000,             // worker_port (for informational purposes)
 *     6000              // raft_port (where to send Raft RPCs)
 * );
 * ```
 * 
 * LIFETIME:
 * Stored in controller's peer list for entire runtime. Cannot remove peers
 * dynamically in this implementation (Raft cluster membership is static).
 */
struct ControllerPeer {
    /**
     * @brief Raft node ID of this peer (matches its node_id config)
     * 
     * Used as key in socket maps and Raft's peer tracking
     */
    uint64_t node_id;
    
    /**
     * @brief DNS hostname or IP address of peer controller
     * 
     * EXAMPLES: "192.168.1.10", "kh02", "controller-2.local"
     * 
     * RESOLUTION: Resolved via getaddrinfo() on first connection attempt
     * If DNS resolution fails, peer is unreachable
     */
    std::string hostname;
    
    /**
     * @brief TCP port where peer listens for worker connections
     * 
     * NOT USED by this controller (only peers' workers connect there)
     * Included for completeness and potential future features (e.g., forwarding
     * client requests to leader)
     */
    uint16_t worker_port;
    
    /**
     * @brief TCP port where peer listens for Raft RPCs
     * 
     * THIS IS THE IMPORTANT ONE for inter-controller communication
     * NetworkRaftNode connects to hostname:raft_port to send Raft messages
     */
    uint16_t raft_port;
};

//==============================================================================
// SECTION 4: RaftController Class
//==============================================================================

/**
 * @class RaftController
 * @brief Fault-tolerant controller integrating Raft consensus with job scheduling
 * 
 * OVERVIEW:
 * RaftController is the heart of the distributed backtesting system. It extends
 * the base Controller class with Raft consensus, enabling:
 * 
 * 1. LEADER ELECTION: Automatically elect one controller as leader to make
 *    scheduling decisions, avoiding split-brain scenarios
 * 
 * 2. STATE REPLICATION: All job submissions, worker registrations, and 
 *    assignments are logged in Raft and replicated to followers
 * 
 * 3. AUTOMATIC FAILOVER: If leader crashes, followers detect timeout and
 *    elect a new leader within ~5 seconds
 * 
 * 4. CONSISTENCY GUARANTEES: Clients never see inconsistent job status
 *    because all state transitions go through committed Raft log
 * 
 * ARCHITECTURE:
 * ```
 *     ┌─────────────┐         ┌─────────────┐         ┌─────────────┐
 *     │ Controller1 │◄───────►│ Controller2 │◄───────►│ Controller3 │
 *     │  (Leader)   │  Raft   │  (Follower) │  Raft   │  (Follower) │
 *     └──────┬──────┘         └─────────────┘         └─────────────┘
 *            │ Job submissions, worker coordination
 *            ▼
 *      ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
 *      │ Worker1 │  │ Worker2 │  │ Worker3 │  │ Worker4 │
 *      └─────────┘  └─────────┘  └─────────┘  └─────────┘
 * ```
 * 
 * ROLE BEHAVIORS:
 * 
 * LEADER:
 * - Accepts job submissions from clients
 * - Schedules jobs to workers (assigns which symbols to backtest)
 * - Monitors worker heartbeats and detects failures
 * - Reassigns jobs from dead workers to healthy ones
 * - Aggregates results from completed jobs
 * - Sends AppendEntries heartbeats to followers (keeps them synchronized)
 * 
 * FOLLOWER:
 * - Rejects job submissions (client must contact leader)
 * - Replicates Raft log entries from leader
 * - Stands ready to become leader if leader fails
 * - May monitor workers as backup (implementation choice)
 * 
 * CANDIDATE (transient during election):
 * - Enters this state when leader timeout expires
 * - Requests votes from peers
 * - Becomes leader if wins majority, returns to follower otherwise
 * 
 * THREADING MODEL:
 * This class manages several concurrent threads:
 * 
 * 1. Main thread: Calls start(), blocks accepting worker connections
 * 2. Raft background thread: Runs Raft state machine (elections, heartbeats)
 * 3. Raft accept thread: Accepts incoming Raft connections from peers
 * 4. Per-worker threads: Handle each worker's requests and heartbeats
 * 
 * SYNCHRONIZATION:
 * - raft_network_mutex_: Protects peer_sockets_ map (Raft connections)
 * - Base Controller has worker_mutex_: Protects workers_ map
 * - Raft has internal locks: Protects term, log, votedFor
 * 
 * TYPICAL LIFECYCLE:
 * ```cpp
 * // 1. Configure
 * RaftControllerConfig config;
 * config.node_id = 1;
 * config.listen_port = 5000;
 * config.raft_port = 6000;
 * 
 * // 2. Create controller
 * RaftController controller(config);
 * 
 * // 3. Add peers (for 3-node cluster)
 * controller.add_controller_peer(2, "kh02", 5000, 6000);
 * controller.add_controller_peer(3, "kh03", 5000, 6000);
 * 
 * // 4. Start (blocks accepting connections)
 * if (!controller.start()) {
 *     std::cerr << "Failed to start controller\n";
 *     return 1;
 * }
 * 
 * // 5. Submit jobs (if leader)
 * if (controller.is_raft_leader()) {
 *     JobParams params;
 *     params.strategy = "SMA";
 *     params.symbols = {"AAPL", "GOOGL"};
 *     uint64_t job_id = controller.submit_job(params);
 * }
 * 
 * // 6. Shutdown (Ctrl-C or signal)
 * controller.stop();  // Graceful cleanup
 * ```
 */
class RaftController : public Controller {
private:
    //==========================================================================
    // SECTION 4.2: Private Implementation Details
    //==========================================================================
    
    /**
     * @brief This controller's Raft configuration (ports, directories, etc.)
     * 
     * Copied from constructor parameter and used throughout lifetime
     */
    RaftControllerConfig raft_config_;
    
    /**
     * @brief The core Raft state machine managing consensus
     * 
     * OWNERSHIP: unique_ptr ensures automatic cleanup on controller destruction
     * 
     * LIFETIME: Created in constructor, destroyed in destructor
     * Do not access after stop() called
     * 
     * RESPONSIBILITIES:
     * - Maintains Raft state: term, log, commitIndex, etc.
     * - Runs background thread for elections and heartbeats
     * - Provides APIs to check leadership, append entries, read committed log
     */
    std::unique_ptr<NetworkRaftNode> raft_node_;
    
    /**
     * @brief Server socket for accepting Raft connections from peers
     * 
     * PROTOCOL: Custom length-prefixed Raft RPC protocol
     * 
     * BINDING: Listens on 0.0.0.0:raft_port (all interfaces)
     * In production, bind to specific interface or use firewall rules
     * 
     * LIFECYCLE:
     * - Created in setup_raft_socket()
     * - Closed in stop()
     * - Invalid (-1) if setup failed
     */
    int raft_server_socket_;
    
    /**
     * @brief Thread continuously accepting incoming Raft connections
     * 
     * RUNS: accept_raft_connections() in a loop
     * 
     * Each accepted connection is handled in a new thread (handle_raft_connection)
     * This architecture allows parallel processing of Raft RPCs from multiple peers
     * 
     * TERMINATION: Stopped by closing raft_server_socket_, causing accept() to fail
     */
    std::thread raft_accept_thread_;
    
    /**
     * @brief Map of peer node IDs to their persistent socket connections
     * 
     * Key: Raft node ID (e.g., 2, 3 for peers in 3-node cluster where this is 1)
     * Value: TCP socket FD, or -1 if not connected
     * 
     * USAGE: NetworkRaftNode sends Raft RPCs over these sockets
     * 
     * CONNECTION MANAGEMENT:
     * - Lazy initialization: Connect on first send attempt
     * - Reconnect on failure: If send fails, attempt reconnect once
     * - Persistent: Keep connections alive for duration of controller runtime
     * 
     * LOCKING: Always access with raft_network_mutex_ held
     */
    std::map<uint64_t, int> peer_sockets_;
    
    /**
     * @brief Mutex protecting peer_sockets_ from concurrent modification
     * 
     * CRITICAL SECTIONS:
     * - Sending Raft RPCs (read socket, possibly reconnect)
     * - Accepting new connections (write socket on first connect)
     * - Cleanup on shutdown (close all sockets)
     * 
     * DEADLOCK PREVENTION: Never hold this lock while blocking on I/O
     * (exception: the I/O operation itself, like send(), is OK)
     */
    std::mutex raft_network_mutex_;
    
    /**
     * @brief List of peer controllers in the cluster
     * 
     * Populated via add_controller_peer() before start()
     * Used to initialize Raft peer list and establish connections
     * 
     * IMMUTABLE after start() called (Raft cluster membership is static)
     */
    std::vector<ControllerPeer> controller_peers_;
    
public:
    //==========================================================================
    // SECTION 4.1: Public Interface
    //==========================================================================
    
    /**
     * @brief Constructs a Raft-enabled controller with given configuration
     * 
     * INITIALIZATION STEPS:
     * 1. Initialize base Controller with worker management
     * 2. Create NetworkRaftNode with Raft algorithm and networking
     * 3. Set up Raft log persistence in raft_log_directory
     * 4. Initialize server sockets (set to -1, bound in start())
     * 
     * @param config Controller and Raft configuration parameters
     * 
     * @throws std::runtime_error if raft_log_directory creation fails
     * @throws std::invalid_argument if node_id is 0 or ports are invalid
     * 
     * POSTCONDITIONS:
     * - Controller ready to add peers via add_controller_peer()
     * - Must call start() before accepting connections
     * - Raft node exists but not yet running (no threads started)
     * 
     * EXAMPLE:
     * ```cpp
     * RaftControllerConfig config;
     * config.node_id = 1;
     * config.listen_port = 5000;
     * config.raft_port = 6000;
     * 
     * try {
     *     RaftController controller(config);
     *     // Configure peers...
     *     controller.start();
     * } catch (const std::exception& e) {
     *     std::cerr << "Controller init failed: " << e.what() << '\n';
     * }
     * ```
     */
    explicit RaftController(const RaftControllerConfig& config);
    
    /**
     * @brief Destroys controller, cleaning up all resources
     * 
     * CLEANUP SEQUENCE:
     * 1. Call stop() if not already stopped (graceful shutdown)
     * 2. Close all peer sockets
     * 3. Destroy Raft node (stops background threads, flushes log)
     * 4. Close server sockets
     * 
     * BLOCKING: May block briefly waiting for threads to terminate
     * Typically completes in < 1 second
     * 
     * EXCEPTION SAFETY: Destructor does not throw (all errors logged)
     */
    ~RaftController();
    
    /**
     * @brief Starts the controller and begins accepting connections
     * 
     * STARTUP SEQUENCE:
     * 1. Validate configuration (check ports, directories)
     * 2. Set up worker listener socket (port 5000)
     * 3. Set up Raft listener socket (port 6000)
     * 4. Connect to peer controllers (establish Raft connections)
     * 5. Start Raft node (begin leader election process)
     * 6. Launch accept threads for workers and Raft peers
     * 7. Enter main loop accepting worker connections (blocks here)
     * 
     * @return true if startup successful, false on error
     * 
     * BLOCKING BEHAVIOR:
     * This method blocks in the main accept loop, handling worker connections
     * until stop() is called (usually from signal handler on Ctrl-C)
     * 
     * FAILURE MODES:
     * - Port already in use: Returns false immediately
     * - Cannot resolve peer hostnames: Logs warning, continues
     *   (Raft will retry connection later)
     * - Raft log corruption: Returns false after attempting recovery
     * 
     * THREAD SAFETY: Not thread-safe, call only once from main thread
     * 
     * EXAMPLE:
     * ```cpp
     * RaftController controller(config);
     * controller.add_controller_peer(2, "kh02", 5000, 6000);
     * controller.add_controller_peer(3, "kh03", 5000, 6000);
     * 
     * if (!controller.start()) {
     *     std::cerr << "Failed to start controller\n";
     *     return 1;
     * }
     * // Blocks here until stop() called or Ctrl-C
     * ```
     */
    bool start() override;
    
    /**
     * @brief Gracefully stops the controller and shuts down all connections
     * 
     * SHUTDOWN SEQUENCE:
     * 1. Set shutdown flag (stops accept loops)
     * 2. Close server sockets (unblocks accept() calls)
     * 3. Stop Raft node (no more leader elections)
     * 4. Disconnect from all peers
     * 5. Terminate all worker connections
     * 6. Flush Raft log to disk (ensure durability)
     * 7. Join all threads (wait for clean termination)
     * 
     * BLOCKING: May take up to ~2 seconds to complete
     * - 1 second for Raft background thread to detect shutdown
     * - <1 second for threads to finish current operations
     * 
     * IDEMPOTENT: Safe to call multiple times (subsequent calls are no-ops)
     * 
     * SIGNAL SAFETY: Can be called from signal handler if needed
     * (though prefer dedicated signal handling thread)
     * 
     * EXAMPLE:
     * ```cpp
     * // Signal handler
     * RaftController* g_controller = nullptr;
     * 
     * void signal_handler(int signum) {
     *     std::cout << "Shutting down...\n";
     *     if (g_controller) {
     *         g_controller->stop();
     *     }
     * }
     * 
     * int main() {
     *     RaftController controller(config);
     *     g_controller = &controller;
     *     signal(SIGINT, signal_handler);
     *     
     *     controller.start();  // Blocks until stop() called
     *     return 0;
     * }
     * ```
     */
    void stop() override;
    
    /**
     * @brief Registers a peer controller in the Raft cluster
     * 
     * MUST BE CALLED BEFORE start() - peers cannot be added dynamically
     * after cluster is running (limitation of this Raft implementation)
     * 
     * @param node_id Unique Raft ID for this peer (must not equal this node's ID)
     * @param hostname DNS name or IP address (e.g., "kh02.khoury.neu.edu")
     * @param worker_port Peer's worker listening port (typically 5000)
     * @param raft_port Peer's Raft listening port (typically 6000)
     * 
     * VALIDATION:
     * - node_id must be unique across all add_controller_peer calls
     * - node_id must not equal this controller's node_id
     * - hostname must be resolvable (checked lazily on connection attempt)
     * 
     * CLUSTER SIZE REQUIREMENTS:
     * - Minimum 3 nodes for fault tolerance (can tolerate 1 failure)
     * - Odd numbers preferred (3, 5, 7) to avoid split votes
     * - 5 nodes can tolerate 2 failures but adds latency
     * 
     * EXAMPLE (3-node cluster setup):
     * ```cpp
     * // On kh01 (node_id=1):
     * RaftControllerConfig config1;
     * config1.node_id = 1;
     * RaftController ctrl1(config1);
     * ctrl1.add_controller_peer(2, "kh02", 5000, 6000);
     * ctrl1.add_controller_peer(3, "kh03", 5000, 6000);
     * 
     * // On kh02 (node_id=2):
     * RaftControllerConfig config2;
     * config2.node_id = 2;
     * RaftController ctrl2(config2);
     * ctrl2.add_controller_peer(1, "kh01", 5000, 6000);
     * ctrl2.add_controller_peer(3, "kh03", 5000, 6000);
     * 
     * // On kh03 (node_id=3):
     * RaftControllerConfig config3;
     * config3.node_id = 3;
     * RaftController ctrl3(config3);
     * ctrl3.add_controller_peer(1, "kh01", 5000, 6000);
     * ctrl3.add_controller_peer(2, "kh02", 5000, 6000);
     * ```
     */
    void add_controller_peer(uint64_t node_id, const std::string& hostname,
                            uint16_t worker_port, uint16_t raft_port);
    
    /**
     * @brief Checks if this controller is the current Raft leader
     * 
     * LEADERSHIP DETERMINATION:
     * - Leader: Can schedule jobs, coordinate workers, accept client requests
     * - Follower: Must redirect clients to leader, only replicates state
     * - Candidate: Temporarily during election, cannot serve requests
     * 
     * @return true if this node is the leader, false otherwise
     * 
     * CONSISTENCY NOTE:
     * Leadership can change at any time due to network partitions or failures.
     * After checking true, leadership may be lost before next operation.
     * Therefore, job submission should re-check leadership atomically.
     * 
     * TYPICAL USAGE:
     * ```cpp
     * if (controller.is_raft_leader()) {
     *     uint64_t job_id = controller.submit_job(params);
     *     std::cout << "Job submitted: " << job_id << '\n';
     * } else {
     *     uint64_t leader_id = controller.get_raft_leader_id();
     *     std::cout << "Not leader, redirect to node " << leader_id << '\n';
     * }
     * ```
     * 
     * PERFORMANCE: O(1) operation (reads cached state)
     */
    bool is_raft_leader() const { return raft_node_ && raft_node_->is_leader(); }
    
    /**
     * @brief Gets the node ID of the current Raft leader
     * 
     * @return Leader's node ID (1, 2, 3, etc.), or 0 if no leader elected yet
     * 
     * DURING ELECTION:
     * Returns 0 when no leader exists (e.g., during initial startup or after
     * leader failure while election is in progress)
     * 
     * CLIENT REDIRECTION:
     * Clients can use this to find the leader's address:
     * ```cpp
     * uint64_t leader_id = controller.get_raft_leader_id();
     * if (leader_id == 0) {
     *     std::cerr << "No leader elected yet, retry later\n";
     * } else if (leader_id != my_node_id) {
     *     std::string leader_addr = get_controller_address(leader_id);
     *     std::cout << "Redirect to leader at " << leader_addr << '\n';
     * }
     * ```
     * 
     * STALENESS:
     * Value may be stale if leader just failed and new election starting
     * Client should handle connection failures gracefully
     */
    uint64_t get_raft_leader_id() const { return raft_node_ ? raft_node_->get_leader_id() : 0; }
    
    /**
     * @brief Submits a backtest job to the distributed system
     * 
     * LEADERSHIP REQUIREMENT:
     * This operation ONLY succeeds on the Raft leader. Followers will reject
     * the submission and return 0 (caller should redirect to leader).
     * 
     * RAFT LOG INTEGRATION:
     * Job is appended to Raft log and replicated to followers before being
     * considered "submitted". This ensures:
     * - Job survives leader crashes (replicated on majority)
     * - New leader sees job if current leader fails
     * - Followers can take over without losing jobs
     * 
     * SCHEDULING FLOW:
     * 1. Client calls submit_job() on leader
     * 2. Leader appends job to Raft log (waits for majority replication)
     * 3. Once committed, leader assigns job to available workers
     * 4. Workers receive job, begin processing
     * 5. Workers send results back as they complete symbols
     * 6. Leader aggregates results, stores in Raft log (for durability)
     * 
     * @param params Job parameters (strategy, symbols, date range, etc.)
     * 
     * @return Job ID (> 0) on success, 0 on failure
     * 
     * FAILURE CASES:
     * - Not leader: Returns 0 immediately
     * - No workers available: May block or return 0 depending on policy
     * - Raft log full: Blocks until log compaction completes
     * 
     * BLOCKING BEHAVIOR:
     * May block for up to ~500ms waiting for Raft replication to majority.
     * This ensures durability but adds latency. For high-throughput scenarios,
     * consider batching multiple jobs into one Raft entry.
     * 
     * EXAMPLE:
     * ```cpp
     * JobParams params;
     * params.strategy = "SMA";
     * params.symbols = {"AAPL", "MSFT", "GOOGL"};
     * params.start_date = "2020-01-01";
     * params.end_date = "2024-12-31";
     * params.fast_period = 50;
     * params.slow_period = 200;
     * 
     * uint64_t job_id = controller.submit_job(params);
     * if (job_id == 0) {
     *     if (!controller.is_raft_leader()) {
     *         std::cerr << "Not leader, retry on node " 
     *                   << controller.get_raft_leader_id() << '\n';
     *     } else {
     *         std::cerr << "Job submission failed (no workers?)\n";
     *     }
     * } else {
     *     std::cout << "Job " << job_id << " submitted successfully\n";
     * }
     * ```
     */
    uint64_t submit_job(const JobParams& params) override;
    
private:
    /**
     * @brief Creates and binds the Raft server socket
     * 
     * SOCKET SETUP:
     * 1. Create TCP socket (AF_INET, SOCK_STREAM)
     * 2. Set SO_REUSEADDR (allow quick restart after crash)
     * 3. Bind to 0.0.0.0:raft_port (all interfaces)
     * 4. Listen with backlog of 5 pending connections
     * 
     * @return true on success, false if bind/listen fails
     * 
     * FAILURE CAUSES:
     * - Port already in use (another controller running?)
     * - Permission denied (port < 1024 requires root)
     * - No network interfaces available
     * 
     * CALLED FROM: start() during initialization
     */
    bool setup_raft_socket();
    
    /**
     * @brief Main loop accepting incoming Raft connections from peers
     * 
     * RUNS IN: raft_accept_thread_
     * 
     * LOOP STRUCTURE:
     * ```
     * while (!shutdown_) {
     *     int client_socket = accept(raft_server_socket_, ...);
     *     if (client_socket < 0) break;  // Shutdown or error
     *     std::thread(handle_raft_connection, client_socket).detach();
     * }
     * ```
     * 
     * THREAD SPAWNING:
     * Each accepted connection gets its own thread to handle RPCs.
     * This allows parallel processing of RequestVote and AppendEntries
     * from multiple peers simultaneously.
     * 
     * TERMINATION:
     * Thread exits when raft_server_socket_ is closed (in stop())
     */
    void accept_raft_connections();
    
    /**
     * @brief Handles Raft RPC requests on a peer connection
     * 
     * PROTOCOL:
     * 1. Read 4-byte message length
     * 2. Read message payload (serialized Raft RPC)
     * 3. Deserialize to RequestVote or AppendEntries
     * 4. Invoke Raft state machine to process RPC
     * 5. Serialize response and send back
     * 6. Loop until connection closed
     * 
     * @param client_socket Socket FD for connected peer
     * 
     * MESSAGE TYPES:
     * - REQUEST_VOTE (0x01): Candidate requesting vote in election
     * - APPEND_ENTRIES (0x02): Leader replicating log or sending heartbeat
     * 
     * ERROR HANDLING:
     * - Deserialize failure: Log error, close connection
     * - Socket read error: Assume peer crashed, exit thread
     * - Invalid message type: Send error response, continue
     * 
     * THREAD LIFETIME:
     * Thread runs until peer closes connection or this controller shuts down
     */
    void handle_raft_connection(int client_socket);
    
    /**
     * @brief Establishes TCP connection to a peer controller
     * 
     * CONNECTION STEPS:
     * 1. Look up peer's hostname and raft_port from controller_peers_
     * 2. Resolve hostname to IP address (getaddrinfo)
     * 3. Create socket and connect() to peer's Raft port
     * 4. Store socket FD in peer_sockets_ map
     * 
     * @param peer_id Raft node ID of target peer
     * 
     * @return true if connection established, false on error
     * 
     * RETRY POLICY:
     * On failure, does NOT retry automatically. Caller (typically Raft message
     * send methods) will retry on next RPC attempt after a delay.
     * 
     * CONNECTION REUSE:
     * Once established, connection persists for controller's lifetime.
     * If connection breaks, peer_sockets_[peer_id] set to -1 and reconnection
     * attempted on next send.
     * 
     * BLOCKING:
     * connect() may block for up to TCP timeout (~75 seconds) if peer is down.
     * Consider using non-blocking connect with timeout in production.
     */
    bool connect_to_peer(uint64_t peer_id);
    
    /**
     * @brief Sends a serialized Raft message over a socket
     * 
     * WIRE PROTOCOL:
     * ```
     * [4 bytes: message length (big-endian)]
     * [N bytes: message data]
     * ```
     * 
     * @param socket_fd Target socket file descriptor
     * @param data Serialized message bytes
     * 
     * @return true if entire message sent, false on error
     * 
     * ATOMICITY:
     * Uses MSG_NOSIGNAL to avoid SIGPIPE if peer closes connection.
     * Handles partial sends by looping until all bytes transmitted.
     * 
     * ERROR HANDLING:
     * - EPIPE / ECONNRESET: Peer disconnected, returns false
     * - EAGAIN / EWOULDBLOCK: Should not occur (blocking socket)
     * - Other errors: Logged and returns false
     */
    bool send_raft_message(int socket_fd, const std::vector<uint8_t>& data);
    
    /**
     * @brief Receives a complete Raft message from a socket
     * 
     * PROTOCOL:
     * 1. Read 4-byte length prefix
     * 2. Allocate buffer of that size
     * 3. Read exactly that many bytes of payload
     * 
     * @param socket_fd Source socket file descriptor
     * 
     * @return Message bytes on success, empty vector on error/EOF
     * 
     * BLOCKING:
     * Blocks until entire message received or error occurs.
     * Timeout handled at TCP level (SO_RCVTIMEO socket option if set).
     * 
     * MEMORY:
     * Allocates buffer based on length prefix. Validate length before
     * allocating to prevent memory exhaustion attacks:
     * ```cpp
     * if (length > MAX_MESSAGE_SIZE) {
     *     // Reject malicious or corrupted message
     *     return {};
     * }
     * ```
     */
    std::vector<uint8_t> receive_raft_message(int socket_fd);
};

} // namespace backtesting

//==============================================================================
// SECTION 5: Usage Examples
//==============================================================================

/**
 * EXAMPLE 1: Setting up a 3-node Raft controller cluster
 * 
 * Deployment on Khoury cluster nodes kh01, kh02, kh03:
 * 
 * ```cpp
 * // On kh01:
 * #include "raft_controller.h"
 * 
 * int main() {
 *     using namespace backtesting;
 *     
 *     RaftControllerConfig config;
 *     config.node_id = 1;
 *     config.listen_port = 5000;  // Workers connect here
 *     config.raft_port = 6000;    // Peers connect here
 *     config.data_directory = "/mnt/shared/sp500_data";
 *     config.raft_log_directory = "/var/raft/node1";
 *     
 *     RaftController controller(config);
 *     
 *     // Register the other 2 controllers as peers
 *     controller.add_controller_peer(2, "kh02.khoury.neu.edu", 5000, 6000);
 *     controller.add_controller_peer(3, "kh03.khoury.neu.edu", 5000, 6000);
 *     
 *     // Start (blocks)
 *     if (!controller.start()) {
 *         std::cerr << "Failed to start controller\n";
 *         return 1;
 *     }
 *     
 *     return 0;
 * }
 * ```
 * 
 * REPEAT for kh02 (node_id=2) and kh03 (node_id=3), adjusting peer lists.
 * Each node lists the OTHER nodes as peers.
 */

/**
 * EXAMPLE 2: Submitting a backtest job with leader detection
 * 
 * ```cpp
 * #include "raft_controller.h"
 * #include <iostream>
 * #include <thread>
 * #include <chrono>
 * 
 * void submit_job_with_retry(RaftController& controller) {
 *     JobParams params;
 *     params.strategy = "SMA";
 *     params.symbols = {"AAPL", "MSFT", "GOOGL", "AMZN"};
 *     params.start_date = "2020-01-01";
 *     params.end_date = "2024-12-31";
 *     params.fast_period = 50;
 *     params.slow_period = 200;
 *     
 *     // Retry until successful (leader might be electing)
 *     for (int attempt = 0; attempt < 10; ++attempt) {
 *         if (!controller.is_raft_leader()) {
 *             uint64_t leader_id = controller.get_raft_leader_id();
 *             if (leader_id == 0) {
 *                 std::cout << "No leader yet, waiting...\n";
 *             } else {
 *                 std::cout << "Not leader, should redirect to node " 
 *                           << leader_id << '\n';
 *                 return;  // In real impl, send HTTP redirect or RPC to leader
 *             }
 *             std::this_thread::sleep_for(std::chrono::seconds(1));
 *             continue;
 *         }
 *         
 *         uint64_t job_id = controller.submit_job(params);
 *         if (job_id > 0) {
 *             std::cout << "Job " << job_id << " submitted successfully\n";
 *             return;
 *         }
 *         
 *         std::cerr << "Job submission failed, retrying...\n";
 *         std::this_thread::sleep_for(std::chrono::seconds(1));
 *     }
 *     
 *     std::cerr << "Failed to submit job after 10 attempts\n";
 * }
 * ```
 */

/**
 * EXAMPLE 3: Graceful shutdown with signal handling
 * 
 * ```cpp
 * #include <csignal>
 * #include <atomic>
 * 
 * std::atomic<bool> g_shutdown{false};
 * RaftController* g_controller = nullptr;
 * 
 * void signal_handler(int signum) {
 *     std::cout << "\nReceived signal " << signum << ", shutting down...\n";
 *     g_shutdown = true;
 *     if (g_controller) {
 *         g_controller->stop();
 *     }
 * }
 * 
 * int main() {
 *     // Set up signal handlers
 *     signal(SIGINT, signal_handler);   // Ctrl-C
 *     signal(SIGTERM, signal_handler);  // kill command
 *     
 *     RaftControllerConfig config;
 *     config.node_id = 1;
 *     RaftController controller(config);
 *     g_controller = &controller;
 *     
 *     controller.add_controller_peer(2, "kh02", 5000, 6000);
 *     controller.add_controller_peer(3, "kh03", 5000, 6000);
 *     
 *     std::cout << "Starting controller...\n";
 *     controller.start();  // Blocks until signal received
 *     
 *     std::cout << "Controller stopped gracefully\n";
 *     return 0;
 * }
 * ```
 */

//==============================================================================
// SECTION 6: Common Pitfalls and How to Avoid Them
//==============================================================================

/**
 * PITFALL 1: Starting controller before adding peers
 * 
 * PROBLEM:
 * ```cpp
 * RaftController controller(config);
 * controller.start();  // No peers added yet!
 * controller.add_controller_peer(2, "kh02", 5000, 6000);  // Too late
 * ```
 * 
 * CONSEQUENCE:
 * Controller runs as single-node cluster, never elects leader (needs majority).
 * 
 * SOLUTION:
 * Always add ALL peers before calling start():
 * ```cpp
 * RaftController controller(config);
 * controller.add_controller_peer(2, "kh02", 5000, 6000);
 * controller.add_controller_peer(3, "kh03", 5000, 6000);
 * controller.start();  // Correct order
 * ```
 */

/**
 * PITFALL 2: Using tmpfs or /tmp for raft_log_directory
 * 
 * PROBLEM:
 * ```cpp
 * config.raft_log_directory = "/tmp/raft_data";  // Volatile storage!
 * ```
 * 
 * CONSEQUENCE:
 * On reboot, controller loses all Raft state (term, log, votes).
 * Cluster may elect multiple leaders (split-brain) if nodes restart at different times.
 * 
 * SOLUTION:
 * Use persistent storage that survives reboots:
 * ```cpp
 * config.raft_log_directory = "/var/lib/raft/node1";  // Persistent
 * // Or on shared NFS (if NFS is reliable):
 * config.raft_log_directory = "/mnt/nfs/raft/node1";
 * ```
 */

/**
 * PITFALL 3: Forgetting to check leadership before submitting jobs
 * 
 * PROBLEM:
 * ```cpp
 * uint64_t job_id = controller.submit_job(params);  // Might be follower!
 * if (job_id == 0) {
 *     std::cerr << "Job failed\n";  // Misleading error message
 * }
 * ```
 * 
 * CONSEQUENCE:
 * Job silently rejected if this node is follower, no indication of why.
 * 
 * SOLUTION:
 * Check leadership first and provide helpful error:
 * ```cpp
 * if (!controller.is_raft_leader()) {
 *     uint64_t leader_id = controller.get_raft_leader_id();
 *     std::cerr << "Cannot submit job: not leader. Redirect to node " 
 *               << leader_id << '\n';
 *     return;
 * }
 * uint64_t job_id = controller.submit_job(params);  // Safe
 * ```
 */

/**
 * PITFALL 4: Identical node_id on multiple controllers
 * 
 * PROBLEM:
 * ```cpp
 * // On both kh01 and kh02:
 * config.node_id = 1;  // Not unique!
 * ```
 * 
 * CONSEQUENCE:
 * Raft protocol breaks - votes and log entries get confused between nodes.
 * Cluster may never elect leader or experience data corruption.
 * 
 * SOLUTION:
 * Assign unique IDs to each controller:
 * ```cpp
 * // On kh01:
 * config.node_id = 1;  // Unique
 * 
 * // On kh02:
 * config.node_id = 2;  // Different
 * 
 * // On kh03:
 * config.node_id = 3;  // Different
 * ```
 */

/**
 * PITFALL 5: Blocking in Raft callback with locks held
 * 
 * PROBLEM:
 * ```cpp
 * bool NetworkRaftNode::send_append_entries(...) {
 *     std::lock_guard<std::mutex> lock(*network_mutex_);
 *     // Holding lock while doing blocking I/O!
 *     send_raft_message(socket, data);  // May block for seconds
 *     // Other threads trying to send are blocked
 * }
 * ```
 * 
 * CONSEQUENCE:
 * Leader cannot send heartbeats to multiple followers in parallel, causing
 * unnecessary latency and potential follower timeouts.
 * 
 * SOLUTION:
 * Hold lock only to copy socket FD, release before I/O:
 * ```cpp
 * bool NetworkRaftNode::send_append_entries(...) {
 *     int socket_fd;
 *     {
 *         std::lock_guard<std::mutex> lock(*network_mutex_);
 *         socket_fd = (*peer_sockets_)[peer_id];  // Quick lookup
 *     }  // Lock released here
 *     
 *     // I/O without lock held
 *     return send_raft_message(socket_fd, data);
 * }
 * ```
 */

/**
 * PITFALL 6: Even-numbered cluster (2, 4, 6 nodes)
 * 
 * PROBLEM:
 * ```cpp
 * // 4-node cluster
 * controller1.add_controller_peer(2, ...);
 * controller1.add_controller_peer(3, ...);
 * controller1.add_controller_peer(4, ...);  // Even number!
 * ```
 * 
 * CONSEQUENCE:
 * On network partition splitting cluster in half (2+2), neither side has
 * majority (need 3/4). Cluster becomes unavailable despite only 2 nodes down.
 * 
 * SOLUTION:
 * Use odd-numbered clusters (3, 5, 7):
 * ```cpp
 * // 5-node cluster ✅
 * // Can tolerate 2 failures, partition tolerance better
 * controller1.add_controller_peer(2, ...);
 * controller1.add_controller_peer(3, ...);
 * controller1.add_controller_peer(4, ...);
 * controller1.add_controller_peer(5, ...);
 * ```
 * 
 * RULE OF THUMB:
 * - 3 nodes: Tolerate 1 failure (good for most applications)
 * - 5 nodes: Tolerate 2 failures (high availability)
 * - 7 nodes: Tolerate 3 failures (rarely needed, adds latency)
 */

//==============================================================================
// SECTION 7: Frequently Asked Questions (FAQ)
//==============================================================================

/**
 * Q1: How long does leader election take?
 * 
 * A: Typically 2-5 seconds after leader failure:
 *    - Followers detect leader timeout: ~1-2 seconds (randomized)
 *    - Candidate requests votes: ~500ms (one RTT to all peers)
 *    - New leader sends first heartbeat: ~100ms
 *    
 *    Total downtime: ~3 seconds average, 10 seconds worst case.
 *    
 *    TUNING: Reduce election timeout in RaftConfig to speed up failover,
 *    but beware of false positives on slow networks.
 */

/**
 * Q2: Can I dynamically add/remove controllers after cluster is running?
 * 
 * A: Not in this implementation. Raft cluster membership is static.
 *    
 *    WORKAROUND:
 *    1. Stop all controllers gracefully
 *    2. Update peer lists in config files
 *    3. Restart all controllers
 *    
 *    FUTURE ENHANCEMENT:
 *    Implement Raft's joint-consensus configuration change protocol
 *    to add/remove nodes without downtime.
 */

/**
 * Q3: What happens if all controllers crash simultaneously?
 * 
 * A: Cluster is unavailable until at least one controller restarts:
 *    1. First controller to restart enters follower state
 *    2. Waits for election timeout (no leader heartbeats)
 *    3. Becomes candidate, requests votes from (down) peers
 *    4. Cannot win election (needs majority)
 *    5. Once 2nd controller restarts, they elect a leader
 *    
 *    DATA SAFETY: No data lost if raft_log_directory intact.
 *    Restarted controllers replay Raft log and resume jobs.
 *    
 *    RECOVERY TIME: ~1 minute (boot time + election time)
 */

/**
 * Q4: How do I debug "stuck" leader election (no leader elected)?
 * 
 * A: Check these common causes:
 *    
 *    1. CLOCK SKEW: Ensure NTP running on all nodes
 *       ```bash
 *       ntpq -p  # Check clock sync
 *       ```
 *    
 *    2. FIREWALL: Verify raft_port (6000) open between all controllers
 *       ```bash
 *       telnet kh02 6000  # Should connect, not "connection refused"
 *       ```
 *    
 *    3. HOSTNAME RESOLUTION: Check DNS or /etc/hosts
 *       ```bash
 *       ping kh02.khoury.neu.edu  # Should resolve
 *       ```
 *    
 *    4. SPLIT BRAIN: Ensure cluster has odd number of nodes
 *       Even-numbered clusters can deadlock on 50/50 partition
 *    
 *    5. LOG CORRUPTION: Check raft_log_directory for errors
 *       ```bash
 *       ls -lh /var/raft/node1/raft.log
 *       # If corrupted, delete and controller will recreate
 *       ```
 */

/**
 * Q5: What's the maximum message rate Raft can handle?
 * 
 * A: Depends on message size and replication latency:
 *    
 *    RULE OF THUMB:
 *    - Small messages (< 1KB): ~1000 messages/sec per leader
 *    - Medium messages (10KB): ~100 messages/sec
 *    - Large messages (1MB): ~10 messages/sec
 *    
 *    BOTTLENECK: Leader must replicate to majority before returning.
 *    On 100ms RTT network, max throughput is ~10 operations/sec.
 *    
 *    OPTIMIZATION: Batch multiple jobs into one Raft log entry:
 *    ```cpp
 *    std::vector<JobParams> batch = {job1, job2, job3};
 *    uint64_t batch_id = controller.submit_job_batch(batch);  // 3x faster
 *    ```
 */

/**
 * Q6: How much disk space does raft_log_directory need?
 * 
 * A: Grows with cluster activity, compacted periodically:
 *    
 *    LOG SIZE: ~1KB per job submission
 *    - 10,000 jobs = ~10MB log file
 *    - Plus snapshots (compressed cluster state)
 *    
 *    COMPACTION: Automatic when log exceeds threshold (e.g., 10,000 entries)
 *    Old entries replaced with snapshot, log restarted from scratch.
 *    
 *    RECOMMENDATION: Allocate 1GB minimum, 10GB for production.
 *    Monitor with:
 *    ```bash
 *    du -sh /var/raft/node1
 *    ```
 */

/**
 * Q7: Can I run multiple controllers on the same machine (for testing)?
 * 
 * A: Yes, but change ports to avoid conflicts:
 *    
 *    ```cpp
 *    // Controller 1 on localhost:
 *    RaftControllerConfig config1;
 *    config1.node_id = 1;
 *    config1.listen_port = 5001;  // Different ports
 *    config1.raft_port = 6001;
 *    config1.raft_log_directory = "/tmp/raft1";  // Different dirs
 *    
 *    // Controller 2 on localhost:
 *    RaftControllerConfig config2;
 *    config2.node_id = 2;
 *    config2.listen_port = 5002;  // Different ports
 *    config2.raft_port = 6002;
 *    config2.raft_log_directory = "/tmp/raft2";  // Different dirs
 *    
 *    controller1.add_controller_peer(2, "localhost", 5002, 6002);
 *    controller2.add_controller_peer(1, "localhost", 5001, 6001);
 *    ```
 *    
 *    LIMITATION: Can't test real network failures this way.
 */

/**
 * Q8: What metrics should I monitor in production?
 * 
 * A: Key health indicators:
 *    
 *    1. LEADER STABILITY: How often does leader change?
 *       - Normal: Once per month (planned maintenance)
 *       - Bad: Multiple times per hour (network issues)
 *    
 *    2. LOG REPLICATION LAG: How far behind are followers?
 *       - Normal: < 100 entries behind leader
 *       - Bad: > 1000 entries (slow follower, will timeout)
 *    
 *    3. COMMIT LATENCY: Time from log append to commit
 *       - Normal: 2-5ms (local network)
 *       - Bad: > 100ms (slow follower or high load)
 *    
 *    4. ELECTION FREQUENCY: Elections per day
 *       - Normal: 0 (no failures)
 *       - Bad: > 5 (investigate root cause)
 *    
 *    IMPLEMENTATION:
 *    Add Prometheus metrics or expose HTTP endpoint:
 *    ```cpp
 *    std::string RaftController::get_metrics() {
 *        return "raft_leader=" + std::to_string(is_raft_leader()) +
 *               ",raft_term=" + std::to_string(raft_node_->get_term()) +
 *               ",raft_log_size=" + std::to_string(raft_node_->log_size());
 *    }
 *    ```
 */

#endif // RAFT_CONTROLLER_H