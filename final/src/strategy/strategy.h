/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: strategy.h
    
    Description:
        This header file defines the core backtesting framework for evaluating
        trading strategies on historical financial data. It provides the base
        Strategy class and supporting data structures for generating trading
        signals, executing simulated trades, and calculating performance metrics.
        
        Core Components:
        - Signal enum: Trading actions (BUY, SELL, HOLD)
        - Trade struct: Records individual trade execution details
        - PortfolioState: Tracks cash, positions, and portfolio value over time
        - Strategy base class: Abstract interface for implementing trading strategies
        
        Key Features:
        - Strategy pattern for pluggable trading algorithms
        - Realistic backtesting with commission and slippage modeling
        - Standard performance metrics (Sharpe ratio, max drawdown)
        - Portfolio tracking with cash and position management
        - Support for multiple strategy types (SMA, RSI, Mean Reversion, etc.)
        
        Trading Strategy Framework:
        Strategies generate signals based on historical price data, then the
        backtesting engine simulates trading those signals with a starting
        capital allocation. The framework tracks all trades, calculates returns,
        and computes risk-adjusted performance metrics.
        
        Backtesting Workflow:
        1. Load historical price data (OHLCV bars)
        2. Strategy analyzes data and generates trading signals
        3. Backtest engine executes trades based on signals
        4. Track portfolio state (cash, shares, value) over time
        5. Calculate performance metrics (total return, Sharpe, drawdown)
        6. Return results for comparison and optimization
        
        Integration with Distributed System:
        This backtesting framework is executed by worker nodes in the distributed
        system. Controller nodes submit JobParams (symbol, date range, strategy
        parameters) to workers, which run backtests and return JobResults with
        performance metrics. Multiple strategies can be backtested concurrently
        across different workers for parallel evaluation.
        
        Performance Considerations:
        - Vectorized signal generation for speed (process entire time series)
        - In-memory computation (no database queries during backtest)
        - O(n) complexity where n = number of price bars
        - Typical backtest: 1000 bars in ~1-10ms
        - Bottleneck: Signal generation algorithm, not framework overhead
        
        Strategy Implementation Pattern:
        To create a new strategy:
        1. Inherit from Strategy base class
        2. Implement generate_signals() with your algorithm logic
        3. Optionally override set_parameters() for custom params
        4. Framework handles execution, tracking, and metric calculation
        
    Dependencies:
        - data/csv_loader.h: PriceBar structure for OHLCV data
        - common/message.h: JobParams and JobResult for distributed execution
        - <vector>: Dynamic arrays for signals and returns
        - <memory>: Smart pointers for strategy instances
        
    Mathematical Foundations:
        SHARPE RATIO: risk-adjusted return metric
        SR = (mean_return - risk_free_rate) / std_deviation_return
        - Measures excess return per unit of risk
        - Higher is better (> 1.0 is good, > 2.0 is excellent)
        
        MAX DRAWDOWN: largest peak-to-trough decline
        MDD = max((peak_value - current_value) / peak_value)
        - Measures worst-case loss from previous high
        - Lower is better (closer to 0% is less risky)
        - Used for risk management and position sizing
        
    Thread Safety:
        Strategy instances are NOT thread-safe. Each worker should create
        its own strategy instance. Do not share strategy objects across threads.
        The design assumes single-threaded execution per backtest job.
*******************************************************************************/

#ifndef STRATEGY_H
#define STRATEGY_H

#include "data/csv_loader.h"
#include "common/message.h"
#include <vector>
#include <memory>

namespace backtesting {

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
// 1. Overview and Architecture
// 2. Signal Enum - Trading Actions
// 3. Trade Structure - Trade Execution Records
// 4. PortfolioState Structure - Position Tracking
// 5. Strategy Base Class
//    5.1 Public Interface
//    5.2 Protected Helper Methods
// 6. Usage Examples and Patterns
// 7. Implementing Custom Strategies
// 8. Performance Metrics Explained
// 9. Common Pitfalls and Solutions
// 10. FAQ
//==============================================================================

//==============================================================================
// SECTION 1: Overview and Architecture
//==============================================================================

/**
 * BACKTESTING FRAMEWORK ARCHITECTURE
 * 
 * This framework implements a realistic backtesting engine for evaluating
 * quantitative trading strategies on historical price data. The design
 * follows several key principles:
 * 
 * 1. SEPARATION OF CONCERNS:
 *    - Signal generation (strategy logic)
 *    - Trade execution (backtesting engine)
 *    - Performance measurement (metrics calculation)
 * 
 * 2. STRATEGY PATTERN:
 *    - Base Strategy class defines interface
 *    - Concrete strategies implement generate_signals()
 *    - Enables easy addition of new strategies without modifying framework
 * 
 * 3. REALISTIC SIMULATION:
 *    - Portfolio tracking with cash and share accounting
 *    - Commission and slippage modeling (optional)
 *    - No look-ahead bias (signals generated only on past data)
 * 
 * WORKFLOW:
 * 
 *   Historical Data → Strategy → Signals → Backtest Engine → Results
 *   
 *   [Price Bars]     [SMA/RSI]  [BUY/SELL]  [Execute Trades]  [Sharpe/DD]
 *        │               │           │              │              │
 *        │               │           │              │              │
 *        └───────────────┴───────────┴──────────────┴──────────────┘
 *                        Pluggable Trading Strategy
 * 
 * DATA FLOW:
 * 
 * 1. Load price data from CSV (date, OHLC, volume)
 * 2. Strategy analyzes entire time series
 * 3. Strategy returns vector of signals (one per bar)
 * 4. Backtest engine simulates trading:
 *    - BUY: Purchase shares with available cash
 *    - SELL: Liquidate all shares
 *    - HOLD: Do nothing
 * 5. Track portfolio value over time
 * 6. Calculate metrics from portfolio history
 * 7. Return JobResult with performance data
 * 
 * EXAMPLE STRATEGIES:
 * 
 * MOVING AVERAGE CROSSOVER:
 * - Calculate fast MA (e.g., 50-day) and slow MA (e.g., 200-day)
 * - BUY when fast MA crosses above slow MA (bullish)
 * - SELL when fast MA crosses below slow MA (bearish)
 * 
 * RSI MOMENTUM:
 * - Calculate Relative Strength Index
 * - BUY when RSI crosses above oversold threshold (e.g., 30)
 * - SELL when RSI crosses below overbought threshold (e.g., 70)
 * 
 * MEAN REVERSION:
 * - Calculate price deviation from moving average
 * - BUY when price is > N standard deviations below MA
 * - SELL when price is > N standard deviations above MA
 * 
 * DISTRIBUTED EXECUTION:
 * 
 * In the distributed backtesting system:
 * 
 * Controller:
 * - Receives client request to backtest strategy
 * - Creates JobParams with symbol, date range, strategy type, parameters
 * - Assigns job to available worker
 * 
 * Worker:
 * - Loads historical price data for symbol
 * - Instantiates strategy (e.g., SMAStrategy)
 * - Sets parameters from JobParams
 * - Runs backtest (this framework)
 * - Returns JobResult with metrics
 * 
 * Controller:
 * - Aggregates results from multiple workers
 * - Returns to client
 * 
 * ADVANTAGES:
 * - Parallel backtesting across symbols and strategies
 * - Scalable to large datasets (distribute by symbol or time period)
 * - Fault-tolerant (worker failures handled by Raft consensus)
 */

//==============================================================================
// SECTION 2: Signal Enum - Trading Actions
//==============================================================================

/**
 * @enum Signal
 * @brief Represents a trading action to take at a given time
 * 
 * TRADING SEMANTICS:
 * 
 * In real-world trading, signals indicate the desired position:
 * - BUY: Enter or increase long position (bullish)
 * - SELL: Exit long position or enter short (bearish)
 * - HOLD: Maintain current position (neutral)
 * 
 * BACKTESTING IMPLEMENTATION (Long-Only):
 * 
 * This framework implements long-only trading (no short selling):
 * 
 * BUY Signal:
 * - If currently hold 0 shares: Buy max shares with available cash
 * - If currently hold shares: Do nothing (already long)
 * 
 * SELL Signal:
 * - If currently hold shares: Sell all shares
 * - If currently hold 0 shares: Do nothing (already flat)
 * 
 * HOLD Signal:
 * - Do nothing, maintain current position
 * 
 * POSITION SIZING:
 * 
 * When BUY signal occurs:
 * ```
 * shares_to_buy = floor(available_cash / current_price)
 * cost = shares_to_buy * current_price
 * cash -= cost
 * shares_held += shares_to_buy
 * ```
 * 
 * When SELL signal occurs:
 * ```
 * proceeds = shares_held * current_price
 * cash += proceeds
 * shares_held = 0
 * ```
 * 
 * SIGNAL GENERATION:
 * 
 * Strategies generate one signal per price bar:
 * ```cpp
 * std::vector<Signal> signals(price_data.size());
 * for (size_t i = 0; i < price_data.size(); ++i) {
 *     if (should_buy(i)) {
 *         signals[i] = Signal::BUY;
 *     } else if (should_sell(i)) {
 *         signals[i] = Signal::SELL;
 *     } else {
 *         signals[i] = Signal::HOLD;
 *     }
 * }
 * return signals;
 * ```
 * 
 * NUMERIC VALUES:
 * 
 * Signals are represented as integers for convenience:
 * - HOLD = 0: Neutral
 * - BUY = 1: Positive action
 * - SELL = -1: Negative action
 * 
 * This allows mathematical operations:
 * ```cpp
 * int total_signal = static_cast<int>(signal1) + static_cast<int>(signal2);
 * // Can combine multiple signals (e.g., ensemble strategies)
 * ```
 * 
 * COMMON PATTERNS:
 * 
 * TREND FOLLOWING:
 * - BUY when trend turns up
 * - SELL when trend turns down
 * - HOLD in sideways markets
 * 
 * MEAN REVERSION:
 * - BUY when price oversold
 * - SELL when price overbought
 * - HOLD near fair value
 * 
 * BREAKOUT:
 * - BUY when price breaks above resistance
 * - SELL when price breaks below support
 * - HOLD in consolidation
 */
enum class Signal {
    /**
     * HOLD: No action, maintain current position
     * 
     * WHEN TO USE:
     * - Insufficient evidence to trade
     * - Market conditions don't match strategy criteria
     * - Waiting for clearer signal
     * - Already in desired position
     * 
     * EFFECT:
     * - No trade executed
     * - Portfolio unchanged
     * - No transaction costs
     * 
     * EXAMPLE:
     * Price is between moving averages (no clear trend)
     * → Signal::HOLD
     */
    HOLD = 0,
    
    /**
     * BUY: Enter long position or increase holdings
     * 
     * WHEN TO USE:
     * - Bullish signal detected
     * - Price expected to rise
     * - Entry condition met
     * - Currently hold no shares
     * 
     * EFFECT:
     * - Purchase maximum shares with available cash
     * - Cash decreases by shares × price
     * - shares_held increases
     * - Transaction costs applied (if modeled)
     * 
     * EXAMPLE:
     * Fast MA crosses above slow MA (bullish crossover)
     * → Signal::BUY
     * 
     * POSITION SIZING NOTE:
     * Current implementation uses all available cash (max leverage).
     * Production systems typically use:
     * - Fixed percentage of capital per trade (e.g., 2%)
     * - Risk-based sizing (e.g., $100 risk per trade)
     * - Kelly criterion for optimal sizing
     */
    BUY = 1,
    
    /**
     * SELL: Exit long position, liquidate holdings
     * 
     * WHEN TO USE:
     * - Bearish signal detected
     * - Price expected to fall
     * - Exit condition met
     * - Take profit or stop loss triggered
     * 
     * EFFECT:
     * - Sell all shares held
     * - Cash increases by shares × price
     * - shares_held becomes 0
     * - Transaction costs applied (if modeled)
     * 
     * EXAMPLE:
     * Fast MA crosses below slow MA (bearish crossover)
     * → Signal::SELL
     * 
     * IMPLEMENTATION NOTE:
     * Current implementation exits entire position (all-or-nothing).
     * Partial exits not supported but could be added by extending
     * Signal enum to include quantity.
     */
    SELL = -1
};

//==============================================================================
// SECTION 3: Trade Structure - Trade Execution Records
//==============================================================================

/**
 * @struct Trade
 * @brief Records details of a single executed trade
 * 
 * PURPOSE:
 * Maintains an audit trail of all trades executed during backtesting.
 * Used for:
 * - Transaction history reporting
 * - Debugging strategy logic
 * - Tax lot accounting (if needed)
 * - Analyzing trade distribution
 * 
 * LIFECYCLE:
 * 1. Trade created when BUY or SELL signal executed
 * 2. Fields populated with execution details
 * 3. Added to trades vector in JobResult
 * 4. Returned to client for analysis
 * 
 * EXAMPLE TRADE SEQUENCE:
 * 
 * Initial: $10,000 cash, 0 shares
 * 
 * Trade 1 (BUY):
 *   date = "2020-01-15"
 *   signal = Signal::BUY
 *   price = $100
 *   shares = 100 (max shares with $10,000)
 *   value = $10,000
 * After: $0 cash, 100 shares
 * 
 * Trade 2 (SELL):
 *   date = "2020-03-20"
 *   signal = Signal::SELL
 *   price = $120
 *   shares = 100
 *   value = $12,000
 * After: $12,000 cash, 0 shares
 * 
 * Profit: $2,000 (20% return)
 * 
 * USAGE IN ANALYSIS:
 * 
 * WINNING TRADES:
 * ```cpp
 * int wins = 0;
 * for (auto& trade : trades) {
 *     if (trade.signal == Signal::SELL) {
 *         // Compare to previous BUY trade
 *         if (trade.price > buy_price) wins++;
 *     }
 * }
 * double win_rate = (double)wins / num_trades;
 * ```
 * 
 * AVERAGE TRADE SIZE:
 * ```cpp
 * double total_value = 0;
 * for (auto& trade : trades) {
 *     total_value += trade.value;
 * }
 * double avg_trade = total_value / trades.size();
 * ```
 * 
 * TRADE FREQUENCY:
 * ```cpp
 * int days_in_backtest = price_data.size();
 * double trades_per_day = (double)trades.size() / days_in_backtest;
 * ```
 */
struct Trade {
    /**
     * @brief Date of trade execution (ISO format: YYYY-MM-DD)
     * 
     * Corresponds to date field in PriceBar where signal occurred.
     * Used for chronological trade sequencing and date-based analysis.
     * 
     * EXAMPLE: "2020-01-15", "2023-07-04"
     */
    std::string date;
    
    /**
     * @brief Signal that triggered this trade (BUY or SELL)
     * 
     * HOLD signals don't create trades (no execution).
     * Only BUY and SELL result in Trade records.
     * 
     * USAGE:
     * - BUY: Opening a position
     * - SELL: Closing a position
     */
    Signal signal;
    
    /**
     * @brief Execution price (typically close price of the bar)
     * 
     * ASSUMPTIONS:
     * - Trades execute at the close price of signal bar
     * - No slippage (get exact close price)
     * - No partial fills (entire order fills)
     * 
     * REALISM CONSIDERATIONS:
     * Real-world trading has:
     * - Slippage (worse price than expected)
     * - Market impact (large orders move price)
     * - Partial fills (order may not fill entirely)
     * 
     * ENHANCEMENT:
     * Could adjust price for slippage:
     * ```cpp
     * double slippage_bps = 5;  // 5 basis points
     * if (signal == Signal::BUY) {
     *     price *= (1 + slippage_bps / 10000);  // Pay more
     * } else {
     *     price *= (1 - slippage_bps / 10000);  // Receive less
     * }
     * ```
     */
    double price;
    
    /**
     * @brief Number of shares traded
     * 
     * BUY: Positive (shares purchased)
     * SELL: Positive (shares sold)
     * 
     * NOTE: Always stored as positive. Use signal to determine direction.
     * 
     * CALCULATION:
     * For BUY: shares = floor(available_cash / price)
     * For SELL: shares = current_shares_held
     * 
     * FRACTIONAL SHARES:
     * Current implementation uses integer shares only.
     * Modern brokers allow fractional shares, but adds complexity.
     */
    int shares;
    
    /**
     * @brief Absolute dollar value of trade (shares × price)
     * 
     * ALWAYS POSITIVE (absolute value, not signed).
     * 
     * BUY: Cost of purchase (cash outflow)
     * SELL: Proceeds from sale (cash inflow)
     * 
     * CALCULATION:
     * ```cpp
     * value = shares * price;
     * ```
     * 
     * USAGE:
     * - Calculate total capital deployed
     * - Analyze trade size distribution
     * - Verify cash accounting
     * 
     * EXAMPLE:
     * BUY 100 shares @ $50 → value = $5,000
     * SELL 100 shares @ $60 → value = $6,000
     */
    double value;
    
    /**
     * @brief Default constructor initializing to safe values
     * 
     * Creates empty trade record. Fields should be populated
     * before adding to trades vector.
     * 
     * USAGE:
     * ```cpp
     * Trade trade;
     * trade.date = price_bar.date;
     * trade.signal = Signal::BUY;
     * trade.price = price_bar.close;
     * trade.shares = shares_to_buy;
     * trade.value = shares_to_buy * price_bar.close;
     * trades.push_back(trade);
     * ```
     */
    Trade() : signal(Signal::HOLD), price(0), shares(0), value(0) {}
};

//==============================================================================
// SECTION 4: PortfolioState Structure - Position Tracking
//==============================================================================

/**
 * @struct PortfolioState
 * @brief Tracks portfolio composition and value at a point in time
 * 
 * PURPOSE:
 * Maintains accounting of cash, shares, and total portfolio value.
 * Used for:
 * - Determining trade feasibility (do we have cash to buy?)
 * - Calculating portfolio value over time
 * - Computing drawdown from peak value
 * - Generating equity curve for visualization
 * 
 * STATE TRANSITIONS:
 * 
 * INITIAL STATE:
 * - cash = initial_capital (e.g., $10,000)
 * - shares_held = 0
 * - portfolio_value = initial_capital
 * - peak_value = initial_capital
 * 
 * AFTER BUY:
 * - cash decreases by (shares_bought × price)
 * - shares_held increases by shares_bought
 * - portfolio_value = cash + (shares_held × current_price)
 * - peak_value = max(peak_value, portfolio_value)
 * 
 * AFTER SELL:
 * - cash increases by (shares_sold × price)
 * - shares_held decreases by shares_sold
 * - portfolio_value = cash + (shares_held × current_price)
 * - peak_value = max(peak_value, portfolio_value)
 * 
 * DAILY UPDATE (HOLD):
 * - cash unchanged
 * - shares_held unchanged
 * - portfolio_value = cash + (shares_held × current_price)
 * - peak_value = max(peak_value, portfolio_value)
 * 
 * EXAMPLE SIMULATION:
 * 
 * Day 0: Start with $10,000
 *   cash = $10,000, shares = 0, value = $10,000, peak = $10,000
 * 
 * Day 1: BUY signal, price = $100
 *   shares_to_buy = $10,000 / $100 = 100
 *   cash = $0, shares = 100, value = $10,000, peak = $10,000
 * 
 * Day 5: HOLD, price = $105
 *   cash = $0, shares = 100, value = $10,500, peak = $10,500
 * 
 * Day 10: HOLD, price = $103
 *   cash = $0, shares = 100, value = $10,300, peak = $10,500
 *   drawdown = ($10,500 - $10,300) / $10,500 = 1.9%
 * 
 * Day 15: SELL signal, price = $110
 *   proceeds = 100 × $110 = $11,000
 *   cash = $11,000, shares = 0, value = $11,000, peak = $11,000
 *   profit = $1,000 (10% return)
 * 
 * INVARIANT:
 * portfolio_value = cash + (shares_held × current_price)
 * 
 * This must hold at all times. Any violation indicates a bug in
 * trade execution or accounting logic.
 * 
 * VERIFICATION:
 * ```cpp
 * void verify_portfolio(const PortfolioState& port, double current_price) {
 *     double expected_value = port.cash + (port.shares_held * current_price);
 *     assert(abs(expected_value - port.portfolio_value) < 0.01);
 * }
 * ```
 */
struct PortfolioState {
    /**
     * @brief Cash available for trading
     * 
     * RULES:
     * - Decreases on BUY (pay for shares)
     * - Increases on SELL (receive proceeds)
     * - Unchanged on HOLD
     * - Must be >= 0 (no margin/borrowing in this implementation)
     * 
     * CALCULATION:
     * After BUY:  cash -= shares_bought × price
     * After SELL: cash += shares_sold × price
     * 
     * MARGIN TRADING (not implemented):
     * If margin were allowed, cash could go negative:
     * - Negative cash = borrowed from broker
     * - Pay interest on negative balance
     * - Subject to margin call if losses too large
     */
    double cash;
    
    /**
     * @brief Number of shares currently held
     * 
     * RULES:
     * - Increases on BUY
     * - Decreases on SELL
     * - Unchanged on HOLD
     * - Must be >= 0 (long-only, no short selling)
     * 
     * CALCULATION:
     * After BUY:  shares_held += shares_bought
     * After SELL: shares_held -= shares_sold (typically to 0)
     * 
     * SHORT SELLING (not implemented):
     * If short selling were allowed, shares could be negative:
     * - Negative shares = borrowed shares, must buy back later
     * - Pay borrow fee to lender
     * - Unlimited loss potential (price can rise infinitely)
     */
    int shares_held;
    
    /**
     * @brief Current total portfolio value (cash + shares × price)
     * 
     * FORMULA:
     * portfolio_value = cash + (shares_held × current_price)
     * 
     * UPDATED:
     * - After every trade execution
     * - At end of each trading day (mark-to-market)
     * 
     * USAGE:
     * - Calculate returns: (final_value - initial_value) / initial_value
     * - Generate equity curve for visualization
     * - Detect drawdowns from peak value
     * 
     * MARK-TO-MARKET:
     * Portfolio value changes daily even without trading due to
     * price fluctuations in held shares. This is unrealized gain/loss.
     * 
     * REALIZED vs UNREALIZED:
     * - Unrealized: Change in value of held shares
     * - Realized: Actual profit/loss when selling
     */
    double portfolio_value;
    
    /**
     * @brief Highest portfolio value achieved so far
     * 
     * PURPOSE:
     * Track all-time high for drawdown calculation.
     * 
     * UPDATES:
     * - Initialized to initial_capital
     * - Updated whenever portfolio_value > peak_value
     * - NEVER decreases (it's the highest value EVER achieved)
     * 
     * DRAWDOWN CALCULATION:
     * ```cpp
     * double current_drawdown = (peak_value - portfolio_value) / peak_value;
     * // Example: peak=$12,000, current=$10,800
     * // drawdown = ($12,000 - $10,800) / $12,000 = 10%
     * ```
     * 
     * MAX DRAWDOWN:
     * Maximum drawdown observed during entire backtest:
     * ```cpp
     * double max_dd = 0;
     * for (auto value : portfolio_history) {
     *     peak = max(peak, value);
     *     double dd = (peak - value) / peak;
     *     max_dd = max(max_dd, dd);
     * }
     * ```
     * 
     * INTERPRETATION:
     * - 0%: Portfolio at all-time high (no drawdown)
     * - 10%: Portfolio 10% below peak (moderate drawdown)
     * - 50%: Portfolio half of peak value (severe drawdown)
     * - 100%: Portfolio worthless (total loss - should never happen!)
     * 
     * RISK MANAGEMENT:
     * Max drawdown indicates worst-case loss an investor would experience.
     * Used to:
     * - Size positions (don't risk more than can tolerate)
     * - Compare strategies (prefer lower drawdown for same return)
     * - Set stop-loss levels (exit if drawdown exceeds threshold)
     */
    double peak_value;
    
    /**
     * @brief Constructor initializing portfolio with starting capital
     * 
     * @param initial_cash Starting capital for backtesting
     * 
     * TYPICAL VALUES:
     * - $10,000: Common for strategy testing
     * - $100,000: Typical retail account
     * - $1,000,000+: Institutional or high-net-worth
     * 
     * INITIALIZATION:
     * - All cash, no shares (start flat)
     * - Portfolio value = initial cash
     * - Peak value = initial cash (no drawdown yet)
     * 
     * EXAMPLE:
     * ```cpp
     * PortfolioState portfolio(10000);
     * // portfolio.cash = $10,000
     * // portfolio.shares_held = 0
     * // portfolio.portfolio_value = $10,000
     * // portfolio.peak_value = $10,000
     * ```
     */
    PortfolioState(double initial_cash) 
        : cash(initial_cash), 
          shares_held(0), 
          portfolio_value(initial_cash), 
          peak_value(initial_cash) {}
};

//==============================================================================
// SECTION 5: Strategy Base Class
//==============================================================================

/**
 * @class Strategy
 * @brief Abstract base class for implementing trading strategies
 * 
 * DESIGN PATTERN: Template Method + Strategy Pattern
 * 
 * ARCHITECTURE:
 * 
 * Base class provides:
 * - Common interface (generate_signals, backtest)
 * - Backtesting framework (trade execution, metrics)
 * - Utility methods (Sharpe ratio, drawdown calculation)
 * 
 * Derived classes provide:
 * - Strategy-specific signal generation (SMA, RSI, etc.)
 * - Custom parameter handling (if needed)
 * 
 * INHERITANCE HIERARCHY:
 * 
 *        Strategy (abstract base)
 *             │
 *     ┌───────┼───────┬─────────┐
 *     │       │       │         │
 *   SMA     RSI   MeanRev   Custom...
 * 
 * Each concrete strategy implements generate_signals() with its own logic.
 * 
 * USAGE PATTERN:
 * 
 * 1. CREATE STRATEGY:
 * ```cpp
 * std::unique_ptr<Strategy> strategy = 
 *     std::make_unique<SMAStrategy>("SMA_50_200");
 * ```
 * 
 * 2. CONFIGURE PARAMETERS:
 * ```cpp
 * JobParams params;
 * params.symbol = "AAPL";
 * params.start_date = "2020-01-01";
 * params.end_date = "2024-12-31";
 * params.strategy_params["fast_period"] = "50";
 * params.strategy_params["slow_period"] = "200";
 * strategy->set_parameters(params);
 * ```
 * 
 * 3. LOAD DATA:
 * ```cpp
 * CSVLoader loader;
 * std::vector<PriceBar> data = loader.load("AAPL.csv");
 * ```
 * 
 * 4. RUN BACKTEST:
 * ```cpp
 * JobResult result = strategy->backtest(data);
 * ```
 * 
 * 5. ANALYZE RESULTS:
 * ```cpp
 * std::cout << "Total Return: " << result.total_return << "%\n";
 * std::cout << "Sharpe Ratio: " << result.sharpe_ratio << "\n";
 * std::cout << "Max Drawdown: " << result.max_drawdown << "%\n";
 * ```
 * 
 * EXTENSION POINTS:
 * 
 * To create a new strategy:
 * 
 * ```cpp
 * class MyStrategy : public Strategy {
 * public:
 *     MyStrategy() : Strategy("MyStrategy") {}
 *     
 *     // Optional: Custom parameter handling
 *     void set_parameters(const JobParams& params) override {
 *         Strategy::set_parameters(params);
 *         // Extract custom parameters
 *         threshold_ = std::stod(params.strategy_params.at("threshold"));
 *     }
 *     
 *     // Required: Signal generation logic
 *     std::vector<Signal> generate_signals(
 *         const std::vector<PriceBar>& price_data) override 
 *     {
 *         std::vector<Signal> signals(price_data.size(), Signal::HOLD);
 *         
 *         // Your strategy logic here
 *         for (size_t i = lookback_; i < price_data.size(); ++i) {
 *             if (your_buy_condition(i, price_data)) {
 *                 signals[i] = Signal::BUY;
 *             } else if (your_sell_condition(i, price_data)) {
 *                 signals[i] = Signal::SELL;
 *             }
 *         }
 *         
 *         return signals;
 *     }
 *     
 * private:
 *     double threshold_;
 * };
 * ```
 * 
 * POLYMORPHISM:
 * 
 * Base pointer to derived object enables runtime strategy selection:
 * 
 * ```cpp
 * std::unique_ptr<Strategy> create_strategy(const std::string& type) {
 *     if (type == "SMA") {
 *         return std::make_unique<SMAStrategy>("SMA");
 *     } else if (type == "RSI") {
 *         return std::make_unique<RSIStrategy>("RSI");
 *     } else if (type == "MeanRev") {
 *         return std::make_unique<MeanRevStrategy>("MeanRev");
 *     } else {
 *         throw std::invalid_argument("Unknown strategy type");
 *     }
 * }
 * 
 * // Worker node determining which strategy to run
 * std::unique_ptr<Strategy> strategy = create_strategy(job.strategy_type);
 * strategy->set_parameters(job.params);
 * JobResult result = strategy->backtest(price_data);
 * ```
 * 
 * THREAD SAFETY:
 * 
 * Strategy objects are NOT thread-safe. Design assumes:
 * - Single thread per backtest
 * - No shared state between backtests
 * - Each worker creates own strategy instance
 * 
 * CORRECT (separate instances):
 * ```cpp
 * // Thread 1
 * Strategy* strat1 = new SMAStrategy();
 * strat1->backtest(data1);
 * 
 * // Thread 2
 * Strategy* strat2 = new SMAStrategy();
 * strat2->backtest(data2);
 * ```
 * 
 * INCORRECT (shared instance):
 * ```cpp
 * Strategy* shared = new SMAStrategy();
 * 
 * // Thread 1 and Thread 2 both use shared
 * // RACE CONDITION in params_, internal state, etc.
 * ```
 */
class Strategy {
protected:
    /**
     * @brief Strategy name for identification and logging
     * 
     * Used in reports and logs to identify which strategy produced results.
     * 
     * EXAMPLES: "SMA_50_200", "RSI_14", "MeanReversion"
     * 
     * NAMING CONVENTION:
     * Include strategy type and key parameters:
     * - "SMA_50_200": SMA crossover with 50 and 200 periods
     * - "RSI_14_30_70": RSI with 14-period and 30/70 thresholds
     */
    std::string name_;
    
    /**
     * @brief Job parameters containing strategy configuration
     * 
     * Set via set_parameters() before backtesting.
     * Contains:
     * - symbol: Stock symbol to backtest
     * - start_date, end_date: Backtest period
     * - initial_capital: Starting cash
     * - strategy_params: Map of strategy-specific parameters
     * 
     * EXAMPLE:
     * ```cpp
     * params_.symbol = "AAPL";
     * params_.start_date = "2020-01-01";
     * params_.end_date = "2024-12-31";
     * params_.initial_capital = 10000.0;
     * params_.strategy_params["fast_period"] = "50";
     * params_.strategy_params["slow_period"] = "200";
     * ```
     * 
     * ACCESS IN DERIVED CLASS:
     * ```cpp
     * int fast = std::stoi(params_.strategy_params.at("fast_period"));
     * int slow = std::stoi(params_.strategy_params.at("slow_period"));
     * ```
     */
    JobParams params_;
    
public:
    //==========================================================================
    // SECTION 5.1: Public Interface
    //==========================================================================
    
    /**
     * @brief Constructs strategy with given name
     * 
     * @param name Strategy identifier
     * 
     * USAGE:
     * ```cpp
     * class SMAStrategy : public Strategy {
     * public:
     *     SMAStrategy() : Strategy("SMA_Crossover") {}
     * };
     * ```
     */
    explicit Strategy(const std::string& name) : name_(name) {}
    
    /**
     * @brief Virtual destructor for proper cleanup of derived classes
     * 
     * IMPORTANCE:
     * Base class must have virtual destructor when using polymorphism.
     * Without it, deleting a derived object through base pointer causes
     * undefined behavior (derived destructor not called).
     * 
     * CORRECT:
     * ```cpp
     * Strategy* strat = new SMAStrategy();
     * delete strat;  // Calls ~SMAStrategy() then ~Strategy()
     * ```
     * 
     * WITHOUT virtual:
     * ```cpp
     * Strategy* strat = new SMAStrategy();
     * delete strat;  // Only calls ~Strategy(), memory leak!
     * ```
     */
    virtual ~Strategy() = default;
    
    /**
     * @brief Gets the strategy name
     * 
     * @return Strategy name for identification
     * 
     * USAGE:
     * ```cpp
     * std::cout << "Running strategy: " << strategy->name() << "\n";
     * ```
     */
    const std::string& name() const { return name_; }
    
    /**
     * @brief Sets parameters for this strategy
     * 
     * @param params Job parameters with configuration
     * 
     * CALLED BEFORE: backtest()
     * 
     * BASE IMPLEMENTATION:
     * Stores params in params_ member for access by derived classes.
     * 
     * OVERRIDE PATTERN:
     * Derived classes can override to extract custom parameters:
     * 
     * ```cpp
     * void SMAStrategy::set_parameters(const JobParams& params) {
     *     // Call base implementation first
     *     Strategy::set_parameters(params);
     *     
     *     // Extract strategy-specific parameters
     *     try {
     *         fast_period_ = std::stoi(
     *             params.strategy_params.at("fast_period"));
     *         slow_period_ = std::stoi(
     *             params.strategy_params.at("slow_period"));
     *     } catch (const std::exception& e) {
     *         // Use defaults if parameters missing
     *         fast_period_ = 50;
     *         slow_period_ = 200;
     *     }
     *     
     *     // Validate parameters
     *     if (fast_period_ >= slow_period_) {
     *         throw std::invalid_argument(
     *             "Fast period must be < slow period");
     *     }
     * }
     * ```
     * 
     * THREAD SAFETY:
     * Not thread-safe. Call once per backtest from single thread.
     */
    virtual void set_parameters(const JobParams& params) {
        params_ = params;
    }
    
    /**
     * @brief Generates trading signals from price data
     * 
     * PURE VIRTUAL: Must be implemented by derived classes
     * 
     * @param price_data Historical OHLCV data (time series)
     * 
     * @return Vector of signals (one per price bar)
     * 
     * REQUIREMENTS:
     * - Return vector must have same size as price_data
     * - signals[i] corresponds to price_data[i]
     * - Use only data up to index i to generate signals[i] (no lookahead!)
     * 
     * LOOKAHEAD BIAS:
     * 
     * INCORRECT (uses future data):
     * ```cpp
     * // BAD: Using price at i+1 to make decision at i
     * if (price_data[i+1].close > price_data[i].close) {
     *     signals[i] = Signal::BUY;  // Cheating!
     * }
     * ```
     * 
     * CORRECT (uses only past data):
     * ```cpp
     * // GOOD: Using prices up to and including i
     * if (sma_fast[i] > sma_slow[i]) {
     *     signals[i] = Signal::BUY;  // Valid
     * }
     * ```
     * 
     * EXAMPLE IMPLEMENTATION (SMA Crossover):
     * 
     * ```cpp
     * std::vector<Signal> SMAStrategy::generate_signals(
     *     const std::vector<PriceBar>& price_data) 
     * {
     *     std::vector<Signal> signals(price_data.size(), Signal::HOLD);
     *     
     *     // Calculate moving averages
     *     std::vector<double> fast_ma = calculate_sma(price_data, fast_period_);
     *     std::vector<double> slow_ma = calculate_sma(price_data, slow_period_);
     *     
     *     // Generate signals based on crossovers
     *     for (size_t i = slow_period_; i < price_data.size(); ++i) {
     *         // Bullish crossover: fast MA crosses above slow MA
     *         if (fast_ma[i] > slow_ma[i] && fast_ma[i-1] <= slow_ma[i-1]) {
     *             signals[i] = Signal::BUY;
     *         }
     *         // Bearish crossover: fast MA crosses below slow MA
     *         else if (fast_ma[i] < slow_ma[i] && fast_ma[i-1] >= slow_ma[i-1]) {
     *             signals[i] = Signal::SELL;
     *         }
     *     }
     *     
     *     return signals;
     * }
     * ```
     * 
     * PERFORMANCE:
     * - Should be O(n) where n = price_data.size()
     * - Vectorize operations when possible (process entire series)
     * - Cache intermediate calculations (moving averages, indicators)
     * 
     * WARM-UP PERIOD:
     * 
     * Most strategies need warm-up data before generating valid signals:
     * - SMA(200): Needs 200 bars before first valid signal
     * - RSI(14): Needs 14 bars minimum
     * 
     * First N signals should be HOLD until enough data:
     * ```cpp
     * for (size_t i = 0; i < lookback_period; ++i) {
     *     signals[i] = Signal::HOLD;  // Not enough data yet
     * }
     * ```
     */
    virtual std::vector<Signal> generate_signals(
        const std::vector<PriceBar>& price_data) = 0;
    
    /**
     * @brief Runs backtest simulation on historical data
     * 
     * FRAMEWORK METHOD: Implemented in base class
     * 
     * @param price_data Historical price bars for backtesting
     * 
     * @return JobResult with performance metrics and trade history
     * 
     * ALGORITHM:
     * 
     * 1. GENERATE SIGNALS:
     * ```cpp
     * std::vector<Signal> signals = generate_signals(price_data);
     * ```
     * 
     * 2. INITIALIZE PORTFOLIO:
     * ```cpp
     * PortfolioState portfolio(params_.initial_capital);
     * std::vector<Trade> trades;
     * std::vector<double> portfolio_values;
     * ```
     * 
     * 3. SIMULATE TRADING:
     * ```cpp
     * for (size_t i = 0; i < price_data.size(); ++i) {
     *     double price = price_data[i].close;
     *     
     *     if (signals[i] == Signal::BUY && portfolio.shares_held == 0) {
     *         // Execute buy
     *         int shares = portfolio.cash / price;
     *         portfolio.cash -= shares * price;
     *         portfolio.shares_held += shares;
     *         
     *         // Record trade
     *         Trade trade;
     *         trade.date = price_data[i].date;
     *         trade.signal = Signal::BUY;
     *         trade.price = price;
     *         trade.shares = shares;
     *         trade.value = shares * price;
     *         trades.push_back(trade);
     *     }
     *     else if (signals[i] == Signal::SELL && portfolio.shares_held > 0) {
     *         // Execute sell
     *         double proceeds = portfolio.shares_held * price;
     *         portfolio.cash += proceeds;
     *         
     *         // Record trade
     *         Trade trade;
     *         trade.date = price_data[i].date;
     *         trade.signal = Signal::SELL;
     *         trade.price = price;
     *         trade.shares = portfolio.shares_held;
     *         trade.value = proceeds;
     *         trades.push_back(trade);
     *         
     *         portfolio.shares_held = 0;
     *     }
     *     
     *     // Update portfolio value (mark-to-market)
     *     portfolio.portfolio_value = portfolio.cash + 
     *                                 (portfolio.shares_held * price);
     *     portfolio.peak_value = std::max(portfolio.peak_value, 
     *                                     portfolio.portfolio_value);
     *     portfolio_values.push_back(portfolio.portfolio_value);
     * }
     * ```
     * 
     * 4. CALCULATE METRICS:
     * ```cpp
     * double total_return = ((portfolio.portfolio_value - initial_capital) / 
     *                       initial_capital) * 100;
     * 
     * std::vector<double> returns = calculate_returns(portfolio_values);
     * double sharpe = calculate_sharpe_ratio(returns);
     * double max_dd = calculate_max_drawdown(portfolio_values);
     * ```
     * 
     * 5. PACKAGE RESULTS:
     * ```cpp
     * JobResult result;
     * result.total_return = total_return;
     * result.sharpe_ratio = sharpe;
     * result.max_drawdown = max_dd * 100;  // Convert to percentage
     * result.num_trades = trades.size();
     * result.final_value = portfolio.portfolio_value;
     * result.trades = trades;
     * return result;
     * ```
     * 
     * RETURN VALUE:
     * JobResult contains:
     * - total_return: Overall percentage return
     * - sharpe_ratio: Risk-adjusted return metric
     * - max_drawdown: Worst peak-to-trough decline
     * - num_trades: Total trades executed
     * - final_value: Ending portfolio value
     * - trades: Complete trade history
     * 
     * USAGE:
     * ```cpp
     * CSVLoader loader;
     * std::vector<PriceBar> data = loader.load("AAPL.csv");
     * 
     * SMAStrategy strategy;
     * strategy.set_parameters(params);
     * 
     * JobResult result = strategy.backtest(data);
     * 
     * std::cout << "Strategy: " << strategy.name() << "\n";
     * std::cout << "Return: " << result.total_return << "%\n";
     * std::cout << "Sharpe: " << result.sharpe_ratio << "\n";
     * std::cout << "Max DD: " << result.max_drawdown << "%\n";
     * std::cout << "Trades: " << result.num_trades << "\n";
     * ```
     * 
     * THREAD SAFETY:
     * Not thread-safe. Each backtest should run on separate strategy instance.
     */
    JobResult backtest(const std::vector<PriceBar>& price_data);
    
protected:
    //==========================================================================
    // SECTION 5.2: Protected Helper Methods
    //==========================================================================
    
    /**
     * @brief Calculates Sharpe ratio from returns series
     * 
     * SHARPE RATIO FORMULA:
     * ```
     * SR = (mean_return - risk_free_rate) / std_dev_return
     * ```
     * 
     * INTERPRETATION:
     * - SR > 1.0: Good risk-adjusted return
     * - SR > 2.0: Excellent (top-tier strategies)
     * - SR > 3.0: Outstanding (rare, verify not curve-fitted)
     * - SR < 0: Losing strategy (return < risk-free rate)
     * 
     * @param returns Vector of period returns (e.g., daily returns)
     * @param risk_free_rate Annualized risk-free rate (default 0.0)
     * 
     * @return Sharpe ratio (unitless)
     * 
     * CALCULATION STEPS:
     * 
     * 1. Calculate mean return:
     * ```cpp
     * double sum = 0;
     * for (double r : returns) sum += r;
     * double mean = sum / returns.size();
     * ```
     * 
     * 2. Calculate standard deviation:
     * ```cpp
     * double variance = 0;
     * for (double r : returns) {
     *     variance += (r - mean) * (r - mean);
     * }
     * variance /= returns.size();
     * double std_dev = sqrt(variance);
     * ```
     * 
     * 3. Calculate Sharpe ratio:
     * ```cpp
     * double sharpe = (mean - risk_free_rate) / std_dev;
     * ```
     * 
     * ANNUALIZATION:
     * If returns are daily, Sharpe ratio is typically annualized:
     * ```cpp
     * double annual_sharpe = sharpe * sqrt(252);  // 252 trading days/year
     * ```
     * 
     * EXAMPLE:
     * ```cpp
     * std::vector<double> returns = {0.01, -0.005, 0.02, 0.015, -0.01};
     * double sharpe = calculate_sharpe_ratio(returns, 0.02/252);
     * std::cout << "Sharpe Ratio: " << sharpe << "\n";
     * ```
     * 
     * LIMITATIONS:
     * - Assumes returns are normally distributed (not always true)
     * - Doesn't distinguish upside vs downside volatility
     * - Can be gamed with infrequent large losses
     * 
     * ALTERNATIVES:
     * - Sortino ratio: Uses only downside deviation
     * - Calmar ratio: Return / max drawdown
     * - Information ratio: Excess return / tracking error
     */
    double calculate_sharpe_ratio(const std::vector<double>& returns,
                                  double risk_free_rate = 0.0) const;
    
    /**
     * @brief Calculates maximum drawdown from portfolio values
     * 
     * MAX DRAWDOWN FORMULA:
     * ```
     * MDD = max_over_time((peak_value - current_value) / peak_value)
     * ```
     * 
     * INTERPRETATION:
     * - 0%: No drawdown (portfolio always at all-time high)
     * - 10%: Moderate (typical for diversified portfolios)
     * - 20%: Significant (requires strong conviction to hold through)
     * - 50%: Severe (half of capital lost from peak)
     * - 100%: Total loss (should never happen with risk management)
     * 
     * @param portfolio_values Time series of portfolio values
     * 
     * @return Maximum drawdown as decimal (0.1 = 10% drawdown)
     * 
     * CALCULATION:
     * 
     * ```cpp
     * double max_drawdown = 0;
     * double peak = portfolio_values[0];
     * 
     * for (double value : portfolio_values) {
     *     // Update peak if new high
     *     if (value > peak) {
     *         peak = value;
     *     }
     *     
     *     // Calculate current drawdown
     *     double drawdown = (peak - value) / peak;
     *     
     *     // Update max drawdown if worse
     *     if (drawdown > max_drawdown) {
     *         max_drawdown = drawdown;
     *     }
     * }
     * 
     * return max_drawdown;
     * ```
     * 
     * EXAMPLE:
     * ```
     * Portfolio values: [$10,000, $11,000, $10,500, $12,000, $9,000]
     * 
     * Day 0: value=$10,000, peak=$10,000, dd=0%
     * Day 1: value=$11,000, peak=$11,000, dd=0% (new high)
     * Day 2: value=$10,500, peak=$11,000, dd=4.5%
     * Day 3: value=$12,000, peak=$12,000, dd=0% (new high)
     * Day 4: value=$9,000,  peak=$12,000, dd=25% ← MAX DRAWDOWN
     * 
     * Result: 25% maximum drawdown
     * ```
     * 
     * USAGE:
     * ```cpp
     * std::vector<double> values = {10000, 11000, 10500, 12000, 9000};
     * double max_dd = calculate_max_drawdown(values);
     * std::cout << "Max Drawdown: " << (max_dd * 100) << "%\n";
     * // Output: Max Drawdown: 25%
     * ```
     * 
     * RISK MANAGEMENT:
     * Max drawdown informs:
     * - Position sizing: Risk per trade = account × (1 - max_tolerable_dd)
     * - Stop-loss placement: Exit if drawdown exceeds threshold
     * - Strategy selection: Prefer lower drawdown for same return
     * 
     * RECOVERY TIME:
     * Consider how long to recover from max drawdown:
     * - 10% dd requires 11.1% gain to recover
     * - 50% dd requires 100% gain to recover
     * - Deeper drawdowns require exponentially more time
     */
    double calculate_max_drawdown(const std::vector<double>& portfolio_values) const;
};

} // namespace backtesting

#endif // STRATEGY_H

//==============================================================================
// SECTION 6: Usage Examples and Patterns
//==============================================================================

/**
 * EXAMPLE 1: Implementing Simple Moving Average Crossover Strategy
 * 
 * ```cpp
 * #include "strategy/strategy.h"
 * #include <cmath>
 * 
 * class SMAStrategy : public Strategy {
 * private:
 *     int fast_period_;
 *     int slow_period_;
 *     
 *     // Calculate Simple Moving Average
 *     std::vector<double> calculate_sma(
 *         const std::vector<PriceBar>& data, 
 *         int period) const 
 *     {
 *         std::vector<double> sma(data.size(), 0);
 *         
 *         for (size_t i = period - 1; i < data.size(); ++i) {
 *             double sum = 0;
 *             for (int j = 0; j < period; ++j) {
 *                 sum += data[i - j].close;
 *             }
 *             sma[i] = sum / period;
 *         }
 *         
 *         return sma;
 *     }
 *     
 * public:
 *     SMAStrategy() : Strategy("SMA_Crossover") {}
 *     
 *     void set_parameters(const JobParams& params) override {
 *         Strategy::set_parameters(params);
 *         
 *         // Extract parameters with defaults
 *         fast_period_ = params.strategy_params.count("fast_period") ?
 *             std::stoi(params.strategy_params.at("fast_period")) : 50;
 *         slow_period_ = params.strategy_params.count("slow_period") ?
 *             std::stoi(params.strategy_params.at("slow_period")) : 200;
 *     }
 *     
 *     std::vector<Signal> generate_signals(
 *         const std::vector<PriceBar>& price_data) override 
 *     {
 *         std::vector<Signal> signals(price_data.size(), Signal::HOLD);
 *         
 *         // Need at least slow_period bars
 *         if (price_data.size() < slow_period_) {
 *             return signals;
 *         }
 *         
 *         // Calculate moving averages
 *         std::vector<double> fast_ma = calculate_sma(price_data, fast_period_);
 *         std::vector<double> slow_ma = calculate_sma(price_data, slow_period_);
 *         
 *         // Generate signals based on crossovers
 *         for (size_t i = slow_period_; i < price_data.size(); ++i) {
 *             // Bullish crossover
 *             if (fast_ma[i] > slow_ma[i] && 
 *                 fast_ma[i-1] <= slow_ma[i-1]) {
 *                 signals[i] = Signal::BUY;
 *             }
 *             // Bearish crossover
 *             else if (fast_ma[i] < slow_ma[i] && 
 *                     fast_ma[i-1] >= slow_ma[i-1]) {
 *                 signals[i] = Signal::SELL;
 *             }
 *         }
 *         
 *         return signals;
 *     }
 * };
 * ```
 */

/**
 * EXAMPLE 2: Running Backtest with Custom Strategy
 * 
 * ```cpp
 * #include "strategy/strategy.h"
 * #include "data/csv_loader.h"
 * #include <iostream>
 * 
 * int main() {
 *     // Load historical data
 *     CSVLoader loader;
 *     std::vector<PriceBar> data = loader.load_csv("AAPL.csv");
 *     
 *     // Create strategy
 *     auto strategy = std::make_unique<SMAStrategy>();
 *     
 *     // Configure parameters
 *     JobParams params;
 *     params.symbol = "AAPL";
 *     params.start_date = "2020-01-01";
 *     params.end_date = "2024-12-31";
 *     params.initial_capital = 10000.0;
 *     params.strategy_params["fast_period"] = "50";
 *     params.strategy_params["slow_period"] = "200";
 *     
 *     strategy->set_parameters(params);
 *     
 *     // Run backtest
 *     std::cout << "Running backtest for " << strategy->name() << "...\n";
 *     JobResult result = strategy->backtest(data);
 *     
 *     // Display results
 *     std::cout << "\n=== BACKTEST RESULTS ===\n";
 *     std::cout << "Strategy: " << strategy->name() << "\n";
 *     std::cout << "Symbol: " << params.symbol << "\n";
 *     std::cout << "Period: " << params.start_date << " to " 
 *               << params.end_date << "\n";
 *     std::cout << "Initial Capital: $" << params.initial_capital << "\n";
 *     std::cout << "Final Value: $" << result.final_value << "\n";
 *     std::cout << "Total Return: " << result.total_return << "%\n";
 *     std::cout << "Sharpe Ratio: " << result.sharpe_ratio << "\n";
 *     std::cout << "Max Drawdown: " << result.max_drawdown << "%\n";
 *     std::cout << "Number of Trades: " << result.num_trades << "\n";
 *     
 *     // Show trade history
 *     std::cout << "\n=== TRADE HISTORY ===\n";
 *     for (const auto& trade : result.trades) {
 *         std::string action = (trade.signal == Signal::BUY) ? "BUY" : "SELL";
 *         std::cout << trade.date << " " << action << " " 
 *                   << trade.shares << " shares @ $" << trade.price 
 *                   << " (value: $" << trade.value << ")\n";
 *     }
 *     
 *     return 0;
 * }
 * ```
 */

/**
 * EXAMPLE 3: Comparing Multiple Strategies
 * 
 * ```cpp
 * #include "strategy/strategy.h"
 * #include <vector>
 * #include <memory>
 * 
 * struct StrategyResult {
 *     std::string name;
 *     double total_return;
 *     double sharpe_ratio;
 *     double max_drawdown;
 * };
 * 
 * std::vector<StrategyResult> compare_strategies(
 *     const std::vector<PriceBar>& data,
 *     const JobParams& base_params) 
 * {
 *     std::vector<std::unique_ptr<Strategy>> strategies;
 *     
 *     // Add strategies to compare
 *     strategies.push_back(std::make_unique<SMAStrategy>());
 *     strategies.push_back(std::make_unique<RSIStrategy>());
 *     strategies.push_back(std::make_unique<MeanRevStrategy>());
 *     
 *     std::vector<StrategyResult> results;
 *     
 *     for (auto& strategy : strategies) {
 *         // Set parameters
 *         strategy->set_parameters(base_params);
 *         
 *         // Run backtest
 *         JobResult result = strategy->backtest(data);
 *         
 *         // Store results
 *         StrategyResult sr;
 *         sr.name = strategy->name();
 *         sr.total_return = result.total_return;
 *         sr.sharpe_ratio = result.sharpe_ratio;
 *         sr.max_drawdown = result.max_drawdown;
 *         results.push_back(sr);
 *     }
 *     
 *     return results;
 * }
 * 
 * int main() {
 *     CSVLoader loader;
 *     auto data = loader.load_csv("AAPL.csv");
 *     
 *     JobParams params;
 *     params.symbol = "AAPL";
 *     params.initial_capital = 10000.0;
 *     
 *     auto results = compare_strategies(data, params);
 *     
 *     std::cout << "=== STRATEGY COMPARISON ===\n";
 *     std::cout << std::left << std::setw(20) << "Strategy"
 *               << std::setw(12) << "Return %"
 *               << std::setw(12) << "Sharpe"
 *               << std::setw(12) << "Max DD %\n";
 *     std::cout << std::string(56, '-') << "\n";
 *     
 *     for (const auto& r : results) {
 *         std::cout << std::left << std::setw(20) << r.name
 *                   << std::setw(12) << r.total_return
 *                   << std::setw(12) << r.sharpe_ratio
 *                   << std::setw(12) << r.max_drawdown << "\n";
 *     }
 *     
 *     return 0;
 * }
 * ```
 */

//==============================================================================
// SECTION 7: Implementing Custom Strategies
//==============================================================================

/**
 * TEMPLATE FOR CUSTOM STRATEGY IMPLEMENTATION
 * 
 * ```cpp
 * #include "strategy/strategy.h"
 * 
 * class MyCustomStrategy : public Strategy {
 * private:
 *     // Strategy-specific parameters
 *     double threshold_;
 *     int lookback_;
 *     
 *     // Helper methods for indicator calculation
 *     double calculate_indicator(
 *         const std::vector<PriceBar>& data, 
 *         size_t index) const 
 *     {
 *         // Your indicator calculation here
 *         // Example: RSI, MACD, Bollinger Bands, etc.
 *         return 0.0;
 *     }
 *     
 *     bool buy_condition(
 *         const std::vector<PriceBar>& data, 
 *         size_t index) const 
 *     {
 *         // Your buy logic here
 *         // Return true when conditions met for buying
 *         return false;
 *     }
 *     
 *     bool sell_condition(
 *         const std::vector<PriceBar>& data, 
 *         size_t index) const 
 *     {
 *         // Your sell logic here
 *         // Return true when conditions met for selling
 *         return false;
 *     }
 *     
 * public:
 *     MyCustomStrategy() 
 *         : Strategy("MyCustomStrategy"),
 *           threshold_(0.0),
 *           lookback_(20) {}
 *     
 *     void set_parameters(const JobParams& params) override {
 *         // Call base implementation
 *         Strategy::set_parameters(params);
 *         
 *         // Extract custom parameters
 *         if (params.strategy_params.count("threshold")) {
 *             threshold_ = std::stod(
 *                 params.strategy_params.at("threshold"));
 *         }
 *         
 *         if (params.strategy_params.count("lookback")) {
 *             lookback_ = std::stoi(
 *                 params.strategy_params.at("lookback"));
 *         }
 *         
 *         // Validate parameters
 *         if (threshold_ < 0 || threshold_ > 1) {
 *             throw std::invalid_argument(
 *                 "Threshold must be between 0 and 1");
 *         }
 *         
 *         if (lookback_ < 1) {
 *             throw std::invalid_argument(
 *                 "Lookback must be positive");
 *         }
 *     }
 *     
 *     std::vector<Signal> generate_signals(
 *         const std::vector<PriceBar>& price_data) override 
 *     {
 *         std::vector<Signal> signals(price_data.size(), Signal::HOLD);
 *         
 *         // Need minimum data for lookback
 *         if (price_data.size() < lookback_) {
 *             return signals;
 *         }
 *         
 *         // Generate signals
 *         for (size_t i = lookback_; i < price_data.size(); ++i) {
 *             if (buy_condition(price_data, i)) {
 *                 signals[i] = Signal::BUY;
 *             } else if (sell_condition(price_data, i)) {
 *                 signals[i] = Signal::SELL;
 *             }
 *             // Otherwise remains HOLD (default)
 *         }
 *         
 *         return signals;
 *     }
 * };
 * ```
 */

/**
 * STRATEGY DESIGN CHECKLIST
 * 
 * ✓ INHERITANCE:
 *   - Inherit from Strategy base class
 *   - Call base constructor with strategy name
 *   
 * ✓ PARAMETERS:
 *   - Override set_parameters() if custom params needed
 *   - Call Strategy::set_parameters(params) first
 *   - Extract params with error handling
 *   - Validate param values
 *   
 * ✓ SIGNAL GENERATION:
 *   - Implement generate_signals() (pure virtual)
 *   - Return vector same size as input
 *   - Initialize all signals to HOLD
 *   - Handle insufficient data gracefully
 *   - Avoid lookahead bias (only use past data)
 *   
 * ✓ INDICATORS:
 *   - Create helper methods for indicator calculations
 *   - Cache results if used multiple times
 *   - Handle edge cases (empty data, NaN values)
 *   
 * ✓ ENTRY/EXIT LOGIC:
 *   - Separate buy_condition() and sell_condition()
 *   - Clear, testable conditions
 *   - Document the strategy logic
 *   
 * ✓ PERFORMANCE:
 *   - O(n) complexity preferred
 *   - Vectorize when possible
 *   - Minimize redundant calculations
 *   
 * ✓ TESTING:
 *   - Unit test indicator calculations
 *   - Verify no lookahead bias
 *   - Test with edge cases (few bars, no signals, etc.)
 *   - Compare results with manual calculation
 */

//==============================================================================
// SECTION 8: Performance Metrics Explained
//==============================================================================

/**
 * TOTAL RETURN
 * 
 * FORMULA:
 * ```
 * Total Return (%) = ((Final Value - Initial Capital) / Initial Capital) × 100
 * ```
 * 
 * INTERPRETATION:
 * - Measures absolute profit/loss
 * - Does not account for risk
 * - Should compare to benchmark (e.g., buy-and-hold)
 * 
 * EXAMPLE:
 * Initial: $10,000
 * Final: $12,000
 * Return: (12000 - 10000) / 10000 = 20%
 * 
 * ANNUALIZATION:
 * ```
 * Years = days_in_backtest / 365.25
 * Annual Return = (1 + total_return) ^ (1/years) - 1
 * ```
 * 
 * Example: 20% over 2 years
 * Annual: (1.20)^(1/2) - 1 = 9.5% per year
 */

/**
 * SHARPE RATIO
 * 
 * FORMULA:
 * ```
 * Sharpe Ratio = (Mean Return - Risk Free Rate) / Std Dev of Returns
 * ```
 * 
 * INTERPRETATION:
 * - Measures risk-adjusted return
 * - Higher is better
 * - Accounts for volatility
 * 
 * BENCHMARK VALUES:
 * - < 0: Losing money (worse than risk-free)
 * - 0 to 1: Sub-par (not worth the risk)
 * - 1 to 2: Good (acceptable risk-adjusted return)
 * - 2 to 3: Very good (professional grade)
 * - > 3: Excellent (top-tier, verify not overfit)
 * 
 * LIMITATIONS:
 * - Assumes normal distribution of returns
 * - Penalizes upside volatility equally with downside
 * - Can be manipulated with infrequent trades
 * 
 * ALTERNATIVES:
 * - Sortino Ratio: Uses downside deviation only
 * - Information Ratio: Excess return vs benchmark
 * - Omega Ratio: Probability-weighted gains/losses
 */

/**
 * MAXIMUM DRAWDOWN
 * 
 * FORMULA:
 * ```
 * MDD (%) = max_over_time((Peak - Current) / Peak) × 100
 * ```
 * 
 * INTERPRETATION:
 * - Worst peak-to-trough decline
 * - Measures downside risk
 * - Lower is better
 * 
 * INVESTOR PSYCHOLOGY:
 * - 10% DD: Uncomfortable but tolerable
 * - 20% DD: Significant anxiety, some investors exit
 * - 30% DD: Major stress, many investors abandon strategy
 * - 50% DD: Devastating, requires 100% gain to recover
 * 
 * RECOVERY TIME:
 * Time from trough back to previous peak
 * - Important for investor patience
 * - Long recoveries test conviction
 * 
 * POSITION SIZING:
 * ```
 * Max Position Size = Capital × (1 - Max Tolerable DD)
 * 
 * Example: $100k capital, 20% max tolerable DD
 * Max position = $100k × (1 - 0.20) = $80k
 * Keep $20k reserve for drawdown protection
 * ```
 */

/**
 * OTHER IMPORTANT METRICS (not in JobResult but useful)
 * 
 * WIN RATE:
 * ```
 * Win Rate = Winning Trades / Total Trades
 * ```
 * Common misconception: High win rate = good strategy
 * Reality: Many profitable strategies have <50% win rate
 * 
 * PROFIT FACTOR:
 * ```
 * Profit Factor = Gross Profit / Gross Loss
 * ```
 * Above 1.0 is profitable, above 2.0 is excellent
 * 
 * AVERAGE WIN / AVERAGE LOSS:
 * ```
 * Expectancy = (Win Rate × Avg Win) - (Loss Rate × Avg Loss)
 * ```
 * Positive expectancy required for profitability
 * 
 * CALMAR RATIO:
 * ```
 * Calmar = Annual Return / Max Drawdown
 * ```
 * Higher is better, measures return per unit of drawdown risk
 */

//==============================================================================
// SECTION 9: Common Pitfalls and Solutions
//==============================================================================

/**
 * PITFALL 1: Lookahead Bias
 * 
 * PROBLEM:
 * Using future data to make current decisions
 * 
 * ```cpp
 * // WRONG: Using tomorrow's price to decide today
 * if (price_data[i+1].close > price_data[i].close) {
 *     signals[i] = Signal::BUY;  // Cheating!
 * }
 * ```
 * 
 * CONSEQUENCE:
 * Unrealistically high backtest returns. Strategy fails in live trading.
 * 
 * SOLUTION:
 * Only use data up to and including current bar
 * 
 * ```cpp
 * // CORRECT: Using only past data
 * if (moving_average[i] > moving_average[i-1]) {
 *     signals[i] = Signal::BUY;  // Valid
 * }
 * ```
 */

/**
 * PITFALL 2: Curve Fitting / Overfitting
 * 
 * PROBLEM:
 * Optimizing parameters to perfectly fit historical data
 * 
 * EXAMPLE:
 * Testing 100 different parameter combinations, picking best performer
 * 
 * CONSEQUENCE:
 * Strategy memorizes past data but fails on new data (poor generalization)
 * 
 * SOLUTION:
 * - Use walk-forward analysis (train on period 1, test on period 2)
 * - Out-of-sample testing (hold back data for final validation)
 * - Limit parameter optimization (fewer free parameters)
 * - Require statistical significance (not just lucky)
 * 
 * ```cpp
 * // Divide data into train/test
 * size_t split = price_data.size() * 0.7;  // 70% train, 30% test
 * 
 * auto train_data = slice(price_data, 0, split);
 * auto test_data = slice(price_data, split, price_data.size());
 * 
 * // Optimize on train data only
 * auto best_params = optimize_parameters(train_data);
 * 
 * // Verify on test data
 * strategy->set_parameters(best_params);
 * auto result = strategy->backtest(test_data);
 * ```
 */

/**
 * PITFALL 3: Ignoring Transaction Costs
 * 
 * PROBLEM:
 * Not accounting for commissions, slippage, spreads
 * 
 * CONSEQUENCE:
 * Overestimated returns, especially for high-frequency strategies
 * 
 * SOLUTION:
 * Model realistic costs
 * 
 * ```cpp
 * // Commission: $0.005 per share
 * // Slippage: 0.05% of trade value
 * 
 * double commission = shares * 0.005;
 * double slippage = trade_value * 0.0005;
 * double total_cost = commission + slippage;
 * 
 * // Adjust cash accordingly
 * if (signal == Signal::BUY) {
 *     cash -= (trade_value + total_cost);
 * } else {
 *     cash += (trade_value - total_cost);
 * }
 * ```
 */

/**
 * PITFALL 4: Insufficient Data
 * 
 * PROBLEM:
 * Backtesting on too short a time period
 * 
 * ```cpp
 * // Only 6 months of data
 * auto data = load_data("2024-01-01", "2024-06-30");
 * ```
 * 
 * CONSEQUENCE:
 * Results not statistically significant, may work in bull market but fail in bear
 * 
 * SOLUTION:
 * Test on multiple years including different market conditions
 * 
 * ```cpp
 * // At least 3-5 years, multiple market cycles
 * auto data = load_data("2019-01-01", "2024-12-31");
 * // Includes: 2020 crash, 2020-2021 bull, 2022 bear, 2023 recovery
 * ```
 */

/**
 * PITFALL 5: Survivorship Bias
 * 
 * PROBLEM:
 * Only testing on stocks that still exist today
 * 
 * EXAMPLE:
 * Testing strategy on current S&P 500 constituents going back 10 years
 * 
 * CONSEQUENCE:
 * Ignores bankrupt/delisted companies (losers), inflates returns
 * 
 * SOLUTION:
 * Use survivorship-bias-free dataset including delisted stocks
 * 
 * ```cpp
 * // Include delisting dates and handle appropriately
 * if (stock.delisted && date > stock.delisting_date) {
 *     // Stock no longer tradeable
 *     portfolio.sell_all(stock);
 * }
 * ```
 */

/**
 * PITFALL 6: Ignoring Dividends
 * 
 * PROBLEM:
 * Not accounting for dividend payments
 * 
 * CONSEQUENCE:
 * Underestimated returns, especially for dividend stocks
 * 
 * SOLUTION:
 * Use adjusted prices or add dividend cash flows
 * 
 * ```cpp
 * // On dividend payment date
 * if (ex_dividend_date) {
 *     double dividend_per_share = get_dividend(stock, date);
 *     cash += shares_held * dividend_per_share;
 *     
 *     // Price drops by dividend amount (already in data if adjusted)
 * }
 * ```
 */

/**
 * PITFALL 7: Strategy Parameter in signal range
 * 
 * PROBLEM:
 * Returning signals vector with wrong size
 * 
 * ```cpp
 * // WRONG: Returning fewer signals than price bars
 * std::vector<Signal> signals(price_data.size() - lookback_);
 * ```
 * 
 * CONSEQUENCE:
 * Index mismatch, crash or incorrect trades
 * 
 * SOLUTION:
 * Always return same size, fill early bars with HOLD
 * 
 * ```cpp
 * // CORRECT: Same size as input
 * std::vector<Signal> signals(price_data.size(), Signal::HOLD);
 * 
 * // Generate signals only where we have enough data
 * for (size_t i = lookback_; i < price_data.size(); ++i) {
 *     // Generate signal for bar i
 * }
 * ```
 */

//==============================================================================
// SECTION 10: FAQ
//==============================================================================

/**
 * Q1: Why use Signal enum instead of bool or int?
 * 
 * A: Type safety and clarity. Signal::BUY is more readable than true or 1.
 *    The enum prevents accidental misuse (can't pass random int as Signal).
 *    The numeric values (0, 1, -1) allow mathematical operations when needed.
 */

/**
 * Q2: Can I short sell in this framework?
 * 
 * A: Not in current implementation (long-only). To add short selling:
 *    - Allow negative shares_held (borrowed shares)
 *    - Add borrow costs (fee to lender)
 *    - Track short positions separately
 *    - Implement margin requirements
 *    - Handle unlimited loss potential
 *    
 *    Short selling significantly complicates backtesting logic.
 */

/**
 * Q3: How do I implement position sizing?
 * 
 * A: Current implementation uses all available cash (max position).
 *    For partial positions, modify trade execution:
 *    
 *    ```cpp
 *    // Fixed percentage of capital
 *    double position_size = 0.20;  // Use 20% of capital per trade
 *    int shares = (cash * position_size) / price;
 *    
 *    // Risk-based sizing
 *    double risk_per_trade = 100;  // Risk $100 per trade
 *    double stop_distance = entry_price - stop_loss_price;
 *    int shares = risk_per_trade / stop_distance;
 *    
 *    // Kelly Criterion
 *    double win_rate = 0.60;
 *    double avg_win = 150;
 *    double avg_loss = 100;
 *    double kelly = (win_rate * avg_win - (1-win_rate) * avg_loss) / avg_win;
 *    int shares = (cash * kelly) / price;
 *    ```
 */

/**
 * Q4: How do I add stop-loss and take-profit?
 * 
 * A: Extend signal generation to include exit logic:
 *    
 *    ```cpp
 *    // Track entry price
 *    double entry_price = 0;
 *    bool in_position = false;
 *    
 *    for (size_t i = 0; i < price_data.size(); ++i) {
 *        double price = price_data[i].close;
 *        
 *        if (!in_position && buy_condition(i)) {
 *            signals[i] = Signal::BUY;
 *            entry_price = price;
 *            in_position = true;
 *        }
 *        else if (in_position) {
 *            // Stop loss (2% below entry)
 *            if (price < entry_price * 0.98) {
 *                signals[i] = Signal::SELL;
 *                in_position = false;
 *            }
 *            // Take profit (5% above entry)
 *            else if (price > entry_price * 1.05) {
 *                signals[i] = Signal::SELL;
 *                in_position = false;
 *            }
 *            // Regular exit condition
 *            else if (sell_condition(i)) {
 *                signals[i] = Signal::SELL;
 *                in_position = false;
 *            }
 *        }
 *    }
 *    ```
 */

/**
 * Q5: What's a good Sharpe ratio target?
 * 
 * A: Depends on strategy type and market:
 *    - Day trading: 1.0-2.0 is good (high frequency, lower per-trade return)
 *    - Swing trading: 1.5-3.0 is good (medium frequency)
 *    - Position trading: 2.0-4.0 is good (low frequency, higher per-trade return)
 *    
 *    Context matters:
 *    - Bull market: Easier to achieve high Sharpe
 *    - Bear market: Harder, but more valuable
 *    - Verify across multiple market conditions
 *    
 *    Above 3.0 is excellent but rare. Verify it's not curve-fitted.
 */

/**
 * Q6: How do I handle missing data or gaps?
 * 
 * A: Several approaches:
 *    
 *    1. Forward fill (use last known value):
 *    ```cpp
 *    double last_price = 0;
 *    for (auto& bar : price_data) {
 *        if (bar.close == 0 || std::isnan(bar.close)) {
 *            bar.close = last_price;  // Use previous
 *        }
 *        last_price = bar.close;
 *    }
 *    ```
 *    
 *    2. Skip missing bars:
 *    ```cpp
 *    if (price_data[i].close == 0) {
 *        signals[i] = Signal::HOLD;  // Don't trade on missing data
 *        continue;
 *    }
 *    ```
 *    
 *    3. Interpolate:
 *    ```cpp
 *    // Linear interpolation between known values
 *    double prev = price_data[i-1].close;
 *    double next = price_data[i+1].close;
 *    price_data[i].close = (prev + next) / 2;
 *    ```
 */

/**
 * Q7: Can I test multiple symbols simultaneously?
 * 
 * A: Current framework is single-symbol. For portfolio backtesting:
 *    
 *    ```cpp
 *    class PortfolioStrategy : public Strategy {
 *        std::vector<Signal> generate_signals(
 *            const std::map<std::string, std::vector<PriceBar>>& portfolio_data)
 *        {
 *            // Implement portfolio-level logic
 *            // - Correlation between symbols
 *            // - Sector rotation
 *            // - Pairs trading
 *            // - Portfolio rebalancing
 *        }
 *    };
 *    ```
 *    
 *    Requires extending framework for multi-symbol support.
 */

/**
 * Q8: How do I optimize strategy parameters?
 * 
 * A: Grid search or more sophisticated methods:
 *    
 *    ```cpp
 *    struct ParamSet {
 *        int fast_period;
 *        int slow_period;
 *        double sharpe;
 *    };
 *    
 *    std::vector<ParamSet> optimize() {
 *        std::vector<ParamSet> results;
 *        
 *        // Grid search
 *        for (int fast = 10; fast <= 100; fast += 10) {
 *            for (int slow = 50; slow <= 300; slow += 50) {
 *                if (fast >= slow) continue;
 *                
 *                JobParams params;
 *                params.strategy_params["fast"] = std::to_string(fast);
 *                params.strategy_params["slow"] = std::to_string(slow);
 *                
 *                strategy->set_parameters(params);
 *                JobResult result = strategy->backtest(train_data);
 *                
 *                results.push_back({fast, slow, result.sharpe_ratio});
 *            }
 *        }
 *        
 *        // Sort by Sharpe ratio
 *        std::sort(results.begin(), results.end(),
 *                 [](auto& a, auto& b) { return a.sharpe > b.sharpe; });
 *        
 *        return results;
 *    }
 *    ```
 *    
 *    WARNING: Always validate on out-of-sample data!
 */

/**
 * Q9: What's the minimum data size for reliable backtesting?
 * 
 * A: Depends on strategy frequency and goals:
 *    
 *    - Minimum: 252 bars (1 year of daily data)
 *    - Recommended: 756-1260 bars (3-5 years)
 *    - Ideal: 2520+ bars (10+ years, multiple market cycles)
 *    
 *    Need enough data for:
 *    - Statistical significance (30+ trades minimum)
 *    - Multiple market conditions (bull, bear, sideways)
 *    - Strategy warm-up period (indicators need history)
 *    
 *    Rule of thumb: 10× your average trade duration
 *    - Day trade strategy: Need 10 days × 10 = 100 days minimum
 *    - Weekly trade strategy: Need 10 weeks × 10 = 100 weeks minimum
 */

/**
 * Q10: How do I know if my strategy is overfit?
 * 
 * A: Warning signs of overfitting:
 *    
 *    ✓ Too many parameters (>5 is suspicious)
 *    ✓ Perfect or near-perfect backtest results
 *    ✓ Sharpe ratio > 4.0 (exceptionally rare)
 *    ✓ Very few trades (< 30) with high win rate
 *    ✓ Performance degrades on out-of-sample data
 *    ✓ Complex rules with many conditions
 *    ✓ Different parameter sets have wildly different results
 *    
 *    VALIDATION TECHNIQUES:
 *    
 *    1. Walk-forward analysis:
 *    ```
 *    Train: 2015-2017 → Test: 2018
 *    Train: 2016-2018 → Test: 2019
 *    Train: 2017-2019 → Test: 2020
 *    etc.
 *    ```
 *    
 *    2. Monte Carlo simulation:
 *    Randomly shuffle trade order, check if results still good
 *    
 *    3. Parameter sensitivity:
 *    Change parameters slightly, results shouldn't change drastically
 *    
 *    4. Compare to benchmark:
 *    Must beat buy-and-hold by meaningful margin after costs
 */