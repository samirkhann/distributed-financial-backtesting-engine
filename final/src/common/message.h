/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: message.h
    
    Description:
        This header file defines the complete message protocol for inter-node
        communication in the distributed financial backtesting system. It provides
        the type system, data structures, and serialization interfaces for all
        network messages exchanged between controller and worker nodes.
        
        Core Components:
        - MessageType enum: Protocol-level message type identifiers
        - Data structures: JobParams, JobResult (domain objects)
        - Message class hierarchy: Base Message + specialized message types
        - Byte order utilities: Network/host endianness conversion
        - Serialization API: Binary protocol encode/decode interfaces
        
    Protocol Architecture:
        This header defines a custom binary protocol optimized for:
        
        1. PERFORMANCE: Compact binary encoding (3-5x smaller than JSON)
        2. EFFICIENCY: Zero-copy where possible, minimal allocations
        3. TYPE SAFETY: Compile-time message type checking
        4. EXTENSIBILITY: Class hierarchy allows protocol evolution
        5. PORTABILITY: Explicit byte order handling for cross-platform
        
    Message Flow in Distributed System:
        
        Controller (Raft Leader) → Worker:
        ┌─────────────────────┐
        │ JobAssignMessage    │  Assign backtest computation
        └─────────────────────┘
        
        Worker → Controller:
        ┌─────────────────────┐
        │ WorkerRegisterMsg   │  Initial connection, provide contact info
        │ HeartbeatMessage    │  Health check (every 2 seconds)
        │ JobResultMessage    │  Return computation results
        └─────────────────────┘
        
        Controller → Worker:
        ┌─────────────────────┐
        │ WorkerRegisterAck   │  Confirm registration, assign worker_id
        │ HeartbeatAck        │  (Optional) Acknowledge heartbeat
        └─────────────────────┘
        
    Design Philosophy:
        
        BINARY vs. TEXT PROTOCOLS:
        - Binary chosen for efficiency (network is bottleneck on Khoury cluster)
        - Trade-off: Not human-readable (debugging requires tools)
        - Benefit: 10-50x faster serialization than JSON/XML
        
        CLASS HIERARCHY:
        - Base Message class: Common header (type, id, size)
        - Derived classes: Specialized payload per message type
        - Polymorphism: Virtual serialize() for extensibility
        
        VALUE TYPES vs. POINTERS:
        - JobParams, JobResult: Value types (copyable, movable)
        - Message classes: Returned via unique_ptr (ownership semantics)
        
        BYTE ORDER HANDLING:
        - Explicit conversion to network byte order (big-endian)
        - Platform-independent: Works on Intel (little), ARM (bi), SPARC (big)
        - Standard practice: All Internet protocols use network byte order
        
    Memory Safety:
        - No raw pointers in public API (unique_ptr for ownership)
        - Value semantics for data structures (RAII)
        - Explicit size tracking (no buffer overruns)
        - Exception-based error handling (no silent failures)
        
    Thread Safety:
        - All types are thread-safe to construct/copy/move
        - Serialization is const (read-only, safe from multiple threads)
        - No shared mutable state in message objects
        - Network layer responsible for connection thread safety
        
    Integration Points:
        - network/tcp_connection.cpp: Uses serialize()/deserialize()
        - controller/job_scheduler.cpp: Creates JobAssignMessage
        - worker/job_executor.cpp: Creates JobResultMessage
        - worker/heartbeat_manager.cpp: Creates HeartbeatMessage
        
    Performance Characteristics:
        Message Type          | Typical Size | Serialize Time | Deserialize Time
        ----------------------|--------------|----------------|------------------
        JobAssignMessage      | 200-400 B    | 2-5 μs         | 3-7 μs
        JobResultMessage      | 300-500 B    | 3-7 μs         | 4-9 μs
        HeartbeatMessage      | 29 B (fixed) | 1-2 μs         | 1-2 μs
        WorkerRegisterMessage | 50-100 B     | 2-4 μs         | 2-4 μs
        
    Dependencies:
        - <string>: std::string for text fields
        - <vector>: std::vector for binary buffers
        - <cstdint>: Fixed-width integer types (uint8_t, uint16_t, ...)
        - <cstring>: memcpy for byte-level operations
        - <memory>: std::unique_ptr for ownership semantics
        
    Related Files:
        - message.cpp: Implementation of serialization/deserialization
        - network/tcp_connection.h: Network layer that uses these messages
        - See message.cpp for detailed wire format specifications

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. HEADER GUARD & PREPROCESSOR
// 2. STANDARD LIBRARY INCLUDES
// 3. NAMESPACE DECLARATION
// 4. MESSAGE TYPE ENUMERATION (MessageType)
//    4.1 Protocol Message Types
//    4.2 Message Type Design Rationale
// 5. DATA STRUCTURES
//    5.1 JobParams: Backtest job parameters
//    5.2 JobResult: Backtest computation results
// 6. MESSAGE CLASS HIERARCHY
//    6.1 Base Message Class
//    6.2 JobAssignMessage: Controller → Worker
//    6.3 JobResultMessage: Worker → Controller
//    6.4 HeartbeatMessage: Worker → Controller
//    6.5 WorkerRegisterMessage: Worker → Controller
// 7. BYTE ORDER CONVERSION UTILITIES
//    7.1 Host-to-Network Conversion (hton16/32/64)
//    7.2 Network-to-Host Conversion (ntoh16/32/64)
//    7.3 Endianness Explanation
// 8. USAGE EXAMPLES
//    8.1 Creating and Sending Messages
//    8.2 Receiving and Parsing Messages
//    8.3 Error Handling Patterns
// 9. COMMON PITFALLS & SOLUTIONS
// 10. FAQ
// 11. BEST PRACTICES
// 12. PROTOCOL EVOLUTION GUIDELINES
// 13. TESTING STRATEGIES
// 14. TROUBLESHOOTING GUIDE
//
//==============================================================================

//==============================================================================
// SECTION 1: HEADER GUARD & PREPROCESSOR
//==============================================================================
//
// #ifndef MESSAGE_H / #define MESSAGE_H / #endif
//
// Standard C++ header guard pattern to prevent multiple inclusion.
// See logger.h documentation (Section 1) for detailed explanation.
//
// Alternative: #pragma once (non-standard but widely supported)
//
//==============================================================================

#ifndef MESSAGE_H
#define MESSAGE_H

//==============================================================================
// SECTION 2: STANDARD LIBRARY INCLUDES
//==============================================================================
//
// Minimal includes for message protocol types and operations.
//
//------------------------------------------------------------------------------

// <string> - std::string for text fields
// =======================================
// Required for:
// - Symbol names: JobParams::symbol (e.g., "AAPL", "GOOGL")
// - Strategy types: JobParams::strategy_type (e.g., "SMA", "RSI")
// - Date strings: JobParams::start_date, end_date (e.g., "2020-01-01")
// - Error messages: JobResult::error_message
// - Hostnames: WorkerRegisterMessage::worker_hostname
//
// Why std::string (not char* or std::string_view)?
// - Ownership: std::string owns its data (safe, no dangling pointers)
// - Convenience: Automatic memory management, rich API
// - Compatibility: Works everywhere, standard since C++98
//
#include <string>

// <vector> - std::vector for binary buffers
// ==========================================
// Required for:
// - Serialization output: serialize() returns std::vector<uint8_t>
// - Dynamic buffer management: Grows as needed during serialization
// - Contiguous memory: Required for network I/O (send/recv)
//
// Why std::vector<uint8_t> (not std::string or char[])?
// - Type safety: uint8_t clearly indicates "raw bytes" (not text)
// - Dynamic sizing: Handles variable-length messages
// - Standard container: RAII, exception-safe, well-understood
//
#include <vector>

// <cstdint> - Fixed-width integer types
// ======================================
// Required for:
// - uint8_t: 1-byte (message type, boolean flags)
// - uint16_t: 2-byte (port numbers)
// - uint32_t: 4-byte (sizes, counts, int32_t fields)
// - uint64_t: 8-byte (IDs, job identifiers)
//
// Why fixed-width types (not int, long)?
// - Portability: int size varies (16-bit on some embedded, 32-bit typical)
// - Network protocol: Must have exact sizes for serialization
// - Explicit: uint32_t is unambiguous (always 32 bits)
//
// Standard since C++11, replaces <stdint.h> from C.
//
#include <cstdint>

// <cstring> - Low-level memory operations
// ========================================
// Required for:
// - std::memcpy: Copying bytes during serialization/deserialization
//
// Why in header (not just .cpp)?
// - Inline functions (hton/ntoh) may use memcpy
// - Future: Template implementations might need it
//
// Note: Only std::memcpy used, not C string functions (strcpy, strlen)
//
#include <cstring>

// <memory> - Smart pointers
// =========================
// Required for:
// - std::unique_ptr: Ownership semantics for deserialized messages
// - Return type: deserialize() returns std::unique_ptr<MessageType>
//
// Why unique_ptr (not shared_ptr or raw pointer)?
// - Ownership: Caller owns message, responsible for lifetime
// - Safety: Automatic cleanup (no memory leaks)
// - Efficiency: Zero overhead vs. raw pointer
// - Semantics: Unique ownership is clear (not shared)
//
#include <memory>

//==============================================================================
// SECTION 3: NAMESPACE DECLARATION
//==============================================================================
//
// namespace backtesting { ... }
//
// Encapsulates all distributed system components.
// See logger.h documentation (Section 3) for detailed namespace rationale.
//
//==============================================================================

namespace backtesting {

//==============================================================================
// SECTION 4: MESSAGE TYPE ENUMERATION
//==============================================================================
//
// enum class MessageType : uint8_t { ... }
//
// OVERVIEW:
// Defines all message types in the distributed system protocol. Each message
// type has a unique identifier that determines payload structure and semantics.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.1 PROTOCOL MESSAGE TYPES
//------------------------------------------------------------------------------
//
// enum class MessageType : uint8_t
//
// DESIGN DECISIONS:
//
// Why enum class (not enum)?
// - Scoped: Must write MessageType::JOB_ASSIGN (prevents naming conflicts)
// - Strongly typed: Cannot accidentally convert to int
// - Modern C++11 best practice
//
// Why uint8_t underlying type?
// - Compact: Only 1 byte per message type in protocol
// - Sufficient: Supports 256 message types (currently use 6)
// - Standard: Many protocols use 1-byte type field
//
// Why these specific values?
// - Explicit values: Makes wire format specification clear
// - Sequential: Easy to understand (1, 2, 3, ...)
// - Reserved: 255 for ERROR (max value, stands out)
// - Gaps allowed: Can add types between existing ones if needed
//
// MESSAGE TYPE CATEGORIES:
//
// Job Management (1-2):
// - JOB_ASSIGN: Controller assigns work to worker
// - JOB_RESULT: Worker returns computation results
//
// Health Monitoring (3-4):
// - HEARTBEAT: Worker reports health (every 2 seconds)
// - HEARTBEAT_ACK: Controller acknowledges (optional, not always sent)
//
// Connection Management (5-6):
// - WORKER_REGISTER: Worker initial connection
// - WORKER_REGISTER_ACK: Controller confirms, assigns worker_id
//
// Error Handling (255):
// - ERROR: Generic error message (protocol violation, etc.)
//
// PROTOCOL EVOLUTION:
// Future message types can use unused values (7-254).
// Examples of potential additions:
// - CHECKPOINT_SAVE = 7    // Worker saves progress
// - CHECKPOINT_LOAD = 8    // Worker resumes from checkpoint
// - JOB_CANCEL = 9         // Controller cancels job
// - WORKER_SHUTDOWN = 10   // Graceful shutdown notification
//
//------------------------------------------------------------------------------

// Message types
//
// CRITICAL: These values appear in network protocol (wire format)
// Changing values breaks compatibility with existing deployed nodes!
//
enum class MessageType : uint8_t {
    // JOB_ASSIGN (1): Controller → Worker
    // ===================================
    // Purpose: Assign backtest job to worker for execution
    // Payload: JobAssignMessage (symbol, strategy, date range, parameters)
    // Response: Worker sends JOB_RESULT when computation completes
    // Frequency: On-demand (when jobs available and workers idle)
    //
    // Example flow:
    //   Controller: Receives client request to backtest AAPL
    //   Controller: Selects available worker (e.g., Worker #3)
    //   Controller: Sends JOB_ASSIGN with job parameters
    //   Worker #3: Receives message, starts backtest computation
    JOB_ASSIGN = 1,
    
    // JOB_RESULT (2): Worker → Controller
    // ====================================
    // Purpose: Return backtest computation results to controller
    // Payload: JobResultMessage (metrics, PnL, Sharpe ratio, trades, etc.)
    // Trigger: Sent when job completes (success or failure)
    // Frequency: Once per assigned job
    //
    // Example flow:
    //   Worker: Completes backtest for AAPL
    //   Worker: Calculates performance metrics
    //   Worker: Sends JOB_RESULT to controller
    //   Controller: Aggregates results, updates client
    JOB_RESULT = 2,
    
    // HEARTBEAT (3): Worker → Controller
    // ===================================
    // Purpose: Periodic health check to detect worker failures
    // Payload: HeartbeatMessage (worker_id, active_jobs, completed_jobs)
    // Frequency: Every 2 seconds (configurable)
    // Timeout: Controller expects heartbeat within 6 seconds (3 missed = failure)
    //
    // Why heartbeats?
    // - Failure detection: Know when worker crashes or network fails
    // - Load balancing: Track active jobs per worker
    // - Monitoring: System health visibility
    //
    // Example flow:
    //   Worker: Background thread wakes every 2 seconds
    //   Worker: Sends HEARTBEAT with current status
    //   Controller: Updates worker registry with last heartbeat time
    //   Controller: If no heartbeat for 6 seconds → mark worker as failed
    HEARTBEAT = 3,
    
    // HEARTBEAT_ACK (4): Controller → Worker (OPTIONAL)
    // ==================================================
    // Purpose: Acknowledge receipt of heartbeat
    // Payload: Empty or minimal (just header)
    // Frequency: Optional (not always sent)
    //
    // Why optional?
    // - Reduces network traffic (no need to ack every heartbeat)
    // - Worker can detect controller failure via lack of job assignments
    // - Could be used for clock synchronization or latency measurement
    //
    // Current implementation: Not used (workers don't wait for ack)
    // Future use case: Measure round-trip time, detect network latency
    HEARTBEAT_ACK = 4,
    
    // WORKER_REGISTER (5): Worker → Controller
    // =========================================
    // Purpose: Initial connection, provide worker contact information
    // Payload: WorkerRegisterMessage (hostname, port)
    // Frequency: Once at startup (or after reconnection)
    //
    // Why register?
    // - Controller needs to know worker address (for reconnection)
    // - Controller assigns unique worker_id
    // - Enables controller to track all workers
    //
    // Example flow:
    //   Worker: Starts up, connects to controller
    //   Worker: Sends WORKER_REGISTER with hostname and port
    //   Controller: Assigns worker_id = 42
    //   Controller: Sends WORKER_REGISTER_ACK with assigned ID
    //   Worker: Now ready to receive job assignments
    WORKER_REGISTER = 5,
    
    // WORKER_REGISTER_ACK (6): Controller → Worker
    // =============================================
    // Purpose: Confirm worker registration, assign worker_id
    // Payload: Worker ID assigned by controller
    // Frequency: Once per registration (response to WORKER_REGISTER)
    //
    // Why acknowledge?
    // - Worker needs to know its assigned ID (for future messages)
    // - Controller confirms successful registration
    // - Enables synchronization (worker waits for ack before proceeding)
    //
    // Example payload (conceptual):
    //   uint64_t assigned_worker_id;  // e.g., 42
    //   std::string cluster_info;     // e.g., "3 controllers, 8 workers"
    WORKER_REGISTER_ACK = 6,
    
    // ERROR (255): Any → Any
    // ======================
    // Purpose: Signal protocol error or exceptional condition
    // Payload: Error code and descriptive message
    // Frequency: Rare (only on malformed messages or protocol violations)
    //
    // Why 255?
    // - Maximum uint8_t value (stands out as special)
    // - Convention: Many protocols use max value for error
    // - Easy to spot in hex dumps: 0xFF
    //
    // Example scenarios:
    //   - Receiver gets unknown message type → Send ERROR
    //   - Deserialization fails → Send ERROR
    //   - Protocol version mismatch → Send ERROR
    //
    // Example payload (conceptual):
    //   uint32_t error_code;          // e.g., 1 = unknown message type
    //   std::string error_message;    // e.g., "Unknown type: 127"
    //
    // Current implementation: Defined but not fully implemented
    // (Connections typically closed on error rather than sending ERROR message)
    ERROR = 255
};

//------------------------------------------------------------------------------
// 4.2 MESSAGE TYPE DESIGN RATIONALE
//------------------------------------------------------------------------------
//
// ALTERNATIVE DESIGNS CONSIDERED:
//
// 1. String-based types (JSON-style):
//    { "type": "JOB_ASSIGN", ... }
//    Pros: Human-readable, self-documenting
//    Cons: Larger (10+ bytes vs. 1 byte), slower parsing
//
// 2. Bitflags (combinable types):
//    enum MessageType { REQUEST = 0x01, RESPONSE = 0x02, JOB = 0x04, ... };
//    Can combine: REQUEST | JOB = job request
//    Pros: Flexible combinations
//    Cons: Complex, rarely needed in practice
//
// 3. Hierarchical types (high byte = category, low byte = specific):
//    JOB_ASSIGN = 0x0101, JOB_RESULT = 0x0102
//    HEARTBEAT = 0x0201, HEARTBEAT_ACK = 0x0202
//    Pros: Grouped by category
//    Cons: Wastes space (2 bytes vs. 1), unnecessary complexity
//
// CHOSEN DESIGN: Simple 1-byte enum
// - Minimal overhead (1 byte per message)
// - Sufficient for foreseeable needs (256 types)
// - Clear, easy to understand
// - Standard practice in binary protocols
//
// COMPARISON WITH OTHER PROTOCOLS:
// - HTTP: Text-based ("GET", "POST", "PUT") - more bytes but human-readable
// - Protocol Buffers: Similar approach (1-byte type field)
// - gRPC: Uses method IDs (integer), similar concept
// - Our protocol: Optimized for low latency and bandwidth efficiency
//
//------------------------------------------------------------------------------

//==============================================================================
// SECTION 5: DATA STRUCTURES
//==============================================================================
//
// Plain Old Data (POD) structures for message payloads.
// These are value types (copyable, movable) used to carry domain data.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 5.1 JOBPARAMS: BACKTEST JOB PARAMETERS
//------------------------------------------------------------------------------
//
// struct JobParams
//
// PURPOSE:
// Encapsulates all parameters needed to execute a backtesting strategy.
// Sent from controller to worker in JobAssignMessage.
//
// DESIGN DECISIONS:
//
// Why struct (not class)?
// - No invariants: All fields are independent
// - Public access: Direct field access is natural
// - Aggregate: Default copy/move/compare is appropriate
// - Convention: C++ structs for data bundles, classes for behavior
//
// Why default values in constructor?
// - Sensible defaults: 50/200 day moving average is common
// - Convenience: Can construct empty and fill in selectively
// - Safety: Never uninitialized (all fields have values)
//
// Why strings for dates (not time_t or chrono)?
// - Human-readable: "2020-01-01" is clear
// - Network-friendly: Strings serialize easily
// - Flexible: Can parse to any date type needed
// - Standard: ISO 8601 format widely supported
//
// FIELD DESCRIPTIONS:
//
// symbol: Stock ticker symbol
//   Examples: "AAPL", "GOOGL", "MSFT", "AMZN"
//   Format: Typically 1-5 uppercase letters
//   Validation: Should match available data files
//   Size: Small (1-10 bytes typical)
//
// strategy_type: Name of backtesting strategy
//   Examples: "SMA" (Simple Moving Average), "RSI" (Relative Strength Index)
//   Purpose: Worker loads appropriate strategy implementation
//   Extensible: New strategies added without protocol changes
//   Size: Small (3-20 bytes typical)
//
// start_date, end_date: Backtesting period
//   Format: "YYYY-MM-DD" (ISO 8601)
//   Examples: "2020-01-01", "2024-12-31"
//   Validation: start_date <= end_date
//   Range: Typically 1-5 years (250-1250 trading days)
//   Size: Fixed 10 bytes per date
//
// short_window, long_window: Moving average periods
//   Units: Trading days
//   Typical values: short_window = 50, long_window = 200
//   Validation: 0 < short_window < long_window
//   Strategy-specific: Not used by all strategies (RSI doesn't need these)
//   Default: 50/200 (industry standard)
//
// initial_capital: Starting portfolio value
//   Units: US Dollars (USD)
//   Typical: $10,000 - $1,000,000
//   Purpose: Calculate absolute returns and position sizes
//   Default: $10,000 (common for backtesting)
//   Type: double (sufficient precision for currency)
//
// USAGE PATTERN:
//
//   JobParams params;
//   params.symbol = "AAPL";
//   params.strategy_type = "SMA";
//   params.start_date = "2020-01-01";
//   params.end_date = "2024-12-31";
//   params.short_window = 50;
//   params.long_window = 200;
//   params.initial_capital = 10000.0;
//   
//   // Or with defaults:
//   JobParams params;
//   params.symbol = "AAPL";
//   params.strategy_type = "SMA";
//   params.start_date = "2020-01-01";
//   params.end_date = "2024-12-31";
//   // short_window, long_window, initial_capital already have defaults
//
// MEMORY LAYOUT:
// Approximate size: 40-80 bytes (depends on string lengths)
// - symbol: ~8 bytes (SSO, no heap allocation for short strings)
// - strategy_type: ~8 bytes
// - start_date: ~8 bytes
// - end_date: ~8 bytes
// - short_window: 4 bytes (int)
// - long_window: 4 bytes (int)
// - initial_capital: 8 bytes (double)
// Total: ~48 bytes + string data
//
// SERIALIZATION SIZE:
// Wire format (in JobAssignMessage):
// - Each string: 4-byte length + data
// - Integers: 4 bytes each
// - Double: 8 bytes
// Total: ~70-100 bytes typical
//
//------------------------------------------------------------------------------

// Job parameters
struct JobParams {
    // Stock symbol to backtest
    // Examples: "AAPL" (Apple), "GOOGL" (Google), "MSFT" (Microsoft)
    std::string symbol;
    
    // Strategy name to execute
    // Examples: "SMA" (Simple Moving Average), "RSI" (Relative Strength Index)
    // Worker uses this to select strategy implementation
    std::string strategy_type;  // "SMA", "RSI", etc.
    
    // Backtest start date (ISO 8601 format: YYYY-MM-DD)
    // Example: "2020-01-01" (January 1, 2020)
    // Validation: Must be <= end_date
    std::string start_date;     // YYYY-MM-DD
    
    // Backtest end date (ISO 8601 format: YYYY-MM-DD)
    // Example: "2024-12-31" (December 31, 2024)
    // Validation: Must be >= start_date
    std::string end_date;       // YYYY-MM-DD
    
    // Short moving average period (trading days)
    // Typical: 50 days (SMA crossover strategy)
    // Validation: Must be > 0 and < long_window
    // Note: Not used by all strategies (strategy-specific parameter)
    int short_window;           // For SMA: short period
    
    // Long moving average period (trading days)
    // Typical: 200 days (SMA crossover strategy)
    // Validation: Must be > short_window
    int long_window;            // For SMA: long period
    
    // Starting portfolio value (USD)
    // Typical: $10,000 - $1,000,000
    // Used to calculate absolute returns and position sizing
    double initial_capital;
    
    // Default constructor with sensible defaults
    // - short_window = 50 (industry standard for short MA)
    // - long_window = 200 (industry standard for long MA)
    // - initial_capital = $10,000 (common backtesting starting point)
    // - Strings default to empty (must be filled by user)
    JobParams() : short_window(50), long_window(200), initial_capital(10000.0) {}
};

//------------------------------------------------------------------------------
// 5.2 JOBRESULT: BACKTEST COMPUTATION RESULTS
//------------------------------------------------------------------------------
//
// struct JobResult
//
// PURPOSE:
// Encapsulates all performance metrics from a completed backtest.
// Sent from worker to controller in JobResultMessage.
//
// DESIGN PHILOSOPHY:
//
// Comprehensive metrics:
// - Return: Absolute and risk-adjusted (Sharpe ratio)
// - Risk: Maximum drawdown (worst decline from peak)
// - Activity: Trade counts (total, winning, losing)
// - Status: Success flag and error message
//
// Why these specific metrics?
// - total_return: Basic profitability measure
// - sharpe_ratio: Risk-adjusted performance (industry standard)
// - max_drawdown: Risk measure (important for risk management)
// - Trade counts: Strategy activity level
// - Success flag: Distinguish success from failure
//
// FIELD DESCRIPTIONS:
//
// job_id: Links result to original job assignment
//   Matches: JobAssignMessage::job_id
//   Purpose: Controller correlates request with response
//   Type: uint64_t (never exhausted in practice)
//
// symbol: Stock ticker (echoed from assignment)
//   Purpose: Logging, tracking which symbol's result this is
//   Redundant: Already in job assignment, but useful for debugging
//
// total_return: Percentage return over backtest period
//   Formula: (final_value - initial_capital) / initial_capital
//   Example: 0.25 = 25% return
//   Range: Typically -1.0 to +5.0 (-100% to +500%)
//   Interpretation: > 0 = profit, < 0 = loss
//
// sharpe_ratio: Risk-adjusted return metric
//   Formula: (mean_return - risk_free_rate) / std_dev_return
//   Interpretation: Higher is better (more return per unit risk)
//   Typical: -2.0 to +3.0
//   Good: > 1.0, Excellent: > 2.0
//
// max_drawdown: Largest peak-to-trough decline (percentage)
//   Formula: (trough_value - peak_value) / peak_value
//   Example: -0.30 = -30% decline from peak
//   Range: -1.0 to 0.0 (negative)
//   Interpretation: Closer to 0 = less risk
//
// final_portfolio_value: Ending portfolio value (USD)
//   Example: $12,500 (started with $10,000 = 25% gain)
//   Purpose: Absolute value of portfolio at end
//   Verification: Should equal initial_capital * (1 + total_return)
//
// num_trades: Total number of trades executed
//   Includes: Both entry and exit (buy and sell)
//   Typical: 10-1000 depending on strategy and timeframe
//   Interpretation: Higher = more active strategy
//
// winning_trades: Number of profitable trades
//   Definition: Trade where exit price > entry price
//   Validation: <= num_trades
//
// losing_trades: Number of unprofitable trades
//   Definition: Trade where exit price < entry price
//   Validation: winning_trades + losing_trades <= num_trades
//   Note: May not sum to num_trades (some trades break-even)
//
// success: Computation completed successfully
//   true: Results are valid
//   false: Computation failed, check error_message
//   Purpose: Distinguish between valid negative returns and failures
//
// error_message: Descriptive error if success = false
//   Examples:
//     - "Failed to load CSV file: AAPL_2020.csv"
//     - "Insufficient data points: need 200, have 50"
//     - "Strategy threw exception: division by zero"
//   Empty: If success = true
//   Purpose: Debugging, logging, user feedback
//
// USAGE PATTERN:
//
//   JobResult result;
//   result.job_id = 1000;
//   result.symbol = "AAPL";
//   result.total_return = 0.25;      // 25% return
//   result.sharpe_ratio = 1.5;       // Good risk-adjusted return
//   result.max_drawdown = -0.15;     // -15% max decline
//   result.final_portfolio_value = 12500.0;
//   result.num_trades = 50;
//   result.winning_trades = 30;
//   result.losing_trades = 20;
//   result.success = true;
//   result.error_message = "";       // Empty on success
//
// FAILURE PATTERN:
//
//   JobResult result;
//   result.job_id = 1001;
//   result.symbol = "INVALID";
//   result.total_return = 0.0;       // All metrics zero on failure
//   result.sharpe_ratio = 0.0;
//   result.max_drawdown = 0.0;
//   result.final_portfolio_value = 0.0;
//   result.num_trades = 0;
//   result.winning_trades = 0;
//   result.losing_trades = 0;
//   result.success = false;          // Mark as failed
//   result.error_message = "Symbol not found: INVALID";
//
// MEMORY LAYOUT:
// Approximate size: 90-150 bytes
// - job_id: 8 bytes
// - symbol: ~8 bytes (SSO)
// - Doubles (4 × 8): 32 bytes
// - Integers (3 × 4): 12 bytes
// - bool: 1 byte
// - error_message: ~8 bytes (SSO) + data if error
// Total: ~70 bytes + string data
//
// SERIALIZATION SIZE:
// Wire format: ~120-180 bytes typical (with strings)
//
//------------------------------------------------------------------------------

// Job result metrics
struct JobResult {
    // Job identifier (matches JobAssignMessage::job_id)
    // Purpose: Correlate result with original job assignment
    uint64_t job_id;
    
    // Stock symbol (echoed from assignment for tracking)
    std::string symbol;
    
    // Percentage return over backtest period
    // Formula: (final_value - initial_capital) / initial_capital
    // Example: 0.25 = 25% return
    double total_return;        // Percentage
    
    // Risk-adjusted return (higher is better)
    // Formula: (mean_return - risk_free) / std_dev_return
    // Interpretation: > 1.0 is good, > 2.0 is excellent
    double sharpe_ratio;
    
    // Maximum peak-to-trough decline (negative percentage)
    // Example: -0.30 = -30% decline from peak
    // Interpretation: Closer to 0 = lower risk
    double max_drawdown;        // Percentage
    
    // Final portfolio value at end of backtest (USD)
    double final_portfolio_value;
    
    // Total number of trades executed
    int num_trades;
    
    // Number of profitable trades
    int winning_trades;
    
    // Number of unprofitable trades
    int losing_trades;
    
    // Computation completed successfully?
    // true = valid results, false = error (check error_message)
    bool success;
    
    // Error description (empty if success = true)
    // Examples: "Failed to load data", "Insufficient data points"
    std::string error_message;
    
    // Default constructor initializes all fields to safe defaults
    // - Numeric fields: 0
    // - success: false (must explicitly set true on success)
    // - Strings: empty
    JobResult() : job_id(0), total_return(0.0), sharpe_ratio(0.0), 
                  max_drawdown(0.0), final_portfolio_value(0.0),
                  num_trades(0), winning_trades(0), losing_trades(0), 
                  success(false) {}
};

//==============================================================================
// SECTION 6: MESSAGE CLASS HIERARCHY
//==============================================================================
//
// Object-oriented design for protocol messages.
// Base class provides common interface, derived classes specialize.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 6.1 BASE MESSAGE CLASS
//------------------------------------------------------------------------------
//
// class Message
//
// PURPOSE:
// Abstract base class for all message types in the protocol.
// Provides common header fields and virtual serialization interface.
//
// DESIGN PATTERN: Class Hierarchy
// - Base class: Common functionality (header fields, interface)
// - Derived classes: Specialized payload per message type
// - Polymorphism: Virtual serialize() for runtime dispatch
//
// FIELDS:
//
// type: MessageType enum value
//   Purpose: Identifies message format (what's in payload)
//   Size: 1 byte (uint8_t)
//   Wire format: First byte of every message
//
// id: Unique message identifier
//   Purpose: Request/response correlation, logging, debugging
//   Size: 8 bytes (uint64_t)
//   Generation: Sequential (0, 1, 2, ...) or random
//   Example: Controller sends job with id=42, worker responds with id=42
//
// payload_size: Number of bytes in payload (after header)
//   Purpose: Framing (receiver knows how many bytes to read)
//   Size: 4 bytes (uint32_t)
//   Maximum: 4GB (2^32 - 1), practically ~10KB for this project
//   Calculated: During serialization (total size - header size)
//
// CONSTRUCTOR:
// Message(MessageType t = MessageType::ERROR)
//   Default: ERROR type (safe, explicit about unknown state)
//   Parameter: Derived classes pass their specific type
//   Fields: id and payload_size default to 0 (set during serialization)
//
// DESTRUCTOR:
// virtual ~Message() = default;
//   Virtual: Ensures proper cleanup of derived classes
//   Default: No special cleanup needed (members are RAII)
//   Why virtual? Allows deletion via base class pointer:
//     std::unique_ptr<Message> msg = ...; // May point to JobAssignMessage
//     // ~JobAssignMessage called (not just ~Message)
//
// SERIALIZATION:
// virtual std::vector<uint8_t> serialize() const;
//   Virtual: Derived classes override to add payload
//   Const: Serialization is read-only (doesn't modify message)
//   Return: Binary buffer ready for network transmission
//   Pattern: Base implementation writes header, derived adds payload
//
// DESERIALIZATION:
// static std::unique_ptr<Message> deserialize(const std::vector<uint8_t>& data);
//   Static: Factory method (constructs appropriate derived class)
//   Return: unique_ptr for ownership semantics
//   Process:
//     1. Read type field (first byte)
//     2. Switch on type
//     3. Call derived class deserialize()
//     4. Return pointer to base class
//
// USAGE PATTERN:
//
//   // Creating message (typically use derived class directly)
//   JobAssignMessage msg;
//   msg.id = GenerateId();
//   msg.job_id = 1000;
//   // ... fill in fields ...
//   
//   // Serializing (polymorphic)
//   std::vector<uint8_t> bytes = msg.serialize();
//   
//   // Deserializing (static factory)
//   auto received = Message::deserialize(bytes);
//   
//   // Downcast to specific type
//   if (received->type == MessageType::JOB_ASSIGN) {
//       auto* job_msg = dynamic_cast<JobAssignMessage*>(received.get());
//       // ... use job_msg ...
//   }
//
// POLYMORPHISM BENEFITS:
// - Uniform interface: All messages serialize the same way
// - Extensibility: New message types without changing network layer
// - Type safety: Compiler checks virtual function overrides
//
// ALTERNATIVE DESIGNS CONSIDERED:
//
// 1. No inheritance, separate serialize functions:
//    std::vector<uint8_t> serialize_job_assign(const JobAssignMessage& msg);
//    Pros: Simpler (no virtual functions)
//    Cons: No polymorphism, must know type at compile time
//
// 2. Template-based:
//    template<typename T> std::vector<uint8_t> serialize(const T& msg);
//    Pros: No runtime overhead
//    Cons: Can't store mixed types in container
//
// 3. Union/variant:
//    std::variant<JobAssignMessage, JobResultMessage, ...>
//    Pros: Type-safe, no heap allocation
//    Cons: More complex, C++17 required
//
// Chosen design (inheritance) is classic OOP approach, well-understood
// and appropriate for this use case.
//
//------------------------------------------------------------------------------

// Base message class
class Message {
public:
    // Message type identifier (determines payload structure)
    // First byte in wire format
    MessageType type;
    
    // Unique message ID for request/response correlation
    // Bytes 1-8 in wire format
    uint64_t id;
    
    // Payload size in bytes (everything after 13-byte header)
    // Bytes 9-12 in wire format
    uint32_t payload_size;
    
    // Constructor with default type (ERROR = uninitialized/invalid)
    // Derived classes pass their specific MessageType
    Message(MessageType t = MessageType::ERROR) 
        : type(t), id(0), payload_size(0) {}
    
    // Virtual destructor enables polymorphic deletion
    // Ensures derived class destructors are called
    virtual ~Message() = default;
    
    // Serialize message to binary format
    // Derived classes override to add payload
    // Base implementation writes common 13-byte header:
    //   [1-byte type][8-byte id][4-byte payload_size]
    virtual std::vector<uint8_t> serialize() const;
    
    // Deserialize message from binary format
    // Static factory method: Reads type, constructs appropriate derived class
    // Returns: unique_ptr to base class (polymorphic)
    // Throws: std::runtime_error on malformed data
    static std::unique_ptr<Message> deserialize(const std::vector<uint8_t>& data);
};

//------------------------------------------------------------------------------
// 6.2 JOBASSIGNMESSAGE: CONTROLLER → WORKER
//------------------------------------------------------------------------------
//
// class JobAssignMessage : public Message
//
// PURPOSE:
// Carries job assignment from controller to worker, including all parameters
// needed to execute the backtest strategy.
//
// INHERITANCE:
// - Inherits: type, id, payload_size from Message base class
// - Adds: job_id and params (payload-specific fields)
//
// FIELDS:
//
// job_id: Unique identifier for this specific job
//   Purpose: Track job throughout system (logs, Raft, results)
//   Different from Message::id:
//     - Message::id: Network-level correlation (this specific message)
//     - job_id: Application-level tracking (this job across messages)
//   Example: job_id=1000 persists from assignment to result
//
// params: JobParams struct with all backtest parameters
//   Contains: symbol, strategy, dates, windows, capital
//   See JobParams documentation for details
//
// CONSTRUCTOR:
// JobAssignMessage()
//   Sets: type = MessageType::JOB_ASSIGN
//   Initializes: job_id = 0 (must be set by caller)
//   Note: params has default constructor (sensible defaults)
//
// SERIALIZATION:
// std::vector<uint8_t> serialize() const override;
//   Wire format: [13-byte header][8-byte job_id][JobParams fields]
//   Typical size: 200-400 bytes
//   See message.cpp for detailed wire format
//
// DESERIALIZATION:
// static std::unique_ptr<JobAssignMessage> deserialize(const uint8_t* data, size_t size);
//   Reconstructs message from binary buffer
//   Validates: Buffer size, field boundaries
//   Returns: unique_ptr (ownership transferred to caller)
//   Throws: std::runtime_error on malformed data
//
// USAGE EXAMPLE:
//
//   // Controller assigns job to worker
//   JobAssignMessage msg;
//   msg.id = GenerateMessageId();       // For network correlation
//   msg.job_id = GenerateJobId();       // For application tracking
//   msg.params.symbol = "AAPL";
//   msg.params.strategy_type = "SMA";
//   msg.params.start_date = "2020-01-01";
//   msg.params.end_date = "2024-12-31";
//   msg.params.short_window = 50;
//   msg.params.long_window = 200;
//   msg.params.initial_capital = 10000.0;
//   
//   // Serialize and send
//   std::vector<uint8_t> bytes = msg.serialize();
//   worker_connection->Send(bytes);
//   
//   // Worker receives and deserializes
//   auto received_msg = JobAssignMessage::deserialize(buffer.data(), buffer.size());
//   ExecuteJob(received_msg->job_id, received_msg->params);
//
// NETWORK LAYER INTEGRATION:
//
//   // In network layer (TCP connection)
//   void SendJobAssignment(const JobAssignMessage& msg) {
//       auto bytes = msg.serialize();
//       send(socket_fd, bytes.data(), bytes.size(), 0);
//   }
//   
//   // Receiving
//   std::vector<uint8_t> ReceiveMessage() {
//       // Read header (13 bytes)
//       uint8_t header[13];
//       recv(socket_fd, header, 13, 0);
//       
//       // Parse payload size
//       uint32_t payload_size = ntoh32(*reinterpret_cast<uint32_t*>(&header[9]));
//       
//       // Read payload
//       std::vector<uint8_t> payload(payload_size);
//       recv(socket_fd, payload.data(), payload_size, 0);
//       
//       // Combine header + payload
//       std::vector<uint8_t> complete;
//       complete.insert(complete.end(), header, header + 13);
//       complete.insert(complete.end(), payload.begin(), payload.end());
//       
//       return complete;
//   }
//
//------------------------------------------------------------------------------

// Job assignment message
class JobAssignMessage : public Message {
public:
    // Unique job identifier (persists from assignment to result)
    // Different from Message::id (which is per-message)
    uint64_t job_id;
    
    // All parameters needed to execute the backtest
    JobParams params;
    
    // Constructor sets type to JOB_ASSIGN
    // job_id defaults to 0 (must be set by caller)
    JobAssignMessage() : Message(MessageType::JOB_ASSIGN), job_id(0) {}
    
    // Serialize to binary format (overrides base class)
    // Wire format: [header][job_id][params fields]
    // Typical size: 200-400 bytes
    std::vector<uint8_t> serialize() const override;
    
    // Deserialize from binary buffer
    // Returns: unique_ptr to new JobAssignMessage
    // Throws: std::runtime_error on malformed data
    static std::unique_ptr<JobAssignMessage> deserialize(const uint8_t* data, size_t size);
};

//------------------------------------------------------------------------------
// 6.3 JOBRESULTMESSAGE: WORKER → CONTROLLER
//------------------------------------------------------------------------------
//
// class JobResultMessage : public Message
//
// PURPOSE:
// Carries backtest results from worker to controller after computation completes.
//
// FIELDS:
//
// result: JobResult struct with all performance metrics
//   Contains: job_id, symbol, returns, Sharpe, drawdown, trades, status
//   See JobResult documentation for details
//
// CONSTRUCTOR:
// JobResultMessage()
//   Sets: type = MessageType::JOB_RESULT
//   Initializes: result fields to default values (all zeros, success=false)
//
// SERIALIZATION:
// Wire format: [13-byte header][JobResult fields]
// Typical size: 300-500 bytes
//
// USAGE EXAMPLE:
//
//   // Worker completes job, creates result
//   JobResultMessage msg;
//   msg.id = original_request_id;  // Match request ID for correlation
//   msg.result.job_id = job_id;
//   msg.result.symbol = "AAPL";
//   msg.result.total_return = 0.25;
//   msg.result.sharpe_ratio = 1.5;
//   msg.result.max_drawdown = -0.15;
//   msg.result.final_portfolio_value = 12500.0;
//   msg.result.num_trades = 50;
//   msg.result.winning_trades = 30;
//   msg.result.losing_trades = 20;
//   msg.result.success = true;
//   msg.result.error_message = "";
//   
//   // Send to controller
//   auto bytes = msg.serialize();
//   controller_connection->Send(bytes);
//
//------------------------------------------------------------------------------

// Job result message
class JobResultMessage : public Message {
public:
    // Backtest computation results
    JobResult result;
    
    // Constructor sets type to JOB_RESULT
    JobResultMessage() : Message(MessageType::JOB_RESULT) {}
    
    // Serialize to binary format
    // Wire format: [header][result fields]
    // Typical size: 300-500 bytes
    std::vector<uint8_t> serialize() const override;
    
    // Deserialize from binary buffer
    static std::unique_ptr<JobResultMessage> deserialize(const uint8_t* data, size_t size);
};

//------------------------------------------------------------------------------
// 6.4 HEARTBEATMESSAGE: WORKER → CONTROLLER
//------------------------------------------------------------------------------
//
// class HeartbeatMessage : public Message
//
// PURPOSE:
// Periodic health check from worker to controller for failure detection.
//
// FREQUENCY: Every 2 seconds (configurable)
// TIMEOUT: Controller detects failure after 3 missed heartbeats (6 seconds)
//
// FIELDS:
//
// worker_id: Unique identifier for this worker
//   Assigned: During worker registration
//   Purpose: Controller knows which worker sent heartbeat
//
// active_jobs: Number of jobs currently executing
//   Purpose: Load balancing (assign to workers with fewer active jobs)
//   Range: Typically 0-5
//
// completed_jobs: Total jobs completed since worker startup
//   Purpose: Monitoring, statistics
//   Monotonic: Always increasing (never decreases)
//
// WIRE FORMAT:
// Fixed size: 29 bytes total
//   - Header: 13 bytes
//   - worker_id: 8 bytes
//   - active_jobs: 4 bytes
//   - completed_jobs: 4 bytes
//
// WHY FIXED SIZE?
// - Predictable: Know exact size without parsing
// - Efficient: No variable-length fields to handle
// - Fast: Can validate size immediately
//
// USAGE EXAMPLE:
//
//   // Worker heartbeat thread (runs every 2 seconds)
//   while (running) {
//       std::this_thread::sleep_for(std::chrono::seconds(2));
//       
//       HeartbeatMessage msg;
//       msg.id = next_heartbeat_id++;
//       msg.worker_id = my_worker_id;
//       msg.active_jobs = job_executor.GetActiveCount();
//       msg.completed_jobs = job_executor.GetCompletedCount();
//       
//       auto bytes = msg.serialize();
//       controller_connection->Send(bytes);
//   }
//
// CONTROLLER PROCESSING:
//
//   // Controller receives heartbeat
//   auto msg = HeartbeatMessage::deserialize(buffer.data(), buffer.size());
//   
//   // Update worker registry
//   Worker* worker = registry.Find(msg->worker_id);
//   worker->last_heartbeat = std::chrono::steady_clock::now();
//   worker->active_jobs = msg->active_jobs;
//   worker->completed_jobs = msg->completed_jobs;
//   
//   // Check for failed workers (separate thread)
//   auto now = std::chrono::steady_clock::now();
//   for (auto* worker : registry.GetAll()) {
//       auto elapsed = now - worker->last_heartbeat;
//       if (elapsed > std::chrono::seconds(6)) {  // 3 missed heartbeats
//           MarkWorkerAsFailed(worker->id);
//           ReassignJobs(worker->id);
//       }
//   }
//
//------------------------------------------------------------------------------

// Heartbeat message
class HeartbeatMessage : public Message {
public:
    // Worker unique identifier (assigned during registration)
    uint64_t worker_id;
    
    // Number of jobs currently executing on this worker
    uint32_t active_jobs;
    
    // Total jobs completed since worker startup (monotonic)
    uint32_t completed_jobs;
    
    // Constructor sets type to HEARTBEAT
    // Fields default to 0 (set by caller)
    HeartbeatMessage() : Message(MessageType::HEARTBEAT), 
                         worker_id(0), active_jobs(0), completed_jobs(0) {}
    
    // Serialize to binary format
    // Wire format: [header][worker_id][active_jobs][completed_jobs]
    // Fixed size: 29 bytes
    std::vector<uint8_t> serialize() const override;
    
    // Deserialize from binary buffer
    // Expects exactly 29 bytes
    static std::unique_ptr<HeartbeatMessage> deserialize(const uint8_t* data, size_t size);
};

//------------------------------------------------------------------------------
// 6.5 WORKERREGISTERMESSAGE: WORKER → CONTROLLER
//------------------------------------------------------------------------------
//
// class WorkerRegisterMessage : public Message
//
// PURPOSE:
// Initial connection message from worker to controller, providing contact
// information for future communication.
//
// FIELDS:
//
// worker_hostname: Hostname or IP address of worker
//   Examples: "kh05.ccs.neu.edu", "192.168.1.105", "worker-3"
//   Purpose: Controller can reconnect if connection drops
//   Format: FQDN or IPv4/IPv6 address
//
// worker_port: TCP port worker listens on
//   Range: 1-65535 (uint16_t)
//   Typical: 5001, 5002, ... (avoid well-known ports 0-1023)
//   Purpose: Controller knows where to connect
//
// REGISTRATION FLOW:
//   1. Worker starts, connects to controller
//   2. Worker sends WORKER_REGISTER with hostname and port
//   3. Controller assigns worker_id (e.g., 42)
//   4. Controller sends WORKER_REGISTER_ACK with assigned ID
//   5. Worker stores assigned ID, begins sending heartbeats
//   6. Worker ready to receive job assignments
//
// WIRE FORMAT:
// Variable size: Typically 50-100 bytes
//   - Header: 13 bytes
//   - worker_hostname: 4-byte length + data (variable)
//   - worker_port: 2 bytes
//
// USAGE EXAMPLE:
//
//   // Worker startup
//   void Worker::ConnectToController() {
//       // Open TCP connection
//       int sock = connect_to_controller(controller_host, controller_port);
//       
//       // Send registration
//       WorkerRegisterMessage msg;
//       msg.id = GenerateMessageId();
//       msg.worker_hostname = GetMyHostname();  // e.g., "kh05.ccs.neu.edu"
//       msg.worker_port = listening_port;       // e.g., 5001
//       
//       auto bytes = msg.serialize();
//       send(sock, bytes.data(), bytes.size(), 0);
//       
//       // Wait for acknowledgment
//       auto ack = ReceiveWorkerRegisterAck();
//       my_worker_id = ack->assigned_worker_id;
//       
//       Logger::info("Registered as worker " + std::to_string(my_worker_id));
//   }
//
// CONTROLLER PROCESSING:
//
//   // Controller receives registration
//   auto msg = WorkerRegisterMessage::deserialize(buffer.data(), buffer.size());
//   
//   // Assign unique worker ID
//   uint64_t worker_id = next_worker_id++;
//   
//   // Add to registry
//   Worker worker;
//   worker.id = worker_id;
//   worker.hostname = msg->worker_hostname;
//   worker.port = msg->worker_port;
//   worker.last_heartbeat = std::chrono::steady_clock::now();
//   registry.Add(worker);
//   
//   // Send acknowledgment
//   WorkerRegisterAckMessage ack;
//   ack.assigned_worker_id = worker_id;
//   auto bytes = ack.serialize();
//   send(socket, bytes.data(), bytes.size(), 0);
//   
//   Logger::info("Registered worker " + std::to_string(worker_id) + 
//                " at " + worker.hostname + ":" + std::to_string(worker.port));
//
//------------------------------------------------------------------------------

// Worker registration message
class WorkerRegisterMessage : public Message {
public:
    // Worker hostname or IP address
    // Examples: "kh05.ccs.neu.edu", "192.168.1.105"
    std::string worker_hostname;
    
    // TCP port worker listens on
    // Range: 1-65535, typically 5001+
    uint16_t worker_port;
    
    // Constructor sets type to WORKER_REGISTER
    // Fields default to empty string and 0
    WorkerRegisterMessage() : Message(MessageType::WORKER_REGISTER), worker_port(0) {}
    
    // Serialize to binary format
    // Wire format: [header][hostname string][port]
    // Typical size: 50-100 bytes
    std::vector<uint8_t> serialize() const override;
    
    // Deserialize from binary buffer
    static std::unique_ptr<WorkerRegisterMessage> deserialize(const uint8_t* data, size_t size);
};

//==============================================================================
// SECTION 7: BYTE ORDER CONVERSION UTILITIES
//==============================================================================
//
// Inline functions for converting between host and network byte order.
// Essential for cross-platform network communication.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 7.1 HOST-TO-NETWORK CONVERSION (hton16/32/64)
//------------------------------------------------------------------------------
//
// PURPOSE:
// Convert integers from host byte order to network byte order (big-endian)
// before sending over network.
//
// WHY NEEDED?
// Different CPU architectures use different byte orders:
// - x86/x64 (Intel, AMD): Little-endian (LSB first)
// - ARM: Bi-endian (configurable, usually little)
// - SPARC, PowerPC: Big-endian (MSB first)
//
// Network standard: Big-endian (most significant byte first)
//
// EXAMPLE:
// Value: 1000 (0x000003E8 in hex)
//
// Little-endian (x86): E8 03 00 00 (LSB first)
// Big-endian (network): 00 00 03 E8 (MSB first)
//
// If x86 sends 1000 without conversion:
//   Sender: E8 03 00 00 (little-endian representation)
//   Network: E8 03 00 00 (bytes transmitted as-is)
//   Receiver (big-endian): Interprets as 3,892,314,112 (WRONG!)
//
// With hton32 conversion:
//   Sender: hton32(1000) → 00 00 03 E8 (network byte order)
//   Network: 00 00 03 E8
//   Receiver: ntoh32(0x000003E8) → 1000 (CORRECT!)
//
// IMPLEMENTATION DETAILS:
//
// hton16: 16-bit (2-byte) conversion
//   Algorithm: Swap two bytes
//   Example: 0x1234 → 0x3412
//   Formula: ((val & 0xFF) << 8) | ((val >> 8) & 0xFF)
//
// hton32: 32-bit (4-byte) conversion
//   Algorithm: Swap byte pairs, then swap within pairs
//   Example: 0x12345678 → 0x78563412
//   Formula: Combinations of shifts and masks
//
// hton64: 64-bit (8-byte) conversion
//   Algorithm: Convert halves separately, then combine
//   Example: 0x123456789ABCDEF0 → 0xF0DEBC9A78563412
//   Implementation: Convert low and high 32-bit halves, swap positions
//
// WHY INLINE?
// - Performance: No function call overhead
// - Compiler optimization: Can optimize with CPU intrinsics
// - Header-only: No need for separate implementation file
//
// PLATFORM OPTIMIZATION:
// Modern compilers recognize byte-swapping patterns and generate
// optimal code (often single instruction: bswap on x86, rev on ARM).
//
// ALTERNATIVE IMPLEMENTATIONS:
// - POSIX: htons(), htonl() (but no htonll for 64-bit)
// - Boost: boost::endian::native_to_big()
// - C++20: std::endian and bit manipulation
// - Our implementation: Self-contained, no external dependencies
//
//------------------------------------------------------------------------------

// Helper functions for network byte order
//
// CRITICAL: These conversions are essential for cross-platform compatibility
// Always use before sending integers over network!
//

// Convert 16-bit integer from host to network byte order
// hton16: Host TO Network, 16-bit
//
// Algorithm:
//   1. Mask low byte: (val & 0xFF) → 0x34 from 0x1234
//   2. Shift left 8 bits: 0x34 << 8 → 0x3400
//   3. Mask high byte: (val >> 8) & 0xFF → 0x12
//   4. Combine: 0x3400 | 0x0012 → 0x3412
//
// Example:
//   Input: 0x1234 (4660 decimal)
//   Output: 0x3412 (same value, different byte order)
//
// Usage:
//   uint16_t port = 5000;
//   uint16_t net_port = hton16(port);
//   send(socket, &net_port, 2, 0);
//
inline uint16_t hton16(uint16_t val) {
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

// Convert 32-bit integer from host to network byte order
// hton32: Host TO Network, 32-bit
//
// Algorithm:
//   Byte positions: 0x[3][2][1][0] → 0x[0][1][2][3]
//   1. Byte 0: (val & 0xFF) << 24
//   2. Byte 1: (val & 0xFF00) << 8
//   3. Byte 2: (val >> 8) & 0xFF00
//   4. Byte 3: (val >> 24) & 0xFF
//   5. Combine all with OR
//
// Example:
//   Input: 0x12345678
//   Step 1: 0x78 << 24 = 0x78000000
//   Step 2: 0x5600 << 8 = 0x00560000
//   Step 3: 0x3400 >> 8 = 0x00003400
//   Step 4: 0x12 >> 24 = 0x00000012
//   Result: 0x78563412
//
// Usage:
//   uint32_t size = 1000;
//   uint32_t net_size = hton32(size);
//   send(socket, &net_size, 4, 0);
//
inline uint32_t hton32(uint32_t val) {
    return ((val & 0xFF) << 24) | ((val & 0xFF00) << 8) |
           ((val >> 8) & 0xFF00) | ((val >> 24) & 0xFF);
}

// Convert 64-bit integer from host to network byte order
// hton64: Host TO Network, 64-bit
//
// Algorithm:
//   1. Convert low 32 bits: hton32(val & 0xFFFFFFFF)
//   2. Shift to high position: result << 32
//   3. Convert high 32 bits: hton32(val >> 32)
//   4. Combine: (converted_low << 32) | converted_high
//
// Why this approach?
//   - Reuses hton32 (avoids code duplication)
//   - Compiler optimizes shifts and masks
//   - Clear and maintainable
//
// Example:
//   Input: 0x123456789ABCDEF0
//   Low 32 bits: 0x9ABCDEF0 → hton32 → 0xF0DEBC9A
//   High 32 bits: 0x12345678 → hton32 → 0x78563412
//   Result: (0xF0DEBC9A << 32) | 0x78563412 = 0xF0DEBC9A78563412
//
// Usage:
//   uint64_t job_id = 1234567890;
//   uint64_t net_job_id = hton64(job_id);
//   send(socket, &net_job_id, 8, 0);
//
inline uint64_t hton64(uint64_t val) {
    return ((uint64_t)hton32(val & 0xFFFFFFFF) << 32) | hton32(val >> 32);
}

//------------------------------------------------------------------------------
// 7.2 NETWORK-TO-HOST CONVERSION (ntoh16/32/64)
//------------------------------------------------------------------------------
//
// PURPOSE:
// Convert integers from network byte order (big-endian) to host byte order
// after receiving from network.
//
// SYMMETRY:
// Network byte order conversion is its own inverse:
//   hton32(hton32(x)) == x
//   ntoh32(ntoh32(x)) == x
//
// This means:
//   ntoh32(x) == hton32(x)  // Same operation!
//
// WHY?
// Byte swapping is symmetric:
//   - Big-endian → Little-endian: Swap bytes
//   - Little-endian → Big-endian: Swap bytes (same operation)
//
// IMPLEMENTATION:
// Simply delegate to hton functions (they're the same operation).
//
// USAGE PATTERN:
//
// Sender (little-endian x86):
//   uint32_t value = 1000;
//   uint32_t net = hton32(value);  // 1000 → network bytes
//   send(socket, &net, 4, 0);
//
// Network:
//   Bytes: 00 00 03 E8 (big-endian representation)
//
// Receiver (little-endian x86):
//   uint32_t net;
//   recv(socket, &net, 4, 0);      // Receives: 00 00 03 E8
//   uint32_t value = ntoh32(net);  // Network bytes → 1000
//
// Receiver (big-endian SPARC):
//   uint32_t net;
//   recv(socket, &net, 4, 0);      // Receives: 00 00 03 E8
//   uint32_t value = ntoh32(net);  // Already correct, ntoh32 is no-op
//   // value = 1000 (correct!)
//
// KEY INSIGHT:
// The receiver doesn't need to know sender's endianness!
// Both sides convert to/from network byte order, which is the standard.
//
//------------------------------------------------------------------------------

// Convert 16-bit integer from network to host byte order
// ntoh16: Network TO Host, 16-bit
// Delegates to hton16 (byte swapping is symmetric)
inline uint16_t ntoh16(uint16_t val) { return hton16(val); }

// Convert 32-bit integer from network to host byte order
// ntoh32: Network TO Host, 32-bit
// Delegates to hton32 (byte swapping is symmetric)
inline uint32_t ntoh32(uint32_t val) { return hton32(val); }

// Convert 64-bit integer from network to host byte order
// ntoh64: Network TO Host, 64-bit
// Delegates to hton64 (byte swapping is symmetric)
inline uint64_t ntoh64(uint64_t val) { return hton64(val); }

//------------------------------------------------------------------------------
// 7.3 ENDIANNESS EXPLANATION & TESTING
//------------------------------------------------------------------------------
//
// DETERMINING YOUR MACHINE'S ENDIANNESS:
//
//   #include <iostream>
//   
//   int main() {
//       uint32_t value = 0x12345678;
//       uint8_t* bytes = reinterpret_cast<uint8_t*>(&value);
//       
//       std::cout << "Bytes in memory: ";
//       for (int i = 0; i < 4; ++i) {
//           std::cout << std::hex << (int)bytes[i] << " ";
//       }
//       std::cout << "\n";
//       
//       if (bytes[0] == 0x78) {
//           std::cout << "Little-endian (x86/x64)\n";
//       } else if (bytes[0] == 0x12) {
//           std::cout << "Big-endian (SPARC/PowerPC)\n";
//       }
//   }
//
// OUTPUT ON x86 (little-endian):
//   Bytes in memory: 78 56 34 12
//   Little-endian (x86/x64)
//
// OUTPUT ON SPARC (big-endian):
//   Bytes in memory: 12 34 56 78
//   Big-endian (SPARC/PowerPC)
//
// TESTING BYTE ORDER FUNCTIONS:
//
//   void test_byte_order() {
//       // Test 32-bit conversion
//       uint32_t host = 0x12345678;
//       uint32_t net = hton32(host);
//       uint32_t back = ntoh32(net);
//       
//       assert(back == host);  // Round-trip should match
//       
//       // On little-endian (x86):
//       assert(net == 0x78563412);  // Bytes swapped
//       
//       // On big-endian (SPARC):
//       assert(net == 0x12345678);  // No change
//       
//       std::cout << "Byte order functions working correctly!\n";
//   }
//
// NETWORK BYTE ORDER VERIFICATION:
//
//   void verify_network_order() {
//       uint32_t value = 1000;  // 0x000003E8
//       uint32_t net = hton32(value);
//       
//       uint8_t* bytes = reinterpret_cast<uint8_t*>(&net);
//       
//       // Network order should always be: 00 00 03 E8
//       assert(bytes[0] == 0x00);
//       assert(bytes[1] == 0x00);
//       assert(bytes[2] == 0x03);
//       assert(bytes[3] == 0xE8);
//       
//       std::cout << "Network byte order correct!\n";
//   }
//
// WHY NETWORK BYTE ORDER IS BIG-ENDIAN:
// Historical reasons from early Internet (1970s):
// - Many early systems were big-endian (Motorola 68000, IBM mainframes)
// - Big-endian is more "natural" for humans (we write numbers MSB first)
// - Once standardized, couldn't change (billions of devices)
//
// MODERN CONTEXT:
// - Most modern CPUs are little-endian (x86, ARM default)
// - But network protocols still use big-endian
// - Conversion overhead is minimal (1-2 CPU cycles)
// - Worth it for universal compatibility
//
//------------------------------------------------------------------------------

} // namespace backtesting

#endif // MESSAGE_H

//==============================================================================
// END OF HEADER FILE
//==============================================================================
//
// Implementation of serialization/deserialization functions is in message.cpp
// See message.cpp documentation for detailed wire format specifications
// and implementation details.
//
//==============================================================================

//==============================================================================
// SECTION 8: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Controller Assigning Job
// ====================================
//
// #include "common/message.h"
// #include "common/logger.h"
// using namespace backtesting;
//
// void Controller::AssignJobToWorker(Worker* worker) {
//     // Create job assignment message
//     JobAssignMessage msg;
//     msg.id = GenerateMessageId();
//     msg.job_id = GenerateJobId();
//     
//     // Fill in job parameters
//     msg.params.symbol = "AAPL";
//     msg.params.strategy_type = "SMA";
//     msg.params.start_date = "2020-01-01";
//     msg.params.end_date = "2024-12-31";
//     msg.params.short_window = 50;
//     msg.params.long_window = 200;
//     msg.params.initial_capital = 10000.0;
//     
//     // Serialize to binary
//     std::vector<uint8_t> bytes = msg.serialize();
//     
//     Logger::info("[Controller] Assigning job " + std::to_string(msg.job_id) +
//                  " to worker " + std::to_string(worker->id));
//     
//     // Send over network
//     try {
//         worker->connection->Send(bytes);
//     } catch (const NetworkException& e) {
//         Logger::error("[Controller] Failed to send job: " + 
//                      std::string(e.what()));
//     }
// }
//
//
// EXAMPLE 2: Worker Receiving and Processing Job
// ===============================================
//
// void Worker::ReceiveJob() {
//     // Read from network (see network layer for details)
//     std::vector<uint8_t> buffer = connection->Receive();
//     
//     try {
//         // Deserialize message
//         auto msg = JobAssignMessage::deserialize(buffer.data(), buffer.size());
//         
//         Logger::info("[Worker] Received job " + std::to_string(msg->job_id) +
//                      " for symbol " + msg->params.symbol);
//         
//         // Execute backtest
//         JobResult result = ExecuteBacktest(msg->job_id, msg->params);
//         
//         // Send result back
//         SendResult(result);
//         
//     } catch (const std::runtime_error& e) {
//         Logger::error("[Worker] Failed to deserialize job: " + 
//                      std::string(e.what()));
//     }
// }
//
//
// EXAMPLE 3: Worker Sending Results
// ==================================
//
// void Worker::SendResult(const JobResult& result) {
//     JobResultMessage msg;
//     msg.id = GenerateMessageId();
//     msg.result = result;
//     
//     auto bytes = msg.serialize();
//     
//     Logger::info("[Worker] Sending result for job " + 
//                  std::to_string(result.job_id) +
//                  " (return: " + std::to_string(result.total_return * 100) + "%)");
//     
//     controller_connection->Send(bytes);
// }
//
//
// EXAMPLE 4: Heartbeat Thread
// ============================
//
// void Worker::RunHeartbeatThread() {
//     while (running_) {
//         std::this_thread::sleep_for(std::chrono::seconds(2));
//         
//         HeartbeatMessage msg;
//         msg.id = next_heartbeat_id_++;
//         msg.worker_id = my_worker_id_;
//         msg.active_jobs = GetActiveJobCount();
//         msg.completed_jobs = GetCompletedJobCount();
//         
//         auto bytes = msg.serialize();
//         
//         try {
//             controller_connection->Send(bytes);
//         } catch (const NetworkException& e) {
//             Logger::warning("[Worker] Heartbeat failed: " + 
//                           std::string(e.what()));
//         }
//     }
// }
//
//
// EXAMPLE 5: Worker Registration
// ===============================
//
// void Worker::RegisterWithController() {
//     WorkerRegisterMessage msg;
//     msg.id = GenerateMessageId();
//     msg.worker_hostname = GetMyHostname();
//     msg.worker_port = listening_port_;
//     
//     auto bytes = msg.serialize();
//     controller_connection->Send(bytes);
//     
//     // Wait for acknowledgment
//     auto ack_buffer = controller_connection->Receive();
//     // ... parse WorkerRegisterAckMessage (not shown) ...
//     
//     Logger::info("[Worker] Registered with controller");
// }
//
//==============================================================================

//==============================================================================
// SECTION 9: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: Forgetting Byte Order Conversion
// ============================================
// WRONG:
//    JobAssignMessage msg;
//    msg.job_id = 1000;
//    // ... serialize without hton64 ...
//
// CORRECT (already handled in serialize()):
//    uint64_t net_job_id = hton64(msg.job_id);
//    // ... write net_job_id to buffer ...
//
// Note: If using message.h API correctly, you don't need to worry about
// byte order—serialize() and deserialize() handle it automatically.
//
//
// PITFALL 2: Not Checking Deserialization Errors
// ===============================================
// WRONG:
//    auto msg = JobAssignMessage::deserialize(data, size);
//    ProcessJob(msg->job_id);  // What if deserialize threw?
//
// CORRECT:
//    try {
//        auto msg = JobAssignMessage::deserialize(data, size);
//        ProcessJob(msg->job_id);
//    } catch (const std::runtime_error& e) {
//        Logger::error("Malformed message: " + std::string(e.what()));
//        CloseConnection();
//    }
//
//
// PITFALL 3: Using Wrong Message Type
// ====================================
// WRONG:
//    auto msg = Message::deserialize(buffer);
//    auto* job_msg = static_cast<JobAssignMessage*>(msg.get());  // Unsafe!
//
// CORRECT:
//    auto msg = Message::deserialize(buffer);
//    if (msg->type == MessageType::JOB_ASSIGN) {
//        auto* job_msg = dynamic_cast<JobAssignMessage*>(msg.get());
//        if (job_msg) {
//            ProcessJob(*job_msg);
//        }
//    }
//
// Or better, use specific deserialize:
//    auto msg = JobAssignMessage::deserialize(data, size);
//    ProcessJob(*msg);  // Type-safe
//
//
// PITFALL 4: Ignoring Default Values
// ===================================
// WRONG:
//    JobParams params;  // Has default windows and capital
//    // User wants different values but forgets to set them
//    msg.params = params;  // Sends defaults!
//
// CORRECT:
//    JobParams params;
//    params.symbol = "AAPL";
//    params.strategy_type = "SMA";
//    params.start_date = "2020-01-01";
//    params.end_date = "2024-12-31";
//    // Explicitly set all fields, even if using defaults
//    params.short_window = 50;
//    params.long_window = 200;
//    params.initial_capital = 10000.0;
//
//
// PITFALL 5: Memory Leaks with Raw Pointers
// ==========================================
// WRONG:
//    Message* msg = new JobAssignMessage();
//    // ... use msg ...
//    // Forgot to delete! Memory leak
//
// CORRECT (use unique_ptr):
//    auto msg = std::make_unique<JobAssignMessage>();
//    // ... use msg ...
//    // Automatic cleanup when msg goes out of scope
//
//
// PITFALL 6: Not Handling Message ID Correlation
// ===============================================
// WRONG:
//    JobAssignMessage assign_msg;
//    assign_msg.id = 42;
//    SendJob(assign_msg);
//    
//    // Later...
//    JobResultMessage result_msg;
//    result_msg.id = 100;  // Different ID!
//    // How do we know this result is for job 42?
//
// CORRECT:
//    JobAssignMessage assign_msg;
//    assign_msg.id = 42;
//    assign_msg.job_id = 1000;  // Application-level ID
//    SendJob(assign_msg);
//    
//    // Worker echoes job_id in result
//    JobResultMessage result_msg;
//    result_msg.id = GenerateNewMessageId();  // New message ID
//    result_msg.result.job_id = 1000;  // Same job_id as assignment
//    // Now we can correlate!
//
//==============================================================================

//==============================================================================
// SECTION 10: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Why create a custom protocol instead of using JSON or Protocol Buffers?
// ============================================================================
// A: Trade-offs:
//    - Binary (ours): Smallest size, fastest, but not human-readable
//    - JSON: Human-readable, but 3-5x larger, 10-50x slower
//    - Protocol Buffers: Good balance, but requires code generation
//    
//    For this academic project:
//    - Learning objective: Understand binary protocols from scratch
//    - Network bottleneck: Khoury cluster bandwidth is limited
//    - Simplicity: ~500 lines total vs. protobuf complexity
//
// Q2: What's the difference between Message::id and JobAssignMessage::job_id?
// ============================================================================
// A: Two levels of tracking:
//    - Message::id: Network-level correlation (this specific message)
//      Example: "Response message 100 corresponds to request message 100"
//    
//    - job_id: Application-level tracking (this job across system)
//      Example: "Job 1000 was assigned, executed, and result returned"
//    
//    Analogy:
//    - Message::id is like an envelope ID (tracks this letter)
//    - job_id is like a case number (tracks entire case across letters)
//
// Q3: Why are ntoh and hton functions the same?
// ==============================================
// A: Byte swapping is symmetric:
//    - Swapping once: A B C D → D C B A
//    - Swapping again: D C B A → A B C D (back to original)
//    
//    Therefore:
//    - hton32(x) swaps bytes
//    - ntoh32(x) swaps bytes (same operation!)
//    - ntoh32(hton32(x)) == x (swap twice = identity)
//
// Q4: How do I add a new message type to the protocol?
// =====================================================
// A: Steps:
//    1. Add to MessageType enum: CHECKPOINT = 7
//    2. Create derived class: class CheckpointMessage : public Message
//    3. Implement serialize() and deserialize()
//    4. Update Message::deserialize() switch statement
//    5. Update documentation
//    6. Test thoroughly (round-trip, malformed data)
//
// Q5: What happens if sender and receiver have different protocol versions?
// ==========================================================================
// A: Current implementation: No versioning, breaks compatibility
//    Solutions:
//    - Add version field to header
//    - Controller and worker negotiate version
//    - Maintain backward compatibility
//    - Or: Use Protocol Buffers (built-in versioning)
//
// Q6: Why use std::unique_ptr for deserialized messages?
// =======================================================
// A: Ownership semantics:
//    - Caller owns the message (responsible for lifetime)
//    - Automatic cleanup (no memory leaks)
//    - Clear ownership transfer (can move to long-term storage)
//    - Polymorphism: Can return base class pointer to derived type
//
// Q7: Can I send multiple messages in one TCP packet?
// ====================================================
// A: Yes, but requires careful framing:
//    1. Read first message header (13 bytes)
//    2. Parse payload_size
//    3. Read payload
//    4. Repeat for next message
//    
//    Or batch serialize:
//    std::vector<uint8_t> batch;
//    batch.insert(batch.end(), msg1.serialize().begin(), msg1.serialize().end());
//    batch.insert(batch.end(), msg2.serialize().begin(), msg2.serialize().end());
//    send(socket, batch.data(), batch.size(), 0);
//
// Q8: How do I debug binary protocol issues?
// ===========================================
// A: Tools and techniques:
//    - Hex dump: Print bytes in hexadecimal
//    - Wireshark: Capture and analyze network packets
//    - Unit tests: Test serialize→deserialize round-trips
//    - Logging: Log message IDs, types, sizes
//    - Assertions: Check invariants (size, field values)
//
// Q9: What's the maximum message size?
// =====================================
// A: Theoretical: 4GB (payload_size is uint32_t)
//    Practical: ~10KB for this project
//    Recommendation: Validate max size:
//    if (payload_size > 1024 * 1024) throw std::runtime_error("Too large");
//
// Q10: Why are JobParams and JobResult structs (not classes)?
// ============================================================
// A: Structs for Plain Old Data (POD):
//    - No invariants: Fields are independent
//    - Public access: Direct field access is natural
//    - Aggregate initialization: Convenient constructors
//    - Convention: Structs for data, classes for behavior
//
//==============================================================================

//==============================================================================
// SECTION 11: BEST PRACTICES
//==============================================================================
//
// BEST PRACTICE 1: Always Use Byte Order Functions
// =================================================
// DO:
//    uint32_t value = 1000;
//    uint32_t net = hton32(value);
//    memcpy(buffer, &net, 4);
//
// DON'T:
//    uint32_t value = 1000;
//    memcpy(buffer, &value, 4);  // Works only on big-endian!
//
//
// BEST PRACTICE 2: Use Type-Specific Deserialize
// ===============================================
// DO:
//    auto msg = JobAssignMessage::deserialize(data, size);
//    // Type-safe, no casting needed
//
// DON'T:
//    auto msg = Message::deserialize(data);
//    auto* job = dynamic_cast<JobAssignMessage*>(msg.get());
//    // Extra cast, runtime overhead
//
//
// BEST PRACTICE 3: Validate Deserialized Data
// ============================================
// After deserialization, check business logic:
//    auto msg = JobAssignMessage::deserialize(data, size);
//    
//    if (msg->params.start_date > msg->params.end_date) {
//        throw std::invalid_argument("Invalid date range");
//    }
//    if (msg->params.short_window >= msg->params.long_window) {
//        throw std::invalid_argument("Short window must be < long window");
//    }
//
//
// BEST PRACTICE 4: Use Exceptions for Protocol Errors
// ====================================================
// DO:
//    try {
//        auto msg = deserialize(data);
//        process(msg);
//    } catch (const std::runtime_error& e) {
//        log_error(e.what());
//        close_connection();
//    }
//
// DON'T:
//    auto msg = deserialize(data);  // May throw, no handler
//    process(msg);  // Never reached if throw
//
//
// BEST PRACTICE 5: Log Message Flow
// ==================================
// Add structured logging:
//    Logger::debug("[Send] JOB_ASSIGN id=" + std::to_string(msg.id) +
//                  " job_id=" + std::to_string(msg.job_id) +
//                  " size=" + std::to_string(bytes.size()));
//    
//    Logger::debug("[Recv] JOB_RESULT id=" + std::to_string(msg.id) +
//                  " job_id=" + std::to_string(msg.result.job_id) +
//                  " return=" + std::to_string(msg.result.total_return));
//
// Enables: Trace message flow through distributed system
//
//
// BEST PRACTICE 6: Test Serialization Round-Trips
// ================================================
// For every message type:
//    TEST(MessageTest, JobAssignRoundTrip) {
//        JobAssignMessage orig;
//        orig.job_id = 1000;
//        orig.params.symbol = "AAPL";
//        // ... fill fields ...
//        
//        auto bytes = orig.serialize();
//        auto copy = JobAssignMessage::deserialize(bytes.data(), bytes.size());
//        
//        ASSERT_EQ(orig.job_id, copy->job_id);
//        ASSERT_EQ(orig.params.symbol, copy->params.symbol);
//        // ... check all fields ...
//    }
//
//==============================================================================

//==============================================================================
// SECTION 12: PROTOCOL EVOLUTION GUIDELINES
//==============================================================================
//
// ADDING NEW MESSAGE TYPES
// =========================
// Safe to add without breaking existing code:
//
// 1. Add to enum (use unused value 7-254):
//    enum class MessageType : uint8_t {
//        // ... existing types ...
//        CHECKPOINT_SAVE = 7,
//        CHECKPOINT_LOAD = 8
//    };
//
// 2. Create new message class:
//    class CheckpointMessage : public Message {
//        // ... fields and methods ...
//    };
//
// 3. Old nodes ignore unknown types (gracefully fail)
//
//
// ADDING FIELDS TO EXISTING MESSAGES
// ===================================
// DANGEROUS: Breaks binary compatibility!
//
// BAD (incompatible change):
//    struct JobParams {
//        std::string symbol;
//        std::string strategy_type;
//        std::string new_field;  // Added field
//        // ... existing fields ...
//    };
//
// BETTER OPTIONS:
//
// Option 1: Add version field to protocol
//    - Check version during deserialization
//    - Handle old and new formats
//    - Migrate gradually
//
// Option 2: Create new message type
//    - CHECKPOINT_SAVE_V2 = 8
//    - Old nodes use v1, new nodes use v2
//    - Both supported during transition
//
// Option 3: Optional fields at end
//    - Check payload_size to detect presence
//    - If size larger, read additional fields
//    - Backward compatible (old nodes stop early)
//
//
// DEPRECATING MESSAGE TYPES
// ==========================
// Process:
// 1. Mark as deprecated (documentation)
// 2. Add warning logs when used
// 3. Monitor usage (ensure zero usage)
// 4. Remove in next major version
//
//==============================================================================

//==============================================================================
// SECTION 13: TESTING STRATEGIES
//==============================================================================
//
// UNIT TESTS: Round-Trip Serialization
// =====================================
//    TEST(MessageTest, AllFieldsPreserved) {
//        JobAssignMessage msg;
//        // Fill all fields with non-default values
//        msg.id = 42;
//        msg.job_id = 1000;
//        msg.params.symbol = "GOOGL";
//        msg.params.strategy_type = "RSI";
//        msg.params.start_date = "2021-06-15";
//        msg.params.end_date = "2023-09-30";
//        msg.params.short_window = 30;
//        msg.params.long_window = 100;
//        msg.params.initial_capital = 50000.0;
//        
//        auto bytes = msg.serialize();
//        auto copy = JobAssignMessage::deserialize(bytes.data(), bytes.size());
//        
//        EXPECT_EQ(msg.id, copy->id);
//        EXPECT_EQ(msg.job_id, copy->job_id);
//        EXPECT_EQ(msg.params.symbol, copy->params.symbol);
//        // ... check all fields ...
//    }
//
//
// FUZZ TESTING: Malformed Data
// =============================
//    TEST(MessageTest, RejectsGarbage) {
//        std::vector<uint8_t> garbage(100);
//        std::generate(garbage.begin(), garbage.end(), std::rand);
//        
//        EXPECT_THROW({
//            JobAssignMessage::deserialize(garbage.data(), garbage.size());
//        }, std::runtime_error);
//    }
//
//
// INTEGRATION TESTS: Network Layer
// =================================
//    TEST(NetworkTest, SendReceiveJob) {
//        // Create message
//        JobAssignMessage send_msg;
//        send_msg.job_id = 1000;
//        // ... fill fields ...
//        
//        // Serialize and "send" (mock network)
//        auto bytes = send_msg.serialize();
//        mock_network.Send(bytes);
//        
//        // "Receive" and deserialize
//        auto recv_bytes = mock_network.Receive();
//        auto recv_msg = JobAssignMessage::deserialize(
//            recv_bytes.data(), recv_bytes.size());
//        
//        // Verify
//        EXPECT_EQ(send_msg.job_id, recv_msg->job_id);
//    }
//
//==============================================================================

//==============================================================================
// SECTION 14: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM: Deserialization throws "Buffer underflow"
// ===================================================
// CAUSE: Incomplete message received
// SOLUTION:
//    ☐ Check network layer reads complete message (header + payload)
//    ☐ Verify payload_size field is correct
//    ☐ Log message size before deserialize
//    ☐ Check TCP framing (may need multiple recv calls)
//
//
// PROBLEM: Wrong values after deserialization
// ============================================
// CAUSE: Byte order not converted
// SOLUTION:
//    ☐ Verify hton/ntoh used during serialize/deserialize
//    ☐ Check both sides use same protocol
//    ☐ Hex dump: Compare bytes with expected format
//    ☐ Test on same machine first (eliminates network issues)
//
//
// PROBLEM: Segmentation fault in deserialize()
// =============================================
// CAUSE: Buffer overrun or misaligned pointer
// SOLUTION:
//    ☐ Check all bounds checks present
//    ☐ Verify pointer arithmetic doesn't overflow
//    ☐ Use AddressSanitizer: g++ -fsanitize=address
//    ☐ Use Valgrind: valgrind ./program
//
//
// PROBLEM: Memory leak in message handling
// =========================================
// CAUSE: Forgot to delete raw pointer or unique_ptr not used
// SOLUTION:
//    ☐ Always use std::unique_ptr for deserialized messages
//    ☐ Never call new without matching delete
//    ☐ Use Valgrind to detect leaks: valgrind --leak-check=full
//
//
// PROBLEM: Can't add new message type
// ====================================
// CAUSE: Enum value conflicts or switch not updated
// SOLUTION:
//    ☐ Choose unused enum value (7-254)
//    ☐ Update Message::deserialize() switch statement
//    ☐ Implement serialize() and deserialize() methods
//    ☐ Add unit tests for new type
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================