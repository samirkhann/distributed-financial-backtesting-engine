/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: raft_log.h
    Module: Controller Cluster - Raft Consensus Log
    
    Description:
        This header file implements the persistent replicated log for the Raft
        consensus algorithm, which provides fault-tolerant coordination across
        the 3-node controller cluster. The RaftLog maintains the authoritative
        sequence of backtest job commands and ensures all controller nodes agree
        on job scheduling decisions even in the presence of failures.
        
        Raft Consensus Overview:
        Raft is a distributed consensus algorithm that ensures multiple machines
        (controller nodes) maintain identical state despite network partitions
        and node failures. It works by:
        1. Electing a leader among controller nodes
        2. Leader accepts all job requests and replicates them to followers
        3. Entries become committed once replicated to majority (2 of 3 nodes)
        4. All nodes eventually apply committed entries in the same order
        
        Why Raft for This Project:
        Per the project plan (Section 2, Clear Goals), the system requires:
        - 3-node controller cluster for fault tolerance
        - Leader election when primary controller fails
        - Automatic recovery within 5 seconds
        - Consistent job queue state across all controllers
        
        Raft provides exactly these guarantees. If the leader controller crashes,
        followers automatically elect a new leader and continue scheduling jobs
        without losing any committed work.
        
        Architecture Context (Module 1: Controller Cluster):
        
        Controller Node A (Leader)                Controller Node B (Follower)
        ┌─────────────────────────┐              ┌─────────────────────────┐
        │ Job Scheduler           │              │ Job Scheduler           │
        │ Worker Registry         │              │ Worker Registry         │
        │ RaftLog (This Class)    │◄────Sync────►│ RaftLog (This Class)    │
        │ - entries_: [E1,E2,E3]  │              │ - entries_: [E1,E2,E3]  │
        │ - current_term_: 5      │              │ - current_term_: 5      │
        └─────────────────────────┘              └─────────────────────────┘
                    │                                        │
                    └────────────Sync─────────────┬─────────┘
                                                   │
                                    Controller Node C (Follower)
                                    ┌─────────────────────────┐
                                    │ Job Scheduler           │
                                    │ Worker Registry         │
                                    │ RaftLog (This Class)    │
                                    │ - entries_: [E1,E2,E3]  │
                                    │ - current_term_: 5      │
                                    └─────────────────────────┘
        
        Key Guarantees:
        - Log Matching: If two logs contain an entry with same index and term,
          all preceding entries are identical
        - Leader Completeness: If a log entry is committed, all future leaders
          will have that entry
        - State Machine Safety: If a node applies entry at index i, no other
          node will apply a different entry at index i
        
    Critical Consistency Properties:
        This class must maintain several invariants for Raft correctness:
        
        1. Monotonic Term Numbers: current_term_ never decreases
        2. Single Vote Per Term: voted_for_ set at most once per term
        3. Log Immutability: Once an entry is committed, it never changes
        4. Persistence: Term, vote, and log survive crashes (disk storage)
        5. Thread Safety: All operations are atomic (mutex protection)
        
        Violation of any invariant can lead to:
        - Split-brain scenarios (two leaders elected simultaneously)
        - Lost job submissions (entry committed then lost on leader failover)
        - Duplicate job execution (same entry applied twice)
        
    Performance Characteristics:
        - Append: O(1) amortized (vector push_back)
        - Truncate: O(1) (vector resize)
        - Lookup: O(1) (direct index access)
        - Persistence: O(n) where n = log size (sequential write)
        - Memory: 8 bytes per entry + data size (typical entry: ~100 bytes)
        
        For project workload (100 concurrent jobs):
        - Log size: ~100 entries × 100 bytes = 10 KB
        - Disk writes: ~50/second (batch commits)
        - Recovery time: <100ms (small log replay)
        
    Thread Safety:
        ALL public methods are thread-safe via mutex_ protection.
        Multiple threads (Raft RPC handlers, leader election timer,
        heartbeat sender) can safely access the log concurrently.
        
        Internal consistency is maintained even under high contention
        because each method acquires exclusive lock before accessing state.
*******************************************************************************/

#ifndef RAFT_LOG_H
#define RAFT_LOG_H

#include <vector>
#include <string>
#include <cstdint>
#include <fstream>
#include <mutex>

namespace backtesting {
namespace raft {

/*******************************************************************************
 * TABLE OF CONTENTS
 *******************************************************************************
 * 
 * 1. RAFT CONSENSUS FUNDAMENTALS
 *    1.1 What is Raft and Why Do We Need It?
 *    1.2 Core Raft Concepts
 *    1.3 Log Replication Protocol
 *    1.4 Leader Election Process
 * 
 * 2. DATA STRUCTURES
 *    2.1 LogEntryType Enumeration
 *    2.2 LogEntry Structure
 *    2.3 RaftLog Class
 * 
 * 3. PERSISTENT STATE (Raft Paper Section 5.2)
 *    3.1 Why Persistence Matters
 *    3.2 What Must Survive Crashes
 *    3.3 Disk Format and Recovery
 * 
 * 4. RAFT LOG OPERATIONS
 *    4.1 Appending Entries (Leader Path)
 *    4.2 Truncating Conflicting Entries (Follower Path)
 *    4.3 Reading Log State
 *    4.4 Term and Vote Management
 * 
 * 5. INTEGRATION WITH CONTROLLER
 *    5.1 Leader Job Scheduling Flow
 *    5.2 Follower Log Replication Flow
 *    5.3 Crash Recovery Flow
 *    5.4 Leader Election Flow
 * 
 * 6. USAGE EXAMPLES
 *    6.1 Controller Initialization
 *    6.2 Submitting Jobs (Leader)
 *    6.3 Replicating Entries (Follower)
 *    6.4 Handling Conflicts
 * 
 * 7. COMMON PITFALLS & BEST PRACTICES
 *    7.1 Persistence Failures
 *    7.2 Term Management Errors
 *    7.3 Log Inconsistency
 *    7.4 Performance Bottlenecks
 * 
 * 8. FAQ
 * 
 * 9. RAFT CORRECTNESS CHECKLIST
 ******************************************************************************/

/*******************************************************************************
 * SECTION 1: RAFT CONSENSUS FUNDAMENTALS
 *******************************************************************************
 * 
 * 1.1 WHAT IS RAFT AND WHY DO WE NEED IT?
 * ----------------------------------------
 * 
 * Problem Statement:
 * In our distributed backtesting system, we have a 3-node controller cluster
 * that must agree on job scheduling decisions. Challenges:
 * 
 * - What if the leader controller crashes during job submission?
 * - How do we ensure followers have the same job queue as the leader?
 * - What if network partitions split the cluster?
 * - How do we elect a new leader when the old one fails?
 * 
 * Naive approaches and their failures:
 * 
 * Single Controller:
 *    - No fault tolerance - if it crashes, entire system halts
 *    - Violates project requirement: "3-node controller cluster"
 * 
 * Primary-Backup (No Consensus):
 *    - If primary crashes before replicating, backup has stale state
 *    - Split-brain: Both nodes think they're primary
 * 
 * Two-Phase Commit (2PC):
 *    - Coordinator failure blocks entire cluster
 *    - Not partition-tolerant
 * 
 * ✓ Raft Consensus:
 *    - Strong leader: All decisions flow through elected leader
 *    - Majority quorum: Survives minority failures (2 of 3 nodes)
 *    - Log replication: Ensures all nodes converge to same state
 *    - Fast recovery: New leader elected in milliseconds
 * 
 * 
 * 1.2 CORE RAFT CONCEPTS
 * ----------------------
 * 
 * Term:
 *   Logical clock that divides time into election periods. When a controller
 *   becomes leader, it increments term. Terms ensure old leaders can't 
 *   interfere with new leaders.
 *   
 *   Example timeline:
 *   Term 1: Node A is leader (handles jobs 1-50)
 *   Term 2: Node A crashes, Node B elected leader (handles jobs 51-100)
 *   Term 3: Network partition, Node C elected leader (handles jobs 101-150)
 * 
 * Log Index:
 *   Position of entry in the log. Indexes start at 1 (index 0 is implicit).
 *   Log index is append-only and monotonically increasing.
 *   
 *   Log visualization:
 *   Index:    1         2         3         4         5
 *   Term:     1         1         2         2         3
 *   Entry:    Job1      Job2      Job3      Job4      Job5
 * 
 * Committed Entry:
 *   An entry is committed when replicated to majority (2 of 3 nodes).
 *   Committed entries will NEVER be lost, even if leader crashes.
 *   
 *   Commitment scenario:
 *   Leader (Node A): [E1, E2, E3*] (* = committed, replicated to majority)
 *   Follower (Node B): [E1, E2, E3]
 *   Follower (Node C): [E1, E2]     (E3 not yet replicated here, but still committed)
 * 
 * 
 * 1.3 LOG REPLICATION PROTOCOL (Simplified)
 * ------------------------------------------
 * 
 * Normal Operation (No Failures):
 * 
 * 1. Client submits job to leader controller
 *    Client → Leader: "Run SMA strategy on AAPL"
 * 
 * 2. Leader appends entry to local log
 *    Leader log: [E1, E2, E3] → [E1, E2, E3, E4]
 * 
 * 3. Leader sends AppendEntries RPC to followers
 *    Leader → Followers: "Here's entry E4 with term 5 at index 4"
 * 
 * 4. Followers append entry to their logs
 *    Follower A log: [E1, E2, E3] → [E1, E2, E3, E4]
 *    Follower B log: [E1, E2, E3] → [E1, E2, E3, E4]
 * 
 * 5. Followers acknowledge successful append
 *    Followers → Leader: "Successfully appended E4"
 * 
 * 6. Once majority acknowledges, leader marks entry as committed
 *    Leader: "E4 is committed (2 of 3 nodes have it)"
 * 
 * 7. Leader applies committed entry (schedules job to worker)
 *    Leader: "Assign job to Worker Node 1"
 * 
 * 8. Leader notifies client of success
 *    Leader → Client: "Job submitted successfully"
 * 
 * 
 * 1.4 LEADER ELECTION PROCESS
 * ---------------------------
 * 
 * When a follower detects leader failure (no heartbeat):
 * 
 * 1. Follower increments term and becomes candidate
 *    Node B: term 5 → term 6, state: FOLLOWER → CANDIDATE
 * 
 * 2. Candidate votes for itself
 *    Node B: voted_for_ = B (my own ID)
 * 
 * 3. Candidate requests votes from other nodes
 *    Node B → Node A: "Vote for me! My log is up-to-date"
 *    Node B → Node C: "Vote for me! My log is up-to-date"
 * 
 * 4. Nodes grant vote if:
 *    - Candidate's term >= their term
 *    - Candidate's log is at least as up-to-date as theirs
 *    - They haven't voted for anyone else this term
 * 
 * 5. If candidate receives majority votes (2 of 3), becomes leader
 *    Node B: "I received votes from A and myself → I'm leader!"
 * 
 * 6. New leader sends heartbeats to assert authority
 *    Leader (Node B) → All: "I'm the leader for term 6"
 * 
 * Per project requirement: This entire process completes in < 5 seconds
 ******************************************************************************/

/*******************************************************************************
 * SECTION 2.1: LogEntryType Enumeration
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * Identifies the type of command stored in a log entry. The controller state
 * machine interprets entries differently based on type.
 * 
 * WHY MULTIPLE TYPES?
 * -------------------
 * Different operations require different data and have different semantics:
 * - Job submissions create worker assignments
 * - Job completions update worker availability
 * - No-ops maintain log continuity without side effects
 * 
 * TYPE DESCRIPTIONS:
 * ------------------
 * 
 * JOB_SUBMIT (Value: 1):
 *   Represents a new backtest job submitted by a client.
 *   
 *   Data Format (serialized in LogEntry.data):
 *     - Job ID (8 bytes)
 *     - Strategy name (variable, null-terminated string)
 *     - Symbol list (variable, comma-separated)
 *     - Parameters (variable, JSON blob)
 *   
 *   When Applied:
 *     Controller's job scheduler assigns work to available workers
 *   
 *   Example:
 *     Job: "Run SMA Crossover on AAPL,MSFT,GOOGL with params {sma_short:20, sma_long:50}"
 * 
 * JOB_COMPLETE (Value: 2):
 *   Marks a backtest job as finished and worker as available.
 *   
 *   Data Format:
 *     - Job ID (8 bytes)
 *     - Worker ID (8 bytes)
 *     - Status code (1 byte: success=0, failure=1)
 *     - Results (variable, serialized BacktestResult)
 *   
 *   When Applied:
 *     Worker registry marks worker as idle, ready for next job
 *   
 *   Example:
 *     "Job 12345 completed by Worker 3, result: PnL=$1250, Sharpe=1.8"
 * 
 * NO_OP (Value: 3):
 *   Special entry that has no effect on state machine.
 *   
 *   Data Format:
 *     Empty (no data)
 *   
 *   When Used:
 *     - Leader commits one NO_OP when elected to commit previous leader's entries
 *     - Maintains log continuity during leadership transitions
 *     - Forces followers to catch up without side effects
 *   
 *   Raft Requirement:
 *     New leaders cannot commit entries from previous terms directly.
 *     They must commit a NO_OP from their own term, which transitively
 *     commits previous entries (Raft paper Section 5.4.2)
 *   
 *   Example Scenario:
 *     Term 5: Leader A appends E10 but crashes before committing
 *     Term 6: Leader B elected, appends NO_OP as E11
 *     When E11 commits → E10 also becomes committed (piggyback commit)
 * 
 * DESIGN DECISION: uint8_t Storage
 * ---------------------------------
 * Using uint8_t (1 byte) instead of int (4 bytes) saves space in serialized log:
 * - 1000 entries × 3 bytes saved = 3 KB saved
 * - More cache-friendly (better performance)
 * - Sufficient range (255 types more than we'll ever need)
 * 
 * EXTENSIBILITY:
 * --------------
 * Future entry types might include:
 * - WORKER_JOIN (4): New worker registers with cluster
 * - WORKER_LEAVE (5): Worker gracefully shuts down
 * - CHECKPOINT (6): Snapshot of system state for log compaction
 * - CONFIG_CHANGE (7): Add/remove controller nodes (Raft extension)
 ******************************************************************************/

enum class LogEntryType : uint8_t {
    JOB_SUBMIT = 1,      // New backtest job submitted
    JOB_COMPLETE = 2,    // Backtest job finished
    NO_OP = 3            // Leader election marker (no side effect)
};

/*******************************************************************************
 * SECTION 2.2: LogEntry Structure
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * Represents a single command in the Raft replicated log. Each entry is a
 * unit of replication - it either gets replicated to all nodes or none.
 * 
 * FIELD DESCRIPTIONS:
 * -------------------
 * 
 * term (uint64_t):
 *   The leader's term when this entry was created.
 *   
 *   Critical for Raft correctness:
 *   - Identifies which leader created this entry
 *   - Used in conflict resolution (higher term wins)
 *   - Ensures stale leaders can't corrupt the log
 *   
 *   Example: If entry has term 5, it was created by leader in term 5
 * 
 * index (uint64_t):
 *   Position of this entry in the log (1-indexed).
 *   
 *   Invariant: index is strictly monotonic within a log
 *   - entries_[0] has index 1
 *   - entries_[1] has index 2
 *   - entries_[n] has index n+1
 *   
 *   Why 1-indexed?
 *   - Raft paper convention (makes math cleaner)
 *   - index 0 represents "before the log starts"
 *   - Simplifies edge cases in replication logic
 * 
 * type (LogEntryType):
 *   What kind of command this entry represents (job submit, complete, or no-op).
 *   Determines how the controller state machine interprets the data field.
 * 
 * data (std::vector<uint8_t>):
 *   Serialized command payload.
 *   
 *   Why vector<uint8_t> instead of string?
 *   - Binary data support (efficient serialization)
 *   - No null-terminator issues
 *   - Flexible size (empty for NO_OP, large for job data)
 *   
 *   Typical Sizes:
 *   - NO_OP: 0 bytes
 *   - JOB_SUBMIT: 50-500 bytes (depends on symbol count)
 *   - JOB_COMPLETE: 100-1000 bytes (depends on result size)
 *   
 *   Memory Management:
 *   - Vector owns the data (automatic cleanup)
 *   - Copy semantics for replication (each node has own copy)
 *   - Move semantics for efficiency (when transferring ownership)
 * 
 * CONSTRUCTORS:
 * -------------
 * 
 * Default Constructor:
 *   Creates empty entry with term=0, index=0, type=NO_OP, empty data.
 *   Used for vector resizing and uninitialized storage.
 * 
 * Parameterized Constructor:
 *   Creates entry with specified values.
 *   Used when leader appends new entry to log.
 *   
 *   Example:
 *     std::vector<uint8_t> job_data = serialize_job(job);
 *     LogEntry entry(current_term, next_index, LogEntryType::JOB_SUBMIT, job_data);
 *     log.append(entry);
 * 
 * MEMORY LAYOUT:
 * --------------
 * sizeof(LogEntry) ≈ 40 bytes (64-bit system) + data.size()
 * - term: 8 bytes
 * - index: 8 bytes
 * - type: 1 byte
 * - padding: 7 bytes (alignment)
 * - vector control: 24 bytes (pointer, size, capacity)
 * - data: variable
 * 
 * For typical job submit (200 bytes data):
 * Total size = 40 + 200 = 240 bytes
 * 
 * SERIALIZATION FORMAT (for disk persistence):
 * --------------------------------------------
 * Binary layout for efficient storage and loading:
 * 
 * [term: 8 bytes][index: 8 bytes][type: 1 byte][data_size: 4 bytes][data: variable]
 * 
 * Example (hexadecimal):
 * 05 00 00 00 00 00 00 00  <- term = 5
 * 0A 00 00 00 00 00 00 00  <- index = 10
 * 01                        <- type = JOB_SUBMIT
 * C8 00 00 00               <- data_size = 200 bytes
 * [200 bytes of job data]
 * 
 * COMPARISON SEMANTICS:
 * ---------------------
 * Two entries are considered "matching" if they have the same index and term.
 * This is fundamental to Raft's Log Matching Property:
 * 
 * If two entries in different logs have the same index and term:
 * → They contain the same command
 * → All preceding entries are identical
 * 
 * This property enables efficient consistency checks without comparing
 * entire logs (just check last index/term).
 ******************************************************************************/

struct LogEntry {
    uint64_t term;              // Leader term when entry created
    uint64_t index;             // Position in log (1-indexed)
    LogEntryType type;          // Command type
    std::vector<uint8_t> data;  // Serialized command payload
    
    // Default constructor - creates empty NO_OP entry
    LogEntry() : term(0), index(0), type(LogEntryType::NO_OP) {}
    
    // Parameterized constructor - creates entry with specified values
    // Params:
    //   t: Term number (leader's current term)
    //   idx: Log index (must be last_index + 1)
    //   tp: Entry type (JOB_SUBMIT, JOB_COMPLETE, or NO_OP)
    //   d: Serialized command data (can be empty for NO_OP)
    LogEntry(uint64_t t, uint64_t idx, LogEntryType tp, const std::vector<uint8_t>& d)
        : term(t), index(idx), type(tp), data(d) {}
};

/*******************************************************************************
 * SECTION 2.3: RaftLog Class - Overview
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * RaftLog is the core data structure that maintains:
 * 1. Replicated command log (entries_)
 * 2. Persistent state required by Raft (current_term_, voted_for_)
 * 3. Disk persistence for crash recovery
 * 4. Thread-safe access for concurrent RPC handlers
 * 
 * ARCHITECTURAL ROLE:
 * -------------------
 * Every controller node has exactly one RaftLog instance. This log is the
 * single source of truth for:
 * - What jobs have been submitted
 * - What order jobs should be processed
 * - Which term the node is in (for leader election)
 * - Who the node voted for (prevents double-voting)
 * 
 * Controller State Machine Relationship:
 * 
 * ┌─────────────────────────────────────────────┐
 * │ Controller State Machine (Job Scheduler)    │
 * │ - Reads committed entries from RaftLog      │
 * │ - Applies entries to maintain job queue     │
 * │ - Does NOT directly modify log              │
 * └─────────────────────────────────────────────┘
 *                    ↑ Reads
 *                    │
 * ┌─────────────────────────────────────────────┐
 * │ RaftLog (This Class)                        │
 * │ - Stores entries in memory and on disk      │
 * │ - Manages term and vote persistence         │
 * │ - Provides thread-safe access               │
 * └─────────────────────────────────────────────┘
 *                    ↑ Writes
 *                    │
 * ┌─────────────────────────────────────────────┐
 * │ Raft Consensus Module                       │
 * │ - Leader: appends new entries               │
 * │ - Follower: replicates from leader          │
 * │ - Candidate: manages elections              │
 * └─────────────────────────────────────────────┘
 * 
 * PERSISTENCE GUARANTEE (Section 3):
 * ----------------------------------
 * RaftLog MUST persist three pieces of state to disk:
 * 1. current_term_: Ensures node doesn't regress to old term after crash
 * 2. voted_for_: Prevents voting twice in same term (split-brain safety)
 * 3. entries_: Ensures committed jobs aren't lost
 * 
 * Crash Recovery Scenario:
 * 
 * Before Crash:
 *   Term: 5, Voted for: Node B, Log: [E1, E2, E3]
 * 
 * During Crash:
 *   Power failure → Memory cleared
 * 
 * After Restart:
 *   load_from_disk() → Restore: Term 5, Voted for B, Log [E1, E2, E3]
 *   Node rejoins cluster with correct state
 * 
 * THREAD SAFETY DESIGN:
 * ---------------------
 * Multiple threads access RaftLog concurrently:
 * - RPC handler threads (AppendEntries, RequestVote)
 * - Leader election timer thread
 * - Heartbeat sender thread
 * - Client request handler thread
 * 
 * Protection Strategy:
 * - Every public method acquires mutex_ lock
 * - Lock held for entire operation (no partial updates visible)
 * - Lock released automatically (RAII via std::lock_guard)
 * 
 * Example Thread Interleaving (Safe):
 * 
 * Thread A (AppendEntries RPC):
 *   lock(mutex_)
 *   append(entry)
 *   unlock(mutex_)
 * 
 * Thread B (Leader Election):
 *   lock(mutex_)  <- Blocks until Thread A releases
 *   increment_term()
 *   unlock(mutex_)
 * 
 * PERFORMANCE CONSIDERATIONS:
 * ---------------------------
 * - Log stored in-memory (fast access, O(1) append/lookup)
 * - Disk writes batched when possible (reduce I/O overhead)
 * - Mutex contention low in normal operation (reads don't modify state)
 * - Log compaction needed for long-running systems (not in initial version)
 ******************************************************************************/

class RaftLog {
private:
    // IN-MEMORY LOG STORAGE
    // Stores all log entries in chronological order
    // entries_[0] corresponds to log index 1
    // entries_[i] has index i+1
    std::vector<LogEntry> entries_;
    
    // DISK PERSISTENCE PATH
    // File path where log is persisted for crash recovery
    // Example: "/var/raft/controller_node_1.log"
    // Contains serialized entries_, current_term_, and voted_for_
    std::string log_file_;
    
    // THREAD SAFETY MUTEX
    // Protects all member variables from concurrent access
    // Mutable: Allows locking in const methods (for thread-safe reads)
    mutable std::mutex mutex_;
    
    // PERSISTENT STATE (Raft Paper Figure 2)
    
    // Current term number (monotonically increasing)
    // Updated on: receiving higher term, starting election
    // Used for: detecting stale messages, resolving conflicts
    uint64_t current_term_;
    
    // Candidate ID that received vote in current term
    // Value 0 means no vote cast yet this term
    // Critical for: preventing double-voting, ensuring single leader per term
    uint64_t voted_for_;
    
    // PERSISTENCE METHODS
    
    // Write current state to disk
    // Returns: true on success, false on I/O error
    // Called after: term update, vote cast, entry append
    // 
    // Failure Implications:
    //   If persist fails, node should NOT acknowledge RPC
    //   Otherwise, crash recovery may violate Raft invariants
    bool persist_to_disk();
    
    // Restore state from disk
    // Returns: true if file exists and loaded successfully, false otherwise
    // Called: During controller initialization (constructor)
    // 
    // Behavior on Missing File:
    //   Not an error - node may be starting for first time
    //   Initialize with term=0, voted_for=0, empty log
    bool load_from_disk();
    
public:
    // CONSTRUCTOR
    // Params:
    //   log_file: Path to persistent storage file
    // 
    // Behavior:
    //   1. Set log_file_ path
    //   2. Attempt to load existing state from disk
    //   3. If load fails (new node), initialize with empty state
    //   4. Ready to participate in Raft protocol
    // 
    // Example:
    //   RaftLog log("/var/raft/controller_1.log");
    explicit RaftLog(const std::string& log_file);
    
    /***************************************************************************
     * SECTION 4.1: Log Append Operations (Leader Path)
     **************************************************************************/
    
    // APPEND: Add single entry to log
    // Params:
    //   entry: Log entry to append (must have index = last_log_index() + 1)
    // 
    // Preconditions:
    //   - entry.term == current_term_ (leader only appends entries in its term)
    //   - entry.index == last_log_index() + 1 (sequential indexing)
    // 
    // Thread Safety: Acquires exclusive lock
    // Persistence: Writes to disk after appending
    // 
    // When Called:
    //   - Leader receives new job submission from client
    //   - Leader appends NO_OP after election
    // 
    // Example:
    //   uint64_t next_idx = log.last_log_index() + 1;
    //   LogEntry entry(log.get_current_term(), next_idx, 
    //                  LogEntryType::JOB_SUBMIT, job_data);
    //   log.append(entry);
    void append(const LogEntry& entry);
    
    // APPEND BATCH: Add multiple entries atomically
    // Params:
    //   entries: Vector of entries to append (must have sequential indexes)
    // 
    // Advantage over multiple append() calls:
    //   - Single disk write (more efficient)
    //   - Atomic: All entries added or none
    //   - Reduces mutex contention
    // 
    // When Called:
    //   - Follower receives AppendEntries RPC with multiple entries
    //   - Leader batch-commits multiple client requests
    // 
    // Example:
    //   std::vector<LogEntry> batch;
    //   for (int i = 0; i < 10; ++i) {
    //       batch.push_back(create_entry(i));
    //   }
    //   log.append_batch(batch);
    void append_batch(const std::vector<LogEntry>& entries);
    
    /***************************************************************************
     * SECTION 4.2: Log Truncation (Follower Conflict Resolution)
     **************************************************************************/
    
    // TRUNCATE FROM: Delete entries starting at specified index
    // Params:
    //   index: Log index to start truncation (inclusive)
    // 
    // Behavior:
    //   Removes entries[index-1] through entries[last_index]
    //   Example: truncate_from(5) with log [E1,E2,E3,E4,E5,E6]
    //            Result: [E1,E2,E3,E4]
    // 
    // When Called:
    //   Follower discovers conflict with leader's log:
    //   
    //   Scenario:
    //     Follower log: [E1(term1), E2(term1), E3(term2)]
    //     Leader log:   [E1(term1), E2(term1), E3(term3)]
    //     
    //     Conflict at index 3 (different terms)
    //     Follower must: truncate_from(3), then append leader's E3
    // 
    // Critical for Consistency:
    //   This is how Raft resolves log divergence after leader changes.
    //   Without truncation, nodes would have permanently inconsistent logs.
    // 
    // Thread Safety: Acquires exclusive lock
    // Persistence: Writes to disk after truncating
    void truncate_from(uint64_t index);
    
    /***************************************************************************
     * SECTION 4.3: Log Queries (Read-Only Operations)
     **************************************************************************/
    
    // SIZE: Get number of entries in log
    // Returns: Count of entries (0 for empty log)
    // 
    // Note: size() returns number of entries, not highest index
    //       size() == last_log_index() only if entries start at index 1
    size_t size() const;
    
    // LAST LOG INDEX: Get index of most recent entry
    // Returns: Highest index in log, or 0 if log is empty
    // 
    // Used for:
    //   - Determining if candidate's log is up-to-date (leader election)
    //   - Calculating next index to append (next_idx = last_log_index() + 1)
    //   - Checking if follower is caught up with leader
    // 
    // Example:
    //   Log: [E1, E2, E3] where E3.index = 3
    //   last_log_index() returns 3
    uint64_t last_log_index() const;
    
    // LAST LOG TERM: Get term of most recent entry
    // Returns: Term of last entry, or 0 if log is empty
    // 
    // Used for:
    //   - Leader election vote comparison
    //   - AppendEntries consistency check
    // 
    // Example:
    //   Log: [E1(term1), E2(term1), E3(term2)]
    //   last_log_term() returns 2
    uint64_t last_log_term() const;
    
    // AT: Get entry at specific index
    // Params:
    //   index: Log index to retrieve (1-indexed)
    // Returns: Reference to entry at that index
    // Throws: std::out_of_range if index invalid
    // 
    // Used for:
    //   - Reading entry to execute (state machine application)
    //   - Consistency checks (matching prev_log_term in AppendEntries)
    // 
    // Example:
    //   const LogEntry& entry = log.at(5);
    //   if (entry.type == LogEntryType::JOB_SUBMIT) {
    //       execute_job(entry.data);
    //   }
    const LogEntry& at(uint64_t index) const;
    
    // GET ENTRIES FROM: Retrieve range of entries starting at index
    // Params:
    //   start_index: First index to include (inclusive)
    // Returns: Vector containing entries from start_index to end of log
    // 
    // Used for:
    //   - Leader sending entries to follower (AppendEntries RPC)
    //   - Follower catching up after network partition
    // 
    // Example:
    //   Leader log: [E1, E2, E3, E4, E5]
    //   Follower has: [E1, E2]
    //   Leader calls: get_entries_from(3) → [E3, E4, E5]
    //   Leader sends these to follower
    std::vector<LogEntry> get_entries_from(uint64_t start_index) const;
    
    /***************************************************************************
     * SECTION 4.4: Term Management
     **************************************************************************/
    
    // GET CURRENT TERM: Retrieve node's current term number
    // Returns: Current term (starts at 0, increases with each election)
    // 
    // Thread Safety: Const method, acquires read lock
    uint64_t get_current_term() const;
    
    // SET CURRENT TERM: Update term to specific value
    // Params:
    //   term: New term number (must be >= current term)
    // 
    // When Called:
    //   - Discover higher term from another node's RPC
    //   - Revert to follower state (clear voted_for_)
    // 
    // Raft Rule (Safety):
    //   If term decreases, consensus is violated. This should never happen
    //   in correct implementation. Consider adding assertion:
    //   assert(term >= current_term_);
    // 
    // Side Effect: Clears voted_for_ (new term = new voting round)
    // Persistence: Writes to disk
    void set_current_term(uint64_t term);
    
    // INCREMENT TERM: Move to next term (used in leader election)
    // 
    // When Called:
    //   - Node starts election (follower → candidate transition)
    //   - Equivalent to: set_current_term(get_current_term() + 1)
    // 
    // Side Effect: Clears voted_for_
    // Persistence: Writes to disk
    void increment_term();
    
    /***************************************************************************
     * SECTION 4.4: Vote Management (Leader Election)
     **************************************************************************/
    
    // GET VOTED FOR: Retrieve candidate ID that received vote this term
    // Returns: Candidate ID, or 0 if no vote cast
    // 
    // Used for:
    //   - Checking if already voted (reject duplicate vote requests)
    //   - Recovery after crash (remember who we voted for)
    uint64_t get_voted_for() const;
    
    // SET VOTED FOR: Record vote for specific candidate
    // Params:
    //   candidate_id: ID of node receiving vote
    // 
    // Preconditions:
    //   - voted_for_ == 0 (haven't voted this term)
    //   - OR voted_for_ == candidate_id (idempotent retry)
    // 
    // Raft Safety Property:
    //   Each node votes at most once per term.
    //   Violating this can cause split-brain (two leaders elected).
    // 
    // When Called:
    //   - Grant RequestVote RPC from candidate
    //   - Vote for self when starting election
    // 
    // Persistence: Writes to disk (critical for safety)
    void set_voted_for(uint64_t candidate_id);
    
    // CLEAR VOTE: Reset vote for new term
    // 
    // When Called:
    //   - Automatically by set_current_term() / increment_term()
    //   - Not typically called directly
    // 
    // Effect: Sets voted_for_ = 0
    void clear_vote();
    
    /***************************************************************************
     * SECTION 4.5: Persistence Operations
     **************************************************************************/
    
    // SAVE: Explicitly persist current state to disk
    // Returns: true on success, false on I/O error
    // 
    // Normally Not Needed:
    //   Other methods (append, set_current_term, etc.) call persist_to_disk()
    //   automatically. This method exists for explicit checkpointing.
    // 
    // When to Use:
    //   - Forced sync before critical operations
    //   - Testing persistence logic
    //   - Manual checkpoint before shutdown
    bool save();
    
    // LOAD: Explicitly reload state from disk
    // Returns: true if loaded successfully, false if file missing/corrupt
    // 
    // When to Use:
    //   - Not typically needed (constructor loads automatically)
    //   - Testing recovery scenarios
    //   - Reloading after log compaction
    bool load();
};

} // namespace raft
} // namespace backtesting

/*******************************************************************************
 * SECTION 3: PERSISTENT STATE (Raft Paper Section 5.2)
 *******************************************************************************
 * 
 * 3.1 WHY PERSISTENCE MATTERS
 * ----------------------------
 * 
 * Problem: What if a controller node crashes and restarts?
 * 
 * Scenario Without Persistence:
 *   1. Node A is leader in term 5
 *   2. Node A accepts job submissions, appends to log
 *   3. Node A replicates entries to majority (committed)
 *   4. Node A CRASHES before telling client "committed"
 *   5. Node A restarts with empty log (memory cleared)
 *   6. Node A believes it's in term 0 with empty log
 *   7. VIOLATION: Committed entries lost, consensus broken
 * 
 * Solution With Persistence:
 *   1-4. Same as above
 *   5. Node A restarts, loads log from disk
 *   6. Node A recovers: term 5, committed entries intact
 *   7. Node A rejoins cluster with correct state
 * 
 * 
 * 3.2 WHAT MUST SURVIVE CRASHES
 * ------------------------------
 * 
 * Per Raft paper, three fields MUST be persistent:
 * 
 * 1. current_term_ (8 bytes):
 *    Why: Prevents node from regressing to old term
 *    
 *    Failure if lost:
 *      Node restarts believing it's in term 0, accepts stale RPCs
 *      from old leaders, potentially overwriting newer entries
 * 
 * 2. voted_for_ (8 bytes):
 *    Why: Prevents double-voting in same term
 *    
 *    Failure if lost:
 *      Node A votes for candidate B, crashes, restarts
 *      Node A (forgetting it voted) votes for candidate C
 *      Both B and C receive majority votes → SPLIT-BRAIN
 * 
 * 3. entries_ (variable size):
 *    Why: Ensures committed entries never lost
 *    
 *    Failure if lost:
 *      Node has committed entry (replicated to majority)
 *      Node crashes, restarts with empty log
 *      Entry is still committed cluster-wide but lost locally
 *      State machine divergence occurs
 * 
 * 
 * 3.3 DISK FORMAT AND RECOVERY
 * -----------------------------
 * 
 * File Layout (binary format):
 * 
 * [MAGIC: 4 bytes] "RAFT"
 * [VERSION: 4 bytes] 1
 * [CURRENT_TERM: 8 bytes]
 * [VOTED_FOR: 8 bytes]
 * [NUM_ENTRIES: 8 bytes]
 * [ENTRY_1: variable]
 * [ENTRY_2: variable]
 * ...
 * [ENTRY_N: variable]
 * [CHECKSUM: 4 bytes] CRC32
 * 
 * Each entry format:
 * [TERM: 8 bytes]
 * [INDEX: 8 bytes]
 * [TYPE: 1 byte]
 * [DATA_SIZE: 4 bytes]
 * [DATA: variable]
 * 
 * Example File (20 entries, ~10KB):
 * 
 * 52 41 46 54                      <- "RAFT" magic
 * 01 00 00 00                      <- version 1
 * 05 00 00 00 00 00 00 00         <- term 5
 * 02 00 00 00 00 00 00 00         <- voted for node 2
 * 14 00 00 00 00 00 00 00         <- 20 entries
 * [20 serialized entries]
 * A3 F2 1B 9C                      <- CRC32 checksum
 * 
 * CRASH RECOVERY PROCEDURE:
 * -------------------------
 * 
 * Step 1: Constructor calls load_from_disk()
 * 
 * Step 2: Verify file exists and is readable
 *   if (file missing) {
 *       // First time startup - initialize fresh
 *       current_term_ = 0;
 *       voted_for_ = 0;
 *       entries_.clear();
 *       return true;  // Not an error
 *   }
 * 
 * Step 3: Read and validate magic/version
 *   if (magic != "RAFT" || version != 1) {
 *       LOG_ERROR("Corrupted log file");
 *       return false;  // Cannot recover
 *   }
 * 
 * Step 4: Read persistent state
 *   current_term_ = read_uint64();
 *   voted_for_ = read_uint64();
 *   num_entries = read_uint64();
 * 
 * Step 5: Deserialize all entries
 *   for (i = 0; i < num_entries; i++) {
 *       entry = deserialize_entry();
 *       entries_.push_back(entry);
 *   }
 * 
 * Step 6: Verify checksum
 *   computed_crc = compute_crc32(data);
 *   stored_crc = read_uint32();
 *   if (computed_crc != stored_crc) {
 *       LOG_ERROR("Checksum mismatch - data corrupted");
 *       return false;
 *   }
 * 
 * Step 7: Recovery complete
 *   LOG_INFO("Recovered term=%lu, voted_for=%lu, entries=%lu",
 *            current_term_, voted_for_, entries_.size());
 *   return true;
 * 
 * DISK WRITE STRATEGY:
 * --------------------
 * 
 * Option 1: Synchronous Write (Safest, Slowest)
 *   persist_to_disk() {
 *       write_all_data();
 *       fsync();  // Force data to physical disk
 *       return true;
 *   }
 *   Latency: ~5-10ms per write (HDD), ~0.1ms (SSD)
 * 
 * Option 2: Asynchronous Write (Faster, Less Safe)
 *   persist_to_disk() {
 *       write_all_data();
 *       // OS buffers write, returns immediately
 *       return true;
 *   }
 *   Latency: ~0.01ms (memory write)
 *   Risk: Data lost if system crashes before OS flushes buffers
 * 
 * Option 3: Batched Sync (Best Balance)
 *   - Collect multiple updates in memory
 *   - Write batch every 100ms or when threshold reached
 *   - Single fsync() for entire batch
 *   Latency: Amortized ~1ms per update
 *   Risk: Up to 100ms of data lost on crash
 * 
 * PROJECT RECOMMENDATION:
 *   Use Option 1 (synchronous write) initially for correctness.
 *   Optimize to Option 3 after system is working correctly.
 *   
 *   Per project plan benchmarks (Section 4.2):
 *   - Expected: 10 jobs/second throughput
 *   - Option 1 overhead: 10ms × 10 = 100ms/sec = 10% overhead (acceptable)
 ******************************************************************************/

/*******************************************************************************
 * SECTION 5: INTEGRATION WITH CONTROLLER
 *******************************************************************************
 * 
 * 5.1 LEADER JOB SCHEDULING FLOW
 * -------------------------------
 * 
 * Complete workflow when leader controller receives job submission:
 * 
 * ```cpp
 * // Controller's job submission handler (runs on leader)
 * void Controller::handle_client_job_submit(const JobRequest& job) {
 *     // Step 1: Serialize job into log entry data
 *     std::vector<uint8_t> job_data = serialize_job(job);
 *     
 *     // Step 2: Create log entry with current term
 *     uint64_t next_index = raft_log_.last_log_index() + 1;
 *     uint64_t current_term = raft_log_.get_current_term();
 *     
 *     LogEntry entry(
 *         current_term,
 *         next_index,
 *         LogEntryType::JOB_SUBMIT,
 *         job_data
 *     );
 *     
 *     // Step 3: Append to local log
 *     raft_log_.append(entry);
 *     
 *     // Step 4: Replicate to followers via AppendEntries RPC
 *     int acks = 1;  // Leader counts as one acknowledgment
 *     for (auto& follower : followers_) {
 *         if (follower.send_append_entries(entry)) {
 *             acks++;
 *         }
 *     }
 *     
 *     // Step 5: Check if replicated to majority (2 of 3 nodes)
 *     if (acks >= 2) {
 *         // Entry is committed!
 *         
 *         // Step 6: Apply to state machine (assign job to worker)
 *         apply_committed_entry(entry);
 *         
 *         // Step 7: Respond to client
 *         send_success_response(job.id);
 *     } else {
 *         // Failed to replicate - retry or report error
 *         send_error_response(job.id, "Replication failed");
 *     }
 * }
 * ```
 * 
 * 
 * 5.2 FOLLOWER LOG REPLICATION FLOW
 * ----------------------------------
 * 
 * Follower receives AppendEntries RPC from leader:
 * 
 * ```cpp
 * // Follower's AppendEntries RPC handler
 * bool Controller::handle_append_entries(const AppendEntriesRPC& rpc) {
 *     std::lock_guard<std::mutex> lock(raft_mutex_);
 *     
 *     // Step 1: Check if RPC is from current or newer term
 *     if (rpc.term < raft_log_.get_current_term()) {
 *         // Stale RPC from old leader - reject
 *         return false;
 *     }
 *     
 *     // Step 2: Update term if higher
 *     if (rpc.term > raft_log_.get_current_term()) {
 *         raft_log_.set_current_term(rpc.term);
 *         convert_to_follower();  // Step down if we were candidate/leader
 *     }
 *     
 *     // Step 3: Check log consistency (prev_log_index and prev_log_term match)
 *     if (rpc.prev_log_index > 0) {
 *         if (rpc.prev_log_index > raft_log_.last_log_index()) {
 *             // Our log is too short - need earlier entries first
 *             return false;
 *         }
 *         
 *         const LogEntry& prev_entry = raft_log_.at(rpc.prev_log_index);
 *         if (prev_entry.term != rpc.prev_log_term) {
 *             // Conflict! Entry at prev_log_index has different term
 *             // Truncate our log from this point
 *             raft_log_.truncate_from(rpc.prev_log_index);
 *             // After truncation, we still don't match, return false
 *             // Leader will retry with earlier entries
 *             return false;
 *         }
 *     }
 *     
 *     // Step 4: Append new entries
 *     if (!rpc.entries.empty()) {
 *         raft_log_.append_batch(rpc.entries);
 *     }
 *     
 *     // Step 5: Update commit index if leader has committed more entries
 *     if (rpc.leader_commit > commit_index_) {
 *         commit_index_ = std::min(rpc.leader_commit, 
 *                                   raft_log_.last_log_index());
 *         
 *         // Apply newly committed entries to state machine
 *         apply_committed_entries();
 *     }
 *     
 *     // Step 6: Success - log is now consistent with leader
 *     reset_election_timer();  // Leader is alive
 *     return true;
 * }
 * ```
 * 
 * 
 * 5.3 CRASH RECOVERY FLOW
 * ------------------------
 * 
 * Node crashes and restarts:
 * 
 * ```cpp
 * // Controller initialization after crash
 * int main() {
 *     try {
 *         // Step 1: Create RaftLog (loads persistent state)
 *         RaftLog raft_log("/var/raft/controller_1.log");
 *         
 *         // Step 2: Check recovered state
 *         uint64_t recovered_term = raft_log.get_current_term();
 *         uint64_t recovered_entries = raft_log.size();
 *         
 *         LOG_INFO("Recovered from crash: term=%lu, entries=%lu",
 *                  recovered_term, recovered_entries);
 *         
 *         // Step 3: Initialize controller with recovered log
 *         Controller controller(raft_log);
 *         
 *         // Step 4: Start as follower (never assume we're still leader)
 *         controller.convert_to_follower();
 *         
 *         // Step 5: Join cluster (send RPCs to discover current leader)
 *         controller.rejoin_cluster();
 *         
 *         // Step 6: Catch up with current state
 *         // If we're behind, leader will send missing entries via AppendEntries
 *         // If we're ahead (impossible if we followed Raft correctly), 
 *         // leader's log will eventually match ours
 *         
 *         // Step 7: Resume normal operation
 *         controller.run();
 *         
 *     } catch (const std::exception& e) {
 *         LOG_FATAL("Failed to recover: %s", e.what());
 *         return 1;
 *     }
 *     
 *     return 0;
 * }
 * ```
 * 
 * 
 * 5.4 LEADER ELECTION FLOW
 * -------------------------
 * 
 * Follower detects leader failure and starts election:
 * 
 * ```cpp
 * // Election timeout fired - no heartbeat from leader
 * void Controller::start_election() {
 *     std::lock_guard<std::mutex> lock(raft_mutex_);
 *     
 *     // Step 1: Increment term (entering new election round)
 *     raft_log_.increment_term();  // Also clears voted_for_
 *     uint64_t election_term = raft_log_.get_current_term();
 *     
 *     // Step 2: Transition to candidate state
 *     state_ = NodeState::CANDIDATE;
 *     
 *     // Step 3: Vote for self
 *     raft_log_.set_voted_for(my_node_id_);
 *     int votes_received = 1;
 *     
 *     // Step 4: Request votes from all other nodes
 *     RequestVoteRPC vote_request;
 *     vote_request.term = election_term;
 *     vote_request.candidate_id = my_node_id_;
 *     vote_request.last_log_index = raft_log_.last_log_index();
 *     vote_request.last_log_term = raft_log_.last_log_term();
 *     
 *     for (auto& peer : peers_) {
 *         if (peer.request_vote(vote_request)) {
 *             votes_received++;
 *         }
 *     }
 *     
 *     // Step 5: Check if won election (majority votes)
 *     int majority = (total_nodes_ / 2) + 1;  // 2 of 3 = 2
 *     
 *     if (votes_received >= majority) {
 *         // WON ELECTION!
 *         
 *         // Step 6: Become leader
 *         state_ = NodeState::LEADER;
 *         LOG_INFO("Elected leader for term %lu", election_term);
 *         
 *         // Step 7: Append NO_OP to commit previous leader's entries
 *         //         (Raft paper Section 5.4.2 requirement)
 *         uint64_t next_index = raft_log_.last_log_index() + 1;
 *         LogEntry noop(election_term, next_index, 
 *                      LogEntryType::NO_OP, {});
 *         raft_log_.append(noop);
 *         
 *         // Step 8: Start sending heartbeats to assert authority
 *         start_heartbeat_timer();
 *         
 *         // Step 9: Initialize follower tracking state
 *         for (auto& peer : peers_) {
 *             peer.next_index = raft_log_.last_log_index() + 1;
 *             peer.match_index = 0;
 *         }
 *     } else {
 *         // Lost election - revert to follower
 *         state_ = NodeState::FOLLOWER;
 *         // Wait for new election timeout or hear from winner
 *     }
 * }
 * 
 * // Other node receives RequestVote RPC
 * bool Controller::handle_request_vote(const RequestVoteRPC& rpc) {
 *     std::lock_guard<std::mutex> lock(raft_mutex_);
 *     
 *     // Step 1: Check term
 *     if (rpc.term < raft_log_.get_current_term()) {
 *         // Stale request from old term - reject
 *         return false;
 *     }
 *     
 *     if (rpc.term > raft_log_.get_current_term()) {
 *         // Newer term - update and step down
 *         raft_log_.set_current_term(rpc.term);
 *         convert_to_follower();
 *     }
 *     
 *     // Step 2: Check if already voted this term
 *     uint64_t voted_for = raft_log_.get_voted_for();
 *     bool already_voted = (voted_for != 0 && voted_for != rpc.candidate_id);
 *     
 *     if (already_voted) {
 *         // Already voted for someone else - reject
 *         return false;
 *     }
 *     
 *     // Step 3: Check if candidate's log is at least as up-to-date
 *     uint64_t my_last_index = raft_log_.last_log_index();
 *     uint64_t my_last_term = raft_log_.last_log_term();
 *     
 *     bool log_ok = (rpc.last_log_term > my_last_term) ||
 *                   (rpc.last_log_term == my_last_term && 
 *                    rpc.last_log_index >= my_last_index);
 *     
 *     if (!log_ok) {
 *         // Candidate's log is behind - reject
 *         // (prevents electing leader that would lose committed entries)
 *         return false;
 *     }
 *     
 *     // Step 4: Grant vote
 *     raft_log_.set_voted_for(rpc.candidate_id);
 *     reset_election_timer();  // Don't start own election
 *     return true;
 * }
 * ```
 ******************************************************************************/

/*******************************************************************************
 * SECTION 6: USAGE EXAMPLES
 *******************************************************************************
 * 
 * EXAMPLE 6.1: Controller Initialization
 * ---------------------------------------
 * 
 * #include "raft_log.h"
 * #include <iostream>
 * 
 * using namespace backtesting::raft;
 * 
 * int main() {
 *     // Initialize log with persistence file
 *     RaftLog log("/var/raft/controller_node_1.log");
 *     
 *     // Check if this is first startup or recovery
 *     if (log.size() == 0 && log.get_current_term() == 0) {
 *         std::cout << "First startup - initializing fresh log\n";
 *     } else {
 *         std::cout << "Recovered from crash:\n";
 *         std::cout << "  Term: " << log.get_current_term() << "\n";
 *         std::cout << "  Entries: " << log.size() << "\n";
 *         std::cout << "  Voted for: " << log.get_voted_for() << "\n";
 *     }
 *     
 *     return 0;
 * }
 * 
 * 
 * EXAMPLE 6.2: Leader Submitting Job
 * -----------------------------------
 * 
 * void Leader::submit_job(const BacktestJob& job) {
 *     // Serialize job data
 *     std::vector<uint8_t> job_data;
 *     // ... serialize job into job_data ...
 *     
 *     // Create log entry
 *     uint64_t term = raft_log_.get_current_term();
 *     uint64_t index = raft_log_.last_log_index() + 1;
 *     
 *     LogEntry entry(term, index, LogEntryType::JOB_SUBMIT, job_data);
 *     
 *     // Append to log (persists automatically)
 *     raft_log_.append(entry);
 *     
 *     std::cout << "Appended job to log at index " << index 
 *               << " term " << term << "\n";
 *     
 *     // Now replicate to followers...
 *     replicate_to_followers();
 * }
 * 
 * 
 * EXAMPLE 6.3: Follower Replicating Entries
 * ------------------------------------------
 * 
 * bool Follower::handle_append_entries(
 *     uint64_t leader_term,
 *     uint64_t prev_log_index,
 *     uint64_t prev_log_term,
 *     const std::vector<LogEntry>& new_entries
 * ) {
 *     // Check term
 *     if (leader_term < raft_log_.get_current_term()) {
 *         return false;  // Stale leader
 *     }
 *     
 *     // Update term if necessary
 *     if (leader_term > raft_log_.get_current_term()) {
 *         raft_log_.set_current_term(leader_term);
 *     }
 *     
 *     // Check consistency
 *     if (prev_log_index > 0) {
 *         if (prev_log_index > raft_log_.last_log_index()) {
 *             std::cout << "Log too short, need index " << prev_log_index << "\n";
 *             return false;
 *         }
 *         
 *         const LogEntry& prev_entry = raft_log_.at(prev_log_index);
 *         if (prev_entry.term != prev_log_term) {
 *             std::cout << "Conflict at index " << prev_log_index << "\n";
 *             std::cout << "  My term: " << prev_entry.term << "\n";
 *             std::cout << "  Leader's term: " << prev_log_term << "\n";
 *             
 *             // Truncate conflicting entries
 *             raft_log_.truncate_from(prev_log_index);
 *             return false;  // Leader will retry with earlier entries
 *         }
 *     }
 *     
 *     // Append new entries
 *     if (!new_entries.empty()) {
 *         raft_log_.append_batch(new_entries);
 *         std::cout << "Appended " << new_entries.size() << " entries\n";
 *     }
 *     
 *     return true;
 * }
 * 
 * 
 * EXAMPLE 6.4: Handling Log Conflicts
 * ------------------------------------
 * 
 * // Scenario: Follower has diverged from leader after network partition
 * void demonstrate_conflict_resolution() {
 *     RaftLog follower_log("/tmp/follower.log");
 *     
 *     // Follower's log (from old leader in term 2):
 *     // [E1(term1), E2(term1), E3(term2), E4(term2)]
 *     
 *     // New leader's log (term 3):
 *     // [E1(term1), E2(term1), E3(term3), E5(term3)]
 *     
 *     // Leader sends AppendEntries:
 *     // prev_log_index=2, prev_log_term=1 (E2 matches)
 *     // new_entries=[E3(term3), E5(term3)]
 *     
 *     uint64_t prev_log_index = 2;
 *     uint64_t prev_log_term = 1;
 *     
 *     // Check consistency at index 2
 *     const LogEntry& e2 = follower_log.at(prev_log_index);
 *     if (e2.term == prev_log_term) {
 *         std::cout << "Consistency check passed at index 2\n";
 *         
 *         // Now check if entry 3 conflicts
 *         if (follower_log.last_log_index() >= 3) {
 *             const LogEntry& e3 = follower_log.at(3);
 *             if (e3.term != 3) {
 *                 std::cout << "Conflict at index 3 (follower has term "
 *                           << e3.term << ", leader has term 3)\n";
 *                 
 *                 // Truncate from index 3 onwards
 *                 follower_log.truncate_from(3);
 *                 std::cout << "Truncated from index 3\n";
 *             }
 *         }
 *         
 *         // Now append leader's entries
 *         LogEntry e3_new(3, 3, LogEntryType::JOB_SUBMIT, {});
 *         LogEntry e5_new(3, 4, LogEntryType::JOB_SUBMIT, {});
 *         
 *         follower_log.append(e3_new);
 *         follower_log.append(e5_new);
 *         
 *         std::cout << "Log repaired: now matches leader\n";
 *         std::cout << "Final log size: " << follower_log.size() << "\n";
 *     }
 * }
 * 
 * 
 * EXAMPLE 6.5: Term and Vote Management in Election
 * --------------------------------------------------
 * 
 * void conduct_election(RaftLog& log, int my_id, 
 *                       const std::vector<int>& peer_ids) {
 *     // Start election
 *     std::cout << "Starting election\n";
 *     std::cout << "  Current term: " << log.get_current_term() << "\n";
 *     
 *     // Increment term (new election round)
 *     log.increment_term();
 *     uint64_t election_term = log.get_current_term();
 *     std::cout << "  New term: " << election_term << "\n";
 *     
 *     // Vote for self
 *     log.set_voted_for(my_id);
 *     std::cout << "  Voted for self (ID " << my_id << ")\n";
 *     
 *     int votes = 1;  // Count own vote
 *     
 *     // Simulate requesting votes from peers
 *     for (int peer_id : peer_ids) {
 *         // In real implementation, send RequestVote RPC
 *         // For demo, assume we get vote if peer hasn't voted yet
 *         bool granted = true;  // Simplified
 *         
 *         if (granted) {
 *             votes++;
 *             std::cout << "  Received vote from node " << peer_id << "\n";
 *         }
 *     }
 *     
 *     // Check for majority (2 out of 3 for our 3-node cluster)
 *     int majority = 2;
 *     if (votes >= majority) {
 *         std::cout << "WON ELECTION! (" << votes << " votes)\n";
 *         std::cout << "Now leader for term " << election_term << "\n";
 *     } else {
 *         std::cout << "Lost election (" << votes << " votes, needed " 
 *                   << majority << ")\n";
 *     }
 * }
 ******************************************************************************/

/*******************************************************************************
 * SECTION 7: COMMON PITFALLS & BEST PRACTICES
 *******************************************************************************
 * 
 * PITFALL 7.1: Forgetting to Persist After State Changes
 * -------------------------------------------------------
 * 
 * Problem:
 *   Modifying term or vote without calling persist_to_disk() means state
 *   is not durable. On crash, node recovers old state and violates invariants.
 * 
 * Example of Bug:
 *   void increment_term() {
 *       current_term_++;
 *       // BUG: Forgot to call persist_to_disk()!
 *   }
 *   
 *   Node increments term, crashes immediately
 *   On restart: loads old term from disk
 *   Result: Node may accept RPCs it should reject
 * 
 * Solution:
 *   ALWAYS persist after modifying persistent state:
 *   - After term update: persist_to_disk()
 *   - After vote: persist_to_disk()
 *   - After append: persist_to_disk()
 *   
 *   Better: Make persistence automatic in all setters (as this class does)
 * 
 * 
 * PITFALL 7.2: Not Handling Disk Write Failures
 * ----------------------------------------------
 * 
 * Problem:
 *   If persist_to_disk() fails (disk full, I/O error), continuing as if
 *   write succeeded violates safety. Node may acknowledge RPC but lose
 *   state on crash.
 * 
 * Example of Bug:
 *   void set_current_term(uint64_t term) {
 *       current_term_ = term;
 *       persist_to_disk();  // Ignores return value!
 *       // BUG: What if persist failed?
 *   }
 * 
 * Solution:
 *   Check return value and handle failure:
 *   
 *   void set_current_term(uint64_t term) {
 *       current_term_ = term;
 *       if (!persist_to_disk()) {
 *           // CRITICAL ERROR - cannot continue safely
 *           LOG_FATAL("Failed to persist term update");
 *           // Options:
 *           // 1. Crash immediately (safest)
 *           // 2. Revert term change and return error to caller
 *           // 3. Enter read-only mode (stop accepting writes)
 *           std::terminate();
 *       }
 *   }
 * 
 * 
 * PITFALL 7.3: Race Conditions Without Proper Locking
 * ----------------------------------------------------
 * 
 * Problem:
 *   Multiple threads accessing log without synchronization can cause:
 *   - Torn reads (reading half-updated state)
 *   - Lost updates (two threads append simultaneously)
 *   - Inconsistent state (term updated but vote not cleared)
 * 
 * Example of Bug:
 *   // Thread A (append entry)
 *   entries_.push_back(entry);  // No lock!
 *   
 *   // Thread B (read last entry) - CONCURRENT
 *   const LogEntry& last = entries_.back();  // Undefined behavior!
 * 
 * Solution:
 *   Use std::lock_guard in EVERY method:
 *   
 *   void append(const LogEntry& entry) {
 *       std::lock_guard<std::mutex> lock(mutex_);
 *       entries_.push_back(entry);
 *       persist_to_disk();
 *   }
 *   
 *   const LogEntry& at(uint64_t index) const {
 *       std::lock_guard<std::mutex> lock(mutex_);
 *       return entries_.at(index - 1);
 *   }
 * 
 * 
 * PITFALL 7.4: Off-By-One Errors with 1-Indexed Log
 * --------------------------------------------------
 * 
 * Problem:
 *   Log entries use 1-based indexing (Raft convention) but vector uses
 *   0-based indexing (C++ convention). Easy to mix up.
 * 
 * Example of Bug:
 *   // Logical index 5 → vector index 5 (WRONG!)
 *   const LogEntry& entry = entries_[logical_index];  
 *   // Should be: entries_[logical_index - 1]
 * 
 * Solution:
 *   Always use helper method at(uint64_t index) which handles conversion:
 *   
 *   const LogEntry& at(uint64_t index) const {
 *       if (index == 0 || index > entries_.size()) {
 *           throw std::out_of_range("Invalid log index");
 *       }
 *       return entries_[index - 1];  // Convert to 0-based
 *   }
 *   
 *   // Use it:
 *   const LogEntry& entry = log.at(5);  // Logical index 5
 * 
 * 
 * PITFALL 7.5: Not Validating Truncate Index
 * -------------------------------------------
 * 
 * Problem:
 *   Truncating at invalid index can corrupt log or cause crashes.
 * 
 * Example of Bug:
 *   void truncate_from(uint64_t index) {
 *       entries_.resize(index - 1);  // No validation!
 *       // What if index == 0? Underflow!
 *       // What if index > size()? No effect but wastes cycle
 *   }
 * 
 * Solution:
 *   Validate index before truncating:
 *   
 *   void truncate_from(uint64_t index) {
 *       std::lock_guard<std::mutex> lock(mutex_);
 *       
 *       if (index == 0) {
 *           LOG_ERROR("Cannot truncate from index 0");
 *           return;
 *       }
 *       
 *       if (index > entries_.size() + 1) {
 *           LOG_WARN("Truncate index beyond log end, no-op");
 *           return;
 *       }
 *       
 *       entries_.resize(index - 1);
 *       persist_to_disk();
 *   }
 * 
 * 
 * BEST PRACTICE 7.6: Atomic State Updates
 * ----------------------------------------
 * 
 * When updating multiple related fields, make changes atomic:
 * 
 * void start_new_term_and_vote(uint64_t new_term, uint64_t candidate_id) {
 *     std::lock_guard<std::mutex> lock(mutex_);
 *     
 *     // Update both atomically
 *     current_term_ = new_term;
 *     voted_for_ = candidate_id;
 *     
 *     // Single persist for both updates
 *     if (!persist_to_disk()) {
 *         // Rollback on failure
 *         LOG_ERROR("Persist failed, cannot update state");
 *         throw std::runtime_error("Persistence failure");
 *     }
 * }
 * 
 * 
 * BEST PRACTICE 7.7: Log State Validation
 * ----------------------------------------
 * 
 * Periodically validate log invariants in debug builds:
 * 
 * #ifdef DEBUG
 * void RaftLog::validate() const {
 *     std::lock_guard<std::mutex> lock(mutex_);
 *     
 *     // Check indexes are sequential
 *     for (size_t i = 0; i < entries_.size(); ++i) {
 *         assert(entries_[i].index == i + 1);
 *     }
 *     
 *     // Check terms are non-decreasing
 *     for (size_t i = 1; i < entries_.size(); ++i) {
 *         assert(entries_[i].term >= entries_[i-1].term);
 *     }
 *     
 *     // Check voted_for valid
 *     assert(voted_for_ >= 0);
 * }
 * #endif
 * 
 * 
 * BEST PRACTICE 7.8: Comprehensive Logging
 * -----------------------------------------
 * 
 * Log all significant events for debugging:
 * 
 * void append(const LogEntry& entry) {
 *     std::lock_guard<std::mutex> lock(mutex_);
 *     
 *     LOG_DEBUG("Appending entry: index=%lu, term=%lu, type=%d",
 *               entry.index, entry.term, (int)entry.type);
 *     
 *     entries_.push_back(entry);
 *     
 *     if (!persist_to_disk()) {
 *         LOG_ERROR("Failed to persist log after append");
 *         throw std::runtime_error("Persist failure");
 *     }
 *     
 *     LOG_DEBUG("Log size now: %lu entries", entries_.size());
 * }
 * 
 * 
 * BEST PRACTICE 7.9: Performance Monitoring
 * ------------------------------------------
 * 
 * Track metrics for optimization:
 * 
 * struct LogMetrics {
 *     uint64_t total_appends = 0;
 *     uint64_t total_truncates = 0;
 *     double avg_persist_time_ms = 0.0;
 *     uint64_t persist_failures = 0;
 * };
 * 
 * void append(const LogEntry& entry) {
 *     auto start = std::chrono::high_resolution_clock::now();
 *     
 *     // ... append logic ...
 *     
 *     auto end = std::chrono::high_resolution_clock::now();
 *     double elapsed_ms = std::chrono::duration<double, std::milli>(
 *         end - start
 *     ).count();
 *     
 *     metrics_.total_appends++;
 *     metrics_.avg_persist_time_ms = 
 *         (metrics_.avg_persist_time_ms * (metrics_.total_appends - 1) 
 *          + elapsed_ms) / metrics_.total_appends;
 *     
 *     if (elapsed_ms > 10.0) {
 *         LOG_WARN("Slow persist: %.2fms", elapsed_ms);
 *     }
 * }
 ******************************************************************************/

/*******************************************************************************
 * SECTION 8: FAQ
 *******************************************************************************
 * 
 * Q1: Why use Raft instead of simpler primary-backup replication?
 * ----------------------------------------------------------------
 * A: Primary-backup has several critical flaws:
 *    - Split-brain: After partition, both nodes think they're primary
 *    - Lost updates: Primary crashes before replicating → data lost
 *    - No automatic failover: Requires external coordinator
 *    
 *    Raft solves all of these:
 *    - Majority quorum prevents split-brain
 *    - Entries committed only after replication to majority
 *    - Automatic leader election (no external dependency)
 * 
 * 
 * Q2: What happens if all 3 controller nodes crash simultaneously?
 * -----------------------------------------------------------------
 * A: System halts (unavailable) but data is NOT lost:
 *    1. When nodes restart, they load state from disk
 *    2. Nodes elect new leader (whoever has most up-to-date log)
 *    3. Leader catches up followers with missing entries
 *    4. System resumes from where it left off
 *    
 *    Downtime: ~5 seconds (per project requirement)
 *    Data loss: Zero (all committed entries were persisted)
 * 
 * 
 * Q3: Can we have more than 3 controller nodes?
 * ----------------------------------------------
 * A: Yes! Raft works with any odd number of nodes (5, 7, 9, etc.).
 *    
 *    Trade-offs:
 *    - More nodes: Higher availability (tolerate more failures)
 *    - More nodes: Lower performance (more replication overhead)
 *    
 *    For your project (3-week timeline, academic cluster):
 *    - 3 nodes is optimal balance
 *    - Tolerates 1 failure (minimal for fault tolerance)
 *    - Low coordination overhead
 * 
 * 
 * Q4: What if disk fills up and persist_to_disk() fails?
 * -------------------------------------------------------
 * A: Critical situation. Options:
 *    1. Best: Reject new entries, enter read-only mode
 *    2. Acceptable: Crash immediately (restart will recover old state)
 *    3. NEVER: Continue without persisting (violates safety)
 *    
 *    Recommended handling:
 *    if (!persist_to_disk()) {
 *        LOG_FATAL("Disk full - entering read-only mode");
 *        enter_read_only_mode();
 *        alert_operator();
 *    }
 * 
 * 
 * Q5: How do we prevent the log from growing infinitely?
 * -------------------------------------------------------
 * A: Implement log compaction (snapshotting). Not in initial version, but:
 *    
 *    Basic approach:
 *    1. Periodically save state machine snapshot to disk
 *    2. Discard log entries older than snapshot
 *    3. Keep snapshot + recent log entries
 *    
 *    Example:
 *    - After 10,000 entries, take snapshot
 *    - Delete entries 1-9,999
 *    - Keep snapshot + entries 10,000+
 *    - Total storage: ~1MB snapshot + recent entries
 * 
 * 
 * Q6: Can we read from followers to distribute load?
 * ---------------------------------------------------
 * A: Reading from followers can return stale data (they may be behind leader).
 *    
 *    Safe approach:
 *    - All reads go to leader (always up-to-date)
 *    - Followers only for replication
 *    
 *    For your project:
 *    - Reads are minimal (job status queries)
 *    - Write-heavy workload (job submissions)
 *    - Leader read is sufficient
 * 
 * 
 * Q7: What's the performance overhead of Raft?
 * ---------------------------------------------
 * A: Overhead comes from two sources:
 *    
 *    1. Disk writes (persist_to_disk):
 *       - ~5-10ms per write (HDD)
 *       - ~0.1ms per write (SSD)
 *       - Mitigation: Batch writes
 *    
 *    2. Network replication:
 *       - Round-trip to replicate to followers
 *       - ~1ms on LAN (Khoury cluster)
 *       - Parallelized (send to both followers simultaneously)
 *    
 *    Total overhead per request: ~10-20ms (acceptable for job scheduling)
 * 
 * 
 * Q8: How do we test Raft implementation?
 * ----------------------------------------
 * A: Three levels of testing:
 *    
 *    1. Unit Tests:
 *       - Test each RaftLog method in isolation
 *       - Verify persistence (write/read disk file)
 *       - Check thread safety (concurrent access)
 *    
 *    2. Integration Tests:
 *       - Simulate multi-node cluster
 *       - Test leader election scenarios
 *       - Test log replication correctness
 *    
 *    3. Fault Injection Tests:
 *       - Kill leader mid-operation
 *       - Introduce network partitions
 *       - Corrupt disk file
 *       - Verify system recovers correctly
 * 
 * 
 * Q9: What are the most common Raft implementation bugs?
 * -------------------------------------------------------
 * A: Top 5 bugs (based on real-world implementations):
 *    
 *    1. Forgetting to persist before acknowledging RPC
 *       → Lost state on crash
 *    
 *    2. Not clearing vote when term changes
 *       → Double voting in same term
 *    
 *    3. Off-by-one errors in log indexing
 *       → Accessing wrong entry or out-of-bounds
 *    
 *    4. Not handling term increases in RPCs
 *       → Accepting stale messages from old leader
 *    
 *    5. Committing entries from previous terms incorrectly
 *       → Violating Log Matching property
 * 
 * 
 * Q10: Can I reuse this RaftLog for other projects?
 * --------------------------------------------------
 * A: Yes! RaftLog is a generic replicated log implementation.
 *    
 *    To adapt for different projects:
 *    1. Change LogEntryType to match your command types
 *    2. Modify data serialization format
 *    3. Integrate with your state machine (job scheduler → your app logic)
 *    
 *    The core Raft logic (terms, votes, persistence) is universal.
 ******************************************************************************/

/*******************************************************************************
 * SECTION 9: RAFT CORRECTNESS CHECKLIST
 *******************************************************************************
 * 
 * Use this checklist to verify your Raft implementation is correct:
 * 
 * PERSISTENCE:
 * ☐ current_term persisted before acknowledging term update
 * ☐ voted_for persisted before granting vote
 * ☐ entries persisted before acknowledging append
 * ☐ Disk write failures handled safely (reject RPC or crash)
 * ☐ Recovery loads all three pieces of state correctly
 * 
 * TERM MANAGEMENT:
 * ☐ Term never decreases (monotonic)
 * ☐ voted_for cleared when term changes
 * ☐ Node steps down to follower if higher term discovered
 * ☐ Stale RPCs (lower term) rejected
 * 
 * VOTE SAFETY:
 * ☐ Vote granted at most once per term
 * ☐ Vote granted only if candidate's log at least as up-to-date
 * ☐ Election timeout randomized to prevent split votes
 * 
 * LOG REPLICATION:
 * ☐ Entries appended in sequential order (no gaps in indexes)
 * ☐ Consistency check (prev_log_index, prev_log_term) before append
 * ☐ Conflicting entries truncated before appending new entries
 * ☐ Leader never deletes or overwrites its own entries
 * 
 * COMMITMENT:
 * ☐ Entries committed only after replication to majority
 * ☐ Committed entries never lost or rolled back
 * ☐ New leader commits NO_OP before committing entries from previous terms
 * 
 * THREAD SAFETY:
 * ☐ All public methods protected by mutex
 * ☐ No race conditions in concurrent append/read
 * ☐ Disk writes atomic (no partial writes visible)
 * 
 * EDGE CASES:
 * ☐ Empty log handled correctly (last_log_index returns 0)
 * ☐ Truncate at index 1 clears entire log
 * ☐ Append to full disk fails gracefully
 * ☐ Corrupted log file detected on recovery
 ******************************************************************************/

#endif // RAFT_LOG_H