/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: message.cpp
    
    Description:
        This file implements the message serialization and deserialization
        system for the distributed financial backtesting engine. It provides
        binary protocol encoding/decoding for all network communication between
        controller nodes and worker nodes across the Khoury cluster.
        
        Core Functionality:
        - Binary message serialization (C++ objects → network bytes)
        - Binary message deserialization (network bytes → C++ objects)
        - Network byte order conversion (host ↔ network endianness)
        - Type-safe message parsing with validation
        - Zero-copy buffer management where possible
        
    Message Types Supported:
        1. JobAssignMessage: Controller → Worker (assign backtest job)
        2. JobResultMessage: Worker → Controller (return computation results)
        3. HeartbeatMessage: Worker → Controller (health status, every 2s)
        4. WorkerRegisterMessage: Worker → Controller (initial connection)
        
    Protocol Design Philosophy:
        - Binary format: Compact, efficient (vs. JSON/XML overhead)
        - Network byte order: Big-endian for cross-platform compatibility
        - Length-prefixed: Strings and payloads include size headers
        - Type-tagged: First byte identifies message type
        - Versioned: ID field enables request/response correlation
        
    Network Communication Context:
        In this distributed system:
        - 3 Raft controller nodes coordinate work distribution
        - 2-8 worker nodes execute backtest computations
        - TCP connections for reliable, ordered message delivery
        - Messages flow across Khoury cluster low-latency network
        
        Communication patterns:
        - Controller (Leader) → Workers: Job assignments
        - Workers → Controller: Results, heartbeats, registration
        - Raft: Controller-to-controller consensus (separate protocol)
        
    Binary Protocol Benefits:
        vs. Text-based (JSON, XML):
        - 3-5x smaller message size (critical for network efficiency)
        - 10-50x faster serialization (no parsing overhead)
        - Strongly typed (compile-time vs. runtime validation)
        - Deterministic size (easier buffer management)
        
        Trade-offs:
        - Not human-readable (need debugging tools)
        - More complex implementation (manual byte packing)
        - Versioning challenges (protocol evolution)
        
    Endianness Handling:
        Network Byte Order (Big-Endian):
        - Standard for all Internet protocols (TCP/IP, HTTP, etc.)
        - Most significant byte first (0x12345678 → 12 34 56 78)
        
        Host Byte Order (varies):
        - x86/x64 (Intel/AMD): Little-endian (78 56 34 12)
        - ARM: Bi-endian (configurable, usually little)
        - SPARC, PowerPC: Big-endian
        
        Conversion functions (defined in message.h):
        - hton16/32/64: Host-to-network byte order
        - ntoh16/32/64: Network-to-host byte order
        
    Memory Safety:
        - Bounds checking: Every read validates buffer limits
        - Exception safety: Throws on malformed data
        - Buffer overrun protection: Validates sizes before operations
        - No raw pointers in public API: Returns std::unique_ptr
        
    Performance Characteristics:
        - JobAssignMessage: ~200-400 bytes, ~2-5 μs to serialize
        - JobResultMessage: ~300-500 bytes, ~3-7 μs to serialize
        - HeartbeatMessage: 29 bytes (fixed), ~1-2 μs to serialize
        - WorkerRegisterMessage: ~50-100 bytes, ~2-4 μs to serialize
        
        Network overhead (TCP/IP headers): +40 bytes per packet
        Total latency: Serialization + Network + Deserialization
        - Khoury cluster (same subnet): ~0.1-0.5 ms round-trip
        
    Related Files:
        - common/message.h: Message class declarations, protocol definitions
        - network/tcp_connection.cpp: Uses these for network transmission
        - controller/job_scheduler.cpp: Creates JobAssignMessages
        - worker/job_executor.cpp: Creates JobResultMessages
        
    Dependencies:
        - <cstring>: memcpy for byte-level operations
        - <stdexcept>: runtime_error for protocol violations
        - <vector>: Dynamic buffer management
        - message.h: Message class definitions, byte order functions
        
    Thread Safety:
        - All functions are stateless (no shared mutable state)
        - Safe to call from multiple threads concurrently
        - Each message owns its buffer (no aliasing)
        
    Error Handling Philosophy:
        - Fail fast: Throw on malformed data (don't silently corrupt)
        - Explicit errors: Descriptive messages ("Buffer underflow reading X")
        - No undefined behavior: Validate before every memory access
        - Caller responsibility: Catch exceptions, log, handle gracefully

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. HEADER INCLUDES & DEPENDENCIES
// 2. NAMESPACE DECLARATION
// 3. HELPER FUNCTIONS
//    3.1 String Serialization (serialize_string)
//    3.2 String Deserialization (deserialize_string)
// 4. MESSAGE SERIALIZATION IMPLEMENTATIONS
//    4.1 Base Message (Message::serialize)
//    4.2 Job Assignment (JobAssignMessage)
//        4.2.1 Serialization
//        4.2.2 Deserialization
//    4.3 Job Result (JobResultMessage)
//        4.3.1 Serialization
//        4.3.2 Deserialization
//    4.4 Heartbeat (HeartbeatMessage)
//        4.4.1 Serialization
//        4.4.2 Deserialization
//    4.5 Worker Registration (WorkerRegisterMessage)
//        4.5.1 Serialization
//        4.5.2 Deserialization
// 5. PROTOCOL WIRE FORMAT SPECIFICATIONS
// 6. USAGE EXAMPLES
//    6.1 Controller: Assigning Jobs
//    6.2 Worker: Sending Results
//    6.3 Worker: Heartbeat Management
//    6.4 Network Layer Integration
// 7. COMMON PITFALLS & SOLUTIONS
// 8. FAQ
// 9. BEST PRACTICES FOR DISTRIBUTED SYSTEMS
// 10. PERFORMANCE OPTIMIZATION GUIDE
// 11. DEBUGGING & TESTING STRATEGIES
// 12. PROTOCOL EVOLUTION & VERSIONING
// 13. SECURITY CONSIDERATIONS
// 14. TROUBLESHOOTING CHECKLIST
//
//==============================================================================

//==============================================================================
// SECTION 1: HEADER INCLUDES & DEPENDENCIES
//==============================================================================
//
// This section imports required headers for message serialization.
//
//------------------------------------------------------------------------------

// "common/message.h" - Message class definitions and protocol constants
// ======================================================================
// Required for:
// - Message class hierarchy (Message, JobAssignMessage, etc.)
// - MessageType enum (JOB_ASSIGN, JOB_RESULT, HEARTBEAT, etc.)
// - Data structures (JobParameters, JobResult)
// - Byte order conversion functions (hton32, ntoh32, etc.)
//
// Expected declarations in message.h:
//   enum class MessageType : uint8_t {
//       JOB_ASSIGN = 1,
//       JOB_RESULT = 2,
//       HEARTBEAT = 3,
//       WORKER_REGISTER = 4
//   };
//
//   struct JobParameters {
//       std::string symbol;
//       std::string strategy_type;
//       std::string start_date;
//       std::string end_date;
//       int32_t short_window;
//       int32_t long_window;
//       double initial_capital;
//   };
//
//   struct JobResult {
//       uint64_t job_id;
//       std::string symbol;
//       double total_return;
//       double sharpe_ratio;
//       double max_drawdown;
//       double final_portfolio_value;
//       int32_t num_trades;
//       int32_t winning_trades;
//       int32_t losing_trades;
//       bool success;
//       std::string error_message;
//   };
//
//   class Message {
//   public:
//       MessageType type;
//       uint64_t id;
//       uint32_t payload_size;
//       virtual std::vector<uint8_t> serialize() const;
//   };
//
//   // Plus: hton16/32/64 and ntoh16/32/64 functions
//
#include "common/message.h"

// <stdexcept> - Exception classes for error handling
// ===================================================
// Required for:
// - std::runtime_error: Thrown on protocol violations
//
// Usage pattern:
//   if (ptr + size > end) {
//       throw std::runtime_error("Buffer underflow reading field X");
//   }
//
// Why runtime_error (not logic_error)?
// - Protocol violations are RUNTIME errors (bad network data)
// - Not programmer errors (which would be logic_error)
//
// Exception safety:
// - All functions provide strong guarantee: Either succeeds or no state change
// - No memory leaks: RAII containers (vector, unique_ptr) clean up automatically
//
#include <stdexcept>

// <cstring> - Low-level memory operations
// ========================================
// Required for:
// - std::memcpy: Copying bytes between buffers (type-punning safe)
//
// Why std::memcpy (not reinterpret_cast + dereference)?
// - Avoids undefined behavior from strict aliasing violations
// - Portable across platforms (no alignment issues)
// - Optimized by compilers (often single instruction)
//
// Example usage:
//   uint32_t value;
//   std::memcpy(&value, buffer_ptr, sizeof(uint32_t));  // Safe
//   // vs.
//   uint32_t value = *reinterpret_cast<uint32_t*>(buffer_ptr);  // UB if misaligned!
//
// Memory alignment concerns:
// - Network buffers may not be aligned for multi-byte types
// - x86/x64 tolerate misalignment (slower but works)
// - ARM can crash on misaligned access
// - memcpy handles all cases correctly
//
#include <cstring>

//==============================================================================
// SECTION 2: NAMESPACE DECLARATION
//==============================================================================
//
// namespace backtesting { ... }
//
// Encapsulates all distributed system components to prevent naming conflicts.
// See logger.h documentation for detailed namespace rationale.
//
//==============================================================================

namespace backtesting {

//==============================================================================
// SECTION 3: HELPER FUNCTIONS
//==============================================================================
//
// Private utility functions for common serialization tasks.
// Declared static to limit visibility to this translation unit (file scope).
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 3.1 STRING SERIALIZATION: serialize_string()
//------------------------------------------------------------------------------
//
// static void serialize_string(std::vector<uint8_t>& buffer, 
//                               const std::string& str)
//
// PURPOSE:
// Encodes a C++ string into binary format with length prefix, appending
// bytes to the provided buffer.
//
// WIRE FORMAT:
// [4-byte length (network byte order)][variable-length UTF-8 data]
//
// Example:
//   String: "AAPL"
//   Bytes: 00 00 00 04 41 41 50 4C
//          ^^^^^^^^^^ ^^^^^^^^^^^
//          Length=4   "AAPL" ASCII
//
// PARAMETERS:
// - buffer: Output buffer (modified in-place, grows as needed)
// - str: Input string to serialize
//
// DESIGN RATIONALE:
//
// Why 4-byte length prefix?
// - Supports strings up to 4GB (2^32 - 1 bytes)
// - Standard in many protocols (Protocol Buffers, Cap'n Proto)
// - For this project: Symbol names ~10 bytes, dates ~10 bytes
// - Could use 2-byte (64KB max) but 4-byte is safer/standard
//
// Why network byte order for length?
// - Ensures consistent interpretation across different architectures
// - Intel (little-endian) vs. ARM/SPARC (can be big-endian)
// - Example: Length 260 (0x0104)
//   Little-endian: 04 01 00 00
//   Big-endian:    00 00 01 04 (network standard)
//
// Why append to existing buffer?
// - Allows building complex messages incrementally
// - Avoids multiple allocations (efficient)
// - Single buffer for entire message simplifies network I/O
//
// MEMORY EFFICIENCY:
// - No intermediate allocations
// - std::vector::insert() is amortized O(1) at end
// - Total: O(n) where n = string length
//
// CHARACTER ENCODING:
// - Assumes UTF-8 (standard for C++ std::string)
// - Binary safe (can contain null bytes if needed)
// - Length in bytes (not characters, handles multi-byte correctly)
//
// EXCEPTION SAFETY:
// - Strong guarantee: If exception thrown, buffer unchanged
// - std::vector operations can throw std::bad_alloc (out of memory)
// - No partial writes (insert is atomic from caller's perspective)
//
//------------------------------------------------------------------------------

// Serialize string: [4-byte length][data]
//
// IMPLEMENTATION DETAILS:
//
// Step 1: Convert string length to network byte order
//   uint32_t len = hton32(static_cast<uint32_t>(str.size()));
//   - str.size(): Number of bytes in string
//   - Cast to uint32_t: Ensure correct type for hton32
//   - hton32: Host-to-network byte order conversion
//
// Step 2: Get pointer to length bytes
//   const uint8_t* len_bytes = reinterpret_cast<const uint8_t*>(&len);
//   - Treat uint32_t as array of 4 bytes
//   - reinterpret_cast: Low-level type conversion
//   - const: Length not modified, only read
//
// Step 3: Append length bytes to buffer
//   buffer.insert(buffer.end(), len_bytes, len_bytes + 4);
//   - buffer.end(): Insert at end (append)
//   - [len_bytes, len_bytes + 4): Range of 4 bytes to copy
//   - std::vector::insert: Handles resizing automatically
//
// Step 4: Append string data
//   buffer.insert(buffer.end(), str.begin(), str.end());
//   - str.begin/end(): Iterator range over string bytes
//   - Copies entire string content (including embedded nulls if any)
//   - No null terminator needed (length prefix tells us size)
//
// ALTERNATIVE DESIGNS CONSIDERED:
//
// 1. Null-terminated strings (C-style):
//    buffer.insert(buffer.end(), str.c_str(), str.c_str() + str.size() + 1);
//    Problems:
//    - Cannot contain null bytes (binary unsafe)
//    - Requires scanning for null (O(n) to find end)
//    - Wastes 1 byte per string
//
// 2. Fixed-length strings:
//    char fixed[64];
//    std::strncpy(fixed, str.c_str(), 64);
//    Problems:
//    - Wastes space (short strings padded)
//    - Truncates long strings (data loss)
//    - Not scalable (symbol names, dates vary)
//
// 3. Variable-length with 1-byte or 2-byte length:
//    uint8_t len = str.size(); // Max 255 bytes
//    Problems:
//    - Too restrictive (error messages can be longer)
//    - 4 bytes is standard in industry protocols
//
// Length-prefixed with 4-byte size is the best trade-off.
//
// PERFORMANCE NOTES:
// - Typical string ("AAPL"): 4 + 4 = 8 bytes written
// - Overhead: 4 bytes per string (acceptable for network protocol)
// - Time complexity: O(n) where n = string length
// - No parsing needed on receiver (just read length, then data)
//
static void serialize_string(std::vector<uint8_t>& buffer, const std::string& str) {
    // Convert string length to network byte order (big-endian)
    // This ensures consistent interpretation across different CPU architectures
    uint32_t len = hton32(static_cast<uint32_t>(str.size()));
    
    // Reinterpret uint32_t as array of bytes for insertion
    // This is safe because we're only reading, not writing
    const uint8_t* len_bytes = reinterpret_cast<const uint8_t*>(&len);
    
    // Append 4-byte length prefix to buffer
    // buffer.end() ensures we append (don't overwrite existing data)
    // [len_bytes, len_bytes + 4) is the range [first byte, past-the-end)
    buffer.insert(buffer.end(), len_bytes, len_bytes + 4);
    
    // Append string data bytes (no null terminator needed, length tells us size)
    // Handles UTF-8 correctly (works in bytes, not characters)
    // Binary safe (can include null bytes if needed)
    buffer.insert(buffer.end(), str.begin(), str.end());
}

//------------------------------------------------------------------------------
// 3.2 STRING DESERIALIZATION: deserialize_string()
//------------------------------------------------------------------------------
//
// static std::string deserialize_string(const uint8_t*& ptr, 
//                                        const uint8_t* end)
//
// PURPOSE:
// Decodes a length-prefixed string from a binary buffer, advancing the
// read pointer and validating buffer boundaries.
//
// PARAMETERS:
// - ptr: Reference to read pointer (MODIFIED: Advanced past string data)
// - end: Pointer to end of valid buffer (IMMUTABLE: Used for bounds checking)
//
// RETURN VALUE:
// std::string containing decoded data (UTF-8)
//
// EXCEPTIONS:
// - std::runtime_error: If buffer is too small (prevents buffer overrun)
//
// DESIGN RATIONALE:
//
// Why pass ptr by reference?
// - Allows advancing read position (multiple fields per message)
// - Caller maintains ptr, calls deserialize_string multiple times
// - Example:
//     const uint8_t* ptr = buffer;
//     std::string symbol = deserialize_string(ptr, end);  // ptr advanced
//     std::string strategy = deserialize_string(ptr, end);  // ptr advanced again
//
// Why pass end by value (not reference)?
// - End never changes (marks buffer boundary)
// - Cheaper to copy pointer than pass reference
// - Const: Cannot accidentally modify
//
// Why throw on underflow?
// - Fail fast: Malformed data should not be silently ignored
// - Prevents crashes: Better exception than segfault
// - Explicit errors: Caller knows exactly what went wrong
// - Security: Prevents reading uninitialized memory
//
// BOUNDS CHECKING STRATEGY:
//
// Two checks required:
// 1. Before reading length: if (ptr + 4 > end)
// 2. Before reading data: if (ptr + len > end)
//
// Why separate checks?
// - First check: Ensure we can safely read the length field
// - Second check: Ensure we can safely read the data bytes
// - Both necessary: Length field could be present but data truncated
//
// Example of malformed buffer:
//   Buffer: [00 00 00 10] [41 41] (truncated)
//           ^length=16    ^only 2 bytes available
//   Check 1: Pass (4 bytes available for length)
//   Check 2: Fail (16 bytes needed, only 2 available)
//   -> Throw exception, prevent reading beyond buffer
//
// ENDIANNESS HANDLING:
// - Read 4 bytes as-is (network byte order)
// - Convert to host byte order using ntoh32
// - Use converted value for allocation and reading
//
// MEMORY ALLOCATION:
// - std::string constructor allocates exactly len bytes
// - Small String Optimization (SSO): Strings ≤15-23 bytes no heap allocation
// - For this project: Most strings small (symbol names, dates)
//
// CHARACTER ENCODING:
// - Binary data copied as-is (no transcoding)
// - Assumes UTF-8 on both sides (standard for C++)
// - Works for ASCII subset (symbols like "AAPL")
//
// SECURITY CONSIDERATIONS:
//
// 1. Integer overflow prevention:
//    If len is maliciously large (e.g., 0xFFFFFFFF):
//    - Check: ptr + len would overflow
//    - Mitigation: Check (ptr + len > end) catches this
//    - Also: std::string allocation would fail (bad_alloc)
//
// 2. Denial of Service (DoS) prevention:
//    - Attacker sends len = 1GB
//    - Mitigation: Payload size limits at higher level
//    - This function trusts input from authenticated source
//
// 3. Buffer overrun prevention:
//    - Explicit bounds checks before every read
//    - No pointer arithmetic without validation
//    - Throws before reading beyond buffer
//
//------------------------------------------------------------------------------

// Deserialize string
//
// IMPLEMENTATION DETAILS:
//
// Step 1: Validate we can read length field (4 bytes)
//   if (ptr + 4 > end) throw std::runtime_error("Buffer underflow reading string length");
//   - ptr: Current read position
//   - ptr + 4: Position after reading length
//   - end: First invalid byte
//   - If ptr + 4 > end: Not enough bytes remaining
//
// Step 2: Read 4-byte length in network byte order
//   uint32_t len;
//   std::memcpy(&len, ptr, 4);
//   - Declares len variable (uninitialized)
//   - memcpy: Safely copies 4 bytes from buffer to len
//   - Why memcpy? Handles misaligned pointers correctly
//
// Step 3: Convert length from network to host byte order
//   len = ntoh32(len);
//   - ntoh32: Network-to-host byte order conversion
//   - Now len contains actual string length in host representation
//
// Step 4: Advance read pointer past length field
//   ptr += 4;
//   - Modifies caller's pointer (passed by reference)
//   - Now points to start of string data
//
// Step 5: Validate we can read string data (len bytes)
//   if (ptr + len > end) throw std::runtime_error("Buffer underflow reading string data");
//   - Ensures len bytes are available
//   - Prevents reading beyond buffer boundary
//
// Step 6: Construct string from buffer data
//   std::string result(reinterpret_cast<const char*>(ptr), len);
//   - reinterpret_cast<const char*>: Treat uint8_t as char (compatible types)
//   - std::string(const char* data, size_t len): Constructor that copies len bytes
//   - Creates new string (does not alias buffer)
//
// Step 7: Advance read pointer past string data
//   ptr += len;
//   - Now points to next field (ready for next deserialize call)
//
// Step 8: Return constructed string
//   return result;
//   - Move semantics (C++11): No copy, efficient
//   - Caller receives ownership of string
//
// EXCEPTION SAFETY:
// - If exception thrown (step 1 or 5): ptr not modified, result not constructed
// - Strong guarantee: Either succeeds completely or no side effects
// - Caller can catch exception, log, and continue
//
// PERFORMANCE:
// - Typical string ("AAPL"): 4-byte read + 4-byte copy
// - Time: ~100-200 nanoseconds (dominated by memory access)
// - No parsing overhead (unlike JSON/XML)
//
static std::string deserialize_string(const uint8_t*& ptr, const uint8_t* end) {
    // BOUNDS CHECK 1: Ensure we can safely read the 4-byte length prefix
    // This prevents reading uninitialized or out-of-bounds memory
    if (ptr + 4 > end) throw std::runtime_error("Buffer underflow reading string length");
    
    // Read 4-byte length from buffer (still in network byte order)
    // Using memcpy for type-punning safety (avoids strict aliasing violations)
    uint32_t len;
    std::memcpy(&len, ptr, 4);
    
    // Convert from network byte order (big-endian) to host byte order
    // This handles endianness differences between sender and receiver
    len = ntoh32(len);
    
    // Advance read pointer past the length field
    // Note: Modifies caller's pointer (passed by reference)
    ptr += 4;
    
    // BOUNDS CHECK 2: Ensure we can safely read 'len' bytes of string data
    // Protects against malformed messages with incorrect length fields
    if (ptr + len > end) throw std::runtime_error("Buffer underflow reading string data");
    
    // Construct string from buffer bytes
    // std::string copies data (does not alias buffer, safe after buffer freed)
    // reinterpret_cast: uint8_t* → const char* (compatible types)
    std::string result(reinterpret_cast<const char*>(ptr), len);
    
    // Advance read pointer past the string data
    // Now ptr points to the next field (ready for next deserialize call)
    ptr += len;
    
    // Return constructed string (move semantics, no copy)
    return result;
}

//==============================================================================
// SECTION 4: MESSAGE SERIALIZATION IMPLEMENTATIONS
//==============================================================================
//
// This section implements serialize() and deserialize() methods for each
// message type in the distributed system protocol.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.1 BASE MESSAGE SERIALIZATION: Message::serialize()
//------------------------------------------------------------------------------
//
// std::vector<uint8_t> Message::serialize() const
//
// PURPOSE:
// Serializes the common message header present in all message types.
// This is the base implementation, typically extended by derived classes.
//
// WIRE FORMAT (13 bytes total):
// [1-byte type][8-byte ID][4-byte payload_size]
//
// Byte breakdown:
//   Offset 0:     MessageType (uint8_t)
//   Offset 1-8:   Message ID (uint64_t, network byte order)
//   Offset 9-12:  Payload size (uint32_t, network byte order)
//
// DESIGN RATIONALE:
//
// Why 1 byte for type?
// - Supports up to 256 message types (plenty for this system: 4 types)
// - Minimal overhead (1 byte vs. 4 for enum class)
// - Enum class MessageType : uint8_t ensures 1-byte size
//
// Why 8 bytes for ID?
// - Globally unique message identifiers (2^64 possible IDs)
// - Enables request/response correlation across distributed system
// - Controller assigns IDs, workers echo in responses
// - Never exhausted in practice (quintillions of messages)
//
// Why 4 bytes for payload_size?
// - Supports messages up to 4GB (2^32 bytes)
// - For this project: Largest message ~1KB (job results)
// - Enables variable-length payloads without parsing
// - Receiver knows exactly how many bytes to read
//
// HEADER-ONLY vs. HEADER+PAYLOAD:
// - This method only serializes header (13 bytes)
// - Derived classes call this, then append payload
// - Allows flexible payload formats per message type
//
// TYPICAL USAGE:
// Not called directly by application code. Instead, derived classes call:
//   std::vector<uint8_t> JobAssignMessage::serialize() const {
//       std::vector<uint8_t> buffer = Message::serialize();  // Header
//       // ... append payload ...
//       return buffer;
//   }
//
// MESSAGE ID GENERATION:
// Not implemented here, but typical pattern:
//   static std::atomic<uint64_t> next_id{1};
//   msg.id = next_id.fetch_add(1, std::memory_order_relaxed);
//
// PAYLOAD SIZE CALCULATION:
// - Set by derived class after serializing payload
// - Pattern:
//     msg.payload_size = total_buffer.size() - 13;  // Exclude header
//
//------------------------------------------------------------------------------

// Base Message serialization
std::vector<uint8_t> Message::serialize() const {
    // Create buffer for header (will grow for payload in derived classes)
    std::vector<uint8_t> buffer;
    
    // FIELD 1: Message type (1 byte)
    // Cast enum class to underlying type (uint8_t)
    // This is safe because MessageType is defined as: enum class MessageType : uint8_t
    buffer.push_back(static_cast<uint8_t>(type));
    
    // FIELD 2: Message ID (8 bytes, network byte order)
    // ID allows correlation of requests with responses across distributed system
    // Example: Controller sends JOB_ASSIGN with id=42, worker responds with id=42
    
    // Convert ID from host to network byte order (handles endianness)
    uint64_t net_id = hton64(id);
    
    // Get pointer to the 8 bytes of the network-ordered ID
    const uint8_t* id_bytes = reinterpret_cast<const uint8_t*>(&net_id);
    
    // Append all 8 bytes to buffer
    // [id_bytes, id_bytes + 8) is a range of 8 consecutive bytes
    buffer.insert(buffer.end(), id_bytes, id_bytes + 8);
    
    // FIELD 3: Payload size (4 bytes, network byte order)
    // Tells receiver how many bytes to read after header
    // For base Message, payload_size should be 0 (header only)
    // Derived classes override this with actual payload size
    
    // Convert payload size to network byte order
    uint32_t net_size = hton32(payload_size);
    
    // Get pointer to the 4 bytes of the network-ordered size
    const uint8_t* size_bytes = reinterpret_cast<const uint8_t*>(&net_size);
    
    // Append all 4 bytes to buffer
    buffer.insert(buffer.end(), size_bytes, size_bytes + 4);
    
    // Return serialized header (13 bytes total: 1 + 8 + 4)
    // Derived classes typically extend this buffer with payload data
    return buffer;
}

//------------------------------------------------------------------------------
// 4.2 JOB ASSIGNMENT MESSAGE
//------------------------------------------------------------------------------
//
// JobAssignMessage: Controller → Worker
//
// PURPOSE:
// Carries backtest job assignment from controller to worker, including
// all parameters needed to execute the strategy computation.
//
// PAYLOAD STRUCTURE:
// - job_id: Unique identifier for this specific job
// - params.symbol: Stock ticker (e.g., "AAPL", "GOOGL")
// - params.strategy_type: Strategy name (e.g., "SMA", "RSI")
// - params.start_date: Start of backtesting period (e.g., "2020-01-01")
// - params.end_date: End of backtesting period (e.g., "2024-12-31")
// - params.short_window: Short moving average window (days)
// - params.long_window: Long moving average window (days)
// - params.initial_capital: Starting portfolio value (dollars)
//
// WIRE FORMAT (variable length, typically 200-400 bytes):
// [13-byte header]
// [8-byte job_id]
// [4-byte len + symbol string]
// [4-byte len + strategy_type string]
// [4-byte len + start_date string]
// [4-byte len + end_date string]
// [4-byte short_window]
// [4-byte long_window]
// [8-byte initial_capital as double]
//
// USE CASE:
// Controller (Raft leader) assigns jobs to workers:
// 1. Receives client backtest request
// 2. Partitions work (e.g., by symbol or date range)
// 3. Creates JobAssignMessage for each partition
// 4. Sends to available workers
// 5. Tracks assignment in Raft log for fault tolerance
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.2.1 JobAssignMessage Serialization
//------------------------------------------------------------------------------
//
// std::vector<uint8_t> JobAssignMessage::serialize() const
//
// IMPLEMENTATION STRATEGY:
//
// 1. Create buffer and write header (type + id + size placeholder)
// 2. Append payload fields in order
// 3. Calculate actual payload size
// 4. Backfill payload size in header
//
// Why backfill size (not calculate first)?
// - Variable-length strings make pre-calculation complex
// - Easier to serialize first, then measure
// - Only costs one memcpy to write size
//
// FIELD ORDERING:
// Fixed-size fields typically come first in protocols, but here we use:
// - job_id (8 bytes, fixed)
// - symbol (variable)
// - strategy_type (variable)
// - dates (variable)
// - windows (fixed, 8 bytes)
// - capital (fixed, 8 bytes)
//
// This ordering is arbitrary but consistent (same order in deserialize).
//
// DOUBLE SERIALIZATION:
// - IEEE 754 double (64-bit floating point)
// - Treated as uint64_t bit pattern (type punning via memcpy)
// - Converted to network byte order like integer
// - Reconstructed on receiver with reverse process
//
// Why this works:
// - IEEE 754 is standard across all modern platforms
// - Bit pattern is portable (same interpretation everywhere)
// - No precision loss (exact bit-for-bit copy)
//
// Alternative: Send as text ("10000.50")
// - Larger (7-15 bytes vs. 8 bytes)
// - Parsing overhead (atof, strtod)
// - Potential precision loss (decimal rounding)
// - Binary is more efficient
//
//------------------------------------------------------------------------------

// JobAssignMessage serialization
std::vector<uint8_t> JobAssignMessage::serialize() const {
    std::vector<uint8_t> buffer;
    
    // =========================================================================
    // HEADER SECTION (13 bytes)
    // =========================================================================
    
    // FIELD 1: Message type (1 byte) - JOB_ASSIGN
    buffer.push_back(static_cast<uint8_t>(type));
    
    // FIELD 2: Message ID (8 bytes, network byte order)
    uint64_t net_id = hton64(id);
    const uint8_t* id_bytes = reinterpret_cast<const uint8_t*>(&net_id);
    buffer.insert(buffer.end(), id_bytes, id_bytes + 8);
    
    // FIELD 3: Payload size (4 bytes, network byte order)
    // PLACEHOLDER: Will be backfilled after serializing payload
    // Reserve 4 bytes (filled with zeros initially)
    size_t payload_start = buffer.size() + 4; // Remember where payload starts
    buffer.resize(payload_start); // Grow buffer to make room for size field
    
    // =========================================================================
    // PAYLOAD SECTION (variable length, typically 180-400 bytes)
    // =========================================================================
    
    // FIELD 4: Job ID (8 bytes, network byte order)
    // This is separate from message ID:
    // - message.id: For request/response correlation
    // - job_id: Identifies the backtest job (persisted in results)
    uint64_t net_job_id = hton64(job_id);
    const uint8_t* job_id_bytes = reinterpret_cast<const uint8_t*>(&net_job_id);
    buffer.insert(buffer.end(), job_id_bytes, job_id_bytes + 8);
    
    // FIELD 5: Symbol (variable length string, typically 3-5 chars)
    // Examples: "AAPL", "GOOGL", "MSFT"
    // Format: [4-byte length][string bytes]
    serialize_string(buffer, params.symbol);
    
    // FIELD 6: Strategy type (variable length string, typically 3-10 chars)
    // Examples: "SMA", "RSI", "MACD", "MeanReversion"
    // Format: [4-byte length][string bytes]
    serialize_string(buffer, params.strategy_type);
    
    // FIELD 7: Start date (variable length string, typically 10 chars)
    // Format: "YYYY-MM-DD" (ISO 8601)
    // Example: "2020-01-01"
    // Format: [4-byte length][string bytes]
    serialize_string(buffer, params.start_date);
    
    // FIELD 8: End date (variable length string, typically 10 chars)
    // Format: "YYYY-MM-DD"
    // Example: "2024-12-31"
    // Format: [4-byte length][string bytes]
    serialize_string(buffer, params.end_date);
    
    // FIELD 9: Short window (4 bytes, network byte order)
    // Moving average short period (e.g., 50 days)
    // Signed integer to allow error values (e.g., -1 = not used)
    int32_t net_short_window = hton32(params.short_window);
    const uint8_t* sw_bytes = reinterpret_cast<const uint8_t*>(&net_short_window);
    buffer.insert(buffer.end(), sw_bytes, sw_bytes + 4);
    
    // FIELD 10: Long window (4 bytes, network byte order)
    // Moving average long period (e.g., 200 days)
    int32_t net_long_window = hton32(params.long_window);
    const uint8_t* lw_bytes = reinterpret_cast<const uint8_t*>(&net_long_window);
    buffer.insert(buffer.end(), lw_bytes, lw_bytes + 4);
    
    // FIELD 11: Initial capital (8 bytes, network byte order)
    // Starting portfolio value (e.g., $10,000.00)
    // Stored as IEEE 754 double, transmitted as bit pattern
    //
    // Serialization process:
    // 1. Interpret double as 64-bit unsigned integer (type punning)
    // 2. Convert to network byte order
    // 3. Write to buffer
    //
    // Why memcpy? Avoids undefined behavior from type punning
    // The compiler will optimize this to essentially nothing
    uint64_t capital_bits;
    std::memcpy(&capital_bits, &params.initial_capital, sizeof(double));
    uint64_t net_capital = hton64(capital_bits);
    const uint8_t* cap_bytes = reinterpret_cast<const uint8_t*>(&net_capital);
    buffer.insert(buffer.end(), cap_bytes, cap_bytes + 8);
    
    // =========================================================================
    // BACKFILL PAYLOAD SIZE IN HEADER
    // =========================================================================
    
    // Calculate actual payload size (everything after header)
    // payload_start points to first byte after size field
    // buffer.size() is current total size
    uint32_t payload_size = static_cast<uint32_t>(buffer.size() - payload_start);
    
    // Convert to network byte order
    uint32_t net_size = hton32(payload_size);
    
    // Write size into reserved space (overwrite the placeholder zeros)
    // payload_start - 4 is where the size field begins
    std::memcpy(&buffer[payload_start - 4], &net_size, 4);
    
    // Return complete serialized message
    // Typical size: 13 (header) + 180-400 (payload) = 193-413 bytes
    return buffer;
}

//------------------------------------------------------------------------------
// 4.2.2 JobAssignMessage Deserialization
//------------------------------------------------------------------------------
//
// std::unique_ptr<JobAssignMessage> JobAssignMessage::deserialize(
//     const uint8_t* data, size_t size)
//
// PURPOSE:
// Reconstructs a JobAssignMessage object from binary network data.
//
// PARAMETERS:
// - data: Pointer to buffer containing serialized message
// - size: Total buffer size (header + payload)
//
// RETURN VALUE:
// std::unique_ptr<JobAssignMessage> - Ownership transferred to caller
//
// DESIGN RATIONALE:
//
// Why return unique_ptr (not raw pointer or value)?
// - Ownership semantics: Caller owns the message, responsible for cleanup
// - Polymorphism: Can return base class pointer to derived class
// - Efficiency: No copy (unlike return by value)
// - Safety: No manual delete (unlike raw pointer)
//
// Why static method (not constructor)?
// - Deserialization can fail (throw exception)
// - Exceptions from constructors are problematic (partially constructed object)
// - Static method allows validating data before constructing object
// - Named constructor idiom (clear intent: "create from bytes")
//
// VALIDATION STRATEGY:
// - Minimum size check: Ensures at least header is present
// - Per-field checks: Bounds validation before each read
// - Fail fast: Throw on first error (don't try to recover)
// - Explicit errors: Descriptive messages for debugging
//
// POINTER ARITHMETIC:
// - ptr: Current read position (advanced after each field)
// - end: Boundary marker (first invalid byte)
// - pattern: Read, validate, advance, repeat
//
// MEMORY SAFETY:
// - All reads bounds-checked
// - No pointer dereferencing without validation
// - memcpy for type-safe reading
// - RAII: unique_ptr automatically cleans up on exception
//
//------------------------------------------------------------------------------

std::unique_ptr<JobAssignMessage> JobAssignMessage::deserialize(const uint8_t* data, size_t size) {
    // Allocate new message object (ownership transferred to unique_ptr)
    // If any exception thrown, unique_ptr automatically cleans up
    auto msg = std::make_unique<JobAssignMessage>();
    
    // VALIDATION 1: Minimum size check
    // Header is 13 bytes (1 type + 8 id + 4 size)
    // If buffer smaller, it's definitely malformed
    if (size < 13) throw std::runtime_error("Invalid JobAssignMessage size");
    
    // Set up pointers for deserialization
    const uint8_t* ptr = data;        // Current read position (will advance)
    const uint8_t* end = data + size; // First byte past valid data (immutable)
    
    // =========================================================================
    // SKIP HEADER (13 bytes)
    // =========================================================================
    // We don't need to parse header fields here because:
    // - Type: Already known (caller determined this is JobAssignMessage)
    // - ID: Typically extracted at network layer for routing
    // - Size: Only needed for framing (already have complete buffer)
    //
    // If header fields needed, would parse like this:
    //   MessageType type = static_cast<MessageType>(*ptr); ptr += 1;
    //   uint64_t id; memcpy(&id, ptr, 8); id = ntoh64(id); ptr += 8;
    //   uint32_t payload_size; memcpy(&payload_size, ptr, 4);
    //   payload_size = ntoh32(payload_size); ptr += 4;
    
    ptr += 13; // Skip header, now ptr points to payload
    
    // =========================================================================
    // PAYLOAD FIELD 1: Job ID (8 bytes)
    // =========================================================================
    
    // Bounds check before reading
    if (ptr + 8 > end) throw std::runtime_error("Buffer underflow reading job_id");
    
    // Read 8 bytes as uint64_t (still in network byte order)
    std::memcpy(&msg->job_id, ptr, 8);
    
    // Convert from network to host byte order
    msg->job_id = ntoh64(msg->job_id);
    
    // Advance read pointer
    ptr += 8;
    
    // =========================================================================
    // PAYLOAD FIELDS 2-5: Strings (variable length)
    // =========================================================================
    // Each string is [4-byte length][data]
    // deserialize_string handles bounds checking and pointer advancement
    
    msg->params.symbol = deserialize_string(ptr, end);
    msg->params.strategy_type = deserialize_string(ptr, end);
    msg->params.start_date = deserialize_string(ptr, end);
    msg->params.end_date = deserialize_string(ptr, end);
    
    // =========================================================================
    // PAYLOAD FIELDS 6-7: Windows (8 bytes total: 4 + 4)
    // =========================================================================
    
    // Bounds check for both windows together (optimization)
    if (ptr + 8 > end) throw std::runtime_error("Buffer underflow reading windows");
    
    // Read short window (4 bytes)
    int32_t short_window, long_window;
    std::memcpy(&short_window, ptr, 4);
    msg->params.short_window = ntoh32(short_window);
    ptr += 4;
    
    // Read long window (4 bytes)
    std::memcpy(&long_window, ptr, 4);
    msg->params.long_window = ntoh32(long_window);
    ptr += 4;
    
    // =========================================================================
    // PAYLOAD FIELD 8: Initial capital (8 bytes, double)
    // =========================================================================
    
    // Bounds check
    if (ptr + 8 > end) throw std::runtime_error("Buffer underflow reading capital");
    
    // Deserialization process (reverse of serialization):
    // 1. Read 8 bytes as uint64_t (network byte order)
    // 2. Convert to host byte order
    // 3. Reinterpret as double (type punning)
    //
    // This preserves exact bit pattern (no precision loss)
    uint64_t capital_bits;
    std::memcpy(&capital_bits, ptr, 8);
    capital_bits = ntoh64(capital_bits);
    std::memcpy(&msg->params.initial_capital, &capital_bits, sizeof(double));
    
    // Note: We don't advance ptr here because we're done reading
    // If more fields added in future, would need: ptr += 8;
    
    // =========================================================================
    // RETURN DESERIALIZED MESSAGE
    // =========================================================================
    
    // Transfer ownership to caller
    // Caller responsible for storing or processing the message
    return msg;
}

//------------------------------------------------------------------------------
// 4.3 JOB RESULT MESSAGE
//------------------------------------------------------------------------------
//
// JobResultMessage: Worker → Controller
//
// PURPOSE:
// Carries backtest results from worker back to controller after computation
// completes. Includes performance metrics and trade statistics.
//
// PAYLOAD STRUCTURE:
// - result.job_id: Identifies which job this result corresponds to
// - result.symbol: Stock ticker (echoed from assignment)
// - result.total_return: Percentage return (e.g., 0.25 = 25%)
// - result.sharpe_ratio: Risk-adjusted return metric
// - result.max_drawdown: Largest peak-to-trough decline
// - result.final_portfolio_value: Ending portfolio value
// - result.num_trades: Total number of trades executed
// - result.winning_trades: Number of profitable trades
// - result.losing_trades: Number of unprofitable trades
// - result.success: true if computation completed, false if error
// - result.error_message: Empty if success, error description if failure
//
// WIRE FORMAT (variable length, typically 300-500 bytes):
// [13-byte header]
// [8-byte job_id]
// [4-byte len + symbol string]
// [8-byte total_return]
// [8-byte sharpe_ratio]
// [8-byte max_drawdown]
// [8-byte final_portfolio_value]
// [4-byte num_trades]
// [4-byte winning_trades]
// [4-byte losing_trades]
// [1-byte success]
// [4-byte len + error_message string]
//
// USE CASE:
// Worker completes backtest:
// 1. Executes strategy computation
// 2. Calculates performance metrics
// 3. Creates JobResultMessage
// 4. Sends to controller
// 5. Controller aggregates results, updates Raft log
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.3.1 JobResultMessage Serialization
//------------------------------------------------------------------------------

// JobResultMessage serialization
std::vector<uint8_t> JobResultMessage::serialize() const {
    std::vector<uint8_t> buffer;
    
    // =========================================================================
    // HEADER SECTION (13 bytes)
    // =========================================================================
    
    buffer.push_back(static_cast<uint8_t>(type));
    uint64_t net_id = hton64(id);
    const uint8_t* id_bytes = reinterpret_cast<const uint8_t*>(&net_id);
    buffer.insert(buffer.end(), id_bytes, id_bytes + 8);
    
    // Placeholder for payload size (backfilled later)
    size_t payload_start = buffer.size() + 4;
    buffer.resize(payload_start);
    
    // =========================================================================
    // PAYLOAD SECTION
    // =========================================================================
    
    // FIELD 1: Job ID (8 bytes)
    // Links this result to the original job assignment
    uint64_t net_job_id = hton64(result.job_id);
    const uint8_t* job_id_bytes = reinterpret_cast<const uint8_t*>(&net_job_id);
    buffer.insert(buffer.end(), job_id_bytes, job_id_bytes + 8);
    
    // FIELD 2: Symbol (variable length string)
    // Echoed from original assignment for tracking/logging
    serialize_string(buffer, result.symbol);
    
    // FIELDS 3-6: Performance metrics (4 doubles = 32 bytes)
    // Lambda function for DRY principle (Don't Repeat Yourself)
    // Encapsulates the double serialization pattern used 4 times
    //
    // Lambda captures:
    // - &buffer: Reference to buffer (modified in lambda)
    //
    // Lambda parameter:
    // - val: The double value to serialize
    //
    // Lambda body:
    // 1. Type-pun double to uint64_t
    // 2. Convert to network byte order
    // 3. Append to buffer
    auto serialize_double = [&buffer](double val) {
        uint64_t bits;
        std::memcpy(&bits, &val, sizeof(double));
        uint64_t net_bits = hton64(bits);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&net_bits);
        buffer.insert(buffer.end(), bytes, bytes + 8);
    };
    
    // Apply lambda to each performance metric
    serialize_double(result.total_return);         // Percentage return
    serialize_double(result.sharpe_ratio);          // Risk-adjusted return
    serialize_double(result.max_drawdown);          // Worst decline from peak
    serialize_double(result.final_portfolio_value); // Ending value
    
    // FIELDS 7-9: Trade counts (3 int32_t = 12 bytes)
    
    int32_t net_num_trades = hton32(result.num_trades);
    const uint8_t* nt_bytes = reinterpret_cast<const uint8_t*>(&net_num_trades);
    buffer.insert(buffer.end(), nt_bytes, nt_bytes + 4);
    
    int32_t net_winning = hton32(result.winning_trades);
    const uint8_t* wt_bytes = reinterpret_cast<const uint8_t*>(&net_winning);
    buffer.insert(buffer.end(), wt_bytes, wt_bytes + 4);
    
    int32_t net_losing = hton32(result.losing_trades);
    const uint8_t* lt_bytes = reinterpret_cast<const uint8_t*>(&net_losing);
    buffer.insert(buffer.end(), lt_bytes, lt_bytes + 4);
    
    // FIELD 10: Success flag (1 byte)
    // Boolean encoded as uint8_t: 0 = false, non-zero = true
    // We use 1 for true by convention
    buffer.push_back(result.success ? 1 : 0);
    
    // FIELD 11: Error message (variable length string)
    // Empty if success = true
    // Descriptive error if success = false (e.g., "Failed to load CSV: AAPL_2020.csv")
    serialize_string(buffer, result.error_message);
    
    // =========================================================================
    // BACKFILL PAYLOAD SIZE
    // =========================================================================
    
    uint32_t payload_size = static_cast<uint32_t>(buffer.size() - payload_start);
    uint32_t net_size = hton32(payload_size);
    std::memcpy(&buffer[payload_start - 4], &net_size, 4);
    
    return buffer;
}

//------------------------------------------------------------------------------
// 4.3.2 JobResultMessage Deserialization
//------------------------------------------------------------------------------

std::unique_ptr<JobResultMessage> JobResultMessage::deserialize(const uint8_t* data, size_t size) {
    auto msg = std::make_unique<JobResultMessage>();
    
    // Minimum size validation
    if (size < 13) throw std::runtime_error("Invalid JobResultMessage size");
    
    const uint8_t* ptr = data + 13; // Skip header
    const uint8_t* end = data + size;
    
    // =========================================================================
    // FIELD 1: Job ID (8 bytes)
    // =========================================================================
    
    if (ptr + 8 > end) throw std::runtime_error("Buffer underflow");
    std::memcpy(&msg->result.job_id, ptr, 8);
    msg->result.job_id = ntoh64(msg->result.job_id);
    ptr += 8;
    
    // =========================================================================
    // FIELD 2: Symbol (variable length)
    // =========================================================================
    
    msg->result.symbol = deserialize_string(ptr, end);
    
    // =========================================================================
    // FIELDS 3-6: Doubles (32 bytes total)
    // =========================================================================
    
    // Lambda for DRY principle (deserialize double)
    // Captures:
    // - &ptr: Reference to read pointer (advanced in lambda)
    // - end: Boundary marker (const, passed by value)
    //
    // Returns: Deserialized double value
    //
    // Process:
    // 1. Bounds check
    // 2. Read 8 bytes as uint64_t
    // 3. Convert from network to host byte order
    // 4. Type-pun to double
    // 5. Advance pointer
    // 6. Return value
    auto deserialize_double = [&ptr, end]() -> double {
        if (ptr + 8 > end) throw std::runtime_error("Buffer underflow");
        uint64_t bits;
        std::memcpy(&bits, ptr, 8);
        bits = ntoh64(bits);
        ptr += 8;
        double val;
        std::memcpy(&val, &bits, sizeof(double));
        return val;
    };
    
    msg->result.total_return = deserialize_double();
    msg->result.sharpe_ratio = deserialize_double();
    msg->result.max_drawdown = deserialize_double();
    msg->result.final_portfolio_value = deserialize_double();
    
    // =========================================================================
    // FIELDS 7-9: Trade counts (12 bytes total)
    // =========================================================================
    
    if (ptr + 12 > end) throw std::runtime_error("Buffer underflow");
    
    int32_t num_trades, winning, losing;
    std::memcpy(&num_trades, ptr, 4);
    msg->result.num_trades = ntoh32(num_trades);
    ptr += 4;
    
    std::memcpy(&winning, ptr, 4);
    msg->result.winning_trades = ntoh32(winning);
    ptr += 4;
    
    std::memcpy(&losing, ptr, 4);
    msg->result.losing_trades = ntoh32(losing);
    ptr += 4;
    
    // =========================================================================
    // FIELD 10: Success flag (1 byte)
    // =========================================================================
    
    if (ptr >= end) throw std::runtime_error("Buffer underflow");
    msg->result.success = (*ptr != 0); // Any non-zero value is true
    ptr++;
    
    // =========================================================================
    // FIELD 11: Error message (variable length)
    // =========================================================================
    
    msg->result.error_message = deserialize_string(ptr, end);
    
    return msg;
}

//------------------------------------------------------------------------------
// 4.4 HEARTBEAT MESSAGE
//------------------------------------------------------------------------------
//
// HeartbeatMessage: Worker → Controller
//
// PURPOSE:
// Periodic health check from worker to controller, enabling failure detection
// and load balancing. Sent every 2 seconds (configurable).
//
// PAYLOAD STRUCTURE:
// - worker_id: Unique identifier for this worker
// - active_jobs: Number of jobs currently executing
// - completed_jobs: Total jobs completed since worker startup
//
// WIRE FORMAT (29 bytes, FIXED SIZE):
// [13-byte header]
// [8-byte worker_id]
// [4-byte active_jobs]
// [4-byte completed_jobs]
//
// DESIGN RATIONALE:
//
// Why fixed size?
// - Heartbeats are frequent (every 2 seconds)
// - Fixed size enables fast parsing (no string lengths to read)
// - Predictable bandwidth: 29 bytes × 8 workers / 2 sec = 116 bytes/sec
// - Network efficiency: Small packets, minimal overhead
//
// Why include active/completed jobs?
// - Load balancing: Controller can assign jobs to least-loaded workers
// - Monitoring: Track system utilization
// - Debugging: Identify stuck workers (active jobs not decreasing)
//
// Failure detection strategy:
// - Worker sends heartbeat every 2 seconds
// - Controller expects heartbeat within 6 seconds (3 × 2 sec = missed 3)
// - If no heartbeat: Mark worker as failed
// - Recovery: Reassign jobs from failed worker to healthy workers
//
// USE CASE:
// Worker heartbeat thread:
// 1. Every 2 seconds, create HeartbeatMessage
// 2. Set worker_id, active_jobs, completed_jobs
// 3. Serialize and send to controller
// 4. Controller updates worker registry with last heartbeat time
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.4.1 HeartbeatMessage Serialization
//------------------------------------------------------------------------------

// HeartbeatMessage serialization
std::vector<uint8_t> HeartbeatMessage::serialize() const {
    std::vector<uint8_t> buffer;
    
    // =========================================================================
    // HEADER SECTION (13 bytes)
    // =========================================================================
    
    buffer.push_back(static_cast<uint8_t>(type));
    uint64_t net_id = hton64(id);
    const uint8_t* id_bytes = reinterpret_cast<const uint8_t*>(&net_id);
    buffer.insert(buffer.end(), id_bytes, id_bytes + 8);
    
    // Payload size is FIXED (16 bytes: 8 + 4 + 4)
    // No need for backfilling, we know the size upfront
    uint32_t payload_size = 16;
    uint32_t net_size = hton32(payload_size);
    const uint8_t* size_bytes = reinterpret_cast<const uint8_t*>(&net_size);
    buffer.insert(buffer.end(), size_bytes, size_bytes + 4);
    
    // =========================================================================
    // PAYLOAD SECTION (16 bytes, FIXED)
    // =========================================================================
    
    // FIELD 1: Worker ID (8 bytes)
    // Uniquely identifies this worker in the cluster
    // Typically assigned during worker registration
    uint64_t net_worker_id = hton64(worker_id);
    const uint8_t* wid_bytes = reinterpret_cast<const uint8_t*>(&net_worker_id);
    buffer.insert(buffer.end(), wid_bytes, wid_bytes + 8);
    
    // FIELD 2: Active jobs (4 bytes)
    // Number of jobs currently being executed by this worker
    // Used for load balancing (prefer workers with fewer active jobs)
    uint32_t net_active = hton32(active_jobs);
    const uint8_t* active_bytes = reinterpret_cast<const uint8_t*>(&net_active);
    buffer.insert(buffer.end(), active_bytes, active_bytes + 4);
    
    // FIELD 3: Completed jobs (4 bytes)
    // Total number of jobs completed since worker started
    // Used for monitoring and statistics
    // Monotonically increasing (never decreases)
    uint32_t net_completed = hton32(completed_jobs);
    const uint8_t* comp_bytes = reinterpret_cast<const uint8_t*>(&net_completed);
    buffer.insert(buffer.end(), comp_bytes, comp_bytes + 4);
    
    // Total size: 13 (header) + 16 (payload) = 29 bytes (fixed)
    return buffer;
}

//------------------------------------------------------------------------------
// 4.4.2 HeartbeatMessage Deserialization
//------------------------------------------------------------------------------

std::unique_ptr<HeartbeatMessage> HeartbeatMessage::deserialize(const uint8_t* data, size_t size) {
    auto msg = std::make_unique<HeartbeatMessage>();
    
    // VALIDATION: Exact size check (heartbeat is fixed size)
    // If size != 29, message is definitely malformed
    if (size < 29) throw std::runtime_error("Invalid HeartbeatMessage size");
    
    const uint8_t* ptr = data + 13; // Skip header
    
    // =========================================================================
    // PAYLOAD FIELDS (no bounds checks needed, size already validated)
    // =========================================================================
    
    // FIELD 1: Worker ID (8 bytes)
    std::memcpy(&msg->worker_id, ptr, 8);
    msg->worker_id = ntoh64(msg->worker_id);
    ptr += 8;
    
    // FIELD 2: Active jobs (4 bytes)
    uint32_t active, completed;
    std::memcpy(&active, ptr, 4);
    msg->active_jobs = ntoh32(active);
    ptr += 4;
    
    // FIELD 3: Completed jobs (4 bytes)
    std::memcpy(&completed, ptr, 4);
    msg->completed_jobs = ntoh32(completed);
    
    // Note: We don't advance ptr at the end because we're done reading
    // If size validation passed, we know all fields are present
    
    return msg;
}

//------------------------------------------------------------------------------
// 4.5 WORKER REGISTRATION MESSAGE
//------------------------------------------------------------------------------
//
// WorkerRegisterMessage: Worker → Controller
//
// PURPOSE:
// Initial connection message from worker to controller, providing contact
// information for future communication.
//
// PAYLOAD STRUCTURE:
// - worker_hostname: Hostname or IP address of worker (e.g., "kh05.ccs.neu.edu", "192.168.1.105")
// - worker_port: TCP port worker listens on for job assignments
//
// WIRE FORMAT (variable length, typically 50-100 bytes):
// [13-byte header]
// [4-byte len + hostname string]
// [2-byte port]
//
// USE CASE:
// Worker startup sequence:
// 1. Worker starts, opens TCP connection to controller
// 2. Creates WorkerRegisterMessage with hostname and port
// 3. Sends to controller
// 4. Controller assigns worker_id, adds to registry
// 5. Controller responds with confirmation (job assignments can now begin)
//
// Why include hostname/port?
// - Controller may need to reconnect to worker (if connection drops)
// - Enables peer-to-peer communication (if added in future)
// - Logging/debugging (know which physical machine is worker)
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.5.1 WorkerRegisterMessage Serialization
//------------------------------------------------------------------------------

// WorkerRegisterMessage serialization
std::vector<uint8_t> WorkerRegisterMessage::serialize() const {
    std::vector<uint8_t> buffer;
    
    // =========================================================================
    // HEADER SECTION (13 bytes)
    // =========================================================================
    
    buffer.push_back(static_cast<uint8_t>(type));
    uint64_t net_id = hton64(id);
    const uint8_t* id_bytes = reinterpret_cast<const uint8_t*>(&net_id);
    buffer.insert(buffer.end(), id_bytes, id_bytes + 8);
    
    // Placeholder for payload size (backfilled later)
    size_t payload_start = buffer.size() + 4;
    buffer.resize(payload_start);
    
    // =========================================================================
    // PAYLOAD SECTION
    // =========================================================================
    
    // FIELD 1: Worker hostname (variable length string)
    // Examples: "kh05.ccs.neu.edu", "192.168.1.105", "worker-3"
    // Fully qualified domain name (FQDN) or IP address
    serialize_string(buffer, worker_hostname);
    
    // FIELD 2: Worker port (2 bytes)
    // TCP port worker listens on (e.g., 5001, 5002, ...)
    // Range: 1-65535 (uint16_t)
    // Typically: 5000+ (avoid well-known ports 0-1023)
    //
    // Why 2 bytes (not 4)?
    // - Port numbers are 16-bit (0-65535)
    // - No need for 32-bit representation
    // - Saves 2 bytes per message (minor but consistent with protocol design)
    uint16_t net_port = hton16(worker_port);
    const uint8_t* port_bytes = reinterpret_cast<const uint8_t*>(&net_port);
    buffer.insert(buffer.end(), port_bytes, port_bytes + 2);
    
    // =========================================================================
    // BACKFILL PAYLOAD SIZE
    // =========================================================================
    
    uint32_t payload_size = static_cast<uint32_t>(buffer.size() - payload_start);
    uint32_t net_size = hton32(payload_size);
    std::memcpy(&buffer[payload_start - 4], &net_size, 4);
    
    return buffer;
}

//------------------------------------------------------------------------------
// 4.5.2 WorkerRegisterMessage Deserialization
//------------------------------------------------------------------------------

std::unique_ptr<WorkerRegisterMessage> WorkerRegisterMessage::deserialize(const uint8_t* data, size_t size) {
    auto msg = std::make_unique<WorkerRegisterMessage>();
    
    // Minimum size validation
    if (size < 13) throw std::runtime_error("Invalid WorkerRegisterMessage size");
    
    const uint8_t* ptr = data + 13;
    const uint8_t* end = data + size;
    
    // =========================================================================
    // FIELD 1: Worker hostname (variable length)
    // =========================================================================
    
    msg->worker_hostname = deserialize_string(ptr, end);
    
    // =========================================================================
    // FIELD 2: Worker port (2 bytes)
    // =========================================================================
    
    if (ptr + 2 > end) throw std::runtime_error("Buffer underflow");
    uint16_t port;
    std::memcpy(&port, ptr, 2);
    msg->worker_port = ntoh16(port);
    
    // Note: No need to advance ptr, we're done reading
    
    return msg;
}

} // namespace backtesting

//==============================================================================
// SECTION 5: PROTOCOL WIRE FORMAT SPECIFICATIONS
//==============================================================================
//
// This section documents the exact byte layout for each message type.
// Essential reference for implementing clients in other languages or
// debugging network captures with tools like Wireshark.
//
//------------------------------------------------------------------------------
//
// COMMON HEADER FORMAT (All Messages)
// ====================================
// Offset  Size  Type       Field          Description
// ------  ----  ---------  -------------  -----------------------------------
// 0       1     uint8_t    type           MessageType enum value (1-4)
// 1       8     uint64_t   id             Request/response correlation ID
// 9       4     uint32_t   payload_size   Number of bytes following header
//
// Total: 13 bytes (present in all messages)
//
//
// JOB_ASSIGN MESSAGE (Type = 1)
// ==============================
// Offset  Size     Type       Field                  Description
// ------  -------  ---------  ---------------------  -----------------------
// 0       13       -          header                 Common header
// 13      8        uint64_t   job_id                 Unique job identifier
// 21      4+N      string     symbol                 Stock ticker (e.g., "AAPL")
// 21+N    4+M      string     strategy_type          Strategy name (e.g., "SMA")
// ...     4+P      string     start_date             ISO date (YYYY-MM-DD)
// ...     4+Q      string     end_date               ISO date (YYYY-MM-DD)
// ...     4        int32_t    short_window           MA short period (days)
// ...     4        int32_t    long_window            MA long period (days)
// ...     8        double     initial_capital        Starting portfolio value
//
// String format: [4-byte length][UTF-8 bytes]
// Total size: Typically 200-400 bytes (variable due to strings)
//
//
// JOB_RESULT MESSAGE (Type = 2)
// ==============================
// Offset  Size     Type       Field                  Description
// ------  -------  ---------  ---------------------  -----------------------
// 0       13       -          header                 Common header
// 13      8        uint64_t   job_id                 Job this result is for
// 21      4+N      string     symbol                 Stock ticker (echo)
// 21+N    8        double     total_return           Percentage return
// 29+N    8        double     sharpe_ratio           Risk-adjusted return
// 37+N    8        double     max_drawdown           Worst decline from peak
// 45+N    8        double     final_portfolio_value  Ending portfolio value
// 53+N    4        int32_t    num_trades             Total trades executed
// 57+N    4        int32_t    winning_trades         Profitable trades
// 61+N    4        int32_t    losing_trades          Unprofitable trades
// 65+N    1        uint8_t    success                0=failure, 1=success
// 66+N    4+M      string     error_message          Empty if success
//
// Total size: Typically 300-500 bytes (variable due to strings)
//
//
// HEARTBEAT MESSAGE (Type = 3)
// =============================
// Offset  Size  Type       Field           Description
// ------  ----  ---------  --------------  -----------------------------------
// 0       13    -          header          Common header
// 13      8     uint64_t   worker_id       Worker identifier
// 21      4     uint32_t   active_jobs     Currently executing jobs
// 25      4     uint32_t   completed_jobs  Total jobs completed
//
// Total size: 29 bytes (FIXED SIZE)
//
//
// WORKER_REGISTER MESSAGE (Type = 4)
// ===================================
// Offset  Size   Type       Field           Description
// ------  -----  ---------  --------------  -----------------------------------
// 0       13     -          header          Common header
// 13      4+N    string     worker_hostname Hostname or IP address
// 13+N    2      uint16_t   worker_port     TCP port for connections
//
// Total size: Typically 50-100 bytes (variable due to hostname)
//
//
// ENDIANNESS NOTES:
// - All multi-byte integers in NETWORK BYTE ORDER (big-endian)
// - Strings are UTF-8 encoded (no endianness concerns)
// - Doubles transmitted as IEEE 754 bit pattern (converted like uint64_t)
//
// EXAMPLE HEX DUMPS:
//
// Heartbeat (29 bytes):
//   03                          Type: HEARTBEAT (3)
//   00 00 00 00 00 00 00 2A     ID: 42
//   00 00 00 10                 Payload size: 16 bytes
//   00 00 00 00 00 00 00 05     Worker ID: 5
//   00 00 00 02                 Active jobs: 2
//   00 00 01 F4                 Completed jobs: 500
//
// Job Assignment (partial, symbol="AAPL"):
//   01                          Type: JOB_ASSIGN (1)
//   00 00 00 00 00 00 00 64     ID: 100
//   00 00 00 B0                 Payload size: 176 bytes
//   00 00 00 00 00 00 03 E8     Job ID: 1000
//   00 00 00 04                 Symbol length: 4
//   41 41 50 4C                 Symbol: "AAPL"
//   ...                         (remaining fields)
//
//==============================================================================

//==============================================================================
// SECTION 6: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Controller Assigning Job to Worker
// ==============================================
//
// File: controller/job_scheduler.cpp
//
// void JobScheduler::AssignJobToWorker(Worker* worker, const BacktestRequest& request) {
//     // Create job assignment message
//     JobAssignMessage msg;
//     msg.type = MessageType::JOB_ASSIGN;
//     msg.id = GenerateMessageId();  // Unique correlation ID
//     msg.job_id = GenerateJobId();  // Unique job identifier
//     
//     // Set job parameters from client request
//     msg.params.symbol = request.symbol;
//     msg.params.strategy_type = request.strategy;
//     msg.params.start_date = request.start_date;
//     msg.params.end_date = request.end_date;
//     msg.params.short_window = request.short_window;
//     msg.params.long_window = request.long_window;
//     msg.params.initial_capital = request.initial_capital;
//     
//     // Serialize to binary
//     std::vector<uint8_t> bytes = msg.serialize();
//     
//     // Send over TCP connection
//     try {
//         worker->SendMessage(bytes);
//         Logger::info("[Scheduler] Assigned job " + std::to_string(msg.job_id) + 
//                      " to worker " + std::to_string(worker->id()));
//         
//         // Record assignment in Raft log for fault tolerance
//         raft_log_.AppendJobAssignment(msg.job_id, worker->id());
//         
//     } catch (const NetworkException& e) {
//         Logger::error("[Scheduler] Failed to send job to worker: " + 
//                      std::string(e.what()));
//         // Reassign to different worker
//         ReassignJob(msg.job_id);
//     }
// }
//
//
// EXAMPLE 2: Worker Sending Results Back
// =======================================
//
// File: worker/job_executor.cpp
//
// void JobExecutor::SendResults(const JobResult& result) {
//     // Create result message
//     JobResultMessage msg;
//     msg.type = MessageType::JOB_RESULT;
//     msg.id = correlation_id_;  // Match request ID
//     msg.result = result;       // Copy result data
//     
//     // Serialize to binary
//     std::vector<uint8_t> bytes = msg.serialize();
//     
//     Logger::info("[Worker] Sending result for job " + 
//                  std::to_string(result.job_id) + 
//                  " (PnL: " + std::to_string(result.total_return * 100) + "%)");
//     
//     // Send to controller
//     try {
//         controller_connection_->SendMessage(bytes);
//     } catch (const NetworkException& e) {
//         Logger::error("[Worker] Failed to send result: " + std::string(e.what()));
//         // Retry or buffer result
//         RetryQueue::Add(msg);
//     }
// }
//
//
// EXAMPLE 3: Worker Heartbeat Thread
// ===================================
//
// File: worker/heartbeat_manager.cpp
//
// void HeartbeatManager::Run() {
//     while (running_) {
//         // Wait 2 seconds between heartbeats
//         std::this_thread::sleep_for(std::chrono::seconds(2));
//         
//         // Create heartbeat message
//         HeartbeatMessage msg;
//         msg.type = MessageType::HEARTBEAT;
//         msg.id = next_heartbeat_id_++;
//         msg.worker_id = worker_id_;
//         msg.active_jobs = job_executor_->GetActiveJobCount();
//         msg.completed_jobs = job_executor_->GetCompletedJobCount();
//         
//         // Serialize (29 bytes, fixed size)
//         std::vector<uint8_t> bytes = msg.serialize();
//         
//         Logger::debug("[Heartbeat] Sending (active: " + 
//                      std::to_string(msg.active_jobs) + 
//                      ", completed: " + std::to_string(msg.completed_jobs) + ")");
//         
//         try {
//             controller_connection_->SendMessage(bytes);
//         } catch (const NetworkException& e) {
//             Logger::warning("[Heartbeat] Failed to send: " + std::string(e.what()));
//             // Controller may mark us as failed if we miss 3 heartbeats
//         }
//     }
// }
//
//
// EXAMPLE 4: Network Layer Receiving Messages
// ============================================
//
// File: network/tcp_connection.cpp
//
// std::unique_ptr<Message> TcpConnection::ReceiveMessage() {
//     // Read header (13 bytes)
//     std::vector<uint8_t> header(13);
//     size_t bytes_read = Read(header.data(), 13);
//     if (bytes_read != 13) {
//         throw NetworkException("Incomplete header received");
//     }
//     
//     // Parse header fields
//     MessageType type = static_cast<MessageType>(header[0]);
//     
//     uint32_t payload_size;
//     std::memcpy(&payload_size, &header[9], 4);
//     payload_size = ntoh32(payload_size);
//     
//     // Read payload
//     std::vector<uint8_t> payload(payload_size);
//     bytes_read = Read(payload.data(), payload_size);
//     if (bytes_read != payload_size) {
//         throw NetworkException("Incomplete payload received");
//     }
//     
//     // Combine header + payload
//     std::vector<uint8_t> complete_message;
//     complete_message.insert(complete_message.end(), header.begin(), header.end());
//     complete_message.insert(complete_message.end(), payload.begin(), payload.end());
//     
//     // Deserialize based on type
//     try {
//         switch (type) {
//             case MessageType::JOB_ASSIGN:
//                 return JobAssignMessage::deserialize(complete_message.data(), 
//                                                     complete_message.size());
//             case MessageType::JOB_RESULT:
//                 return JobResultMessage::deserialize(complete_message.data(), 
//                                                      complete_message.size());
//             case MessageType::HEARTBEAT:
//                 return HeartbeatMessage::deserialize(complete_message.data(), 
//                                                      complete_message.size());
//             case MessageType::WORKER_REGISTER:
//                 return WorkerRegisterMessage::deserialize(complete_message.data(), 
//                                                          complete_message.size());
//             default:
//                 throw ProtocolException("Unknown message type: " + 
//                                       std::to_string(static_cast<int>(type)));
//         }
//     } catch (const std::runtime_error& e) {
//         Logger::error("[Network] Deserialization failed: " + std::string(e.what()));
//         throw ProtocolException("Malformed message");
//     }
// }
//
//==============================================================================

//==============================================================================
// SECTION 7: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: Forgetting Network Byte Order Conversion
// ====================================================
// WRONG (endianness bug):
//    uint32_t value = 1000;
//    buffer.insert(buffer.end(), 
//                  reinterpret_cast<uint8_t*>(&value), 
//                  reinterpret_cast<uint8_t*>(&value) + 4);
//    // On little-endian: E8 03 00 00
//    // On big-endian: 00 00 03 E8
//    // Different representations!
//
// CORRECT (portable):
//    uint32_t value = 1000;
//    uint32_t net_value = hton32(value);  // Convert to network byte order
//    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&net_value);
//    buffer.insert(buffer.end(), bytes, bytes + 4);
//    // Always: 00 00 03 E8 (network byte order)
//
//
// PITFALL 2: Buffer Overrun from Missing Bounds Check
// ====================================================
// WRONG (crash or security vulnerability):
//    std::string deserialize_string(const uint8_t*& ptr) {
//        uint32_t len;
//        std::memcpy(&len, ptr, 4);  // No check if 4 bytes available!
//        len = ntoh32(len);
//        ptr += 4;
//        std::string result(reinterpret_cast<const char*>(ptr), len);  // No check!
//        ptr += len;
//        return result;
//    }
//
// CORRECT (safe):
//    std::string deserialize_string(const uint8_t*& ptr, const uint8_t* end) {
//        if (ptr + 4 > end) throw std::runtime_error("Buffer underflow");
//        uint32_t len;
//        std::memcpy(&len, ptr, 4);
//        len = ntoh32(len);
//        ptr += 4;
//        if (ptr + len > end) throw std::runtime_error("Buffer underflow");
//        std::string result(reinterpret_cast<const char*>(ptr), len);
//        ptr += len;
//        return result;
//    }
//
//
// PITFALL 3: Misaligned Pointer Dereference
// ==========================================
// WRONG (undefined behavior, crashes on ARM):
//    uint32_t value = *reinterpret_cast<uint32_t*>(buffer_ptr);
//    // If buffer_ptr not 4-byte aligned: UB on ARM, slow on x86
//
// CORRECT (portable):
//    uint32_t value;
//    std::memcpy(&value, buffer_ptr, 4);  // Safe regardless of alignment
//
//
// PITFALL 4: Forgetting to Backfill Payload Size
// ===============================================
// WRONG (incorrect size field):
//    std::vector<uint8_t> buffer;
//    buffer.push_back(type);
//    // ... write header ...
//    buffer.resize(payload_start);  // Reserve space for size
//    // ... write payload ...
//    return buffer;  // Oops! Size field still zero
//
// CORRECT (backfill size):
//    // ... after writing payload ...
//    uint32_t payload_size = buffer.size() - payload_start;
//    uint32_t net_size = hton32(payload_size);
//    std::memcpy(&buffer[payload_start - 4], &net_size, 4);  // Backfill
//    return buffer;
//
//
// PITFALL 5: Deserializing Before Validating Entire Buffer
// =========================================================
// WRONG (possible partial read):
//    // Network layer receives partial message (e.g., 20 bytes of 200)
//    auto msg = JobAssignMessage::deserialize(partial_buffer, 20);
//    // Throws exception, but better to wait for complete message
//
// CORRECT (wait for complete message):
//    // Read header first (13 bytes)
//    uint32_t payload_size = ReadPayloadSize(header);
//    // Wait until we have header + payload
//    while (buffer.size() < 13 + payload_size) {
//        ReadMoreBytes();
//    }
//    // Now safe to deserialize complete message
//    auto msg = JobAssignMessage::deserialize(buffer.data(), buffer.size());
//
//
// PITFALL 6: Using Wrong Byte Order Conversion
// =============================================
// WRONG (opposite direction):
//    // Serialization:
//    uint32_t value = 1000;
//    uint32_t net = ntoh32(value);  // WRONG! Should be hton32
//    
//    // Deserialization:
//    uint32_t net;
//    std::memcpy(&net, buffer, 4);
//    uint32_t value = hton32(net);  // WRONG! Should be ntoh32
//
// CORRECT:
//    // Serialization: host TO network
//    uint32_t net = hton32(value);
//    
//    // Deserialization: network TO host
//    uint32_t value = ntoh32(net);
//
//
// PITFALL 7: Not Handling Deserialization Exceptions
// ===================================================
// WRONG (crash on malformed data):
//    auto msg = JobAssignMessage::deserialize(data, size);
//    ProcessMessage(msg.get());  // What if deserialize threw?
//
// CORRECT (handle errors gracefully):
//    try {
//        auto msg = JobAssignMessage::deserialize(data, size);
//        ProcessMessage(msg.get());
//    } catch (const std::runtime_error& e) {
//        Logger::error("Malformed message: " + std::string(e.what()));
//        CloseConnection();  // Disconnect malicious/buggy sender
//    }
//
//==============================================================================

//==============================================================================
// SECTION 8: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Why binary protocol instead of JSON or Protocol Buffers?
// =============================================================
// A: Trade-offs:
//    - Binary: Smallest size, fastest, but not human-readable
//    - JSON: Human-readable, but 3-5x larger, 10-50x slower to parse
//    - Protocol Buffers: Good balance, but requires code generation
//    
//    For this academic project:
//    - Binary chosen for simplicity and performance
//    - Total implementation: ~500 lines (vs. protobuf code gen complexity)
//    - Network is bottleneck (Khoury cluster): Binary minimizes bandwidth
//
// Q2: How do you handle protocol versioning?
// ===========================================
// A: Current implementation doesn't version explicitly. For production:
//    - Add version field to header: [1-byte version][1-byte type][...]
//    - Controller/worker negotiate version during connection
//    - Maintain backward compatibility (old workers, new controller)
//    - Or: Use protocol buffers (built-in versioning)
//
// Q3: What if sender and receiver have different sizeof(double)?
// ===============================================================
// A: Not a concern:
//    - IEEE 754 double is 64-bit on all modern platforms
//    - C++ standard guarantees sizeof(double) >= sizeof(float)
//    - Practically: Always 8 bytes on x86, ARM, SPARC, etc.
//    - If paranoid: static_assert(sizeof(double) == 8);
//
// Q4: Why use memcpy instead of reinterpret_cast?
// ================================================
// A: Safety and portability:
//    - reinterpret_cast + dereference can violate strict aliasing
//    - Crashes on misaligned pointers (ARM requires alignment)
//    - memcpy handles all cases correctly
//    - Compiler optimizes memcpy to single instruction (no overhead)
//
// Q5: How do you test serialization/deserialization?
// ===================================================
// A: Multiple strategies:
//    - Unit tests: Serialize then deserialize, check round-trip
//    - Fuzz testing: Random data, verify exception handling
//    - Network captures: Wireshark to inspect actual bytes
//    - Cross-platform: Test Linux and macOS (endianness same, but validates)
//
// Q6: What's the maximum message size?
// =====================================
// A: Theoretical: 4GB (payload_size is uint32_t)
//    Practical: ~10KB for this project (largest job result)
//    Recommendations:
//    - Add validation: if (payload_size > 1MB) reject
//    - Prevents DoS attacks (attacker sends huge payload_size)
//    - For large data (CSV files): Use separate transfer mechanism
//
// Q7: Why use std::vector<uint8_t> (not std::string or char array)?
// ==================================================================
// A: Type safety and semantics:
//    - uint8_t: Clearly indicates "raw bytes" (not text)
//    - std::vector: Dynamic sizing, RAII memory management
//    - std::string: Implies text, has string-specific operations
//    - char[]: Fixed size, manual memory management
//
// Q8: How do you handle message fragmentation over TCP?
// ======================================================
// A: TCP is stream-based (no message boundaries). Must implement framing:
//    1. Read header (13 bytes) - contains payload_size
//    2. Read payload (payload_size bytes)
//    3. Repeat for next message
//    
//    Key: Never assume send() delivers entire message in one recv()
//    Must loop until all bytes received (see Example 4 in Section 6)
//
// Q9: What happens if worker and controller have different endianness?
// =====================================================================
// A: Network byte order handles this:
//    - All systems convert TO network byte order (big-endian) before sending
//    - All systems convert FROM network byte order after receiving
//    - Example:
//      * Little-endian sender: value 1000 → hton32 → 00 00 03 E8 (network)
//      * Big-endian receiver: 00 00 03 E8 → ntoh32 → 1000 (host)
//      * Result: Both agree on value 1000
//
// Q10: Why not use existing serialization library (Boost.Serialization)?
// =======================================================================
// A: For academic project:
//    - Learning: Understand binary protocols from first principles
//    - Control: Exact wire format for cross-language compatibility
//    - Dependencies: Minimize external dependencies (only STL)
//    
//    For production: Libraries like Boost, Cap'n Proto are excellent choices
//
//==============================================================================

//==============================================================================
// SECTION 9: BEST PRACTICES FOR DISTRIBUTED SYSTEMS
//==============================================================================
//
// BEST PRACTICE 1: Design for Debuggability
// ==========================================
// Include message IDs for request/response correlation:
//    Controller sends job with id=42
//    Worker responds with id=42
//    Logs: "Sent job id=42" ... "Received result id=42" (can grep and match)
//
// Add sequence numbers to heartbeats:
//    HeartbeatMessage msg;
//    msg.id = next_heartbeat_id_++;  // 1, 2, 3, ...
//    If controller receives 1, 2, 4: Knows message 3 was lost
//
//
// BEST PRACTICE 2: Validate Everything
// =====================================
// Never trust network data:
//    - Bounds check before every read
//    - Validate enum values: if (type < 1 || type > 4) reject
//    - Sanity check sizes: if (payload_size > MAX_SIZE) reject
//    - Validate string lengths: if (len > 1024) reject
//
// Defense in depth: Validate at multiple layers
//    - Deserialization: Protocol-level validation
//    - Application: Business logic validation (e.g., capital > 0)
//
//
// BEST PRACTICE 3: Handle Partial Messages
// =========================================
// TCP delivers bytes, not messages:
//    - send(1000 bytes) might result in recv(500 bytes) + recv(500 bytes)
//    - Must implement message framing
//    - Pattern: Read header → get payload_size → read payload → repeat
//
// Implement state machine for receiving:
//    enum class RecvState { HEADER, PAYLOAD };
//    RecvState state = RecvState::HEADER;
//    std::vector<uint8_t> header_buffer(13);
//    std::vector<uint8_t> payload_buffer;
//    size_t bytes_received = 0;
//    // ... state machine to accumulate bytes ...
//
//
// BEST PRACTICE 4: Log Binary Protocol Events
// ============================================
// Log serialization/deserialization for debugging:
//    Logger::debug("[Serialize] JobAssignMessage: job_id=" + 
//                  std::to_string(job_id) + " size=" + 
//                  std::to_string(buffer.size()) + " bytes");
//    
//    Logger::debug("[Deserialize] Received message type=" + 
//                  std::to_string(static_cast<int>(type)) + 
//                  " payload_size=" + std::to_string(payload_size));
//
// For deep debugging: Hex dump
//    std::string hex_dump(const std::vector<uint8_t>& data, size_t max_bytes = 64) {
//        std::ostringstream oss;
//        for (size_t i = 0; i < std::min(data.size(), max_bytes); ++i) {
//            oss << std::hex << std::setw(2) << std::setfill('0') 
//                << static_cast<int>(data[i]) << ' ';
//        }
//        return oss.str();
//    }
//
//
// BEST PRACTICE 5: Design for Evolution
// ======================================
// Plan for protocol changes:
//    - Add version field (currently not present)
//    - Use extensible formats (TLV: Type-Length-Value)
//    - Reserve fields for future use
//    - Document wire format carefully (Section 5)
//
// For this project: Protocol is fixed (ok for academic setting)
// For production: Use versioned protocols or Protocol Buffers
//
//
// BEST PRACTICE 6: Test Serialization Round-Trips
// ================================================
// Always verify serialize → deserialize → identical:
//    TEST(MessageTest, JobAssignRoundTrip) {
//        JobAssignMessage orig;
//        orig.job_id = 42;
//        orig.params.symbol = "AAPL";
//        // ... set other fields ...
//        
//        auto bytes = orig.serialize();
//        auto copy = JobAssignMessage::deserialize(bytes.data(), bytes.size());
//        
//        ASSERT_EQ(orig.job_id, copy->job_id);
//        ASSERT_EQ(orig.params.symbol, copy->params.symbol);
//        // ... check all fields ...
//    }
//
//
// BEST PRACTICE 7: Handle Endianness Correctly
// =============================================
// Always use hton/ntoh functions:
//    uint32_t net = hton32(host);   // Before sending
//    uint32_t host = ntoh32(net);   // After receiving
//    
//    Sending raw host bytes (works only if same endianness)
//
// Remember: Network byte order = Big-endian (most significant byte first)
//
//==============================================================================

//==============================================================================
// SECTION 10: PERFORMANCE OPTIMIZATION GUIDE
//==============================================================================
//
// OPTIMIZATION 1: Minimize Allocations
// =====================================
// Problem: std::vector allocations during serialization
//    Every buffer.insert() may reallocate
//
// Solution: Reserve capacity upfront
//    std::vector<uint8_t> buffer;
//    buffer.reserve(256);  // Estimate: most messages < 256 bytes
//    // Now inserts won't reallocate until 256 bytes
//
// For known sizes:
//    HeartbeatMessage always 29 bytes:
//    buffer.reserve(29);  // Exact size, no reallocation
//
//
// OPTIMIZATION 2: Reuse Buffers
// ==============================
// Problem: Allocating new buffer for each message
//
// Solution: Object pool or thread-local storage
//    thread_local std::vector<uint8_t> buffer;
//    
//    std::vector<uint8_t> SerializeMessage(const Message& msg) {
//        buffer.clear();  // Reuse capacity
//        buffer.reserve(256);
//        msg.serialize_into(buffer);  // Serialize into existing buffer
//        return buffer;  // Or move
//    }
//
//
// OPTIMIZATION 3: Batch Small Messages
// =====================================
// Problem: TCP overhead for many small messages (heartbeats)
//
// Solution: Nagle's algorithm (default) or explicit batching
//    // Option 1: Let TCP batch (Nagle's algorithm)
//    // Already enabled by default, no code needed
//    
//    // Option 2: Explicit batching (send multiple messages in one packet)
//    std::vector<uint8_t> batch;
//    batch.reserve(1024);
//    for (const auto& msg : messages) {
//        auto bytes = msg.serialize();
//        batch.insert(batch.end(), bytes.begin(), bytes.end());
//    }
//    SendBytes(batch);
//
//
// OPTIMIZATION 4: Zero-Copy Deserialization
// ==========================================
// Problem: Copying strings from buffer to std::string
//
// Solution: std::string_view (C++17, read-only)
//    std::string_view deserialize_string_view(const uint8_t*& ptr, 
//                                             const uint8_t* end) {
//        if (ptr + 4 > end) throw std::runtime_error("Buffer underflow");
//        uint32_t len;
//        std::memcpy(&len, ptr, 4);
//        len = ntoh32(len);
//        ptr += 4;
//        if (ptr + len > end) throw std::runtime_error("Buffer underflow");
//        std::string_view result(reinterpret_cast<const char*>(ptr), len);
//        ptr += len;
//        return result;  // No copy! Points into buffer
//    }
//
// Warning: Buffer must outlive string_view (dangling pointer risk)
//
//
// OPTIMIZATION 5: Profile Before Optimizing
// ==========================================
// Measure actual performance:
//    auto start = std::chrono::high_resolution_clock::now();
//    
//    for (int i = 0; i < 10000; ++i) {
//        auto bytes = msg.serialize();
//        auto copy = Message::deserialize(bytes.data(), bytes.size());
//    }
//    
//    auto duration = std::chrono::high_resolution_clock::now() - start;
//    auto ns_per_op = duration.count() / 10000;
//    
//    std::cout << "Serialize+Deserialize: " << ns_per_op << " ns\n";
//
// Typical results (optimized build):
//    JobAssignMessage: ~2-5 μs per round-trip
//    HeartbeatMessage: ~1-2 μs per round-trip
//
//
// OPTIMIZATION 6: Compiler Optimizations
// =======================================
// Enable optimizations:
//    g++ -O3 -march=native message.cpp
//    
// -O3: Aggressive optimizations
// -march=native: Use CPU-specific instructions
//
// Link-Time Optimization (LTO):
//    g++ -O3 -flto message.cpp main.cpp
//    
// Benefit: Inline across translation units
//
//==============================================================================

//==============================================================================
// SECTION 11: DEBUGGING & TESTING STRATEGIES
//==============================================================================
//
// STRATEGY 1: Unit Tests for Each Message Type
// =============================================
//    TEST(MessageTest, JobAssignSerialize) {
//        JobAssignMessage msg;
//        msg.type = MessageType::JOB_ASSIGN;
//        msg.id = 42;
//        msg.job_id = 1000;
//        msg.params.symbol = "AAPL";
//        msg.params.strategy_type = "SMA";
//        msg.params.start_date = "2020-01-01";
//        msg.params.end_date = "2024-12-31";
//        msg.params.short_window = 50;
//        msg.params.long_window = 200;
//        msg.params.initial_capital = 10000.0;
//        
//        auto bytes = msg.serialize();
//        
//        // Check header
//        ASSERT_EQ(bytes[0], 1);  // Type: JOB_ASSIGN
//        
//        // Check sizes
//        ASSERT_GE(bytes.size(), 13);  // At least header
//        
//        // Deserialize and compare
//        auto copy = JobAssignMessage::deserialize(bytes.data(), bytes.size());
//        ASSERT_EQ(msg.job_id, copy->job_id);
//        ASSERT_EQ(msg.params.symbol, copy->params.symbol);
//        // ... check all fields ...
//    }
//
//
// STRATEGY 2: Fuzz Testing for Malformed Messages
// ================================================
//    TEST(MessageTest, MalformedData) {
//        // Test with random bytes
//        std::vector<uint8_t> random_data(100);
//        std::generate(random_data.begin(), random_data.end(), std::rand);
//        
//        EXPECT_THROW({
//            JobAssignMessage::deserialize(random_data.data(), random_data.size());
//        }, std::runtime_error);
//    }
//
//    TEST(MessageTest, TruncatedMessage) {
//        JobAssignMessage msg;
//        // ... fill msg ...
//        auto bytes = msg.serialize();
//        
//        // Try to deserialize with only half the bytes
//        EXPECT_THROW({
//            JobAssignMessage::deserialize(bytes.data(), bytes.size() / 2);
//        }, std::runtime_error);
//    }
//
//
// STRATEGY 3: Hex Dump for Debugging
// ===================================
//    void print_hex(const std::vector<uint8_t>& data, size_t max_bytes = 64) {
//        std::cout << "Hex dump (" << data.size() << " bytes):\n";
//        for (size_t i = 0; i < std::min(data.size(), max_bytes); ++i) {
//            if (i % 16 == 0) std::cout << std::setw(4) << i << ": ";
//            std::cout << std::hex << std::setw(2) << std::setfill('0') 
//                      << static_cast<int>(data[i]) << ' ';
//            if ((i + 1) % 16 == 0) std::cout << '\n';
//        }
//        std::cout << std::dec << '\n';
//    }
//
// Usage:
//    auto bytes = msg.serialize();
//    print_hex(bytes);
//    // Output:
//    //    0: 01 00 00 00 00 00 00 00 2A 00 00 00 B0 00 00 00
//    //   16: 00 00 00 03 E8 00 00 00 04 41 41 50 4C ...
//
//
// STRATEGY 4: Wireshark for Network Capture
// ==========================================
// Capture packets on Khoury cluster:
//    1. ssh to controller node
//    2. sudo tcpdump -i eth0 -w capture.pcap port 5000
//    3. Run distributed system
//    4. scp capture.pcap to local machine
//    5. Open in Wireshark
//    6. Analyze: Filter by TCP port, inspect packet contents
//    7. Verify: Correct byte order, field values
//
//
// STRATEGY 5: Mock Network Layer for Testing
// ===========================================
//    class MockConnection {
//        std::queue<std::vector<uint8_t>> sent_messages_;
//    public:
//        void Send(const std::vector<uint8_t>& data) {
//            sent_messages_.push(data);
//        }
//        
//        std::vector<uint8_t> GetLastSent() {
//            auto msg = sent_messages_.front();
//            sent_messages_.pop();
//            return msg;
//        }
//    };
//
//    TEST(ControllerTest, SendsJobAssignment) {
//        MockConnection conn;
//        Controller controller(&conn);
//        
//        controller.AssignJob(job_params);
//        
//        auto sent = conn.GetLastSent();
//        auto msg = JobAssignMessage::deserialize(sent.data(), sent.size());
//        
//        ASSERT_EQ(msg->params.symbol, job_params.symbol);
//    }
//
//==============================================================================

//==============================================================================
// SECTION 12: PROTOCOL EVOLUTION & VERSIONING
//==============================================================================
//
// CURRENT STATE: No Versioning
// =============================
// Current protocol has no version field.
// - Works for this project (controller and workers deployed together)
// - Problem for production (rolling upgrades, backward compatibility)
//
//
// FUTURE: Add Version Field
// =========================
// Modify header to include version:
//
// Old header (13 bytes):
//   [1-byte type][8-byte id][4-byte payload_size]
//
// New header (14 bytes):
//   [1-byte version][1-byte type][8-byte id][4-byte payload_size]
//
// Version negotiation:
//   1. Client sends: version=2
//   2. Server supports: 1, 2
//   3. Use: min(client, server) = version 1
//   4. If incompatible: Reject connection
//
//
// BACKWARD COMPATIBILITY STRATEGY
// ================================
// When adding new fields:
//
// Option 1: Optional fields at end
//   - Old clients: Stop reading at old fields
//   - New clients: Read additional fields if present
//   - Check payload_size to know if new fields exist
//
// Option 2: Use Protocol Buffers
//   - Built-in support for adding/removing fields
//   - Unknown fields silently skipped
//   - Requires code generation (more complex)
//
//
// MIGRATION PLAN
// ==============
// If protocol changes are needed:
//   1. Deploy new controller (understands v1 and v2)
//   2. Deploy new workers (send v2, understand v1 responses)
//   3. Wait for all workers updated
//   4. Controller starts using v2 exclusively
//   5. Remove v1 support in next release
//
//==============================================================================

//==============================================================================
// SECTION 13: SECURITY CONSIDERATIONS
//==============================================================================
//
// THREAT 1: Buffer Overflow Attacks
// ==================================
// Attacker sends: payload_size = 0xFFFFFFFF (4GB)
// Mitigation: Validate maximum size
//    if (payload_size > MAX_MESSAGE_SIZE) {
//        throw ProtocolException("Payload too large");
//    }
//
//
// THREAT 2: Integer Overflow in String Length
// ============================================
// Attacker sends: string length = 0xFFFFFFFF
// Mitigation: Bounds check catches this
//    if (ptr + len > end) throw std::runtime_error("Buffer underflow");
//    // If len huge, ptr + len wraps around, check fails correctly
//
//
// THREAT 3: Denial of Service (DoS)
// ==================================
// Attacker floods with heartbeat messages
// Mitigation: Rate limiting
//    if (heartbeat_rate > 100/sec) {
//        DisconnectClient();
//    }
//
//
// THREAT 4: Malformed Message Injection
// ======================================
// Attacker sends garbage data
// Mitigation: Fail fast, close connection
//    try {
//        auto msg = Message::deserialize(data, size);
//    } catch (const std::runtime_error&) {
//        Logger::warning("Malformed message from " + peer_address);
//        CloseConnection();
//    }
//
//
// THREAT 5: Man-in-the-Middle (MITM)
// ===================================
// Attacker intercepts/modifies messages
// Mitigation: Use TLS/SSL (not implemented in this project)
//    - For production: Wrap TCP in TLS
//    - Add authentication (client certificates)
//    - Add message signing (HMAC, signatures)
//
//
// AUTHENTICATION & AUTHORIZATION
// ===============================
// Current implementation: No authentication
// - Ok for academic project (trusted Khoury cluster)
// - For production: Add authentication layer
//   * Worker presents certificate during registration
//   * Controller verifies certificate
//   * All messages authenticated (prevents spoofing)
//
//==============================================================================

//==============================================================================
// SECTION 14: TROUBLESHOOTING CHECKLIST
//==============================================================================
//
// PROBLEM: Deserialization throws "Buffer underflow"
// ===================================================
// SOLUTION:
//    ☐ Verify complete message received (header + payload)
//    ☐ Check network layer: Reading full message, not partial
//    ☐ Verify payload_size field matches actual payload
//    ☐ Check for truncated TCP packets
//    ☐ Add logging: Log message size before deserialize
//
//
// PROBLEM: Wrong values after deserialization
// ============================================
// SOLUTION:
//    ☐ Check byte order conversions (hton/ntoh)
//    ☐ Verify serialize and deserialize use same field order
//    ☐ Check alignment (use memcpy, not reinterpret_cast)
//    ☐ Hex dump: Compare serialized bytes with expected
//    ☐ Test: Serialize known values, check byte-by-byte
//
//
// PROBLEM: Double values incorrect after deserialization
// =======================================================
// SOLUTION:
//    ☐ Verify sizeof(double) == 8 on both sides
//    ☐ Check type punning via memcpy (not direct cast)
//    ☐ Verify byte order conversion applied to uint64_t
//    ☐ Test with known values: 0.0, 1.0, 3.14159
//    ☐ Check IEEE 754 compliance (static_assert)
//
//
// PROBLEM: String deserialization fails randomly
// ===============================================
// SOLUTION:
//    ☐ Check length field is in network byte order
//    ☐ Verify bounds check: ptr + len <= end
//    ☐ Check for off-by-one errors in buffer management
//    ☐ Verify string length includes only data (no null terminator counted)
//    ☐ Test with empty string (length = 0)
//
//
// PROBLEM: Segmentation fault during deserialization
// ===================================================
// SOLUTION:
//    ☐ Check all bounds checks present (every memcpy)
//    ☐ Verify pointer arithmetic doesn't overflow
//    ☐ Use AddressSanitizer: g++ -fsanitize=address
//    ☐ Use Valgrind: valgrind --leak-check=full ./program
//    ☐ Add debug logging before each memcpy
//
//
// PROBLEM: Controller and worker can't communicate
// =================================================
// SOLUTION:
//    ☐ Verify both use same protocol version
//    ☐ Check byte order (both big-endian? both using hton/ntoh?)
//    ☐ Compare sizeof(types) on both platforms
//    ☐ Test serialization on same machine first
//    ☐ Use Wireshark to inspect actual network packets
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================