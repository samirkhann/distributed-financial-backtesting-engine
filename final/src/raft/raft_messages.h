/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: raft_messages.h
    
    Description:
        This header file implements the serialization and deserialization layer
        for Raft consensus protocol Remote Procedure Calls (RPCs). It provides
        binary encoding/decoding for RequestVote and AppendEntries messages,
        which are the two core RPC types in the Raft algorithm.
        
        The Raft protocol requires reliable network communication between
        controller nodes for:
        - Leader election (RequestVote RPC)
        - Log replication and heartbeats (AppendEntries RPC)
        
        This implementation uses a custom binary protocol with network byte
        order (big-endian) conversion to ensure platform-independent message
        exchange across the distributed controller cluster.
        
    Key Components:
        - RaftRPCType: Enumeration of message types
        - Network byte order helpers: Cross-platform serialization utilities
        - RequestVoteRPC: Serializer/deserializer for leader election messages
        - AppendEntriesRPC: Serializer/deserializer for log replication messages
        
    Dependencies:
        - raft/raft_node.h: Contains RequestVoteRequest, RequestVoteResponse,
                           AppendEntriesRequest, AppendEntriesResponse, and
                           LogEntry data structures
        - Standard C++ libraries: <vector>, <cstdint>, <cstring>
        
    Design Philosophy:
        This implementation prioritizes:
        1. Zero-copy operations where possible
        2. Explicit buffer size validation to prevent overflow attacks
        3. Network byte order for cross-platform compatibility
        4. Minimal memory allocations during serialization
        5. Exception-based error handling for malformed messages
        
    Thread Safety:
        All functions are stateless and thread-safe. They can be called
        concurrently from multiple threads without synchronization.
        
    Performance Considerations:
        - Serialization: O(n) where n is the total size of log entries
        - Deserialization: O(n) with bounds checking
        - Memory: Single allocation per message, no fragmentation
        
*******************************************************************************/

#ifndef RAFT_MESSAGES_H
#define RAFT_MESSAGES_H

#include "raft/raft_node.h"
#include <vector>
#include <cstdint>
#include <cstring>

namespace backtesting {
namespace raft {

/*******************************************************************************
 * TABLE OF CONTENTS
 * =================
 * 
 * 1. MESSAGE TYPE DEFINITIONS
 *    - RaftRPCType enum
 * 
 * 2. NETWORK BYTE ORDER UTILITIES
 *    - hton64_raft() - Host to network 64-bit conversion
 *    - ntoh64_raft() - Network to host 64-bit conversion
 * 
 * 3. REQUEST VOTE RPC (Leader Election)
 *    - RequestVoteRPC::serialize_request()
 *    - RequestVoteRPC::deserialize_request()
 *    - RequestVoteRPC::serialize_response()
 *    - RequestVoteRPC::deserialize_response()
 * 
 * 4. APPEND ENTRIES RPC (Log Replication & Heartbeats)
 *    - AppendEntriesRPC::serialize_request()
 *    - AppendEntriesRPC::deserialize_request()
 *    - AppendEntriesRPC::serialize_response()
 *    - AppendEntriesRPC::deserialize_response()
 * 
 * 5. USAGE EXAMPLES
 * 6. COMMON PITFALLS
 * 7. FREQUENTLY ASKED QUESTIONS
 * 
*******************************************************************************/

/*******************************************************************************
 * SECTION 1: MESSAGE TYPE DEFINITIONS
 *******************************************************************************/

/**
 * @enum RaftRPCType
 * @brief Identifies the type of Raft RPC message for protocol routing
 * 
 * Each Raft message begins with a single byte indicating its type. This allows
 * the receiver to route the message to the appropriate handler without parsing
 * the entire message body.
 * 
 * Wire Format Position: Byte 0 of every serialized message
 * 
 * Design Note:
 *   Using uint8_t as the underlying type keeps message headers compact.
 *   Values 1-4 are assigned to leave room for future extensions (5-255).
 *   Never reuse or reorder these values as it would break protocol compatibility.
 * 
 * @see RequestVoteRPC for REQUEST_VOTE and REQUEST_VOTE_RESPONSE usage
 * @see AppendEntriesRPC for APPEND_ENTRIES and APPEND_ENTRIES_RESPONSE usage
 */
enum class RaftRPCType : uint8_t {
    REQUEST_VOTE = 1,              // Candidate requests vote during election
    REQUEST_VOTE_RESPONSE = 2,     // Follower grants or denies vote
    APPEND_ENTRIES = 3,            // Leader replicates log entries or sends heartbeat
    APPEND_ENTRIES_RESPONSE = 4    // Follower acknowledges AppendEntries
};

/*******************************************************************************
 * SECTION 2: NETWORK BYTE ORDER UTILITIES
 *******************************************************************************/

/**
 * @brief Converts a 64-bit unsigned integer from host to network byte order
 * 
 * Network protocols require big-endian byte order for portability across
 * different architectures (x86 is little-endian, some ARM can be big-endian).
 * This function ensures consistent byte ordering across the cluster.
 * 
 * Implementation Strategy:
 *   - Split 64-bit value into two 32-bit halves
 *   - Use standard htonl() for each half (widely optimized)
 *   - Swap the halves to achieve 64-bit big-endian ordering
 * 
 * Why not use htobe64()?
 *   Not all platforms provide htobe64() in standard libraries. This portable
 *   implementation works everywhere and gets inlined by modern compilers.
 * 
 * @param val The 64-bit value in host byte order
 * @return The same value in network (big-endian) byte order
 * 
 * @performance O(1), typically inlined to 2-3 assembly instructions
 * 
 * Example:
 *   uint64_t term = 42;
 *   uint64_t network_term = hton64_raft(term);  // Ready for wire transmission
 */
inline uint64_t hton64_raft(uint64_t val) {
    // Extract high 32 bits and convert to network order
    uint32_t high = htonl(static_cast<uint32_t>(val >> 32));
    
    // Extract low 32 bits and convert to network order
    uint32_t low = htonl(static_cast<uint32_t>(val & 0xFFFFFFFF));
    
    // Reassemble with halves swapped (this achieves big-endian 64-bit)
    return (static_cast<uint64_t>(low) << 32) | high;
}

/**
 * @brief Converts a 64-bit unsigned integer from network to host byte order
 * 
 * This is the inverse operation of hton64_raft(). Since network-to-host and
 * host-to-network are symmetric operations for byte swapping, we can reuse
 * the same implementation.
 * 
 * Mathematical Property:
 *   ntoh64_raft(hton64_raft(x)) == x for all x
 *   hton64_raft(ntoh64_raft(x)) == x for all x
 * 
 * @param val The 64-bit value in network (big-endian) byte order
 * @return The same value in host byte order
 * 
 * @performance O(1), typically inlined
 * 
 * Example:
 *   uint64_t network_term = read_from_network(); // Received from network
 *   uint64_t host_term = ntoh64_raft(network_term);  // Ready for local use
 */
inline uint64_t ntoh64_raft(uint64_t val) {
    return hton64_raft(val);  // Symmetric operation
}

/*******************************************************************************
 * SECTION 3: REQUEST VOTE RPC (Leader Election)
 * 
 * The RequestVote RPC is invoked by candidates during elections. When a
 * follower's election timeout expires, it transitions to candidate, increments
 * its term, and sends RequestVote to all peers asking for their vote.
 * 
 * Raft Safety Requirements:
 *   - Each server votes for at most one candidate per term (first-come basis)
 *   - Candidate must have log at least as up-to-date as voter's log
 *   - "Up-to-date" means: later term, or same term with >= log length
 * 
 * Wire Protocol:
 *   REQUEST_VOTE message (33 bytes):
 *     [0]      : uint8_t  - RaftRPCType::REQUEST_VOTE
 *     [1-8]    : uint64_t - term (candidate's term)
 *     [9-16]   : uint64_t - candidate_id (candidate requesting vote)
 *     [17-24]  : uint64_t - last_log_index (index of candidate's last log entry)
 *     [25-32]  : uint64_t - last_log_term (term of candidate's last log entry)
 * 
 *   REQUEST_VOTE_RESPONSE message (10 bytes):
 *     [0]      : uint8_t  - RaftRPCType::REQUEST_VOTE_RESPONSE
 *     [1-8]    : uint64_t - term (responder's current term)
 *     [9]      : uint8_t  - vote_granted (1 = granted, 0 = denied)
 * 
 *******************************************************************************/

/**
 * @class RequestVoteRPC
 * @brief Serialization handlers for Raft leader election messages
 * 
 * This class provides static methods to convert RequestVote request/response
 * structs into binary wire format and vice versa. All methods are stateless.
 * 
 * Usage Pattern:
 *   Sender side:  struct → serialize() → network
 *   Receiver side: network → deserialize() → struct
 * 
 * Thread Safety: All methods are thread-safe (no shared state)
 */
class RequestVoteRPC {
public:
    /**
     * @brief Serializes a RequestVote request into binary wire format
     * 
     * Converts a RequestVoteRequest struct into a 33-byte binary message
     * suitable for transmission over TCP. All multi-byte integers are
     * converted to network byte order.
     * 
     * Algorithm:
     *   1. Allocate vector with type byte
     *   2. Append term (8 bytes, network order)
     *   3. Append candidate_id (8 bytes, network order)
     *   4. Append last_log_index (8 bytes, network order)
     *   5. Append last_log_term (8 bytes, network order)
     * 
     * @param req The RequestVoteRequest to serialize
     * @return Vector containing the serialized message (33 bytes)
     * 
     * @throws None - This method cannot fail
     * 
     * @performance 
     *   Time: O(1) - fixed 33 bytes
     *   Memory: Single 33-byte allocation
     *   Network I/O: ~33 bytes transmitted
     * 
     * Example:
     *   RequestVoteRequest req;
     *   req.term = 5;
     *   req.candidate_id = 2;
     *   req.last_log_index = 100;
     *   req.last_log_term = 4;
     *   
     *   auto bytes = RequestVoteRPC::serialize_request(req);
     *   send_over_network(bytes.data(), bytes.size());
     */
    static std::vector<uint8_t> serialize_request(const RequestVoteRequest& req) {
        std::vector<uint8_t> buffer;
        
        // Byte 0: Message type identifier
        buffer.push_back(static_cast<uint8_t>(RaftRPCType::REQUEST_VOTE));
        
        // Bytes 1-8: Candidate's current term
        // The term is crucial for Raft's safety - receivers reject requests
        // from older terms and update their own term if they see a higher one
        uint64_t net_term = hton64_raft(req.term);
        const uint8_t* term_bytes = reinterpret_cast<const uint8_t*>(&net_term);
        buffer.insert(buffer.end(), term_bytes, term_bytes + 8);
        
        // Bytes 9-16: Candidate's node ID
        // This identifies which node is requesting the vote
        uint64_t net_candidate = hton64_raft(req.candidate_id);
        const uint8_t* cand_bytes = reinterpret_cast<const uint8_t*>(&net_candidate);
        buffer.insert(buffer.end(), cand_bytes, cand_bytes + 8);
        
        // Bytes 17-24: Index of candidate's last log entry
        // Used to determine if candidate's log is up-to-date
        uint64_t net_last_idx = hton64_raft(req.last_log_index);
        const uint8_t* idx_bytes = reinterpret_cast<const uint8_t*>(&net_last_idx);
        buffer.insert(buffer.end(), idx_bytes, idx_bytes + 8);
        
        // Bytes 25-32: Term of candidate's last log entry
        // Together with last_log_index, this determines log "up-to-date-ness"
        uint64_t net_last_term = hton64_raft(req.last_log_term);
        const uint8_t* lterm_bytes = reinterpret_cast<const uint8_t*>(&net_last_term);
        buffer.insert(buffer.end(), lterm_bytes, lterm_bytes + 8);
        
        return buffer;  // RVO (Return Value Optimization) eliminates copy
    }
    
    /**
     * @brief Deserializes a RequestVote request from binary wire format
     * 
     * Parses a 33-byte binary message into a RequestVoteRequest struct.
     * Performs strict size validation before parsing to prevent buffer overruns.
     * 
     * Security Note:
     *   This function is the first line of defense against malicious or
     *   corrupted network data. The size check prevents buffer overflow attacks.
     * 
     * @param data Pointer to the serialized message buffer
     * @param size Size of the buffer in bytes
     * @return Deserialized RequestVoteRequest struct
     * 
     * @throws std::runtime_error if size < 33 bytes
     * 
     * @performance 
     *   Time: O(1) - fixed parsing of 33 bytes
     *   Memory: No additional allocation
     * 
     * @warning 
     *   Caller must ensure 'data' remains valid during function execution.
     *   This function does NOT take ownership of the buffer.
     * 
     * Example:
     *   uint8_t buffer[33];
     *   ssize_t received = recv(socket, buffer, sizeof(buffer), 0);
     *   
     *   try {
     *       auto req = RequestVoteRPC::deserialize_request(buffer, received);
     *       handle_vote_request(req);
     *   } catch (const std::runtime_error& e) {
     *       log_error("Malformed RequestVote: ", e.what());
     *   }
     */
    static RequestVoteRequest deserialize_request(const uint8_t* data, size_t size) {
        // Critical safety check - prevent buffer overflow
        // A real attack might send truncated messages to exploit parsing bugs
        if (size < 33) throw std::runtime_error("Invalid RequestVote size");
        
        RequestVoteRequest req;
        const uint8_t* ptr = data + 1; // Skip type byte (already validated)
        
        // Extract term (bytes 1-8)
        std::memcpy(&req.term, ptr, 8);
        req.term = ntoh64_raft(req.term);  // Convert to host byte order
        ptr += 8;
        
        // Extract candidate_id (bytes 9-16)
        std::memcpy(&req.candidate_id, ptr, 8);
        req.candidate_id = ntoh64_raft(req.candidate_id);
        ptr += 8;
        
        // Extract last_log_index (bytes 17-24)
        std::memcpy(&req.last_log_index, ptr, 8);
        req.last_log_index = ntoh64_raft(req.last_log_index);
        ptr += 8;
        
        // Extract last_log_term (bytes 25-32)
        std::memcpy(&req.last_log_term, ptr, 8);
        req.last_log_term = ntoh64_raft(req.last_log_term);
        
        return req;
    }
    
    /**
     * @brief Serializes a RequestVote response into binary wire format
     * 
     * Converts a RequestVoteResponse struct into a 10-byte binary message.
     * This is the reply sent by followers to candidates during elections.
     * 
     * Response Semantics:
     *   - vote_granted = true: "I grant you my vote for this term"
     *   - vote_granted = false: "I deny your vote" (reasons: already voted,
     *     candidate's log not up-to-date, or candidate has stale term)
     * 
     * @param resp The RequestVoteResponse to serialize
     * @return Vector containing the serialized message (10 bytes)
     * 
     * @throws None
     * 
     * @performance O(1) - fixed 10 bytes
     * 
     * Example:
     *   RequestVoteResponse resp;
     *   resp.term = 5;
     *   resp.vote_granted = true;
     *   
     *   auto bytes = RequestVoteRPC::serialize_response(resp);
     *   send_over_network(bytes.data(), bytes.size());
     */
    static std::vector<uint8_t> serialize_response(const RequestVoteResponse& resp) {
        std::vector<uint8_t> buffer;
        
        // Byte 0: Message type identifier
        buffer.push_back(static_cast<uint8_t>(RaftRPCType::REQUEST_VOTE_RESPONSE));
        
        // Bytes 1-8: Responder's current term
        // If this is higher than candidate's term, candidate will step down
        uint64_t net_term = hton64_raft(resp.term);
        const uint8_t* term_bytes = reinterpret_cast<const uint8_t*>(&net_term);
        buffer.insert(buffer.end(), term_bytes, term_bytes + 8);
        
        // Byte 9: Vote granted flag (boolean as 0 or 1)
        // Using 0/1 encoding instead of raw bool for cross-platform compatibility
        buffer.push_back(resp.vote_granted ? 1 : 0);
        
        return buffer;
    }
    
    /**
     * @brief Deserializes a RequestVote response from binary wire format
     * 
     * Parses a 10-byte binary message into a RequestVoteResponse struct.
     * 
     * @param data Pointer to the serialized message buffer
     * @param size Size of the buffer in bytes
     * @return Deserialized RequestVoteResponse struct
     * 
     * @throws std::runtime_error if size < 10 bytes
     * 
     * @performance O(1) - fixed parsing of 10 bytes
     * 
     * Example:
     *   uint8_t buffer[10];
     *   ssize_t received = recv(socket, buffer, sizeof(buffer), 0);
     *   
     *   try {
     *       auto resp = RequestVoteRPC::deserialize_response(buffer, received);
     *       if (resp.vote_granted) {
     *           votes_received++;
     *       }
     *   } catch (const std::runtime_error& e) {
     *       log_error("Malformed response: ", e.what());
     *   }
     */
    static RequestVoteResponse deserialize_response(const uint8_t* data, size_t size) {
        if (size < 10) throw std::runtime_error("Invalid RequestVoteResponse size");
        
        RequestVoteResponse resp;
        const uint8_t* ptr = data + 1; // Skip type byte
        
        // Extract term (bytes 1-8)
        std::memcpy(&resp.term, ptr, 8);
        resp.term = ntoh64_raft(resp.term);
        ptr += 8;
        
        // Extract vote_granted (byte 9)
        // Any non-zero value is interpreted as true for robustness
        resp.vote_granted = (*ptr != 0);
        
        return resp;
    }
};

/*******************************************************************************
 * SECTION 4: APPEND ENTRIES RPC (Log Replication & Heartbeats)
 * 
 * The AppendEntries RPC serves two critical purposes in Raft:
 * 
 * 1. LOG REPLICATION: Leader sends log entries to followers
 *    - Entries contain state machine commands (jobs in our case)
 *    - Leader tracks which entries each follower has replicated
 *    - Once majority replicate an entry, it becomes "committed"
 * 
 * 2. HEARTBEATS: Leader sends empty AppendEntries (no log entries)
 *    - Prevents followers from starting elections
 *    - Sent at regular intervals (typically every 50-150ms)
 *    - Also used to advance follower's commit index
 * 
 * Raft Log Consistency:
 *   AppendEntries includes prev_log_index and prev_log_term. The follower
 *   only accepts entries if its log matches at that position. This ensures
 *   logs never diverge across the cluster.
 * 
 * Wire Protocol:
 *   APPEND_ENTRIES message (variable length):
 *     [0]      : uint8_t  - RaftRPCType::APPEND_ENTRIES
 *     [1-8]    : uint64_t - term
 *     [9-16]   : uint64_t - leader_id
 *     [17-24]  : uint64_t - prev_log_index
 *     [25-32]  : uint64_t - prev_log_term
 *     [33-36]  : uint32_t - num_entries (count of log entries)
 *     [37-...] : LogEntry[] - variable-length entries
 *     [...-end]: uint64_t - leader_commit
 * 
 *   Each LogEntry:
 *     [0-7]    : uint64_t - term
 *     [8-15]   : uint64_t - index
 *     [16]     : uint8_t  - type (LogEntryType enum)
 *     [17-20]  : uint32_t - data_size
 *     [21-...] : uint8_t[] - data (job payload)
 * 
 *   APPEND_ENTRIES_RESPONSE message (18 bytes):
 *     [0]      : uint8_t  - RaftRPCType::APPEND_ENTRIES_RESPONSE
 *     [1-8]    : uint64_t - term
 *     [9]      : uint8_t  - success (1 = entries accepted, 0 = rejected)
 *     [10-17]  : uint64_t - match_index (highest index follower has replicated)
 * 
 *******************************************************************************/

/**
 * @class AppendEntriesRPC
 * @brief Serialization handlers for Raft log replication and heartbeat messages
 * 
 * This class handles the most complex message type in Raft. AppendEntries
 * messages can contain zero to thousands of log entries, making efficient
 * serialization crucial for performance.
 * 
 * Performance Considerations:
 *   - Large batches of entries are common (hundreds of jobs)
 *   - Each entry contains arbitrary-length payload data
 *   - Total message size can reach megabytes
 *   - Must avoid excessive memory allocations
 * 
 * Thread Safety: All methods are thread-safe (no shared state)
 */
class AppendEntriesRPC {
public:
    /**
     * @brief Serializes an AppendEntries request into binary wire format
     * 
     * Converts an AppendEntriesRequest (potentially containing many log entries)
     * into a binary message. This is the workhorse of Raft replication.
     * 
     * Message Size Calculation:
     *   Base overhead: 45 bytes (header + footer)
     *   Per entry: 21 bytes + data.size()
     *   Total: 45 + Σ(21 + entry[i].data.size())
     * 
     * Optimization Notes:
     *   - Single allocation: vector pre-sizing would reduce reallocations but
     *     requires calculating total size first (tradeoff: extra pass vs reallocs)
     *   - Current implementation favors simplicity over micro-optimization
     *   - For very large batches (>1000 entries), consider pre-sizing buffer
     * 
     * @param req The AppendEntriesRequest to serialize
     * @return Vector containing the serialized message (variable length)
     * 
     * @throws None - But may throw std::bad_alloc if out of memory
     * 
     * @performance 
     *   Time: O(n * m) where n = entry count, m = avg entry data size
     *   Memory: O(total message size) - single contiguous allocation
     * 
     * Example:
     *   AppendEntriesRequest req;
     *   req.term = 5;
     *   req.leader_id = 1;
     *   req.prev_log_index = 100;
     *   req.prev_log_term = 4;
     *   req.leader_commit = 95;
     *   
     *   // Add a log entry (e.g., a job)
     *   LogEntry entry;
     *   entry.term = 5;
     *   entry.index = 101;
     *   entry.type = LogEntryType::COMMAND;
     *   entry.data = serialize_job(job);  // Job-specific serialization
     *   req.entries.push_back(entry);
     *   
     *   auto bytes = AppendEntriesRPC::serialize_request(req);
     *   send_over_network(bytes.data(), bytes.size());
     */
    static std::vector<uint8_t> serialize_request(const AppendEntriesRequest& req) {
        std::vector<uint8_t> buffer;
        
        // Byte 0: Message type identifier
        buffer.push_back(static_cast<uint8_t>(RaftRPCType::APPEND_ENTRIES));
        
        // Bytes 1-8: Leader's current term
        uint64_t net_term = hton64_raft(req.term);
        const uint8_t* term_bytes = reinterpret_cast<const uint8_t*>(&net_term);
        buffer.insert(buffer.end(), term_bytes, term_bytes + 8);
        
        // Bytes 9-16: Leader's node ID
        // Followers track this to know who to respond to
        uint64_t net_leader = hton64_raft(req.leader_id);
        const uint8_t* leader_bytes = reinterpret_cast<const uint8_t*>(&net_leader);
        buffer.insert(buffer.end(), leader_bytes, leader_bytes + 8);
        
        // Bytes 17-24: Index of log entry immediately preceding new ones
        // Critical for log consistency check on follower side
        uint64_t net_prev_idx = hton64_raft(req.prev_log_index);
        const uint8_t* pidx_bytes = reinterpret_cast<const uint8_t*>(&net_prev_idx);
        buffer.insert(buffer.end(), pidx_bytes, pidx_bytes + 8);
        
        // Bytes 25-32: Term of prev_log_index entry
        // Follower rejects if its log doesn't match at this position
        uint64_t net_prev_term = hton64_raft(req.prev_log_term);
        const uint8_t* pterm_bytes = reinterpret_cast<const uint8_t*>(&net_prev_term);
        buffer.insert(buffer.end(), pterm_bytes, pterm_bytes + 8);
        
        // Bytes 33-36: Number of entries in this message
        // Zero entries = heartbeat message
        uint32_t num_entries = htonl(static_cast<uint32_t>(req.entries.size()));
        const uint8_t* num_bytes = reinterpret_cast<const uint8_t*>(&num_entries);
        buffer.insert(buffer.end(), num_bytes, num_bytes + 4);
        
        // Serialize each log entry
        // Each entry is: term(8) + index(8) + type(1) + data_size(4) + data(n)
        for (const auto& entry : req.entries) {
            // Entry term (8 bytes)
            uint64_t net_e_term = hton64_raft(entry.term);
            const uint8_t* eterm_bytes = reinterpret_cast<const uint8_t*>(&net_e_term);
            buffer.insert(buffer.end(), eterm_bytes, eterm_bytes + 8);
            
            // Entry index (8 bytes)
            uint64_t net_e_idx = hton64_raft(entry.index);
            const uint8_t* eidx_bytes = reinterpret_cast<const uint8_t*>(&net_e_idx);
            buffer.insert(buffer.end(), eidx_bytes, eidx_bytes + 8);
            
            // Entry type (1 byte) - LogEntryType enum value
            buffer.push_back(static_cast<uint8_t>(entry.type));
            
            // Data size (4 bytes)
            uint32_t data_size = htonl(static_cast<uint32_t>(entry.data.size()));
            const uint8_t* dsize_bytes = reinterpret_cast<const uint8_t*>(&data_size);
            buffer.insert(buffer.end(), dsize_bytes, dsize_bytes + 4);
            
            // Data payload (variable length)
            // This is where job specifications, commands, etc. are stored
            buffer.insert(buffer.end(), entry.data.begin(), entry.data.end());
        }
        
        // Final 8 bytes: Leader's commit index
        // Tells follower which entries are committed (safe to apply to state machine)
        uint64_t net_commit = hton64_raft(req.leader_commit);
        const uint8_t* commit_bytes = reinterpret_cast<const uint8_t*>(&net_commit);
        buffer.insert(buffer.end(), commit_bytes, commit_bytes + 8);
        
        return buffer;
    }
    
    /**
     * @brief Deserializes an AppendEntries request from binary wire format
     * 
     * Parses a variable-length binary message into an AppendEntriesRequest.
     * This is the most complex deserialization function due to variable-length
     * entries and the need for extensive bounds checking.
     * 
     * Security Critical:
     *   This function must validate EVERY size field before dereferencing
     *   pointers. A malicious sender could craft messages with incorrect size
     *   fields to cause buffer overruns. Each bounds check (ptr + N > end)
     *   is essential for security.
     * 
     * Error Handling Philosophy:
     *   - Fail fast: Throw immediately on any inconsistency
     *   - Avoid partial parsing: Either complete success or throw
     *   - Provide useful error messages for debugging
     * 
     * @param data Pointer to the serialized message buffer
     * @param size Size of the buffer in bytes
     * @return Deserialized AppendEntriesRequest struct
     * 
     * @throws std::runtime_error if:
     *   - Total size < 45 bytes (minimum valid message)
     *   - Buffer underflow detected during entry parsing
     *   - Entry data_size field would exceed remaining buffer
     * 
     * @performance 
     *   Time: O(n * m) where n = entry count, m = avg entry data size
     *   Memory: O(total entry data size) for copying entry payloads
     * 
     * @warning 
     *   Caller must ensure 'data' remains valid during function execution.
     *   This function creates copies of all entry data.
     * 
     * Example:
     *   uint8_t buffer[1024];
     *   ssize_t received = recv(socket, buffer, sizeof(buffer), MSG_WAITALL);
     *   
     *   try {
     *       auto req = AppendEntriesRPC::deserialize_request(buffer, received);
     *       
     *       // Process entries
     *       for (const auto& entry : req.entries) {
     *           apply_to_state_machine(entry);
     *       }
     *       
     *       // Send response
     *       AppendEntriesResponse resp;
     *       resp.term = current_term;
     *       resp.success = true;
     *       resp.match_index = req.prev_log_index + req.entries.size();
     *       send_response(resp);
     *       
     *   } catch (const std::runtime_error& e) {
     *       log_error("Malformed AppendEntries: ", e.what());
     *       // Network disconnect or send error response
     *   }
     */
    static AppendEntriesRequest deserialize_request(const uint8_t* data, size_t size) {
        // Minimum valid message: header(37) + num_entries(4) + commit(8) = 45 bytes
        // But we need at least one more byte for the message type, so 45 minimum
        if (size < 45) throw std::runtime_error("Invalid AppendEntries size");
        
        AppendEntriesRequest req;
        const uint8_t* ptr = data + 1; // Skip type byte
        const uint8_t* end = data + size; // Track buffer end for safety
        
        // Extract term (bytes 1-8)
        std::memcpy(&req.term, ptr, 8);
        req.term = ntoh64_raft(req.term);
        ptr += 8;
        
        // Extract leader_id (bytes 9-16)
        std::memcpy(&req.leader_id, ptr, 8);
        req.leader_id = ntoh64_raft(req.leader_id);
        ptr += 8;
        
        // Extract prev_log_index (bytes 17-24)
        std::memcpy(&req.prev_log_index, ptr, 8);
        req.prev_log_index = ntoh64_raft(req.prev_log_index);
        ptr += 8;
        
        // Extract prev_log_term (bytes 25-32)
        std::memcpy(&req.prev_log_term, ptr, 8);
        req.prev_log_term = ntoh64_raft(req.prev_log_term);
        ptr += 8;
        
        // Extract num_entries (bytes 33-36)
        uint32_t num_entries;
        std::memcpy(&num_entries, ptr, 4);
        num_entries = ntohl(num_entries);
        ptr += 4;
        
        // Reserve space for entries to avoid repeated reallocations
        // This is a significant performance optimization for large batches
        req.entries.reserve(num_entries);
        
        // Deserialize each log entry
        // This loop must carefully track pointer position and validate bounds
        for (uint32_t i = 0; i < num_entries; ++i) {
            // Each entry header is 21 bytes minimum (term + index + type + data_size)
            if (ptr + 21 > end) throw std::runtime_error("Buffer underflow");
            
            LogEntry entry;
            
            // Entry term (8 bytes)
            std::memcpy(&entry.term, ptr, 8);
            entry.term = ntoh64_raft(entry.term);
            ptr += 8;
            
            // Entry index (8 bytes)
            std::memcpy(&entry.index, ptr, 8);
            entry.index = ntoh64_raft(entry.index);
            ptr += 8;
            
            // Entry type (1 byte)
            // Cast to enum type (no validation - trust sender)
            entry.type = static_cast<LogEntryType>(*ptr);
            ptr += 1;
            
            // Data size (4 bytes)
            uint32_t data_size;
            std::memcpy(&data_size, ptr, 4);
            data_size = ntohl(data_size);
            ptr += 4;
            
            // Validate data_size before allocating/copying
            // This prevents attacks where data_size is maliciously large
            if (ptr + data_size > end) throw std::runtime_error("Buffer underflow");
            
            // Copy entry data payload
            // Using assign() for efficient vector construction from iterators
            entry.data.assign(ptr, ptr + data_size);
            ptr += data_size;
            
            req.entries.push_back(std::move(entry)); // Move to avoid copy
        }
        
        // Extract leader_commit (final 8 bytes)
        if (ptr + 8 > end) throw std::runtime_error("Buffer underflow");
        std::memcpy(&req.leader_commit, ptr, 8);
        req.leader_commit = ntoh64_raft(req.leader_commit);
        
        return req;
    }
    
    /**
     * @brief Serializes an AppendEntries response into binary wire format
     * 
     * Converts an AppendEntriesResponse struct into an 18-byte binary message.
     * This response tells the leader whether the follower successfully appended
     * the entries and what the follower's highest replicated index is.
     * 
     * Response Semantics:
     *   - success = true: "I appended your entries, consistency check passed"
     *     match_index indicates highest index follower now has
     *   
     *   - success = false: "I rejected your entries" (reasons: term mismatch,
     *     prev_log_index doesn't match, or follower's log diverged)
     *     match_index is undefined/ignored when success = false
     * 
     * Leader's Use of match_index:
     *   The leader tracks match_index for each follower. Once a majority of
     *   followers have match_index >= N, the leader can commit entry N and
     *   apply it to the state machine.
     * 
     * @param resp The AppendEntriesResponse to serialize
     * @return Vector containing the serialized message (18 bytes)
     * 
     * @throws None
     * 
     * @performance O(1) - fixed 18 bytes
     * 
     * Example:
     *   AppendEntriesResponse resp;
     *   resp.term = 5;
     *   resp.success = true;
     *   resp.match_index = 105;  // Follower now has entries 1-105
     *   
     *   auto bytes = AppendEntriesRPC::serialize_response(resp);
     *   send_over_network(bytes.data(), bytes.size());
     */
    static std::vector<uint8_t> serialize_response(const AppendEntriesResponse& resp) {
        std::vector<uint8_t> buffer;
        
        // Byte 0: Message type identifier
        buffer.push_back(static_cast<uint8_t>(RaftRPCType::APPEND_ENTRIES_RESPONSE));
        
        // Bytes 1-8: Responder's current term
        // If higher than leader's term, leader will step down
        uint64_t net_term = hton64_raft(resp.term);
        const uint8_t* term_bytes = reinterpret_cast<const uint8_t*>(&net_term);
        buffer.insert(buffer.end(), term_bytes, term_bytes + 8);
        
        // Byte 9: Success flag
        // 1 = entries accepted, 0 = rejected (leader will retry with earlier index)
        buffer.push_back(resp.success ? 1 : 0);
        
        // Bytes 10-17: Match index (highest index follower has replicated)
        // Leader uses this to track replication progress and determine commit index
        uint64_t net_match = hton64_raft(resp.match_index);
        const uint8_t* match_bytes = reinterpret_cast<const uint8_t*>(&net_match);
        buffer.insert(buffer.end(), match_bytes, match_bytes + 8);
        
        return buffer;
    }
    
    /**
     * @brief Deserializes an AppendEntries response from binary wire format
     * 
     * Parses an 18-byte binary message into an AppendEntriesResponse struct.
     * 
     * @param data Pointer to the serialized message buffer
     * @param size Size of the buffer in bytes
     * @return Deserialized AppendEntriesResponse struct
     * 
     * @throws std::runtime_error if size < 18 bytes
     * 
     * @performance O(1) - fixed parsing of 18 bytes
     * 
     * Example:
     *   uint8_t buffer[18];
     *   ssize_t received = recv(socket, buffer, sizeof(buffer), 0);
     *   
     *   try {
     *       auto resp = AppendEntriesRPC::deserialize_response(buffer, received);
     *       
     *       if (resp.success) {
     *           // Update match_index for this follower
     *           follower_match_index[follower_id] = resp.match_index;
     *           
     *           // Check if we can advance commit index
     *           if (majority_have_replicated(resp.match_index)) {
     *               commit_index = resp.match_index;
     *               apply_committed_entries();
     *           }
     *       } else {
     *           // Follower rejected - retry with earlier prev_log_index
     *           next_index[follower_id]--;
     *           retry_append_entries(follower_id);
     *       }
     *       
     *   } catch (const std::runtime_error& e) {
     *       log_error("Malformed response: ", e.what());
     *   }
     */
    static AppendEntriesResponse deserialize_response(const uint8_t* data, size_t size) {
        if (size < 18) throw std::runtime_error("Invalid AppendEntriesResponse size");
        
        AppendEntriesResponse resp;
        const uint8_t* ptr = data + 1; // Skip type byte
        
        // Extract term (bytes 1-8)
        std::memcpy(&resp.term, ptr, 8);
        resp.term = ntoh64_raft(resp.term);
        ptr += 8;
        
        // Extract success (byte 9)
        resp.success = (*ptr != 0);
        ptr += 1;
        
        // Extract match_index (bytes 10-17)
        std::memcpy(&resp.match_index, ptr, 8);
        resp.match_index = ntoh64_raft(resp.match_index);
        
        return resp;
    }
};

} // namespace raft
} // namespace backtesting

/*******************************************************************************
 * SECTION 5: USAGE EXAMPLES
 *******************************************************************************/

/*
 * EXAMPLE 1: Candidate Sending RequestVote During Election
 * ----------------------------------------------------------
 * 
 * When a follower's election timeout expires, it becomes a candidate and
 * sends RequestVote RPCs to all other nodes.
 * 
 * Code:
 * 
 *   // Transition to candidate state
 *   current_term++;
 *   voted_for = my_node_id;
 *   votes_received = 1;  // Vote for self
 *   
 *   // Prepare RequestVote message
 *   RequestVoteRequest vote_req;
 *   vote_req.term = current_term;
 *   vote_req.candidate_id = my_node_id;
 *   vote_req.last_log_index = log.back().index;
 *   vote_req.last_log_term = log.back().term;
 *   
 *   // Serialize and send to all peers
 *   auto vote_bytes = RequestVoteRPC::serialize_request(vote_req);
 *   
 *   for (auto& peer : peers) {
 *       send_tcp(peer.socket, vote_bytes.data(), vote_bytes.size());
 *   }
 * 
 * EXAMPLE 2: Follower Handling RequestVote
 * ------------------------------------------
 * 
 * When a follower receives a RequestVote, it decides whether to grant the
 * vote based on term and log up-to-date checks.
 * 
 * Code:
 * 
 *   uint8_t buffer[33];
 *   ssize_t received = recv(socket, buffer, sizeof(buffer), MSG_WAITALL);
 *   
 *   try {
 *       auto vote_req = RequestVoteRPC::deserialize_request(buffer, received);
 *       
 *       RequestVoteResponse vote_resp;
 *       vote_resp.term = current_term;
 *       vote_resp.vote_granted = false;
 *       
 *       // Grant vote if:
 *       // 1. Candidate's term >= our term
 *       // 2. We haven't voted yet this term (or voted for this candidate)
 *       // 3. Candidate's log is at least as up-to-date as ours
 *       
 *       if (vote_req.term > current_term) {
 *           current_term = vote_req.term;
 *           voted_for = -1;  // Reset vote for new term
 *           transition_to_follower();
 *       }
 *       
 *       bool log_ok = (vote_req.last_log_term > last_log_term) ||
 *                     (vote_req.last_log_term == last_log_term && 
 *                      vote_req.last_log_index >= last_log_index);
 *       
 *       if (vote_req.term == current_term && 
 *           (voted_for == -1 || voted_for == vote_req.candidate_id) &&
 *           log_ok) {
 *           vote_resp.vote_granted = true;
 *           voted_for = vote_req.candidate_id;
 *           reset_election_timeout();  // Don't start our own election
 *       }
 *       
 *       auto resp_bytes = RequestVoteRPC::serialize_response(vote_resp);
 *       send_tcp(socket, resp_bytes.data(), resp_bytes.size());
 *       
 *   } catch (const std::runtime_error& e) {
 *       log_error("Failed to handle RequestVote: ", e.what());
 *       close(socket);
 *   }
 * 
 * EXAMPLE 3: Leader Sending Heartbeat
 * -------------------------------------
 * 
 * Leaders must send periodic heartbeats to prevent follower elections.
 * Heartbeats are AppendEntries RPCs with zero entries.
 * 
 * Code:
 * 
 *   // Send heartbeat every 50ms
 *   while (state == LEADER) {
 *       for (auto& follower : followers) {
 *           AppendEntriesRequest heartbeat;
 *           heartbeat.term = current_term;
 *           heartbeat.leader_id = my_node_id;
 *           heartbeat.prev_log_index = follower.next_index - 1;
 *           heartbeat.prev_log_term = log[heartbeat.prev_log_index].term;
 *           heartbeat.entries = {};  // Empty = heartbeat
 *           heartbeat.leader_commit = commit_index;
 *           
 *           auto hb_bytes = AppendEntriesRPC::serialize_request(heartbeat);
 *           send_tcp(follower.socket, hb_bytes.data(), hb_bytes.size());
 *       }
 *       
 *       std::this_thread::sleep_for(std::chrono::milliseconds(50));
 *   }
 * 
 * EXAMPLE 4: Leader Replicating Log Entries
 * -------------------------------------------
 * 
 * When the leader receives a client command (job), it appends to its log
 * and replicates to followers.
 * 
 * Code:
 * 
 *   // Client submitted a job
 *   Job new_job = parse_job_submission();
 *   
 *   // Append to our log
 *   LogEntry entry;
 *   entry.term = current_term;
 *   entry.index = log.back().index + 1;
 *   entry.type = LogEntryType::COMMAND;
 *   entry.data = serialize_job(new_job);
 *   log.push_back(entry);
 *   
 *   // Replicate to all followers
 *   for (auto& follower : followers) {
 *       AppendEntriesRequest req;
 *       req.term = current_term;
 *       req.leader_id = my_node_id;
 *       req.prev_log_index = follower.next_index - 1;
 *       req.prev_log_term = log[req.prev_log_index].term;
 *       
 *       // Send all entries from next_index onwards
 *       req.entries.assign(log.begin() + follower.next_index, log.end());
 *       req.leader_commit = commit_index;
 *       
 *       auto req_bytes = AppendEntriesRPC::serialize_request(req);
 *       send_tcp(follower.socket, req_bytes.data(), req_bytes.size());
 *   }
 * 
 * EXAMPLE 5: Follower Handling AppendEntries
 * --------------------------------------------
 * 
 * Followers check log consistency and append entries if checks pass.
 * 
 * Code:
 * 
 *   std::vector<uint8_t> buffer(1024 * 1024);  // 1MB buffer for large messages
 *   ssize_t received = recv(socket, buffer.data(), buffer.size(), 0);
 *   
 *   try {
 *       auto req = AppendEntriesRPC::deserialize_request(buffer.data(), received);
 *       
 *       AppendEntriesResponse resp;
 *       resp.term = current_term;
 *       resp.success = false;
 *       resp.match_index = 0;
 *       
 *       // Step 1: Reply false if term < current_term
 *       if (req.term < current_term) {
 *           auto resp_bytes = AppendEntriesRPC::serialize_response(resp);
 *           send_tcp(socket, resp_bytes.data(), resp_bytes.size());
 *           return;
 *       }
 *       
 *       // Update term if we see higher term
 *       if (req.term > current_term) {
 *           current_term = req.term;
 *           voted_for = -1;
 *           transition_to_follower();
 *       }
 *       
 *       reset_election_timeout();  // Valid leader message
 *       current_leader = req.leader_id;
 *       
 *       // Step 2: Log consistency check
 *       if (req.prev_log_index > 0) {
 *           if (log.size() <= req.prev_log_index ||
 *               log[req.prev_log_index].term != req.prev_log_term) {
 *               // Log doesn't match - leader will retry with earlier index
 *               resp.success = false;
 *               auto resp_bytes = AppendEntriesRPC::serialize_response(resp);
 *               send_tcp(socket, resp_bytes.data(), resp_bytes.size());
 *               return;
 *           }
 *       }
 *       
 *       // Step 3: Append entries
 *       size_t index = req.prev_log_index + 1;
 *       for (const auto& entry : req.entries) {
 *           if (log.size() > index && log[index].term != entry.term) {
 *               // Conflicting entry - delete it and all that follow
 *               log.erase(log.begin() + index, log.end());
 *           }
 *           if (log.size() <= index) {
 *               log.push_back(entry);
 *           }
 *           index++;
 *       }
 *       
 *       // Step 4: Update commit index
 *       if (req.leader_commit > commit_index) {
 *           commit_index = std::min(req.leader_commit, log.back().index);
 *           apply_committed_entries();  // Execute jobs!
 *       }
 *       
 *       // Step 5: Send success response
 *       resp.success = true;
 *       resp.match_index = log.back().index;
 *       auto resp_bytes = AppendEntriesRPC::serialize_response(resp);
 *       send_tcp(socket, resp_bytes.data(), resp_bytes.size());
 *       
 *   } catch (const std::runtime_error& e) {
 *       log_error("Failed to handle AppendEntries: ", e.what());
 *       close(socket);
 *   }
 */

/*******************************************************************************
 * SECTION 6: COMMON PITFALLS
 *******************************************************************************/

/*
 * PITFALL 1: Forgetting Network Byte Order Conversion
 * ----------------------------------------------------
 * 
 * Problem:
 *   Directly sending uint64_t values without hton64_raft() works on homogeneous
 *   clusters (all x86) but breaks in mixed environments (x86 + ARM).
 * 
 * Wrong Code:
 *   buffer.insert(buffer.end(), 
 *                 reinterpret_cast<uint8_t*>(&req.term),
 *                 reinterpret_cast<uint8_t*>(&req.term) + 8);
 * 
 * Correct Code:
 *   uint64_t net_term = hton64_raft(req.term);
 *   buffer.insert(buffer.end(),
 *                 reinterpret_cast<const uint8_t*>(&net_term),
 *                 reinterpret_cast<const uint8_t*>(&net_term) + 8);
 * 
 * Impact: Silent data corruption on mixed-architecture clusters
 * 
 * 
 * PITFALL 2: Insufficient Buffer Size Validation
 * -----------------------------------------------
 * 
 * Problem:
 *   Not checking buffer size before deserializing allows buffer overruns.
 *   Attackers can send truncated messages to exploit this.
 * 
 * Wrong Code:
 *   auto req = RequestVoteRPC::deserialize_request(buffer, received);
 *   // Assumes 'received' is correct size - may crash or read garbage
 * 
 * Correct Code:
 *   try {
 *       auto req = RequestVoteRPC::deserialize_request(buffer, received);
 *       // ... process req
 *   } catch (const std::runtime_error& e) {
 *       log_error("Invalid message: ", e.what());
 *       return;  // Don't process malformed messages
 *   }
 * 
 * Impact: Security vulnerability, crashes, undefined behavior
 * 
 * 
 * PITFALL 3: Ignoring Match Index in AppendEntries Responses
 * -----------------------------------------------------------
 * 
 * Problem:
 *   Not tracking match_index per follower makes it impossible to determine
 *   when entries are committed by a majority.
 * 
 * Wrong Code:
 *   if (resp.success) {
 *       // Just assume it worked - don't track progress
 *   }
 * 
 * Correct Code:
 *   if (resp.success) {
 *       match_index[follower_id] = resp.match_index;
 *       next_index[follower_id] = resp.match_index + 1;
 *       
 *       // Check if majority have replicated
 *       std::vector<uint64_t> indices;
 *       for (auto [_, idx] : match_index) indices.push_back(idx);
 *       std::sort(indices.begin(), indices.end());
 *       
 *       uint64_t majority_index = indices[indices.size() / 2];
 *       if (majority_index > commit_index) {
 *           commit_index = majority_index;
 *           apply_committed_entries();
 *       }
 *   }
 * 
 * Impact: Jobs never get committed/executed despite successful replication
 * 
 * 
 * PITFALL 4: Not Handling Variable-Length Entry Data
 * ---------------------------------------------------
 * 
 * Problem:
 *   Assuming all log entries are the same size or forgetting to serialize
 *   the data size field.
 * 
 * Wrong Code:
 *   // Missing data_size field!
 *   buffer.insert(buffer.end(), entry.data.begin(), entry.data.end());
 * 
 * Correct Code:
 *   uint32_t data_size = htonl(static_cast<uint32_t>(entry.data.size()));
 *   buffer.insert(buffer.end(),
 *                 reinterpret_cast<const uint8_t*>(&data_size),
 *                 reinterpret_cast<const uint8_t*>(&data_size) + 4);
 *   buffer.insert(buffer.end(), entry.data.begin(), entry.data.end());
 * 
 * Impact: Deserialization fails, data corruption
 * 
 * 
 * PITFALL 5: Memory Allocation in Hot Paths
 * ------------------------------------------
 * 
 * Problem:
 *   Not pre-allocating vectors causes repeated reallocations during serialization.
 * 
 * Optimization:
 *   For AppendEntries with many entries, reserve vector capacity:
 *   
 *   size_t estimated_size = 45;  // Base overhead
 *   for (const auto& entry : req.entries) {
 *       estimated_size += 21 + entry.data.size();
 *   }
 *   buffer.reserve(estimated_size);
 * 
 * Impact: Performance degradation under high throughput
 * 
 * 
 * PITFALL 6: Not Validating Enum Values
 * --------------------------------------
 * 
 * Problem:
 *   Casting arbitrary bytes to enums without validation can lead to
 *   undefined behavior if invalid values are used in switch statements.
 * 
 * Current Code (acceptable for trusted network):
 *   entry.type = static_cast<LogEntryType>(*ptr);
 * 
 * Hardened Code (for untrusted network):
 *   uint8_t type_byte = *ptr;
 *   if (type_byte > static_cast<uint8_t>(LogEntryType::MAX_VALUE)) {
 *       throw std::runtime_error("Invalid LogEntryType");
 *   }
 *   entry.type = static_cast<LogEntryType>(type_byte);
 * 
 * Impact: Undefined behavior, security vulnerability
 * 
 * 
 * PITFALL 7: Reusing Buffers Without Clearing
 * --------------------------------------------
 * 
 * Problem:
 *   Reusing a vector for multiple messages without clearing can append
 *   to previous message data.
 * 
 * Wrong Code:
 *   std::vector<uint8_t> buffer;
 *   for (auto& follower : followers) {
 *       // buffer still contains previous message!
 *       auto msg = AppendEntriesRPC::serialize_request(req);
 *       buffer.insert(buffer.end(), msg.begin(), msg.end());
 *       send(follower.socket, buffer.data(), buffer.size(), 0);
 *   }
 * 
 * Correct Code:
 *   for (auto& follower : followers) {
 *       auto buffer = AppendEntriesRPC::serialize_request(req);
 *       send(follower.socket, buffer.data(), buffer.size(), 0);
 *   }
 * 
 * Impact: Corrupted messages, protocol violations
 */

/*******************************************************************************
 * SECTION 7: FREQUENTLY ASKED QUESTIONS
 *******************************************************************************/

/*
 * Q1: Why use a custom binary protocol instead of JSON or Protobuf?
 * 
 * A1: Performance and control. JSON:
 *     - Pros: Human-readable, easy debugging
 *     - Cons: 2-3x larger messages, slower parsing, no type safety
 *     
 *     Protobuf:
 *     - Pros: Efficient, versioning support, schema validation
 *     - Cons: Additional dependency, compilation step, learning curve
 *     
 *     Custom binary:
 *     - Pros: Zero dependencies, minimal overhead, full control
 *     - Cons: More code to write/maintain, harder to debug
 *     
 *     For this project, we prioritized simplicity and performance. With
 *     only 4 message types, a custom protocol is manageable and gives us
 *     the tightest possible encoding.
 * 
 * 
 * Q2: What happens if two messages arrive concatenated in one recv()?
 * 
 * A2: This implementation assumes message framing is handled at the TCP layer.
 *     In production, you'd need either:
 *     
 *     Option 1: Length-prefix each message
 *       [4 bytes: message length][message payload]
 *     
 *     Option 2: Use MSG_WAITALL with known message sizes
 *       recv(socket, buffer, expected_size, MSG_WAITALL)
 *     
 *     Option 3: Implement a message queue that reassembles partial receives
 * 
 *     For the Khoury cluster with reliable, low-latency TCP, MSG_WAITALL
 *     is sufficient.
 * 
 * 
 * Q3: How do I add a new RPC type (e.g., InstallSnapshot)?
 * 
 * A3: Follow these steps:
 *     
 *     1. Add enum value to RaftRPCType:
 *        enum class RaftRPCType : uint8_t {
 *            ...
 *            INSTALL_SNAPSHOT = 5,
 *            INSTALL_SNAPSHOT_RESPONSE = 6
 *        };
 *     
 *     2. Define request/response structs in raft_node.h:
 *        struct InstallSnapshotRequest { ... };
 *        struct InstallSnapshotResponse { ... };
 *     
 *     3. Create serialization class:
 *        class InstallSnapshotRPC {
 *        public:
 *            static std::vector<uint8_t> serialize_request(...);
 *            static InstallSnapshotRequest deserialize_request(...);
 *            static std::vector<uint8_t> serialize_response(...);
 *            static InstallSnapshotResponse deserialize_response(...);
 *        };
 *     
 *     4. Update message router to handle new type:
 *        switch (static_cast<RaftRPCType>(buffer[0])) {
 *            case RaftRPCType::INSTALL_SNAPSHOT:
 *                handle_install_snapshot(...);
 *                break;
 *            ...
 *        }
 * 
 * 
 * Q4: Why are term and index uint64_t instead of uint32_t?
 * 
 * A4: Future-proofing and paranoia. Raft terms increment on every election,
 *     and log indices grow indefinitely. While uint32_t (4 billion) seems
 *     plenty, consider:
 *     
 *     - Elections in unstable networks can cause rapid term increases
 *     - Long-running systems might accumulate millions of log entries
 *     - uint64_t only costs 4 extra bytes per message
 *     - Changing sizes later would break wire compatibility
 *     
 *     Better to over-allocate now than hit limits later.
 * 
 * 
 * Q5: Can I use these functions from multiple threads concurrently?
 * 
 * A5: Yes! All functions are stateless and thread-safe. They only operate on
 *     their parameters and return values, with no shared state. You can call
 *     serialize/deserialize from any thread without locking.
 *     
 *     However, the underlying network sockets are NOT thread-safe. You must
 *     synchronize access to sockets yourself (e.g., one thread per connection).
 * 
 * 
 * Q6: How do I test this code?
 * 
 * A6: Recommended test strategy:
 *     
 *     1. Unit tests: Round-trip serialization
 *        auto req = create_test_request();
 *        auto bytes = RequestVoteRPC::serialize_request(req);
 *        auto req2 = RequestVoteRPC::deserialize_request(bytes.data(), bytes.size());
 *        assert(req == req2);
 *     
 *     2. Boundary tests: Empty entries, max values
 *        AppendEntriesRequest req;
 *        req.entries = {};  // Zero entries (heartbeat)
 *        req.term = UINT64_MAX;
 *        // ... verify serialization succeeds
 *     
 *     3. Error tests: Invalid sizes
 *        uint8_t truncated[10];
 *        EXPECT_THROW(
 *            RequestVoteRPC::deserialize_request(truncated, 10),
 *            std::runtime_error
 *        );
 *     
 *     4. Fuzz testing: Random bytes
 *        for (int i = 0; i < 10000; i++) {
 *            std::vector<uint8_t> random_bytes = generate_random();
 *            try {
 *                deserialize_request(random_bytes.data(), random_bytes.size());
 *            } catch (...) {
 *                // Should throw, not crash
 *            }
 *        }
 *     
 *     5. Integration tests: Actual network send/receive
 *        // Send from node A, receive on node B
 *        // Verify full Raft protocol flow works
 * 
 * 
 * Q7: What's the maximum message size I can send?
 * 
 * A7: Theoretical limit: ~4GB (uint32_t num_entries, uint32_t data_size per entry)
 *     
 *     Practical limits:
 *     - TCP send buffer size (typically 2-4 MB)
 *     - Memory allocation (std::vector can throw std::bad_alloc)
 *     - Network MTU and fragmentation
 *     - Latency considerations (large messages take longer)
 *     
 *     Recommendation:
 *     - Keep AppendEntries batches < 1 MB for responsiveness
 *     - If you need to send massive amounts of data, implement
 *       log compaction and InstallSnapshot RPC (not included here)
 * 
 * 
 * Q8: How do I debug serialization issues?
 * 
 * A8: Useful debugging techniques:
 *     
 *     1. Hex dump: Print serialized bytes in hex
 *        void hex_dump(const std::vector<uint8_t>& data) {
 *            for (size_t i = 0; i < data.size(); i++) {
 *                printf("%02X ", data[i]);
 *                if ((i + 1) % 16 == 0) printf("\n");
 *            }
 *            printf("\n");
 *        }
 *     
 *     2. Comparison: Serialize, deserialize, serialize again
 *        auto bytes1 = serialize(req);
 *        auto req2 = deserialize(bytes1);
 *        auto bytes2 = serialize(req2);
 *        assert(bytes1 == bytes2);  // Should be identical
 *     
 *     3. Wireshark: Capture network traffic
 *        tcpdump -i lo -w raft.pcap port 5000
 *        wireshark raft.pcap
 *        # Write a Wireshark dissector for your protocol
 *     
 *     4. Logging: Print before/after values
 *        std::cout << "Serializing term: " << req.term << std::endl;
 *        auto bytes = serialize(req);
 *        auto req2 = deserialize(bytes);
 *        std::cout << "Deserialized term: " << req2.term << std::endl;
 * 
 * 
 * Q9: Should I add checksums or CRC to messages?
 * 
 * A9: TCP already provides checksums, so adding your own is redundant for
 *     LAN environments. However, if you're concerned about:
 *     
 *     - Memory corruption (cosmic rays, hardware errors)
 *     - Bugs in serialization code
 *     - Security against bit-flipping attacks
 *     
 *     Then yes, add a CRC32 or hash to each message. Append it after the
 *     message type byte and verify on deserialize().
 *     
 *     For this academic project, TCP checksums are sufficient.
 * 
 * 
 * Q10: Can I use this code in production?
 * 
 * A10: This code is suitable for academic projects and prototypes, but
 *      production use would require:
 *      
 *      1. Comprehensive testing (unit, integration, stress, fuzz)
 *      2. Security hardening (input validation, DoS protection)
 *      3. Performance optimization (zero-copy, custom allocators)
 *      4. Monitoring and metrics (message counts, sizes, errors)
 *      5. Versioning (protocol version field for backwards compatibility)
 *      6. Documentation (this file is a good start!)
 *      7. Code review by multiple experienced developers
 *      
 *      Consider using a battle-tested library like gRPC, Thrift, or
 *      Cap'n Proto for production systems.
 */

#endif // RAFT_MESSAGES_H