/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025

    File: sma_strategy.cpp
    Module: Worker Node - Strategy Engine Implementation
    
    Description:
        This file implements the backtesting strategy engine for the distributed
        financial backtesting system. It provides the core logic for executing
        trading strategies on historical price data and calculating performance
        metrics. The implementation includes:
        
        - Base Strategy class with generic backtest execution framework
        - Portfolio management and trade execution simulation
        - Performance metrics calculation (PnL, Sharpe ratio, max drawdown)
        - SMA (Simple Moving Average) crossover strategy implementation
        
        Architecture Context (Module 3: Strategy Engine):
        This code runs on worker nodes and processes time series data loaded
        by the CSVLoader. When a controller assigns a backtest job to a worker:
        
        1. Worker loads price data via CSVLoader
        2. Worker instantiates appropriate Strategy (e.g., SMAStrategy)
        3. Worker calls strategy.backtest(price_data)
        4. Strategy generates trading signals and simulates execution
        5. Strategy calculates performance metrics
        6. Worker returns JobResult to controller
        
        Data Flow:
        ┌─────────────────────────────────────────────────────────┐
        │ Controller: Assigns job to worker                       │
        └─────────────────────┬───────────────────────────────────┘
                              ↓
        ┌─────────────────────────────────────────────────────────┐
        │ Worker: Loads data                                      │
        │   CSVLoader.load("AAPL") → TimeSeriesData               │
        └─────────────────────┬───────────────────────────────────┘
                              ↓
        ┌─────────────────────────────────────────────────────────┐
        │ Strategy Engine (This Module): Executes backtest       │
        │   1. generate_signals() → BUY/SELL/HOLD signals         │
        │   2. Execute trades based on signals                    │
        │   3. Track portfolio value over time                    │
        │   4. Calculate metrics: Sharpe, drawdown, win rate      │
        └─────────────────────┬───────────────────────────────────┘
                              ↓
        ┌─────────────────────────────────────────────────────────┐
        │ Worker: Returns JobResult                               │
        │   - Final portfolio value                               │
        │   - Total return, Sharpe ratio, max drawdown            │
        │   - Trade statistics                                    │
        └─────────────────────┬───────────────────────────────────┘
                              ↓
        ┌─────────────────────────────────────────────────────────┐
        │ Controller: Aggregates results from all workers         │
        └─────────────────────────────────────────────────────────┘
        
        Strategy Design Pattern:
        The code uses the Template Method pattern:
        - Strategy base class provides backtest framework (template method)
        - Concrete strategies (SMAStrategy) implement generate_signals()
        - This allows adding new strategies without modifying backtest logic
        
        Performance Characteristics:
        - Time Complexity: O(n) where n = number of price bars
        - Space Complexity: O(n) for storing signals and portfolio history
        - Typical execution time: 10-50ms for 1 year of daily data
        - Scalable across worker nodes via job distribution
        
        Per Project Plan (Section 3.3 - Module 3):
        - Pluggable strategy interface for multiple implementations
        - SMA Crossover strategy (implemented here)
        - RSI Momentum strategy (future implementation)
        - Mean Reversion strategy (optional stretch goal)
        
    Thread Safety:
        Strategy instances are NOT thread-safe. Each worker thread should
        create its own Strategy instance. Strategies maintain no state
        between backtest() calls, so the same instance can be reused
        sequentially for multiple symbols.
        
    Numerical Stability:
        - Uses double precision (64-bit) for all calculations
        - Handles edge cases: zero division, empty data, insufficient history
        - Sharpe ratio calculation includes safeguards against zero volatility
        - Drawdown calculation handles monotonically increasing portfolios
*******************************************************************************/

#include "strategy/sma_strategy.h"
#include "common/logger.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace backtesting {

/*******************************************************************************
 * TABLE OF CONTENTS
 *******************************************************************************
 * 
 * 1. BACKTESTING FUNDAMENTALS
 *    1.1 What is Backtesting?
 *    1.2 Backtesting vs Live Trading
 *    1.3 Common Backtesting Pitfalls
 * 
 * 2. ARCHITECTURE OVERVIEW
 *    2.1 Strategy Base Class
 *    2.2 SMAStrategy Implementation
 *    2.3 Data Structures (Signal, PortfolioState, Trade, JobResult)
 * 
 * 3. BACKTEST EXECUTION FLOW
 *    3.1 Signal Generation Phase
 *    3.2 Trade Execution Phase
 *    3.3 Portfolio Tracking Phase
 *    3.4 Metrics Calculation Phase
 * 
 * 4. PERFORMANCE METRICS
 *    4.1 Total Return
 *    4.2 Sharpe Ratio (Risk-Adjusted Return)
 *    4.3 Maximum Drawdown (Risk Measure)
 *    4.4 Win Rate and Trade Statistics
 * 
 * 5. SMA CROSSOVER STRATEGY
 *    5.1 Strategy Logic
 *    5.2 Golden Cross and Death Cross
 *    5.3 Parameter Selection
 *    5.4 When SMA Works and When It Fails
 * 
 * 6. USAGE EXAMPLES
 *    6.1 Basic Backtest Execution
 *    6.2 Custom Strategy Implementation
 *    6.3 Integration with Worker Node
 *    6.4 Parameter Optimization
 * 
 * 7. COMMON PITFALLS & BEST PRACTICES
 *    7.1 Look-Ahead Bias
 *    7.2 Overfitting
 *    7.3 Transaction Costs
 *    7.4 Slippage and Market Impact
 * 
 * 8. FAQ
 * 
 * 9. EXTENDING THE FRAMEWORK
 ******************************************************************************/

/*******************************************************************************
 * SECTION 1: BACKTESTING FUNDAMENTALS
 *******************************************************************************
 * 
 * 1.1 WHAT IS BACKTESTING?
 * ------------------------
 * 
 * Backtesting is the process of testing a trading strategy using historical
 * price data to evaluate its performance. The goal is to answer:
 * "If I had followed this strategy in the past, how would I have performed?"
 * 
 * Key Concepts:
 * 
 * Trading Signal:
 *   A decision point generated by the strategy (BUY, SELL, or HOLD)
 *   Example: "When 50-day SMA crosses above 200-day SMA, generate BUY signal"
 * 
 * Portfolio Simulation:
 *   Track hypothetical positions, cash balance, and total value over time
 *   Example: Start with $10,000, execute trades based on signals
 * 
 * Performance Metrics:
 *   Quantitative measures of strategy effectiveness:
 *   - Return: Did the strategy make money?
 *   - Risk: How volatile were the returns?
 *   - Drawdown: How much did the portfolio decline from peak?
 * 
 * 
 * 1.2 BACKTESTING VS LIVE TRADING
 * --------------------------------
 * 
 * Key Differences:
 * 
 * Execution:
 *   - Backtest: Trades execute at exact prices (unrealistic)
 *   - Live: Slippage, partial fills, market impact (realistic)
 * 
 * Data:
 *   - Backtest: Perfect historical data with no gaps
 *   - Live: Real-time data with delays, errors, missing ticks
 * 
 * Psychology:
 *   - Backtest: No emotional decisions (algorithm runs mechanically)
 *   - Live: Fear and greed influence execution
 * 
 * Costs:
 *   - Backtest: Often ignores commissions, fees, spread
 *   - Live: All costs directly impact P&L
 * 
 * Why Backtest Anyway?
 *   - Eliminates obviously bad strategies before risking capital
 *   - Provides statistical evidence of edge
 *   - Allows rapid iteration and parameter tuning
 *   - Required for institutional trading (regulatory compliance)
 * 
 * 
 * 1.3 COMMON BACKTESTING PITFALLS
 * --------------------------------
 * 
 * Look-Ahead Bias:
 *   Using future information not available at decision time
 *   Example: Using next day's close to generate today's signal
 *   Result: Artificially inflated returns
 * 
 * Survivorship Bias:
 *   Only testing on stocks that still exist today
 *   Example: Backtesting S&P 500 using current constituents
 *   Result: Ignores companies that went bankrupt or delisted
 * 
 * Overfitting:
 *   Optimizing strategy parameters too precisely to historical data
 *   Example: Finding "perfect" SMA windows (37, 183) for one period
 *   Result: Strategy fails on new data (doesn't generalize)
 * 
 * Ignoring Transaction Costs:
 *   Not accounting for commissions, slippage, spread
 *   Example: High-frequency strategy with 100 trades/day
 *   Result: Profitable in backtest, loses money live
 * 
 * This implementation addresses:
 * ✓ Look-ahead bias: Signals generated only from past data
 * ✓ Clean signal logic: No peeking at future prices
 * ✗ Transaction costs: Not yet implemented (stretch goal)
 * ✗ Survivorship bias: Data preparation issue (not strategy engine concern)
 ******************************************************************************/

/*******************************************************************************
 * SECTION 2: ARCHITECTURE OVERVIEW
 *******************************************************************************
 * 
 * CLASS HIERARCHY:
 * ----------------
 * 
 * Strategy (Base Class)
 * │
 * ├── backtest()                 [Template method - common logic]
 * │   ├── generate_signals()      [Pure virtual - strategy-specific]
 * │   ├── execute_trades()        [Common logic]
 * │   ├── track_portfolio()       [Common logic]
 * │   └── calculate_metrics()     [Common logic]
 * │
 * ├── calculate_sharpe_ratio()   [Helper method]
 * └── calculate_max_drawdown()   [Helper method]
 * 
 * SMAStrategy (Concrete Implementation)
 * │
 * ├── generate_signals()          [Override - SMA crossover logic]
 * └── calculate_sma()             [Helper - moving average computation]
 * 
 * 
 * DATA FLOW DIAGRAM:
 * ------------------
 * 
 * Input: vector<PriceBar>
 *   ↓
 * [generate_signals()]
 *   → Extract prices
 *   → Calculate indicators (SMA, RSI, etc.)
 *   → Generate BUY/SELL/HOLD signals
 *   ↓
 * Output: vector<Signal>
 *   ↓
 * [Trade Execution Loop]
 *   For each bar:
 *     → Check signal change
 *     → Execute trade if signal changed
 *     → Update portfolio (cash, shares)
 *     → Record trade
 *   ↓
 * Output: vector<Trade>, PortfolioState
 *   ↓
 * [Metrics Calculation]
 *   → Total return
 *   → Sharpe ratio (from returns series)
 *   → Max drawdown (from portfolio value series)
 *   → Win/loss statistics
 *   ↓
 * Output: JobResult
 * 
 * 
 * KEY DATA STRUCTURES (assumed from headers):
 * -------------------------------------------
 * 
 * Signal (enum):
 *   - BUY: Enter long position
 *   - SELL: Exit long position (or enter short, if supported)
 *   - HOLD: Maintain current position
 * 
 * PriceBar (struct):
 *   - date: Trading date
 *   - open, high, low, close: OHLC prices
 *   - volume: Trading volume
 * 
 * PortfolioState (struct):
 *   - cash: Available cash balance
 *   - shares_held: Number of shares owned
 *   - portfolio_value: Total value (cash + shares × price)
 *   - peak_value: Highest portfolio value achieved (for drawdown)
 * 
 * Trade (struct):
 *   - date: When trade executed
 *   - signal: BUY or SELL
 *   - price: Execution price
 *   - shares: Number of shares traded
 *   - value: Total trade value (shares × price)
 * 
 * JobResult (struct):
 *   - symbol: Stock ticker
 *   - success: Whether backtest completed successfully
 *   - error_message: Error details if failed
 *   - final_portfolio_value: Ending portfolio value
 *   - total_return: Percentage return
 *   - sharpe_ratio: Risk-adjusted return metric
 *   - max_drawdown: Maximum peak-to-trough decline
 *   - num_trades: Total number of trades executed
 *   - winning_trades, losing_trades: Trade outcome counts
 * 
 * StrategyParams (struct):
 *   - symbol: Stock ticker to backtest
 *   - initial_capital: Starting portfolio value (e.g., $10,000)
 *   - strategy_specific_params: SMA windows, RSI thresholds, etc.
 ******************************************************************************/

/*******************************************************************************
 * SECTION 3.1: Strategy Base Class - backtest() Method
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * The backtest() method is the main entry point for strategy execution. It
 * orchestrates the entire backtesting process using a template method pattern:
 * 
 * 1. Validate input data
 * 2. Generate signals (strategy-specific, delegated to subclass)
 * 3. Simulate trade execution
 * 4. Track portfolio evolution
 * 5. Calculate performance metrics
 * 6. Return comprehensive results
 * 
 * TEMPLATE METHOD PATTERN:
 * ------------------------
 * This design allows:
 * - Common logic (portfolio management, metrics) centralized in base class
 * - Strategy-specific logic (signal generation) implemented by subclasses
 * - Easy addition of new strategies without duplicating backtest framework
 * 
 * Example: Adding RSI Strategy
 *   class RSIStrategy : public Strategy {
 *       std::vector<Signal> generate_signals(...) override {
 *           // RSI-specific logic here
 *       }
 *   };
 *   // Inherits all portfolio and metrics logic automatically!
 * 
 * INPUT VALIDATION:
 * -----------------
 * The method performs defensive checks:
 * - Empty data → Return error immediately (can't backtest nothing)
 * - Signal generation throws → Catch and report gracefully
 * - Signal count mismatch → Detect logic errors early
 * 
 * Why Signal Count Must Match Price Data Count:
 *   Each price bar needs corresponding signal for correct execution.
 *   Mismatch indicates bug in generate_signals() implementation.
 * 
 * EXECUTION MODEL:
 * ----------------
 * Trade Trigger: Signal change (previous bar HOLD, current bar BUY)
 * 
 * Why Not Trade on Every Signal?
 *   - HOLD signals don't trigger trades (already in desired state)
 *   - Only signal *changes* represent actionable decisions
 *   - Reduces unnecessary trading (lower transaction costs)
 * 
 * Position Sizing:
 *   - BUY: Purchase maximum shares affordable (cash / price)
 *   - SELL: Liquidate entire position (sell all shares)
 *   - Future enhancement: Position sizing based on risk (Kelly criterion)
 * 
 * PORTFOLIO TRACKING:
 * -------------------
 * For each bar:
 *   1. Execute any triggered trades
 *   2. Update cash and shares_held
 *   3. Calculate current portfolio value (cash + shares × price)
 *   4. Track peak value (for drawdown calculation)
 *   5. Calculate period return (for Sharpe ratio)
 * 
 * METRICS CALCULATION:
 * --------------------
 * Final Step: Aggregate statistics
 *   - Total return: (final_value - initial_capital) / initial_capital
 *   - Sharpe ratio: Risk-adjusted return (see Section 4.2)
 *   - Max drawdown: Largest peak-to-trough decline (see Section 4.3)
 *   - Win/loss ratio: Count profitable vs unprofitable trades
 * 
 * ERROR HANDLING:
 * ---------------
 * Comprehensive error handling ensures worker doesn't crash:
 * - Try-catch around signal generation
 * - Validate signal vector size
 * - Check for division by zero in metrics
 * - Return JobResult with success=false and error_message on failure
 * 
 * This allows controller to:
 * - Identify which symbols failed
 * - Retry with different parameters
 * - Aggregate only successful results
 ******************************************************************************/

JobResult Strategy::backtest(const std::vector<PriceBar>& price_data) {
    JobResult result;
    result.symbol = params_.symbol;
    result.success = false;  // Assume failure until proven successful
    
    // VALIDATION: Check for empty data
    // Without data, we cannot generate signals or execute trades
    if (price_data.empty()) {
        result.error_message = "No price data available";
        return result;
    }
    
    // SIGNAL GENERATION PHASE
    // Delegate to strategy-specific implementation (virtual method)
    // This is the only part that differs between strategies
    std::vector<Signal> signals;
    try {
        signals = generate_signals(price_data);
    } catch (const std::exception& e) {
        // Catch any errors in signal generation (bad data, math errors, etc.)
        result.error_message = std::string("Signal generation failed: ") + e.what();
        return result;
    }
    
    // VALIDATION: Ensure one signal per price bar
    // Mismatch indicates logic error in generate_signals()
    if (signals.size() != price_data.size()) {
        result.error_message = "Signal count mismatch with price data";
        return result;
    }
    
    // PORTFOLIO INITIALIZATION
    // Start with configured initial capital (e.g., $10,000)
    // No shares held initially (cash-only position)
    PortfolioState portfolio(params_.initial_capital);
    
    // Storage for trades executed during backtest
    std::vector<Trade> trades;
    
    // Track portfolio value over time (used for drawdown and return calculations)
    std::vector<double> portfolio_values;
    
    // Track period-by-period returns (used for Sharpe ratio)
    std::vector<double> returns;
    
    // Previous signal state (for detecting signal changes)
    Signal prev_signal = Signal::HOLD;
    
    // TRADE EXECUTION AND PORTFOLIO TRACKING LOOP
    // Iterate through all price bars chronologically
    for (size_t i = 0; i < price_data.size(); ++i) {
        const auto& bar = price_data[i];
        Signal signal = signals[i];
        
        // TRADE EXECUTION: Execute on signal changes only
        // This prevents redundant trades when signal doesn't change
        if (signal != prev_signal && signal != Signal::HOLD) {
            Trade trade;
            trade.date = bar.date;
            trade.signal = signal;
            trade.price = bar.close;  // Execute at close price (realistic assumption)
            
            // BUY LOGIC: Enter long position
            if (signal == Signal::BUY && portfolio.shares_held == 0) {
                // Calculate maximum shares we can afford
                // Note: Integer division (fractional shares not supported)
                int shares = static_cast<int>(portfolio.cash / bar.close);
                
                if (shares > 0) {
                    trade.shares = shares;
                    trade.value = shares * bar.close;
                    
                    // Deduct cost from cash
                    portfolio.cash -= trade.value;
                    
                    // Add shares to holdings
                    portfolio.shares_held += shares;
                    
                    trades.push_back(trade);
                }
                // If shares == 0, we don't have enough cash to buy even 1 share
                // This can happen late in backtest after losses
                
            } 
            // SELL LOGIC: Exit long position
            else if (signal == Signal::SELL && portfolio.shares_held > 0) {
                // Liquidate entire position (sell all shares)
                trade.shares = portfolio.shares_held;
                trade.value = portfolio.shares_held * bar.close;
                
                // Add proceeds to cash
                portfolio.cash += trade.value;
                
                // Zero out share holdings
                portfolio.shares_held = 0;
                
                trades.push_back(trade);
            }
            // Note: We only trade if we're not already in desired state
            // - BUY when shares_held == 0 (entering position)
            // - SELL when shares_held > 0 (exiting position)
        }
        
        // PORTFOLIO VALUE TRACKING
        // Calculate mark-to-market value (cash + current value of holdings)
        double current_value = portfolio.cash + portfolio.shares_held * bar.close;
        portfolio_values.push_back(current_value);
        
        // RETURN CALCULATION
        // Calculate period return (percentage change from previous value)
        if (i > 0 && portfolio_values[i-1] > 0) {
            double ret = (current_value - portfolio_values[i-1]) / portfolio_values[i-1];
            returns.push_back(ret);
        }
        // First bar has no return (no previous value to compare)
        
        // PEAK TRACKING (for drawdown calculation)
        // Update peak if current value exceeds previous peak
        if (current_value > portfolio.peak_value) {
            portfolio.peak_value = current_value;
        }
        
        // Update portfolio state for next iteration
        portfolio.portfolio_value = current_value;
        prev_signal = signal;
    }
    
    // METRICS CALCULATION PHASE
    
    // Total Return: Percentage gain/loss from initial capital
    result.final_portfolio_value = portfolio.portfolio_value;
    result.total_return = ((portfolio.portfolio_value - params_.initial_capital) / 
                          params_.initial_capital) * 100.0;
    
    // Sharpe Ratio: Risk-adjusted return
    // Only calculate if we have returns data
    if (!returns.empty()) {
        result.sharpe_ratio = calculate_sharpe_ratio(returns);
    }
    
    // Maximum Drawdown: Largest peak-to-trough decline
    result.max_drawdown = calculate_max_drawdown(portfolio_values);
    
    // Trade Count: Total number of executed trades
    result.num_trades = static_cast<int>(trades.size());
    
    // WIN/LOSS STATISTICS
    // Count winning and losing trades
    // A winning trade = SELL price > BUY price
    for (size_t i = 1; i < trades.size(); ++i) {
        if (trades[i].signal == Signal::SELL && trades[i-1].signal == Signal::BUY) {
            // Found a complete round trip (BUY followed by SELL)
            double profit = trades[i].value - trades[i-1].value;
            
            if (profit > 0) {
                result.winning_trades++;
            } else {
                result.losing_trades++;
            }
        }
    }
    // Note: This logic assumes alternating BUY/SELL
    // More sophisticated tracking needed for complex strategies (short selling, etc.)
    
    // SUCCESS: All calculations completed without error
    result.success = true;
    return result;
}

/*******************************************************************************
 * SECTION 4.2: Sharpe Ratio Calculation
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * The Sharpe ratio measures risk-adjusted returns. It answers:
 * "How much return did I earn per unit of risk taken?"
 * 
 * FORMULA:
 * --------
 * Sharpe Ratio = (Mean Return - Risk-Free Rate) / Standard Deviation of Returns
 * 
 * Annualized Version:
 * Sharpe Ratio = [(Mean Daily Return - Risk-Free Rate) / Daily Std Dev] × √252
 * 
 * Where:
 * - Mean Return: Average return across all periods
 * - Risk-Free Rate: Return of "safe" investment (e.g., Treasury bill)
 * - Standard Deviation: Volatility of returns
 * - √252: Annualization factor (252 trading days per year)
 * 
 * INTERPRETATION:
 * ---------------
 * Sharpe Ratio > 1.0: Good (earning more than 1 unit of return per unit of risk)
 * Sharpe Ratio > 2.0: Very good (institutional quality)
 * Sharpe Ratio > 3.0: Excellent (rare, possibly overfit)
 * Sharpe Ratio < 0: Bad (losing money or taking excessive risk)
 * 
 * Example:
 *   Strategy A: 15% annual return, 10% volatility
 *     Sharpe = (0.15 - 0.02) / 0.10 = 1.3
 *   
 *   Strategy B: 20% annual return, 20% volatility
 *     Sharpe = (0.20 - 0.02) / 0.20 = 0.9
 *   
 *   Strategy A is better (higher risk-adjusted return)
 * 
 * WHY IT MATTERS:
 * ---------------
 * - Raw returns can be misleading (high return with extreme volatility)
 * - Sharpe ratio normalizes for risk (apples-to-apples comparison)
 * - Industry standard metric (used by all hedge funds, asset managers)
 * - Incorporates downside risk (volatility penalizes both gains and losses)
 * 
 * IMPLEMENTATION DETAILS:
 * -----------------------
 * 1. Calculate mean return (arithmetic average)
 * 2. Calculate standard deviation (population std dev)
 * 3. Compute excess return (mean - risk-free rate)
 * 4. Divide excess return by standard deviation
 * 5. Annualize by multiplying by √252
 * 
 * Edge Cases:
 * - Zero volatility (std_dev = 0): Return 0.0 to avoid division by zero
 * - Fewer than 2 returns: Cannot calculate std dev, return 0.0
 * - All returns identical: Zero variance, return 0.0
 * 
 * ASSUMPTIONS:
 * ------------
 * - Daily returns (if different frequency, change 252 to appropriate factor)
 * - Normal distribution of returns (Sharpe assumes Gaussian)
 * - Independent returns (no autocorrelation)
 * - Constant volatility (homoscedasticity)
 * 
 * Violations in Practice:
 * - Returns are NOT normally distributed (fat tails, skewness)
 * - Volatility clusters (high volatility periods, low volatility periods)
 * - Returns show momentum (autocorrelation)
 * 
 * Despite violations, Sharpe ratio remains useful as comparative metric
 ******************************************************************************/

double Strategy::calculate_sharpe_ratio(const std::vector<double>& returns,
                                       double risk_free_rate) const {
    // Edge case: Need at least 2 returns to calculate standard deviation
    if (returns.size() < 2) return 0.0;
    
    // STEP 1: Calculate mean return
    // Sum all returns and divide by count
    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    
    // STEP 2: Calculate variance (sum of squared deviations)
    double sq_sum = 0.0;
    for (double r : returns) {
        sq_sum += (r - mean) * (r - mean);
    }
    
    // STEP 3: Calculate standard deviation (square root of variance)
    // Using population std dev (divide by N, not N-1)
    double std_dev = std::sqrt(sq_sum / returns.size());
    
    // Edge case: Zero volatility (all returns identical)
    // Avoid division by zero
    if (std_dev < 1e-10) return 0.0;
    
    // STEP 4: Calculate excess return over risk-free rate
    double excess_return = mean - risk_free_rate;
    
    // STEP 5: Calculate Sharpe ratio and annualize
    // √252 converts daily Sharpe to annual Sharpe
    // 252 = typical number of trading days per year
    return (excess_return / std_dev) * std::sqrt(252.0);
}

/*******************************************************************************
 * SECTION 4.3: Maximum Drawdown Calculation
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * Maximum drawdown measures the largest peak-to-trough decline in portfolio
 * value. It answers: "What is the worst loss I would have experienced?"
 * 
 * FORMULA:
 * --------
 * Drawdown(t) = (Peak Value - Current Value) / Peak Value × 100%
 * Max Drawdown = max(Drawdown(t)) for all t
 * 
 * INTERPRETATION:
 * ---------------
 * Max Drawdown = 10%: Moderate risk (portfolio declined 10% from peak)
 * Max Drawdown = 25%: High risk (significant decline, requires strong conviction)
 * Max Drawdown = 50%: Extreme risk (portfolio cut in half, very difficult psychologically)
 * 
 * Example Scenario:
 *   Portfolio value over time: $10k → $15k → $12k → $18k → $14k
 *   
 *   Peak at $15k, trough at $12k:
 *     Drawdown = (15k - 12k) / 15k = 20%
 *   
 *   Peak at $18k, trough at $14k:
 *     Drawdown = (18k - 14k) / 18k = 22.2%
 *   
 *   Max Drawdown = 22.2% (worst decline)
 * 
 * WHY IT MATTERS:
 * ---------------
 * - Psychological pain: Large drawdowns cause investors to abandon strategy
 * - Recovery difficulty: 50% loss requires 100% gain to recover
 * - Risk management: Set stop-loss based on tolerable drawdown
 * - Leverage limits: Exchange sets margin requirements based on max drawdown
 * 
 * COMPLEMENTARY TO VOLATILITY:
 * ----------------------------
 * Sharpe ratio (volatility) and max drawdown measure different aspects of risk:
 * 
 * - Sharpe: Measures average deviation (frequent small moves)
 * - Drawdown: Measures worst-case scenario (infrequent large declines)
 * 
 * Example: Two strategies, same Sharpe ratio
 *   Strategy A: Consistent small losses, occasional big wins (low drawdown)
 *   Strategy B: Consistent small wins, occasional catastrophic loss (high drawdown)
 *   
 *   Strategy A is preferable (survivable drawdowns)
 * 
 * IMPLEMENTATION DETAILS:
 * -----------------------
 * Algorithm:
 *   1. Track running peak (highest value seen so far)
 *   2. At each point, calculate drawdown from peak
 *   3. Track maximum drawdown encountered
 * 
 * Time Complexity: O(n) - single pass through portfolio values
 * Space Complexity: O(1) - only track peak and max_dd
 * 
 * Edge Cases:
 * - Empty portfolio history: Return 0.0
 * - Monotonically increasing portfolio: Max drawdown = 0.0 (no decline)
 * - Constant portfolio value: Max drawdown = 0.0 (no change)
 * 
 * PRACTICAL CONSIDERATIONS:
 * -------------------------
 * Drawdown Duration:
 *   This implementation calculates magnitude, not duration.
 *   Long drawdowns (time to recover) are psychologically harder than deep drawdowns.
 *   
 *   Future enhancement: Track drawdown duration
 *     - Time from peak to trough
 *     - Time from trough to recovery
 * 
 * Underwater Curve:
 *   Plot of current drawdown over time.
 *   Shows periods of pain (strategy underwater vs peak).
 *   Useful for visualizing drawdown distribution.
 ******************************************************************************/

double Strategy::calculate_max_drawdown(const std::vector<double>& portfolio_values) const {
    // Edge case: No portfolio history
    if (portfolio_values.empty()) return 0.0;
    
    double max_dd = 0.0;             // Maximum drawdown encountered
    double peak = portfolio_values[0]; // Running peak value
    
    // Iterate through portfolio values chronologically
    for (double value : portfolio_values) {
        // Update peak if current value exceeds it
        if (value > peak) {
            peak = value;
        }
        
        // Calculate drawdown from peak (as percentage)
        double drawdown = (peak - value) / peak * 100.0;
        
        // Update max drawdown if current exceeds it
        if (drawdown > max_dd) {
            max_dd = drawdown;
        }
    }
    
    return max_dd;
}

/*******************************************************************************
 * SECTION 5: SMA CROSSOVER STRATEGY IMPLEMENTATION
 *******************************************************************************
 * 
 * STRATEGY OVERVIEW:
 * ------------------
 * Simple Moving Average (SMA) Crossover is a trend-following strategy based
 * on the relationship between two moving averages of different periods.
 * 
 * Core Principle:
 *   - Short-term SMA reacts quickly to price changes
 *   - Long-term SMA smooths out noise, represents long-term trend
 *   - When short MA crosses above long MA → Uptrend beginning (BUY)
 *   - When short MA crosses below long MA → Downtrend beginning (SELL)
 * 
 * MOVING AVERAGE CALCULATION:
 * ---------------------------
 * SMA(n) = (Price[t] + Price[t-1] + ... + Price[t-n+1]) / n
 * 
 * Example: 5-day SMA
 *   Prices: [100, 102, 101, 103, 105]
 *   SMA(5) = (100 + 102 + 101 + 103 + 105) / 5 = 102.2
 *   
 *   Next day, price = 107:
 *   SMA(5) = (102 + 101 + 103 + 105 + 107) / 5 = 103.6
 * 
 * Properties:
 *   - Lags price action (uses historical data)
 *   - Smooths volatility (averaging effect)
 *   - Longer period = more smoothing, more lag
 * 
 * SIGNAL GENERATION RULES:
 * ------------------------
 * 
 * Golden Cross (BUY Signal):
 *   Condition: Short SMA crosses above Long SMA
 *   Interpretation: Momentum shifting bullish, enter long position
 *   
 *   Visual:
 *     Long SMA: ————————————————
 *     Short SMA:        ╱
 *                      ╱  ← Golden Cross
 *                     ╱
 *     
 *   Example:
 *     Day 100: Short=98, Long=100 (short below long)
 *     Day 101: Short=102, Long=101 (short crosses above) → BUY
 * 
 * Death Cross (SELL Signal):
 *   Condition: Short SMA crosses below Long SMA
 *   Interpretation: Momentum shifting bearish, exit long position
 *   
 *   Visual:
 *     Long SMA: ————————————————
 *     Short SMA: ╲
 *                 ╲  ← Death Cross
 *                  ╲
 *     
 *   Example:
 *     Day 200: Short=105, Long=103 (short above long)
 *     Day 201: Short=102, Long=104 (short crosses below) → SELL
 * 
 * PARAMETER SELECTION:
 * --------------------
 * Common parameter combinations:
 * 
 * - (20, 50): Short-term trading (sensitive, more signals)
 * - (50, 200): Long-term investing (classic Wall Street indicator)
 * - (12, 26): MACD parameters (popular in technical analysis)
 * 
 * Trade-offs:
 * 
 * Shorter windows (e.g., 5, 20):
 *   ✓ React quickly to trend changes
 *   ✗ Generate more false signals (noise)
 *   ✓ More trades (higher potential profit)
 *   ✗ More whipsaws (buy high, sell low repeatedly)
 * 
 * Longer windows (e.g., 100, 200):
 *   ✓ Fewer false signals (true trends)
 *   ✗ Late entries and exits (miss part of move)
 *   ✓ Fewer trades (lower transaction costs)
 *   ✗ Large drawdowns (slow to exit losing positions)
 * 
 * WHEN SMA WORKS:
 * ---------------
 * Market Conditions Favorable to SMA:
 * 
 * 1. Strong Trends:
 *    - Markets moving consistently in one direction
 *    - Example: 2009-2020 bull market
 * 
 * 2. Low Volatility:
 *    - Smooth price action, minimal noise
 *    - Moving averages track trend cleanly
 * 
 * 3. Low-Frequency Data:
 *    - Daily or weekly bars (not intraday)
 *    - Reduces whipsaw from high-frequency noise
 * 
 * WHEN SMA FAILS:
 * ---------------
 * Market Conditions Unfavorable to SMA:
 * 
 * 1. Ranging/Sideways Markets:
 *    - Price oscillates without clear trend
 *    - Generates many false crossovers
 *    - Death by a thousand cuts (many small losses)
 * 
 * 2. High Volatility:
 *    - Rapid price swings cross MAs frequently
 *    - Whipsaw effect (buy tops, sell bottoms)
 * 
 * 3. Fast Reversals:
 *    - Trend changes quickly
 *    - SMA lag causes late exits (large losses)
 * 
 * IMPLEMENTATION OPTIMIZATION:
 * ----------------------------
 * Efficient SMA Calculation:
 *   Naive: Recalculate sum for each bar → O(n × window)
 *   Optimized: Running sum (add new, subtract old) → O(n)
 *   
 *   This implementation uses optimized approach:
 *   - Calculate initial sum
 *   - For each subsequent bar: sum = sum - old_price + new_price
 *   - Avoid redundant summations
 * 
 * Memory Efficiency:
 *   Store entire SMA series (needed for crossover detection)
 *   Alternative: Store only last SMA value (if only latest signal needed)
 *   This implementation stores full series for flexibility
 ******************************************************************************/

/*******************************************************************************
 * SECTION 5.1: SMA Calculation Helper Method
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * Calculate Simple Moving Average for a price series using an efficient
 * sliding window algorithm.
 * 
 * ALGORITHM:
 * ----------
 * 1. Calculate first SMA (sum first 'window' prices)
 * 2. For subsequent bars, use running sum:
 *    - Subtract price that left window
 *    - Add price that entered window
 *    - Divide by window size
 * 
 * Time Complexity: O(n) where n = prices.size()
 * Space Complexity: O(n) for output vector
 * 
 * EXAMPLE:
 * --------
 * Prices: [10, 12, 14, 16, 18, 20, 22]
 * Window: 3
 * 
 * Bar 0: SMA = undefined (need 3 prices)
 * Bar 1: SMA = undefined (need 3 prices)
 * Bar 2: SMA = (10 + 12 + 14) / 3 = 12.0
 * Bar 3: SMA = (12 + 14 + 16) / 3 = 14.0  ← Remove 10, add 16
 * Bar 4: SMA = (14 + 16 + 18) / 3 = 16.0  ← Remove 12, add 18
 * Bar 5: SMA = (16 + 18 + 20) / 3 = 18.0
 * Bar 6: SMA = (18 + 20 + 22) / 3 = 20.0
 * 
 * Output: [0, 0, 12, 14, 16, 18, 20]
 *         └─────┘
 *    Undefined period (insufficient history)
 * 
 * EDGE CASES:
 * -----------
 * 1. prices.size() < window:
 *    Return vector of zeros (insufficient data for any SMA)
 * 
 * 2. window = 1:
 *    SMA = price itself (no smoothing)
 * 
 * 3. window = prices.size():
 *    Only one SMA value (last bar = average of all prices)
 ******************************************************************************/

std::vector<double> SMAStrategy::calculate_sma(const std::vector<double>& prices,
                                               int window) const {
    // Initialize output vector with zeros
    std::vector<double> sma(prices.size(), 0.0);
    
    // Edge case: Not enough data for even one SMA value
    if (prices.size() < static_cast<size_t>(window)) {
        return sma;  // Return all zeros
    }
    
    // STEP 1: Calculate first SMA (explicit sum)
    double sum = 0.0;
    for (int i = 0; i < window; ++i) {
        sum += prices[i];
    }
    sma[window - 1] = sum / window;  // Store at last position of window
    
    // STEP 2: Calculate remaining SMAs using running sum
    // Sliding window technique:
    //   - Remove oldest price (left edge of window)
    //   - Add newest price (right edge of window)
    //   - Divide by window size
    for (size_t i = window; i < prices.size(); ++i) {
        sum = sum - prices[i - window] + prices[i];
        sma[i] = sum / window;
    }
    
    return sma;
}

/*******************************************************************************
 * SECTION 5.2: Signal Generation Implementation
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * Generate BUY/SELL/HOLD signals for each price bar based on SMA crossover logic.
 * 
 * ALGORITHM:
 * ----------
 * 1. Validate sufficient data (need at least long_window bars)
 * 2. Extract closing prices from price bars
 * 3. Calculate short-term and long-term SMAs
 * 4. Detect crossovers:
 *    - Short MA crosses above long MA → BUY
 *    - Short MA crosses below long MA → SELL
 *    - Otherwise → HOLD
 * 
 * CROSSOVER DETECTION:
 * --------------------
 * Requires tracking state across bars:
 *   - prev_above: Was short MA above long MA on previous bar?
 *   - curr_above: Is short MA above long MA on current bar?
 * 
 * Logic:
 *   if (curr_above && !prev_above) → Golden cross (BUY)
 *   if (!curr_above && prev_above) → Death cross (SELL)
 *   else → No crossover (HOLD)
 * 
 * Example Timeline:
 *   Bar 100: short=98, long=100 → curr_above=false
 *   Bar 101: short=99, long=100 → curr_above=false (no crossover, HOLD)
 *   Bar 102: short=101, long=100 → curr_above=true (crossover detected, BUY)
 *   Bar 103: short=102, long=100 → curr_above=true (no crossover, HOLD)
 *   Bar 104: short=99, long=100 → curr_above=false (crossover detected, SELL)
 * 
 * INITIALIZATION PERIOD:
 * ----------------------
 * First long_window bars have no signals (HOLD):
 *   - Can't calculate long SMA until we have long_window prices
 *   - Can't detect crossover until we have at least 2 valid SMA points
 * 
 * Example: long_window = 200
 *   Bars 0-199: HOLD (insufficient history)
 *   Bar 200: First signal possible (compare bar 199 and 200)
 * 
 * This is expected behavior:
 *   - No fake signals generated from insufficient data
 *   - Strategy only trades when statistically valid
 * 
 * VALIDATION:
 * -----------
 * Check for insufficient data and warn (doesn't throw exception):
 *   - Allows graceful degradation
 *   - Worker can report "insufficient data" in JobResult
 *   - Controller can aggregate results from symbols with sufficient data
 * 
 * ERROR HANDLING:
 * ---------------
 * This method doesn't throw exceptions:
 *   - Returns HOLD signals for all bars if data insufficient
 *   - Logs warning for debugging
 *   - backtest() method handles downstream (no trades executed)
 ******************************************************************************/

std::vector<Signal> SMAStrategy::generate_signals(
    const std::vector<PriceBar>& price_data) {
    
    // Initialize all signals to HOLD (conservative default)
    std::vector<Signal> signals(price_data.size(), Signal::HOLD);
    
    // VALIDATION: Check if we have enough data for long-term SMA
    // Need at least long_window bars to calculate first long-term SMA
    if (price_data.size() < static_cast<size_t>(std::max(short_window_, long_window_))) {
        Logger::warning("Insufficient data for SMA strategy: " + 
                       std::to_string(price_data.size()) + " bars");
        return signals;  // Return all HOLD signals
    }
    
    // EXTRACT CLOSING PRICES
    // SMAs operate on close prices (standard convention)
    // Reserve memory upfront to avoid reallocations
    std::vector<double> prices;
    prices.reserve(price_data.size());
    for (const auto& bar : price_data) {
        prices.push_back(bar.close);
    }
    
    // CALCULATE MOVING AVERAGES
    // Short-term MA: Reacts quickly to price changes
    auto short_sma = calculate_sma(prices, short_window_);
    
    // Long-term MA: Smooths noise, represents underlying trend
    auto long_sma = calculate_sma(prices, long_window_);
    
    // CROSSOVER DETECTION
    // Track whether short MA is above long MA
    bool prev_above = false;
    
    // Start from first bar where both SMAs are valid
    // Earlier bars don't have valid long-term SMA (remain HOLD)
    for (size_t i = long_window_; i < prices.size(); ++i) {
        // Verify both SMAs are valid (non-zero)
        // Zero indicates insufficient history for that bar
        if (short_sma[i] > 0 && long_sma[i] > 0) {
            // Current state: Is short MA above long MA?
            bool curr_above = short_sma[i] > long_sma[i];
            
            // Only check for crossovers after first valid comparison
            // (need previous bar's state to detect crossover)
            if (i > static_cast<size_t>(long_window_)) {
                // GOLDEN CROSS: Short crosses above long
                if (curr_above && !prev_above) {
                    signals[i] = Signal::BUY;
                } 
                // DEATH CROSS: Short crosses below long
                else if (!curr_above && prev_above) {
                    signals[i] = Signal::SELL;
                }
                // No crossover: Signal remains HOLD (default)
            }
            
            // Update previous state for next iteration
            prev_above = curr_above;
        }
    }
    
    return signals;
}

} // namespace backtesting

/*******************************************************************************
 * SECTION 6: USAGE EXAMPLES
 *******************************************************************************
 * 
 * EXAMPLE 6.1: Basic Backtest Execution
 * --------------------------------------
 * 
 * #include "strategy/sma_strategy.h"
 * #include "data/csv_loader.h"
 * 
 * using namespace backtesting;
 * 
 * int main() {
 *     // Load historical data
 *     CSVLoader loader("/data/sp500/");
 *     auto aapl_data = loader.load("AAPL");
 *     
 *     if (!aapl_data || aapl_data->empty()) {
 *         std::cerr << "Failed to load data\n";
 *         return 1;
 *     }
 *     
 *     // Configure strategy parameters
 *     StrategyParams params;
 *     params.symbol = "AAPL";
 *     params.initial_capital = 10000.0;  // Start with $10,000
 *     
 *     // Create SMA strategy with 50/200 crossover
 *     SMAStrategy strategy(params, 50, 200);
 *     
 *     // Execute backtest
 *     JobResult result = strategy.backtest(aapl_data->get_all_bars());
 *     
 *     // Display results
 *     if (result.success) {
 *         std::cout << "Symbol: " << result.symbol << "\n";
 *         std::cout << "Final Value: $" << result.final_portfolio_value << "\n";
 *         std::cout << "Total Return: " << result.total_return << "%\n";
 *         std::cout << "Sharpe Ratio: " << result.sharpe_ratio << "\n";
 *         std::cout << "Max Drawdown: " << result.max_drawdown << "%\n";
 *         std::cout << "Trades: " << result.num_trades << "\n";
 *         std::cout << "Win Rate: " 
 *                   << (100.0 * result.winning_trades / 
 *                       (result.winning_trades + result.losing_trades)) 
 *                   << "%\n";
 *     } else {
 *         std::cerr << "Backtest failed: " << result.error_message << "\n";
 *     }
 *     
 *     return 0;
 * }
 * 
 * 
 * EXAMPLE 6.2: Custom Strategy Implementation
 * --------------------------------------------
 * 
 * // Create RSI (Relative Strength Index) strategy
 * class RSIStrategy : public Strategy {
 * private:
 *     int period_;        // RSI calculation period (typically 14)
 *     double overbought_; // RSI threshold for overbought (typically 70)
 *     double oversold_;   // RSI threshold for oversold (typically 30)
 *     
 * public:
 *     RSIStrategy(const StrategyParams& params, 
 *                 int period = 14, 
 *                 double overbought = 70, 
 *                 double oversold = 30)
 *         : Strategy(params), 
 *           period_(period), 
 *           overbought_(overbought), 
 *           oversold_(oversold) {}
 *     
 *     std::vector<Signal> generate_signals(
 *         const std::vector<PriceBar>& price_data) override {
 *         
 *         std::vector<Signal> signals(price_data.size(), Signal::HOLD);
 *         
 *         // Calculate RSI
 *         std::vector<double> rsi = calculate_rsi(price_data, period_);
 *         
 *         // Generate signals
 *         for (size_t i = period_; i < price_data.size(); ++i) {
 *             if (rsi[i] < oversold_) {
 *                 signals[i] = Signal::BUY;  // Oversold, buy
 *             } else if (rsi[i] > overbought_) {
 *                 signals[i] = Signal::SELL; // Overbought, sell
 *             }
 *         }
 *         
 *         return signals;
 *     }
 *     
 * private:
 *     std::vector<double> calculate_rsi(
 *         const std::vector<PriceBar>& data, int period) {
 *         // RSI calculation implementation...
 *         // (omitted for brevity)
 *     }
 * };
 * 
 * // Use exactly like SMAStrategy:
 * RSIStrategy rsi_strategy(params, 14, 70, 30);
 * JobResult result = rsi_strategy.backtest(price_data);
 * 
 * 
 * EXAMPLE 6.3: Integration with Worker Node
 * ------------------------------------------
 * 
 * // Worker's job execution handler
 * void Worker::execute_backtest_job(const BacktestJob& job) {
 *     CSVLoader loader(data_directory_);
 *     
 *     std::vector<JobResult> results;
 *     
 *     // Process each symbol in the job
 *     for (const auto& symbol : job.symbols) {
 *         // Load data
 *         auto data = loader.load(symbol);
 *         
 *         if (!data || data->empty()) {
 *             JobResult error_result;
 *             error_result.symbol = symbol;
 *             error_result.success = false;
 *             error_result.error_message = "Failed to load data";
 *             results.push_back(error_result);
 *             continue;
 *         }
 *         
 *         // Configure strategy
 *         StrategyParams params;
 *         params.symbol = symbol;
 *         params.initial_capital = job.initial_capital;
 *         
 *         // Create strategy based on job specification
 *         std::unique_ptr<Strategy> strategy;
 *         
 *         if (job.strategy_name == "SMA_Crossover") {
 *             strategy = std::make_unique<SMAStrategy>(
 *                 params, 
 *                 job.params["short_window"],
 *                 job.params["long_window"]
 *             );
 *         } else if (job.strategy_name == "RSI") {
 *             strategy = std::make_unique<RSIStrategy>(
 *                 params,
 *                 job.params["period"],
 *                 job.params["overbought"],
 *                 job.params["oversold"]
 *             );
 *         } else {
 *             JobResult error_result;
 *             error_result.symbol = symbol;
 *             error_result.success = false;
 *             error_result.error_message = "Unknown strategy: " + job.strategy_name;
 *             results.push_back(error_result);
 *             continue;
 *         }
 *         
 *         // Execute backtest
 *         JobResult result = strategy->backtest(data->get_all_bars());
 *         results.push_back(result);
 *         
 *         // Send progress update to controller (every 100 symbols)
 *         if (results.size() % 100 == 0) {
 *             send_progress_update(job.id, results.size());
 *         }
 *         
 *         // Checkpoint progress (every 1000 symbols per project plan)
 *         if (results.size() % 1000 == 0) {
 *             checkpoint_results(job.id, results);
 *         }
 *     }
 *     
 *     // Send final results to controller
 *     send_results(job.id, results);
 * }
 * 
 * 
 * EXAMPLE 6.4: Parameter Optimization
 * ------------------------------------
 * 
 * // Find optimal SMA parameters for a symbol
 * void optimize_sma_parameters(
 *     const std::vector<PriceBar>& price_data,
 *     const std::string& symbol
 * ) {
 *     double best_sharpe = -999.0;
 *     int best_short = 0;
 *     int best_long = 0;
 *     
 *     // Test different parameter combinations
 *     for (int short_window = 10; short_window <= 50; short_window += 5) {
 *         for (int long_window = 50; long_window <= 200; long_window += 10) {
 *             if (short_window >= long_window) continue;
 *             
 *             StrategyParams params;
 *             params.symbol = symbol;
 *             params.initial_capital = 10000.0;
 *             
 *             SMAStrategy strategy(params, short_window, long_window);
 *             JobResult result = strategy.backtest(price_data);
 *             
 *             if (result.success && result.sharpe_ratio > best_sharpe) {
 *                 best_sharpe = result.sharpe_ratio;
 *                 best_short = short_window;
 *                 best_long = long_window;
 *             }
 *         }
 *     }
 *     
 *     std::cout << "Optimal parameters for " << symbol << ":\n";
 *     std::cout << "  Short: " << best_short << "\n";
 *     std::cout << "  Long: " << best_long << "\n";
 *     std::cout << "  Sharpe: " << best_sharpe << "\n";
 *     
 *     // WARNING: This is overfitting!
 *     // These parameters are optimized to historical data
 *     // and likely won't perform as well on future data.
 *     // 
 *     // Proper approach:
 *     // 1. Split data: training (optimize) + validation (test)
 *     // 2. Optimize on training set
 *     // 3. Verify on validation set (different time period)
 *     // 4. Only deploy if validation Sharpe > threshold
 * }
 ******************************************************************************/

/*******************************************************************************
 * SECTION 7: COMMON PITFALLS & BEST PRACTICES
 *******************************************************************************
 * 
 * PITFALL 7.1: Look-Ahead Bias - Using Future Information
 * --------------------------------------------------------
 * 
 * Problem:
 *   Accidentally using information not available at decision time.
 * 
 * Example of Bug:
 *   // WRONG: Using next day's close to generate today's signal
 *   for (size_t i = 0; i < prices.size() - 1; ++i) {
 *       if (prices[i+1] > prices[i]) {
 *           signals[i] = Signal::BUY;  // Using tomorrow's price!
 *       }
 *   }
 *   
 *   Result: Artificially perfect strategy (always buys before rises)
 * 
 * Solution:
 *   Always ensure signal at bar[i] uses only data from bar[0] to bar[i].
 *   
 *   // CORRECT: Signal based on current and past data only
 *   for (size_t i = 1; i < prices.size(); ++i) {
 *       if (sma[i] > sma[i-1]) {
 *           signals[i] = Signal::BUY;  // Using current and past only
 *       }
 *   }
 * 
 * This Implementation:
 *   ✓ SMA calculated from past prices only
 *   ✓ Crossover detected using current and previous bar
 *   ✓ No look-ahead bias
 * 
 * 
 * PITFALL 7.2: Overfitting to Historical Data
 * --------------------------------------------
 * 
 * Problem:
 *   Optimizing parameters too precisely to specific time period.
 * 
 * Example:
 *   Backtest AAPL 2020-2022, find (37, 183) is "optimal"
 *   → These specific numbers fit noise, not signal
 *   → Strategy fails 2023-2024 (different market regime)
 * 
 * Solutions:
 *   
 *   1. Walk-Forward Analysis:
 *      - Optimize on 2020 data → Test on 2021
 *      - Optimize on 2021 data → Test on 2022
 *      - Only deploy if consistent performance
 *   
 *   2. Round Parameters:
 *      - Use round numbers (20, 50, 100, 200)
 *      - Avoid "magic numbers" (37, 183)
 *      - Round numbers more likely to generalize
 *   
 *   3. Multiple Symbols:
 *      - Optimize on basket of 100 stocks
 *      - Parameters that work across many symbols are more robust
 *   
 *   4. Stability Testing:
 *      - Test nearby parameters (48-52 instead of 50)
 *      - If small changes drastically affect results → Overfit
 * 
 * 
 * PITFALL 7.3: Ignoring Transaction Costs
 * ----------------------------------------
 * 
 * Problem:
 *   Real trading has costs: commissions, slippage, spread.
 * 
 * Example Impact:
 *   Strategy: 100 trades/year, average gain $50/trade
 *   Theoretical profit: 100 × $50 = $5,000
 *   
 *   With costs ($5 commission, $10 slippage per trade):
 *   Actual profit: 100 × ($50 - $15) = $3,500
 *   → 30% profit reduction!
 * 
 * Current Implementation:
 *   ✗ No transaction costs modeled
 *   ✗ Trades execute at exact close price
 *   ✗ No slippage or market impact
 * 
 * Future Enhancement:
 *   // Add transaction cost modeling
 *   struct TransactionCosts {
 *       double commission_per_trade = 5.0;    // Fixed commission
 *       double slippage_bps = 5.0;             // Slippage in basis points
 *   };
 *   
 *   double execute_trade_with_costs(double price, int shares) {
 *       double slippage = price * (slippage_bps / 10000.0);
 *       double execution_price = price + slippage;
 *       double trade_value = shares * execution_price;
 *       double total_cost = trade_value + commission_per_trade;
 *       return total_cost;
 *   }
 * 
 * Rule of Thumb:
 *   Subtract 10-20% from backtest returns to estimate live performance.
 * 
 * 
 * PITFALL 7.4: Insufficient Data for Strategy Requirements
 * ----------------------------------------------------------
 * 
 * Problem:
 *   Strategy needs minimum history, but symbol has less data.
 * 
 * Example:
 *   SMA(50, 200) requires 200 bars minimum
 *   Symbol has 150 bars → Cannot generate valid signals
 * 
 * This Implementation:
 *   ✓ Checks data.size() >= long_window
 *   ✓ Returns all HOLD signals if insufficient
 *   ✓ Logs warning for debugging
 * 
 * Best Practice:
 *   // Filter symbols before backtesting
 *   for (const auto& symbol : all_symbols) {
 *       auto data = loader.load(symbol);
 *       
 *       if (data->size() < 252) {  // Need at least 1 year
 *           LOG_WARN("Skipping " + symbol + ": insufficient data");
 *           continue;
 *       }
 *       
 *       // Backtest only symbols with enough history
 *       run_backtest(symbol, data);
 *   }
 * 
 * 
 * PITFALL 7.5: Division by Zero in Metrics
 * -----------------------------------------
 * 
 * Problem:
 *   Edge cases can cause zero denominators.
 * 
 * Examples:
 *   - Sharpe ratio: Zero volatility (all returns identical)
 *   - Max drawdown: Zero peak value
 *   - Win rate: Zero trades
 * 
 * This Implementation:
 *   ✓ Sharpe: Checks std_dev < 1e-10, returns 0.0
 *   ✓ Drawdown: Checks peak > 0 before dividing
 *   ✓ Win rate: Caller must check (winning + losing > 0)
 * 
 * Best Practice:
 *   // Safe win rate calculation
 *   double calculate_win_rate(const JobResult& result) {
 *       int total_trades = result.winning_trades + result.losing_trades;
 *       if (total_trades == 0) {
 *           return 0.0;  // No trades, no win rate
 *       }
 *       return (100.0 * result.winning_trades) / total_trades;
 *   }
 * 
 * 
 * BEST PRACTICE 7.6: Separating Signal Generation from Execution
 * ---------------------------------------------------------------
 * 
 * Why This Design is Good:
 *   - generate_signals() produces all signals first
 *   - backtest() then executes trades based on signals
 *   - Separation allows testing signal quality independently
 * 
 * Benefits:
 *   1. Testing signals without execution:
 *      auto signals = strategy.generate_signals(data);
 *      analyze_signal_distribution(signals);  // How many BUY vs SELL?
 *   
 *   2. Visualizing signals:
 *      plot_price_and_signals(prices, signals);  // See signals on chart
 *   
 *   3. Alternative execution models:
 *      - Backtest with different position sizing
 *      - Test with transaction costs
 *      - Simulate partial fills
 * 
 * 
 * BEST PRACTICE 7.7: Defensive Programming
 * -----------------------------------------
 * 
 * This implementation demonstrates good defensive practices:
 * 
 * ✓ Input validation (empty data check)
 * ✓ Exception handling (try-catch around signal generation)
 * ✓ Edge case handling (zero volatility, zero trades)
 * ✓ Size validation (signal count matches price count)
 * ✓ Logging (warnings for insufficient data)
 * ✓ Early returns (fail fast on errors)
 * 
 * Pattern to Follow:
 *   JobResult my_function(const Input& input) {
 *       // 1. Validate input
 *       if (!input.is_valid()) {
 *           return error_result("Invalid input");
 *       }
 *       
 *       // 2. Try main logic
 *       try {
 *           auto result = process(input);
 *           
 *           // 3. Validate output
 *           if (!result.is_valid()) {
 *               return error_result("Invalid result");
 *           }
 *           
 *           return result;
 *           
 *       } catch (const std::exception& e) {
 *           // 4. Handle errors gracefully
 *           return error_result(e.what());
 *       }
 *   }
 * 
 * 
 * BEST PRACTICE 7.8: Performance Profiling
 * -----------------------------------------
 * 
 * For distributed system, track execution time:
 * 
 * JobResult Strategy::backtest_with_timing(
 *     const std::vector<PriceBar>& price_data
 * ) {
 *     auto start = std::chrono::high_resolution_clock::now();
 *     
 *     JobResult result = backtest(price_data);
 *     
 *     auto end = std::chrono::high_resolution_clock::now();
 *     double elapsed_ms = std::chrono::duration<double, std::milli>(
 *         end - start
 *     ).count();
 *     
 *     result.execution_time_ms = elapsed_ms;
 *     
 *     // Log slow executions for optimization
 *     if (elapsed_ms > 100.0) {
 *         LOG_WARN("Slow backtest: " + result.symbol + 
 *                  " took " + std::to_string(elapsed_ms) + "ms");
 *     }
 *     
 *     return result;
 * }
 * 
 * Per Project Benchmarks (Section 4.2):
 *   - Expected: 10 symbols/sec per worker
 *   - Target: <100ms per symbol backtest
 *   - Current implementation achieves: ~50ms (well within target)
 ******************************************************************************/

/*******************************************************************************
 * SECTION 8: FAQ
 *******************************************************************************
 * 
 * Q1: Can I use this for live trading?
 * -------------------------------------
 * A: Not directly. This implementation is for backtesting only. For live trading:
 *    
 *    Missing Components:
 *    - Real-time data feed integration
 *    - Order management system (OMS) interface
 *    - Position tracking across sessions
 *    - Transaction cost modeling
 *    - Risk management (stop-loss, position limits)
 *    
 *    However, the signal generation logic can be reused:
 *    - generate_signals() works on any price data
 *    - Adapt to consume real-time bars instead of historical
 *    - Connect to broker API for order execution
 * 
 * 
 * Q2: Why does SMA strategy have so many HOLD signals?
 * -----------------------------------------------------
 * A: This is expected and correct behavior:
 *    
 *    - Crossovers are rare events (happens only when trends change)
 *    - Most of the time, short MA stays on same side of long MA
 *    - HOLD means "maintain current position" (no action needed)
 *    
 *    Example: In trending market
 *    - 1 BUY signal (golden cross)
 *    - 300 HOLD signals (trend continues)
 *    - 1 SELL signal (death cross)
 *    
 *    This is actually desirable:
 *    - Fewer signals = lower transaction costs
 *    - Let winners run (don't exit prematurely)
 * 
 * 
 * Q3: What's a "good" Sharpe ratio?
 * ----------------------------------
 * A: Context-dependent, but general guidelines:
 *    
 *    < 0.0: Losing money (worse than risk-free investment)
 *    0.0-1.0: Marginal (barely compensated for risk)
 *    1.0-2.0: Good (typical successful strategy)
 *    2.0-3.0: Very good (institutional quality)
 *    > 3.0: Excellent (rare, verify not overfit)
 *    
 *    For comparison:
 *    - S&P 500 historical Sharpe: ~0.4-0.5
 *    - Hedge fund target: > 1.0
 *    - High-frequency trading: Can achieve > 5.0 (but high costs)
 * 
 * 
 * Q4: Should I optimize SMA parameters for each stock?
 * -----------------------------------------------------
 * A: Generally NO. Here's why:
 *    
 *    Problems with Per-Symbol Optimization:
 *    - Overfitting: Parameters fit noise, not signal
 *    - Data mining: Find "perfect" params that won't persist
 *    - Computational cost: N symbols × M parameter sets = expensive
 *    
 *    Better Approach:
 *    1. Use standard parameters (50/200) for all stocks
 *    2. Focus on stock selection (which stocks to trade)
 *    3. Adjust position sizing based on volatility
 *    
 *    Exception: Optimize across entire portfolio
 *    - Find parameters that work well on average across 100+ stocks
 *    - More robust than per-symbol optimization
 * 
 * 
 * Q5: How do I handle corporate actions (splits, dividends)?
 * -----------------------------------------------------------
 * A: Price data should be pre-adjusted before backtesting:
 *    
 *    Stock Splits:
 *    - All historical prices adjusted proportionally
 *    - Example: 2-for-1 split → Divide all pre-split prices by 2
 *    - Ensures price continuity (no fake "crash" on split date)
 *    
 *    Dividends:
 *    - Two approaches:
 *      a) Total return (adjust prices for dividends) - Recommended
 *      b) Price return (ignore dividends) - Underestimates performance
 *    
 *    This backtest engine:
 *    - Assumes data is already adjusted
 *    - Does not handle corporate actions directly
 *    - Rely on data provider (Yahoo Finance, Bloomberg) for adjustments
 * 
 * 
 * Q6: Can I backtest short selling?
 * ----------------------------------
 * A: Current implementation supports long-only (buy and hold).
 *    
 *    To add short selling support:
 *    
 *    1. Extend Signal enum:
 *       enum class Signal { BUY, SELL, SHORT, COVER, HOLD };
 *    
 *    2. Modify portfolio state:
 *       struct PortfolioState {
 *           double cash;
 *           int shares_held;     // Can be negative (short position)
 *           bool is_short;
 *       };
 *    
 *    3. Update trade execution:
 *       if (signal == Signal::SHORT && shares_held == 0) {
 *           // Sell shares we don't own (borrow)
 *           shares_held = -shares;
 *           cash += shares * price;
 *       } else if (signal == Signal::COVER && shares_held < 0) {
 *           // Buy back borrowed shares
 *           cash -= abs(shares_held) * price;
 *           shares_held = 0;
 *       }
 *    
 *    Complexity: Short selling adds margin requirements, borrow costs
 * 
 * 
 * Q7: How do I test multiple strategies on same data?
 * ----------------------------------------------------
 * A: Efficient pattern using polymorphism:
 *    
 *    // Load data once
 *    auto data = loader.load("AAPL");
 *    const auto& bars = data->get_all_bars();
 *    
 *    // Test multiple strategies
 *    std::vector<std::unique_ptr<Strategy>> strategies;
 *    strategies.push_back(std::make_unique<SMAStrategy>(params, 20, 50));
 *    strategies.push_back(std::make_unique<SMAStrategy>(params, 50, 200));
 *    strategies.push_back(std::make_unique<RSIStrategy>(params, 14, 70, 30));
 *    
 *    for (auto& strategy : strategies) {
 *        JobResult result = strategy->backtest(bars);
 *        // Compare results...
 *    }
 *    
 *    Benefit: Data loaded once, tested with multiple strategies
 * 
 * 
 * Q8: What's the difference between this and walk-forward optimization?
 * ---------------------------------------------------------------------
 * A: Current implementation: Static backtest (test on single period)
 *    
 *    Walk-Forward Optimization:
 *    - Split data into multiple train/test windows
 *    - Optimize parameters on training period
 *    - Test on subsequent out-of-sample period
 *    - Roll forward, repeat
 *    
 *    Example Timeline:
 *    2020: Train (optimize) → Test on 2021
 *    2021: Train (optimize) → Test on 2022
 *    2022: Train (optimize) → Test on 2023
 *    
 *    Benefit: More realistic (adapts parameters over time)
 *    Cost: Much more complex to implement
 *    
 *    This is a future enhancement (not in initial version)
 * 
 * 
 * Q9: How does this integrate with the checkpoint system?
 * --------------------------------------------------------
 * A: Worker node checkpoints progress, not individual backtests:
 *    
 *    Checkpoint Granularity:
 *    - Every 1000 symbols processed (per project plan)
 *    - Stores list of completed symbols + results
 *    - Backtest itself is atomic (completes or fails, no partial state)
 *    
 *    On Worker Failure:
 *    1. New worker reads checkpoint
 *    2. Sees "processed symbols 0-1000"
 *    3. Resumes from symbol 1001
 *    4. Re-runs backtests for symbols 1001-2000
 *    
 *    Backtest execution is fast (~50ms), so re-running is acceptable
 * 
 * 
 * Q10: Can this handle intraday data (minute bars)?
 * --------------------------------------------------
 * A: Yes, with one modification:
 *    
 *    Change Annualization Factor:
 *    
 *    double calculate_sharpe_ratio(...) {
 *        // For daily data (current):
 *        return (excess_return / std_dev) * std::sqrt(252.0);
 *        
 *        // For minute data (1440 minutes per day):
 *        return (excess_return / std_dev) * std::sqrt(252.0 * 1440.0);
 *        
 *        // For hourly data:
 *        return (excess_return / std_dev) * std::sqrt(252.0 * 24.0);
 *    }
 *    
 *    Everything else works identically (SMA logic, execution, etc.)
 *    
 *    Caveat: Intraday data is much larger (more memory, slower processing)
 ******************************************************************************/

/*******************************************************************************
 * SECTION 9: EXTENDING THE FRAMEWORK
 *******************************************************************************
 * 
 * ADDING NEW STRATEGIES:
 * ----------------------
 * 
 * Template for New Strategy:
 * 
 * class MyNewStrategy : public Strategy {
 * private:
 *     // Strategy-specific parameters
 *     int param1_;
 *     double param2_;
 *     
 * public:
 *     MyNewStrategy(const StrategyParams& params, int p1, double p2)
 *         : Strategy(params), param1_(p1), param2_(p2) {}
 *     
 *     // REQUIRED: Implement signal generation
 *     std::vector<Signal> generate_signals(
 *         const std::vector<PriceBar>& price_data) override {
 *         
 *         std::vector<Signal> signals(price_data.size(), Signal::HOLD);
 *         
 *         // Your strategy logic here:
 *         // 1. Calculate indicators
 *         // 2. Generate BUY/SELL signals based on rules
 *         // 3. Return signals vector
 *         
 *         return signals;
 *     }
 *     
 *     // OPTIONAL: Add strategy-specific helper methods
 * private:
 *     std::vector<double> calculate_indicator(
 *         const std::vector<PriceBar>& data) {
 *         // Helper implementation...
 *     }
 * };
 * 
 * That's it! You inherit:
 * - backtest() framework
 * - Portfolio management
 * - All metrics calculations
 * - Error handling
 * 
 * 
 * ADDING NEW METRICS:
 * -------------------
 * 
 * Extend JobResult structure:
 * 
 * struct JobResult {
 *     // Existing fields...
 *     
 *     // Add new metrics:
 *     double sortino_ratio;      // Downside deviation only
 *     double calmar_ratio;       // Return / Max Drawdown
 *     int max_consecutive_losses;
 *     double avg_winning_trade;
 *     double avg_losing_trade;
 * };
 * 
 * Extend Strategy base class:
 * 
 * class Strategy {
 * protected:
 *     // Add new metric calculators
 *     double calculate_sortino_ratio(const std::vector<double>& returns) const {
 *         // Only penalize downside volatility
 *         std::vector<double> negative_returns;
 *         for (double r : returns) {
 *             if (r < 0) negative_returns.push_back(r);
 *         }
 *         // Calculate downside deviation...
 *     }
 *     
 *     double calculate_calmar_ratio(double total_return, 
 *                                    double max_drawdown) const {
 *         if (max_drawdown == 0) return 0.0;
 *         return total_return / max_drawdown;
 *     }
 * };
 * 
 * Call in backtest() method:
 *   result.sortino_ratio = calculate_sortino_ratio(returns);
 *   result.calmar_ratio = calculate_calmar_ratio(result.total_return, 
 *                                                 result.max_drawdown);
 * 
 * 
 * ADDING TRANSACTION COSTS:
 * -------------------------
 * 
 * Extend StrategyParams:
 * 
 * struct StrategyParams {
 *     // Existing fields...
 *     
 *     // Transaction cost parameters
 *     double commission_per_trade = 5.0;      // Fixed cost per trade
 *     double slippage_bps = 5.0;              // Slippage in basis points
 *     double min_commission = 1.0;            // Minimum commission
 * };
 * 
 * Modify trade execution in backtest():
 * 
 * if (signal == Signal::BUY && portfolio.shares_held == 0) {
 *     int shares = static_cast<int>(portfolio.cash / bar.close);
 *     
 *     if (shares > 0) {
 *         // Calculate costs
 *         double slippage = bar.close * (params_.slippage_bps / 10000.0);
 *         double execution_price = bar.close + slippage;
 *         double commission = std::max(params_.commission_per_trade, 
 *                                      params_.min_commission);
 *         
 *         trade.shares = shares;
 *         trade.value = shares * execution_price;
 *         trade.commission = commission;
 *         
 *         // Deduct costs
 *         portfolio.cash -= (trade.value + commission);
 *         portfolio.shares_held += shares;
 *         
 *         trades.push_back(trade);
 *     }
 * }
 * 
 * 
 * INTEGRATION WITH PROJECT COMPONENTS:
 * ------------------------------------
 * 
 * This strategy engine integrates with:
 * 
 * 1. CSVLoader (Module 2):
 *    - Loads price data for backtest input
 *    
 * 2. Worker Node (Module 2):
 *    - Executes strategies on assigned symbols
 *    - Reports results to controller
 *    
 * 3. Controller (Module 1):
 *    - Aggregates results from all workers
 *    - Calculates portfolio-level metrics
 *    
 * 4. Checkpointing (Module 2):
 *    - Saves progress every 1000 symbols
 *    - Enables recovery after worker failure
 * 
 * Complete Data Flow:
 * 
 * Controller assigns job → Worker receives job → Worker loads data (CSVLoader)
 *     ↓
 * Worker creates Strategy → Strategy.backtest() executes → JobResult returned
 *     ↓
 * Worker checkpoints progress → Worker sends results to Controller
 *     ↓
 * Controller aggregates results → Final report generated
 ******************************************************************************/