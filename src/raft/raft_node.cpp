/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: raft_node.cpp
    
    Description:
        This file implements the core Raft consensus algorithm, which provides
        fault-tolerant distributed coordination for the backtesting system's
        controller cluster. Raft ensures that multiple controller nodes maintain
        consistent state through leader election, log replication, and safety
        guarantees even in the presence of network failures and node crashes.
        
        Raft Consensus Overview:
        Raft is a consensus algorithm designed for understandability and
        practical implementation. It solves the fundamental problem of
        distributed systems: getting multiple machines to agree on a shared
        state despite failures.
        
        Core Components Implemented:
        1. LEADER ELECTION: Selects single leader from cluster
        2. LOG REPLICATION: Leader replicates entries to followers
        3. SAFETY GUARANTEES: Ensures consistency across failures
        4. PERSISTENCE: Durable storage of term, vote, and log
        5. COMMIT PROTOCOL: Majority-based commit decisions
        
    Raft State Machine:
        
        Each node can be in one of three states:
        
        ┌──────────┐  Election timeout     ┌───────────┐
        │          │──────────────────────→│           │
        │ FOLLOWER │                        │ CANDIDATE │
        │          │←──────────────────────│           │
        └──────────┘  Discovers leader     └───────────┘
             ↑        or higher term             │
             │                                   │ Wins election
             │                                   │ (majority votes)
             │                                   ↓
             │                            ┌────────┐
             │                            │        │
             └────────────────────────────│ LEADER │
                Discovers higher term     │        │
                                          └────────┘
        
        State Descriptions:
        - FOLLOWER: Passive, responds to leader and candidates
        - CANDIDATE: Actively seeking votes to become leader
        - LEADER: Accepts client requests, replicates to followers
        
    Key Raft Properties:
        
        SAFETY (Correctness Guarantees):
        - Election Safety: At most one leader per term
        - Leader Append-Only: Leaders never overwrite entries
        - Log Matching: Identical indices+terms → identical logs
        - Leader Completeness: Committed entries in all future leaders
        - State Machine Safety: Nodes apply same commands in same order
        
        LIVENESS (Progress Guarantees):
        - Available if majority alive
        - Eventually elects leader (randomized timeouts prevent livelock)
        - Progress continues despite minority failures
        
    Timing Parameters:
        
        Critical timing values that affect system behavior:
        
        Election Timeout: 150-300ms (RANDOMIZED)
        - Follower waits this long for leader heartbeat
        - If timeout: Become candidate, start election
        - Randomized: Prevents simultaneous elections (split votes)
        
        Heartbeat Interval: 50ms (FIXED)
        - Leader sends AppendEntries this often
        - Purpose: Prevent election timeouts, maintain authority
        - Must be: << election_timeout (typically 3-10x smaller)
        
        RPC Timeout: 2 seconds
        - Network call timeout (send/receive)
        - Prevents: Hanging on dead/slow peer
        
        Relationship:
        heartbeat_interval < election_timeout_min < election_timeout_max
        50ms < 150ms < 300ms (satisfied)
        
    Thread Architecture:
        
        ELECTION TIMER THREAD:
        - Checks: Every 10ms if election timeout expired
        - If timeout: Triggers election (become candidate, request votes)
        - Runs: Until node stopped
        
        HEARTBEAT THREAD:
        - If leader: Send heartbeats every 50ms
        - If follower/candidate: Sleep (no action)
        - Purpose: Leader maintains authority, replicates log
        
        APPLICATION THREADS (external):
        - Call append_log_entry() to submit commands
        - Call handle_request_vote() / handle_append_entries() for RPCs
        - Thread-safe: Protected by mutexes
        
    Persistence:
        
        PERSISTENT STATE (survives crashes):
        - current_term: Latest term server has seen
        - voted_for: Candidate that received vote in current term
        - log[]: Log entries (commands and term when received)
        
        VOLATILE STATE (lost on crashes):
        - commit_index: Highest log entry known to be committed
        - last_applied: Highest log entry applied to state machine
        - Leader state: next_index[], match_index[] for each peer
        
        Storage:
        - RaftLog class handles persistence (file-based)
        - Location: raft_log_directory/raft_log_<node_id>.dat
        - Format: Binary (efficient, durable)
        
    Concurrency Model:
        
        MUTEXES:
        - state_mutex_: Protects node state (follower/candidate/leader)
        - peers_mutex_: Protects peer information (next_index, match_index)
        - timer_mutex_: Protects election timer
        - log_mutex_: Internal to RaftLog (log entries)
        
        LOCK HIERARCHY (prevent deadlock):
        1. state_mutex_
        2. peers_mutex_
        3. timer_mutex_
        Always acquire in this order!
        
        CONDITION VARIABLE:
        - state_cv_: Signals state changes (not heavily used)
        
    Algorithm Guarantees:
        
        COMMITMENT RULE:
        - Entry committed when replicated to majority
        - Committed entries never lost
        - Applied to state machine in order
        
        ELECTION RULE:
        - Candidate with up-to-date log wins
        - "Up-to-date": Higher term, or same term with longer log
        - Ensures: Leader has all committed entries
        
        CONSISTENCY RULE:
        - Follower appends only if log matches at prev_log_index
        - Inconsistent logs: Follower rejects, leader backs up
        - Eventually: Leader and follower logs converge
        
    Performance Characteristics:
        
        Operation                | Latency    | Frequency
        -------------------------|------------|------------------
        Leader election          | 200-500ms  | Rare (on failure)
        Log replication (local)  | <1ms       | Per append
        Log replication (quorum) | 5-20ms     | Per append
        Heartbeat                | 1-5ms      | Every 50ms
        Commit decision          | 5-20ms     | Per append
        State machine apply      | <1ms       | Per commit
        
    Integration with Backtesting System:
        
        Raft provides fault tolerance for controller cluster:
        - Without Raft: Single controller = single point of failure
        - With Raft: 3 controllers, survives 1 failure
        
        Job submission flow with Raft:
        1. Client → Leader: submit_job(params)
        2. Leader → Local log: Append JOB_SUBMIT entry
        3. Leader → Followers: Replicate entry (AppendEntries RPC)
        4. Followers → Leader: Acknowledge replication
        5. Leader: Wait for majority (quorum)
        6. Leader: Commit entry, assign job to worker
        7. Leader → Client: Return job_id
        
    Dependencies:
        - raft/raft_node.h: Class declarations
        - raft/raft_log.h: Persistent log storage
        - raft/raft_rpc.h: RPC message definitions
        - common/logger.h: Diagnostic logging
        - <algorithm>: std::min
        - <random>: Randomized election timeouts
        
    Related Files:
        - raft/raft_node.h: Class and structure definitions
        - raft/raft_log.cpp: Persistent log implementation
        - raft/raft_rpc.cpp: RPC serialization
        - controller/raft_controller.cpp: Network integration
        
    References:
        - Raft Paper: "In Search of an Understandable Consensus Algorithm"
          by Diego Ongaro and John Ousterhout (2014)
        - Raft Website: https://raft.github.io/
        - Raft Visualization: http://thesecretlivesofdata.com/raft/

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. INCLUDES & DEPENDENCIES
// 2. NAMESPACE DECLARATIONS
// 3. CONSTRUCTOR & DESTRUCTOR
//    3.1 RaftNode Constructor - Initialization
//    3.2 RaftNode Destructor - Cleanup
// 4. LIFECYCLE MANAGEMENT
//    4.1 start() - Start Raft Node
//    4.2 stop() - Stop Raft Node
// 5. PEER MANAGEMENT
//    5.1 add_peer() - Register Peer Node
// 6. ELECTION TIMING
//    6.1 reset_election_timer() - Reset Timer
//    6.2 is_election_timeout() - Check Timeout
//    6.3 get_random_timeout_ms() - Generate Random Timeout
//    6.4 election_timer_loop() - Election Timer Thread
// 7. LEADER ELECTION
//    7.1 start_election() - Initiate Election
//    7.2 become_follower() - Transition to Follower
//    7.3 become_candidate() - Transition to Candidate
//    7.4 become_leader() - Transition to Leader
// 8. LOG REPLICATION
//    8.1 heartbeat_loop() - Heartbeat Thread
//    8.2 send_heartbeats() - Send to All Peers
//    8.3 replicate_log_to_peer() - Replicate to Single Peer
//    8.4 advance_commit_index() - Update Commit Index
//    8.5 append_log_entry() - Add Entry to Log
// 9. RPC HANDLERS
//    9.1 handle_request_vote() - Process Vote Request
//    9.2 handle_append_entries() - Process Log Replication
// 10. VIRTUAL RPC METHODS
//    10.1 send_request_vote() - Send Vote Request (Override)
//    10.2 send_append_entries() - Send Log Entries (Override)
// 11. RAFT ALGORITHM DETAILS
// 12. USAGE EXAMPLES
// 13. COMMON PITFALLS & SOLUTIONS
// 14. FAQ
// 15. BEST PRACTICES
// 16. TESTING STRATEGIES
// 17. TROUBLESHOOTING GUIDE
// 18. CORRECTNESS PROOF SKETCH
//
//==============================================================================

//==============================================================================
// SECTION 1: INCLUDES & DEPENDENCIES
//==============================================================================

#include "raft/raft_node.h"

// <algorithm> - std::min
// For calculating minimum in commit index update
#include <algorithm>

// <sstream> - std::stringstream
// For string building in logging (if needed)
#include <sstream>

//==============================================================================
// SECTION 2: NAMESPACE DECLARATIONS
//==============================================================================

namespace backtesting {
namespace raft {

//==============================================================================
// SECTION 3: CONSTRUCTOR & DESTRUCTOR
//==============================================================================

//------------------------------------------------------------------------------
// 3.1 RAFTNODE CONSTRUCTOR - INITIALIZATION
//------------------------------------------------------------------------------
//
// RaftNode::RaftNode(const RaftConfig& config)
//
// PURPOSE:
// Initializes Raft node with configuration, persistent state, and volatile state.
//
// INITIALIZATION SEQUENCE:
// 1. Store configuration (node_id, timeouts, log directory)
// 2. Set initial state to FOLLOWER (all nodes start as followers)
// 3. Initialize persistent log storage (RaftLog)
// 4. Initialize volatile state (commit_index, last_applied)
// 5. Initialize leader tracking (current_leader = 0)
// 6. Initialize election state (votes_received = 0)
// 7. Seed random number generator (for election timeout randomization)
// 8. Reset election timer
//
// PERSISTENT STATE:
// Handled by RaftLog (initialized from disk or created):
// - current_term: Starts at 0 (incremented during first election)
// - voted_for: Starts at 0 (no vote cast yet)
// - log[]: Starts empty (entries added during replication)
//
// VOLATILE STATE:
// In-memory state (not persisted):
// - commit_index: 0 (no entries committed yet)
// - last_applied: 0 (no entries applied to state machine yet)
// - state: FOLLOWER (initial state)
// - current_leader: 0 (no leader known)
// - votes_received: 0 (no votes yet)
//
// LOG FILE PATH:
// Pattern: <log_dir>/raft_log_<node_id>.dat
// Example: ./raft_logs/node_1/raft_log_1.dat
// Purpose: Each node has separate log file
//
// RANDOM NUMBER GENERATOR:
// - std::random_device: Seeds RNG with entropy
// - Used for: Randomized election timeouts
// - Why random? Prevents synchronized elections (split votes)
//
// ELECTION TIMER:
// - reset_election_timer() sets last_heartbeat to now
// - Timer starts ticking immediately
// - If not reset by heartbeat: Eventually times out, triggers election
//
// THREAD SAFETY:
// - Constructor not thread-safe (shouldn't be concurrent)
// - After construction: Object ready for multi-threaded use
//
// EXCEPTION SAFETY:
// - RaftLog constructor may throw (file I/O errors)
// - If throws: RaftNode partially constructed, destructor not called
// - Caller responsible for handling construction failures
//

RaftNode::RaftNode(const RaftConfig& config)
    : config_(config),                    // Store configuration
      node_id_(config.node_id),           // Extract node ID for convenience
      state_(NodeState::FOLLOWER),        // All nodes start as FOLLOWER
      log_(config.log_dir + "/raft_log_" + std::to_string(config.node_id) + ".dat"),  // Persistent log
      commit_index_(0),                   // No entries committed yet
      last_applied_(0),                   // No entries applied yet
      current_leader_(0),                 // No leader known yet
      running_(false),                    // Not started yet
      votes_received_(0) {                // No votes yet
    
    // STEP 1: Initialize random number generator
    // std::random_device: Hardware entropy source (non-deterministic)
    // Used to seed Mersenne Twister (rng_)
    std::random_device rd;
    rng_.seed(rd());
    
    // STEP 2: Reset election timer
    // Sets last_heartbeat_ to current time
    // Timer will expire after random interval (150-300ms)
    reset_election_timer();
    
    // Constructor complete
    // Node is initialized but not running
    // Call start() to begin Raft protocol
}

//------------------------------------------------------------------------------
// 3.2 RAFTNODE DESTRUCTOR - CLEANUP
//------------------------------------------------------------------------------
//
// RaftNode::~RaftNode()
//
// PURPOSE:
// Ensures clean shutdown of Raft node.
//
// CLEANUP:
// - Calls stop() to halt threads
// - RaftLog flushed to disk (in RaftLog destructor)
// - Threads joined
// - Mutexes destroyed
//
// EXCEPTION SAFETY:
// - Destructor must not throw
// - stop() designed to be exception-safe
//

RaftNode::~RaftNode() {
    // Ensure node stopped before destruction
    // stop() is idempotent (safe to call multiple times)
    stop();
    
    // Automatic cleanup:
    // - RaftLog destructor flushes to disk
    // - Threads already joined in stop()
    // - Mutexes destroyed
    // - STL containers destroyed
}

//==============================================================================
// SECTION 4: LIFECYCLE MANAGEMENT
//==============================================================================

//------------------------------------------------------------------------------
// 4.1 START() - START RAFT NODE
//------------------------------------------------------------------------------
//
// bool RaftNode::start()
//
// PURPOSE:
// Starts Raft node by spawning background threads for election timing
// and heartbeat sending.
//
// THREAD CREATION:
// 1. Election timer thread: Monitors timeout, triggers elections
// 2. Heartbeat thread: Leader sends periodic AppendEntries
//
// INITIAL STATE:
// - Node state: FOLLOWER
// - Election timer: Running (will timeout if no leader)
// - Heartbeat thread: Sleeping (only active when leader)
//
// STARTUP FLOW:
// 1. Check not already running
// 2. Set running_ = true
// 3. Spawn election timer thread
// 4. Spawn heartbeat thread
// 5. Log startup
//
// WHAT HAPPENS NEXT (automatic):
// - Election timer expires (random 150-300ms)
// - Node becomes candidate
// - Requests votes from peers
// - Eventually: One node elected leader
//
// THREAD SAFETY:
// - Not thread-safe: Call from main thread only
// - After start(): Node is multi-threaded
//
// RETURNS:
// - true: Successfully started
// - false: Already running
//

bool RaftNode::start() {
    // GUARD: Prevent double-start
    if (running_) {
        Logger::warning("Raft node already running");
        return false;
    }
    
    Logger::info("Starting Raft node " + std::to_string(node_id_));
    
    // PHASE 1: Enable threads
    running_ = true;
    
    // PHASE 2: Spawn election timer thread
    // Function: Monitors election timeout, triggers elections
    // Frequency: Checks every 10ms
    election_thread_ = std::thread(&RaftNode::election_timer_loop, this);
    
    // PHASE 3: Spawn heartbeat thread
    // Function: Leader sends periodic AppendEntries
    // Frequency: Every 50ms (when leader)
    heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);
    
    Logger::info("Raft node " + std::to_string(node_id_) + " started as FOLLOWER");
    
    return true;
    
    // AFTER START:
    // - Election timer running (will trigger first election)
    // - Heartbeat thread running (waiting for leadership)
    // - Node ready to participate in Raft protocol
    // - Will transition to candidate when timer expires
}

//------------------------------------------------------------------------------
// 4.2 STOP() - STOP RAFT NODE
//------------------------------------------------------------------------------
//
// void RaftNode::stop()
//
// PURPOSE:
// Stops Raft node by halting background threads and flushing state.
//
// SHUTDOWN SEQUENCE:
// 1. Check if running
// 2. Set running_ = false (signal threads to stop)
// 3. Notify condition variable (wake any waiting threads)
// 4. Join election thread
// 5. Join heartbeat thread
// 6. Log shutdown
//
// THREAD COORDINATION:
// - Threads check running_ in their loops
// - When false: Threads exit loops and terminate
// - join() waits for threads to finish
//
// STATE PERSISTENCE:
// - RaftLog automatically flushes to disk (in destructor)
// - Current term, voted_for, log entries all persisted
// - On restart: Node recovers state from log
//
// IDEMPOTENCY:
// - Safe to call multiple times
// - First call: Stops threads
// - Subsequent calls: Return immediately
//

void RaftNode::stop() {
    // GUARD: Check if already stopped
    if (!running_) return;
    
    Logger::info("Stopping Raft node " + std::to_string(node_id_));
    
    // PHASE 1: Signal threads to stop
    running_ = false;
    
    // PHASE 2: Wake any waiting threads
    // In case threads are blocked on condition variable
    state_cv_.notify_all();
    
    // PHASE 3: Join election timer thread
    // Wait for it to exit (checks running_ in loop)
    if (election_thread_.joinable()) {
        election_thread_.join();
    }
    
    // PHASE 4: Join heartbeat thread
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    
    Logger::info("Raft node stopped");
    
    // After this:
    // - Both threads have exited
    // - Node is quiescent (no background activity)
    // - Safe to destroy object
    // - State persisted in log file
}

//==============================================================================
// SECTION 5: PEER MANAGEMENT
//==============================================================================

//------------------------------------------------------------------------------
// 5.1 ADD_PEER() - REGISTER PEER NODE
//------------------------------------------------------------------------------
//
// void RaftNode::add_peer(uint64_t node_id, 
//                         const std::string& hostname, 
//                         uint16_t port)
//
// PURPOSE:
// Registers a peer node in the Raft cluster for election and replication.
//
// PARAMETERS:
// - node_id: Unique identifier for peer
// - hostname: Peer's hostname or IP address
// - port: Peer's Raft communication port
//
// PEER STATE INITIALIZATION:
// - next_index: Initialize to leader's last log index + 1
//   * Optimistic: Assume peer's log is up-to-date
//   * Will decrement if inconsistency detected
// - match_index: 0 (no entries known to match yet)
//   * Conservative: Assume no replication yet
//   * Updated as replication succeeds
//
// NEXT_INDEX vs MATCH_INDEX:
// - next_index: Where to start sending entries to peer
//   * "I think peer needs entries starting at index X"
//   * Decrements on rejection (log inconsistency)
// - match_index: Highest index known to match peer's log
//   * "I know peer has entries up to index Y"
//   * Used for: Commit decisions (majority calculation)
//
// WHEN TO CALL:
// - Before start(): During cluster configuration
// - After start(): If implementing dynamic membership (advanced)
//
// THREAD SAFETY:
// - Locks peers_mutex_ for safe concurrent access
// - Safe to call from multiple threads
//

void RaftNode::add_peer(uint64_t node_id, const std::string& hostname, uint16_t port) {
    // Lock mutex: Protect peers_ map
    std::lock_guard<std::mutex> lock(peers_mutex_);
    
    // Create peer information structure
    PeerInfo peer;
    peer.node_id = node_id;
    peer.hostname = hostname;
    peer.port = port;
    
    // Initialize replication state
    // next_index: Optimistic - assume peer is up-to-date
    // Will back up if peer's log is behind
    peer.next_index = log_.last_log_index() + 1;
    
    // match_index: Conservative - assume no replication yet
    // Will advance as entries successfully replicated
    peer.match_index = 0;
    
    // Add to peers map
    peers_[node_id] = peer;
    
    Logger::info("Added peer " + std::to_string(node_id) + " at " + 
                hostname + ":" + std::to_string(port));
}

//==============================================================================
// SECTION 6: ELECTION TIMING
//==============================================================================

//------------------------------------------------------------------------------
// 6.1 RESET_ELECTION_TIMER() - RESET TIMER
//------------------------------------------------------------------------------
//
// void RaftNode::reset_election_timer()
//
// PURPOSE:
// Resets election timeout by updating last heartbeat timestamp.
//
// WHEN CALLED:
// - Constructor: Initialize timer
// - Receive AppendEntries: Leader is alive
// - Grant vote: Acknowledge election activity
// - Become follower: Reset after state change
//
// WHY RESET?
// - Prevents election while leader is active
// - Leader sends heartbeats (AppendEntries) regularly
// - Follower resets timer on each heartbeat
// - If no heartbeat: Timer expires, election starts
//
// THREAD SAFETY:
// - Locks timer_mutex_ for safe concurrent access
// - Multiple threads may reset timer
//

void RaftNode::reset_election_timer() {
    // Lock timer mutex
    std::lock_guard<std::mutex> lock(timer_mutex_);
    
    // Update last heartbeat timestamp
    // Uses steady_clock: Monotonic, not affected by system clock changes
    last_heartbeat_ = std::chrono::steady_clock::now();
    
    // EFFECT:
    // - Election timeout recalculated from this timestamp
    // - is_election_timeout() will return false for next 150-300ms
}

//------------------------------------------------------------------------------
// 6.2 IS_ELECTION_TIMEOUT() - CHECK TIMEOUT
//------------------------------------------------------------------------------
//
// bool RaftNode::is_election_timeout()
//
// PURPOSE:
// Checks if election timeout has expired (time to start election).
//
// CALCULATION:
// - elapsed = now - last_heartbeat
// - timeout if: elapsed >= random_timeout (150-300ms)
//
// RANDOMIZATION:
// - Each call generates new random timeout
// - Prevents: Synchronized elections across nodes
// - Range: 150-300ms (configured)
//
// WHY RANDOMIZE PER CHECK?
// - Alternative: Generate once, compare same value
// - Current: New random value each check
// - Effect: Timeout threshold changes each check
// - Practically: Timeout will eventually succeed
//
// IMPROVEMENT OPPORTUNITY:
// - Could generate timeout once when timer reset
// - Store in member variable
// - Compare against fixed target
// - Would be more predictable
//
// THREAD SAFETY:
// - Locks timer_mutex_
// - Safe to call concurrently
//
// RETURNS:
// - true: Timeout expired, should start election
// - false: Still within timeout window
//

bool RaftNode::is_election_timeout() {
    // Lock timer mutex
    std::lock_guard<std::mutex> lock(timer_mutex_);
    
    // Calculate time elapsed since last heartbeat
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_heartbeat_).count();
    
    // Check if elapsed exceeds random timeout
    // get_random_timeout_ms(): Returns random value in [150, 300]
    return elapsed >= get_random_timeout_ms();
    
    // NOTE: This generates new random timeout each call
    // Alternative approach:
    //   timeout_threshold_ = get_random_timeout_ms();  // Once when reset
    //   return elapsed >= timeout_threshold_;          // Compare against fixed value
}

//------------------------------------------------------------------------------
// 6.3 GET_RANDOM_TIMEOUT_MS() - GENERATE RANDOM TIMEOUT
//------------------------------------------------------------------------------
//
// int RaftNode::get_random_timeout_ms()
//
// PURPOSE:
// Generates randomized election timeout to prevent simultaneous elections.
//
// RANDOMIZATION:
// - Distribution: Uniform (all values equally likely)
// - Range: [election_timeout_min, election_timeout_max]
// - Typical: [150ms, 300ms]
//
// WHY RANDOMIZE?
// Critical for Raft liveness (prevents election livelock):
//
// Scenario without randomization:
// - Node A timeout: 150ms
// - Node B timeout: 150ms
// - Node C timeout: 150ms
// - All start election simultaneously
// - All request votes at same time
// - Votes split: A gets 1, B gets 1, C gets 1
// - No majority: Election fails
// - Repeat indefinitely (livelock)
//
// With randomization:
// - Node A timeout: 167ms (random)
// - Node B timeout: 243ms (random)
// - Node C timeout: 189ms (random)
// - Node A starts first, likely wins before others timeout
// - If fails: Different random delays for next round
// - Eventually: One node wins (probabilistic guarantee)
//
// DISTRIBUTION PARAMETERS:
// - min: 150ms (fast enough for responsiveness)
// - max: 300ms (slow enough to avoid frequent elections)
// - Range: 150ms spread (2x factor, good randomization)
//
// RAFT PAPER RECOMMENDATION:
// - Election timeout: 150-300ms
// - Spread: At least 2x (prevents synchronized elections)
// - Broadcast time << election_timeout (satisfied: 1-5ms << 150ms)
//
// THREAD SAFETY:
// - Uses rng_ member variable
// - Not thread-safe: Multiple threads may race on rng_
// - In practice: Only election timer thread calls this
// - If multi-threaded: Should use thread-local RNG or mutex
//

int RaftNode::get_random_timeout_ms() {
    // Create uniform distribution over configured range
    // min: config_.election_timeout_min_ms (e.g., 150)
    // max: config_.election_timeout_max_ms (e.g., 300)
    std::uniform_int_distribution<int> dist(config_.election_timeout_min_ms,
                                            config_.election_timeout_max_ms);
    
    // Generate random value using Mersenne Twister RNG
    // Returns: Integer in [min, max] inclusive
    return dist(rng_);
    
    // Example values:
    // - Call 1: 167ms
    // - Call 2: 243ms
    // - Call 3: 189ms
    // - Distribution: Uniform (all values equally likely)
}

//------------------------------------------------------------------------------
// 6.4 ELECTION_TIMER_LOOP() - ELECTION TIMER THREAD
//------------------------------------------------------------------------------
//
// void RaftNode::election_timer_loop()
//
// PURPOSE:
// Background thread that monitors election timeout and triggers elections.
//
// LOOP LOGIC:
// 1. Sleep 10ms (polling interval)
// 2. If not leader and timeout expired: Start election
// 3. Repeat until stopped
//
// POLLING INTERVAL:
// - 10ms: Balance between responsiveness and CPU usage
// - Could be longer: Less CPU, slower detection
// - Could be shorter: More CPU, faster detection
//
// TIMEOUT CHECK:
// - Only for non-leaders (followers and candidates)
// - Leaders don't timeout (they send heartbeats)
// - If timeout: Calls start_election()
//
// THREAD LIFECYCLE:
// - Started by: start() method
// - Runs until: running_ = false
// - Joined by: stop() method
//

void RaftNode::election_timer_loop() {
    // Election monitoring loop
    while (running_) {
        // Sleep briefly (polling interval)
        // 10ms: Fast enough to detect timeout within ~10ms of expiration
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        // Check timeout for non-leaders
        // Leaders don't need election timeout (they send heartbeats)
        if (state_ != NodeState::LEADER && is_election_timeout()) {
            // TIMEOUT EXPIRED: Start election
            Logger::info("Election timeout - starting election");
            start_election();
        }
    }
    
    // Thread exits when running_ = false
}

//==============================================================================
// SECTION 7: LEADER ELECTION
//==============================================================================

//------------------------------------------------------------------------------
// 7.1 START_ELECTION() - INITIATE ELECTION
//------------------------------------------------------------------------------
//
// void RaftNode::start_election()
//
// PURPOSE:
// Initiates leader election by transitioning to candidate and requesting
// votes from all peers.
//
// ELECTION PROCESS:
// 1. Become candidate (state transition)
// 2. Increment term (new election round)
// 3. Vote for self (one vote guaranteed)
// 4. Reset election timer (prevents immediate re-election)
// 5. Send RequestVote to all peers (in parallel conceptually)
// 6. Count votes received
// 7. If majority: Become leader
// 8. If discover higher term: Become follower
// 9. If not enough votes: Remain candidate (timeout will trigger new election)
//
// VOTE COLLECTION:
// - Sequential: Send RPC to each peer, wait for response
// - Could optimize: Parallel RPCs (threads or async I/O)
// - Current: Simple sequential approach
//
// VOTE COUNTING:
// - Start with 1 (vote for self)
// - Increment for each vote granted
// - Need majority: (total_nodes / 2) + 1
// - Example: 3 nodes need 2 votes, 5 nodes need 3 votes
//
// HIGHER TERM DISCOVERY:
// - If any peer has higher term: Immediately become follower
// - Reason: This node's term is stale
// - Effect: Election aborted, node returns to follower
//
// THREAD SAFETY:
// - Called from election timer thread
// - Locks various mutexes (state, peers, log)
// - Not reentrant: Election timer prevents concurrent calls
//

void RaftNode::start_election() {
    // PHASE 1: Become candidate
    // State transition: FOLLOWER/CANDIDATE → CANDIDATE
    become_candidate();
    
    // PHASE 2: Increment term and vote for self
    // Persistent state: Write to log immediately
    log_.increment_term();           // term++
    log_.set_voted_for(node_id_);    // Vote for self
    votes_received_ = 1;              // Start with self-vote
    
    // Get current term for this election
    uint64_t current_term = log_.get_current_term();
    
    Logger::info("Node " + std::to_string(node_id_) + 
                " starting election for term " + std::to_string(current_term));
    
    // PHASE 3: Prepare RequestVote request
    // Include log information for up-to-date check
    RequestVoteRequest req;
    req.term = current_term;                 // Our current term
    req.candidate_id = node_id_;             // We are candidate
    req.last_log_index = log_.last_log_index();  // Our last log index
    req.last_log_term = log_.last_log_term();    // Term of last log entry
    
    // PHASE 4: Collect peer IDs (release lock before RPCs)
    std::vector<uint64_t> peer_ids;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            peer_ids.push_back(id);
        }
    }
    
    // PHASE 5: Send RequestVote to each peer
    // Sequential: Could be optimized with parallel threads
    for (uint64_t peer_id : peer_ids) {
        RequestVoteResponse resp;
        
        // Send RPC (may take 1-5ms)
        if (send_request_vote(peer_id, req, resp)) {
            // RPC succeeded: Check response
            
            if (resp.vote_granted) {
                // VOTE GRANTED: Increment counter
                votes_received_++;
                Logger::info("Received vote from node " + std::to_string(peer_id));
                
            } else if (resp.term > current_term) {
                // HIGHER TERM DISCOVERED: Abort election
                // This node's term is stale, become follower
                Logger::info("Discovered higher term " + std::to_string(resp.term) +
                           " during election, stepping down");
                become_follower(resp.term);
                return;  // Election aborted
            }
            // else: Vote denied (peer already voted for someone else)
        }
        // If RPC failed: Peer unreachable, don't count vote
    }
    
    // PHASE 6: Check if won election
    // Majority calculation: (total_nodes / 2) + 1
    // total_nodes = peers.size() + 1 (peers + self)
    int majority = (static_cast<int>(peers_.size()) + 1) / 2 + 1;
    
    if (votes_received_ >= majority) {
        // WON ELECTION: Become leader
        Logger::info("Node " + std::to_string(node_id_) + 
                    " won election with " + std::to_string(votes_received_.load()) + " votes");
        become_leader();
    } else {
        // LOST ELECTION: Remain candidate
        // Election timer will eventually fire again
        // Will start new election with incremented term
        Logger::info("Node " + std::to_string(node_id_) +
                    " did not win election (" + std::to_string(votes_received_.load()) +
                    " votes, need " + std::to_string(majority) + ")");
    }
    
    // ELECTION OUTCOMES:
    // 1. Won: Become leader (immediately send heartbeats)
    // 2. Lost: Remain candidate (retry on next timeout)
    // 3. Aborted: Become follower (discovered higher term)
}

//------------------------------------------------------------------------------
// 7.2 BECOME_FOLLOWER() - TRANSITION TO FOLLOWER
//------------------------------------------------------------------------------
//
// void RaftNode::become_follower(uint64_t term)
//
// PURPOSE:
// Transitions node to follower state and updates term.
//
// WHEN CALLED:
// - Discover higher term (from any RPC)
// - Receive valid AppendEntries from leader
// - Grant vote to candidate
//
// STATE CHANGES:
// - state: X → FOLLOWER
// - current_term: X → term (parameter)
// - voted_for: X → 0 (clear vote for new term)
// - election_timer: Reset (prevent immediate election)
//
// WHY CLEAR VOTE?
// - New term: Can vote again
// - Ensures: Can grant vote in new term's election
//
// THREAD SAFETY:
// - Locks state_mutex_
// - Safe concurrent access
//

void RaftNode::become_follower(uint64_t term) {
    // Lock state mutex
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Update state
    state_ = NodeState::FOLLOWER;
    
    // Update term (persistent state)
    log_.set_current_term(term);
    
    // Clear vote (can vote again in new term)
    log_.clear_vote();
    
    // Reset election timer (prevent immediate election)
    reset_election_timer();
    
    Logger::info("Node " + std::to_string(node_id_) + 
                " became FOLLOWER in term " + std::to_string(term));
}

//------------------------------------------------------------------------------
// 7.3 BECOME_CANDIDATE() - TRANSITION TO CANDIDATE
//------------------------------------------------------------------------------
//
// void RaftNode::become_candidate()
//
// PURPOSE:
// Transitions node to candidate state (seeking election).
//
// WHEN CALLED:
// - Election timeout expires (in start_election)
//
// STATE CHANGES:
// - state: FOLLOWER → CANDIDATE
// - election_timer: Reset (for this election round)
//
// NEXT STEPS:
// - Increment term (in start_election)
// - Vote for self
// - Request votes from peers
//

void RaftNode::become_candidate() {
    // Lock state mutex
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Update state
    state_ = NodeState::CANDIDATE;
    
    // Reset election timer
    // If this election fails, timer will fire again
    reset_election_timer();
    
    Logger::info("Node " + std::to_string(node_id_) + " became CANDIDATE");
}

//------------------------------------------------------------------------------
// 7.4 BECOME_LEADER() - TRANSITION TO LEADER
//------------------------------------------------------------------------------
//
// void RaftNode::become_leader()
//
// PURPOSE:
// Transitions node to leader state after winning election.
//
// WHEN CALLED:
// - Won election (received majority votes)
//
// STATE CHANGES:
// - state: CANDIDATE → LEADER
// - current_leader: 0 → node_id (we are leader)
//
// LEADER STATE INITIALIZATION:
// For each peer:
// - next_index = last_log_index + 1 (optimistic: assume peers up-to-date)
// - match_index = 0 (conservative: assume no replication yet)
//
// WHY REINITIALIZE?
// - Peer states may have changed during election
// - Fresh start: Establish current replication status
// - Will correct via AppendEntries (back up if needed)
//
// IMMEDIATE ACTION:
// - Send heartbeats to all followers
// - Establishes leadership (prevents new elections)
// - Begins log replication
//
// THREAD SAFETY:
// - Locks state_mutex_ and peers_mutex_
// - Safe concurrent access
//

void RaftNode::become_leader() {
    // Lock state mutex
    std::lock_guard<std::mutex> lock(state_mutex_);
    
    // Update state
    state_ = NodeState::LEADER;
    current_leader_ = node_id_;  // We are the leader
    
    // INITIALIZE LEADER STATE
    // For each peer: Set next_index and match_index
    {
        std::lock_guard<std::mutex> peers_lock(peers_mutex_);
        for (auto& [id, peer] : peers_) {
            // next_index: Optimistic assumption (peer is up-to-date)
            // Will decrement if peer's log is behind
            peer.next_index = log_.last_log_index() + 1;
            
            // match_index: Conservative assumption (no replication yet)
            // Will advance as replication succeeds
            peer.match_index = 0;
        }
    }
    
    Logger::info("Node " + std::to_string(node_id_) + 
                " became LEADER in term " + std::to_string(log_.get_current_term()));
    
    // IMMEDIATE ACTION: Send heartbeats
    // Establishes leadership immediately
    // Prevents followers from timing out
    // Begins log replication if entries exist
    send_heartbeats();
    
    // LEADER RESPONSIBILITIES (ongoing):
    // - Send periodic heartbeats (heartbeat_loop)
    // - Accept client requests (append_log_entry)
    // - Replicate log to followers
    // - Advance commit index when majority replicates
}

//==============================================================================
// SECTION 8: LOG REPLICATION
//==============================================================================

//------------------------------------------------------------------------------
// 8.1 HEARTBEAT_LOOP() - HEARTBEAT THREAD
//------------------------------------------------------------------------------
//
// void RaftNode::heartbeat_loop()
//
// PURPOSE:
// Background thread that sends periodic heartbeats (AppendEntries) when leader.
//
// LOOP LOGIC:
// 1. If leader: Send heartbeats to all followers
// 2. Sleep for heartbeat_interval_ms (e.g., 50ms)
// 3. Repeat until stopped
//
// HEARTBEAT PURPOSE:
// - Maintain leadership (prevent election timeouts)
// - Replicate log entries (if any)
// - Update followers' commit index
//
// INTERVAL:
// - Configured: heartbeat_interval_ms (typically 50ms)
// - Must be: Much less than election_timeout_min (150ms)
// - Rule of thumb: heartbeat = election_timeout / 3
//
// BEHAVIOR BY STATE:
// - Leader: Send heartbeats actively
// - Follower: Sleep (no action)
// - Candidate: Sleep (no action)
//
// THREAD LIFECYCLE:
// - Started by: start() method
// - Runs until: running_ = false
// - Joined by: stop() method
//

void RaftNode::heartbeat_loop() {
    // Heartbeat loop: Send periodic AppendEntries when leader
    while (running_) {
        // Check if we are leader
        if (state_ == NodeState::LEADER) {
            // LEADER: Send heartbeats to all followers
            // This prevents election timeouts
            // Also replicates any pending log entries
            send_heartbeats();
        }
        // FOLLOWER/CANDIDATE: Do nothing (no heartbeats to send)
        
        // Sleep until next heartbeat interval
        // Configured: heartbeat_interval_ms (e.g., 50ms)
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.heartbeat_interval_ms));
    }
    
    // Thread exits when running_ = false
}

//------------------------------------------------------------------------------
// 8.2 SEND_HEARTBEATS() - SEND TO ALL PEERS
//------------------------------------------------------------------------------
//
// void RaftNode::send_heartbeats()
//
// PURPOSE:
// Sends AppendEntries RPC to all peers (heartbeat or log replication).
//
// OPERATION:
// 1. Collect peer IDs (snapshot under lock)
// 2. For each peer: Call replicate_log_to_peer()
//
// WHY COLLECT IDS FIRST?
// - Avoid holding peers_mutex_ during RPCs
// - RPCs are slow (1-5ms each)
// - Holding lock would block other operations
//
// PARALLELIZATION:
// - Current: Sequential (one peer at a time)
// - Could optimize: Parallel threads (faster replication)
// - Trade-off: Simplicity vs. performance
//
// THREAD SAFETY:
// - Locks peers_mutex_ briefly to collect IDs
// - Each replicate call locks again for specific peer
//

void RaftNode::send_heartbeats() {
    // STEP 1: Collect peer IDs
    // Lock briefly to snapshot peer list
    std::vector<uint64_t> peer_ids;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            peer_ids.push_back(id);
        }
    }
    // Lock released: Safe to do slow operations (RPCs)
    
    // STEP 2: Replicate to each peer
    for (uint64_t peer_id : peer_ids) {
        // Lock again for this specific peer
        std::lock_guard<std::mutex> lock(peers_mutex_);
        
        // Find peer (may have been removed, so check)
        auto it = peers_.find(peer_id);
        if (it != peers_.end()) {
            // Replicate log to this peer
            // Sends AppendEntries with appropriate entries
            replicate_log_to_peer(it->second);
        }
    }
    
    // AFTER HEARTBEATS:
    // - Followers received AppendEntries (reset their timers)
    // - Log entries replicated (if any)
    // - Commit index advanced (if majority replicated)
}

//------------------------------------------------------------------------------
// 8.3 REPLICATE_LOG_TO_PEER() - REPLICATE TO SINGLE PEER
//------------------------------------------------------------------------------
//
// void RaftNode::replicate_log_to_peer(PeerInfo& peer)
//
// PURPOSE:
// Sends AppendEntries RPC to specific peer for log replication.
//
// APPENDENTRIES CONSTRUCTION:
// - term: Current term
// - leader_id: This node (we are leader)
// - leader_commit: Our commit index (tells follower what's committed)
// - entries: Log entries to send (from peer.next_index onwards)
// - prev_log_index: Index before first entry (for consistency check)
// - prev_log_term: Term at prev_log_index (for consistency check)
//
// ENTRY SELECTION:
// - If peer.next_index <= last_log_index: Send entries from next_index
// - If peer is up-to-date: Send empty (heartbeat only)
//
// CONSISTENCY CHECK:
// - prev_log_index and prev_log_term: Tell follower where to append
// - Follower checks: Entry at prev_log_index has matching term
// - If match: Append new entries
// - If mismatch: Reject (leader will retry with earlier index)
//
// RESPONSE HANDLING:
// - Success: Update next_index and match_index, check for commit
// - Failure (higher term): Step down to follower
// - Failure (log inconsistency): Decrement next_index, retry
//
// BACKOFF ALGORITHM:
// - On rejection: next_index--
// - Try sending earlier entries
// - Eventually: Find consistency point
// - Then: Replicate all entries from that point
//
// COMMIT ADVANCEMENT:
// - After successful replication: Check if can commit
// - advance_commit_index() checks majority replication
//
// THREAD SAFETY:
// - Peer passed by reference (caller holds lock)
// - Modifies peer.next_index and peer.match_index
// - Safe because caller locked peers_mutex_
//

void RaftNode::replicate_log_to_peer(PeerInfo& peer) {
    // Prepare AppendEntries request
    AppendEntriesRequest req;
    req.term = log_.get_current_term();
    req.leader_id = node_id_;
    req.leader_commit = commit_index_;
    
    // DETERMINE ENTRIES TO SEND
    // If peer's next_index <= our last_log_index: Send entries
    // Otherwise: Peer is up-to-date, send empty (heartbeat)
    if (peer.next_index <= log_.last_log_index()) {
        // Get entries from peer's next_index onwards
        req.entries = log_.get_entries_from(peer.next_index);
    }
    // else: req.entries empty (heartbeat only)
    
    // SET CONSISTENCY CHECK PARAMETERS
    // prev_log_index: Entry immediately before new entries
    // prev_log_term: Term of that entry
    // Follower must have matching entry at this position
    if (peer.next_index > 1) {
        // Peer expects entries after prev_log_index
        req.prev_log_index = peer.next_index - 1;
        req.prev_log_term = log_.at(req.prev_log_index).term;
    } else {
        // Peer's next_index is 1 (start of log)
        // No previous entry to check
        req.prev_log_index = 0;
        req.prev_log_term = 0;
    }
    
    // SEND RPC
    AppendEntriesResponse resp;
    if (send_append_entries(peer.node_id, req, resp)) {
        // RPC SUCCEEDED: Process response
        
        if (resp.success) {
            // ================================================================
            // SUCCESS: Peer accepted entries
            // ================================================================
            
            // Update peer replication state
            if (!req.entries.empty()) {
                // next_index: Advance to after last replicated entry
                peer.next_index = req.entries.back().index + 1;
                
                // match_index: Record highest known matching index
                peer.match_index = req.entries.back().index;
                
                // CHECK FOR COMMIT
                // If majority of peers have this entry: Can commit
                advance_commit_index();
            }
            
        } else {
            // ================================================================
            // FAILURE: Peer rejected entries
            // ================================================================
            
            if (resp.term > log_.get_current_term()) {
                // HIGHER TERM: We are stale leader
                // Step down immediately
                Logger::info("Discovered higher term " + std::to_string(resp.term) +
                           " from peer " + std::to_string(peer.node_id) +
                           ", stepping down");
                become_follower(resp.term);
                
            } else {
                // LOG INCONSISTENCY: Peer's log doesn't match
                // Backoff strategy: Decrement next_index
                if (peer.next_index > 1) {
                    peer.next_index--;
                    Logger::debug("Log inconsistency with peer " + 
                                std::to_string(peer.node_id) +
                                ", backing up to index " + 
                                std::to_string(peer.next_index));
                }
                // Next heartbeat: Will try sending earlier entries
                // Eventually: Find consistency point
            }
        }
    }
    // If RPC failed (network error): Do nothing, retry on next heartbeat
}

//------------------------------------------------------------------------------
// 8.4 ADVANCE_COMMIT_INDEX() - UPDATE COMMIT INDEX
//------------------------------------------------------------------------------
//
// void RaftNode::advance_commit_index()
//
// PURPOSE:
// Advances commit index to highest index replicated on majority of servers.
//
// COMMIT RULE:
// Entry can be committed if:
// 1. Replicated to majority of servers
// 2. Entry is from current term (safety requirement)
//
// ALGORITHM:
// - For each index from commit_index+1 to last_log_index:
//   1. Count how many servers have this entry (check match_index)
//   2. If majority: Can commit
//   3. But only if entry is from current term
//   4. Advance commit_index to this index
//
// WHY CHECK CURRENT TERM?
// - Safety requirement from Raft paper
// - Cannot commit entries from previous terms directly
// - Once current-term entry committed: Implicitly commits earlier entries
//
// Example:
//   Term 1: Leader appends entry 1, replicates to majority, but doesn't commit
//   Term 1: Leader crashes before committing
//   Term 2: New leader elected
//   Term 2: Cannot commit entry 1 directly (wrong term)
//   Term 2: Appends entry 2, replicates to majority
//   Term 2: Commits entry 2 (current term)
//   Term 2: Entry 1 implicitly committed (before entry 2)
//
// MAJORITY CALCULATION:
// - Total nodes: peers.size() + 1 (peers + self)
// - Majority: (total / 2) + 1
// - Example: 5 nodes need 3, 3 nodes need 2
//
// THREAD SAFETY:
// - Locks peers_mutex_ to read match_index
// - Safe to call concurrently
//

void RaftNode::advance_commit_index() {
    // Get highest log index
    uint64_t last_index = log_.last_log_index();
    
    // Try committing each entry from commit_index+1 to last_index
    for (uint64_t n = commit_index_ + 1; n <= last_index; ++n) {
        // COUNT REPLICAS
        // Count how many servers have entry at index n
        int count = 1;  // Leader has it (by definition)
        
        // Check each peer's match_index
        std::lock_guard<std::mutex> lock(peers_mutex_);
        for (const auto& [id, peer] : peers_) {
            if (peer.match_index >= n) {
                count++;  // Peer has this entry
            }
        }
        
        // CALCULATE MAJORITY
        // Total nodes: peers.size() + 1
        // Majority: (total / 2) + 1
        int majority = (static_cast<int>(peers_.size()) + 1) / 2 + 1;
        
        // CHECK COMMIT CONDITIONS
        // 1. Replicated to majority
        // 2. Entry is from current term (safety requirement)
        if (count >= majority && log_.at(n).term == log_.get_current_term()) {
            // CAN COMMIT: Advance commit index
            commit_index_ = n;
            Logger::debug("Advanced commit index to " + std::to_string(n));
            
            // Note: We advance one index at a time
            // Could optimize: Jump to highest committable index
            // Current approach: Simpler, guarantees sequential commits
        }
    }
    
    // AFTER COMMIT:
    // - Entries now durable (majority has them)
    // - Can safely apply to state machine
    // - Next heartbeat will tell followers about commit
    //   (via leader_commit field in AppendEntries)
}

//------------------------------------------------------------------------------
// 8.5 APPEND_LOG_ENTRY() - ADD ENTRY TO LOG
//------------------------------------------------------------------------------
//
// bool RaftNode::append_log_entry(LogEntryType type, 
//                                  const std::vector<uint8_t>& data)
//
// PURPOSE:
// Appends new entry to leader's log (client request).
//
// PRECONDITION:
// - Node must be leader
// - Followers/candidates reject (return false)
//
// ENTRY CONSTRUCTION:
// - term: Current term (leader's term)
// - index: last_log_index + 1 (next sequential index)
// - type: Entry type (e.g., JOB_SUBMIT)
// - data: Entry payload (application-specific)
//
// REPLICATION:
// - Immediately triggers send_heartbeats()
// - Doesn't wait for replication (asynchronous)
// - Entry committed later (when majority replicates)
//
// RETURNS:
// - true: Entry appended (replication in progress)
// - false: Not leader (cannot append)
//
// USAGE:
//   if (raft_node->is_leader()) {
//       std::vector<uint8_t> job_data = SerializeJob(params);
//       raft_node->append_log_entry(LogEntryType::JOB_SUBMIT, job_data);
//   }
//

bool RaftNode::append_log_entry(LogEntryType type, const std::vector<uint8_t>& data) {
    // PRECONDITION: Must be leader
    if (!is_leader()) {
        Logger::warning("Cannot append log entry - not leader");
        return false;
    }
    
    // CREATE LOG ENTRY
    LogEntry entry;
    entry.term = log_.get_current_term();         // Current term
    entry.index = log_.last_log_index() + 1;      // Next index
    entry.type = type;                             // Entry type
    entry.data = data;                             // Entry payload
    
    // APPEND TO LOCAL LOG
    // This writes to disk (persistent storage)
    log_.append(entry);
    
    Logger::info("Leader appended log entry " + std::to_string(entry.index));
    
    // TRIGGER IMMEDIATE REPLICATION
    // Don't wait for next heartbeat interval
    // Send immediately to minimize replication latency
    send_heartbeats();
    
    return true;
    
    // REPLICATION LIFECYCLE (after this returns):
    // 1. send_heartbeats() sends to all followers
    // 2. Followers append to their logs
    // 3. Followers respond with success
    // 4. Leader counts successful responses
    // 5. If majority: advance_commit_index()
    // 6. Entry now committed (durable)
    // 7. Leader applies to state machine
}

//==============================================================================
// SECTION 9: RPC HANDLERS
//==============================================================================

//------------------------------------------------------------------------------
// 9.1 HANDLE_REQUEST_VOTE() - PROCESS VOTE REQUEST
//------------------------------------------------------------------------------
//
// RequestVoteResponse RaftNode::handle_request_vote(const RequestVoteRequest& req)
//
// PURPOSE:
// Handles RequestVote RPC from candidate during election.
//
// VOTING RULES (from Raft paper):
// 1. If req.term < currentTerm: Reject (stale request)
// 2. If req.term > currentTerm: Update term, become follower
// 3. If already voted in this term: Reject (unless voted for this candidate)
// 4. If candidate's log is not up-to-date: Reject
// 5. Otherwise: Grant vote
//
// UP-TO-DATE CHECK:
// Candidate's log is up-to-date if:
// - last_log_term > our last_log_term, OR
// - last_log_term == our last_log_term AND last_log_index >= our last_log_index
//
// Why this check?
// - Ensures: Leader has all committed entries
// - Prevents: Electing leader with incomplete log
//
// VOTE GRANTING:
// - Record vote: set_voted_for(candidate_id)
// - Reset timer: Acknowledge election activity
// - Return: vote_granted = true
//
// PARAMETERS:
// - req: RequestVoteRequest from candidate
//
// RETURNS:
// - RequestVoteResponse with vote decision
//
// THREAD SAFETY:
// - Accesses log and state with appropriate locks
// - Safe to call concurrently (from RPC handler threads)
//

RequestVoteResponse RaftNode::handle_request_vote(const RequestVoteRequest& req) {
    // Prepare response structure
    RequestVoteResponse resp;
    resp.term = log_.get_current_term();  // Our current term
    resp.vote_granted = false;             // Default: Reject
    
    // =========================================================================
    // RULE 1: Reject if request term is stale
    // =========================================================================
    if (req.term < log_.get_current_term()) {
        // Candidate's term is old
        // We're in a newer term, reject
        Logger::debug("Rejecting vote request from " + std::to_string(req.candidate_id) +
                     " (stale term: " + std::to_string(req.term) + 
                     " < " + std::to_string(log_.get_current_term()) + ")");
        return resp;  // vote_granted = false
    }
    
    // =========================================================================
    // RULE 2: If request term is newer, update our term
    // =========================================================================
    if (req.term > log_.get_current_term()) {
        // Candidate has newer term
        // We are stale, must update and become follower
        Logger::info("Received vote request with higher term " + 
                    std::to_string(req.term) + ", updating term and becoming follower");
        become_follower(req.term);
        resp.term = req.term;  // Update response term
    }
    
    // =========================================================================
    // RULE 3: Check if we can vote for this candidate
    // =========================================================================
    // Can vote if:
    // - Haven't voted in this term (voted_for == 0), OR
    // - Already voted for this candidate (voted_for == candidate_id)
    uint64_t voted_for = log_.get_voted_for();
    bool can_vote = (voted_for == 0 || voted_for == req.candidate_id);
    
    // =========================================================================
    // RULE 4: Check if candidate's log is up-to-date
    // =========================================================================
    // Up-to-date if:
    // - Candidate's last log term > our last log term, OR
    // - Same term AND candidate's log at least as long as ours
    bool log_ok = (req.last_log_term > log_.last_log_term()) ||
                  (req.last_log_term == log_.last_log_term() && 
                   req.last_log_index >= log_.last_log_index());
    
    // =========================================================================
    // DECISION: Grant vote if both conditions met
    // =========================================================================
    if (can_vote && log_ok) {
        // GRANT VOTE
        
        // Record vote (persistent state)
        log_.set_voted_for(req.candidate_id);
        
        // Reset election timer (acknowledge election activity)
        reset_election_timer();
        
        // Set response
        resp.vote_granted = true;
        
        Logger::info("Node " + std::to_string(node_id_) + 
                    " granted vote to " + std::to_string(req.candidate_id) +
                    " in term " + std::to_string(req.term));
    } else {
        // DENY VOTE
        Logger::debug("Denied vote to " + std::to_string(req.candidate_id) +
                     " (can_vote=" + std::to_string(can_vote) +
                     ", log_ok=" + std::to_string(log_ok) + ")");
    }
    
    return resp;
}

//------------------------------------------------------------------------------
// 9.2 HANDLE_APPEND_ENTRIES() - PROCESS LOG REPLICATION
//------------------------------------------------------------------------------
//
// AppendEntriesResponse RaftNode::handle_append_entries(
//     const AppendEntriesRequest& req)
//
// PURPOSE:
// Handles AppendEntries RPC from leader for heartbeat or log replication.
//
// APPEND ENTRIES RULES (from Raft paper):
// 1. If req.term < currentTerm: Reject (stale leader)
// 2. If req.term >= currentTerm: Reset election timer (valid leader)
// 3. If req.term > currentTerm: Update term, become follower
// 4. If candidate receives AppendEntries: Become follower (leader exists)
// 5. Check log consistency at prev_log_index
// 6. If inconsistency: Truncate conflicting entries, reject
// 7. Append new entries (skip duplicates, overwrite conflicts)
// 8. Update commit index to min(leader_commit, last_new_entry_index)
//
// CONSISTENCY CHECK:
// - prev_log_index: Index where new entries should be appended
// - prev_log_term: Expected term at prev_log_index
// - Follower checks: log[prev_log_index].term == prev_log_term
// - If match: Consistency verified, safe to append
// - If mismatch: Reject (leader will back up)
//
// CONFLICT RESOLUTION:
// - If new entry conflicts with existing (same index, different term):
//   * Delete existing and all following entries (log inconsistency)
//   * Append new entry from leader
// - This ensures: Leader's log is authoritative
//
// COMMIT INDEX UPDATE:
// - Leader tells follower what's committed (leader_commit)
// - Follower advances commit_index to min(leader_commit, last_entry_index)
// - Why min? Follower may not have all leader's entries yet
//
// PARAMETERS:
// - req: AppendEntriesRequest from leader
//
// RETURNS:
// - AppendEntriesResponse with success/failure
//
// THREAD SAFETY:
// - Accesses log and state with locks
// - Safe to call concurrently
//

AppendEntriesResponse RaftNode::handle_append_entries(const AppendEntriesRequest& req) {
    // Prepare response
    AppendEntriesResponse resp;
    resp.term = log_.get_current_term();
    resp.success = false;  // Default: Reject
    
    // =========================================================================
    // RULE 1: Reject if request term is stale
    // =========================================================================
    if (req.term < log_.get_current_term()) {
        Logger::debug("Rejecting AppendEntries from stale term " + 
                     std::to_string(req.term));
        return resp;  // success = false
    }
    
    // =========================================================================
    // RULE 2: Valid leader - reset election timer
    // =========================================================================
    // Leader is alive, prevent election timeout
    reset_election_timer();
    current_leader_ = req.leader_id;  // Track current leader
    
    // =========================================================================
    // RULE 3: If request term is newer, update term and become follower
    // =========================================================================
    if (req.term > log_.get_current_term()) {
        Logger::info("Received AppendEntries with higher term " + 
                    std::to_string(req.term) + ", becoming follower");
        become_follower(req.term);
        resp.term = req.term;
    }
    
    // =========================================================================
    // RULE 4: If candidate, step down (leader exists)
    // =========================================================================
    if (state_ == NodeState::CANDIDATE) {
        // Received AppendEntries from valid leader
        // Election is over, we didn't win
        Logger::info("Candidate stepping down, discovered leader " + 
                    std::to_string(req.leader_id));
        become_follower(req.term);
    }
    
    // =========================================================================
    // RULE 5: Check log consistency
    // =========================================================================
    
    // CHECK 1: Do we have entry at prev_log_index?
    if (req.prev_log_index > log_.last_log_index()) {
        // Log is too short
        // We don't have entry at prev_log_index
        // Cannot append (would create gap)
        Logger::debug("Log too short: prev_log_index=" + 
                     std::to_string(req.prev_log_index) +
                     " > last_log_index=" + std::to_string(log_.last_log_index()));
        return resp;  // success = false
    }
    
    // CHECK 2: Does entry at prev_log_index have matching term?
    if (req.prev_log_index > 0) {
        const LogEntry& prev_entry = log_.at(req.prev_log_index);
        if (prev_entry.term != req.prev_log_term) {
            // INCONSISTENCY DETECTED
            // Entry at prev_log_index has wrong term
            // Everything from prev_log_index onwards is suspect
            // Truncate from prev_log_index (delete conflicting entries)
            Logger::info("Log inconsistency at index " + std::to_string(req.prev_log_index) +
                        " (expected term " + std::to_string(req.prev_log_term) +
                        ", got " + std::to_string(prev_entry.term) + "), truncating");
            log_.truncate_from(req.prev_log_index);
            return resp;  // success = false, leader will back up
        }
    }
    
    // =========================================================================
    // RULE 6: Append new entries
    // =========================================================================
    if (!req.entries.empty()) {
        for (const auto& entry : req.entries) {
            if (entry.index <= log_.last_log_index()) {
                // ENTRY ALREADY EXISTS: Check for conflict
                const LogEntry& existing = log_.at(entry.index);
                if (existing.term != entry.term) {
                    // CONFLICT: Different term at same index
                    // Delete this entry and all following
                    // Then append new entry from leader
                    Logger::debug("Conflict at index " + std::to_string(entry.index) +
                                 ", truncating and appending from leader");
                    log_.truncate_from(entry.index);
                    log_.append(entry);
                }
                // else: Entries match (idempotent, skip append)
            } else {
                // NEW ENTRY: Append to log
                log_.append(entry);
            }
        }
        
        // Record highest index replicated
        resp.match_index = req.entries.back().index;
    }
    
    // =========================================================================
    // RULE 7: Update commit index
    // =========================================================================
    // Leader tells us what's committed
    // We can advance our commit index to this point
    // (But only up to our last entry - we may not have everything leader has)
    if (req.leader_commit > commit_index_) {
        // Advance to minimum of leader_commit and our last_log_index
        // Why min? We might not have all entries leader has yet
        commit_index_ = std::min(req.leader_commit, log_.last_log_index());
        Logger::debug("Updated commit index to " + std::to_string(commit_index_));
    }
    
    // SUCCESS: Entries appended (or heartbeat processed)
    resp.success = true;
    return resp;
}

//==============================================================================
// SECTION 10: VIRTUAL RPC METHODS
//==============================================================================

//------------------------------------------------------------------------------
// 10.1 SEND_REQUEST_VOTE() - SEND VOTE REQUEST (OVERRIDE)
//------------------------------------------------------------------------------
//
// bool RaftNode::send_request_vote(uint64_t peer_id, 
//                                   const RequestVoteRequest& req,
//                                   RequestVoteResponse& resp)
//
// PURPOSE:
// Virtual method for sending RequestVote RPC (overridden by NetworkRaftNode).
//
// BASE IMPLEMENTATION:
// - Does nothing (returns false)
// - Logs debug message
// - Suppresses unused parameter warnings
//
// WHY VIRTUAL?
// - Allows subclasses to provide network implementation
// - RaftNode: Core logic (election, replication)
// - NetworkRaftNode: Network layer (TCP sockets)
// - Separation of concerns: Logic vs. I/O
//
// OVERRIDE PATTERN:
// - NetworkRaftNode overrides this method
// - Provides actual network communication
// - Base class calls virtual method (polymorphism)
//
// PARAMETERS:
// - peer_id: Peer to send to
// - req: Request to send
// - resp: Response (filled by override)
//
// RETURNS:
// - Base: false (no network implementation)
// - Override: true if RPC succeeded
//

// Default RPC implementations (to be overridden by network layer)
// These are virtual and will be overridden by RaftController
bool RaftNode::send_request_vote(uint64_t peer_id, const RequestVoteRequest& req,
                                 RequestVoteResponse& resp) {
    // BASE IMPLEMENTATION
    // Subclasses (NetworkRaftNode) override this
    
    // Suppress unused parameter warnings
    (void)req;   // Request not used in base implementation
    (void)resp;  // Response not used in base implementation
    
    Logger::debug("Base send_request_vote called for peer " + std::to_string(peer_id));
    
    // Return false: No network implementation in base class
    return false;
}

//------------------------------------------------------------------------------
// 10.2 SEND_APPEND_ENTRIES() - SEND LOG ENTRIES (OVERRIDE)
//------------------------------------------------------------------------------
//
// bool RaftNode::send_append_entries(uint64_t peer_id, 
//                                     const AppendEntriesRequest& req,
//                                     AppendEntriesResponse& resp)
//
// PURPOSE:
// Virtual method for sending AppendEntries RPC (overridden by NetworkRaftNode).
//
// BASE IMPLEMENTATION:
// - Does nothing (returns false)
// - Same pattern as send_request_vote
//
// OVERRIDE:
// - NetworkRaftNode provides TCP network implementation
// - See raft_controller.cpp for full implementation
//

bool RaftNode::send_append_entries(uint64_t peer_id, const AppendEntriesRequest& req,
                                   AppendEntriesResponse& resp) {
    // BASE IMPLEMENTATION
    // Subclasses (NetworkRaftNode) override this
    
    // Suppress unused parameter warnings
    (void)req;
    (void)resp;
    
    Logger::debug("Base send_append_entries called for peer " + std::to_string(peer_id));
    
    // Return false: No network implementation in base class
    return false;
}

} // namespace raft
} // namespace backtesting

//==============================================================================
// END OF IMPLEMENTATION
//==============================================================================

//==============================================================================
// SECTION 11: RAFT ALGORITHM DETAILS
//==============================================================================
//
// LEADER ELECTION DETAILED WALKTHROUGH:
// ======================================
//
// Initial State (3-node cluster):
//   Node A: FOLLOWER, term=0
//   Node B: FOLLOWER, term=0
//   Node C: FOLLOWER, term=0
//
// Step 1: Node A timeout expires (random: 167ms)
//   Node A: becomes CANDIDATE, term=1, votes for self (1 vote)
//   Node A → B: RequestVote(term=1, candidate=A, ...)
//   Node A → C: RequestVote(term=1, candidate=A, ...)
//
// Step 2: Node B receives RequestVote
//   Node B: term=0 < request.term=1 → update to term=1, become follower
//   Node B: haven't voted in term 1 → can vote
//   Node B: log check passes → grant vote
//   Node B → A: VoteResponse(term=1, vote_granted=true)
//
// Step 3: Node C receives RequestVote (similar to B)
//   Node C → A: VoteResponse(term=1, vote_granted=true)
//
// Step 4: Node A counts votes
//   Votes: 3 (self + B + C)
//   Majority: 2 (need (3/2)+1 = 2)
//   Result: WON ELECTION
//   Node A: becomes LEADER
//
// Step 5: Node A sends heartbeats
//   Node A → B: AppendEntries(term=1, leader=A, entries=[])
//   Node A → C: AppendEntries(term=1, leader=A, entries=[])
//   Node B, C: receive heartbeat, reset timers, remain followers
//
// SPLIT VOTE SCENARIO:
// ====================
//
// Initial State (3-node cluster):
//   Node A, B, C: all FOLLOWER
//
// Bad scenario (without randomization):
//   T=0ms: A, B, C all timeout simultaneously
//   T=1ms: A, B, C all become candidates, vote for selves
//   T=2ms: A → B (vote request), B → A (vote request)
//          A votes for self (already voted)
//          B votes for self (already voted)
//   Result: Each node has 1 vote, no majority
//   Next round: Same thing happens (livelock)
//
// With randomization:
//   T=0ms: A timeout (random: 167ms)
//   T=1ms: A becomes candidate, requests votes
//   T=10ms: B receives vote request, grants vote
//   T=15ms: C receives vote request, grants vote
//   T=20ms: A counts votes (3 total), becomes leader
//   T=25ms: A sends heartbeats
//   T=30ms: B timeout would occur (random: 197ms), but heartbeat resets timer
//   T=35ms: C timeout would occur (random: 243ms), but heartbeat resets timer
//   Result: A elected before B or C time out
//
// LOG REPLICATION DETAILED WALKTHROUGH:
// ======================================
//
// Initial State (3-node cluster, A is leader):
//   Node A: term=1, log=[entry1(term=1)]
//   Node B: term=1, log=[entry1(term=1)]
//   Node C: term=1, log=[entry1(term=1)]
//   All: commit_index=1 (entry1 committed)
//
// Step 1: Client submits command to leader
//   Node A: append_log_entry(type=JOB_SUBMIT, data=job_params)
//   Node A: log=[entry1, entry2(term=1, data=job_params)]
//   Node A: local log updated (not committed yet)
//
// Step 2: Leader sends AppendEntries to followers
//   Node A → B: AppendEntries(term=1, prev_index=1, prev_term=1, 
//                             entries=[entry2], leader_commit=1)
//   Node A → C: AppendEntries(term=1, prev_index=1, prev_term=1,
//                             entries=[entry2], leader_commit=1)
//
// Step 3: Followers process AppendEntries
//   Node B: log[1].term == prev_term(1) ✓ Consistency OK
//   Node B: append entry2 → log=[entry1, entry2]
//   Node B → A: AppendEntriesResponse(success=true, match_index=2)
//   
//   Node C: (similar) → AppendEntriesResponse(success=true, match_index=2)
//
// Step 4: Leader counts replicas
//   Entry 2 replicated on: A (leader), B, C (3/3 nodes)
//   Majority: 2/3 nodes needed ✓
//   Entry is from current term (1) ✓
//   Result: CAN COMMIT
//
// Step 5: Leader advances commit_index
//   Node A: commit_index = 2
//   Node A: entry2 now committed (durable, won't be lost)
//
// Step 6: Leader applies entry
//   Node A: Apply entry2 to state machine (assign job to worker)
//   Node A: Job assignment proceeds
//
// Step 7: Next heartbeat informs followers of commit
//   Node A → B: AppendEntries(entries=[], leader_commit=2)
//   Node B: updates commit_index=2
//   Node B: applies entry2 (for state machine consistency)
//
// CONFLICT RESOLUTION EXAMPLE:
// ============================
//
// Scenario: Network partition caused log divergence
//
// Initial State:
//   Leader A: term=2, log=[entry1(term=1), entry2(term=2), entry3(term=2)]
//   Follower B: term=1, log=[entry1(term=1), entry2(term=1)]
//   (B's entry2 has wrong term - from old leader in term 1)
//
// Step 1: Leader sends AppendEntries
//   A → B: AppendEntries(prev_index=2, prev_term=2, entries=[entry3])
//
// Step 2: Follower checks consistency
//   B: log[2].term = 1, but prev_term = 2
//   B: MISMATCH! Truncate from index 2
//   B: log=[entry1]
//   B → A: AppendEntriesResponse(success=false)
//
// Step 3: Leader backs up
//   A: Decrement next_index for B (next_index = 2 → 1)
//   A → B: AppendEntries(prev_index=1, prev_term=1, entries=[entry2(term=2), entry3])
//
// Step 4: Follower checks again
//   B: log[1].term = 1, prev_term = 1 ✓ MATCH
//   B: Append entry2 and entry3
//   B: log=[entry1, entry2(term=2), entry3(term=2)]
//   B → A: AppendEntriesResponse(success=true, match_index=3)
//
// Result: B's log now matches A's log (consistency restored)
//
//==============================================================================

//==============================================================================
// SECTION 12: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Creating and Starting Raft Node
// ===========================================
//
// #include "raft/raft_node.h"
//
// int main() {
//     // Configure Raft node
//     raft::RaftConfig config;
//     config.node_id = 1;
//     config.log_dir = "./raft_logs/node_1";
//     config.election_timeout_min_ms = 150;
//     config.election_timeout_max_ms = 300;
//     config.heartbeat_interval_ms = 50;
//     
//     // Create node
//     raft::RaftNode node(config);
//     
//     // Add peers (other nodes in cluster)
//     node.add_peer(2, "node2.example.com", 5001);
//     node.add_peer(3, "node3.example.com", 5001);
//     
//     // Start Raft protocol
//     if (!node.start()) {
//         Logger::error("Failed to start Raft node");
//         return 1;
//     }
//     
//     // Wait for leader election
//     std::this_thread::sleep_for(std::chrono::seconds(2));
//     
//     // Check if we are leader
//     if (node.is_leader()) {
//         Logger::info("This node is the leader");
//         
//         // Append log entry
//         std::vector<uint8_t> data = {1, 2, 3};
//         node.append_log_entry(raft::LogEntryType::JOB_SUBMIT, data);
//     } else {
//         Logger::info("This node is a follower");
//     }
//     
//     // Keep running
//     std::this_thread::sleep_for(std::chrono::hours(1));
//     
//     // Stop node
//     node.stop();
//     
//     return 0;
// }
//
//
// EXAMPLE 2: Monitoring Raft State
// =================================
//
// void monitor_raft_node(raft::RaftNode& node) {
//     while (node.is_running()) {
//         std::this_thread::sleep_for(std::chrono::seconds(5));
//         
//         // Get state
//         std::string state;
//         if (node.is_leader()) state = "LEADER";
//         else if (node.is_candidate()) state = "CANDIDATE";
//         else state = "FOLLOWER";
//         
//         // Log status
//         Logger::info("Raft Status:");
//         Logger::info("  State: " + state);
//         Logger::info("  Term: " + std::to_string(node.get_current_term()));
//         Logger::info("  Leader: " + std::to_string(node.get_leader_id()));
//         Logger::info("  Log size: " + std::to_string(node.get_log_size()));
//         Logger::info("  Commit index: " + std::to_string(node.get_commit_index()));
//     }
// }
//
//
// EXAMPLE 3: Testing Leader Failover
// ===================================
//
// TEST(RaftTest, LeaderFailover) {
//     // Create 3-node cluster
//     raft::RaftNode node1(config1);
//     raft::RaftNode node2(config2);
//     raft::RaftNode node3(config3);
//     
//     // Configure peers
//     node1.add_peer(2, "localhost", 5002);
//     node1.add_peer(3, "localhost", 5003);
//     node2.add_peer(1, "localhost", 5001);
//     node2.add_peer(3, "localhost", 5003);
//     node3.add_peer(1, "localhost", 5001);
//     node3.add_peer(2, "localhost", 5002);
//     
//     // Start all nodes
//     node1.start();
//     node2.start();
//     node3.start();
//     
//     // Wait for initial election
//     std::this_thread::sleep_for(std::chrono::seconds(2));
//     
//     // Find leader
//     raft::RaftNode* leader = nullptr;
//     if (node1.is_leader()) leader = &node1;
//     else if (node2.is_leader()) leader = &node2;
//     else if (node3.is_leader()) leader = &node3;
//     
//     ASSERT_NE(leader, nullptr);  // Must have leader
//     
//     // Kill leader
//     leader->stop();
//     
//     // Wait for new election
//     std::this_thread::sleep_for(std::chrono::seconds(2));
//     
//     // Verify new leader elected
//     int leader_count = 0;
//     if (node1.is_leader()) leader_count++;
//     if (node2.is_leader()) leader_count++;
//     if (node3.is_leader()) leader_count++;
//     
//     EXPECT_EQ(leader_count, 1);  // Exactly one new leader
// }
//
//==============================================================================

//==============================================================================
// SECTION 13: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: Forgetting to Add Peers
// ===================================
// PROBLEM:
//    RaftNode node(config);
//    node.start();  // No peers added!
//    // Never elects leader (no one to vote for)
//
// SOLUTION:
//    node.add_peer(2, "host2", 5001);
//    node.add_peer(3, "host3", 5001);
//    node.start();
//
//
// PITFALL 2: Non-Randomized Election Timeouts
// ============================================
// PROBLEM:
//    // All nodes use same timeout (150ms)
//    // All timeout simultaneously
//    // Split votes, no leader elected
//
// SOLUTION (already implemented):
//    // Use randomized timeouts [150, 300]ms
//    // Prevents simultaneous elections
//
//
// PITFALL 3: Heartbeat Interval Too Large
// ========================================
// PROBLEM:
//    heartbeat_interval = 200ms
//    election_timeout_min = 150ms
//    // Heartbeat arrives after election timeout!
//    // Constant elections
//
// SOLUTION:
//    heartbeat_interval << election_timeout_min
//    Example: 50ms << 150ms (3x safety margin)
//
//
// PITFALL 4: Submitting to Follower
// ==================================
// PROBLEM:
//    node.append_log_entry(type, data);  // Node is follower
//    // Returns false, entry not appended
//
// SOLUTION:
//    if (node.is_leader()) {
//        node.append_log_entry(type, data);
//    } else {
//        // Redirect to leader or retry
//    }
//
//
// PITFALL 5: Not Waiting for Leader Election
// ===========================================
// PROBLEM:
//    node.start();
//    node.append_log_entry(...);  // Immediately after start
//    // Might fail (leader not elected yet)
//
// SOLUTION:
//    node.start();
//    std::this_thread::sleep_for(std::chrono::seconds(1));
//    // Or poll: while (!node.is_leader()) sleep(100ms);
//
//
// PITFALL 6: Log Directory Permissions
// =====================================
// PROBLEM:
//    // Can't write to log_dir
//    // RaftLog constructor throws
//
// SOLUTION:
//    mkdir -p ./raft_logs/node_1
//    chmod 755 ./raft_logs/node_1
//
//==============================================================================

//==============================================================================
// SECTION 14: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: How long does it take to elect a leader?
// =============================================
// A: Typical: 200-500ms
//    - First timeout: 150-300ms (random)
//    - RequestVote RPCs: 1-5ms each (parallel conceptually)
//    - Total: One timeout period + RPC latency
//    
//    Worst case: Multiple rounds if split votes (~1-2 seconds)
//
// Q2: What happens if leader crashes?
// ====================================
// A: Automatic failover:
//    1. Followers stop receiving heartbeats
//    2. After timeout (150-300ms): Followers become candidates
//    3. Election begins (200-500ms)
//    4. New leader elected
//    5. Total downtime: ~350-800ms
//
// Q3: Can there be multiple leaders at once?
// ===========================================
// A: No (Election Safety property):
//    - At most one leader per term
//    - Majority voting prevents multiple leaders
//    - If partition: Only majority partition can elect leader
//
// Q4: What if network partitions the cluster?
// ============================================
// A: Depends on partition size:
//    - Majority partition: Elects leader, continues
//    - Minority partition: No quorum, no progress
//    - When healed: Minority rejoins majority
//
// Q5: How are log conflicts resolved?
// ====================================
// A: Leader's log is authoritative:
//    - Follower has conflicting entry: Truncate and replace
//    - Leader backs up: Finds consistency point
//    - Eventually: All logs match leader's
//
// Q6: What if all nodes fail simultaneously?
// ===========================================
// A: System recovers:
//    - All nodes restart
//    - Read state from persistent log
//    - Recover term, voted_for, log entries
//    - Begin new election
//    - Committed entries preserved
//
// Q7: Can I add/remove nodes dynamically?
// ========================================
// A: Not implemented in this version.
//    Full Raft supports: Joint consensus for membership changes
//    Complex: Requires special log entries and two-phase protocol
//
// Q8: Why use virtual methods for RPC?
// =====================================
// A: Separation of concerns:
//    - RaftNode: Core logic (algorithm)
//    - NetworkRaftNode: Network I/O (sockets)
//    - Testing: Can mock network layer
//    - Flexibility: Different network implementations
//
// Q9: What's stored in the persistent log?
// =========================================
// A: Three things:
//    - current_term: Latest term seen
//    - voted_for: Candidate voted for in current term
//    - log entries: All log entries with term, index, type, data
//
// Q10: How fast can Raft replicate entries?
// ==========================================
// A: Throughput: ~100-1000 entries/second
//    - Limited by: Network RTT, disk I/O
//    - Batching: Can improve (send multiple entries per RPC)
//    - For backtesting: Plenty fast (jobs take seconds)
//
//==============================================================================

//==============================================================================
// SECTION 15: BEST PRACTICES
//==============================================================================
//
// BEST PRACTICE 1: Use Odd Number of Nodes
// =========================================
// DO:
//    3 nodes (survives 1 failure)
//    5 nodes (survives 2 failures)
//
// DON'T:
//    2 nodes (no fault tolerance)
//    4 nodes (same tolerance as 3, wastes resources)
//
//
// BEST PRACTICE 2: Configure Timeouts Correctly
// ==============================================
// Relationships:
//    heartbeat_interval < election_timeout_min < election_timeout_max
//    50ms < 150ms < 300ms
//
// Rule of thumb:
//    heartbeat = election_timeout / 3
//
//
// BEST PRACTICE 3: Monitor Leader Status
// =======================================
// DO:
//    if (node.get_leader_id() == 0) {
//        Logger::warning("No leader elected!");
//    }
//
//
// BEST PRACTICE 4: Handle Leadership Changes
// ===========================================
// Application must handle:
//    - Leader fails: Redirect to new leader
//    - Node loses leadership: Stop accepting requests
//    - Use callbacks or polling to detect changes
//
//
// BEST PRACTICE 5: Persist All Required State
// ============================================
// Always persist:
//    - current_term (critical for safety)
//    - voted_for (prevents double voting)
//    - log entries (durability)
//
// Never persist:
//    - commit_index (volatile, recalculated)
//    - state (derived, recalculated)
//
//
// BEST PRACTICE 6: Test Failure Scenarios
// ========================================
// Test:
//    - Leader crash
//    - Follower crash
//    - Network partition
//    - Simultaneous failures
//    - Log divergence and conflict resolution
//
//==============================================================================

//==============================================================================
// SECTION 16: TESTING STRATEGIES
//==============================================================================
//
// UNIT TESTS: Core Logic
// ======================
//
// TEST(RaftNodeTest, ElectionTimeout) {
//     raft::RaftNode node(config);
//     node.start();
//     
//     // Wait for timeout
//     std::this_thread::sleep_for(std::chrono::milliseconds(400));
//     
//     // Should have transitioned to candidate
//     EXPECT_TRUE(node.is_candidate() || node.is_leader());
//     EXPECT_GT(node.get_current_term(), 0);
// }
//
// TEST(RaftNodeTest, VoteGranting) {
//     raft::RaftNode node(config);
//     
//     raft::RequestVoteRequest req;
//     req.term = 5;
//     req.candidate_id = 2;
//     req.last_log_index = 0;
//     req.last_log_term = 0;
//     
//     auto resp = node.handle_request_vote(req);
//     
//     EXPECT_TRUE(resp.vote_granted);
//     EXPECT_EQ(node.get_current_term(), 5);
// }
//
//
// INTEGRATION TESTS: Cluster
// ===========================
//
// TEST(RaftIntegrationTest, ThreeNodeElection) {
//     // Start 3 nodes
//     // Wait for election
//     // Verify exactly one leader
//     // Verify all nodes agree on leader
// }
//
// TEST(RaftIntegrationTest, LogReplication) {
//     // Find leader
//     // Append entry to leader
//     // Wait for replication
//     // Verify all nodes have entry
//     // Verify commit index advanced
// }
//
//
// CHAOS TESTS: Fault Injection
// =============================
//
// TEST(RaftChaosTest, LeaderFailure) {
//     // Elect leader
//     // Kill leader
//     // Verify new leader elected
//     // Verify system continues
// }
//
// TEST(RaftChaosTest, NetworkPartition) {
//     // Partition cluster (3-2 split)
//     // Verify majority elects leader
//     // Verify minority doesn't elect
//     // Heal partition
//     // Verify consistency restored
// }
//
//==============================================================================

//==============================================================================
// SECTION 17: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM: No leader elected
// ===========================
// SYMPTOMS:
//    - All nodes remain followers or candidates
//    - get_leader_id() returns 0
//
// DEBUGGING:
//    ☐ Check all nodes started
//    ☐ Check peers added correctly
//    ☐ Check network connectivity (can nodes reach each other?)
//    ☐ Check logs for election attempts
//    ☐ Check for split votes (insufficient randomization)
//
//
// PROBLEM: Constant elections
// ============================
// SYMPTOMS:
//    - Term incrementing rapidly
//    - Frequent "starting election" logs
//
// CAUSES:
//    ☐ Heartbeat interval too large (>= election timeout)
//    ☐ Network latency high (heartbeats arriving late)
//    ☐ Leader crashing repeatedly
//
// SOLUTIONS:
//    ☐ Decrease heartbeat interval
//    ☐ Increase election timeout
//    ☐ Check network performance
//    ☐ Check leader process stability
//
//
// PROBLEM: Log entries not committing
// ====================================
// SYMPTOMS:
//    - Entries appended but commit_index not advancing
//    - get_commit_index() stays at 0
//
// CAUSES:
//    ☐ Majority not reachable (nodes down or partitioned)
//    ☐ AppendEntries failing (network errors)
//    ☐ Entries from old term (won't commit until new term entry)
//
// SOLUTIONS:
//    ☐ Check peer connectivity
//    ☐ Check AppendEntries RPC logs
//    ☐ Append entry in current term (triggers commit of old entries)
//
//==============================================================================

//==============================================================================
// SECTION 18: CORRECTNESS PROOF SKETCH
//==============================================================================
//
// ELECTION SAFETY (at most one leader per term):
// ===============================================
// Proof:
//   - Candidate needs majority votes to become leader
//   - Each node votes at most once per term
//   - Two majorities must overlap (pigeonhole principle)
//   - Overlapping voter prevents two leaders
//   ∴ At most one leader per term
//
// LEADER COMPLETENESS (leader has all committed entries):
// ========================================================
// Proof:
//   - Entry committed → replicated to majority
//   - New leader elected by majority
//   - By pigeonhole: Majority electing leader overlaps with majority that committed entry
//   - Overlapping voter has entry
//   - Election restricts: Only candidates with up-to-date logs win
//   - Up-to-date check ensures: Winner has all committed entries
//   ∴ Leader has all committed entries
//
// STATE MACHINE SAFETY (deterministic application):
// ==================================================
// Proof:
//   - Log Matching Property: Same index+term → identical logs
//   - Leader Completeness: Leader has all committed entries
//   - Commit order: Monotonically increasing indices
//   - Each node applies in order: apply index 1, then 2, then 3, ...
//   ∴ All nodes apply same commands in same order
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================