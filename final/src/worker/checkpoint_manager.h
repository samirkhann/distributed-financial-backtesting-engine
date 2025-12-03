/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: checkpoint_manager.h
    
    Description:
        This header file defines the checkpoint management system for enabling
        fault-tolerant, resumable backtesting jobs in the distributed system.
        CheckpointManager allows workers to save intermediate progress during
        long-running jobs and resume from the last checkpoint if interrupted
        by crashes, network failures, or planned maintenance.
        
        Core Functionality:
        - Persist job execution state to disk at regular intervals
        - Enable resume from last checkpoint on worker restart
        - Prevent redundant computation after failures
        - Support graceful shutdown with progress preservation
        - Manage checkpoint lifecycle (create, load, delete)
        
        Why Checkpointing?
        
        WITHOUT CHECKPOINTING:
        Job: Backtest 5000 symbols, 10 minutes estimated runtime
        - Worker processes 4500 symbols (9 minutes of work)
        - Worker crashes at 90% complete
        - Controller reassigns job to new worker
        - New worker starts from beginning
        - 9 minutes of work LOST, another 10 minutes needed
        - Total: 19 minutes for 10-minute job (90% waste)
        
        WITH CHECKPOINTING:
        - Worker saves checkpoint every 1000 symbols (checkpoints at 1000, 2000, 3000, 4000)
        - Worker crashes at symbol 4500
        - New worker loads checkpoint from symbol 4000
        - New worker resumes from symbol 4001
        - Only 500 symbols re-processed (~1 minute)
        - Total: 10 minutes for 10-minute job (5% waste)
        
        Checkpoint Strategy:
        - FREQUENCY: Every 1000 symbols or every 60 seconds (configurable)
        - GRANULARITY: Per-job (each job has independent checkpoint)
        - STORAGE: Local disk per worker (not shared)
        - RETENTION: Deleted after job completion or expiration
        - ATOMICITY: Write to temp file, atomic rename on success
        
        Use Cases:
        1. WORKER CRASHES: Hardware failure, OOM kill, segfault
        2. PLANNED MAINTENANCE: Graceful worker shutdown for updates
        3. JOB PREEMPTION: Higher priority job needs worker's resources
        4. NETWORK PARTITIONS: Worker loses connection, reconnects
        5. SPOT INSTANCE TERMINATION: Cloud provider reclaims instance
        
        Fault Tolerance Benefits:
        - Reduces wasted computation: 90%+ savings on long jobs
        - Enables spot instance usage: Save $$ on cloud compute
        - Supports rolling upgrades: Restart workers without job loss
        - Improves user experience: Jobs complete even with failures
        - Increases system throughput: Less redundant work
        
        Trade-offs:
        - Disk space: ~1KB per checkpoint, MB for many concurrent jobs
        - I/O overhead: ~1-10ms per checkpoint save (negligible vs job runtime)
        - Complexity: Additional state management and recovery logic
        - Stale checkpoints: Need cleanup of old/abandoned checkpoints
        
        File Format (Binary):
        Each checkpoint stored as binary file in checkpoint_dir/job_<id>.ckpt:
```
        [8 bytes: job_id]
        [8 bytes: symbol_length]
        [N bytes: symbol string]
        [8 bytes: symbols_processed]
        [8 bytes: last_processed_index]
        [8 bytes: current_cash (double)]
        [4 bytes: current_shares (int)]
        [8 bytes: portfolio_value (double)]
        [8 bytes: date_length]
        [N bytes: last_date_processed string]
        [1 byte: valid flag]
```
        
        Checkpoint Lifecycle:
        1. Job assigned to worker
        2. Worker starts processing
        3. After 1000 symbols, save checkpoint
        4. Continue processing, save checkpoints periodically
        5. Job completes, delete checkpoint
        6. If worker crashes, new worker loads checkpoint and resumes
        
        Recovery Workflow:
        Worker receives job_id:
        - Check if checkpoint exists for job_id
        - If yes: Load checkpoint, resume from last position
        - If no: Start from beginning (first execution)
        - On completion: Delete checkpoint (cleanup)
        
    Dependencies:
        - <string>: String handling for paths and symbols
        - <cstdint>: Fixed-width integers for cross-platform compatibility
        - <vector>: Dynamic arrays for checkpoint lists
        - <fstream>: File I/O for checkpoint persistence
        - <mutex>: Thread-safe concurrent access
        
    Thread Safety:
        CheckpointManager is fully thread-safe. Multiple threads can:
        - Save checkpoints concurrently (for different jobs)
        - Load checkpoints while others are being saved
        - Query checkpoint existence simultaneously
        
        Internal mutex protects file operations and checkpoint directory access.
        
    File System Requirements:
        - checkpoint_dir must be writable
        - Sufficient disk space (~1KB per checkpoint)
        - Local filesystem recommended (not NFS) for atomicity
        - Atomic rename support (POSIX filesystems)
        
    Performance:
        - Save checkpoint: O(1), ~1-10ms (disk I/O)
        - Load checkpoint: O(1), ~1-5ms (disk I/O)
        - Check exists: O(1), ~0.1ms (stat syscall)
        - Delete: O(1), ~0.1ms (unlink syscall)
        - Get all IDs: O(n) where n = number of checkpoints, ~1-10ms
*******************************************************************************/

#ifndef CHECKPOINT_MANAGER_H
#define CHECKPOINT_MANAGER_H

#include <string>
#include <cstdint>
#include <vector>
#include <fstream>
#include <mutex>

namespace backtesting {

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
// 1. Overview and Architecture
// 2. Checkpoint Structure
// 3. CheckpointManager Class
//    3.1 Public Interface
//    3.2 Private Implementation
// 4. Usage Examples and Patterns
// 5. Checkpoint File Format
// 6. Recovery Scenarios
// 7. Performance Optimization
// 8. Common Pitfalls
// 9. FAQ
//==============================================================================

//==============================================================================
// SECTION 1: Overview and Architecture
//==============================================================================

/**
 * CHECKPOINTING IN DISTRIBUTED SYSTEMS
 * 
 * Checkpointing is a classic fault tolerance technique used in:
 * - High-performance computing (HPC): Save simulation state
 * - Databases: Transaction logs and recovery
 * - Video encoding: Resume after crash
 * - MapReduce: Task checkpointing
 * - Our system: Resumable backtesting jobs
 * 
 * CHECKPOINT FREQUENCY TRADEOFF:
 * 
 * TOO FREQUENT (every symbol):
 * - Pros: Minimal lost work on crash (< 1 symbol)
 * - Cons: High I/O overhead (1ms × 5000 symbols = 5 seconds)
 * - Cons: Disk wear, reduced performance
 * 
 * TOO INFREQUENT (once per job):
 * - Pros: Minimal overhead (1 disk write)
 * - Cons: Maximum lost work (entire job)
 * - Cons: No benefit over no checkpointing
 * 
 * OPTIMAL (every 1000 symbols or 60 seconds):
 * - Pros: Low overhead (~1-10ms per job)
 * - Pros: Reasonable recovery (max 1000 symbols lost)
 * - Balance: 1-2% overhead, 98%+ progress preserved
 * 
 * CHECKPOINT GRANULARITY:
 * 
 * PER-SYMBOL: Too fine-grained
 * - 5000 checkpoints for 5000-symbol job
 * - Excessive disk I/O and storage
 * 
 * PER-JOB: Too coarse-grained
 * - 1 checkpoint for entire job
 * - No benefit for mid-job failures
 * 
 * PER-BATCH: Just right
 * - 5 checkpoints for 5000-symbol job (every 1000)
 * - Reasonable overhead, good recovery
 * 
 * DISTRIBUTED CHECKPOINTING CONSIDERATIONS:
 * 
 * COORDINATOR (Controller):
 * - Tracks which workers have which jobs
 * - Knows which jobs have checkpoints
 * - Assigns jobs considering checkpoint availability
 * 
 * WORKER:
 * - Saves checkpoints to local disk (not shared storage)
 * - On startup, checks for existing checkpoints
 * - Resumes jobs from checkpoints if available
 * 
 * RECOVERY COORDINATOR:
 * - When worker fails, controller assigns job to new worker
 * - New worker checks if old worker left checkpoint
 * - If checkpoint exists, resume from there
 * - If no checkpoint, start from beginning
 * 
 * CHECKPOINT STORAGE DESIGN:
 * 
 * LOCAL vs SHARED STORAGE:
 * 
 * LOCAL (chosen):
 * - Each worker saves to its own disk
 * - Pros: Fast (no network), independent (no contention)
 * - Cons: If worker dies, checkpoint on dead worker's disk
 * - Solution: Accept loss, rely on periodic checkpoints
 * 
 * SHARED (alternative):
 * - All workers save to NFS/S3/etc.
 * - Pros: Checkpoints survive worker death
 * - Cons: Slower (network latency), potential conflicts
 * - Solution: Good for critical jobs, overkill for backtesting
 * 
 * HYBRID (best):
 * - Save to local disk for speed
 * - Periodically sync to shared storage for durability
 * - Recover from shared storage if local unavailable
 */

//==============================================================================
// SECTION 2: Checkpoint Structure
//==============================================================================

/**
 * @struct Checkpoint
 * @brief Captures complete state of a partially-completed backtesting job
 * 
 * PURPOSE:
 * Enables resuming job execution from a known-good intermediate state.
 * Contains all information needed to reconstruct worker state and
 * continue processing without repeating completed work.
 * 
 * STATE SNAPSHOT:
 * Checkpoint represents job state at a specific point in time:
 * - Which symbol was being processed
 * - How many symbols completed so far
 * - Current portfolio state (cash, shares, value)
 * - Last price bar processed
 * 
 * USAGE PATTERN:
 * 
 * SAVING:
 * ```cpp
 * Checkpoint ckpt;
 * ckpt.job_id = 42;
 * ckpt.symbol = "AAPL";
 * ckpt.symbols_processed = 1500;
 * ckpt.last_processed_index = 999;
 * ckpt.current_cash = 12500.0;
 * ckpt.current_shares = 50;
 * ckpt.portfolio_value = 15000.0;
 * ckpt.last_date_processed = "2023-06-15";
 * ckpt.valid = true;
 * 
 * checkpoint_mgr.save_checkpoint(ckpt);
 * ```
 * 
 * LOADING:
 * ```cpp
 * Checkpoint ckpt;
 * if (checkpoint_mgr.load_checkpoint(job_id, ckpt)) {
 *     if (ckpt.valid) {
 *         // Resume from checkpoint
 *         start_index = ckpt.symbols_processed;
 *         portfolio.cash = ckpt.current_cash;
 *         portfolio.shares = ckpt.current_shares;
 *     }
 * }
 * ```
 * 
 * SERIALIZATION FORMAT:
 * Binary format for efficiency and portability.
 * All multi-byte integers in little-endian (x86 native).
 * Strings length-prefixed (8-byte length + data).
 * 
 * SIZE ESTIMATION:
 * - Fixed fields: ~60 bytes
 * - Variable strings: symbol (4-6 bytes) + date (10 bytes)
 * - Total: ~80-100 bytes per checkpoint
 * - For 100 concurrent jobs: ~10KB total
 */
struct Checkpoint {
    /**
     * @brief Job identifier this checkpoint belongs to
     * 
     * USAGE:
     * - Filename: checkpoint_dir/job_<job_id>.ckpt
     * - Correlation: Match checkpoint to job assignment
     * - Cleanup: Delete checkpoint when job completes
     * 
     * EXAMPLE:
     * job_id = 42 → checkpoint file: ./checkpoints/job_42.ckpt
     */
    uint64_t job_id;
    
    /**
     * @brief Stock symbol currently being processed
     * 
     * MULTI-SYMBOL JOBS:
     * For jobs processing multiple symbols (e.g., entire S&P 500),
     * this indicates which symbol was in progress when checkpoint saved.
     * 
     * RESUME LOGIC:
     * - If symbol matches next symbol to process: Continue within symbol
     * - If symbol completed: Move to next symbol
     * 
     * EXAMPLE:
     * Job symbols: [AAPL, GOOGL, MSFT, AMZN]
     * Checkpoint: symbol = "GOOGL", symbols_processed = 2
     * Resume: Continue with GOOGL's remaining bars, then MSFT, AMZN
     */
    std::string symbol;
    
    /**
     * @brief Number of symbols fully processed
     * 
     * SEMANTICS:
     * Count of symbols completely backtested (all bars processed).
     * Does NOT include current symbol (in progress).
     * 
     * EXAMPLE:
     * Job has 5000 symbols total
     * symbols_processed = 2500
     * → 50% complete, 2500 symbols remaining
     * 
     * RESUME INDEX:
     * Next symbol to process = symbols_processed + 1
     * (if current symbol incomplete) or symbols_processed (if complete)
     * 
     * PROGRESS CALCULATION:
     * ```cpp
     * double progress = (double)symbols_processed / total_symbols;
     * std::cout << "Progress: " << (progress * 100) << "%\n";
     * ```
     */
    uint64_t symbols_processed;
    
    /**
     * @brief Last price bar index processed within current symbol
     * 
     * INTRA-SYMBOL PROGRESS:
     * Indicates how far we've processed within the current symbol's data.
     * 
     * EXAMPLE:
     * Symbol AAPL has 1000 price bars (daily data for ~4 years)
     * last_processed_index = 650
     * → Processed bars 0-650, need to process 651-999
     * 
     * RESUME LOGIC:
     * ```cpp
     * for (size_t i = ckpt.last_processed_index + 1; i < price_data.size(); ++i) {
     *     // Process bar at index i
     * }
     * ```
     * 
     * FIRST BAR:
     * last_processed_index = 0 means first bar processed
     * To resume: Start from index 1
     * 
     * SYMBOL COMPLETE:
     * last_processed_index = price_data.size() - 1 means all bars done
     * To resume: Move to next symbol
     * 
     * INVARIANT:
     * last_processed_index < total_bars_for_current_symbol
     */
    uint64_t last_processed_index;
    
    /**
     * @brief Cash available in portfolio at checkpoint time
     * 
     * PORTFOLIO STATE:
     * Part of PortfolioState needed to resume trading simulation.
     * 
     * USAGE ON RESUME:
     * ```cpp
     * PortfolioState portfolio;
     * portfolio.cash = ckpt.current_cash;
     * portfolio.shares_held = ckpt.current_shares;
     * portfolio.portfolio_value = ckpt.portfolio_value;
     * 
     * // Continue backtesting from this state
     * ```
     * 
     * VALIDATION:
     * current_cash should be >= 0 (no margin in this system).
     * If negative, checkpoint may be corrupted.
     * 
     * EXAMPLE:
     * Started with $10,000, bought shares for $8,000
     * current_cash = $2,000 (remaining cash)
     */
    double current_cash;
    
    /**
     * @brief Number of shares held in portfolio at checkpoint time
     * 
     * POSITION STATE:
     * Combined with current_cash, defines complete portfolio position.
     * 
     * USAGE ON RESUME:
     * Restore shares_held to continue marking-to-market.
     * 
     * VALIDATION:
     * current_shares should be >= 0 (long-only strategy).
     * If negative, checkpoint corrupted or using short selling.
     * 
     * EXAMPLE:
     * Bought 100 shares at $80, now holding
     * current_shares = 100
     */
    int current_shares;
    
    /**
     * @brief Total portfolio value at checkpoint time
     * 
     * CALCULATED VALUE:
     * portfolio_value = current_cash + (current_shares × current_price)
     * 
     * USAGE:
     * - Resume portfolio value tracking
     * - Calculate returns from checkpoint point
     * - Verify checkpoint integrity
     * 
     * INTEGRITY CHECK:
     * ```cpp
     * double expected = ckpt.current_cash + 
     *                   (ckpt.current_shares * current_price);
     * if (abs(expected - ckpt.portfolio_value) > 0.01) {
     *     Logger::warn("Checkpoint portfolio value mismatch");
     * }
     * ```
     * 
     * EXAMPLE:
     * Cash: $2,000, Shares: 100 @ $130 each
     * portfolio_value = $2,000 + $13,000 = $15,000
     */
    double portfolio_value;
    
    /**
     * @brief Last trading date processed (ISO format: YYYY-MM-DD)
     * 
     * TEMPORAL TRACKING:
     * Indicates chronological position in backtest.
     * 
     * USAGE:
     * - Display progress to user ("Currently at 2023-06-15")
     * - Debug time-based issues
     * - Verify checkpoint isn't too old
     * 
     * FORMAT:
     * ISO 8601 date format: "YYYY-MM-DD"
     * Example: "2023-06-15", "2024-12-31"
     * 
     * STALENESS CHECK:
     * ```cpp
     * auto checkpoint_date = parse_date(ckpt.last_date_processed);
     * auto now = current_date();
     * auto age = now - checkpoint_date;
     * 
     * if (age > std::chrono::hours(24)) {
     *     Logger::warn("Checkpoint is stale (>24h old)");
     * }
     * ```
     */
    std::string last_date_processed;
    
    /**
     * @brief Flag indicating whether checkpoint data is valid
     * 
     * VALIDITY SEMANTICS:
     * 
     * valid = true:
     * - Checkpoint successfully saved
     * - All fields populated correctly
     * - Safe to use for resume
     * 
     * valid = false:
     * - Checkpoint empty or corrupted
     * - Load failed or file doesn't exist
     * - Should not use for resume (start from beginning)
     * 
     * USAGE:
     * ```cpp
     * Checkpoint ckpt;
     * if (mgr.load_checkpoint(job_id, ckpt) && ckpt.valid) {
     *     // Safe to resume from checkpoint
     *     resume_job(ckpt);
     * } else {
     *     // Start job from beginning
     *     start_new_job();
     * }
     * ```
     * 
     * WHY NEEDED?
     * Distinguishes between:
     * - "Checkpoint loaded successfully" (load_checkpoint returns true)
     * - "Checkpoint data is usable" (ckpt.valid == true)
     * 
     * Can have: load succeeds but valid=false (corrupt data detected).
     */
    bool valid;
    
    /**
     * @brief Default constructor initializing to safe empty state
     * 
     * INITIALIZATION:
     * - All numeric fields to 0
     * - Strings to empty
     * - valid to false (unusable until populated)
     * 
     * USAGE:
     * ```cpp
     * Checkpoint ckpt;  // Safe empty state
     * // ckpt.valid == false, won't be used accidentally
     * 
     * // Load from file
     * if (mgr.load_checkpoint(42, ckpt)) {
     *     // ckpt now populated, ckpt.valid indicates usability
     * }
     * ```
     */
    Checkpoint() : job_id(0), symbols_processed(0), last_processed_index(0),
                   current_cash(0), current_shares(0), portfolio_value(0),
                   valid(false) {}
};

//==============================================================================
// SECTION 3: CheckpointManager Class
//==============================================================================

/**
 * @class CheckpointManager
 * @brief Manages persistent storage and retrieval of job checkpoints
 * 
 * RESPONSIBILITIES:
 * - Save checkpoints to disk atomically
 * - Load checkpoints from disk with validation
 * - Manage checkpoint file lifecycle (create, query, delete)
 * - Provide thread-safe concurrent access
 * - Handle filesystem errors gracefully
 * 
 * DESIGN PRINCIPLES:
 * 
 * 1. SIMPLICITY:
 *    One file per job (job_<id>.ckpt)
 *    Binary format for efficiency
 *    No complex indexing or database
 * 
 * 2. ATOMICITY:
 *    Write to temp file, rename on success
 *    Either complete checkpoint or no checkpoint (no partial)
 * 
 * 3. THREAD SAFETY:
 *    Internal mutex protects concurrent access
 *    Multiple jobs can checkpoint simultaneously
 * 
 * 4. ROBUSTNESS:
 *    Handles missing files, corrupted data, full disks
 *    Never crashes on checkpoint errors (degrades gracefully)
 * 
 * TYPICAL WORKFLOW:
 * 
 * Worker receives job_id=42:
 * 
 * 1. Check for existing checkpoint:
 * ```cpp
 * CheckpointManager mgr("./checkpoints");
 * Checkpoint ckpt;
 * 
 * if (mgr.load_checkpoint(42, ckpt) && ckpt.valid) {
 *     // Resume from checkpoint
 *     start_index = ckpt.symbols_processed;
 *     Logger::info("Resuming from symbol " + std::to_string(start_index));
 * } else {
 *     // Start from beginning
 *     start_index = 0;
 *     Logger::info("Starting job from scratch");
 * }
 * ```
 * 
 * 2. Process symbols, saving checkpoints periodically:
 * ```cpp
 * for (size_t i = start_index; i < total_symbols; ++i) {
 *     process_symbol(symbols[i]);
 *     
 *     if ((i + 1) % 1000 == 0) {  // Every 1000 symbols
 *         Checkpoint ckpt;
 *         ckpt.job_id = 42;
 *         ckpt.symbols_processed = i + 1;
 *         // ... populate other fields ...
 *         ckpt.valid = true;
 *         
 *         mgr.save_checkpoint(ckpt);
 *         Logger::info("Saved checkpoint at symbol " + std::to_string(i+1));
 *     }
 * }
 * ```
 * 
 * 3. On completion, delete checkpoint:
 * ```cpp
 * mgr.delete_checkpoint(42);
 * Logger::info("Job complete, checkpoint deleted");
 * ```
 * 
 * RECOVERY SCENARIO:
 * 
 * Timeline:
 *   T0: Worker starts job 42, processes symbols 0-999
 *   T1: Saves checkpoint (symbols_processed=1000)
 *   T2: Processes symbols 1000-1999
 *   T3: Saves checkpoint (symbols_processed=2000)
 *   T4: Processing symbol 2456...
 *   T5: **WORKER CRASHES** (power failure, kernel panic, etc.)
 *   
 *   T6: Controller detects worker death (heartbeat timeout)
 *   T7: Controller assigns job 42 to Worker 2
 *   T8: Worker 2 checks for checkpoint
 *   T9: Worker 2 loads checkpoint (symbols_processed=2000)
 *  T10: Worker 2 resumes from symbol 2000
 *  T11: Processes symbols 2000-2456 (re-work)
 *  T12: Continues from 2457 onward (new work)
 *  T13: Completes job, deletes checkpoint
 * 
 * WORK SAVED:
 * - Total symbols: 5000
 * - Processed before crash: 2456
 * - Last checkpoint: 2000
 * - Re-work needed: 456 symbols (9%)
 * - Work saved: 2000 symbols (91%)
 * 
 * THREAD SAFETY:
 * Checkpoint struct itself is NOT thread-safe (plain old data).
 * But CheckpointManager methods are thread-safe.
 * 
 * Don't share Checkpoint objects between threads:
 * ```cpp
 * // BAD: Shared checkpoint object
 * Checkpoint shared_ckpt;
 * 
 * // Thread 1
 * shared_ckpt.symbols_processed = 1000;  // Race condition!
 * 
 * // Thread 2
 * mgr.save_checkpoint(shared_ckpt);  // May save partial data!
 * ```
 * 
 * Safe pattern (separate objects):
 * ```cpp
 * // GOOD: Each thread has own checkpoint
 * 
 * // Thread 1
 * Checkpoint ckpt1;
 * ckpt1.job_id = 42;
 * // ... populate ...
 * mgr.save_checkpoint(ckpt1);
 * 
 * // Thread 2
 * Checkpoint ckpt2;
 * ckpt2.job_id = 43;
 * // ... populate ...
 * mgr.save_checkpoint(ckpt2);  // Different job, no conflict
 * ```
 */

//==============================================================================
// SECTION 3: CheckpointManager Class
//==============================================================================

/**
 * @class CheckpointManager
 * @brief Thread-safe manager for job checkpoint persistence
 * 
 * DESIGN OVERVIEW:
 * 
 * CheckpointManager provides a simple, robust interface for checkpoint
 * operations without exposing filesystem details. It encapsulates:
 * - File path generation (job_id → filename)
 * - Binary serialization (Checkpoint → bytes)
 * - Atomic writes (temp file + rename)
 * - Error handling (missing files, corrupted data)
 * - Thread synchronization (mutex protection)
 * 
 * FILESYSTEM LAYOUT:
 * 
 * checkpoint_dir/
 * ├── job_42.ckpt      (checkpoint for job 42)
 * ├── job_43.ckpt      (checkpoint for job 43)
 * ├── job_44.ckpt      (checkpoint for job 44)
 * └── ...
 * 
 * NAMING CONVENTION:
 * - Prefix: "job_"
 * - ID: Job identifier (decimal)
 * - Extension: ".ckpt"
 * 
 * Example: job_id=1234 → filename="job_1234.ckpt"
 * 
 * CONCURRENCY MODEL:
 * 
 * Multiple workers can use same CheckpointManager instance:
 * - Each worker processing different job
 * - save_checkpoint() serialized by mutex (no file conflicts)
 * - Reads (load_checkpoint, checkpoint_exists) can happen concurrently
 * 
 * STORAGE REQUIREMENTS:
 * 
 * DISK SPACE:
 * - Per checkpoint: ~100 bytes
 * - 1000 concurrent jobs: ~100KB
 * - 1M historical checkpoints: ~100MB
 * 
 * PERFORMANCE:
 * - Save: 1-10ms (depends on disk speed)
 * - Load: 1-5ms
 * - Check: <1ms (stat syscall)
 * 
 * Negligible overhead compared to backtest runtime (seconds to minutes).
 * 
 * CLEANUP POLICY:
 * 
 * Checkpoints should be deleted:
 * - After job completion (success)
 * - After job failure (error)
 * - After expiration (e.g., > 24 hours old)
 * 
 * Implement periodic cleanup:
 * ```cpp
 * void cleanup_old_checkpoints(CheckpointManager& mgr) {
 *     auto all_ids = mgr.get_all_checkpoint_ids();
 *     for (auto id : all_ids) {
 *         // Check if job still active
 *         if (!is_job_active(id)) {
 *             mgr.delete_checkpoint(id);
 *         }
 *     }
 * }
 * ```
 * 
 * USAGE EXAMPLE:
 * ```cpp
 * CheckpointManager mgr("/var/worker/checkpoints");
 * 
 * // Check for existing checkpoint
 * if (mgr.checkpoint_exists(job_id)) {
 *     Checkpoint ckpt;
 *     if (mgr.load_checkpoint(job_id, ckpt) && ckpt.valid) {
 *         resume_from_checkpoint(ckpt);
 *     }
 * } else {
 *     start_job_from_beginning();
 * }
 * 
 * // Save checkpoint periodically
 * if (should_checkpoint()) {
 *     Checkpoint ckpt = create_checkpoint();
 *     mgr.save_checkpoint(ckpt);
 * }
 * 
 * // Delete on completion
 * mgr.delete_checkpoint(job_id);
 * ```
 */
class CheckpointManager {
private:
    /**
     * @brief Directory where checkpoint files are stored
     * 
     * EXAMPLES:
     * - "./checkpoints" (relative to current directory)
     * - "/var/worker/checkpoints" (absolute path)
     * - "/tmp/checkpoints_worker1" (temp directory per worker)
     * 
     * REQUIREMENTS:
     * - Must exist (not created automatically by all methods)
     * - Must be writable
     * - Recommend local disk (not NFS) for atomic rename
     * 
     * ISOLATION:
     * Each worker should have unique checkpoint_dir:
     * - Worker 1: /tmp/ckpt1
     * - Worker 2: /tmp/ckpt2
     * Prevents file conflicts if workers on same host.
     */
    std::string checkpoint_dir_;
    
    /**
     * @brief Mutex protecting concurrent checkpoint operations
     * 
     * CRITICAL SECTIONS:
     * - File creation/deletion (prevent race conditions)
     * - Directory scanning (get_all_checkpoint_ids)
     * - Path generation (thread-safe string ops)
     * 
     * GRANULARITY:
     * Coarse-grained (single mutex for all operations).
     * Alternative: Per-job mutex for better concurrency.
     * 
     * mutable:
     * Allows locking in const methods (checkpoint_exists, get_checkpoint_path).
     */
    mutable std::mutex mutex_;
    
    /**
     * @brief Generates filesystem path for job's checkpoint file
     * 
     * FORMULA:
     * path = checkpoint_dir + "/job_" + to_string(job_id) + ".ckpt"
     * 
     * @param job_id Job identifier
     * 
     * @return Full path to checkpoint file
     * 
     * EXAMPLE:
     * checkpoint_dir_ = "/var/checkpoints"
     * job_id = 42
     * Returns: "/var/checkpoints/job_42.ckpt"
     * 
     * THREAD SAFETY:
     * Typically called with mutex_ held.
     * String operations are thread-safe.
     * 
     * PATH SEPARATOR:
     * Assumes POSIX path separator (/).
     * For cross-platform, should use std::filesystem::path:
     * ```cpp
     * #include <filesystem>
     * auto path = std::filesystem::path(checkpoint_dir_) / 
     *             ("job_" + std::to_string(job_id) + ".ckpt");
     * return path.string();
     * ```
     */
    std::string get_checkpoint_path(uint64_t job_id) const;
    
public:
    //==========================================================================
    // SECTION 3.1: Public Interface
    //==========================================================================
    
    /**
     * @brief Constructs checkpoint manager for specified directory
     * 
     * @param checkpoint_dir Directory for storing checkpoint files
     * 
     * DIRECTORY HANDLING:
     * - If directory doesn't exist, may be created by save_checkpoint()
     * - If directory exists, validates it's writable (not implemented!)
     * 
     * PRODUCTION IMPROVEMENT:
     * ```cpp
     * CheckpointManager(const std::string& dir) : checkpoint_dir_(dir) {
     *     // Create directory if doesn't exist
     *     std::filesystem::create_directories(checkpoint_dir_);
     *     
     *     // Verify writable
     *     if (!is_writable(checkpoint_dir_)) {
     *         throw std::runtime_error("Directory not writable: " + dir);
     *     }
     * }
     * ```
     * 
     * EXAMPLE USAGE:
     * ```cpp
     * // Worker-specific checkpoint directory
     * std::string dir = "/tmp/checkpoints_" + get_worker_hostname();
     * CheckpointManager mgr(dir);
     * ```
     */
    explicit CheckpointManager(const std::string& checkpoint_dir = "./checkpoints");
    
    /**
     * @brief Saves checkpoint to disk atomically
     * 
     * ATOMIC WRITE PATTERN:
     * 1. Write to temporary file (job_<id>.ckpt.tmp)
     * 2. Flush buffers to ensure data on disk
     * 3. Atomic rename: job_<id>.ckpt.tmp → job_<id>.ckpt
     * 4. If crash before rename, temp file ignored
     * 5. If crash after rename, checkpoint complete
     * 
     * @param checkpoint Checkpoint to save
     * 
     * @return true if saved successfully, false on error
     * 
     * PRECONDITIONS:
     * - checkpoint.valid should be true (caller sets this)
     * - checkpoint.job_id must be non-zero
     * 
     * POSTCONDITIONS:
     * - Checkpoint file exists on disk
     * - File contains complete, valid checkpoint data
     * - Previous checkpoint (if any) overwritten
     * 
     * SERIALIZATION FORMAT:
     * Binary format (see "Section 5: Checkpoint File Format")
     * 
     * ERROR HANDLING:
     * - Directory doesn't exist: Creates it (or fails gracefully)
     * - Disk full: Returns false, logs error
     * - Permission denied: Returns false, logs error
     * - I/O error: Returns false, logs error
     * 
     * DURABILITY:
     * Uses fflush() to flush C++ buffers.
     * Should also call fsync() for guaranteed durability:
     * ```cpp
     * file.flush();
     * fsync(fileno(file));  // Ensure on disk
     * ```
     * 
     * THREAD SAFETY:
     * Fully thread-safe via mutex.
     * Multiple threads can save different jobs concurrently.
     * 
     * EXAMPLE:
     * ```cpp
     * Checkpoint ckpt;
     * ckpt.job_id = 42;
     * ckpt.symbol = "AAPL";
     * ckpt.symbols_processed = 1500;
     * ckpt.last_processed_index = 999;
     * ckpt.current_cash = 5000.0;
     * ckpt.current_shares = 100;
     * ckpt.portfolio_value = 15000.0;
     * ckpt.last_date_processed = "2023-06-15";
     * ckpt.valid = true;
     * 
     * if (!mgr.save_checkpoint(ckpt)) {
     *     Logger::error("Failed to save checkpoint for job 42");
     *     // Continue anyway (checkpoint is optimization, not critical)
     * }
     * ```
     * 
     * PERFORMANCE:
     * - Typical: 1-5ms (SSD)
     * - Slow disk: 10-50ms (HDD)
     * - Network storage: 50-200ms (avoid if possible!)
     * 
     * FREQUENCY RECOMMENDATION:
     * Save checkpoint when:
     * - Every 1000 symbols processed
     * - Every 60 seconds elapsed
     * - Before graceful shutdown
     * - After completing major milestone
     */
    bool save_checkpoint(const Checkpoint& checkpoint);
    
    /**
     * @brief Loads checkpoint from disk
     * 
     * @param job_id Job identifier to load checkpoint for
     * @param checkpoint [out] Loaded checkpoint data
     * 
     * @return true if file exists and loaded, false if not found or error
     * 
     * LOAD SEQUENCE:
     * 1. Generate checkpoint file path
     * 2. Check if file exists
     * 3. Open file for binary reading
     * 4. Deserialize all fields
     * 5. Validate checkpoint integrity
     * 6. Set checkpoint.valid based on validation
     * 
     * RETURN VALUE vs valid FLAG:
     * 
     * CASE 1: File doesn't exist
     * - Returns: false
     * - checkpoint.valid: false
     * - Meaning: No checkpoint for this job
     * 
     * CASE 2: File exists, loads successfully
     * - Returns: true
     * - checkpoint.valid: true
     * - Meaning: Checkpoint ready to use
     * 
     * CASE 3: File exists but corrupted
     * - Returns: true (file found and read)
     * - checkpoint.valid: false (data invalid)
     * - Meaning: Checkpoint exists but unusable
     * 
     * USAGE PATTERN:
     * ```cpp
     * Checkpoint ckpt;
     * if (mgr.load_checkpoint(job_id, ckpt)) {
     *     // File exists and loaded
     *     if (ckpt.valid) {
     *         // Data is good, safe to resume
     *         resume_from(ckpt);
     *     } else {
     *         // File corrupted, start from scratch
     *         Logger::warn("Corrupted checkpoint, starting fresh");
     *         start_from_beginning();
     *     }
     * } else {
     *     // No checkpoint file, first run
     *     start_from_beginning();
     * }
     * ```
     * 
     * VALIDATION CHECKS:
     * After loading, verify:
     * - job_id matches requested id
     * - cash >= 0 (no negative)
     * - shares >= 0 (long-only)
     * - symbols_processed <= total_symbols
     * - Dates in valid format
     * 
     * CORRUPTION DETECTION:
     * ```cpp
     * bool validate_checkpoint(const Checkpoint& ckpt) {
     *     if (ckpt.job_id == 0) return false;
     *     if (ckpt.current_cash < 0) return false;
     *     if (ckpt.current_shares < 0) return false;
     *     if (ckpt.symbol.empty()) return false;
     *     // Add more checks as needed
     *     return true;
     * }
     * ```
     * 
     * THREAD SAFETY:
     * Safe to load while other threads save different jobs.
     * Not safe to load same job from multiple threads (unnecessary).
     * 
     * PERFORMANCE:
     * - File access: ~0.1ms (cached) to ~1ms (not cached)
     * - Deserialization: ~0.1ms
     * - Total: ~1-5ms typical
     */
    bool load_checkpoint(uint64_t job_id, Checkpoint& checkpoint);
    
    /**
     * @brief Checks if checkpoint file exists for given job
     * 
     * @param job_id Job identifier to check
     * 
     * @return true if checkpoint file exists, false otherwise
     * 
     * USAGE:
     * Quick check before attempting load (avoids unnecessary load).
     * 
     * EXAMPLE:
     * ```cpp
     * if (mgr.checkpoint_exists(job_id)) {
     *     Checkpoint ckpt;
     *     mgr.load_checkpoint(job_id, ckpt);
     *     // ... use checkpoint ...
     * } else {
     *     // No checkpoint, start fresh
     * }
     * ```
     * 
     * OPTIMIZATION:
     * If you're going to load anyway, can skip this check:
     * ```cpp
     * // No need for exists check, just try to load
     * Checkpoint ckpt;
     * if (mgr.load_checkpoint(job_id, ckpt) && ckpt.valid) {
     *     resume_from(ckpt);
     * } else {
     *     start_fresh();
     * }
     * ```
     * 
     * IMPLEMENTATION:
     * Typically uses stat() or std::ifstream::is_open():
     * ```cpp
     * bool checkpoint_exists(uint64_t job_id) const {
     *     std::string path = get_checkpoint_path(job_id);
     *     std::ifstream file(path);
     *     return file.good();
     * }
     * ```
     * 
     * THREAD SAFETY:
     * Safe to call concurrently with save/load.
     * File may be created between exists() check and load() call
     * (TOCTOU - Time Of Check, Time Of Use race), but harmless here.
     * 
     * PERFORMANCE:
     * Very fast (~0.1ms), just a filesystem stat.
     */
    bool checkpoint_exists(uint64_t job_id) const;
    
    /**
     * @brief Deletes checkpoint file for given job
     * 
     * @param job_id Job identifier whose checkpoint to delete
     * 
     * @return true if deleted successfully or doesn't exist, false on error
     * 
     * WHEN TO DELETE:
     * 
     * 1. JOB COMPLETION:
     * ```cpp
     * JobResult result = execute_job(job);
     * send_result_to_controller(result);
     * mgr.delete_checkpoint(job.id);  // Cleanup
     * ```
     * 
     * 2. JOB FAILURE:
     * ```cpp
     * try {
     *     execute_job(job);
     * } catch (const std::exception& e) {
     *     Logger::error("Job failed: " + std::string(e.what()));
     *     mgr.delete_checkpoint(job.id);  // Cleanup failed job
     * }
     * ```
     * 
     * 3. PERIODIC CLEANUP:
     * ```cpp
     * // Delete checkpoints for completed jobs
     * auto all_ckpts = mgr.get_all_checkpoint_ids();
     * for (auto id : all_ckpts) {
     *     if (job_is_complete(id) || job_is_stale(id)) {
     *         mgr.delete_checkpoint(id);
     *     }
     * }
     * ```
     * 
     * IDEMPOTENT:
     * Safe to call even if checkpoint doesn't exist.
     * Returns true in both cases (deleted or already gone).
     * 
     * ERROR CASES:
     * - Permission denied: Returns false
     * - File locked by other process: Returns false
     * - I/O error: Returns false
     * 
     * THREAD SAFETY:
     * Safe to call concurrently.
     * If multiple threads delete same job, only first succeeds
     * (others return true anyway - idempotent).
     * 
     * EXAMPLE:
     * ```cpp
     * // Cleanup after job completion
     * if (!mgr.delete_checkpoint(job_id)) {
     *     Logger::warn("Failed to delete checkpoint for job " + 
     *                  std::to_string(job_id));
     *     // Not critical, checkpoint just takes disk space
     * }
     * ```
     * 
     * PERFORMANCE:
     * Fast (~0.1ms), just an unlink() syscall.
     */
    bool delete_checkpoint(uint64_t job_id);
    
    /**
     * @brief Retrieves list of all job IDs with checkpoints
     * 
     * @return Vector of job IDs (empty if no checkpoints)
     * 
     * PURPOSE:
     * - Discover orphaned checkpoints (jobs completed but checkpoint not deleted)
     * - Implement cleanup policies
     * - Monitor checkpoint disk usage
     * - Resume multiple jobs after worker restart
     * 
     * ALGORITHM:
     * 1. Scan checkpoint_dir for files matching "job_*.ckpt"
     * 2. Extract job_id from each filename
     * 3. Return vector of IDs
     * 
     * EXAMPLE DIRECTORY:
     * ```
     * checkpoints/
     * ├── job_42.ckpt
     * ├── job_43.ckpt
     * ├── job_100.ckpt
     * └── other_file.txt (ignored, doesn't match pattern)
     * ```
     * 
     * Returns: [42, 43, 100]
     * 
     * USAGE:
     * 
     * CLEANUP ORPHANED CHECKPOINTS:
     * ```cpp
     * auto all_ids = mgr.get_all_checkpoint_ids();
     * for (auto id : all_ids) {
     *     // Check if job still exists in controller
     *     if (!controller.has_job(id)) {
     *         // Orphaned checkpoint from old job
     *         mgr.delete_checkpoint(id);
     *         Logger::info("Deleted orphaned checkpoint for job " + 
     *                      std::to_string(id));
     *     }
     * }
     * ```
     * 
     * RESUME ALL PENDING JOBS:
     * ```cpp
     * // After worker restart, resume all checkpointed jobs
     * auto pending_jobs = mgr.get_all_checkpoint_ids();
     * for (auto job_id : pending_jobs) {
     *     Checkpoint ckpt;
     *     if (mgr.load_checkpoint(job_id, ckpt) && ckpt.valid) {
     *         // Inform controller we can resume this job
     *         request_job_assignment(job_id);
     *     }
     * }
     * ```
     * 
     * MONITOR DISK USAGE:
     * ```cpp
     * auto ids = mgr.get_all_checkpoint_ids();
     * size_t count = ids.size();
     * size_t estimated_size = count * 100;  // ~100 bytes per checkpoint
     * std::cout << "Checkpoints: " << count 
     *           << " (~" << estimated_size << " bytes)\n";
     * ```
     * 
     * PERFORMANCE:
     * - O(n) where n = number of files in directory
     * - Requires directory scan (readdir)
     * - Typical: 1-10ms for directories with < 1000 files
     * - Slow for very large directories (10,000+ files)
     * 
     * OPTIMIZATION FOR LARGE DIRECTORIES:
     * If expecting many checkpoints, use database instead of files:
     * ```cpp
     * // SQLite for checkpoint metadata
     * CREATE TABLE checkpoints (
     *     job_id INTEGER PRIMARY KEY,
     *     data BLOB,
     *     created_at TIMESTAMP
     * );
     * 
     * std::vector<uint64_t> get_all_checkpoint_ids() {
     *     return execute_query("SELECT job_id FROM checkpoints");
     * }
     * ```
     * 
     * THREAD SAFETY:
     * Safe to call concurrently with save/load/delete.
     * Returns snapshot of checkpoint IDs at call time.
     * IDs may become stale immediately after return.
     */
    std::vector<uint64_t> get_all_checkpoint_ids() const;
};

} // namespace backtesting

#endif // CHECKPOINT_MANAGER_H

//==============================================================================
// SECTION 4: Usage Examples and Patterns
//==============================================================================

/**
 * PATTERN 1: Basic Checkpoint Save/Load
 * 
 * ```cpp
 * #include "checkpoint/checkpoint_manager.h"
 * 
 * void execute_job_with_checkpointing(uint64_t job_id) {
 *     CheckpointManager mgr("./checkpoints");
 *     
 *     // Try to resume from checkpoint
 *     Checkpoint ckpt;
 *     size_t start_index = 0;
 *     PortfolioState portfolio(10000.0);  // Default initial state
 *     
 *     if (mgr.load_checkpoint(job_id, ckpt) && ckpt.valid) {
 *         // Resume from checkpoint
 *         start_index = ckpt.symbols_processed;
 *         portfolio.cash = ckpt.current_cash;
 *         portfolio.shares_held = ckpt.current_shares;
 *         portfolio.portfolio_value = ckpt.portfolio_value;
 *         
 *         Logger::info("Resuming job " + std::to_string(job_id) + 
 *                      " from symbol " + std::to_string(start_index));
 *     } else {
 *         Logger::info("Starting job " + std::to_string(job_id) + " from beginning");
 *     }
 *     
 *     // Process symbols
 *     for (size_t i = start_index; i < total_symbols; ++i) {
 *         backtest_symbol(symbols[i], portfolio);
 *         
 *         // Save checkpoint every 1000 symbols
 *         if ((i + 1) % 1000 == 0) {
 *             Checkpoint new_ckpt;
 *             new_ckpt.job_id = job_id;
 *             new_ckpt.symbol = symbols[i];
 *             new_ckpt.symbols_processed = i + 1;
 *             new_ckpt.last_processed_index = get_last_bar_index();
 *             new_ckpt.current_cash = portfolio.cash;
 *             new_ckpt.current_shares = portfolio.shares_held;
 *             new_ckpt.portfolio_value = portfolio.portfolio_value;
 *             new_ckpt.last_date_processed = get_current_date();
 *             new_ckpt.valid = true;
 *             
 *             mgr.save_checkpoint(new_ckpt);
 *         }
 *     }
 *     
 *     // Job complete, delete checkpoint
 *     mgr.delete_checkpoint(job_id);
 *     Logger::info("Job " + std::to_string(job_id) + " completed");
 * }
 * ```
 */

/**
 * PATTERN 2: Time-Based Checkpointing
 * 
 * ```cpp
 * void execute_with_time_checkpoints(uint64_t job_id) {
 *     CheckpointManager mgr("./checkpoints");
 *     auto last_checkpoint_time = std::chrono::steady_clock::now();
 *     const auto checkpoint_interval = std::chrono::seconds(60);
 *     
 *     // ... load existing checkpoint if any ...
 *     
 *     for (size_t i = start_index; i < total_symbols; ++i) {
 *         backtest_symbol(symbols[i], portfolio);
 *         
 *         // Save checkpoint every 60 seconds
 *         auto now = std::chrono::steady_clock::now();
 *         auto elapsed = now - last_checkpoint_time;
 *         
 *         if (elapsed >= checkpoint_interval) {
 *             Checkpoint ckpt = create_checkpoint(job_id, i, portfolio);
 *             mgr.save_checkpoint(ckpt);
 *             last_checkpoint_time = now;
 *             
 *             Logger::info("Time-based checkpoint saved");
 *         }
 *     }
 *     
 *     mgr.delete_checkpoint(job_id);
 * }
 * ```
 */

/**
 * PATTERN 3: Graceful Shutdown with Checkpoint
 * 
 * ```cpp
 * class Worker {
 *     std::atomic<bool> shutdown_requested_{false};
 *     CheckpointManager checkpoint_mgr_;
 *     
 * public:
 *     void execute_job(uint64_t job_id) {
 *         // ... load checkpoint, start processing ...
 *         
 *         for (size_t i = start_index; i < total_symbols; ++i) {
 *             // Check for shutdown request
 *             if (shutdown_requested_) {
 *                 // Save checkpoint before exiting
 *                 Checkpoint ckpt = create_checkpoint(job_id, i, portfolio);
 *                 checkpoint_mgr_.save_checkpoint(ckpt);
 *                 
 *                 Logger::info("Shutdown requested, saved checkpoint at symbol " +
 *                             std::to_string(i));
 *                 return;  // Exit gracefully
 *             }
 *             
 *             backtest_symbol(symbols[i], portfolio);
 *             
 *             // Regular checkpointing
 *             if ((i + 1) % 1000 == 0) {
 *                 Checkpoint ckpt = create_checkpoint(job_id, i + 1, portfolio);
 *                 checkpoint_mgr_.save_checkpoint(ckpt);
 *             }
 *         }
 *         
 *         // Completed, cleanup
 *         checkpoint_mgr_.delete_checkpoint(job_id);
 *     }
 *     
 *     void stop() {
 *         shutdown_requested_ = true;
 *         // Worker will save checkpoint and exit cleanly
 *     }
 * };
 * ```
 */

/**
 * PATTERN 4: Checkpoint Cleanup and Maintenance
 * 
 * ```cpp
 * class CheckpointCleaner {
 *     CheckpointManager& mgr_;
 *     std::set<uint64_t> active_jobs_;
 *     
 * public:
 *     void cleanup_orphaned_checkpoints() {
 *         auto all_ckpts = mgr_.get_all_checkpoint_ids();
 *         int deleted = 0;
 *         
 *         for (auto job_id : all_ckpts) {
 *             if (!active_jobs_.count(job_id)) {
 *                 // Job not active, checkpoint is orphaned
 *                 if (mgr_.delete_checkpoint(job_id)) {
 *                     deleted++;
 *                 }
 *             }
 *         }
 *         
 *         Logger::info("Cleaned up " + std::to_string(deleted) + 
 *                      " orphaned checkpoints");
 *     }
 *     
 *     void cleanup_old_checkpoints(int max_age_hours) {
 *         auto all_ckpts = mgr_.get_all_checkpoint_ids();
 *         
 *         for (auto job_id : all_ckpts) {
 *             Checkpoint ckpt;
 *             if (!mgr_.load_checkpoint(job_id, ckpt)) continue;
 *             
 *             // Check age based on last_date_processed
 *             auto ckpt_date = parse_date(ckpt.last_date_processed);
 *             auto age = get_file_age(job_id);  // From filesystem mtime
 *             
 *             if (age > std::chrono::hours(max_age_hours)) {
 *                 mgr_.delete_checkpoint(job_id);
 *                 Logger::info("Deleted stale checkpoint for job " +
 *                             std::to_string(job_id));
 *             }
 *         }
 *     }
 * };
 * 
 * // Run cleanup periodically
 * void maintenance_loop() {
 *     CheckpointCleaner cleaner(mgr, active_jobs);
 *     
 *     while (running) {
 *         std::this_thread::sleep_for(std::chrono::hours(1));
 *         cleaner.cleanup_orphaned_checkpoints();
 *         cleaner.cleanup_old_checkpoints(24);  // Delete >24h old
 *     }
 * }
 * ```
 */

//==============================================================================
// SECTION 5: Checkpoint File Format
//==============================================================================

/**
 * BINARY FILE FORMAT SPECIFICATION
 * 
 * DESIGN GOALS:
 * - Compact (minimize disk space)
 * - Fast to read/write (binary, not text)
 * - Cross-platform (explicit sizes, endianness)
 * - Extensible (can add fields without breaking old checkpoints)
 * 
 * CURRENT FORMAT (Version 1):
 * 
 * Offset | Size | Type     | Field
 * -------|------|----------|---------------------------
 * 0      | 8    | uint64_t | job_id
 * 8      | 8    | uint64_t | symbol_length
 * 16     | N    | char[]   | symbol (N = symbol_length)
 * 16+N   | 8    | uint64_t | symbols_processed
 * 24+N   | 8    | uint64_t | last_processed_index
 * 32+N   | 8    | double   | current_cash
 * 40+N   | 4    | int32_t  | current_shares
 * 44+N   | 8    | double   | portfolio_value
 * 52+N   | 8    | uint64_t | date_length
 * 60+N   | M    | char[]   | last_date_processed (M = date_length)
 * 60+N+M | 1    | uint8_t  | valid (0 or 1)
 * 
 * TOTAL SIZE:
 * 61 + symbol_length + date_length bytes
 * Typical: 61 + 6 + 10 = 77 bytes
 * 
 * EXAMPLE HEX DUMP:
 * ```
 * 00000000: 2a 00 00 00 00 00 00 00  job_id = 42
 * 00000008: 04 00 00 00 00 00 00 00  symbol_length = 4
 * 00000010: 41 41 50 4c              symbol = "AAPL"
 * 00000014: dc 05 00 00 00 00 00 00  symbols_processed = 1500
 * 0000001c: e7 03 00 00 00 00 00 00  last_processed_index = 999
 * 00000024: 00 00 00 00 00 88 b3 40  current_cash = 5000.0
 * 0000002c: 64 00 00 00              current_shares = 100
 * 00000030: 00 00 00 00 00 46 cd 40  portfolio_value = 15000.0
 * 00000038: 0a 00 00 00 00 00 00 00  date_length = 10
 * 00000040: 32 30 32 33 2d 30 36 2d  last_date = "2023-06-15"
 *           31 35
 * 0000004a: 01                       valid = 1 (true)
 * ```
 * 
 * VERSIONING FOR FUTURE EXTENSIBILITY:
 * 
 * Current format has no version field. To add new fields:
 * 
 * VERSION 1 (current):
 * ```cpp
 * [fields as above]
 * ```
 * 
 * VERSION 2 (future):
 * ```cpp
 * [4 bytes: magic = 0xCKPT]  // New magic number
 * [4 bytes: version = 2]     // Version field
 * [existing fields]
 * [8 bytes: peak_portfolio_value]  // New field
 * [4 bytes: num_trades]             // New field
 * ```
 * 
 * Reader checks magic and version, uses appropriate parser.
 * 
 * ENDIANNESS:
 * Current implementation likely uses native endianness (little-endian on x86).
 * For true cross-platform, should explicitly specify:
 * ```cpp
 * void write_uint64_le(std::ostream& out, uint64_t value) {
 *     uint8_t bytes[8];
 *     bytes[0] = value & 0xFF;
 *     bytes[1] = (value >> 8) & 0xFF;
 *     bytes[2] = (value >> 16) & 0xFF;
 *     bytes[3] = (value >> 24) & 0xFF;
 *     bytes[4] = (value >> 32) & 0xFF;
 *     bytes[5] = (value >> 40) & 0xFF;
 *     bytes[6] = (value >> 48) & 0xFF;
 *     bytes[7] = (value >> 56) & 0xFF;
 *     out.write(reinterpret_cast<char*>(bytes), 8);
 * }
 * ```
 */

//==============================================================================
// SECTION 6: Recovery Scenarios
//==============================================================================

/**
 * SCENARIO 1: Worker Crash Mid-Job
 * 
 * Job: Backtest 5000 symbols on Worker 1
 * 
 * Timeline:
 *   00:00 - Job 42 assigned to Worker 1
 *   00:10 - Processed 1000 symbols, checkpoint saved
 *   00:20 - Processed 2000 symbols, checkpoint saved
 *   00:30 - Processed 3000 symbols, checkpoint saved
 *   00:35 - Processing symbol 3456...
 *   00:36 - **Worker 1 crashes** (kernel panic)
 *   
 *   00:42 - Controller detects Worker 1 death (heartbeat timeout)
 *   00:43 - Controller assigns Job 42 to Worker 2
 *   00:44 - Worker 2 checks for checkpoint
 *   00:45 - Worker 2 loads checkpoint (symbols_processed=3000)
 *   00:46 - Worker 2 resumes from symbol 3000
 *   00:47 - Re-processes symbols 3000-3456 (duplicated work)
 *   00:50 - Processes symbols 3457-5000 (new work)
 *   01:00 - Job complete!
 * 
 * OUTCOME:
 * - Total time: 60 minutes (including crash and recovery)
 * - Without checkpointing: Would need 100 minutes (full restart × 2)
 * - Savings: 40 minutes (67% faster recovery)
 */

/**
 * SCENARIO 2: Planned Maintenance
 * 
 * Job: Long-running backtest, 10 hours estimated
 * 
 * Timeline:
 *   Hour 0 - Job starts on Worker 1
 *   Hour 1 - Checkpoint saved (10% complete)
 *   Hour 2 - Checkpoint saved (20% complete)
 *   Hour 3 - Checkpoint saved (30% complete)
 *   Hour 4 - **Operator sends SIGTERM** (maintenance window)
 *   Hour 4 - Worker 1 receives signal, saves checkpoint (35% complete)
 *   Hour 4 - Worker 1 shuts down gracefully
 *   Hour 4 - System maintenance (kernel upgrade, hardware swap)
 *   Hour 5 - Worker 1 restarts with new kernel
 *   Hour 5 - Loads checkpoint (35% complete)
 *   Hour 5 - Resumes job
 *   Hour 11 - Job completes (total 11 hours including maintenance)
 * 
 * OUTCOME:
 * - Maintenance window: 1 hour
 * - Lost progress: 0 hours (checkpoint saved on shutdown)
 * - Total time: 11 hours (only 1 hour overhead for maintenance)
 */

/**
 * SCENARIO 3: Spot Instance Termination (Cloud)
 * 
 * AWS/GCP can terminate spot instances with 2-minute warning.
 * 
 * Timeline:
 *   00:00 - Job processing on spot instance
 *   00:30 - Checkpoint saved
 *   01:00 - Checkpoint saved
 *   01:15 - **Spot termination warning received**
 *   01:16 - Worker receives shutdown signal
 *   01:16 - Worker saves checkpoint immediately
 *   01:17 - Worker shuts down gracefully
 *   01:17 - Spot instance terminated by cloud provider
 *   
 *   01:18 - Auto-scaling launches replacement spot instance
 *   01:19 - New worker starts, connects to controller
 *   01:20 - Controller assigns same job to new worker
 *   01:21 - New worker loads checkpoint
 *   01:22 - Resumes from checkpoint (minimal lost work)
 * 
 * OUTCOME:
 * - Spot interruption handled gracefully
 * - ~1-2 minutes of work lost (not critical)
 * - Job completes successfully despite instance loss
 * - Cost savings: 60-80% cheaper compute
 */

//==============================================================================
// SECTION 7: Performance Optimization
//==============================================================================

/**
 * OPTIMIZATION 1: Checkpoint Only When Necessary
 * 
 * PROBLEM: Checkpointing too frequently wastes I/O
 * 
 * SOLUTION: Adaptive checkpointing
 * ```cpp
 * bool should_checkpoint(size_t symbols_done, 
 *                       std::chrono::seconds elapsed) {
 *     // Checkpoint if:
 *     // - Processed 1000 symbols since last checkpoint
 *     if (symbols_done % 1000 == 0) return true;
 *     
 *     // - OR 60 seconds elapsed since last checkpoint
 *     if (elapsed >= std::chrono::seconds(60)) return true;
 *     
 *     // - OR significant portfolio change (>10%)
 *     double change = abs(current_value - last_checkpoint_value) / 
 *                     last_checkpoint_value;
 *     if (change > 0.10) return true;
 *     
 *     return false;
 * }
 * ```
 */

/**
 * OPTIMIZATION 2: Async Checkpointing
 * 
 * PROBLEM: Synchronous checkpoint blocks job execution for 1-10ms
 * 
 * SOLUTION: Save checkpoint in background thread
 * ```cpp
 * class AsyncCheckpointManager {
 *     std::queue<Checkpoint> pending_checkpoints_;
 *     std::thread writer_thread_;
 *     std::mutex queue_mutex_;
 *     
 *     void save_checkpoint_async(const Checkpoint& ckpt) {
 *         {
 *             std::lock_guard<std::mutex> lock(queue_mutex_);
 *             pending_checkpoints_.push(ckpt);
 *         }
 *         // Returns immediately, actual write happens in background
 *     }
 *     
 *     void writer_loop() {
 *         while (running_) {
 *             Checkpoint ckpt;
 *             {
 *                 std::lock_guard<std::mutex> lock(queue_mutex_);
 *                 if (pending_checkpoints_.empty()) continue;
 *                 ckpt = pending_checkpoints_.front();
 *                 pending_checkpoints_.pop();
 *             }
 *             
 *             mgr_.save_checkpoint(ckpt);  // Blocking I/O in background
 *         }
 *     }
 * };
 * ```
 * 
 * TRADEOFF: If crash before async write completes, checkpoint lost.
 */

/**
 * OPTIMIZATION 3: Checkpoint Compression
 * 
 * PROBLEM: Large checkpoints (if storing trade history)
 * 
 * SOLUTION: Compress before writing
 * ```cpp
 * #include <zlib.h>
 * 
 * bool save_compressed_checkpoint(const Checkpoint& ckpt) {
 *     std::vector<uint8_t> raw_data = serialize(ckpt);
 *     
 *     // Compress with zlib
 *     uLongf compressed_size = compressBound(raw_data.size());
 *     std::vector<uint8_t> compressed(compressed_size);
 *     
 *     compress(compressed.data(), &compressed_size,
 *             raw_data.data(), raw_data.size());
 *     
 *     // Write compressed data
 *     write_to_file(compressed);
 * }
 * ```
 * 
 * BENEFIT: 50-90% size reduction for text-heavy data
 * COST: CPU overhead (1-2ms compression time)
 */

//==============================================================================
// SECTION 8: Common Pitfalls and Solutions
//==============================================================================

/**
 * PITFALL 1: Forgetting to set valid flag
 * 
 * PROBLEM:
 * ```cpp
 * Checkpoint ckpt;
 * ckpt.job_id = 42;
 * // ... populate other fields ...
 * // Forgot: ckpt.valid = true;
 * mgr.save_checkpoint(ckpt);
 * ```
 * 
 * CONSEQUENCE:
 * Checkpoint saved with valid=false.
 * On resume, checkpoint loaded but not used (valid check fails).
 * Job starts from beginning despite checkpoint existing.
 * 
 * SOLUTION:
 * Always set valid=true before saving:
 * ```cpp
 * ckpt.valid = true;  // Mark as usable
 * mgr.save_checkpoint(ckpt);
 * ```
 */

/**
 * PITFALL 2: Not deleting checkpoints after completion
 * 
 * PROBLEM:
 * ```cpp
 * complete_job(job_id);
 * send_results();
 * // Forgot to delete checkpoint!
 * ```
 * 
 * CONSEQUENCE:
 * Checkpoint files accumulate, wasting disk space.
 * After 10,000 jobs: ~1MB wasted.
 * 
 * SOLUTION:
 * Always cleanup on completion:
 * ```cpp
 * complete_job(job_id);
 * send_results();
 * mgr.delete_checkpoint(job_id);  // Cleanup
 * ```
 * 
 * Use RAII for automatic cleanup:
 * ```cpp
 * class CheckpointGuard {
 *     CheckpointManager& mgr_;
 *     uint64_t job_id_;
 * public:
 *     CheckpointGuard(CheckpointManager& mgr, uint64_t id) 
 *         : mgr_(mgr), job_id_(id) {}
 *     ~CheckpointGuard() { mgr_.delete_checkpoint(job_id_); }
 * };
 * 
 * void execute_job(uint64_t job_id) {
 *     CheckpointGuard guard(mgr, job_id);  // Auto-cleanup
 *     // ... process job ...
 * }  // Checkpoint deleted on scope exit
 * ```
 */

/**
 * PITFALL 3: Checkpoint directory on NFS/network storage
 * 
 * PROBLEM:
 * ```cpp
 * CheckpointManager mgr("/mnt/nfs/checkpoints");  // Network storage
 * ```
 * 
 * CONSEQUENCE:
 * - Slow writes (50-200ms vs 1-5ms)
 * - Atomic rename not guaranteed (NFS caching issues)
 * - Potential checkpoint corruption
 * 
 * SOLUTION:
 * Use local disk:
 * ```cpp
 * CheckpointManager mgr("/var/local/checkpoints");  // Local disk
 * ```
 */

/**
 * PITFALL 4: Race condition on checkpoint_exists + load
 * 
 * PROBLEM:
 * ```cpp
 * if (mgr.checkpoint_exists(job_id)) {
 *     // File might be deleted here by another thread!
 *     Checkpoint ckpt;
 *     mgr.load_checkpoint(job_id, ckpt);  // May fail
 * }
 * ```
 * 
 * CONSEQUENCE:
 * TOCTOU (Time Of Check, Time Of Use) race condition.
 * If file deleted between exists and load, load fails unexpectedly.
 * 
 * SOLUTION:
 * Just try to load, handle failure:
 * ```cpp
 * Checkpoint ckpt;
 * if (mgr.load_checkpoint(job_id, ckpt) && ckpt.valid) {
 *     // Atomic check and load
 *     resume_from(ckpt);
 * } else {
 *     start_fresh();
 * }
 * ```
 */

/**
 * PITFALL 5: Not handling save_checkpoint failure
 * 
 * PROBLEM:
 * ```cpp
 * mgr.save_checkpoint(ckpt);  // Ignoring return value
 * // Assume success...
 * ```
 * 
 * CONSEQUENCE:
 * If save fails (disk full, permissions, etc.), no checkpoint created.
 * Job crash will restart from beginning, not from checkpoint point.
 * 
 * SOLUTION:
 * Check return value, but don't abort job:
 * ```cpp
 * if (!mgr.save_checkpoint(ckpt)) {
 *     Logger::warn("Failed to save checkpoint, continuing anyway");
 *     // Checkpoint is optimization, failure is acceptable
 * }
 * ```
 * 
 * Checkpoint failure should NOT fail the job (it's just optimization).
 */

/**
 * PITFALL 6: Checkpointing too frequently
 * 
 * PROBLEM:
 * ```cpp
 * for (size_t i = 0; i < symbols.size(); ++i) {
 *     process_symbol(symbols[i]);
 *     mgr.save_checkpoint(create_checkpoint(i));  // Every symbol!
 * }
 * ```
 * 
 * CONSEQUENCE:
 * For 5000 symbols: 5000 disk writes × 5ms = 25 seconds overhead!
 * Job takes 2× longer due to checkpointing.
 * 
 * SOLUTION:
 * Checkpoint periodically (every N symbols or T seconds):
 * ```cpp
 * if (i % 1000 == 0) {  // Every 1000 symbols
 *     mgr.save_checkpoint(create_checkpoint(i));
 * }
 * ```
 */

//==============================================================================
// SECTION 9: FAQ
//==============================================================================

/**
 * Q1: Should checkpoints be on local disk or shared storage?
 * 
 * A: LOCAL DISK (recommended for this system):
 *    - Fast (1-5ms vs 50-200ms)
 *    - Atomic rename guaranteed
 *    - No network dependencies
 *    - Acceptable loss: Job reassigned, restarts from last checkpoint
 *    
 *    SHARED STORAGE (for critical systems):
 *    - Survives worker failure
 *    - Higher durability
 *    - But slower and more complex
 */

/**
 * Q2: How often should I save checkpoints?
 * 
 * A: Balance overhead vs recovery time:
 *    
 *    RULE OF THUMB:
 *    Checkpoint when losing N minutes of work is acceptable:
 *    - Every 1 minute of work → 1% overhead, max 1min lost
 *    - Every 10 minutes of work → 0.1% overhead, max 10min lost
 *    
 *    FOR THIS SYSTEM:
 *    - 1000 symbols takes ~10 seconds
 *    - Checkpoint takes ~5ms
 *    - Overhead: 5ms / 10s = 0.05%
 *    - Max lost work: 10 seconds
 *    - Good balance!
 */

/**
 * Q3: What if checkpoint file is corrupted?
 * 
 * A: Load will fail or set valid=false. Job starts from beginning.
 *    This is safe (no wrong results), just loses optimization.
 *    
 *    DETECTION:
 *    - Checksum validation (not implemented)
 *    - Field validation (negative values, etc.)
 *    - Size validation (too small or large)
 *    
 *    PREVENTION:
 *    - Atomic writes (temp file + rename)
 *    - Flush before rename
 *    - Journaling filesystem
 */

/**
 * Q4: Can checkpoints be shared between workers?
 * 
 * A: Technically yes, but not designed for it:
 *    
 *    CURRENT DESIGN:
 *    - Each worker has own checkpoint directory
 *    - No coordination between workers
 *    
 *    TO ENABLE SHARING:
 *    - Use shared directory (NFS, S3)
 *    - Add locking to prevent conflicts
 *    - Include worker_id in checkpoint
 *    - Handle concurrent access carefully
 *    
 *    USUALLY UNNECESSARY:
 *    Controller assigns job to one worker at a time.
 *    No need for multiple workers to access same checkpoint.
 */

/**
 * Q5: How do I migrate checkpoints when upgrading code?
 * 
 * A: Add version field to format:
 *    
 *    ```cpp
 *    struct CheckpointV2 : public Checkpoint {
 *        uint32_t version = 2;
 *        // New fields...
 *    };
 *    
 *    bool load_checkpoint(uint64_t job_id, Checkpoint& ckpt) {
 *        // Try to read version
 *        uint32_t version = read_version_from_file();
 *        
 *        if (version == 1) {
 *            return load_v1(job_id, ckpt);
 *        } else if (version == 2) {
 *            return load_v2(job_id, ckpt);
 *        } else {
 *            Logger::error("Unknown checkpoint version");
 *            return false;
 *        }
 *    }
 *    ```
 */

/**
 * Q6: What happens if worker runs out of disk space?
 * 
 * A: save_checkpoint() fails, returns false.
 *    
 *    Job continues WITHOUT checkpointing (degraded mode).
 *    If worker crashes, job restarts from beginning.
 *    
 *    MONITORING:
 *    Check disk space before checkpointing:
 *    ```cpp
 *    auto free_space = std::filesystem::space(checkpoint_dir).available;
 *    if (free_space < 10 * 1024 * 1024) {  // <10MB free
 *        Logger::error("Low disk space, skipping checkpoint");
 *        return false;
 *    }
 *    ```
 */

/**
 * Q7: Can I store trade history in checkpoints?
 * 
 * A: Yes, but increases checkpoint size significantly:
 *    
 *    WITHOUT trade history: ~100 bytes
 *    WITH 1000 trades: ~100KB (1000× larger!)
 *    
 *    IMPACT:
 *    - Slower saves (10-50ms instead of 1-5ms)
 *    - More disk space
 *    - Longer recovery time
 *    
 *    ALTERNATIVE:
 *    Store trades separately, checkpoint only refers to them:
 *    ```cpp
 *    struct Checkpoint {
 *        // ... existing fields ...
 *        std::string trades_file;  // "trades_42_001.bin"
 *    };
 *    ```
 */

/**
 * Q8: How do I test checkpoint/resume logic?
 * 
 * A: Simulate failure scenarios:
 *    
 *    TEST 1: Save and Load
 *    ```cpp
 *    Checkpoint original;
 *    original.job_id = 42;
 *    original.symbols_processed = 1500;
 *    original.valid = true;
 *    
 *    mgr.save_checkpoint(original);
 *    
 *    Checkpoint loaded;
 *    assert(mgr.load_checkpoint(42, loaded));
 *    assert(loaded.valid);
 *    assert(loaded.symbols_processed == 1500);
 *    ```
 *    
 *    TEST 2: Resume After Crash
 *    ```cpp
 *    // First run
 *    process_until_symbol(1500);
 *    save_checkpoint();
 *    // Simulate crash (exit program)
 *    
 *    // Second run
 *    load_checkpoint();
 *    assert(start_index == 1500);  // Resumed correctly
 *    ```
 *    
 *    TEST 3: Corrupted Checkpoint
 *    ```cpp
 *    // Corrupt checkpoint file
 *    corrupt_file("job_42.ckpt");
 *    
 *    Checkpoint ckpt;
 *    bool loaded = mgr.load_checkpoint(42, ckpt);
 *    assert(!ckpt.valid);  // Detected corruption
 *    ```
 */

/**
 * Q9: What's the checkpoint retention policy?
 * 
 * A: Recommended policies:
 *    
 *    COMPLETION-BASED:
 *    - Keep during job execution
 *    - Delete immediately on completion
 *    
 *    TIME-BASED:
 *    - Delete checkpoints >24 hours old
 *    - Assumes jobs don't take >24 hours
 *    
 *    SIZE-BASED:
 *    - Monitor disk usage
 *    - Delete oldest checkpoints when disk >80% full
 *    
 *    HYBRID (best):
 *    ```cpp
 *    // Delete if:
 *    // - Job completed (definitive)
 *    // - OR checkpoint >24h old AND job not active
 *    if (job_completed || (age > 24h && !job_active)) {
 *        delete_checkpoint();
 *    }
 *    ```
 */

/**
 * Q10: Can I use database instead of files?
 * 
 * A: Yes! Files are simple but databases offer benefits:
 *    
 *    SQLITE APPROACH:
 *    ```cpp
 *    CREATE TABLE checkpoints (
 *        job_id INTEGER PRIMARY KEY,
 *        symbol TEXT,
 *        symbols_processed INTEGER,
 *        checkpoint_data BLOB,
 *        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
 *    );
 *    
 *    bool save_checkpoint(const Checkpoint& ckpt) {
 *        auto data = serialize(ckpt);
 *        execute("INSERT OR REPLACE INTO checkpoints VALUES (?, ?, ?)",
 *                ckpt.job_id, serialize(ckpt), ...);
 *    }
 *    ```
 *    
 *    BENEFITS:
 *    - ACID transactions
 *    - Easier queries (find old checkpoints)
 *    - Automatic indexing
 *    - Corruption protection
 *    
 *    TRADEOFFS:
 *    - More complex setup
 *    - Slightly slower (~2× overhead)
 *    - Additional dependency
 */