/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: raft_node.h
    
    Description:
        This header file defines the RaftNode class, which implements the core
        Raft consensus algorithm for distributed coordination. RaftNode manages
        the complete state machine of a Raft participant, including leader
        election, log replication, and failure detection.
        
        Core Responsibilities:
        - Implement the three Raft roles: Follower, Candidate, and Leader
        - Manage leader election through randomized timeouts and voting
        - Replicate log entries from leader to followers
        - Ensure log consistency across the cluster
        - Detect and recover from node failures automatically
        - Provide linearizable consistency for state machine operations
        
        Key Features:
        - Automatic leader election with randomized timeouts (150-300ms)
        - Heartbeat-based failure detection (50ms interval)
        - Log replication with consistency checking
        - Support for 3-node minimum cluster (fault tolerance)
        - Thread-safe operations with concurrent RPC handling
        - Crash recovery through persistent log storage
        
        Protocol Implementation:
        This implementation follows the Raft consensus algorithm as described
        in "In Search of an Understandable Consensus Algorithm" by Diego Ongaro
        and John Ousterhout (Stanford, 2014). Key invariants maintained:
        
        1. Election Safety: At most one leader per term
        2. Leader Append-Only: Leader never overwrites or deletes entries
        3. Log Matching: If two logs contain entry with same index and term,
           then the logs are identical in all preceding entries
        4. Leader Completeness: If entry committed in term, it will be present
           in logs of all leaders for higher-numbered terms
        5. State Machine Safety: If server applies log entry at given index,
           no other server will apply different entry for that index
        
        Architecture:
        RaftNode operates as an independent state machine with two background
        threads: election timer (monitors leader liveness) and heartbeat sender
        (leader maintains authority). The node responds to external RPCs
        (RequestVote, AppendEntries) synchronously while managing internal
        state transitions asynchronously.
        
        Integration:
        RaftNode is subclassed by NetworkRaftNode (raft_controller.h) which
        implements the virtual send_* methods with actual TCP networking.
        The controller layer uses RaftNode to coordinate job scheduling and
        worker management with strong consistency guarantees.
        
    Dependencies:
        - raft/raft_log.h: Persistent log storage and term management
        - common/logger.h: Logging infrastructure for debugging
        - C++17 threading and synchronization primitives
        - Standard library containers and algorithms
        
    Thread Safety:
        RaftNode is fully thread-safe. Multiple threads can:
        - Call RPC handlers concurrently (handle_request_vote, handle_append_entries)
        - Query state simultaneously (is_leader, get_current_term)
        - Append log entries (leader only)
        
        Internal synchronization uses multiple mutexes to minimize contention:
        - state_mutex_: Protects state transitions
        - peers_mutex_: Protects peer metadata (next_index, match_index)
        - timer_mutex_: Protects election timer state
        - RaftLog has its own internal mutex
        
    Performance Characteristics:
        - Leader election latency: 150-300ms (one timeout period)
        - Heartbeat overhead: ~50ms × cluster_size network messages/sec
        - Log replication latency: 2× RTT (one AppendEntries roundtrip)
        - Commit latency: ~100-200ms (heartbeat + majority ack)
        - Memory footprint: O(log_size + peer_count)
        
    Configuration Tuning:
        - election_timeout: 150-300ms (default) - Lower for faster failover,
          higher for more stability on slow networks
        - heartbeat_interval: 50ms (default) - Must be << election_timeout
          to prevent spurious elections. Rule of thumb: 1/3 of min election timeout
        - These values are tuned for LAN deployment (< 10ms RTT)
*******************************************************************************/

#ifndef RAFT_NODE_H
#define RAFT_NODE_H

#include "raft/raft_log.h"
#include "common/logger.h"
#include <string>
#include <vector>
#include <map>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <random>

namespace backtesting {
namespace raft {

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
// 1. Overview and Architecture
// 2. NodeState Enum - Raft Role Definitions
// 3. PeerInfo Structure - Peer Metadata
// 4. RPC Message Structures
//    4.1 RequestVote RPC
//    4.2 AppendEntries RPC
// 5. RaftConfig Structure - Node Configuration
// 6. RaftNode Class
//    6.1 Public Interface
//    6.2 State Queries
//    6.3 Log Operations
//    6.4 RPC Handlers
//    6.5 Virtual RPC Senders
//    6.6 Private Implementation
// 7. Usage Examples and Patterns
// 8. State Machine Transitions
// 9. Timing and Failure Scenarios
// 10. Common Pitfalls and Solutions
// 11. Tuning and Performance
// 12. FAQ
//==============================================================================

//==============================================================================
// SECTION 1: Overview and Architecture
//==============================================================================

/**
 * RAFT CONSENSUS ALGORITHM OVERVIEW
 * 
 * Raft is a consensus algorithm designed for managing a replicated log.
 * It produces results equivalent to Paxos but is easier to understand
 * and implement. Raft separates consensus into three concerns:
 * 
 * 1. LEADER ELECTION:
 *    - One server elected as leader
 *    - Leader manages log replication
 *    - If leader fails, new leader elected
 * 
 * 2. LOG REPLICATION:
 *    - Leader accepts commands from clients
 *    - Leader replicates commands to followers
 *    - Safe to apply command once replicated on majority
 * 
 * 3. SAFETY:
 *    - Ensures consistency even with failures
 *    - Only elect leaders with all committed entries
 *    - Committed entries never lost
 * 
 * RAFT NODE ROLES:
 * 
 * FOLLOWER:
 *   - Passive role, responds to RPCs from leader/candidates
 *   - If timeout without hearing from leader, becomes candidate
 *   - Most common state (nodes start as followers)
 * 
 * CANDIDATE:
 *   - Transitional state during election
 *   - Requests votes from peers
 *   - Becomes leader if wins majority
 *   - Returns to follower if discovers higher term
 * 
 * LEADER:
 *   - Accepts client requests
 *   - Appends to log and replicates to followers
 *   - Sends periodic heartbeats to maintain authority
 *   - At most one leader per term (safety property)
 * 
 * NORMAL OPERATION FLOW:
 * 
 * Time:  T0          T1              T2              T3              T4
 *        ├───────────┼───────────────┼───────────────┼───────────────┤
 * Node1: FOLLOWER────┼──LEADER───────┼──LEADER───────┼──LEADER───────┤
 *        (elect) ────┘   │           │               │               │
 *                        │ heartbeat │ heartbeat     │ log append    │
 * Node2: FOLLOWER────────┼──FOLLOWER─┼──FOLLOWER─────┼──FOLLOWER─────┤
 *                        │           │               │               │
 * Node3: FOLLOWER────────┼──FOLLOWER─┼──FOLLOWER─────┼──FOLLOWER─────┤
 * 
 * FAILURE RECOVERY FLOW:
 * 
 * Time:  T0          T1              T2              T3              T4
 *        ├───────────┼───────────────┼───────────────┼───────────────┤
 * Node1: LEADER──────┼──CRASH!       │               │               │
 *                    │               │               │               │
 * Node2: FOLLOWER────┼──timeout──────┼──CANDIDATE────┼──LEADER───────┤
 *        (election)  │               │ (wins vote)   │ (new leader)  │
 *                    │               │               │               │
 * Node3: FOLLOWER────┼──timeout──────┼──votes for 2──┼──FOLLOWER─────┤
 * 
 * DESIGN RATIONALE:
 * 
 * WHY RANDOMIZED TIMEOUTS?
 * - Prevents split votes (all nodes timing out simultaneously)
 * - Ensures one node typically wins election quickly
 * - Range: 150-300ms (2x spread prevents collisions)
 * 
 * WHY HEARTBEATS?
 * - Maintains leader's authority (prevents spurious elections)
 * - Carries log entries (efficiency - no separate replication)
 * - Frequency: 50ms (much faster than election timeout)
 * 
 * WHY TERMS?
 * - Logical clock for detecting stale information
 * - Prevents multiple leaders in same term
 * - Each term has at most one leader
 * 
 * WHY MAJORITY QUORUM?
 * - Ensures at least one server has committed entries
 * - Prevents split-brain (two groups operating independently)
 * - Example: 5 nodes need 3 for majority (can tolerate 2 failures)
 */

//==============================================================================
// SECTION 2: NodeState Enum - Raft Role Definitions
//==============================================================================

/**
 * @enum NodeState
 * @brief Represents the current role of this Raft node
 * 
 * RAFT THREE STATES:
 * 
 * Every Raft node is in exactly one of these states at any time.
 * State transitions occur based on:
 * - Timeouts (FOLLOWER → CANDIDATE)
 * - Election results (CANDIDATE → LEADER or CANDIDATE → FOLLOWER)
 * - Discovering higher term (any state → FOLLOWER)
 * 
 * STATE TRANSITION DIAGRAM:
 * 
 *                    discovers current leader
 *                    or new term
 *         ┌─────────────────────────────────┐
 *         │                                 │
 *         ▼                                 │
 *    FOLLOWER ──timeout──→ CANDIDATE ──wins election──→ LEADER
 *         ▲                    │                            │
 *         │                    │                            │
 *         └────────────────────┴────────────────────────────┘
 *              discovers server with higher term
 * 
 * INVARIANTS:
 * 1. At most one node in LEADER state per term
 * 2. CANDIDATE state is transient (quickly becomes LEADER or FOLLOWER)
 * 3. Most nodes spend most time in FOLLOWER state
 * 4. State changes are always logged for debugging
 */
enum class NodeState {
    /**
     * FOLLOWER: Passive state, responds to RPCs
     * 
     * BEHAVIORS:
     * - Respond to AppendEntries from leader
     * - Grant votes in elections
     * - If no heartbeat for timeout period, become CANDIDATE
     * 
     * RESPONSIBILITIES:
     * - Validate and append leader's log entries
     * - Update commit_index based on leader's commit
     * - Reset election timer on each leader heartbeat
     * 
     * TYPICAL DURATION:
     * - Indefinite (until leader fails or this node times out)
     * 
     * ENTRY CONDITIONS:
     * - Node startup (initial state)
     * - Lost election (became candidate but didn't win)
     * - Discovered higher term (was leader/candidate, found newer leader)
     * 
     * EXIT CONDITIONS:
     * - Election timeout expires without hearing from leader
     */
    FOLLOWER,
    
    /**
     * CANDIDATE: Requesting votes to become leader
     * 
     * BEHAVIORS:
     * - Increment term
     * - Vote for self
     * - Request votes from all peers
     * - If win majority, become LEADER
     * - If discover leader/higher term, become FOLLOWER
     * - If timeout with no winner, start new election
     * 
     * RESPONSIBILITIES:
     * - Send RequestVote RPCs to all peers
     * - Count votes received
     * - Detect split votes and retry
     * 
     * TYPICAL DURATION:
     * - Very short (< 100ms) - one election round
     * - May repeat if split votes occur
     * 
     * ENTRY CONDITIONS:
     * - Follower timeout expires (no leader heartbeat)
     * 
     * EXIT CONDITIONS:
     * - Win election (receive majority votes)
     * - Lose election (discover current leader)
     * - Split vote (timeout and retry)
     * - Discover higher term
     */
    CANDIDATE,
    
    /**
     * LEADER: Coordinating cluster, accepting client requests
     * 
     * BEHAVIORS:
     * - Accept client commands
     * - Append commands to local log
     * - Replicate log entries to followers
     * - Send periodic heartbeats (empty AppendEntries)
     * - Advance commit_index once majority replicated
     * - Apply committed entries to state machine
     * 
     * RESPONSIBILITIES:
     * - Maintain authority via heartbeats (every 50ms)
     * - Track each follower's replication progress
     * - Detect and repair follower log inconsistencies
     * - Never overwrite or delete own log entries
     * 
     * TYPICAL DURATION:
     * - Long (minutes to hours) - until failure or network partition
     * 
     * ENTRY CONDITIONS:
     * - Win election (receive majority votes)
     * 
     * EXIT CONDITIONS:
     * - Discover higher term (another node is leader)
     * - Crash/failure (other nodes detect via timeout)
     */
    LEADER
};

//==============================================================================
// SECTION 3: PeerInfo Structure - Peer Metadata
//==============================================================================

/**
 * @struct PeerInfo
 * @brief Tracks replication state for each peer node
 * 
 * LEADER PERSPECTIVE:
 * Leader maintains a PeerInfo for each follower to track replication progress.
 * This enables the leader to:
 * - Determine which entries to send to each follower
 * - Detect when follower is caught up or lagging
 * - Advance commit_index based on majority replication
 * 
 * MATCHMAKER ALGORITHM:
 * Leader uses next_index and match_index to implement efficient log repair:
 * 
 * 1. Initially set next_index = leader's last log index + 1
 *    (optimistically assume follower is caught up)
 * 
 * 2. If AppendEntries fails (log inconsistency):
 *    - Decrement next_index
 *    - Retry with earlier entries
 *    - Continue until finding matching point
 * 
 * 3. Once AppendEntries succeeds:
 *    - Update match_index to highest replicated entry
 *    - Advance next_index to match_index + 1
 *    - Send subsequent entries
 * 
 * EXAMPLE REPLICATION SCENARIO:
 * 
 * Leader's log:   [1,1] [2,1] [3,2] [4,2] [5,3] [6,3]
 * Follower's log: [1,1] [2,1] [3,1] [4,1]
 *                                   ↑ conflict at index 3
 * 
 * Step 1: Leader sends entries starting from next_index=5
 *         prevLogIndex=4, prevLogTerm=2
 *         Follower checks entry[4]: term is 1, not 2 → FAIL
 * 
 * Step 2: Leader decrements next_index to 4, retries
 *         prevLogIndex=3, prevLogTerm=2
 *         Follower checks entry[3]: term is 1, not 2 → FAIL
 * 
 * Step 3: Leader decrements next_index to 3, retries
 *         prevLogIndex=2, prevLogTerm=1
 *         Follower checks entry[2]: term is 1, matches! → SUCCESS
 *         Follower truncates from index 3, appends leader's entries
 * 
 * Result: Follower's log now matches leader's log
 * 
 * OPTIMIZATION:
 * Instead of decrementing next_index by 1 each time, follower can return
 * the index of its last entry in conflicting term for faster convergence.
 * Current implementation uses simple decrement (correct but slower).
 */
struct PeerInfo {
    /**
     * @brief Unique identifier for this peer node
     * 
     * Corresponds to RaftConfig::node_id for this peer.
     * Used as key in peers_ map and for sending RPCs.
     */
    uint64_t node_id;
    
    /**
     * @brief Network address of peer node
     * 
     * DNS hostname or IP address (e.g., "node2.cluster.local" or "192.168.1.10")
     * Used by network layer to establish connections.
     */
    std::string hostname;
    
    /**
     * @brief TCP port where peer listens for Raft RPCs
     * 
     * Typically 6000 for Raft traffic (separate from worker port 5000).
     */
    uint16_t port;
    
    /**
     * @brief Next log entry to send to this peer (leader only)
     * 
     * SEMANTICS:
     * - next_index represents the index of the next log entry the leader
     *   will send to this follower
     * - Initialized to leader's last_log_index + 1 when becoming leader
     * - Decremented on AppendEntries failure (log mismatch)
     * - Incremented on AppendEntries success
     * 
     * INVARIANT:
     * next_index > match_index (always sending entries after last match)
     * 
     * USAGE:
     * ```cpp
     * // Leader preparing AppendEntries for follower
     * std::vector<LogEntry> entries_to_send = 
     *     log_.get_entries_from(peer.next_index);
     * ```
     */
    uint64_t next_index;
    
    /**
     * @brief Highest log entry known to be replicated on this peer (leader only)
     * 
     * SEMANTICS:
     * - match_index represents the highest index that definitely exists
     *   on this follower (confirmed via successful AppendEntries)
     * - Initialized to 0 when becoming leader
     * - Updated to highest index in successful AppendEntries response
     * - Used to advance commit_index (majority replication check)
     * 
     * COMMITMENT DECISION:
     * Leader can commit entry at index N if:
     *   majority of peers have match_index >= N
     *   AND entry at N was created in current term
     * 
     * EXAMPLE:
     * 3-node cluster, leader's log has entries up to index 10
     * - Peer1: match_index = 10 (fully caught up)
     * - Peer2: match_index = 8 (lagging by 2 entries)
     * 
     * Majority (2 of 3) have replicated up to index 8, so leader can
     * commit up to index 8 (even though peer2 is behind).
     */
    uint64_t match_index;
    
    /**
     * @brief Default constructor initializing to safe defaults
     * 
     * RATIONALE FOR DEFAULTS:
     * - node_id=0: Invalid, must be set explicitly
     * - port=0: Invalid, must be set explicitly
     * - next_index=1: Start sending from first real entry
     * - match_index=0: Nothing replicated yet
     */
    PeerInfo() : node_id(0), port(0), next_index(1), match_index(0) {}
};

//==============================================================================
// SECTION 4: RPC Message Structures
//==============================================================================

//------------------------------------------------------------------------------
// SECTION 4.1: RequestVote RPC
//------------------------------------------------------------------------------

/**
 * @struct RequestVoteRequest
 * @brief Candidate's request for votes during election
 * 
 * PROTOCOL:
 * Candidate sends this RPC to all peers when starting election.
 * Peer evaluates request and decides whether to grant vote.
 * 
 * VOTING RESTRICTIONS (Raft safety):
 * Voter grants vote to candidate ONLY if ALL conditions met:
 * 
 * 1. TERM CHECK:
 *    - candidate.term >= voter.term
 *    - If candidate.term < voter.term, reject (stale candidate)
 *    - If candidate.term > voter.term, update voter's term first
 * 
 * 2. VOTE AVAILABILITY:
 *    - Voter hasn't voted yet in this term, OR
 *    - Voter already voted for this candidate (retry/duplicate)
 * 
 * 3. LOG UP-TO-DATE CHECK:
 *    - Candidate's log is at least as up-to-date as voter's log
 *    - "Up-to-date" defined by: higher term wins, same term longer wins
 *    - Prevents electing leader missing committed entries
 * 
 * EXAMPLE EVALUATION:
 * ```cpp
 * bool should_grant_vote(const RequestVoteRequest& req) {
 *     // Check 1: Term
 *     if (req.term < log_.get_current_term()) {
 *         return false;  // Stale candidate
 *     }
 *     
 *     // Check 2: Vote availability
 *     uint64_t voted_for = log_.get_voted_for();
 *     if (voted_for != 0 && voted_for != req.candidate_id) {
 *         return false;  // Already voted for someone else
 *     }
 *     
 *     // Check 3: Log up-to-date
 *     if (req.last_log_term > log_.last_log_term()) {
 *         return true;  // Candidate has higher term
 *     }
 *     if (req.last_log_term == log_.last_log_term() &&
 *         req.last_log_index >= log_.last_log_index()) {
 *         return true;  // Same term, candidate's log is longer
 *     }
 *     
 *     return false;  // Candidate's log is outdated
 * }
 * ```
 */
struct RequestVoteRequest {
    /**
     * @brief Candidate's current term
     * 
     * Used to detect stale candidates and update voter's term if needed.
     */
    uint64_t term;
    
    /**
     * @brief Node ID of candidate requesting vote
     * 
     * Allows voter to record who they voted for (one vote per term).
     */
    uint64_t candidate_id;
    
    /**
     * @brief Index of candidate's last log entry
     * 
     * Used with last_log_term to determine if candidate's log is
     * at least as up-to-date as voter's log.
     */
    uint64_t last_log_index;
    
    /**
     * @brief Term of candidate's last log entry
     * 
     * Primary criterion for log up-to-dateness (higher is better).
     */
    uint64_t last_log_term;
};

/**
 * @struct RequestVoteResponse
 * @brief Voter's response to RequestVote RPC
 * 
 * SIMPLE STRUCTURE:
 * Only two pieces of information needed:
 * - Did we grant the vote?
 * - What's our current term? (for candidate to update itself)
 */
struct RequestVoteResponse {
    /**
     * @brief Voter's current term
     * 
     * If higher than candidate's term, candidate must revert to follower.
     * This can happen if voter already moved to a later term.
     */
    uint64_t term;
    
    /**
     * @brief Whether vote was granted
     * 
     * true: Vote granted, candidate can count this toward majority
     * false: Vote denied (already voted, term mismatch, or log outdated)
     */
    bool vote_granted;
};

//------------------------------------------------------------------------------
// SECTION 4.2: AppendEntries RPC
//------------------------------------------------------------------------------

/**
 * @struct AppendEntriesRequest
 * @brief Leader's log replication and heartbeat message
 * 
 * DUAL PURPOSE:
 * 
 * 1. HEARTBEAT (entries empty):
 *    - Leader sends every heartbeat_interval_ms (50ms)
 *    - Prevents followers from timing out and starting elections
 *    - Maintains leader's authority
 *    - Low overhead (small message, no log writes)
 * 
 * 2. LOG REPLICATION (entries non-empty):
 *    - Sends new log entries to follower
 *    - Follower validates consistency and appends
 *    - Once majority appends, leader can commit
 * 
 * CONSISTENCY CHECKING:
 * 
 * The prev_log_index and prev_log_term fields enable consistency checking:
 * 
 * SCENARIO: Leader sending entries [5, 6, 7] to follower
 * 
 * Leader sets:
 *   prev_log_index = 4 (entry before first new entry)
 *   prev_log_term = 2 (term of entry 4)
 * 
 * Follower checks:
 *   Does my entry at index 4 have term 2?
 *   YES → Append entries [5, 6, 7]
 *   NO → Reject (log inconsistency, leader will backtrack)
 * 
 * This ensures follower's log is consistent with leader's log
 * at the point where new entries are appended.
 * 
 * SAFETY PROPERTY:
 * If AppendEntries succeeds, follower's log is guaranteed to match
 * leader's log up to and including all entries in this RPC.
 */
struct AppendEntriesRequest {
    /**
     * @brief Leader's current term
     * 
     * Follower updates its term if this is higher.
     * Rejects RPC if this is lower (stale leader).
     */
    uint64_t term;
    
    /**
     * @brief Node ID of current leader
     * 
     * Allows follower to know who the leader is (for redirecting clients).
     */
    uint64_t leader_id;
    
    /**
     * @brief Index of log entry immediately preceding new entries
     * 
     * Used for consistency check. Follower verifies it has an entry
     * at this index with the specified term.
     * 
     * Special case: 0 means no previous entry (sending from beginning)
     */
    uint64_t prev_log_index;
    
    /**
     * @brief Term of entry at prev_log_index
     * 
     * Used for consistency check. Follower compares this with its
     * entry at prev_log_index.
     */
    uint64_t prev_log_term;
    
    /**
     * @brief Log entries to replicate (empty for heartbeat)
     * 
     * HEARTBEAT: entries.empty() == true
     * - Maintains leader authority
     * - Updates follower's commit_index
     * - No disk write required
     * 
     * REPLICATION: entries.empty() == false
     * - Contains one or more entries to append
     * - Follower writes to disk before acknowledging
     * - May contain entries follower already has (leader doesn't know)
     */
    std::vector<LogEntry> entries;
    
    /**
     * @brief Leader's commit index
     * 
     * Tells follower which entries are committed (safe to apply to state machine).
     * Follower updates its own commit_index to min(leader_commit, last_log_index).
     * 
     * SAFETY:
     * Leader only increments commit_index after majority replication,
     * so follower can safely apply these entries.
     */
    uint64_t leader_commit;
};

/**
 * @struct AppendEntriesResponse
 * @brief Follower's response to AppendEntries RPC
 * 
 * FEEDBACK TO LEADER:
 * Response tells leader:
 * - Was the append successful?
 * - What's follower's current term?
 * - What's follower's latest replicated index?
 * 
 * Leader uses this information to:
 * - Update follower's match_index and next_index
 * - Advance commit_index if majority replicated
 * - Backtrack and repair log inconsistencies
 */
struct AppendEntriesResponse {
    /**
     * @brief Follower's current term
     * 
     * If higher than leader's term, leader must step down to follower.
     */
    uint64_t term;
    
    /**
     * @brief Whether log append succeeded
     * 
     * true: Entries appended successfully, follower's log matches leader
     * false: Consistency check failed, leader should backtrack
     * 
     * FAILURE REASONS:
     * - term < follower's term (stale leader)
     * - prev_log_index > follower's log length (follower missing entries)
     * - prev_log_index exists but term doesn't match (conflicting entries)
     */
    bool success;
    
    /**
     * @brief Highest log index follower has replicated
     * 
     * On success: Index of last entry in successful append
     * On failure: Typically 0 or follower's last_log_index
     * 
     * Leader uses this to update peer.match_index.
     */
    uint64_t match_index;
};

//==============================================================================
// SECTION 5: RaftConfig Structure - Node Configuration
//==============================================================================

/**
 * @struct RaftConfig
 * @brief Configuration parameters for Raft node initialization
 * 
 * PARAMETER TUNING GUIDE:
 * 
 * ELECTION TIMEOUTS (election_timeout_min_ms, election_timeout_max_ms):
 * - Controls how quickly cluster detects leader failure
 * - Must be >> heartbeat_interval to avoid spurious elections
 * - Range should be 2x to prevent simultaneous timeouts (split votes)
 * 
 * RECOMMENDED VALUES:
 * - LAN (< 10ms RTT): 150-300ms
 * - WAN (50-100ms RTT): 500-1000ms
 * - Unstable network: 1000-2000ms
 * 
 * HEARTBEAT INTERVAL (heartbeat_interval_ms):
 * - How often leader sends heartbeats
 * - Must be << election_timeout_min to prevent false leader failures
 * - Rule of thumb: 1/3 to 1/5 of min election timeout
 * 
 * RECOMMENDED VALUES:
 * - LAN: 50ms (default)
 * - WAN: 150-200ms
 * 
 * TRADEOFFS:
 * - Shorter timeouts: Faster failover, but more sensitive to transient issues
 * - Longer timeouts: More stable, but slower failover
 * - More frequent heartbeats: Better failure detection, higher network overhead
 * - Less frequent heartbeats: Lower overhead, risk of spurious elections
 */
struct RaftConfig {
    /**
     * @brief Unique identifier for this node in the cluster
     * 
     * REQUIREMENTS:
     * - Must be unique across all nodes
     * - Must be > 0 (0 is reserved for "no leader")
     * - Typically sequential: 1, 2, 3 for 3-node cluster
     * 
     * EXAMPLE:
     * Node on server kh01: node_id = 1
     * Node on server kh02: node_id = 2
     * Node on server kh03: node_id = 3
     */
    uint64_t node_id;
    
    /**
     * @brief Directory for persistent Raft log storage
     * 
     * REQUIREMENTS:
     * - Must be writable by process
     * - Should be on durable storage (local SSD, not tmpfs)
     * - Separate directory per node (don't share!)
     * 
     * EXAMPLE:
     * Node 1: "/var/raft/node1"
     * Node 2: "/var/raft/node2"
     */
    std::string log_dir;
    
    /**
     * @brief Minimum election timeout in milliseconds
     * 
     * MEANING:
     * If follower doesn't receive heartbeat for this duration,
     * it may start an election (actual timeout is randomized
     * between min and max).
     * 
     * DEFAULT: 150ms (suitable for LAN)
     * 
     * EFFECTS OF CHANGING:
     * - Lower (100ms): Faster failover, more sensitive to delays
     * - Higher (500ms): More stable, slower failover
     */
    int election_timeout_min_ms;
    
    /**
     * @brief Maximum election timeout in milliseconds
     * 
     * MEANING:
     * Upper bound for randomized election timeout.
     * Spread between min and max prevents simultaneous elections.
     * 
     * DEFAULT: 300ms (2× min, prevents split votes)
     * 
     * RULE: max should be 2-3× min for good randomization
     */
    int election_timeout_max_ms;
    
    /**
     * @brief Interval between leader heartbeats in milliseconds
     * 
     * MEANING:
     * How often leader sends AppendEntries (heartbeat or replication).
     * 
     * DEFAULT: 50ms (well below min election timeout)
     * 
     * RULE: Should be < min_election_timeout / 3
     * Ensures follower receives multiple heartbeats before timing out.
     * 
     * EXAMPLE TIMING:
     * heartbeat_interval = 50ms
     * election_timeout_min = 150ms
     * Follower receives ~3 heartbeats before timeout (safe margin)
     */
    int heartbeat_interval_ms;
    
    /**
     * @brief Default constructor with production-ready values
     * 
     * VALUES CHOSEN FOR:
     * - LAN deployment (< 10ms RTT)
     * - Fast failover (< 300ms leader election)
     * - Stability (2× timeout spread)
     * - Low overhead (~20 heartbeats/sec)
     */
    RaftConfig()
        : node_id(0), 
          log_dir("./raft_data"),
          election_timeout_min_ms(150),
          election_timeout_max_ms(300),
          heartbeat_interval_ms(50) {}
};

//==============================================================================
// SECTION 6: RaftNode Class
//==============================================================================

/**
 * @class RaftNode
 * @brief Core implementation of Raft consensus algorithm
 * 
 * ARCHITECTURE OVERVIEW:
 * 
 * RaftNode manages three concurrent activities:
 * 
 * 1. STATE MACHINE:
 *    - Transitions between FOLLOWER, CANDIDATE, LEADER
 *    - Responds to RPC requests synchronously
 *    - Protected by state_mutex_
 * 
 * 2. ELECTION TIMER:
 *    - Background thread (election_thread_)
 *    - Monitors leader liveness
 *    - Triggers elections on timeout
 * 
 * 3. HEARTBEAT SENDER:
 *    - Background thread (heartbeat_thread_)
 *    - Active only when LEADER
 *    - Sends periodic AppendEntries to maintain authority
 * 
 * THREAD INTERACTION:
 * 
 *   RPC Threads          Election Thread      Heartbeat Thread
 *   (external)           (internal)           (internal)
 *        │                     │                     │
 *        │                     │                     │
 *        ├─handle_request_vote─┤                     │
 *        │  (synchronized)     │                     │
 *        │                     │                     │
 *        │                     ├─check timeout───────┤
 *        │                     │                     │
 *        ├─handle_append_entries──────────────────────┤
 *        │  (synchronized)     │                     │
 *        │                     │                     │
 *        │                     │                     ├─send heartbeats
 *        │                     │                     │
 *        │                     │                     │
 *        └─────────────────────┴─────────────────────┘
 *                    All synchronized via mutexes
 * 
 * INVARIANTS MAINTAINED:
 * 
 * 1. STATE CONSISTENCY:
 *    - Node is always in exactly one state (FOLLOWER/CANDIDATE/LEADER)
 *    - State transitions are atomic
 *    - Only one leader per term
 * 
 * 2. LOG CONSISTENCY:
 *    - Leader never deletes or modifies existing entries
 *    - Followers' logs match leader's log at all common indices
 *    - Committed entries never lost
 * 
 * 3. TERM MONOTONICITY:
 *    - current_term never decreases
 *    - If message.term > current_term, update current_term
 * 
 * 4. VOTE RESTRICTION:
 *    - At most one vote per term
 *    - Vote persisted before responding
 * 
 * LIFECYCLE:
 * 
 * 1. Construction:
 *    - Initialize state (FOLLOWER, term 0)
 *    - Load persistent log
 *    - Configure timeouts
 * 
 * 2. Add Peers:
 *    - Call add_peer() for each other node
 *    - Must be done before start()
 * 
 * 3. Start:
 *    - Launch election timer thread
 *    - Launch heartbeat thread
 *    - Begin participating in cluster
 * 
 * 4. Operation:
 *    - Handle RPCs from peers
 *    - Append log entries (if leader)
 *    - State transitions as needed
 * 
 * 5. Stop:
 *    - Signal threads to terminate
 *    - Wait for clean shutdown
 *    - Flush log to disk
 * 
 * USAGE EXAMPLE:
 * ```cpp
 * // Create and configure node
 * RaftConfig config;
 * config.node_id = 1;
 * config.log_dir = "/var/raft/node1";
 * RaftNode node(config);
 * 
 * // Add peer nodes
 * node.add_peer(2, "node2.cluster", 6000);
 * node.add_peer(3, "node3.cluster", 6000);
 * 
 * // Start participating in cluster
 * if (!node.start()) {
 *     std::cerr << "Failed to start node\n";
 *     return 1;
 * }
 * 
 * // Wait for leader election
 * std::this_thread::sleep_for(std::chrono::seconds(1));
 * 
 * // Submit entries if leader
 * if (node.is_leader()) {
 *     std::vector<uint8_t> data = serialize_command(cmd);
 *     node.append_log_entry(LogEntryType::JOB_SUBMIT, data);
 * }
 * 
 * // Graceful shutdown
 * node.stop();
 * ```
 */
class RaftNode {
private:
    //==========================================================================
    // PRIVATE MEMBER VARIABLES
    //==========================================================================
    
    /**
     * CONFIGURATION AND IDENTITY
     */
    
    /**
     * @brief Configuration parameters (timeouts, directories, etc.)
     * 
     * Immutable after construction. Copied from constructor parameter.
     */
    RaftConfig config_;
    
    /**
     * @brief This node's unique identifier
     * 
     * Cached from config_.node_id for quick access.
     * Used in RPCs and log entries.
     */
    uint64_t node_id_;
    
    /**
     * RAFT STATE (PERSISTENT AND VOLATILE)
     */
    
    /**
     * @brief Current role in cluster (FOLLOWER/CANDIDATE/LEADER)
     * 
     * THREAD SAFETY: std::atomic for lock-free reads
     * Most frequent operation is checking "am I leader?"
     * Atomic allows this without mutex overhead.
     * 
     * MODIFICATIONS: Always with state_mutex_ held for complex transitions
     */
    std::atomic<NodeState> state_;
    
    /**
     * @brief Persistent log storage (term, vote, entries)
     * 
     * THREAD SAFETY: RaftLog has internal mutex
     * Multiple threads can safely access log concurrently.
     * 
     * DURABILITY: All mutations persisted to disk immediately
     */
    RaftLog log_;
    
    /**
     * @brief Highest log entry known to be committed
     * 
     * VOLATILE STATE (not persisted, rebuilt on startup)
     * 
     * INVARIANT: commit_index <= last_log_index
     * 
     * LEADER PERSPECTIVE:
     * - Advances when majority of peers replicate entry
     * - Only advances for entries from current term (safety)
     * 
     * FOLLOWER PERSPECTIVE:
     * - Updated from leader's AppendEntries.leader_commit
     * - Never exceeds own last_log_index
     * 
     * CONCURRENCY: Protected by state_mutex_ for updates
     */
    uint64_t commit_index_;
    
    /**
     * @brief Highest log entry applied to state machine
     * 
     * VOLATILE STATE (not persisted)
     * 
     * INVARIANT: last_applied <= commit_index
     * 
     * USAGE:
     * Background thread applies entries from last_applied+1 to commit_index.
     * (Not implemented in this header, would be in derived class)
     * 
     * EXAMPLE:
     * commit_index = 10, last_applied = 7
     * → Need to apply entries 8, 9, 10 to state machine
     */
    uint64_t last_applied_;
    
    /**
     * PEER MANAGEMENT
     */
    
    /**
     * @brief Map of peer node ID to replication metadata
     * 
     * LEADER USE:
     * Tracks each follower's replication progress (next_index, match_index).
     * 
     * FOLLOWER/CANDIDATE USE:
     * Stores peer contact information for RPCs.
     * 
     * THREAD SAFETY: Protected by peers_mutex_
     * Separate from state_mutex_ to reduce contention.
     * 
     * EXAMPLE:
     * 3-node cluster (this node + 2 peers):
     * peers_[2] = {node_id=2, hostname="node2", next_index=10, match_index=9}
     * peers_[3] = {node_id=3, hostname="node3", next_index=10, match_index=8}
     */
    std::map<uint64_t, PeerInfo> peers_;
    
    /**
     * @brief Protects peers_ map
     * 
     * GRANULARITY: Separate mutex for peer metadata
     * Allows state transitions without blocking peer updates.
     */
    std::mutex peers_mutex_;
    
    /**
     * LEADER TRACKING
     */
    
    /**
     * @brief Node ID of current leader (0 if unknown)
     * 
     * THREAD SAFETY: std::atomic for lock-free reads
     * Frequently queried by clients for request forwarding.
     * 
     * VALUES:
     * - 0: No leader known (election in progress)
     * - Non-zero: Current leader's node_id
     * 
     * UPDATES:
     * - Set when receiving AppendEntries from leader
     * - Set to self when becoming leader
     * - Reset to 0 when starting election
     */
    std::atomic<uint64_t> current_leader_;
    
    /**
     * THREADING AND SYNCHRONIZATION
     */
    
    /**
     * @brief Flag to stop background threads
     * 
     * THREAD SAFETY: std::atomic for visibility across threads
     * 
     * USAGE:
     * - Set to true by stop() method
     * - Checked by election_thread_ and heartbeat_thread_
     * - Threads exit their loops when true
     */
    std::atomic<bool> running_;
    
    /**
     * @brief Background thread monitoring election timeouts
     * 
     * RUNS: election_timer_loop()
     * 
     * RESPONSIBILITIES:
     * - Check if election timeout expired (follower/candidate only)
     * - Trigger election if timeout without leader heartbeat
     * - Sleep between checks to reduce CPU usage
     */
    std::thread election_thread_;
    
    /**
     * @brief Background thread sending leader heartbeats
     * 
     * RUNS: heartbeat_loop()
     * 
     * RESPONSIBILITIES:
     * - Send AppendEntries to all followers (leader only)
     * - Replicate log entries
     * - Maintain leader authority
     * - Sleep between heartbeats (50ms default)
     */
    std::thread heartbeat_thread_;
    
    /**
     * @brief Protects state transitions and commit_index
     * 
     * CRITICAL SECTIONS:
     * - State changes (FOLLOWER ↔ CANDIDATE ↔ LEADER)
     * - Commit index updates
     * - Leader initialization
     * 
     * DESIGN: Coarse-grained locking for simplicity
     * Fine-grained alternative would split state/commit mutexes.
     */
    std::mutex state_mutex_;
    
    /**
     * @brief Condition variable for waking threads on state changes
     * 
     * USAGE:
     * - Signal when becoming leader (wake heartbeat thread)
     * - Signal when stopping (wake all threads)
     * 
     * PATTERN:
     * ```cpp
     * std::unique_lock<std::mutex> lock(state_mutex_);
     * state_cv_.wait(lock, [this]{ return running_ && is_leader(); });
     * ```
     */
    std::condition_variable state_cv_;
    
    /**
     * ELECTION TIMER MANAGEMENT
     */
    
    /**
     * @brief Last time we received leader heartbeat
     * 
     * USAGE:
     * - Reset on every AppendEntries from valid leader
     * - Compared against current time to detect timeout
     * 
     * THREAD SAFETY: Protected by timer_mutex_
     * 
     * EXAMPLE:
     * last_heartbeat_ = now() - 200ms
     * election_timeout = 150ms
     * → Timeout! Start election
     */
    std::chrono::steady_clock::time_point last_heartbeat_;
    
    /**
     * @brief Protects last_heartbeat_ and election timer state
     * 
     * SEPARATE MUTEX: High-frequency access (every heartbeat)
     * Keeping separate from state_mutex_ reduces contention.
     */
    std::mutex timer_mutex_;
    
    /**
     * @brief Random number generator for election timeouts
     * 
     * USAGE:
     * Generate random timeout in range [min, max] to prevent
     * simultaneous elections (split vote avoidance).
     * 
     * THREAD SAFETY: std::mt19937 not thread-safe
     * Only accessed from election_thread_ (single-threaded).
     */
    std::mt19937 rng_;
    
    /**
     * VOTE COUNTING (CANDIDATE STATE)
     */
    
    /**
     * @brief Number of votes received in current election
     * 
     * THREAD SAFETY: std::atomic (incremented by RPC handler threads)
     * 
     * USAGE:
     * - Reset to 1 when starting election (vote for self)
     * - Incremented for each RequestVote response with vote_granted=true
     * - Compared against majority threshold to determine election win
     * 
     * EXAMPLE:
     * 5-node cluster: majority = 3
     * votes_received = 3 → Won election, become leader
     */
    std::atomic<int> votes_received_;
    
public:
    //==========================================================================
    // SECTION 6.1: Public Interface - Construction and Lifecycle
    //==========================================================================
    
    /**
     * @brief Constructs a Raft node with specified configuration
     * 
     * INITIALIZATION SEQUENCE:
     * 1. Store configuration and node_id
     * 2. Initialize state to FOLLOWER
     * 3. Create RaftLog (attempts to load existing log from disk)
     * 4. Initialize volatile state (commit_index=0, last_applied=0)
     * 5. Seed random number generator for election timeouts
     * 6. Set current_leader to 0 (unknown)
     * 7. Set running to false (not started yet)
     * 
     * @param config Configuration parameters
     * 
     * PRECONDITIONS:
     * - config.node_id must be > 0
     * - config.log_dir must be writable
     * 
     * POSTCONDITIONS:
     * - Node ready to add peers
     * - Must call start() to begin participating
     * 
     * EXCEPTION SAFETY:
     * - May throw if log directory inaccessible
     * - May throw if log file corrupted (std::runtime_error)
     * 
     * EXAMPLE:
     * ```cpp
     * RaftConfig config;
     * config.node_id = 1;
     * config.log_dir = "/var/raft/node1";
     * config.election_timeout_min_ms = 150;
     * config.election_timeout_max_ms = 300;
     * 
     * try {
     *     RaftNode node(config);
     *     // Node constructed successfully
     * } catch (const std::exception& e) {
     *     std::cerr << "Failed to create node: " << e.what() << '\n';
     * }
     * ```
     */
    explicit RaftNode(const RaftConfig& config);
    
    /**
     * @brief Destroys the Raft node, ensuring clean shutdown
     * 
     * CLEANUP SEQUENCE:
     * 1. Call stop() if not already stopped
     * 2. Wait for threads to terminate
     * 3. Flush log to disk
     * 4. Release all resources
     * 
     * BLOCKING:
     * Destructor may block briefly waiting for threads to exit.
     * Typically completes in < 500ms.
     * 
     * EXCEPTION SAFETY:
     * Destructor does not throw (all errors logged).
     */
    ~RaftNode();
    
    /**
     * @brief Starts the Raft node and begins cluster participation
     * 
     * STARTUP SEQUENCE:
     * 1. Validate configuration (check peers added, etc.)
     * 2. Reset election timer
     * 3. Launch election timer thread
     * 4. Launch heartbeat thread
     * 5. Set running flag to true
     * 
     * @return true if started successfully, false on error
     * 
     * PRECONDITIONS:
     * - Peers must be added via add_peer() before calling start()
     * - Must not already be running
     * 
     * POSTCONDITIONS:
     * - Background threads active
     * - Node participates in elections and heartbeats
     * - Ready to handle RPCs
     * 
     * THREADING:
     * This method returns immediately after launching threads.
     * Actual cluster participation happens asynchronously.
     * 
     * FAILURE MODES:
     * - Returns false if already running
     * - Returns false if no peers configured
     * - Returns false if thread creation fails
     * 
     * EXAMPLE:
     * ```cpp
     * RaftNode node(config);
     * node.add_peer(2, "node2", 6000);
     * node.add_peer(3, "node3", 6000);
     * 
     * if (!node.start()) {
     *     std::cerr << "Failed to start node\n";
     *     return 1;
     * }
     * 
     * std::cout << "Node started, participating in cluster\n";
     * ```
     */
    bool start();
    
    /**
     * @brief Stops the Raft node gracefully
     * 
     * SHUTDOWN SEQUENCE:
     * 1. Set running flag to false
     * 2. Signal condition variables (wake sleeping threads)
     * 3. Join election_thread_ (wait for exit)
     * 4. Join heartbeat_thread_ (wait for exit)
     * 5. Flush log to disk
     * 
     * BLOCKING:
     * This method blocks until threads have fully terminated.
     * Typically completes in < 500ms.
     * 
     * IDEMPOTENT:
     * Safe to call multiple times (no-op if already stopped).
     * 
     * THREAD SAFETY:
     * Can be called from any thread (e.g., signal handler).
     * 
     * GRACEFUL SHUTDOWN:
     * - Completes current RPC handlers
     * - Persists all state to disk
     * - No data loss
     * 
     * EXAMPLE:
     * ```cpp
     * // Signal handler for Ctrl-C
     * RaftNode* g_node = nullptr;
     * void signal_handler(int) {
     *     if (g_node) {
     *         g_node->stop();
     *     }
     * }
     * 
     * int main() {
     *     RaftNode node(config);
     *     g_node = &node;
     *     signal(SIGINT, signal_handler);
     *     
     *     node.start();
     *     // ... run until Ctrl-C
     *     return 0;
     * }
     * ```
     */
    void stop();
    
    /**
     * @brief Registers a peer node in the cluster
     * 
     * CLUSTER FORMATION:
     * Each node must call add_peer() for every OTHER node in the cluster.
     * This establishes the membership for consensus.
     * 
     * @param node_id Unique identifier for peer (must not equal this node's ID)
     * @param hostname DNS name or IP address of peer
     * @param port TCP port where peer listens for Raft RPCs
     * 
     * PRECONDITIONS:
     * - Must be called BEFORE start()
     * - node_id must be unique (not already added)
     * - node_id must not equal this node's node_id
     * 
     * POSTCONDITIONS:
     * - Peer added to peers_ map
     * - Peer will be included in elections and replication
     * 
     * THREAD SAFETY:
     * Safe to call from any thread before start().
     * Undefined behavior if called after start().
     * 
     * CLUSTER SIZE:
     * Minimum 3 nodes for fault tolerance (can tolerate 1 failure).
     * Odd numbers preferred (3, 5, 7) to avoid split votes.
     * 
     * EXAMPLE:
     * ```cpp
     * // 3-node cluster configuration
     * 
     * // On node 1 (kh01):
     * RaftConfig config1;
     * config1.node_id = 1;
     * RaftNode node1(config1);
     * node1.add_peer(2, "kh02", 6000);  // Add node 2
     * node1.add_peer(3, "kh03", 6000);  // Add node 3
     * node1.start();
     * 
     * // On node 2 (kh02):
     * RaftConfig config2;
     * config2.node_id = 2;
     * RaftNode node2(config2);
     * node2.add_peer(1, "kh01", 6000);  // Add node 1
     * node2.add_peer(3, "kh03", 6000);  // Add node 3
     * node2.start();
     * 
     * // On node 3 (kh03):
     * RaftConfig config3;
     * config3.node_id = 3;
     * RaftNode node3(config3);
     * node3.add_peer(1, "kh01", 6000);  // Add node 1
     * node3.add_peer(2, "kh02", 6000);  // Add node 2
     * node3.start();
     * ```
     */
    void add_peer(uint64_t node_id, const std::string& hostname, uint16_t port);
    
    //==========================================================================
    // SECTION 6.2: State Queries
    //==========================================================================
    
    /**
     * @brief Checks if this node is currently the leader
     * 
     * @return true if leader, false otherwise
     * 
     * USAGE:
     * Check before appending log entries or handling client requests.
     * Only leader can accept writes.
     * 
     * ATOMICITY:
     * Lock-free read via std::atomic<NodeState>.
     * May return stale result if state transition in progress.
     * 
     * LEADERSHIP LOSS:
     * Even if returns true, leadership may be lost immediately after.
     * Caller should handle potential failures gracefully.
     * 
     * EXAMPLE:
     * ```cpp
     * if (node.is_leader()) {
     *     // Safe to append entry
     *     node.append_log_entry(type, data);
     * } else {
     *     // Redirect to leader
     *     uint64_t leader_id = node.get_leader_id();
     *     redirect_client_to(leader_id);
     * }
     * ```
     */
    bool is_leader() const { return state_ == NodeState::LEADER; }
    
    /**
     * @brief Gets the current state (FOLLOWER/CANDIDATE/LEADER)
     * 
     * @return Current NodeState
     * 
     * DEBUGGING:
     * Useful for monitoring and debugging cluster state.
     * 
     * ATOMICITY:
     * Lock-free read, may be stale.
     */
    NodeState get_state() const { return state_; }
    
    /**
     * @brief Gets the current Raft term
     * 
     * @return Current term from log
     * 
     * USAGE:
     * - Include in RPC messages
     * - Compare with peer terms
     * - Detect stale messages
     * 
     * THREAD SAFETY:
     * Delegates to RaftLog which has internal synchronization.
     */
    uint64_t get_current_term() const { return log_.get_current_term(); }
    
    /**
     * @brief Gets the node ID of current leader (0 if unknown)
     * 
     * @return Leader's node_id, or 0 if no leader
     * 
     * CLIENT FORWARDING:
     * Clients can use this to find leader for write requests.
     * 
     * VALUES:
     * - 0: Election in progress, no leader yet
     * - Non-zero: Current leader's node_id
     * 
     * STALENESS:
     * Value may be outdated if leader just failed.
     * Client should retry if connection to "leader" fails.
     * 
     * ATOMICITY:
     * Lock-free read via std::atomic.
     * 
     * EXAMPLE:
     * ```cpp
     * uint64_t leader_id = node.get_leader_id();
     * if (leader_id == 0) {
     *     std::cout << "No leader, election in progress\n";
     * } else {
     *     std::cout << "Current leader is node " << leader_id << "\n";
     * }
     * ```
     */
    uint64_t get_leader_id() const { return current_leader_; }
    
    //==========================================================================
    // SECTION 6.3: Log Operations (Leader Only)
    //==========================================================================
    
    /**
     * @brief Appends a new entry to the log (leader only)
     * 
     * CLIENT WRITE PATH:
     * This is how clients submit commands to the Raft cluster.
     * Leader appends to log, replicates to followers, commits once
     * majority replicates.
     * 
     * @param type Entry type (JOB_SUBMIT, CONFIG_CHANGE, etc.)
     * @param data Serialized command payload
     * 
     * @return true if successfully appended, false if not leader
     * 
     * PRECONDITIONS:
     * - Must be leader (returns false otherwise)
     * - Log must have space (no size limits in this implementation)
     * 
     * POSTCONDITIONS:
     * - Entry appended to local log
     * - Entry will be replicated to followers asynchronously
     * - Once majority replicates, entry becomes committed
     * 
     * SYNCHRONOUS vs ASYNCHRONOUS:
     * - Append to local log: SYNCHRONOUS (blocks on disk write ~1-5ms)
     * - Replication to followers: ASYNCHRONOUS (happens in background)
     * - Commitment: ASYNCHRONOUS (detected by heartbeat thread)
     * 
     * DURABILITY:
     * Entry is durable on leader immediately (log_.append persists).
     * Not yet committed (not safe to apply to state machine).
     * 
     * FAILURE HANDLING:
     * - If not leader: Returns false immediately
     * - If disk write fails: Logs error, may crash (data safety)
     * 
     * EXAMPLE:
     * ```cpp
     * // Client submitting job
     * JobParams params = {...};
     * std::vector<uint8_t> data = serialize(params);
     * 
     * if (!node.append_log_entry(LogEntryType::JOB_SUBMIT, data)) {
     *     // Not leader, redirect client
     *     uint64_t leader_id = node.get_leader_id();
     *     send_to_leader(leader_id, params);
     * } else {
     *     // Appended successfully, will be committed soon
     *     std::cout << "Job submitted to leader\n";
     * }
     * ```
     * 
     * LINEARIZABILITY:
     * For linearizable semantics, must wait for commit before responding:
     * ```cpp
     * uint64_t index = node.append_log_entry(type, data);
     * wait_until_committed(index);  // Block until majority replicates
     * respond_to_client(success);
     * ```
     */
    bool append_log_entry(LogEntryType type, const std::vector<uint8_t>& data);
    
    //==========================================================================
    // SECTION 6.4: RPC Handlers (Called by Network Layer)
    //==========================================================================
    
    /**
     * @brief Handles incoming RequestVote RPC
     * 
     * VOTER LOGIC:
     * Implements Raft's voting rules to ensure safety properties.
     * 
     * @param req Vote request from candidate
     * @return Response indicating whether vote granted
     * 
     * DECISION LOGIC:
     * 
     * STEP 1: Term Check
     * ```cpp
     * if (req.term < log_.get_current_term()) {
     *     return {.term = log_.get_current_term(), .vote_granted = false};
     * }
     * if (req.term > log_.get_current_term()) {
     *     become_follower(req.term);
     * }
     * ```
     * 
     * STEP 2: Vote Availability
     * ```cpp
     * uint64_t voted_for = log_.get_voted_for();
     * if (voted_for != 0 && voted_for != req.candidate_id) {
     *     return {.term = log_.get_current_term(), .vote_granted = false};
     * }
     * ```
     * 
     * STEP 3: Log Up-to-Date Check
     * ```cpp
     * bool log_ok = (req.last_log_term > log_.last_log_term()) ||
     *               (req.last_log_term == log_.last_log_term() &&
     *                req.last_log_index >= log_.last_log_index());
     * if (!log_ok) {
     *     return {.term = log_.get_current_term(), .vote_granted = false};
     * }
     * ```
     * 
     * STEP 4: Grant Vote
     * ```cpp
     * log_.set_voted_for(req.candidate_id);  // Persist vote
     * reset_election_timer();  // Don't start own election
     * return {.term = log_.get_current_term(), .vote_granted = true};
     * ```
     * 
     * THREAD SAFETY:
     * Fully thread-safe. Multiple peers can request votes concurrently.
     * First valid request wins (one vote per term).
     * 
     * BLOCKING:
     * Blocks briefly on log_.set_voted_for() (disk write ~1-5ms).
     * This is intentional for durability.
     * 
     * EXAMPLE TRACE:
     * ```
     * [Node 2] Received RequestVote from Node 1
     *          term=5, candidate_id=1, last_log_index=10, last_log_term=4
     * [Node 2] My term=4, updating to term=5
     * [Node 2] Haven't voted yet this term
     * [Node 2] Candidate's log is up-to-date (term 4 >= 4, index 10 >= 9)
     * [Node 2] GRANTED vote to Node 1
     * ```
     */
    RequestVoteResponse handle_request_vote(const RequestVoteRequest& req);
    
    /**
     * @brief Handles incoming AppendEntries RPC
     * 
     * FOLLOWER LOGIC:
     * Validates leader's request and appends entries if consistent.
     * 
     * @param req Append request from leader (heartbeat or replication)
     * @return Response indicating success/failure
     * 
     * DECISION LOGIC:
     * 
     * STEP 1: Term Check
     * ```cpp
     * if (req.term < log_.get_current_term()) {
     *     return {.term = log_.get_current_term(), .success = false};
     * }
     * if (req.term >= log_.get_current_term()) {
     *     become_follower(req.term);
     *     current_leader_ = req.leader_id;
     *     reset_election_timer();
     * }
     * ```
     * 
     * STEP 2: Log Consistency Check
     * ```cpp
     * if (req.prev_log_index > 0) {
     *     if (req.prev_log_index > log_.last_log_index()) {
     *         return {.term = log_.get_current_term(), .success = false};
     *     }
     *     LogEntry prev = log_.at(req.prev_log_index);
     *     if (prev.term != req.prev_log_term) {
     *         log_.truncate_from(req.prev_log_index);
     *         return {.term = log_.get_current_term(), .success = false};
     *     }
     * }
     * ```
     * 
     * STEP 3: Append Entries
     * ```cpp
     * if (!req.entries.empty()) {
     *     log_.append_batch(req.entries);
     * }
     * ```
     * 
     * STEP 4: Update Commit Index
     * ```cpp
     * if (req.leader_commit > commit_index_) {
     *     commit_index_ = std::min(req.leader_commit, log_.last_log_index());
     * }
     * ```
     * 
     * STEP 5: Return Success
     * ```cpp
     * return {
     *     .term = log_.get_current_term(),
     *     .success = true,
     *     .match_index = log_.last_log_index()
     * };
     * ```
     * 
     * HEARTBEAT vs REPLICATION:
     * - Heartbeat: req.entries.empty() == true (no disk write)
     * - Replication: req.entries.empty() == false (disk write required)
     * 
     * THREAD SAFETY:
     * Fully thread-safe. Leader can send AppendEntries to multiple
     * followers concurrently.
     * 
     * BLOCKING:
     * If entries non-empty, blocks on disk write (~1-5ms).
     * Heartbeats are fast (no disk write).
     * 
     * EXAMPLE TRACE (Heartbeat):
     * ```
     * [Node 2] Received AppendEntries from Node 1
     *          term=5, leader_id=1, prev_log_index=10, entries=[]
     * [Node 2] Heartbeat from leader, resetting election timer
     * [Node 2] Commit index updated to 8 (was 7)
     * [Node 2] SUCCESS
     * ```
     * 
     * EXAMPLE TRACE (Replication):
     * ```
     * [Node 2] Received AppendEntries from Node 1
     *          term=5, leader_id=1, prev_log_index=10, entries=[11,12]
     * [Node 2] Consistency check passed
     * [Node 2] Appending 2 entries to log
     * [Node 2] Disk write completed
     * [Node 2] SUCCESS, match_index=12
     * ```
     */
    AppendEntriesResponse handle_append_entries(const AppendEntriesRequest& req);
    
    //==========================================================================
    // SECTION 6.5: Virtual RPC Senders (Implemented by Subclass)
    //==========================================================================
    
    /**
     * @brief Sends RequestVote RPC to peer (virtual method)
     * 
     * NETWORK ABSTRACTION:
     * This method is pure virtual (must be implemented by subclass).
     * Allows different network implementations (TCP, UDP, in-memory testing).
     * 
     * @param peer_id Target peer's node_id
     * @param req Vote request to send
     * @param resp [out] Response from peer
     * 
     * @return true if RPC succeeded (got response), false on failure
     * 
     * IMPLEMENTATION RESPONSIBILITIES:
     * - Establish connection to peer (if needed)
     * - Serialize req to wire format
     * - Send over network
     * - Wait for response (with timeout)
     * - Deserialize response into resp
     * - Handle network errors gracefully
     * 
     * TIMEOUT:
     * Should timeout after ~RPC_TIMEOUT (e.g., 500ms).
     * Don't block indefinitely if peer is down.
     * 
     * ERROR HANDLING:
     * Return false on any error:
     * - Connection refused
     * - Timeout
     * - Serialization failure
     * - Network partition
     * 
     * EXAMPLE IMPLEMENTATION (TCP):
     * ```cpp
     * bool NetworkRaftNode::send_request_vote(
     *     uint64_t peer_id,
     *     const RequestVoteRequest& req,
     *     RequestVoteResponse& resp)
     * {
     *     // Get peer's socket
     *     int sock = get_peer_socket(peer_id);
     *     if (sock < 0) return false;
     *     
     *     // Serialize and send
     *     std::vector<uint8_t> data = serialize(req);
     *     if (send(sock, data.data(), data.size(), 0) < 0) {
     *         return false;
     *     }
     *     
     *     // Receive response
     *     std::vector<uint8_t> resp_data(1024);
     *     ssize_t n = recv(sock, resp_data.data(), resp_data.size(), 0);
     *     if (n <= 0) return false;
     *     
     *     // Deserialize
     *     resp = deserialize<RequestVoteResponse>(resp_data);
     *     return true;
     * }
     * ```
     */
    virtual bool send_request_vote(uint64_t peer_id, 
                                   const RequestVoteRequest& req,
                                   RequestVoteResponse& resp);
    
    /**
     * @brief Sends AppendEntries RPC to peer (virtual method)
     * 
     * NETWORK ABSTRACTION:
     * Pure virtual method for network layer implementation.
     * 
     * @param peer_id Target peer's node_id
     * @param req AppendEntries request to send
     * @param resp [out] Response from peer
     * 
     * @return true if RPC succeeded, false on failure
     * 
     * FREQUENCY:
     * Called very frequently by leader (every 50ms per peer).
     * Implementation should be efficient.
     * 
     * OPTIMIZATION:
     * Consider connection pooling and message batching for performance.
     * 
     * LARGE MESSAGES:
     * If req.entries is large (many entries), may take longer to send.
     * Consider limiting batch size or using compression.
     * 
     * EXAMPLE IMPLEMENTATION (TCP):
     * ```cpp
     * bool NetworkRaftNode::send_append_entries(
     *     uint64_t peer_id,
     *     const AppendEntriesRequest& req,
     *     AppendEntriesResponse& resp)
     * {
     *     int sock = get_peer_socket(peer_id);
     *     if (sock < 0) return false;
     *     
     *     std::vector<uint8_t> data = serialize(req);
     *     if (send(sock, data.data(), data.size(), 0) < 0) {
     *         return false;
     *     }
     *     
     *     std::vector<uint8_t> resp_data(1024);
     *     ssize_t n = recv(sock, resp_data.data(), resp_data.size(), 0);
     *     if (n <= 0) return false;
     *     
     *     resp = deserialize<AppendEntriesResponse>(resp_data);
     *     return true;
     * }
     * ```
     */
    virtual bool send_append_entries(uint64_t peer_id, 
                                     const AppendEntriesRequest& req,
                                     AppendEntriesResponse& resp);
    
private:
    //==========================================================================
    // SECTION 6.6: Private Implementation
    //==========================================================================
    
    /**
     * ELECTION LOGIC
     */
    
    /**
     * @brief Background thread monitoring election timeouts
     * 
     * LOOP STRUCTURE:
     * ```cpp
     * while (running_) {
     *     if (state_ != LEADER && is_election_timeout()) {
     *         start_election();
     *     }
     *     sleep(10ms);  // Check frequently but not constantly
     * }
     * ```
     * 
     * FOLLOWERS:
     * If no heartbeat for election_timeout, become candidate.
     * 
     * CANDIDATES:
     * If election doesn't complete within timeout, retry.
     * 
     * LEADERS:
     * Don't check timeout (already leader).
     */
    void election_timer_loop();
    
    /**
     * @brief Initiates leader election
     * 
     * ELECTION PROCESS:
     * 1. Increment term
     * 2. Transition to CANDIDATE state
     * 3. Vote for self
     * 4. Reset election timer
     * 5. Send RequestVote to all peers
     * 6. Count votes
     * 7. If win majority, become LEADER
     * 8. If discover higher term, become FOLLOWER
     * 9. If timeout, retry election
     * 
     * CONCURRENT ELECTIONS:
     * Multiple nodes may start elections simultaneously (split vote).
     * Randomized timeouts ensure one eventually wins.
     */
    void start_election();
    
    /**
     * @brief Transitions to FOLLOWER state
     * 
     * TRIGGERS:
     * - Discover higher term in RPC
     * - Lose election
     * - Timeout as candidate without winning
     * 
     * @param term New term to adopt
     * 
     * STATE CHANGES:
     * - Update current_term in log
     * - Clear vote for new term
     * - Set state to FOLLOWER
     * - Reset election timer
     * - Log state transition
     */
    void become_follower(uint64_t term);
    
    /**
     * @brief Transitions to CANDIDATE state
     * 
     * TRIGGERS:
     * - Election timeout as follower
     * 
     * STATE CHANGES:
     * - Set state to CANDIDATE
     * - Increment term
     * - Vote for self
     * - Reset votes_received to 1
     * - Log state transition
     */
    void become_candidate();
    
    /**
     * @brief Transitions to LEADER state
     * 
     * TRIGGERS:
     * - Win election (receive majority votes)
     * 
     * STATE CHANGES:
     * - Set state to LEADER
     * - Set current_leader to self
     * - Initialize peer next_index and match_index
     * - Send initial heartbeats to assert authority
     * - Wake heartbeat thread
     * - Log state transition
     * 
     * INITIALIZATION:
     * For each peer:
     * - next_index = last_log_index + 1 (optimistic)
     * - match_index = 0 (nothing confirmed yet)
     */
    void become_leader();
    
    /**
     * HEARTBEAT LOGIC
     */
    
    /**
     * @brief Background thread sending leader heartbeats
     * 
     * LOOP STRUCTURE:
     * ```cpp
     * while (running_) {
     *     if (is_leader()) {
     *         send_heartbeats();
     *     }
     *     sleep(heartbeat_interval_ms);
     * }
     * ```
     * 
     * LEADERS ONLY:
     * Only active when this node is leader.
     * Followers/candidates sleep until becoming leader.
     * 
     * FREQUENCY:
     * Runs every heartbeat_interval_ms (default 50ms).
     */
    void heartbeat_loop();
    
    /**
     * @brief Sends AppendEntries to all followers
     * 
     * LEADER OPERATION:
     * For each peer:
     * 1. Prepare AppendEntries based on peer's next_index
     * 2. Send RPC (via send_append_entries)
     * 3. Process response
     * 4. Update peer's next_index and match_index
     * 5. Advance commit_index if majority replicated
     * 
     * PARALLELIZATION:
     * Sends to all peers concurrently (parallel RPCs).
     * Don't wait for one peer before contacting next.
     */
    void send_heartbeats();
    
    /**
     * @brief Replicates log to specific peer
     * 
     * LOG REPAIR:
     * Determines which entries peer needs based on next_index.
     * Sends AppendEntries with those entries.
     * Adjusts next_index on failure (backtrack).
     * 
     * @param peer Peer to replicate to (contains next_index, match_index)
     * 
     * CONSISTENCY CHECKING:
     * Uses prev_log_index and prev_log_term to ensure consistency.
     * 
     * EFFICIENCY:
     * - If peer caught up: Send heartbeat (empty entries)
     * - If peer behind: Send batch of entries
     * - If peer far behind: May take multiple rounds
     */
    void replicate_log_to_peer(PeerInfo& peer);
    
    /**
     * TIMER MANAGEMENT
     */
    
    /**
     * @brief Resets election timer to current time
     * 
     * USAGE:
     * Called when receiving valid heartbeat from leader.
     * Prevents follower from starting election.
     * 
     * THREAD SAFETY:
     * Protected by timer_mutex_.
     */
    void reset_election_timer();
    
    /**
     * @brief Checks if election timeout has expired
     * 
     * @return true if timeout expired, false otherwise
     * 
     * CALCULATION:
     * timeout_duration = get_random_timeout_ms()
     * elapsed = now() - last_heartbeat_
     * return elapsed > timeout_duration
     * 
     * THREAD SAFETY:
     * Protected by timer_mutex_.
     */
    bool is_election_timeout();
    
    /**
     * @brief Generates random election timeout in configured range
     * 
     * @return Random value in [election_timeout_min_ms, election_timeout_max_ms]
     * 
     * RANDOMIZATION:
     * Prevents simultaneous elections (split vote avoidance).
     * Different nodes have different timeouts.
     * 
     * DISTRIBUTION:
     * Uniform random distribution across range.
     */
    int get_random_timeout_ms();
    
    /**
     * COMMIT MANAGEMENT
     */
    
    /**
     * @brief Advances commit_index based on majority replication
     * 
     * LEADER OPERATION:
     * Finds highest index N such that:
     * - Majority of peers have match_index >= N
     * - Entry at N was created in current term
     * 
     * ALGORITHM:
     * 1. Collect match_index from all peers (including self)
     * 2. Sort indices
     * 3. Find median (majority threshold)
     * 4. Verify entry at median was created in current term
     * 5. Update commit_index to median
     * 
     * SAFETY:
     * Only commits entries from current term (Raft safety property).
     * Entries from previous terms committed indirectly.
     * 
     * EXAMPLE:
     * 5-node cluster, leader's log up to index 100
     * match_indices: [100 (self), 98, 96, 90, 85]
     * Sorted: [85, 90, 96, 98, 100]
     * Majority (3rd of 5): 96
     * If entry[96].term == current_term: commit_index = 96
     */
    void advance_commit_index();
};

} // namespace raft
} // namespace backtesting

#endif // RAFT_NODE_H

//==============================================================================
// SECTION 7: Usage Examples and Patterns
//==============================================================================

/**
 * PATTERN 1: Basic 3-Node Cluster Setup
 * 
 * ```cpp
 * // Main function for each node
 * int main(int argc, char** argv) {
 *     // Parse command-line arguments
 *     uint64_t node_id = parse_node_id(argv[1]);  // 1, 2, or 3
 *     
 *     // Configure node
 *     RaftConfig config;
 *     config.node_id = node_id;
 *     config.log_dir = "/var/raft/node" + std::to_string(node_id);
 *     
 *     // Create node
 *     RaftNode node(config);
 *     
 *     // Add peers (other nodes in cluster)
 *     if (node_id != 1) node.add_peer(1, "kh01", 6000);
 *     if (node_id != 2) node.add_peer(2, "kh02", 6000);
 *     if (node_id != 3) node.add_peer(3, "kh03", 6000);
 *     
 *     // Start participating in cluster
 *     if (!node.start()) {
 *         std::cerr << "Failed to start node\n";
 *         return 1;
 *     }
 *     
 *     std::cout << "Node " << node_id << " started\n";
 *     
 *     // Run until Ctrl-C
 *     signal(SIGINT, [](int){ exit(0); });
 *     while (true) {
 *         std::this_thread::sleep_for(std::chrono::seconds(1));
 *     }
 *     
 *     return 0;
 * }
 * ```
 */

/**
 * PATTERN 2: Client Request Handling with Leader Forwarding
 * 
 * ```cpp
 * class ClientRequestHandler {
 *     RaftNode& node_;
 *     std::map<uint64_t, std::string> peer_addresses_;
 *     
 * public:
 *     Response handle_request(const ClientRequest& req) {
 *         // Check if we're leader
 *         if (!node_.is_leader()) {
 *             // Forward to leader
 *             uint64_t leader_id = node_.get_leader_id();
 *             if (leader_id == 0) {
 *                 return Response::error("No leader available");
 *             }
 *             std::string leader_addr = peer_addresses_[leader_id];
 *             return Response::redirect(leader_addr);
 *         }
 *         
 *         // We're leader, process request
 *         std::vector<uint8_t> data = serialize(req);
 *         if (!node_.append_log_entry(LogEntryType::CLIENT_REQUEST, data)) {
 *             // Lost leadership during processing
 *             return Response::error("Lost leadership, retry");
 *         }
 *         
 *         // Entry appended, will be committed soon
 *         // For linearizable semantics, should wait for commit here
 *         return Response::success();
 *     }
 * };
 * ```
 */

/**
 * PATTERN 3: Monitoring Cluster State
 * 
 * ```cpp
 * class ClusterMonitor {
 *     RaftNode& node_;
 *     
 * public:
 *     void print_status() {
 *         NodeState state = node_.get_state();
 *         uint64_t term = node_.get_current_term();
 *         uint64_t leader = node_.get_leader_id();
 *         
 *         std::cout << "=== Cluster Status ===\n";
 *         std::cout << "State: " << state_to_string(state) << "\n";
 *         std::cout << "Term: " << term << "\n";
 *         
 *         if (leader == 0) {
 *             std::cout << "Leader: None (election in progress)\n";
 *         } else if (node_.is_leader()) {
 *             std::cout << "Leader: This node\n";
 *         } else {
 *             std::cout << "Leader: Node " << leader << "\n";
 *         }
 *     }
 *     
 * private:
 *     std::string state_to_string(NodeState state) {
 *         switch (state) {
 *             case NodeState::FOLLOWER: return "FOLLOWER";
 *             case NodeState::CANDIDATE: return "CANDIDATE";
 *             case NodeState::LEADER: return "LEADER";
 *         }
 *     }
 * };
 * ```
 */

//==============================================================================
// SECTION 8: State Machine Transitions
//==============================================================================

/**
 * COMPLETE STATE TRANSITION DIAGRAM
 * 
 * States: F=Follower, C=Candidate, L=Leader
 * 
 * STARTUP:
 *   ┌─────┐
 *   │START│
 *   └──┬──┘
 *      │ initialize
 *      ▼
 *   ┌──────────┐
 *   │ FOLLOWER │◄──────────────────┐
 *   └────┬─────┘                   │
 *        │                         │
 *        │ timeout without         │ discover higher term
 *        │ leader heartbeat        │ or current leader
 *        ▼                         │
 *   ┌──────────┐                   │
 *   │CANDIDATE │───────────────────┤
 *   └────┬─────┘                   │
 *        │                         │
 *        │ win election            │
 *        │ (receive majority       │
 *        │  votes)                 │
 *        ▼                         │
 *   ┌──────────┐                   │
 *   │  LEADER  │───────────────────┘
 *   └──────────┘
 * 
 * DETAILED TRIGGERS:
 * 
 * F→C: Election timeout expired (no heartbeat from leader)
 * C→F: Discovered current leader or higher term
 * C→L: Won election (received votes from majority)
 * L→F: Discovered higher term (another node is leader)
 * C→C: Election timeout (split vote, retry election)
 * 
 * TYPICAL SEQUENCE (Normal Operation):
 * T0: All nodes start as FOLLOWER
 * T1: Node 1 times out, becomes CANDIDATE
 * T2: Node 1 requests votes from Node 2 and 3
 * T3: Nodes 2 and 3 grant votes to Node 1
 * T4: Node 1 receives majority, becomes LEADER
 * T5: Node 1 sends heartbeats to maintain leadership
 * T6-∞: Node 1 remains LEADER (until failure)
 * 
 * TYPICAL SEQUENCE (Leader Failure):
 * T0: Node 1 is LEADER, Nodes 2,3 are FOLLOWERS
 * T1: Node 1 crashes
 * T2: Nodes 2,3 timeout (no heartbeat)
 * T3: Node 2 times out first (random), becomes CANDIDATE
 * T4: Node 2 requests votes (term++, votes for self)
 * T5: Node 3 grants vote to Node 2
 * T6: Node 2 wins election, becomes LEADER
 * T7: Node 2 sends heartbeats to Node 3
 * T8-∞: Node 2 is LEADER
 */

//==============================================================================
// SECTION 9: Timing and Failure Scenarios
//==============================================================================

/**
 * SCENARIO 1: Normal Leader Election (No Failures)
 * 
 * Timeline (ms):
 *   0: All nodes start, become FOLLOWERS
 *  50: Node 1 times out first (random), becomes CANDIDATE, term=1
 *  60: Node 1 sends RequestVote to Nodes 2,3
 *  70: Nodes 2,3 grant votes (haven't timed out yet)
 *  80: Node 1 receives majority (2 of 3), becomes LEADER
 *  80: Node 1 sends initial heartbeats to Nodes 2,3
 * 100: Nodes 2,3 receive heartbeats, reset election timers
 * 150: Node 1 sends heartbeat (maintains authority)
 * 200: Node 1 sends heartbeat
 * ...
 * 
 * Result: Node 1 is stable leader, election took 80ms
 */

/**
 * SCENARIO 2: Split Vote (Multiple Candidates)
 * 
 * Timeline (ms):
 *   0: All nodes are FOLLOWERS
 * 150: Node 1 and Node 2 timeout simultaneously (bad luck!)
 * 150: Both become CANDIDATES, increment term to 1
 * 150: Node 1 votes for self, Node 2 votes for self
 * 160: Node 1 requests vote from Node 2,3
 * 160: Node 2 requests vote from Node 1,3
 * 170: Node 3 receives both requests, grants to Node 1 (arrived first)
 * 170: Node 1 rejects Node 2's request (already voted for self)
 * 170: Node 2 rejects Node 1's request (already voted for self)
 * 180: Node 1: 2 votes (self + Node 3) → not majority (need 2/3)
 * 180: Node 2: 1 vote (self) → not majority
 * 300: Both Node 1 and Node 2 timeout again (different random delays)
 * 310: Node 1 times out first, increments term to 2, restarts election
 * 320: Node 1 requests votes again
 * 330: Nodes 2,3 grant votes (Node 2 gave up, reverted to FOLLOWER)
 * 340: Node 1 wins election, becomes LEADER
 * 
 * Result: Split vote delayed election by 150ms (one extra timeout)
 */

/**
 * SCENARIO 3: Leader Failure and Recovery
 * 
 * Timeline (ms):
 *     0: Node 1 is LEADER, sends heartbeats every 50ms
 *    50: Heartbeat sent
 *   100: Heartbeat sent
 *   150: **Node 1 CRASHES**
 *   200: Nodes 2,3 expect heartbeat, don't receive it
 *   250: Nodes 2,3 waiting...
 *   300: Node 2 times out first, becomes CANDIDATE (term=2)
 *   310: Node 2 requests votes from Node 3
 *   320: Node 3 grants vote (hasn't timed out yet, willing to vote)
 *   330: Node 2 receives majority, becomes LEADER
 *   330: Node 2 sends heartbeats to Node 3
 *   380: Heartbeat sent
 *   430: Heartbeat sent
 *   ...
 *   
 * 1000: **Node 1 RECOVERS**, restarts
 * 1000: Node 1 becomes FOLLOWER (default state), term=1
 * 1050: Node 1 receives heartbeat from Node 2 (term=2)
 * 1050: Node 1 updates term to 2, recognizes Node 2 as leader
 * 1050: Node 1 stays FOLLOWER
 * 
 * Result: Cluster recovered in 180ms (300ms timeout + 30ms election)
 *         Node 1 rejoins as follower without disrupting new leader
 */

/**
 * SCENARIO 4: Network Partition (Split-Brain Prevention)
 * 
 * Cluster: 5 nodes (1,2,3,4,5), Node 1 is LEADER
 * 
 * Timeline (ms):
 *     0: Normal operation, Node 1 is LEADER
 *   100: **Network partition**: {1,2} | {3,4,5}
 *        - Nodes 1,2 can communicate
 *        - Nodes 3,4,5 can communicate
 *        - No communication between groups
 *   
 *   150: Nodes 3,4,5 timeout (no heartbeat from Node 1)
 *   200: Node 3 becomes CANDIDATE (term=2)
 *   210: Node 3 requests votes from 4,5 (can't reach 1,2)
 *   220: Nodes 4,5 grant votes
 *   230: Node 3 has 3 votes (self + 4,5) → MAJORITY (3 of 5)
 *   230: Node 3 becomes LEADER of partition {3,4,5}
 *   
 *   250: Node 1 tries to send heartbeats, fails to reach 3,4,5
 *   250: Node 1 only has 2 votes (self + 2) → NOT MAJORITY
 *   250: **Node 1 cannot commit new entries** (no majority)
 *   250: Node 3 **can commit new entries** (has majority)
 *   
 *  1000: **Partition heals**, all nodes can communicate
 *  1050: Node 1 receives heartbeat from Node 3 (term=2 > term=1)
 *  1050: Node 1 recognizes higher term, becomes FOLLOWER
 *  1050: Node 2 also becomes FOLLOWER
 *  1050: Nodes 1,2 replicate Node 3's log
 *  
 * Result: Partition {3,4,5} continued operating (had majority)
 *         Partition {1,2} stalled (no majority, safe!)
 *         After heal, all nodes converge on Node 3's log
 *         No split-brain, no data loss (Raft safety property)
 */

//==============================================================================
// SECTION 10: Common Pitfalls and Solutions
//==============================================================================

/**
 * PITFALL 1: Election timeout too short
 * 
 * PROBLEM:
 * election_timeout_min_ms = 50ms (too short!)
 * heartbeat_interval_ms = 50ms
 * 
 * CONSEQUENCE:
 * Follower may timeout before heartbeat arrives due to slight delays.
 * Causes frequent unnecessary elections, cluster instability.
 * 
 * SOLUTION:
 * election_timeout_min_ms should be >> heartbeat_interval_ms
 * Rule of thumb: min_timeout >= 3 × heartbeat_interval
 * 
 * CORRECT:
 * heartbeat_interval_ms = 50ms
 * election_timeout_min_ms = 150ms (3× heartbeat)
 * election_timeout_max_ms = 300ms (2× min)
 */

/**
 * PITFALL 2: Forgetting to call add_peer() before start()
 * 
 * PROBLEM:
 * ```cpp
 * RaftNode node(config);
 * node.start();  // No peers added!
 * // Node thinks it's alone, immediately becomes leader
 * ```
 * 
 * CONSEQUENCE:
 * Node runs as single-node cluster. If multiple nodes do this,
 * each thinks it's the only leader (split-brain).
 * 
 * SOLUTION:
 * Always add all peers before start():
 * ```cpp
 * RaftNode node(config);
 * node.add_peer(2, "node2", 6000);
 * node.add_peer(3, "node3", 6000);
 * node.start();  // Correct
 * ```
 */

/**
 * PITFALL 3: Using same log_dir for multiple nodes
 * 
 * PROBLEM:
 * ```cpp
 * // All nodes on same host
 * config1.log_dir = "/var/raft";  // Shared directory!
 * config2.log_dir = "/var/raft";
 * config3.log_dir = "/var/raft";
 * ```
 * 
 * CONSEQUENCE:
 * Nodes overwrite each other's log files. Data corruption.
 * 
 * SOLUTION:
 * Separate directory per node:
 * ```cpp
 * config1.log_dir = "/var/raft/node1";  // Unique
 * config2.log_dir = "/var/raft/node2";
 * config3.log_dir = "/var/raft/node3";
 * ```
 */

/**
 * PITFALL 4: Even-numbered cluster (2, 4, 6 nodes)
 * 
 * PROBLEM:
 * 4-node cluster can split 2+2 on partition.
 * Neither side has majority (need 3/4).
 * Cluster becomes unavailable despite only 2 nodes down.
 * 
 * CONSEQUENCE:
 * Lower availability than necessary.
 * 
 * SOLUTION:
 * Use odd-numbered clusters (3, 5, 7):
 * - 3 nodes: Tolerate 1 failure (need 2/3)
 * - 5 nodes: Tolerate 2 failures (need 3/5)
 * - 7 nodes: Tolerate 3 failures (need 4/7)
 */

/**
 * PITFALL 5: Blocking in RPC handlers
 * 
 * PROBLEM:
 * ```cpp
 * AppendEntriesResponse handle_append_entries(const AppendEntriesRequest& req) {
 *     // ... processing ...
 *     expensive_computation();  // Blocks RPC thread!
 *     // ... more processing ...
 * }
 * ```
 * 
 * CONSEQUENCE:
 * RPC takes too long, leader times out waiting for response.
 * May cause leader to think follower is dead.
 * 
 * SOLUTION:
 * Keep RPC handlers fast:
 * - Do minimal work (append to log, update state)
 * - Defer expensive work to background thread
 * - Target < 10ms per RPC
 */

/**
 * PITFALL 6: Not handling leadership changes
 * 
 * PROBLEM:
 * ```cpp
 * if (node.is_leader()) {
 *     node.append_log_entry(type, data);  // May fail!
 * }
 * // Assume success...
 * ```
 * 
 * CONSEQUENCE:
 * append_log_entry() returns false if leadership lost between check and call.
 * Client thinks operation succeeded but it didn't.
 * 
 * SOLUTION:
 * Always check return value:
 * ```cpp
 * if (node.is_leader()) {
 *     if (!node.append_log_entry(type, data)) {
 *         // Lost leadership, retry on new leader
 *         redirect_to_leader();
 *     }
 * }
 * ```
 */

//==============================================================================
// SECTION 11: Tuning and Performance
//==============================================================================

/**
 * TUNING FOR DIFFERENT SCENARIOS
 * 
 * LOW-LATENCY LAN (< 1ms RTT):
 * - election_timeout_min_ms = 100
 * - election_timeout_max_ms = 200
 * - heartbeat_interval_ms = 30
 * - Benefit: 100-200ms failover time
 * - Risk: Sensitive to transient delays
 * 
 * STANDARD LAN (< 10ms RTT):
 * - election_timeout_min_ms = 150 (default)
 * - election_timeout_max_ms = 300
 * - heartbeat_interval_ms = 50
 * - Benefit: Good balance of speed and stability
 * - Risk: Minimal
 * 
 * HIGH-LATENCY WAN (50-100ms RTT):
 * - election_timeout_min_ms = 500
 * - election_timeout_max_ms = 1000
 * - heartbeat_interval_ms = 150
 * - Benefit: Stable despite variable latency
 * - Risk: Slow failover (500-1000ms)
 * 
 * UNSTABLE NETWORK:
 * - election_timeout_min_ms = 1000
 * - election_timeout_max_ms = 2000
 * - heartbeat_interval_ms = 300
 * - Benefit: Tolerates packet loss and delays
 * - Risk: Very slow failover (1-2 seconds)
 */

/**
 * PERFORMANCE OPTIMIZATION TECHNIQUES
 * 
 * TECHNIQUE 1: Batch Log Entries
 * Instead of one entry per append, buffer and batch:
 * ```cpp
 * std::vector<LogEntry> batch;
 * for (auto& cmd : commands) {
 *     batch.push_back(create_entry(cmd));
 * }
 * log_.append_batch(batch);  // One disk write for all
 * ```
 * 
 * TECHNIQUE 2: Pipeline Replication
 * Don't wait for one AppendEntries before sending next:
 * ```cpp
 * // Bad: Sequential
 * for (auto& peer : peers_) {
 *     send_append_entries(peer);  // Wait for response
 * }
 * 
 * // Good: Parallel
 * std::vector<std::future<Response>> futures;
 * for (auto& peer : peers_) {
 *     futures.push_back(std::async([&]{ 
 *         return send_append_entries(peer); 
 *     }));
 * }
 * // Wait for all
 * for (auto& f : futures) f.get();
 * ```
 * 
 * TECHNIQUE 3: Connection Pooling
 * Reuse TCP connections instead of creating new ones:
 * ```cpp
 * std::map<uint64_t, TcpConnection> connections_;
 * 
 * bool send_rpc(uint64_t peer_id, const RPC& rpc) {
 *     if (!connections_.count(peer_id)) {
 *         connections_[peer_id] = connect_to_peer(peer_id);
 *     }
 *     return connections_[peer_id].send(rpc);
 * }
 * ```
 */

//==============================================================================
// SECTION 12: FAQ
//==============================================================================

/**
 * Q1: Why do we need randomized election timeouts?
 * 
 * A: Without randomization, multiple nodes might timeout simultaneously,
 *    all become candidates, split votes, timeout again simultaneously,
 *    etc. - never electing a leader.
 *    
 *    With randomization (150-300ms range), one node typically times out
 *    first, becomes candidate, and wins election before others timeout.
 *    This avoids repeated split votes.
 */

/**
 * Q2: Can I have a 2-node cluster?
 * 
 * A: Technically yes, but not recommended. 2-node cluster has no fault
 *    tolerance - if one node fails, can't form majority (need 2/2).
 *    
 *    Minimum 3 nodes recommended for production (tolerates 1 failure).
 */

/**
 * Q3: What happens if leader and follower have different logs?
 * 
 * A: Leader detects mismatch via AppendEntries consistency check.
 *    Leader backtracks (decrements next_index) until finding matching
 *    point, then sends all entries from there onward. Follower truncates
 *    conflicting entries and appends leader's entries. Eventually they
 *    converge on identical logs.
 */

/**
 * Q4: How does Raft handle slow followers?
 * 
 * A: Leader tracks each follower's replication progress (match_index).
 *    Slow follower lags behind but doesn't block cluster. Once majority
 *    replicates entry, leader commits it and applies to state machine.
 *    Slow follower eventually catches up through normal AppendEntries.
 */

/**
 * Q5: What if two nodes think they're leader?
 * 
 * A: Impossible if Raft implemented correctly. Only one leader per term
 *    (election safety). If node discovers another leader with same or
 *    higher term, it immediately reverts to follower.
 */

/**
 * Q6: How do clients find the leader?
 * 
 * A: Clients can query any node via get_leader_id(). If node is not
 *    leader, it returns current leader's ID (or 0 if unknown). Client
 *    retries on leader. Leader accepts write, returns success.
 */

/**
 * Q7: Can I read from followers?
 * 
 * A: Yes, but reads may be stale (follower's commit_index behind leader).
 *    For strongly consistent reads, must read from leader.
 *    For eventually consistent reads, follower is fine.
 */

/**
 * Q8: How do I add/remove nodes dynamically?
 * 
 * A: Raft supports configuration changes through special log entries
 *    (joint consensus). Not implemented in this basic version. Would
 *    require cluster to agree on new membership before taking effect.
 */

/**
 * Q9: What's the maximum cluster size?
 * 
 * A: Theoretically unlimited, but practical limits:
 *    - 3-5 nodes: Typical for most applications
 *    - 7-9 nodes: High availability requirements
 *    - 11+ nodes: Rare, high replication latency
 *    
 *    Larger clusters have higher latency (more nodes to reach consensus).
 */

/**
 * Q10: How do I debug leader election issues?
 * 
 * A: Enable detailed logging:
 *    - Log all state transitions (FOLLOWER→CANDIDATE→LEADER)
 *    - Log RequestVote requests and responses
 *    - Log election timer resets
 *    - Log term changes
 *    
 *    Common issues:
 *    - Network partition: Nodes can't communicate
 *    - Clock skew: Nodes have different system times
 *    - Timeout too short: Spurious elections
 *    - Missing peers: Node thinks it's alone
 */