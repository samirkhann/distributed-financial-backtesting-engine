/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025

    File: strategy_with_checkpoint.cpp

    Description:
        This file contains the user-implemented functions for Part 1 exercises:
        - Part 1-1: Demonstrating the use of std::unique_ptr and transferring ownership.
        - Part 1-2: Demonstrating the use of std::shared_ptr and shared ownership.
        - Part 1-3: Implementing Library class functions using std::list and std::unique_ptr
          to add and check out books.

    Purpose:
        This code implements checkpoint-aware strategy execution for a C++ backtesting
        engine. It contains:
          - StrategyWithCheckpoint: base class that manages checkpoint I/O, portfolio
            state, backtest loop, and metrics calculation.
          - SMAStrategyWithCheckpoint: a concrete SMA crossover strategy derived from
            StrategyWithCheckpoint, providing signal generation logic.

    How to use (summary):
      1. Instantiate a concrete strategy (e.g., SMAStrategyWithCheckpoint) with:
         - params_ filled (including symbol, initial capital, windows)
         - a pointer to a checkpoint manager (must be thread-safe if used concurrently)
         - current_job_id_ set (> 0) if checkpointing is desired
         - checkpoint_interval_ set to required frequency (0 disables checkpointing)
      2. Call backtest_with_checkpoint(price_data) to run or resume a job.
      3. Inspect returned JobResult for metrics, trades, and status.
      4. The checkpoint is automatically deleted on successful completion.

    File-level Table of Contents:
      1. Module overview (above)
      2. High level contract for key functions (documented before functions)
      3. Inline commentary explaining tricky parts, concurrency and failure modes
      4. Examples & common mistakes
      5. FAQ (common questions answered in comments)

    Important design notes and assumptions:
      - CheckpointManager is an external dependency and must provide:
          bool load_checkpoint(job_id, Checkpoint& out);
          bool save_checkpoint(const Checkpoint& cp);
          bool delete_checkpoint(job_id);
      - PriceBar vector must be time-ordered ascending.
      - generate_signals() must return exactly price_data.size() signals, one per bar.
      - The backtest engine here executes discrete buy-all / sell-all trades only.
      - Checkpoints store minimal resume state: last_processed_index, cash, shares, portfolio_value.
      - This file purposely does not change algorithmic logic — only adds documentation.
*******************************************************************************/

#include "strategy/strategy_with_checkpoint.h"
#include "common/logger.h"
#include <algorithm>
#include <numeric>

namespace backtesting {

/*
================================================================================
Function: load_checkpoint_state
Purpose : Attempt to load a checkpoint for the current_job_id_. If found and valid,
          returns true and sets resume_index to cp.last_processed_index. If no
          checkpoint exists or checkpoint manager is unavailable, returns false.

Key invariants / expectations:
  - checkpoint_manager_ must be non-null to allow checkpoint operations.
  - current_job_id_ must be > 0 to identify an existing job.
  - Checkpoint::valid flag is used as a final integrity check.

Thread-safety:
  - This function delegates persistence to checkpoint_manager_. The manager must
    be thread-safe if multiple threads may load/save checkpoints concurrently.

Return:
  - true  => checkpoint loaded and resume_index set to last_processed_index.
  - false => no usable checkpoint found or checkpoint manager unavailable.

Common pitfalls:
  - Relying on cp.valid without additional integrity checks (e.g., checksum).
  - Assuming resume_index corresponds to safe re-entry point for all strategies;
    ensure generate_signals() is deterministic and idempotent from resume_index.
*/
bool StrategyWithCheckpoint::load_checkpoint_state(Checkpoint& cp, size_t& resume_index) {
    if (!checkpoint_manager_ || current_job_id_ == 0) {
        // No manager or invalid job id -> treat as "no checkpoint"
        return false;
    }
    
    if (!checkpoint_manager_->load_checkpoint(current_job_id_, cp)) {
        // Manager exists but no checkpoint for this job id
        return false;
    }
    
    if (!cp.valid) {
        // The checkpoint manager returned a checkpoint but it's marked invalid.
        // Log and do not resume from corrupt checkpoint.
        Logger::warning("Invalid checkpoint for job " + std::to_string(current_job_id_));
        return false;
    }
    
    resume_index = cp.last_processed_index;
    Logger::info("Resuming job " + std::to_string(current_job_id_) +
                " from index " + std::to_string(resume_index));
    
    return true;
}

/*
================================================================================
Function: save_progress_checkpoint
Purpose : Persist a small resume state mid-run. This is called periodically from the
          main backtest loop to allow crash-recovery without reprocessing work.

Inputs:
  - current_index  : index in price_data last processed (0-based)
  - portfolio      : snapshot of portfolio state (cash, shares_held, portfolio_value)
  - current_date   : string date associated with the current index (for human readability)

What it stores in checkpoint:
  - job_id, symbol, last_processed_index, symbols_processed (same as index),
    current_cash, current_shares, portfolio_value, last_date_processed, valid=true

Return:
  - result of checkpoint_manager_->save_checkpoint(cp).
  - If checkpoint manager is null or job id is 0, returns false.

Best practices:
  - Keep checkpoint content minimal to reduce I/O.
  - Write checkpoints atomically in the manager (tmp file + rename) to avoid corrupt states.
  - If checkpointing very frequently, consider batching or adaptive intervals.

Potential improvements:
  - Add a checksum or version field to the checkpoint to ensure schema compatibility.
  - Persist RNG state (if strategies rely on randomness) to guarantee exact resume semantics.
*/
bool StrategyWithCheckpoint::save_progress_checkpoint(size_t current_index,
                                                     const PortfolioState& portfolio,
                                                     const std::string& current_date) {
    if (!checkpoint_manager_ || current_job_id_ == 0) {
        return false;
    }
    
    Checkpoint cp;
    cp.job_id = current_job_id_;
    cp.symbol = params_.symbol;
    cp.symbols_processed = current_index;
    cp.last_processed_index = current_index;
    cp.current_cash = portfolio.cash;
    cp.current_shares = portfolio.shares_held;
    cp.portfolio_value = portfolio.portfolio_value;
    cp.last_date_processed = current_date;
    cp.valid = true;
    
    return checkpoint_manager_->save_checkpoint(cp);
}

/*
================================================================================
Function: backtest_with_checkpoint
Purpose : Run the backtest for the given price_data vector, optionally resuming
          from a saved checkpoint. Produces a JobResult that contains performance
          metrics, trade counts, and success/error information.

High-level algorithm:
  1. Validate input data.
  2. Try to load checkpoint state.
  3. Initialize or resume portfolio state.
  4. Generate signals for the entire dataset (one signal per price bar).
  5. Iterate from resume index to end:
       - Execute buy/sell logic based on signals and portfolio.
       - Update portfolio value, compute returns.
       - Periodically call save_progress_checkpoint.
  6. Compute final metrics (total return, Sharpe, max drawdown, trade counts).
  7. On success, delete checkpoint to avoid stale resume.
  8. Return JobResult.

Important behavioral details:
  - The code uses a simple "buy all with available cash" on BUY and "sell all" on SELL.
  - No partial fills, slippage, commissions, or order types are modeled.
  - Trades are only executed on transitions (signal != prev_signal) and not on HOLD signals.
  - Checkpoint interval is interpreted relative to the resume point: (i - start_index) % checkpoint_interval_.
  - After a successful run, checkpoint is deleted to avoid accidental resumption.

Edge cases & assumptions:
  - generate_signals must be deterministic for resume correctness.
  - If resume_index points to a bar that already included a trade, the logic must ensure idempotence:
      e.g., if we checkpointed after processing bar i and resume at i, we must not re-execute side-effectful trades twice.
    This implementation resumes at last_processed_index and starts processing from that index; ensure that the checkpoint
    semantics align with when the checkpoint was created (after or before applying the trade).

Performance considerations:
  - Generating signals for the entire dataset up front uses more memory but simplifies resume semantics.
  - If price_data is large, consider generating signals on-the-fly or precomputing and storing to disk.

Return:
  - JobResult with success=true on successful completion, or success=false with error_message filled.
*/
JobResult StrategyWithCheckpoint::backtest_with_checkpoint(
    const std::vector<PriceBar>& price_data) {
    
    JobResult result;
    result.symbol = params_.symbol;
    result.success = false;
    
    if (price_data.empty()) {
        result.error_message = "No price data available";
        return result;
    }
    
    // Try to load checkpoint
    Checkpoint checkpoint;
    size_t resume_index = 0;
    bool has_checkpoint = load_checkpoint_state(checkpoint, resume_index);
    
    // Initialize portfolio (start fresh or resume)
    PortfolioState portfolio(params_.initial_capital);
    if (has_checkpoint) {
        // Resume portfolio state from checkpoint snapshot.
        portfolio.cash = checkpoint.current_cash;
        portfolio.shares_held = checkpoint.current_shares;
        portfolio.portfolio_value = checkpoint.portfolio_value;
        portfolio.peak_value = checkpoint.portfolio_value;
        
        Logger::info("Resumed with: cash=" + std::to_string(portfolio.cash) +
                    ", shares=" + std::to_string(portfolio.shares_held));
    }
    
    // Generate signals for entire dataset up-front.
    // NOTE: This must be deterministic and stable across runs for correct resume behavior.
    std::vector<Signal> signals;
    try {
        signals = generate_signals(price_data);
    } catch (const std::exception& e) {
        // Fail early if signal generation throws.
        result.error_message = std::string("Signal generation failed: ") + e.what();
        return result;
    }
    
    if (signals.size() != price_data.size()) {
        // Signal array mismatch indicates a bug in the concrete strategy.
        result.error_message = "Signal count mismatch";
        return result;
    }
    
    // Containers to record trades and values for metrics.
    std::vector<Trade> trades;
    std::vector<double> portfolio_values;
    std::vector<double> returns;
    
    Signal prev_signal = Signal::HOLD;
    size_t start_index = has_checkpoint ? resume_index : 0;
    
    // Iterate through bars from resume point
    for (size_t i = start_index; i < price_data.size(); ++i) {
        const auto& bar = price_data[i];
        Signal signal = signals[i];
        
        // Execute trades on transition and when not HOLD.
        if (signal != prev_signal && signal != Signal::HOLD) {
            Trade trade;
            trade.date = bar.date;
            trade.signal = signal;
            trade.price = bar.close;
            
            if (signal == Signal::BUY && portfolio.shares_held == 0) {
                // Buy with available cash: integer number of shares
                int shares = static_cast<int>(portfolio.cash / bar.close);
                if (shares > 0) {
                    trade.shares = shares;
                    trade.value = shares * bar.close;
                    portfolio.cash -= trade.value;
                    portfolio.shares_held += shares;
                    trades.push_back(trade);
                } else {
                    // No shares bought because cash < price. This is normal for some datasets.
                    Logger::debug("Not enough cash to buy at index " + std::to_string(i));
                }
            } else if (signal == Signal::SELL && portfolio.shares_held > 0) {
                // Sell all shares held.
                trade.shares = portfolio.shares_held;
                trade.value = portfolio.shares_held * bar.close;
                portfolio.cash += trade.value;
                portfolio.shares_held = 0;
                trades.push_back(trade);
            }
        }
        
        // Update portfolio value and metrics
        double current_value = portfolio.cash + portfolio.shares_held * bar.close;
        portfolio_values.push_back(current_value);
        
        // Simple consecutive return (period-to-period)
        if (i > start_index && portfolio_values.size() > 1) {
            double prev_val = portfolio_values[portfolio_values.size() - 2];
            // Avoid division by zero; if previous value is zero, skip return computation.
            if (prev_val != 0.0) {
                double ret = (current_value - prev_val) / prev_val;
                returns.push_back(ret);
            } else {
                // prev_val == 0 — skip the return calculation to avoid NaN/inf
                Logger::debug("Previous portfolio value is zero at index " + std::to_string(i-1));
            }
        }
        
        // Track peak for drawdown computation
        if (current_value > portfolio.peak_value) {
            portfolio.peak_value = current_value;
        }
        
        portfolio.portfolio_value = current_value;
        prev_signal = signal;
        
        // Periodic checkpointing (do not checkpoint at first processed index immediately)
        if (checkpoint_interval_ > 0 && 
            (i - start_index) % checkpoint_interval_ == 0 && 
            i > start_index) {
            // Save current progress; manager expected to be thread-safe and atomic.
            if (!save_progress_checkpoint(i, portfolio, bar.date)) {
                Logger::warning("Failed to save checkpoint for job " + std::to_string(current_job_id_));
            }
        }
    }
    
    // Final metrics calculation
    result.final_portfolio_value = portfolio.portfolio_value;
    result.total_return = ((portfolio.portfolio_value - params_.initial_capital) / 
                          params_.initial_capital) * 100.0;
    
    if (!returns.empty()) {
        result.sharpe_ratio = calculate_sharpe_ratio(returns);
    } else {
        // No returns computed; zero or undefined Sharpe.
        result.sharpe_ratio = 0.0;
    }
    
    result.max_drawdown = calculate_max_drawdown(portfolio_values);
    result.num_trades = static_cast<int>(trades.size());
    
    // Count winning/losing trades by pairing BUY then SELL events.
    // Note: trades recorded are in chronological order; we analyze pairs of buy->sell.
    for (size_t i = 1; i < trades.size(); ++i) {
        if (trades[i].signal == Signal::SELL && trades[i-1].signal == Signal::BUY) {
            double profit = trades[i].value - trades[i-1].value;
            if (profit > 0) {
                result.winning_trades++;
            } else {
                result.losing_trades++;
            }
        }
    }
    
    result.success = true;
    
    // On successful completion, delete checkpoint to prevent stale resume.
    if (checkpoint_manager_ && current_job_id_ > 0) {
        checkpoint_manager_->delete_checkpoint(current_job_id_);
    }
    
    return result;
}

/*
================================================================================
SMA Strategy implementation

calculate_sma:
  - Efficient rolling-window calculation (O(n)).
  - Input: vector of prices, window size (int).
  - Output: vector<double> of same length as prices, with 0.0 for indices with
            insufficient history (first window-1 entries are zero except the
            index window-1 holds the first average).

Notes:
  - For windows larger than prices.size(), the function returns zeros.
  - No smoothing or NaN handling beyond zero fill is performed.
*/
std::vector<double> SMAStrategyWithCheckpoint::calculate_sma(
    const std::vector<double>& prices, int window) const {
    
    std::vector<double> sma(prices.size(), 0.0);
    
    if (prices.size() < static_cast<size_t>(window)) {
        return sma;
    }
    
    double sum = 0.0;
    // Initial sum for first window
    for (int i = 0; i < window; ++i) {
        sum += prices[i];
    }
    sma[window - 1] = sum / window;
    
    // Sliding window updates to maintain O(n) complexity
    for (size_t i = window; i < prices.size(); ++i) {
        sum = sum - prices[i - window] + prices[i];
        sma[i] = sum / window;
    }
    
    return sma;
}

/*
================================================================================
generate_signals (SMA crossover logic):

Purpose:
  - For each bar, determine BUY/SELL/HOLD based on SMA short/long crossover.

Algorithm:
  - Compute short_sma and long_sma arrays (same length as prices).
  - Starting from index long_window_, determine whether short_sma > long_sma
    (short above) or not.
  - A crossover event occurs when the boolean "short above long" changes state.
      - If it transitions false -> true => BUY
      - If it transitions true -> false => SELL
  - The implementation avoids generating a signal at the exact long_window_ index
    (i > long_window_ in the code), reducing spurious signals on initial edge.

Important correctness notes:
  - Both SMAs must be > 0 to be considered (guards against insufficient data).
  - The function fills the signals vector with HOLD by default.
  - Caller must ensure price_data.size() >= max(short_window, long_window).

Edge cases & behavior:
  - If short_window_ >= long_window_, the logic still runs but crossover semantics
    may be meaningless. Validate window sizes before use.
  - No hysteresis or delay is added; consider adding a confirmation window if
    you observe excessive whipsaw trading in noisy data.

Complexity:
  - O(n) for computing SMAs and O(n) for generating signals. Memory O(n).
*/
std::vector<Signal> SMAStrategyWithCheckpoint::generate_signals(
    const std::vector<PriceBar>& price_data) {
    
    std::vector<Signal> signals(price_data.size(), Signal::HOLD);
    
    if (price_data.size() < static_cast<size_t>(std::max(short_window_, long_window_))) {
        // Not enough data; return all HOLDs.
        return signals;
    }
    
    // Extract prices into a compact vector for number crunching.
    std::vector<double> prices;
    prices.reserve(price_data.size());
    for (const auto& bar : price_data) {
        prices.push_back(bar.close);
    }
    
    auto short_sma = calculate_sma(prices, short_window_);
    auto long_sma = calculate_sma(prices, long_window_);
    
    bool prev_above = false;
    // Start at long_window_ because long_sma before that is zero/unset.
    for (size_t i = long_window_; i < prices.size(); ++i) {
        if (short_sma[i] > 0 && long_sma[i] > 0) {
            bool curr_above = short_sma[i] > long_sma[i];
            
            if (i > static_cast<size_t>(long_window_)) {
                // We only emit a signal when the relationship changes compared to prev_above.
                if (curr_above && !prev_above) {
                    signals[i] = Signal::BUY;
                } else if (!curr_above && prev_above) {
                    signals[i] = Signal::SELL;
                }
            }
            
            prev_above = curr_above;
        } else {
            // If one SMA is non-positive, we skip signaling for this index.
            // This can happen in initial padding region; signals remain HOLD.
        }
    }
    
    return signals;
}

} // namespace backtesting
