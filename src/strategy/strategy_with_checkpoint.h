/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: strategy_with_checkpoint.h
    
    Description:
        This header file defines checkpoint-aware strategy classes for the
        distributed backtesting system. It extends the base Strategy class with
        fault tolerance capabilities through periodic checkpoint saving and
        resume functionality, enabling long-running backtests to survive worker
        failures and reassignments without losing computation progress.
        
        Core Features:
        - Checkpoint Integration: Periodic progress saving during backtest
        - Resume Capability: Continue from last checkpoint after interruption
        - Progress Tracking: Monitor computation state and current position
        - Fault Tolerance: Survive worker crashes with minimal work loss
        - Template Pattern: Base class for checkpoint-aware strategies
        
    Fault Tolerance Architecture:
        
        WITHOUT Checkpoints:
        ┌────────────────────────────────────────────┐
        │ Worker executes job (5000 symbols)        │
        │ Progress: 0 → 2500 symbols                │
        │ Worker crashes at symbol 2501              │
        │ Progress lost: Must restart from 0        │
        │ Wasted work: 2500 symbols (50% wasted)    │
        └────────────────────────────────────────────┘
        
        WITH Checkpoints:
        ┌────────────────────────────────────────────┐
        │ Worker executes job (5000 symbols)        │
        │ Checkpoint every 100 symbols:              │
        │   - Index 100: Saved                       │
        │   - Index 200: Saved                       │
        │   - ...                                    │
        │   - Index 2500: Saved                      │
        │ Worker crashes at symbol 2501              │
        │ New worker resumes from checkpoint 2500    │
        │ Wasted work: 1 symbol (0.02% wasted)      │
        └────────────────────────────────────────────┘
        
    Checkpointing Strategy:
        
        PERIODIC SAVING:
        - Interval: Every N symbols (configurable, default 100)
        - Content: Current index, portfolio state, date
        - Storage: CheckpointManager (file or memory)
        - Overhead: ~1-5 ms per checkpoint
        
        RESUME LOGIC:
        1. Job starts → Check for existing checkpoint
        2. If checkpoint exists: Load state, resume from saved index
        3. If no checkpoint: Start from beginning
        4. During execution: Save checkpoints periodically
        5. On completion: Delete checkpoint (no longer needed)
        
        CHECKPOINT CONTENT:
        - current_index: Position in price data array
        - portfolio_state: Cash, positions, transaction history
        - current_date: Last processed date (for validation)
        - metadata: Job ID, timestamp, symbol
        
    Class Hierarchy:
        
        Strategy (base class)
              ↑
              │
        StrategyWithCheckpoint (checkpoint template)
              ↑
              │
        SMAStrategyWithCheckpoint (concrete implementation)
        
        INHERITANCE BENEFITS:
        - Strategy: Pure virtual interface (generate_signals, backtest)
        - StrategyWithCheckpoint: Adds checkpointing infrastructure
        - SMAStrategyWithCheckpoint: Implements SMA algorithm with checkpoints
        
    Checkpoint Intervals:
        
        Choosing checkpoint frequency (checkpoint_interval_):
        
        Too Frequent (e.g., every 1 symbol):
        - Pros: Minimal work lost on failure
        - Cons: High overhead (many I/O operations)
        - Not recommended: Overhead > benefit
        
        Optimal (e.g., every 100 symbols):
        - Pros: Balanced (1-2% overhead, <1% work lost)
        - Cons: None significant
        - Recommended: Default setting
        
        Too Infrequent (e.g., every 1000 symbols):
        - Pros: Low overhead
        - Cons: More work lost on failure
        - Use when: Very fast computations or reliable workers
        
        Formula for choosing:
        interval = max(10, min(1000, sqrt(total_symbols)))
        
    Integration with Distributed System:
        
        Worker Failure Scenario:
        1. Worker 1 executing job (AAPL, 5 years)
        2. Progress: 2500/5000 symbols, checkpoint saved
        3. Worker 1 crashes (hardware failure, network partition)
        4. Controller detects: Heartbeat timeout (6 seconds)
        5. Controller reassigns: Job → Worker 2
        6. Worker 2 receives job
        7. Worker 2 checks: Checkpoint exists
        8. Worker 2 loads: Resume from index 2500
        9. Worker 2 completes: Symbols 2500-5000
        10. Total time: Original + 6s detection + 2500 symbols (vs. 5000)
        
        Performance Impact:
        - Detection latency: 6 seconds (heartbeat timeout)
        - Checkpoint overhead: 1-2% of total execution time
        - Work recovery: 98-99% (vs. 0% without checkpoints)
        - Net benefit: 50-80% time savings on failures
        
    Memory Management:
        
        CHECKPOINT MANAGER:
        - Raw pointer: checkpoint_manager_ (not owned)
        - Lifetime: Managed by caller (typically Worker class)
        - Null check: Always validate before use
        
        WHY NOT UNIQUE_PTR/SHARED_PTR?
        - CheckpointManager shared across multiple strategies
        - Owned by Worker (single owner, multiple users)
        - Raw pointer appropriate for non-owning reference
        
    Thread Safety:
        
        Strategy Execution:
        - Single-threaded: One thread per strategy execution
        - No concurrent calls to same strategy instance
        - Multiple strategies can run concurrently (different instances)
        
        CheckpointManager:
        - Thread-safe: Manages its own synchronization
        - Multiple strategies can checkpoint concurrently
        - No race conditions (each job has unique checkpoint file)
        
    Performance Characteristics:
        
        Operation                    | Time      | Frequency
        -----------------------------|-----------|------------------
        Checkpoint save              | 1-5 ms    | Every 100 symbols
        Checkpoint load              | 1-3 ms    | Once per resume
        Checkpoint delete            | <1 ms     | Once per completion
        Additional overhead          | 1-2%      | Of total execution
        
        Memory:
        - PortfolioState: ~1-10 KB (depends on position count)
        - Checkpoint file: ~10-50 KB on disk
        - Negligible compared to price data (~100 KB per symbol)
        
    Dependencies:
        - strategy/strategy.h: Base strategy interface
        - worker/checkpoint_manager.h: Checkpoint persistence
        - data/csv_loader.h: PriceBar definition
        - common/message.h: JobParams, JobResult
        
    Related Files:
        - strategy_with_checkpoint.cpp: Implementation
        - strategy/strategy.h: Base class definition
        - worker/checkpoint_manager.h: Checkpoint storage
        - worker/worker.cpp: Uses checkpoint strategies

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. HEADER GUARD & PREPROCESSOR
// 2. INCLUDES & DEPENDENCIES
// 3. NAMESPACE DECLARATION
// 4. STRATEGYWITHCHECKPOINT CLASS
//    4.1 Class Overview & Design Philosophy
//    4.2 Protected Members
//        4.2.1 Checkpoint Manager Reference
//        4.2.2 Job Identification
//        4.2.3 Checkpoint Configuration
//    4.3 Public Interface
//        4.3.1 Constructor & Destructor
//        4.3.2 Configuration Methods
//        4.3.3 Backtest with Checkpoint
//    4.4 Protected Helper Methods
//        4.4.1 Save Progress Checkpoint
//        4.4.2 Load Checkpoint State
// 5. SMASTRATEGY WITH CHECKPOINT
//    5.1 Class Overview
//    5.2 Private Members
//    5.3 Constructor & Configuration
//    5.4 Signal Generation Override
// 6. USAGE EXAMPLES
//    6.1 Worker Integration
//    6.2 Long-Running Backtests
//    6.3 Checkpoint Configuration
//    6.4 Resume After Failure
// 7. CHECKPOINT PROTOCOL
// 8. COMMON PITFALLS & SOLUTIONS
// 9. FAQ
// 10. BEST PRACTICES
// 11. PERFORMANCE TUNING
// 12. TESTING STRATEGIES
// 13. TROUBLESHOOTING GUIDE
//
//==============================================================================

//==============================================================================
// SECTION 1: HEADER GUARD & PREPROCESSOR
//==============================================================================

#ifndef STRATEGY_WITH_CHECKPOINT_H
#define STRATEGY_WITH_CHECKPOINT_H

//==============================================================================
// SECTION 2: INCLUDES & DEPENDENCIES
//==============================================================================

// strategy/strategy.h - Base strategy interface
// ==============================================
// Provides: Strategy base class, Signal, PortfolioState
// Required for: Inheritance, method signatures
#include "strategy/strategy.h"

// worker/checkpoint_manager.h - Checkpoint persistence
// =====================================================
// Provides: CheckpointManager class, Checkpoint structure
// Required for: Saving/loading progress during execution
#include "worker/checkpoint_manager.h"

//==============================================================================
// SECTION 3: NAMESPACE DECLARATION
//==============================================================================

namespace backtesting {

//==============================================================================
// SECTION 4: STRATEGYWITHCHECKPOINT CLASS
//==============================================================================

//------------------------------------------------------------------------------
// 4.1 CLASS OVERVIEW & DESIGN PHILOSOPHY
//------------------------------------------------------------------------------
//
// class StrategyWithCheckpoint : public Strategy
//
// OVERVIEW:
// Template base class that adds checkpoint/resume capabilities to any
// backtesting strategy, enabling fault tolerance for long-running computations.
//
// DESIGN PHILOSOPHY:
//
// 1. SEPARATION OF CONCERNS:
//    - Strategy base: Pure algorithm (signal generation, PnL calculation)
//    - StrategyWithCheckpoint: Fault tolerance (save/load progress)
//    - Concrete strategies: Specific algorithms (SMA, RSI, etc.)
//
// 2. MINIMAL OVERHEAD:
//    - Checkpointing is optional (can disable by setting interval high)
//    - Only saves when needed (periodic intervals)
//    - Negligible impact on performance (1-2% overhead)
//
// 3. TRANSPARENT RESUME:
//    - Strategy execution looks identical with/without checkpoints
//    - Resume happens automatically if checkpoint exists
//    - No changes needed to algorithm implementation
//
// 4. TEMPLATE PATTERN:
//    - Base class provides framework (checkpoint infrastructure)
//    - Derived classes implement specifics (signal generation)
//    - Concrete strategies inherit fault tolerance "for free"
//
// WHY INHERITANCE (not composition)?
// - Natural fit: Checkpoint logic wraps strategy execution
// - Code reuse: All strategies get checkpointing automatically
// - Polymorphism: Can treat all checkpoint strategies uniformly
// - Alternative (composition): More complex, less intuitive
//
// CHECKPOINT LIFECYCLE:
//
//   Job Start → Check for checkpoint
//        ↓
//   Checkpoint exists?
//        ├─ Yes → Load checkpoint → Resume from saved index
//        └─ No  → Start from beginning
//        ↓
//   Execute strategy (with periodic saves)
//        ├─ Every N symbols: Save checkpoint
//        └─ Continue processing
//        ↓
//   Job complete → Delete checkpoint
//
// FAULT TOLERANCE BENEFITS:
// - Worker crash: Resume from last checkpoint (minimal work lost)
// - Network partition: Same (worker reassigned)
// - Out of memory: Same (worker restarted)
// - Graceful degradation: System continues despite failures
//
// INHERITANCE HIERARCHY:
//
//   Strategy (abstract)
//       ↑
//       │ (adds checkpointing)
//       │
//   StrategyWithCheckpoint (template)
//       ↑
//       │ (implements SMA algorithm)
//       │
//   SMAStrategyWithCheckpoint (concrete)
//
// POLYMORPHISM USAGE:
//   Strategy* strategy = new SMAStrategyWithCheckpoint(cp_mgr);
//   strategy->backtest(price_data);  // Polymorphic call
//
// VIRTUAL METHOD TABLE:
//   generate_signals() - Pure virtual in Strategy
//                      - Implemented in SMAStrategyWithCheckpoint
//   backtest()         - Concrete in Strategy
//                      - Can be overridden if needed
//
//------------------------------------------------------------------------------

// Enhanced strategy with checkpoint support
class StrategyWithCheckpoint : public Strategy {
protected:
    //==========================================================================
    // SECTION 4.2: PROTECTED MEMBERS
    //==========================================================================
    //
    // Protected members accessible to derived classes but not external code.
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // 4.2.1 CHECKPOINT MANAGER REFERENCE
    //--------------------------------------------------------------------------
    //
    // CheckpointManager* checkpoint_manager_
    //
    // PURPOSE:
    // Reference to checkpoint manager for saving/loading progress.
    //
    // OWNERSHIP:
    // - Not owned by strategy (raw pointer)
    // - Owned by: Worker class (single owner)
    // - Lifetime: Must outlive strategy execution
    //
    // NULL HANDLING:
    // - Can be nullptr: Checkpointing disabled
    // - Always check before use: if (checkpoint_manager_) { ... }
    // - Graceful degradation: If null, execute without checkpoints
    //
    // THREAD SAFETY:
    // - CheckpointManager handles its own thread safety
    // - Multiple strategies can use same manager concurrently
    // - Each job has unique checkpoint (no conflicts)
    //
    // USAGE PATTERN:
    //   if (checkpoint_manager_) {
    //       checkpoint_manager_->save_checkpoint(job_id, checkpoint);
    //   }
    //
    // WHY RAW POINTER (not shared_ptr)?
    // - Manager is shared across strategies (but single owner)
    // - Worker owns manager (clear ownership)
    // - Strategy just uses it (non-owning reference)
    // - Raw pointer is appropriate for non-owning references
    //
    CheckpointManager* checkpoint_manager_;
    
    //--------------------------------------------------------------------------
    // 4.2.2 JOB IDENTIFICATION
    //--------------------------------------------------------------------------
    //
    // uint64_t current_job_id_
    //
    // PURPOSE:
    // Identifies current job for checkpoint file naming.
    //
    // USAGE:
    // - Set by: set_job_id() before backtest
    // - Used in: Checkpoint save/load operations
    // - File naming: checkpoint_<job_id>.dat
    //
    // WHY NEEDED?
    // - CheckpointManager manages multiple jobs
    // - Each job needs unique checkpoint file
    // - job_id provides unique identifier
    //
    // EXAMPLE:
    //   Job 1000: checkpoint_1000.dat
    //   Job 1001: checkpoint_1001.dat
    //   No conflicts: Each job has separate file
    //
    // INITIALIZATION:
    // - Default: 0 (invalid)
    // - Must be set: Before backtest_with_checkpoint()
    // - Validation: Should check > 0 before use
    //
    uint64_t current_job_id_;
    
    //--------------------------------------------------------------------------
    // 4.2.3 CHECKPOINT CONFIGURATION
    //--------------------------------------------------------------------------
    //
    // int checkpoint_interval_
    //
    // PURPOSE:
    // Number of symbols to process between checkpoints.
    //
    // DEFAULT:
    // - 100 symbols (configurable via constructor)
    //
    // TUNING:
    // - Smaller (10): More frequent, higher overhead, less work lost
    // - Larger (1000): Less frequent, lower overhead, more work lost
    // - Optimal: ~100 (1-2% overhead, <1% work lost)
    //
    // CALCULATION:
    //   if (current_index % checkpoint_interval_ == 0) {
    //       save_checkpoint();
    //   }
    //
    // OVERHEAD ANALYSIS:
    // - Checkpoint save: ~1-5 ms
    // - Symbol processing: ~0.1-1 ms
    // - Interval 100: Save every 10-100ms of work
    // - Overhead: 1-5ms / 10-100ms = 1-5%
    //
    // WORK LOSS ANALYSIS:
    // - Worst case: Crash right before next checkpoint
    // - Work lost: checkpoint_interval - 1 symbols
    // - Percentage: (interval - 1) / total_symbols
    // - Example: 99/5000 = 1.98%
    //
    int checkpoint_interval_;
    
public:
    //==========================================================================
    // SECTION 4.3: PUBLIC INTERFACE
    //==========================================================================
    
    //--------------------------------------------------------------------------
    // 4.3.1 CONSTRUCTOR & DESTRUCTOR
    //--------------------------------------------------------------------------
    
    // Constructor: Initialize strategy with checkpoint support
    //
    // PARAMETERS:
    //   - name: Strategy name (e.g., "SMA Crossover")
    //   - cp_mgr: Checkpoint manager (nullable)
    //   - cp_interval: Symbols between checkpoints (default 100)
    //
    // INITIALIZATION:
    //   - Calls Strategy base constructor
    //   - Stores checkpoint manager reference
    //   - Sets job_id to 0 (invalid, must be set later)
    //   - Configures checkpoint interval
    //
    // EXPLICIT:
    //   - Prevents implicit conversions
    //   - Must explicitly construct: StrategyWithCheckpoint s(name, mgr, 100);
    //   - Not allowed: StrategyWithCheckpoint s = name; (prevented)
    //
    // EXAMPLE:
    //   CheckpointManager mgr;
    //   StrategyWithCheckpoint strategy("My Strategy", &mgr, 50);
    //   // Checkpoints every 50 symbols
    //
    explicit StrategyWithCheckpoint(const std::string& name, 
                                   CheckpointManager* cp_mgr = nullptr,
                                   int cp_interval = 100)
        : Strategy(name),                    // Initialize base class
          checkpoint_manager_(cp_mgr),       // Store manager reference
          current_job_id_(0),                // Invalid job ID initially
          checkpoint_interval_(cp_interval)  // Configure interval
    {}
    
    // Destructor: Clean up strategy
    //
    // VIRTUAL:
    //   - Allows polymorphic deletion
    //   - Can delete via Strategy* pointer
    //   - Ensures derived class destructor called
    //
    // DEFAULT:
    //   - No special cleanup needed
    //   - No owned resources (checkpoint_manager_ not owned)
    //   - Compiler-generated implementation sufficient
    //
    // EXCEPTION SAFETY:
    //   - Nothrow: Destructors must not throw
    //
    virtual ~StrategyWithCheckpoint() = default;
    
    //--------------------------------------------------------------------------
    // 4.3.2 CONFIGURATION METHODS
    //--------------------------------------------------------------------------
    
    // Set checkpoint manager
    //
    // PURPOSE:
    //   - Configure checkpoint manager after construction
    //   - Useful if manager not available at construction time
    //
    // PARAMETERS:
    //   - mgr: CheckpointManager pointer (can be nullptr to disable)
    //
    // WHEN TO CALL:
    //   - After construction, before backtest
    //   - To change manager (e.g., switch to different storage)
    //   - To disable checkpointing (pass nullptr)
    //
    // EXAMPLE:
    //   StrategyWithCheckpoint strategy("SMA");  // No manager yet
    //   CheckpointManager mgr;
    //   strategy.set_checkpoint_manager(&mgr);   // Configure now
    //
    void set_checkpoint_manager(CheckpointManager* mgr) {
        checkpoint_manager_ = mgr;
    }
    
    // Set current job ID
    //
    // PURPOSE:
    //   - Associate strategy execution with specific job
    //   - Required for checkpoint file naming
    //
    // PARAMETERS:
    //   - job_id: Unique job identifier
    //
    // WHEN TO CALL:
    //   - Before backtest_with_checkpoint()
    //   - When job assigned to worker
    //
    // VALIDATION:
    //   - Should be > 0 (0 is invalid)
    //   - Caller responsible for uniqueness
    //
    // EXAMPLE:
    //   strategy.set_job_id(1000);  // Job 1000
    //   strategy.backtest_with_checkpoint(price_data);
    //
    void set_job_id(uint64_t job_id) {
        current_job_id_ = job_id;
    }
    
    //--------------------------------------------------------------------------
    // 4.3.3 BACKTEST WITH CHECKPOINT
    //--------------------------------------------------------------------------
    
    // Execute backtest with checkpoint support
    //
    // PURPOSE:
    //   - Run backtest with periodic checkpoint saving
    //   - Resume from checkpoint if exists
    //   - Delete checkpoint on completion
    //
    // PARAMETERS:
    //   - price_data: Historical price bars (OHLCV)
    //
    // RETURNS:
    //   - JobResult: Backtest results (returns, Sharpe, trades, etc.)
    //
    // EXECUTION FLOW:
    //   1. Check for existing checkpoint
    //   2. If exists: Load checkpoint, resume from saved index
    //   3. If not: Initialize portfolio, start from beginning
    //   4. For each symbol:
    //      a. Generate signals
    //      b. Execute trades
    //      c. Update portfolio
    //      d. If checkpoint interval: Save checkpoint
    //   5. Calculate final metrics
    //   6. Delete checkpoint (job complete)
    //   7. Return results
    //
    // CHECKPOINT INTEGRATION:
    //   - Transparent: Algorithm doesn't need to know about checkpoints
    //   - Automatic: Framework handles save/load
    //   - Optional: Works even if checkpoint_manager_ is null
    //
    // ERROR HANDLING:
    //   - Checkpoint load fails: Start from beginning (log warning)
    //   - Checkpoint save fails: Continue execution (log error)
    //   - Computation error: Return error result, delete checkpoint
    //
    // THREAD SAFETY:
    //   - Not thread-safe: Single execution per instance
    //   - Different instances: Can run concurrently
    //
    // EXAMPLE:
    //   auto result = strategy.backtest_with_checkpoint(price_data);
    //   if (result.success) {
    //       std::cout << "Return: " << result.total_return << "\n";
    //   }
    //
    JobResult backtest_with_checkpoint(const std::vector<PriceBar>& price_data);
    
protected:
    //==========================================================================
    // SECTION 4.4: PROTECTED HELPER METHODS
    //==========================================================================
    //
    // Internal methods for checkpoint operations.
    // Protected: Available to derived classes but not external code.
    //
    //--------------------------------------------------------------------------
    
    //--------------------------------------------------------------------------
    // 4.4.1 SAVE PROGRESS CHECKPOINT
    //--------------------------------------------------------------------------
    //
    // bool save_progress_checkpoint(size_t current_index, 
    //                               const PortfolioState& portfolio,
    //                               const std::string& current_date)
    //
    // PURPOSE:
    // Saves current execution state to checkpoint.
    //
    // PARAMETERS:
    //   - current_index: Current position in price_data array
    //   - portfolio: Current portfolio state (cash, positions, etc.)
    //   - current_date: Current date being processed
    //
    // CHECKPOINT CONTENT:
    //   - job_id: Identifies this job
    //   - current_index: Resume point
    //   - portfolio_state: Cash, positions, transaction history
    //   - current_date: For validation on resume
    //   - timestamp: When checkpoint created
    //
    // RETURNS:
    //   - true: Checkpoint saved successfully
    //   - false: Save failed (manager null, I/O error, etc.)
    //
    // ERROR HANDLING:
    //   - If fails: Log error, continue execution
    //   - Non-fatal: Checkpoint is optimization, not requirement
    //
    // THREAD SAFETY:
    //   - CheckpointManager handles synchronization
    //   - Safe to call concurrently (different job IDs)
    //
    // PERFORMANCE:
    //   - Typical: 1-5 ms per save
    //   - Disk I/O: Dominant cost
    //   - Could optimize: Batch writes, async I/O
    //
    // EXAMPLE USAGE (in derived class):
    //   if (i % checkpoint_interval_ == 0) {
    //       save_progress_checkpoint(i, portfolio, current_date);
    //   }
    //
    bool save_progress_checkpoint(size_t current_index, 
                                  const PortfolioState& portfolio,
                                  const std::string& current_date);
    
    //--------------------------------------------------------------------------
    // 4.4.2 LOAD CHECKPOINT STATE
    //--------------------------------------------------------------------------
    //
    // bool load_checkpoint_state(Checkpoint& cp, size_t& resume_index)
    //
    // PURPOSE:
    // Loads checkpoint and determines resume point.
    //
    // PARAMETERS:
    //   - cp: Output - loaded checkpoint data
    //   - resume_index: Output - index to resume from
    //
    // RETURNS:
    //   - true: Checkpoint loaded successfully
    //   - false: No checkpoint or load failed
    //
    // VALIDATION:
    //   - Checkpoint exists for current_job_id?
    //   - Checkpoint not corrupted?
    //   - Resume index within bounds?
    //
    // RESUME INDEX:
    //   - Typically: checkpoint.current_index (exact resume point)
    //   - Could adjust: Resume from earlier index for safety
    //
    // ERROR HANDLING:
    //   - Checkpoint missing: Return false (start from beginning)
    //   - Checkpoint corrupted: Return false (log warning)
    //   - Invalid resume_index: Return false
    //
    // USAGE PATTERN:
    //   Checkpoint cp;
    //   size_t resume_idx;
    //   if (load_checkpoint_state(cp, resume_idx)) {
    //       // Resume from checkpoint
    //       portfolio = cp.portfolio_state;
    //       start_index = resume_idx;
    //   } else {
    //       // Start from beginning
    //       portfolio = InitialPortfolio();
    //       start_index = 0;
    //   }
    //
    bool load_checkpoint_state(Checkpoint& cp, size_t& resume_index);
};

//==============================================================================
// SECTION 5: SMASTRATEGY WITH CHECKPOINT
//==============================================================================

//------------------------------------------------------------------------------
// 5.1 CLASS OVERVIEW
//------------------------------------------------------------------------------
//
// class SMAStrategyWithCheckpoint : public StrategyWithCheckpoint
//
// OVERVIEW:
// Concrete implementation of Simple Moving Average (SMA) crossover strategy
// with built-in checkpoint support for fault tolerance.
//
// SMA CROSSOVER STRATEGY:
// - Calculates short-term moving average (e.g., 50-day)
// - Calculates long-term moving average (e.g., 200-day)
// - Buy signal: Short MA crosses above long MA (bullish)
// - Sell signal: Short MA crosses below long MA (bearish)
//
// MOVING AVERAGE:
// - Simple arithmetic mean of N most recent prices
// - Example (5-day MA):
//   Prices: [100, 102, 101, 103, 104]
//   MA: (100 + 102 + 101 + 103 + 104) / 5 = 102.0
//
// CROSSOVER SIGNALS:
// - Golden Cross: Short MA crosses above long MA (BUY)
//   * Indicates: Uptrend momentum
//   * Action: Enter long position
// - Death Cross: Short MA crosses below long MA (SELL)
//   * Indicates: Downtrend momentum
//   * Action: Exit long position or short
//
// CHECKPOINT INTEGRATION:
// - Inherits: backtest_with_checkpoint() from base
// - Implements: generate_signals() (pure virtual from Strategy)
// - Automatic: Checkpoint saving/loading handled by base class
//
// TYPICAL PARAMETERS:
// - short_window: 50 days (short-term trend)
// - long_window: 200 days (long-term trend)
// - initial_capital: $10,000
//
// PERFORMANCE:
// - Computational complexity: O(n × w) where n = bars, w = window
// - Memory: O(n) for price arrays and moving averages
// - Typical execution: 10-100 ms for 1260 bars (5 years)
//
//------------------------------------------------------------------------------

// SMA strategy with checkpoint support
class SMAStrategyWithCheckpoint : public StrategyWithCheckpoint {
private:
    //==========================================================================
    // SECTION 5.2: PRIVATE MEMBERS
    //==========================================================================
    
    // Short moving average window (days)
    // Typical: 50 days
    // Range: 10-100 days (shorter = more responsive, more signals)
    int short_window_;
    
    // Long moving average window (days)
    // Typical: 200 days
    // Range: 100-300 days (longer = smoother, fewer signals)
    // Constraint: Must be > short_window_
    int long_window_;
    
    //--------------------------------------------------------------------------
    // PRIVATE HELPER METHOD
    //--------------------------------------------------------------------------
    
    // Calculate simple moving average
    //
    // PURPOSE:
    //   - Computes arithmetic mean of last N prices
    //
    // PARAMETERS:
    //   - prices: Price array (typically closing prices)
    //   - window: Number of periods to average
    //
    // RETURNS:
    //   - vector<double>: Moving average at each point
    //   - Size: prices.size() - window + 1
    //
    // ALGORITHM:
    //   For each index i from window to prices.size():
    //     MA[i] = mean(prices[i-window:i])
    //
    // EXAMPLE:
    //   Prices: [100, 102, 104, 103, 105]
    //   Window: 3
    //   Result: [102.0, 103.0, 104.0]
    //           (mean of [100,102,104], [102,104,103], [104,103,105])
    //
    // OPTIMIZATION:
    //   - Sliding window: O(n) instead of O(n×w)
    //   - Current: Recalculates each window O(n×w) (simpler)
    //   - For production: Use rolling sum (more efficient)
    //
    // THREAD SAFETY:
    //   - const method: Read-only, thread-safe
    //   - No shared state
    //
    std::vector<double> calculate_sma(const std::vector<double>& prices, int window) const;
    
public:
    //==========================================================================
    // SECTION 5.3: CONSTRUCTOR & CONFIGURATION
    //==========================================================================
    
    // Constructor: Create SMA strategy with checkpoints
    //
    // PARAMETERS:
    //   - cp_mgr: Checkpoint manager (nullable)
    //
    // DEFAULT PARAMETERS:
    //   - short_window: 50 days (industry standard)
    //   - long_window: 200 days (industry standard)
    //
    // PARAMETER CONFIGURATION:
    //   - Call set_parameters() after construction
    //   - Overrides defaults with job-specific values
    //
    // EXAMPLE:
    //   CheckpointManager mgr;
    //   SMAStrategyWithCheckpoint strategy(&mgr);
    //   // Uses default 50/200 windows initially
    //   
    //   JobParams params;
    //   params.short_window = 30;
    //   params.long_window = 100;
    //   strategy.set_parameters(params);
    //   // Now uses 30/100 windows
    //
    SMAStrategyWithCheckpoint(CheckpointManager* cp_mgr = nullptr)
        : StrategyWithCheckpoint("SMA Crossover", cp_mgr),  // Base class init
          short_window_(50),                                // Default short
          long_window_(200)                                 // Default long
    {}
    
    // Set strategy parameters from job
    //
    // PURPOSE:
    //   - Configure windows from job parameters
    //   - Override default 50/200 if job specifies different values
    //
    // OVERRIDE:
    //   - Extends Strategy::set_parameters()
    //   - Calls base class first (sets common params)
    //   - Then sets SMA-specific params
    //
    // PARAMETERS:
    //   - params: Job parameters (includes short_window, long_window)
    //
    // VALIDATION:
    //   - Should check: short_window < long_window
    //   - Should check: both > 0
    //   - Not implemented: Assumes valid input
    //
    // EXAMPLE:
    //   JobParams params;
    //   params.symbol = "AAPL";
    //   params.short_window = 30;
    //   params.long_window = 100;
    //   params.initial_capital = 10000.0;
    //   
    //   strategy.set_parameters(params);
    //   // Now configured for this job
    //
    void set_parameters(const JobParams& params) override {
        // Call base class implementation
        // Sets: symbol, initial_capital, etc.
        Strategy::set_parameters(params);
        
        // Set SMA-specific parameters
        short_window_ = params.short_window;
        long_window_ = params.long_window;
    }
    
    //==========================================================================
    // SECTION 5.4: SIGNAL GENERATION OVERRIDE
    //==========================================================================
    
    // Generate trading signals using SMA crossover
    //
    // PURPOSE:
    //   - Implements pure virtual method from Strategy base class
    //   - Generates buy/sell signals based on MA crossovers
    //
    // ALGORITHM:
    //   1. Extract closing prices from price_data
    //   2. Calculate short-term moving average
    //   3. Calculate long-term moving average
    //   4. For each time point:
    //      - If short MA crosses above long MA: BUY signal
    //      - If short MA crosses below long MA: SELL signal
    //      - Otherwise: HOLD signal
    //
    // PARAMETERS:
    //   - price_data: Historical price bars
    //
    // RETURNS:
    //   - vector<Signal>: Trading signals (BUY/SELL/HOLD)
    //   - Same length as price_data (one signal per bar)
    //
    // SIGNAL TYPES:
    //   - Signal::BUY: Enter long position
    //   - Signal::SELL: Exit position or short
    //   - Signal::HOLD: No action
    //
    // REQUIRED DATA:
    //   - Minimum: long_window bars (need data for long MA)
    //   - Less data: Returns error result (insufficient data)
    //
    // OVERRIDE:
    //   - Pure virtual in Strategy base class
    //   - Must implement in concrete strategy
    //
    // EXAMPLE OUTPUT:
    //   Signals: [HOLD, HOLD, ..., BUY, HOLD, ..., SELL, HOLD, ...]
    //           ^---------------^    ^--------^    ^---------^
    //           Warmup period     Long position   Exit
    //
    // IMPLEMENTATION:
    //   - Defined in strategy_with_checkpoint.cpp
    //   - See implementation for detailed algorithm
    //
    std::vector<Signal> generate_signals(const std::vector<PriceBar>& price_data) override;
};

} // namespace backtesting

#endif // STRATEGY_WITH_CHECKPOINT_H

//==============================================================================
// END OF HEADER FILE
//==============================================================================
//
// Implementation of methods is in strategy_with_checkpoint.cpp
// See that file for detailed algorithm implementations
//
//==============================================================================

//==============================================================================
// SECTION 6: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Worker Integration
// ==============================
//
// #include "strategy/strategy_with_checkpoint.h"
// #include "worker/checkpoint_manager.h"
//
// class Worker {
//     CheckpointManager checkpoint_manager_;
//     
//     JobResult execute_job(const JobParams& params, uint64_t job_id) {
//         // Create strategy with checkpoint support
//         SMAStrategyWithCheckpoint strategy(&checkpoint_manager_);
//         
//         // Configure strategy
//         strategy.set_parameters(params);
//         strategy.set_job_id(job_id);
//         
//         // Load price data
//         CSVLoader loader("./data");
//         auto data = loader.load(params.symbol);
//         auto bars = data->get_range(params.start_date, params.end_date);
//         
//         // Execute with checkpoint support
//         auto result = strategy.backtest_with_checkpoint(bars);
//         
//         return result;
//     }
// };
//
//
// EXAMPLE 2: Long-Running Backtests
// ==================================
//
// void run_large_backtest() {
//     CheckpointManager mgr;
//     SMAStrategyWithCheckpoint strategy(&mgr);
//     
//     // Configure for large dataset
//     JobParams params;
//     params.symbol = "SPY";  // S&P 500 ETF
//     params.start_date = "2000-01-01";
//     params.end_date = "2024-12-31";  // 25 years!
//     params.short_window = 50;
//     params.long_window = 200;
//     
//     strategy.set_parameters(params);
//     strategy.set_job_id(1000);
//     
//     // Load data (~6300 bars for 25 years)
//     auto bars = load_price_data(params);
//     
//     Logger::info("Starting backtest of " + std::to_string(bars.size()) + " bars");
//     Logger::info("Checkpoint interval: 100 symbols");
//     Logger::info("Expected checkpoints: " + std::to_string(bars.size() / 100));
//     
//     // Execute (with automatic checkpoints every 100 symbols)
//     auto result = strategy.backtest_with_checkpoint(bars);
//     
//     Logger::info("Backtest complete: " + std::to_string(result.num_trades) + " trades");
// }
//
//
// EXAMPLE 3: Configuring Checkpoint Interval
// ===========================================
//
// void configure_checkpoint_strategy(size_t total_symbols) {
//     CheckpointManager mgr;
//     
//     // Calculate optimal checkpoint interval
//     // Rule of thumb: sqrt(total_symbols), clamped to [10, 1000]
//     int interval = std::clamp(static_cast<int>(std::sqrt(total_symbols)),
//                               10, 1000);
//     
//     // Create strategy with custom interval
//     StrategyWithCheckpoint strategy("My Strategy", &mgr, interval);
//     
//     Logger::info("Configured checkpoint interval: " + std::to_string(interval));
//     
//     // For 1260 symbols: interval = sqrt(1260) ≈ 35
//     // For 5000 symbols: interval = sqrt(5000) ≈ 70
//     // For 100 symbols: interval = 10 (min)
// }
//
//
// EXAMPLE 4: Resume After Failure Simulation
// ===========================================
//
// void simulate_failure_and_resume() {
//     CheckpointManager mgr;
//     SMAStrategyWithCheckpoint strategy(&mgr);
//     
//     strategy.set_job_id(2000);
//     strategy.set_parameters(params);
//     
//     // First execution (simulated crash at 50%)
//     {
//         auto bars = load_data();
//         
//         Logger::info("Starting initial execution...");
//         // Simulate crash: Only process half the data
//         std::vector<PriceBar> partial(bars.begin(), bars.begin() + bars.size()/2);
//         
//         try {
//             strategy.backtest_with_checkpoint(partial);
//         } catch (...) {
//             Logger::info("Simulated crash!");
//         }
//         // Checkpoint saved at index 100, 200, etc.
//     }
//     
//     // Second execution (resume from checkpoint)
//     {
//         auto bars = load_data();
//         
//         Logger::info("Resuming from checkpoint...");
//         auto result = strategy.backtest_with_checkpoint(bars);
//         
//         Logger::info("Resumed successfully!");
//         Logger::info("Total return: " + std::to_string(result.total_return));
//         // Should show results from entire dataset
//         // Not just second half (portfolio state restored)
//     }
// }
//
//==============================================================================

//==============================================================================
// SECTION 7: CHECKPOINT PROTOCOL
//==============================================================================
//
// CHECKPOINT STRUCTURE:
// =====================
//
// struct Checkpoint {
//     uint64_t job_id;                    // Job identifier
//     size_t current_index;               // Resume point in data array
//     std::string current_date;           // Last processed date
//     PortfolioState portfolio_state;     // Current portfolio
//     std::chrono::system_clock::time_point timestamp;  // When saved
// };
//
// struct PortfolioState {
//     double cash;                        // Available cash
//     std::map<std::string, Position> positions;  // Open positions
//     std::vector<Trade> trades;          // Transaction history
//     double total_value;                 // Current portfolio value
// };
//
// CHECKPOINT FILENAME:
// Pattern: checkpoint_<job_id>.dat
// Example: checkpoint_1000.dat
// Location: Configured in CheckpointManager
//
// CHECKPOINT LIFECYCLE:
//
// 1. JOB START:
//    - Check: Does checkpoint_<job_id>.dat exist?
//    - If yes: Load checkpoint → resume_index
//    - If no: resume_index = 0 (start from beginning)
//
// 2. EXECUTION:
//    - Every checkpoint_interval_ symbols:
//      * Create checkpoint
//      * Save to file
//      * Continue execution
//
// 3. JOB COMPLETION:
//    - Delete checkpoint file
//    - No longer needed (job finished)
//
// 4. JOB FAILURE:
//    - Checkpoint remains on disk
//    - Next execution: Resumes from checkpoint
//
// CHECKPOINT VALIDATION:
// ======================
//
// When loading checkpoint, verify:
// - job_id matches current job
// - current_index within bounds (0 to data.size())
// - current_date is valid (ISO 8601 format)
// - portfolio_state is consistent (cash >= 0, etc.)
// - timestamp recent (not too old - could indicate stale data)
//
// If validation fails: Reject checkpoint, start from beginning
//
// CHECKPOINT CLEANUP:
// ===================
//
// Checkpoints should be deleted when:
// - Job completes successfully
// - Job fails permanently (not reassigned)
// - Checkpoint too old (e.g., > 24 hours)
//
// Orphaned checkpoints:
// - Can accumulate if cleanup fails
// - Periodic cleanup: Delete old checkpoints
// - Pattern: checkpoint_*.dat older than threshold
//
//==============================================================================

//==============================================================================
// SECTION 8: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: Forgetting to Set Job ID
// ====================================
// PROBLEM:
//    SMAStrategyWithCheckpoint strategy(&mgr);
//    strategy.backtest_with_checkpoint(data);  // job_id = 0 (invalid)
//    // Checkpoint saved as checkpoint_0.dat (conflicts!)
//
// SOLUTION:
//    strategy.set_job_id(job_id);  // Must set before backtest
//    strategy.backtest_with_checkpoint(data);
//
//
// PITFALL 2: Checkpoint Manager Null
// ===================================
// PROBLEM:
//    SMAStrategyWithCheckpoint strategy(nullptr);
//    // Checkpointing disabled (but no error)
//    // Worker crash: Must restart from beginning
//
// SOLUTION:
//    CheckpointManager mgr;
//    SMAStrategyWithCheckpoint strategy(&mgr);  // Enable checkpoints
//
//
// PITFALL 3: Checkpoint Interval Too Small
// =========================================
// PROBLEM:
//    StrategyWithCheckpoint strategy("SMA", &mgr, 1);  // Every symbol!
//    // High overhead: 1-5ms per symbol
//    // Backtest 10x slower
//
// SOLUTION:
//    StrategyWithCheckpoint strategy("SMA", &mgr, 100);  // Reasonable
//    // Overhead: ~1-2% of total time
//
//
// PITFALL 4: Portfolio State Not Serializable
// ============================================
// PROBLEM:
//    // Custom portfolio state with complex objects
//    // Can't serialize to checkpoint
//
// SOLUTION:
//    // Keep portfolio state simple (POD types)
//    // Or implement custom serialization
//    // struct PortfolioState {
//    //     double cash;
//    //     std::vector<Position> positions;
//    //     // All serializable types
//    // };
//
//
// PITFALL 5: Not Deleting Checkpoints on Completion
// ==================================================
// PROBLEM:
//    // Checkpoint files accumulate
//    // Disk space exhausted
//
// SOLUTION:
//    // backtest_with_checkpoint() should delete on completion
//    // Or: Periodic cleanup of old checkpoints
//    mgr.cleanup_old_checkpoints(24 * 3600);  // Older than 24 hours
//
//
// PITFALL 6: Resume Index Out of Bounds
// ======================================
// PROBLEM:
//    // Checkpoint: resume_index = 1500
//    // Data: Only 1000 bars (data changed since checkpoint)
//    // Resume beyond end of array (crash)
//
// SOLUTION:
//    if (resume_idx >= price_data.size()) {
//        Logger::warning("Invalid checkpoint: resume beyond data size");
//        resume_idx = 0;  // Start from beginning
//    }
//
//
// PITFALL 7: Checkpoint/Resume with Different Parameters
// =======================================================
// PROBLEM:
//    // First run: short_window = 50
//    // Checkpoint saved with 50-day MA state
//    // Second run: short_window = 30 (different!)
//    // Resume with wrong MA (incorrect results)
//
// SOLUTION:
//    // Store parameters in checkpoint
//    // Validate on resume: params match checkpoint params
//    // Or: Include params in checkpoint filename
//    // checkpoint_<job_id>_<param_hash>.dat
//
//==============================================================================

//==============================================================================
// SECTION 9: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: When should I use checkpoints?
// ==================================
// A: Use checkpoints when:
//    - Long-running backtests (> 1 minute)
//    - Many symbols to process (> 100)
//    - Worker failures expected
//    - Cost of restart is high
//    
//    Don't use when:
//    - Very fast backtests (< 1 second)
//    - Highly reliable environment
//    - Checkpoint overhead > restart cost
//
// Q2: How often should I checkpoint?
// ===================================
// A: Rule of thumb: Every 1-2% of total work
//    - For 1000 symbols: Every 10-20 symbols
//    - For 5000 symbols: Every 50-100 symbols
//    - Optimal: checkpoint_interval = sqrt(total_symbols)
//    
//    Balance: Overhead vs. work lost on failure
//
// Q3: What happens if checkpoint file is corrupted?
// ==================================================
// A: Graceful degradation:
//    - load_checkpoint_state() returns false
//    - Strategy starts from beginning
//    - Log warning about corruption
//    - Could implement: Multiple checkpoint slots (checkpoint_old, checkpoint_new)
//
// Q4: Can multiple workers use same checkpoint?
// ==============================================
// A: No:
//    - Each job has unique checkpoint (job_id based)
//    - Job assigned to exactly one worker at a time
//    - If reassigned: New worker uses checkpoint from old worker
//
// Q5: Where are checkpoints stored?
// ==================================
// A: Configured in CheckpointManager:
//    - Typically: Local filesystem (/tmp/checkpoints/)
//    - Could be: Shared NFS (for worker migration)
//    - Could be: Redis (for distributed storage)
//
// Q6: What if backtest parameters change during resume?
// ======================================================
// A: Undefined behavior in current implementation:
//    - Checkpoint saved with one set of parameters
//    - Resume with different parameters
//    - Results will be incorrect
//    
//    Solution: Validate parameters match or include in checkpoint
//
// Q7: How much disk space do checkpoints use?
// ============================================
// A: Per checkpoint:
//    - PortfolioState: ~1-10 KB (depends on positions)
//    - Metadata: ~100 bytes
//    - Total: ~10-50 KB typical
//    
//    For 10 concurrent jobs: ~100-500 KB (negligible)
//
// Q8: Can I checkpoint to memory instead of disk?
// ================================================
// A: Yes:
//    - CheckpointManager supports different backends
//    - In-memory: Faster but not durable (lost on crash)
//    - Disk: Slower but survives crashes
//    - Trade-off: Speed vs. durability
//
// Q9: Does checkpointing slow down execution?
// ============================================
// A: Minimal impact:
//    - Overhead: 1-2% of total execution time
//    - Checkpoint save: 1-5 ms
//    - Frequency: Every 100 symbols (~100-1000 ms of work)
//    - Net: Negligible for most backtests
//
// Q10: Can I use checkpoints for parallel execution?
// ===================================================
// A: Not in current design (sequential execution assumed):
//    - Could extend: Partition data, checkpoint each partition
//    - Complex: Requires aggregating partial results
//    - Not implemented: Single-threaded execution
//
//==============================================================================

//==============================================================================
// SECTION 10: BEST PRACTICES
//==============================================================================
//
// BEST PRACTICE 1: Always Set Job ID
// ===================================
// DO:
//    strategy.set_job_id(job_id);  // Before backtest
//    strategy.backtest_with_checkpoint(data);
//
//
// BEST PRACTICE 2: Use Reasonable Checkpoint Interval
// ====================================================
// DO:
//    // Default 100 is good for most cases
//    SMAStrategyWithCheckpoint strategy(&mgr);
//    
//    // Or calculate based on data size
//    int interval = std::max(10, static_cast<int>(std::sqrt(bars.size())));
//    StrategyWithCheckpoint strategy("SMA", &mgr, interval);
//
//
// BEST PRACTICE 3: Validate Checkpoint on Resume
// ===============================================
// DO (in implementation):
//    if (load_checkpoint_state(cp, resume_idx)) {
//        // Validate checkpoint
//        if (resume_idx >= data.size() || 
//            cp.job_id != current_job_id_) {
//            Logger::warning("Invalid checkpoint, starting from beginning");
//            resume_idx = 0;
//        }
//    }
//
//
// BEST PRACTICE 4: Clean Up Completed Checkpoints
// ================================================
// DO (in backtest_with_checkpoint):
//    // On completion
//    if (checkpoint_manager_) {
//        checkpoint_manager_->delete_checkpoint(current_job_id_);
//    }
//
//
// BEST PRACTICE 5: Log Checkpoint Events
// =======================================
// DO:
//    Logger::info("Saved checkpoint at index " + std::to_string(idx));
//    Logger::info("Resuming from checkpoint at index " + std::to_string(resume_idx));
//    Logger::info("Checkpoint deleted (job complete)");
//
//
// BEST PRACTICE 6: Handle Checkpoint Failures Gracefully
// =======================================================
// DO:
//    if (!save_progress_checkpoint(idx, portfolio, date)) {
//        Logger::error("Checkpoint save failed, continuing anyway");
//        // Don't crash - checkpoints are optimization, not requirement
//    }
//
//
// BEST PRACTICE 7: Test Both Fresh Start and Resume
// ==================================================
// Test scenarios:
//    - Fresh start (no checkpoint)
//    - Resume from checkpoint (various resume points)
//    - Checkpoint corruption (graceful fallback)
//    - Checkpoint interval variations
//
//==============================================================================

//==============================================================================
// SECTION 11: PERFORMANCE TUNING
//==============================================================================
//
// CHECKPOINT INTERVAL TUNING:
// ===========================
//
// Overhead Analysis:
//   checkpoint_interval = 100
//   checkpoint_save_time = 2 ms
//   symbol_process_time = 0.5 ms
//   
//   Time per interval:
//     Processing: 100 × 0.5 ms = 50 ms
//     Checkpoint: 1 × 2 ms = 2 ms
//     Total: 52 ms
//   
//   Overhead: 2 / 52 = 3.8%
//
// Work Loss Analysis:
//   total_symbols = 5000
//   checkpoint_interval = 100
//   
//   Worst case (crash just before checkpoint):
//     Work lost: 99 symbols
//     Percentage: 99 / 5000 = 1.98%
//   
//   Average case:
//     Work lost: interval / 2 = 50 symbols
//     Percentage: 50 / 5000 = 1%
//
// Optimal Interval Formula:
//   minimize: overhead + expected_work_loss
//   
//   overhead = (checkpoint_time / interval) / symbol_time
//   work_loss = (interval / 2) / total_symbols × failure_rate
//   
//   Optimal: interval ≈ sqrt(total_symbols × checkpoint_time / symbol_time)
//
// OPTIMIZATION TECHNIQUES:
//
// 1. Asynchronous Checkpointing:
//    - Save checkpoint in background thread
//    - Continue execution while saving
//    - Complexity: Must ensure consistency
//
// 2. Incremental Checkpoints:
//    - Only save changes since last checkpoint
//    - Smaller files, faster saves
//    - Complexity: Must merge on resume
//
// 3. Compression:
//    - Compress checkpoint before saving
//    - Reduces I/O time (if disk is slow)
//    - Trade-off: CPU for I/O
//
// 4. Memory-Only Checkpoints:
//    - Save to RAM instead of disk
//    - Much faster (microseconds vs milliseconds)
//    - Loss: Not durable (lost on crash)
//    - Use when: Worker process crash rare, OS crash more common
//
//==============================================================================

//==============================================================================
// SECTION 12: TESTING STRATEGIES
//==============================================================================
//
// UNIT TESTS: Checkpoint Operations
// ==================================
//
// TEST(StrategyCheckpointTest, SaveAndLoad) {
//     CheckpointManager mgr;
//     SMAStrategyWithCheckpoint strategy(&mgr);
//     strategy.set_job_id(1000);
//     
//     // Create portfolio state
//     PortfolioState portfolio;
//     portfolio.cash = 9000.0;
//     portfolio.total_value = 10500.0;
//     
//     // Save checkpoint
//     bool saved = strategy.save_progress_checkpoint(250, portfolio, "2020-06-15");
//     EXPECT_TRUE(saved);
//     
//     // Load checkpoint
//     Checkpoint cp;
//     size_t resume_idx;
//     bool loaded = strategy.load_checkpoint_state(cp, resume_idx);
//     
//     EXPECT_TRUE(loaded);
//     EXPECT_EQ(resume_idx, 250);
//     EXPECT_EQ(cp.portfolio_state.cash, 9000.0);
// }
//
//
// INTEGRATION TESTS: Resume Capability
// =====================================
//
// TEST(StrategyCheckpointTest, ResumeAfterInterruption) {
//     CheckpointManager mgr;
//     SMAStrategyWithCheckpoint strategy(&mgr);
//     strategy.set_job_id(2000);
//     strategy.set_parameters(params);
//     
//     auto price_data = load_test_data();
//     
//     // First execution (partial)
//     {
//         std::vector<PriceBar> partial(price_data.begin(), 
//                                       price_data.begin() + 500);
//         strategy.backtest_with_checkpoint(partial);
//         // Checkpoint saved at 100, 200, 300, 400
//     }
//     
//     // Second execution (full - should resume)
//     {
//         auto result = strategy.backtest_with_checkpoint(price_data);
//         
//         // Verify: Result includes entire dataset
//         EXPECT_GT(result.num_trades, 0);
//         EXPECT_TRUE(result.success);
//         
//         // Verify: Checkpoint deleted
//         EXPECT_FALSE(mgr.checkpoint_exists(2000));
//     }
// }
//
//
// STRESS TESTS: Large Datasets
// =============================
//
// TEST(StrategyCheckpointTest, LargeDataset) {
//     // Test with 25 years of data (~6300 bars)
//     // Verify: Checkpoints created and deleted
//     // Verify: Performance acceptable
//     // Verify: Memory usage reasonable
// }
//
//
// FAULT INJECTION TESTS:
// ======================
//
// TEST(StrategyCheckpointTest, CheckpointCorruption) {
//     // Save valid checkpoint
//     // Corrupt checkpoint file
//     // Attempt resume
//     // Verify: Falls back to fresh start
//     // Verify: No crash
// }
//
// TEST(StrategyCheckpointTest, DiskFull) {
//     // Fill disk during checkpoint save
//     // Verify: Handles gracefully
//     // Verify: Execution continues
// }
//
//==============================================================================

//==============================================================================
// SECTION 13: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM: Checkpoint not saving
// ===============================
// SYMPTOMS:
//    - No checkpoint files created
//    - Worker crash: Restarts from beginning
//
// CAUSES & SOLUTIONS:
//    ☐ checkpoint_manager_ is nullptr
//       → Set manager: strategy.set_checkpoint_manager(&mgr)
//    
//    ☐ Disk permissions: Can't write to checkpoint directory
//       → chmod 755 /tmp/checkpoints/
//    
//    ☐ Disk full: No space for checkpoint
//       → Free disk space or change location
//    
//    ☐ job_id not set: checkpoint_0.dat conflicts
//       → Call set_job_id() before backtest
//
//
// PROBLEM: Resume not working
// ============================
// SYMPTOMS:
//    - Always starts from beginning
//    - Checkpoint exists but not loaded
//
// CAUSES & SOLUTIONS:
//    ☐ Different job_id: Looking for wrong checkpoint file
//       → Verify job_id consistent across executions
//    
//    ☐ Checkpoint validation fails: Rejected as invalid
//       → Check logs for validation errors
//    
//    ☐ Checkpoint deleted prematurely: No file to load
//       → Don't delete until job truly complete
//
//
// PROBLEM: Incorrect results after resume
// ========================================
// SYMPTOMS:
//    - Resume execution produces different results
//    - Trades don't match full execution
//
// CAUSES & SOLUTIONS:
//    ☐ Portfolio state not fully saved/loaded
//       → Verify all portfolio fields in checkpoint
//    
//    ☐ Parameters changed between runs
//       → Validate parameters match checkpoint
//    
//    ☐ Random number generator state not saved
//       → If strategy uses randomness, save RNG state too
//
//
// PROBLEM: High checkpoint overhead
// ==================================
// SYMPTOMS:
//    - Backtest much slower with checkpoints
//    - High I/O wait time
//
// CAUSES & SOLUTIONS:
//    ☐ Interval too small: Checkpointing too often
//       → Increase checkpoint_interval_ (e.g., 100 → 500)
//    
//    ☐ Slow disk: I/O bottleneck
//       → Use faster storage (SSD) or in-memory checkpoints
//    
//    ☐ Large portfolio state: Serialization expensive
//       → Minimize checkpoint size (only essential state)
//
//==============================================================================

//==============================================================================
// ARCHITECTURAL INTEGRATION NOTES
//==============================================================================
//
// INTEGRATION WITH WORKER:
// ========================
//
// Worker class usage pattern:
//
// class Worker {
//     CheckpointManager checkpoint_manager_;
//     
//     JobResult execute_job(uint64_t job_id, const JobParams& params) {
//         // Create checkpoint-aware strategy
//         SMAStrategyWithCheckpoint strategy(&checkpoint_manager_);
//         
//         // Configure
//         strategy.set_job_id(job_id);
//         strategy.set_parameters(params);
//         
//         // Load data
//         auto data = load_price_data(params);
//         
//         // Execute with checkpoints
//         auto result = strategy.backtest_with_checkpoint(data);
//         
//         return result;
//     }
// };
//
// CHECKPOINT MANAGER LIFECYCLE:
// - Created: Worker initialization
// - Shared: Across all strategy executions
// - Destroyed: Worker shutdown
//
// STRATEGY LIFECYCLE:
// - Created: Per job execution
// - Configured: Before backtest
// - Destroyed: After backtest complete
// - Lifetime: Shorter than CheckpointManager
//
// INTEGRATION WITH CONTROLLER:
// ============================
//
// Controller doesn't know about checkpoints:
// - Assigns jobs to workers
// - Worker handles checkpointing internally
// - If worker fails: Controller reassigns job
// - New worker: Finds checkpoint, resumes
//
// Transparency: Controller sees worker failure and reassignment
//              Worker sees job execution with resume capability
//
// COMPARISON WITH ALTERNATIVES:
// ==============================
//
// Alternative 1: No Checkpoints
//   Pros: Simpler, no overhead
//   Cons: Must restart on failure (high cost for long jobs)
//
// Alternative 2: Transaction Log (Fine-grained)
//   Pros: Can replay any point
//   Cons: High overhead (log every operation)
//
// Alternative 3: Periodic Snapshots (Current Design)
//   Pros: Low overhead, good recovery
//   Cons: Some work lost (acceptable)
//
// Alternative 4: Redundant Execution
//   Pros: No restart needed (continue on backup)
//   Cons: 2x compute resources
//
// Chosen: Periodic snapshots (best trade-off)
//
// FUTURE ENHANCEMENTS:
// ====================
//
// 1. Adaptive Interval:
//    - Adjust based on symbol processing time
//    - Faster symbols: Larger interval
//    - Slower symbols: Smaller interval
//
// 2. Multi-level Checkpoints:
//    - Frequent: Quick checkpoints (memory)
//    - Infrequent: Full checkpoints (disk)
//    - Combines: Fast recovery + durability
//
// 3. Delta Checkpoints:
//    - Save only changes since last checkpoint
//    - Reduces: Checkpoint size and save time
//    - Complexity: Must merge on resume
//
// 4. Checkpoint Compression:
//    - Compress before writing to disk
//    - Reduces: I/O time (if disk slow)
//    - Trade-off: CPU for I/O
//
//==============================================================================

//==============================================================================
// SMA CROSSOVER ALGORITHM DETAILS
//==============================================================================
//
// SIMPLE MOVING AVERAGE (SMA):
// ============================
//
// Definition:
//   SMA[i, window] = (1/window) × Σ(price[i-window+1 : i])
//
// Example (5-day SMA):
//   Prices: [100, 102, 101, 103, 104, 105, 106]
//   Window: 5
//   
//   SMA[4] = (100 + 102 + 101 + 103 + 104) / 5 = 102.0
//   SMA[5] = (102 + 101 + 103 + 104 + 105) / 5 = 103.0
//   SMA[6] = (101 + 103 + 104 + 105 + 106) / 5 = 103.8
//
// CROSSOVER SIGNALS:
// ==================
//
// Golden Cross (BUY):
//   - Condition: short_MA[i] > long_MA[i] AND short_MA[i-1] <= long_MA[i-1]
//   - Meaning: Short MA crossed above long MA
//   - Interpretation: Bullish signal (upward momentum)
//   - Action: Enter long position
//
// Death Cross (SELL):
//   - Condition: short_MA[i] < long_MA[i] AND short_MA[i-1] >= long_MA[i-1]
//   - Meaning: Short MA crossed below long MA
//   - Interpretation: Bearish signal (downward momentum)
//   - Action: Exit long position
//
// SIGNAL GENERATION PSEUDOCODE:
//
//   for i from long_window to data.size():
//       short_MA = average(prices[i-short_window : i])
//       long_MA = average(prices[i-long_window : i])
//       
//       if short_MA > long_MA and prev_short_MA <= prev_long_MA:
//           signal[i] = BUY  # Golden cross
//       elif short_MA < long_MA and prev_short_MA >= prev_long_MA:
//           signal[i] = SELL  # Death cross
//       else:
//           signal[i] = HOLD  # No crossover
//
// TYPICAL RESULTS:
// - Number of signals: 2-10 per year (infrequent)
// - Win rate: ~50% (momentum can be profitable in trending markets)
// - Sharpe ratio: 0.5-1.5 (moderate risk-adjusted returns)
//
// STRENGTHS:
// - Simple and intuitive
// - Works well in trending markets
// - Reduces noise (smoothing effect)
//
// WEAKNESSES:
// - Lagging indicator (late entries/exits)
// - Poor in ranging/choppy markets (whipsaws)
// - Many parameters (window sizes)
//
//==============================================================================

//==============================================================================
// COMPARISON WITH OTHER STRATEGY PATTERNS
//==============================================================================
//
// PATTERN 1: Plain Strategy (No Checkpoints)
// ===========================================
// class SMAStrategy : public Strategy {
//     std::vector<Signal> generate_signals(...) override;
// };
//
// Pros: Simple, minimal overhead
// Cons: No fault tolerance, must restart on failure
//
//
// PATTERN 2: Strategy with Manual Checkpoints
// ============================================
// class SMAStrategy : public Strategy {
//     void save_state() { /* manual checkpoint */ }
//     void load_state() { /* manual resume */ }
// };
//
// Pros: Full control over checkpoint content
// Cons: More boilerplate, error-prone
//
//
// PATTERN 3: Strategy with Checkpoint (Current Design)
// =====================================================
// class SMAStrategyWithCheckpoint : public StrategyWithCheckpoint {
//     // Inherits checkpoint infrastructure
//     std::vector<Signal> generate_signals(...) override;
// };
//
// Pros: Automatic checkpointing, minimal code, fault tolerant
// Cons: Slight complexity (inheritance), small overhead
//
// RECOMMENDATION: Use StrategyWithCheckpoint for long-running jobs
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================