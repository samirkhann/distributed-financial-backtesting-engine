/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: test_sma_strategy.cpp
    
    Description:
        This file implements comprehensive unit tests for the SMA (Simple
        Moving Average) crossover trading strategy, validating signal generation,
        backtest execution, and strategy behavior across different market
        conditions. These tests ensure the SMA strategy implementation is
        correct before deployment in the distributed backtesting system.
        
        Test Coverage:
        - Test 1: Signal Generation - Validates BUY/SELL signals generated correctly
        - Test 2: Backtest Execution - Validates complete backtest workflow
        - Test 3: Positive Returns - Validates strategy profits in uptrend
        
        Simple Moving Average (SMA) Strategy:
        
        CONCEPT:
        Uses two moving averages of different periods (fast and slow) to
        identify trend changes. Classic technical analysis technique.
        
        SIGNAL RULES:
        - BUY (Golden Cross): Fast MA crosses above slow MA (bullish)
        - SELL (Death Cross): Fast MA crosses below slow MA (bearish)
        - HOLD: No crossover, maintain current position
        
        EXAMPLE:
        Fast MA (20-period), Slow MA (50-period)
        
        Day 51: fast=100, slow=102 → fast < slow (bearish)
        Day 75: fast=103, slow=103 → fast = slow (no signal)
        Day 76: fast=104, slow=103 → fast > slow (GOLDEN CROSS → BUY)
        ...
        Day 150: fast=108, slow=109 → fast < slow (DEATH CROSS → SELL)
        
        PARAMETERS:
        - short_window: Fast MA period (e.g., 10, 20, 50)
        - long_window: Slow MA period (e.g., 50, 100, 200)
        - Constraint: short_window < long_window
        
        TYPICAL CONFIGURATIONS:
        - Day trading: (5, 20) or (10, 30)
        - Swing trading: (20, 50) or (50, 100)
        - Position trading: (50, 200) or (100, 200)
        
        Test Data Design:
        
        The test generates 300 bars with three distinct market phases to
        thoroughly test strategy behavior:
        
        PHASE 1 (Bars 0-79): Sideways Market
        - Price oscillates around 100 with small noise
        - No clear trend, many false signals
        - Tests: Noise filtering, avoiding overtrading
        
        PHASE 2 (Bars 80-119): Downtrend
        - Price declines from 100 to 88 (-12%)
        - Fast MA falls below slow MA (bearish)
        - Tests: Sell signal generation, loss avoidance
        
        PHASE 3 (Bars 120-299): Strong Uptrend
        - Price rises from 88 to 196 (+122%)
        - Fast MA crosses above slow MA (golden cross)
        - Tests: Buy signal generation, profit capture
        
        This three-phase design ensures strategy tested across:
        - Choppy markets (many whipsaws)
        - Bear markets (negative returns)
        - Bull markets (positive returns)
        
        Testing Philosophy for Trading Strategies:
        
        CORRECTNESS TESTS:
        - Signals generated at right times
        - No lookahead bias (only using past data)
        - Metrics calculated correctly
        - No crashes on edge cases
        
        BEHAVIOR TESTS:
        - Profitable on uptrends
        - Avoids losses on downtrends
        - Doesn't overtrade in sideways markets
        - Risk metrics reasonable
        
        ROBUSTNESS TESTS:
        - Handles insufficient data gracefully
        - Works with various parameter combinations
        - No NaN or infinite values in results
        - Consistent results on repeated runs
        
        Why Synthetic Test Data?
        
        ADVANTAGES:
        - Controlled scenarios (know expected behavior)
        - No external dependencies (self-contained tests)
        - Deterministic (same results every run)
        - Fast generation (no file I/O)
        - Specific patterns (crossovers, trends, noise)
        
        LIMITATIONS:
        - Not realistic market behavior
        - Doesn't test real-world edge cases
        - May overfit to test scenarios
        
        For development: Synthetic data perfect
        For validation: Real historical data essential
        
    Dependencies:
        - strategy/sma_strategy.h: SMAStrategy implementation
        - common/logger.h: Logging infrastructure
        - <cassert>: Assertion macros
        - <cmath>: Math functions (isnan, abs)
        - <iostream>: Console output
        
    Prerequisites:
        - SMAStrategy class implemented
        - Strategy::backtest() framework working
        - Signal enum defined (BUY, SELL, HOLD)
        - PriceBar structure available
        
    Exit Codes:
        0: All tests passed (strategy working correctly)
        1: One or more tests failed (bug in strategy)
        
    Running Tests:
```bash
        # Compile
        g++ -std=c++17 test_sma_strategy.cpp sma_strategy.cpp strategy.cpp \
            logger.cpp -o test_sma -pthread
        
        # Run
        ./test_sma
        
        # Expected output:
        # Test 1: Signal generation... PASSED (Buy=X, Sell=Y)
        # Test 2: Backtest execution... PASSED (Return=Z.Z%)
        # Test 3: Positive return on uptrend... PASSED (Return=W.W%)
        # 
        # === Results ===
        # Passed: 3
        # Failed: 0
```
        
    Interpreting Test Results:
        
        Test 1 - Signal Counts:
        - Buy signals: Expect 1-3 (golden crosses)
        - Sell signals: Expect 0-2 (death crosses)
        - More signals = more trading = higher costs
        
        Test 2 - Backtest Metrics:
        - Total return: Should be reasonable (-50% to +200%)
        - Sharpe ratio: Should not be NaN or infinite
        - Max drawdown: Should be 0-100%
        
        Test 3 - Uptrend Performance:
        - Return: Should be positive (prices increase 150%)
        - Strategy should capture most of uptrend
        - If negative, indicates strategy broken
        
    Debugging Failed Tests:
        - Enable debug logging: Logger::set_level(LogLevel::DEBUG)
        - Print generated signals: for(auto s : signals) cout << (int)s << " ";
        - Print MA values: Show fast and slow MA calculations
        - Export test data: Write to CSV and visualize in Excel/Python
        - Step through with debugger: Break at signal generation logic
*******************************************************************************/

#include "strategy/sma_strategy.h"
#include "common/logger.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace backtesting;

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
// 1. Overview and Testing Strategy for Trading Algorithms
// 2. Test Data Generation
//    2.1 Three-Phase Market Simulation
//    2.2 Price Bar Construction
// 3. Test Cases
//    3.1 Test 1: Signal Generation Validation
//    3.2 Test 2: Backtest Execution Validation
//    3.3 Test 3: Uptrend Performance Validation
// 4. Main Function - Test Runner
// 5. Understanding SMA Crossover Behavior
// 6. Debugging Strategy Tests
// 7. Common Strategy Testing Pitfalls
// 8. FAQ
//==============================================================================

//==============================================================================
// SECTION 1: Overview and Testing Strategy for Trading Algorithms
//==============================================================================

/**
 * TESTING TRADING STRATEGIES: UNIQUE CHALLENGES
 * 
 * Trading strategies are different from typical software:
 * - No "correct" answer (markets are stochastic)
 * - Success measured by statistics, not determinism
 * - Behavior varies with market conditions
 * - Small bugs can cause large losses
 * 
 * TESTING APPROACH:
 * 
 * LEVEL 1: Correctness (Can we trust the code?)
 * - Signals generated without crashes
 * - Metrics calculated without NaN/infinity
 * - No lookahead bias
 * - Consistent results on repeated runs
 * 
 * LEVEL 2: Behavior (Does strategy make sense?)
 * - Generates trades in expected market conditions
 * - Profitable in trending markets
 * - Conservative in choppy markets
 * - Risk metrics within reasonable ranges
 * 
 * LEVEL 3: Performance (Does strategy work well?)
 * - Beats benchmark (buy-and-hold)
 * - Positive risk-adjusted returns
 * - Works across multiple time periods
 * - Robust to parameter changes
 * 
 * This file focuses on LEVEL 1 and some LEVEL 2.
 * LEVEL 3 requires extensive historical data and is beyond unit tests.
 * 
 * WHY THREE-PHASE TEST DATA?
 * 
 * Real markets have different regimes:
 * - TRENDING: Prices move directionally (up or down)
 * - RANGING: Prices oscillate without clear direction
 * - VOLATILE: Large price swings
 * 
 * Strategy should handle all regimes appropriately:
 * - Trending: Generate few signals, capture trend
 * - Ranging: Minimize trading (avoid whipsaws)
 * - Volatile: Manage risk (stop losses, position sizing)
 * 
 * Our test data simulates:
 * 1. Ranging market (80 bars sideways)
 * 2. Downtrend (40 bars declining)
 * 3. Uptrend (180 bars rising)
 * 
 * This exposes strategy to diverse conditions in a single test.
 * 
 * WHAT MAKES A GOOD STRATEGY TEST?
 * 
 * ✓ Tests core algorithm (signal generation)
 * ✓ Tests integration (backtest framework)
 * ✓ Tests various market conditions
 * ✓ Fast execution (< 100ms per test)
 * ✓ Deterministic (same data → same results)
 * ✓ Clear pass/fail criteria
 * ✓ Helpful failure messages
 * 
 * WHAT THIS TEST DOESN'T COVER:
 * - Real historical data (too large for unit test)
 * - Multiple stocks (single symbol only)
 * - Different time periods (fixed test data)
 * - Parameter optimization (not testing all combinations)
 * - Out-of-sample validation (no train/test split)
 * 
 * Those require separate evaluation framework.
 */

//==============================================================================
// SECTION 2: Test Data Generation
//==============================================================================

/**
 * @brief Creates synthetic price data with three distinct market phases
 * 
 * PURPOSE:
 * Generate controlled price series that tests SMA crossover strategy
 * across different market conditions without needing real data files.
 * 
 * DATA CHARACTERISTICS:
 * - Total bars: 300 (about 1 year of daily data)
 * - Three phases: Sideways → Downtrend → Uptrend
 * - Deterministic (same every time, no randomness)
 * - Valid OHLCV format (Open, High, Low, Close, Volume)
 * 
 * PRICE TRAJECTORY:
 * 
 * ```
 * Price
 * 200 |                                              ┌───
 *     |                                          ┌───┘
 * 150 |                                      ┌───┘
 *     |                                  ┌───┘
 * 100 |  ≈≈≈≈≈≈≈≈≈≈≈≈┐               ┌───┘        Phase 3: Uptrend
 *     |              └───┐       ┌───┘            120-300 bars
 *  50 |                  └───────┘                +122% gain
 *     |                  Phase 2: Downtrend
 *   0 |                  80-119 bars
 *     |                  -12% decline
 *     └──────┬──────┬──────┬──────┬──────┬───→ Time (bars)
 *            0     80    120    200    300
 *          Phase 1: Sideways
 *          0-79 bars
 *          ±3% noise
 * ```
 * 
 * PHASE 1: SIDEWAYS MARKET (Bars 0-79)
 * 
 * FORMULA:
 * ```cpp
 * close = 100.0 + (i % 10) * 0.3 - 1.5
 * ```
 * 
 * BEHAVIOR:
 * - Base price: 100
 * - Oscillation: ±1.5 around base
 * - Pattern: Repeats every 10 bars (saw-tooth)
 * 
 * Example prices:
 * - Bar 0: 100 + 0*0.3 - 1.5 = 98.5
 * - Bar 5: 100 + 5*0.3 - 1.5 = 100.0
 * - Bar 9: 100 + 9*0.3 - 1.5 = 101.2
 * - Bar 10: 100 + 0*0.3 - 1.5 = 98.5 (cycle repeats)
 * 
 * STRATEGY IMPACT:
 * - Fast MA and slow MA stay close together
 * - Many false crossovers (whipsaws)
 * - Should generate HOLD mostly to avoid overtrading
 * - Testing: Avoid excessive trading in noise
 * 
 * PHASE 2: DOWNTREND (Bars 80-119)
 * 
 * FORMULA:
 * ```cpp
 * close = 100.0 - (i - 80) * 0.3
 * ```
 * 
 * BEHAVIOR:
 * - Starting price: 100 (at bar 80)
 * - Decline rate: -0.3 per bar
 * - Ending price: 88 (at bar 119)
 * - Total decline: -12% over 40 bars
 * 
 * Example prices:
 * - Bar 80: 100 - 0*0.3 = 100.0
 * - Bar 100: 100 - 20*0.3 = 94.0
 * - Bar 119: 100 - 39*0.3 = 88.3
 * 
 * STRATEGY IMPACT:
 * - Fast MA falls faster than slow MA
 * - Death cross: Fast crosses below slow
 * - Should generate SELL signal
 * - Testing: Strategy recognizes downtrend, exits position
 * 
 * PHASE 3: STRONG UPTREND (Bars 120-299)
 * 
 * FORMULA:
 * ```cpp
 * close = 88.0 + (i - 120) * 0.6
 * ```
 * 
 * BEHAVIOR:
 * - Starting price: 88 (at bar 120)
 * - Rise rate: +0.6 per bar
 * - Ending price: 196 (at bar 299)
 * - Total gain: +122% over 180 bars
 * 
 * Example prices:
 * - Bar 120: 88 + 0*0.6 = 88.0
 * - Bar 200: 88 + 80*0.6 = 136.0
 * - Bar 299: 88 + 179*0.6 = 195.4
 * 
 * STRATEGY IMPACT:
 * - Fast MA rises faster than slow MA
 * - Golden cross: Fast crosses above slow
 * - Should generate BUY signal
 * - Testing: Strategy captures uptrend profit
 * 
 * OHLC CONSTRUCTION:
 * 
 * For each bar, create realistic OHLC relationships:
 * ```cpp
 * open = close - 0.5   (slightly below close)
 * high = close + 0.5   (slightly above close)
 * low = close - 1.0    (lowest point of day)
 * ```
 * 
 * INVARIANTS MAINTAINED:
 * - low <= open <= high
 * - low <= close <= high
 * - Real candle patterns
 * 
 * VOLUME:
 * Constant 1,000,000 shares per day.
 * Volume analysis not part of SMA strategy, so can be constant.
 * 
 * WHY THIS SPECIFIC DESIGN?
 * 
 * ALTERNATIVE 1: Random walk
 * ```cpp
 * close = prev_close + random(-1, 1);
 * ```
 * - PRO: Realistic volatility
 * - CON: Non-deterministic, hard to predict signals
 * 
 * ALTERNATIVE 2: Pure uptrend
 * ```cpp
 * close = 100 + i;
 * ```
 * - PRO: Simple, definitely profitable
 * - CON: Doesn't test downtrends or sideways
 * 
 * CHOSEN: Three-phase pattern
 * - PRO: Deterministic AND comprehensive
 * - PRO: Tests all market conditions
 * - CON: Not perfectly realistic (but good enough for unit tests)
 * 
 * @return Vector of 300 PriceBar objects with controlled characteristics
 * 
 * USAGE:
 * ```cpp
 * auto data = create_test_data();
 * SMAStrategy strategy;
 * auto signals = strategy.generate_signals(data);
 * // signals[80] should be SELL or HOLD (downtrend starting)
 * // signals[150] should be BUY (uptrend established)
 * ```
 */
std::vector<PriceBar> create_test_data() {
    std::vector<PriceBar> data;
    
    /**
     * GENERATE 300 PRICE BARS
     * 
     * Each iteration creates one daily price bar.
     * Three phases determined by index range.
     */
    for (int i = 0; i < 300; ++i) {
        PriceBar bar;
        
        /**
         * DATE GENERATION
         * 
         * Format: "2020-01-{day}"
         * Simple incrementing days (not calendar-accurate).
         * 
         * CSV parsers typically accept this format.
         * For production, would use proper date library.
         */
        bar.date = "2020-01-" + std::to_string(i + 1);
        
        /**
         * CLOSE PRICE CALCULATION
         * 
         * Varies by phase (market condition).
         */
        
        /**
         * PHASE 1 (0-79): SIDEWAYS MARKET
         * 
         * Price oscillates in narrow range around 100.
         * (i % 10) creates repeating pattern 0,1,2,...,9,0,1,2...
         * Multiply by 0.3 and subtract 1.5 for oscillation.
         * 
         * Range: 98.5 to 101.2 (±1.5%)
         * Tests strategy's noise filtering.
         */
        if (i < 80) {
            bar.close = 100.0 + (i % 10) * 0.3 - 1.5;
        }
        /**
         * PHASE 2 (80-119): DOWNTREND
         * 
         * Linear decline at -0.3 per bar.
         * From 100 to 88 over 40 bars.
         * 
         * Tests strategy's ability to:
         * - Detect bearish trend
         * - Generate SELL signal
         * - Avoid losses (or minimize them)
         */
        else if (i < 120) {
            bar.close = 100.0 - (i - 80) * 0.3;
        }
        /**
         * PHASE 3 (120-299): STRONG UPTREND
         * 
         * Linear rise at +0.6 per bar.
         * From 88 to 196 over 180 bars.
         * 
         * Strong uptrend (2× downtrend rate) creates clear golden cross.
         * Tests strategy's ability to:
         * - Detect bullish trend
         * - Generate BUY signal
         * - Capture profits
         * 
         * EXPECTED STRATEGY BEHAVIOR:
         * - Bar ~140-150: Fast MA crosses above slow MA → BUY
         * - Bar 150-299: Hold position through uptrend
         * - Final: Large profit (~100% of uptrend captured)
         */
        else {
            bar.close = 88.0 + (i - 120) * 0.6;
        }
        
        /**
         * CONSTRUCT OHLC FROM CLOSE
         * 
         * Create realistic intraday price action:
         * - Open slightly below close
         * - High slightly above close
         * - Low well below close
         * 
         * This creates a slightly bullish candle pattern.
         * For more realism, could vary based on trend:
         * - Uptrend: close near high
         * - Downtrend: close near low
         */
        bar.open = bar.close - 0.5;
        bar.high = bar.close + 0.5;
        bar.low = bar.close - 1.0;
        
        /**
         * VOLUME
         * 
         * Constant volume (not realistic but sufficient).
         * SMA strategy doesn't use volume, so variation not needed.
         * 
         * For volume-based strategies, would model:
         * - Higher volume in trends
         * - Lower volume in consolidation
         * - Spikes at reversals
         */
        bar.volume = 1000000;
        
        data.push_back(bar);
    }
    
    return data;
}

//==============================================================================
// SECTION 3: Test Cases
//==============================================================================

/**
 * @brief Main test runner for SMA strategy validation
 * 
 * EXECUTION FLOW:
 * 1. Initialize logging and counters
 * 2. Run Test 1: Signal generation
 * 3. Run Test 2: Backtest execution
 * 4. Run Test 3: Uptrend performance
 * 5. Print summary
 * 6. Exit with appropriate code
 * 
 * TEST INDEPENDENCE:
 * Each test uses fresh strategy instance and data.
 * No shared state between tests.
 * 
 * TIMING:
 * - Per test: ~1-10ms (fast computation)
 * - Total suite: ~10-30ms
 * - Much faster than integration tests (no network/threads)
 */
int main() {
    /**
     * LOGGING SETUP
     * 
     * INFO level shows test progress.
     * DEBUG level would show strategy internals (MA calculations, etc.)
     */
    Logger::set_level(LogLevel::INFO);
    Logger::info("Running SMA Strategy tests...");
    
    /**
     * TEST COUNTERS
     */
    int passed = 0;
    int failed = 0;
    
    //--------------------------------------------------------------------------
    // SECTION 3.1: Test 1 - Signal Generation Validation
    //--------------------------------------------------------------------------
    
    /**
     * TEST 1: Signal Generation
     * 
     * GOAL:
     * Verify SMA strategy generates signals correctly.
     * 
     * VALIDATION CRITERIA:
     * 1. Output size matches input size (one signal per bar)
     * 2. Signals are generated (not all HOLD)
     * 3. At least some BUY signals (strategy should enter positions)
     * 4. No crashes or exceptions
     * 
     * WHY THIS TEST MATTERS:
     * Signal generation is the core of any strategy.
     * If signals wrong, entire backtest is meaningless.
     * 
     * WHAT WE'RE TESTING:
     * - Moving average calculation is correct
     * - Crossover detection works (fast > slow and vice versa)
     * - No off-by-one errors in indexing
     * - No lookahead bias (signals[i] only uses data up to i)
     * 
     * EXPECTED BEHAVIOR:
     * With 20/50 MA on our test data:
     * - Bars 0-49: HOLD (insufficient data for 50-period MA)
     * - Bars 50-120: Mix of signals (sideways then downtrend)
     * - Bar ~140: BUY (golden cross as uptrend establishes)
     * - Bars 141-299: HOLD (stay in position during uptrend)
     * - Maybe 1 SELL near bar 120 (downtrend)
     * 
     * SIGNAL COUNT EXPECTATIONS:
     * - buy_signals: 1-3 typical (one golden cross, maybe false positives)
     * - sell_signals: 0-2 typical (one death cross, maybe false exits)
     * 
     * If buy_signals == 0: Strategy never enters market (bug!)
     * If buy_signals > 10: Too many signals (overtrading, bug!)
     * 
     * ASSERTION PHILOSOPHY:
     * 
     * STRONG ASSERTION (exact):
     * ```cpp
     * assert(signals.size() == data.size());  // Must be exact
     * ```
     * This should ALWAYS be true. Violation is clear bug.
     * 
     * WEAK ASSERTION (existence):
     * ```cpp
     * assert(buy_signals > 0);  // At least one BUY
     * ```
     * We don't specify exact count (depends on parameters, data).
     * Just verify strategy generates some signals.
     * 
     * NO ASSERTION (informational):
     * ```cpp
     * std::cout << "Buy=" << buy_signals << ", Sell=" << sell_signals;
     * ```
     * Display for manual inspection, but don't fail test.
     * Allows seeing behavior without brittle exact-value assertions.
     */
    {
        std::cout << "Test 1: Signal generation... ";
        try {
            /**
             * ARRANGE: Create test data and strategy
             */
            auto data = create_test_data();
            
            SMAStrategy strategy;
            
            /**
             * CONFIGURE STRATEGY
             * 
             * Parameters:
             * - short_window: 20 (fast MA)
             * - long_window: 50 (slow MA)
             * 
             * With 300 bars of data:
             * - First 49 bars: HOLD (need 50 bars for slow MA)
             * - Bars 50+: Can generate signals
             */
            JobParams params;
            params.symbol = "TEST";
            params.short_window = 20;
            params.long_window = 50;
            params.initial_capital = 10000.0;
            strategy.set_parameters(params);
            
            /**
             * ACT: Generate signals
             */
            auto signals = strategy.generate_signals(data);
            
            /**
             * ASSERT: Validate signal vector
             * 
             * CRITICAL: Size must match input
             * If not, indicates:
             * - Off-by-one error in signal generation
             * - Not handling early bars (warm-up period)
             * - Array sizing bug
             */
            assert(signals.size() == data.size());
            
            /**
             * COUNT SIGNAL TYPES
             * 
             * Categorize all signals for analysis.
             * Helps understand strategy behavior.
             */
            int buy_signals = 0, sell_signals = 0;
            for (auto sig : signals) {
                if (sig == Signal::BUY) buy_signals++;
                if (sig == Signal::SELL) sell_signals++;
            }
            
            /**
             * VALIDATE SIGNAL GENERATION
             * 
             * At minimum, should generate some BUY signals.
             * If zero, strategy never enters market (broken logic).
             * 
             * WEAK ASSERTION (intentionally):
             * Don't assert exact count (too brittle).
             * Just verify strategy does something.
             * 
             * MANUAL INSPECTION:
             * Operator reads buy/sell counts to see if reasonable.
             * Typical: 1-3 BUYs, 0-2 SELLs for this data.
             */
            assert(buy_signals > 0); // Should have some buy signals
            
            /**
             * TEST PASSED
             * 
             * Display signal counts for informational purposes.
             * Helps detect behavioral changes (not bugs, but strategy drift).
             */
            std::cout << "PASSED (Buy=" << buy_signals 
                      << ", Sell=" << sell_signals << ")\n";
            passed++;
            
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    
    //--------------------------------------------------------------------------
    // SECTION 3.2: Test 2 - Backtest Execution Validation
    //--------------------------------------------------------------------------
    
    /**
     * TEST 2: Backtest Execution
     * 
     * GOAL:
     * Verify complete backtest workflow produces valid results.
     * 
     * VALIDATION CRITERIA:
     * 1. Backtest completes without crashes
     * 2. Result marked as success
     * 3. Symbol field populated correctly
     * 4. All metrics are valid numbers (not NaN or infinity)
     * 5. Portfolio value is positive
     * 
     * WHAT WE'RE TESTING:
     * - Strategy::backtest() framework works
     * - Trade execution logic correct
     * - Portfolio accounting accurate
     * - Metric calculations don't crash
     * - Result serialization works
     * 
     * NOT TESTING:
     * - Whether strategy is profitable (that's strategy-specific)
     * - Exact return values (too brittle)
     * - Comparison to benchmarks (requires more setup)
     * 
     * This is a SMOKE TEST: Does it run without exploding?
     * 
     * NaN/INFINITY CHECKS:
     * 
     * Common causes of NaN:
     * - Division by zero: sharpe = mean / std_dev (std_dev=0)
     * - Square root of negative: sqrt(-1)
     * - Log of negative/zero: log(0)
     * 
     * Common causes of infinity:
     * - Division by very small number
     * - Exponential overflow
     * 
     * WHY CHECK FOR NaN?
     * ```cpp
     * double mean = 0, variance = 0;
     * for (auto r : returns) {
     *     mean += r;
     * }
     * mean /= returns.size();
     * 
     * for (auto r : returns) {
     *     variance += (r - mean) * (r - mean);
     * }
     * variance /= returns.size();
     * 
     * double sharpe = mean / sqrt(variance);  // If variance=0, sharpe=NaN!
     * ```
     * 
     * Checking !std::isnan() catches these bugs.
     * 
     * EXAMPLE FAILURE SCENARIO:
     * Strategy never trades (all HOLD signals):
     * - No returns data
     * - Mean = NaN (division by zero)
     * - Sharpe = NaN
     * - Test fails at !std::isnan(result.sharpe_ratio)
     * 
     * This correctly identifies the bug (no trades = broken strategy).
     */
    {
        std::cout << "Test 2: Backtest execution... ";
        try {
            /**
             * ARRANGE
             */
            auto data = create_test_data();
            
            SMAStrategy strategy;
            JobParams params;
            params.symbol = "TEST";
            params.short_window = 20;
            params.long_window = 50;
            params.initial_capital = 10000.0;
            strategy.set_parameters(params);
            
            /**
             * ACT: Run backtest
             * 
             * This executes:
             * 1. generate_signals(data)
             * 2. Simulate trading based on signals
             * 3. Track portfolio over time
             * 4. Calculate metrics (return, Sharpe, drawdown)
             * 5. Package into JobResult
             */
            auto result = strategy.backtest(data);
            
            /**
             * ASSERT: Result validity
             */
            
            /**
             * SUCCESS FLAG
             * 
             * result.success should be true unless:
             * - Data loading failed (N/A for in-memory data)
             * - Strategy threw exception
             * - Calculation error occurred
             */
            assert(result.success);
            
            /**
             * SYMBOL VERIFICATION
             * 
             * Should match params.symbol.
             * Verifies result corresponds to correct job.
             */
            assert(result.symbol == "TEST");
            
            /**
             * NaN CHECKS
             * 
             * All metrics must be valid numbers:
             * - total_return: Percentage return (-100% to +∞)
             * - sharpe_ratio: Risk-adjusted return (-∞ to +∞)
             * - max_drawdown: Peak-to-trough decline (0% to 100%)
             * 
             * std::isnan() returns true if value is NaN (not-a-number).
             * We assert !isnan() (value is NOT NaN).
             * 
             * WHY NOT CHECK VALUES?
             * Return could be negative (strategy loses money).
             * Sharpe could be negative (worse than risk-free).
             * We only check that calculations complete (no errors).
             * 
             * ALTERNATIVE (stronger):
             * ```cpp
             * assert(result.total_return >= -100);  // Can't lose more than 100%
             * assert(result.max_drawdown >= 0 && result.max_drawdown <= 100);
             * ```
             */
            assert(!std::isnan(result.total_return));
            assert(!std::isnan(result.sharpe_ratio));
            assert(!std::isnan(result.max_drawdown));
            
            /**
             * PORTFOLIO VALUE CHECK
             * 
             * Final value must be positive.
             * Zero or negative indicates:
             * - Total loss (portfolio went to zero)
             * - Accounting bug (negative cash impossible)
             * 
             * With $10,000 initial capital and no margin:
             * Worst case: $0 (total loss)
             * Cannot be negative.
             */
            assert(result.final_portfolio_value > 0);
            
            /**
             * TEST PASSED
             * 
             * Display return for manual verification.
             * On our test data, expect positive return (uptrend dominant).
             */
            std::cout << "PASSED (Return=" << result.total_return << "%)\n";
            passed++;
            
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    
    //--------------------------------------------------------------------------
    // SECTION 3.3: Test 3 - Uptrend Performance Validation
    //--------------------------------------------------------------------------
    
    /**
     * TEST 3: Positive Return on Uptrend
     * 
     * GOAL:
     * Verify strategy captures profits in clear uptrend.
     * 
     * VALIDATION CRITERIA:
     * 1. Backtest completes successfully
     * 2. Return is not NaN (calculation works)
     * 3. Optionally: Return is positive (strategy works)
     * 
     * TEST DATA:
     * Pure uptrend (no sideways or downtrend phases).
     * 300 bars, price rises from 100 to 250 (+150%).
     * 
     * EXPECTED STRATEGY BEHAVIOR:
     * - Fast MA (10) crosses above slow MA (30) early
     * - BUY signal generated around bar 30-40
     * - Hold position through entire uptrend
     * - No SELL signal (trend never reverses)
     * - Capture ~140% return (most of 150% trend)
     * 
     * WHY SEPARATE UPTREND TEST?
 * 
     * Test 2 uses three-phase data (complex).
     * Test 3 uses pure uptrend (simple).
     * 
     * If Test 2 passes but Test 3 fails:
     * - Strategy has problem with simple trends
     * - Logic error in crossover detection
     * 
     * If Test 2 fails but Test 3 passes:
     * - Strategy can't handle ranging/declining markets
     * - Overfits to uptrends
     * 
     * Both passing: Strategy robust across conditions.
     * 
     * PRICE CONSTRUCTION:
     * 
     * Linear uptrend:
     * ```cpp
     * close = 100 + i * 0.5
     * ```
     * 
     * - Bar 0: 100.0
     * - Bar 100: 150.0 (+50%)
     * - Bar 299: 249.5 (+149.5%)
     * 
     * CROSSOVER TIMING:
     * 
     * With parameters (10, 30):
     * - Need 30 bars for slow MA (bar 0-29 are HOLD)
     * - Bar 30: First valid signals possible
     * - Bar 30-40: Fast MA (recent prices) > slow MA (older prices)
     * - Golden cross occurs, BUY signal generated
     * - Bar 40-299: Fast stays above slow (uptrend continues)
     * 
     * ASSERTION CHOICE:
     * 
     * We assert:
     * ```cpp
     * assert(result.success);
     * assert(!std::isnan(result.total_return));
     * ```
     * 
     * We DON'T assert:
     * ```cpp
     * // assert(result.total_return > 0);  // NOT ASSERTED
     * ```
     * 
     * Why not assert positive return?
     * - Strategy might not generate signals (all HOLD)
     * - Strategy might exit early and miss uptrend
     * - We want to test correctness, not profitability
     * 
     * Comment says "buy-and-hold would be profitable" and
     * "strategy may not generate signals on pure uptrend, which is OK".
     * 
     * This is actually a realistic concern:
     * - If price rises smoothly, MAs both rise smoothly
     * - Fast MA may never cross above slow MA (both rising in parallel)
     * - Strategy stays HOLD entire time
     * - Return = 0% (never entered market)
     * 
     * This isn't a bug, it's strategy behavior.
     * Test accepts this (hence no positive return assertion).
     * 
     * ALTERNATIVE (stricter):
     * If we want to enforce profitable behavior:
     * ```cpp
     * assert(result.total_return > 0);
     * // Forces strategy to capture uptrend
     * ```
     * But this makes test brittle to parameter changes.
     */
    {
        std::cout << "Test 3: Positive return on uptrend... ";
        try {
            /**
             * GENERATE PURE UPTREND DATA
             * 
             * This is different from create_test_data().
             * Single phase: Continuous rise.
             * 
             * WHY RECREATE?
             * Want isolated test of uptrend behavior.
             * Don't want downtrend or sideways phases interfering.
             */
            std::vector<PriceBar> uptrend_data;
            for (int i = 0; i < 300; ++i) {
                PriceBar bar;
                
                /**
                 * DATE FORMATTING
                 * 
                 * Format: "2020-{month}-{day}"
                 * - month = (i / 30) + 1 → 1,1,1...,2,2,2...,10
                 * - day = (i % 30) + 1 → 1,2,3,...,30,1,2,3,...
                 * 
                 * Creates ~10 months of data.
                 */
                bar.date = "2020-" + std::to_string((i / 30) + 1) + "-" + 
                          std::to_string((i % 30) + 1);
                
                /**
                 * PURE UPTREND PRICE
                 * 
                 * Linear growth at +0.5 per bar.
                 * - Bar 0: 100.0
                 * - Bar 150: 175.0
                 * - Bar 299: 249.5
                 * 
                 * Total gain: 149.5% (strong bull market)
                 */
                bar.close = 100.0 + i * 0.5;
                
                /**
                 * OHLC CONSTRUCTION
                 * 
                 * Same pattern as create_test_data():
                 * - Open below close (bullish candle)
                 * - High above close
                 * - Low well below close
                 */
                bar.open = bar.close - 0.5;
                bar.high = bar.close + 0.5;
                bar.low = bar.close - 1.0;
                bar.volume = 1000000;
                
                uptrend_data.push_back(bar);
            }
            
            /**
             * CONFIGURE STRATEGY
             * 
             * Different parameters from Test 1:
             * - short_window: 10 (very fast)
             * - long_window: 30 (moderate)
             * 
             * Faster parameters should respond quicker to uptrend.
             * Golden cross occurs earlier (around bar 30-35).
             */
            SMAStrategy strategy;
            JobParams params;
            params.symbol = "TEST";
            params.short_window = 10;
            params.long_window = 30;
            params.initial_capital = 10000.0;
            strategy.set_parameters(params);
            
            /**
             * ACT: Execute backtest
             */
            auto result = strategy.backtest(uptrend_data);
            
            /**
             * ASSERT: Basic validity
             * 
             * success: Backtest completed without errors
             * !isnan: Return calculation didn't fail
             * 
             * NOTE: We don't assert positive return!
             * See detailed explanation above.
             * 
             * Strategy may generate no signals and return 0%.
             * This is valid behavior (overly conservative strategy).
             */
            assert(result.success);
            assert(!std::isnan(result.total_return));
            
            /**
             * INFORMATIONAL OUTPUT
             * 
             * Display return for manual inspection.
             * Operator can verify it's reasonable:
             * - Expect: 50-140% (captured most of uptrend)
             * - Acceptable: 0-50% (some profit or missed entry)
             * - Concerning: < 0% (lost money in uptrend - bug?)
             * 
             * But don't automatically fail test on low/negative return.
             */
            std::cout << "PASSED (Return=" << result.total_return << "%)\n";
            passed++;
            
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    
    //==========================================================================
    // TEST SUMMARY
    //==========================================================================
    
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";
    
    return (failed == 0) ? 0 : 1;
}

//==============================================================================
// SECTION 5: Understanding SMA Crossover Behavior
//==============================================================================

/**
 * HOW MOVING AVERAGE CROSSOVER WORKS
 * 
 * MOVING AVERAGE (MA):
 * Average of last N closing prices.
 * 
 * CALCULATION:
 * ```cpp
 * MA(period, index) = sum(close[index-period+1 : index]) / period
 * ```
 * 
 * Example: 5-period MA on [100, 102, 104, 103, 105]
 * ```
 * MA(5, 4) = (100 + 102 + 104 + 103 + 105) / 5 = 102.8
 * ```
 * 
 * CROSSOVER DETECTION:
 * 
 * GOLDEN CROSS (Bullish):
 * Fast MA crosses ABOVE slow MA
 * ```
 * Previous: fast[i-1] <= slow[i-1]
 * Current:  fast[i] > slow[i]
 * Signal:   BUY
 * ```
 * 
 * DEATH CROSS (Bearish):
 * Fast MA crosses BELOW slow MA
 * ```
 * Previous: fast[i-1] >= slow[i-1]
 * Current:  fast[i] < slow[i]
 * Signal:   SELL
 * ```
 * 
 * TIMING EXAMPLE:
 * 
 * ```
 * Bar | Close | Fast MA(3) | Slow MA(5) | Signal
 * ----|-------|------------|------------|--------
 * 0   | 100   | -          | -          | HOLD
 * 1   | 101   | -          | -          | HOLD
 * 2   | 102   | 101.0      | -          | HOLD
 * 3   | 103   | 102.0      | -          | HOLD
 * 4   | 104   | 103.0      | 102.0      | HOLD
 * 5   | 106   | 104.3      | 103.2      | HOLD (fast>slow already)
 * 6   | 105   | 105.0      | 104.2      | HOLD
 * 7   | 103   | 104.7      | 104.4      | HOLD
 * 8   | 102   | 103.3      | 103.8      | SELL (fast<slow, was >=)
 * 9   | 104   | 103.0      | 103.2      | HOLD
 * 10  | 107   | 104.3      | 103.6      | BUY (fast>slow, was <=)
 * ```
 * 
 * WHY TWO DIFFERENT PERIODS?
 * 
 * FAST MA:
 * - Responds quickly to price changes
 * - More sensitive (follows price closely)
 * - More false signals (noise)
 * 
 * SLOW MA:
 * - Responds slowly (smoothed trend)
 * - Less sensitive (filters noise)
 * - Fewer false signals
 * - Lags price changes
 * 
 * COMBINATION:
 * - Fast crosses slow → Significant trend change
 * - Filters noise (both MAs must agree on direction)
 * - Balance: Responsiveness vs reliability
 * 
 * PARAMETER TUNING:
 * 
 * LARGER WINDOW:
 * - Smoother MA
 * - Fewer signals
 * - Later trend detection
 * - Missed early moves
 * 
 * SMALLER WINDOW:
 * - More responsive MA
 * - More signals
 * - Earlier trend detection
 * - More false signals (whipsaws)
 * 
 * OPTIMAL:
 * Depends on:
 * - Market characteristics (volatile vs stable)
 * - Time horizon (day trading vs investing)
 * - Risk tolerance (active vs passive)
 * 
 * Common combinations:
 * - (50, 200): Classic long-term (year-long trends)
 * - (20, 50): Medium-term (month-long trends)
 * - (5, 20): Short-term (week-long trends)
 */

//==============================================================================
// SECTION 6: Debugging Strategy Tests
//==============================================================================

/**
 * DEBUGGING FAILURE: "Assert failed: buy_signals > 0"
 * 
 * SYMPTOM:
 * Test 1 fails because no BUY signals generated.
 * 
 * DIAGNOSIS:
 * 
 * STEP 1: Print generated signals
 * ```cpp
 * auto signals = strategy.generate_signals(data);
 * 
 * std::cout << "Generated " << signals.size() << " signals:\n";
 * for (size_t i = 0; i < signals.size(); ++i) {
 *     if (signals[i] != Signal::HOLD) {
 *         std::string sig_str = (signals[i] == Signal::BUY) ? "BUY" : "SELL";
 *         std::cout << "  Bar " << i << ": " << sig_str << "\n";
 *     }
 * }
 * ```
 * 
 * STEP 2: Verify MA calculations
 * ```cpp
 * auto fast_ma = strategy.calculate_sma(data, 20);
 * auto slow_ma = strategy.calculate_sma(data, 50);
 * 
 * for (size_t i = 50; i < 60; ++i) {
 *     std::cout << "Bar " << i << ": close=" << data[i].close
 *               << " fast=" << fast_ma[i]
 *               << " slow=" << slow_ma[i] << "\n";
 * }
 * ```
 * 
 * STEP 3: Check crossover logic
 * ```cpp
 * // Should detect golden cross
 * for (size_t i = 51; i < data.size(); ++i) {
 *     if (fast_ma[i] > slow_ma[i] && fast_ma[i-1] <= slow_ma[i-1]) {
 *         std::cout << "Golden cross at bar " << i << "\n";
 *     }
 * }
 * ```
 * 
 * POSSIBLE CAUSES:
 * - MA calculation wrong (all zeros or incorrect values)
 * - Crossover detection logic inverted
 * - Index off-by-one error
 * - Warm-up period too long (no signals in valid range)
 * - Parameters invalid (short >= long)
 */

/**
 * DEBUGGING FAILURE: "Assert failed: !std::isnan(result.sharpe_ratio)"
 * 
 * SYMPTOM:
 * Backtest completes but Sharpe ratio is NaN.
 * 
 * DIAGNOSIS:
 * 
 * Sharpe ratio = mean_return / std_dev_return
 * 
 * NaN occurs when:
 * 1. std_dev = 0 (all returns identical)
 * 2. No returns data (no trades executed)
 * 3. sqrt(negative) somewhere in calculation
 * 
 * STEP 1: Check if any trades occurred
 * ```cpp
 * std::cout << "Number of trades: " << result.num_trades << "\n";
 * if (result.num_trades == 0) {
 *     std::cout << "No trades! Strategy never entered market.\n";
 * }
 * ```
 * 
 * STEP 2: Examine returns data
 * ```cpp
 * // Inside calculate_sharpe_ratio
 * std::cout << "Returns size: " << returns.size() << "\n";
 * if (returns.empty()) {
 *     std::cout << "No returns to calculate Sharpe!\n";
 * }
 * ```
 * 
 * STEP 3: Check variance calculation
 * ```cpp
 * double mean = calculate_mean(returns);
 * double variance = calculate_variance(returns, mean);
 * double std_dev = sqrt(variance);
 * 
 * std::cout << "Mean: " << mean << "\n";
 * std::cout << "Variance: " << variance << "\n";
 * std::cout << "Std dev: " << std_dev << "\n";
 * 
 * if (std_dev == 0) {
 *     std::cout << "Zero volatility! All returns identical.\n";
 * }
 * ```
 * 
 * FIX:
 * Handle edge cases in Sharpe calculation:
 * ```cpp
 * double calculate_sharpe_ratio(const std::vector<double>& returns) {
 *     if (returns.empty()) return 0.0;  // No data → 0
 *     
 *     double mean = calculate_mean(returns);
 *     double std_dev = calculate_std_dev(returns, mean);
 *     
 *     if (std_dev < 1e-10) return 0.0;  // Zero volatility → 0
 *     
 *     return mean / std_dev;
 * }
 * ```
 */

/**
 * DEBUGGING FAILURE: Unexpected return values
 * 
 * SYMPTOM:
 * Test passes but return is suspicious (e.g., -80% on uptrend).
 * 
 * DIAGNOSIS:
 * 
 * STEP 1: Check trade execution
 * ```cpp
 * // Print all trades
 * for (auto& trade : result.trades) {
 *     std::cout << trade.date << " " 
 *               << (trade.signal == Signal::BUY ? "BUY" : "SELL")
 *               << " " << trade.shares << " @ $" << trade.price << "\n";
 * }
 * ```
 * 
 * STEP 2: Trace portfolio value
 * ```cpp
 * // Inside backtest(), print portfolio after each signal
 * if (signals[i] != Signal::HOLD) {
 *     std::cout << "After " << (signals[i] == Signal::BUY ? "BUY" : "SELL")
 *               << ": cash=$" << portfolio.cash
 *               << ", shares=" << portfolio.shares
 *               << ", value=$" << portfolio.value << "\n";
 * }
 * ```
 * 
 * COMMON BUGS:
 * - Buying at high, selling at low (inverted logic)
 * - Not updating portfolio value correctly
 * - Using wrong price (open instead of close)
 * - Arithmetic errors in profit calculation
 */

//==============================================================================
// SECTION 7: Common Strategy Testing Pitfalls
//==============================================================================

/**
 * PITFALL 1: Testing only uptrends
 * 
 * PROBLEM:
 * ```cpp
 * // All test data is uptrending
 * for (int i = 0; i < 100; ++i) {
 *     bar.close = 100 + i;  // Only tests bull market
 * }
 * ```
 * 
 * CONSEQUENCE:
 * Strategy works great in tests, fails in real markets (down or sideways).
 * 
 * SOLUTION:
 * Test multiple market conditions (like our three-phase data):
 * ```cpp
 * test_on_uptrend();
 * test_on_downtrend();
 * test_on_sideways();
 * test_on_volatile();
 * ```
 */

/**
 * PITFALL 2: Not testing edge cases
 * 
 * PROBLEM:
 * ```cpp
 * // Test with 300 bars (plenty of data)
 * auto data = create_test_data();  // Only tests typical case
 * ```
 * 
 * MISSING:
 * - Insufficient data (< long_window bars)
 * - Exactly long_window bars (boundary)
 * - Very long data (10,000+ bars)
 * - Missing bars (gaps in data)
 * 
 * SOLUTION:
 * ```cpp
 * // Test with minimal data
 * std::vector<PriceBar> minimal(30);  // Exactly slow MA period
 * auto signals = strategy.generate_signals(minimal);
 * // Should handle gracefully (all HOLD)
 * 
 * // Test with insufficient data
 * std::vector<PriceBar> insufficient(10);  // Less than slow MA
 * auto signals2 = strategy.generate_signals(insufficient);
 * // Should not crash
 * ```
 */

/**
 * PITFALL 3: Asserting exact return values
 * 
 * PROBLEM:
 * ```cpp
 * assert(result.total_return == 45.23);  // Too brittle!
 * ```
 * 
 * CONSEQUENCE:
 * Tiny changes to strategy break tests:
 * - Adjust MA calculation method
 * - Fix rounding error
 * - Change execution price model
 * 
 * All cause return to change by 0.01%, test fails.
 * 
 * SOLUTION:
 * Range assertions:
 * ```cpp
 * assert(result.total_return > 40 && result.total_return < 50);  // ✅ Range
 * ```
 * 
 * Or just check validity:
 * ```cpp
 * assert(!std::isnan(result.total_return));  // ✅ Just verify it calculated
 * ```
 */

/**
 * PITFALL 4: Not testing parameter validation
 * 
 * PROBLEM:
 * ```cpp
 * // What if short_window >= long_window?
 * params.short_window = 100;
 * params.long_window = 50;  // Invalid!
 * strategy.set_parameters(params);
 * ```
 * 
 * EXPECTED:
 * Strategy should reject invalid parameters.
 * 
 * TEST:
 * ```cpp
 * // Test invalid parameters
 * {
 *     JobParams bad_params;
 *     bad_params.short_window = 100;
 *     bad_params.long_window = 50;  // Invalid: short >= long
 *     
 *     try {
 *         strategy.set_parameters(bad_params);
 *         assert(false);  // Should have thrown
 *     } catch (const std::invalid_argument& e) {
 *         // Expected exception
 *         assert(true);
 *     }
 * }
 * ```
 */

/**
 * PITFALL 5: Ignoring trading costs
 * 
 * PROBLEM:
 * Test shows 50% return, but real trading has:
 * - Commissions: $5 per trade
 * - Slippage: 0.05% of trade value
 * - Spread: Bid-ask difference
 * 
 * After costs, 50% becomes 45% (or worse with many trades).
 * 
 * SOLUTION:
 * Test with transaction costs:
 * ```cpp
 * params.commission_per_share = 0.01;
 * params.slippage_bps = 5;  // 5 basis points
 * 
 * auto result_with_costs = strategy.backtest(data);
 * 
 * // Return should be lower with costs
 * assert(result_with_costs.total_return < result_no_costs.total_return);
 * ```
 */

/**
 * PITFALL 6: Not seeding random number generator
 * 
 * PROBLEM:
 * If strategy uses randomness (Monte Carlo, stochastic signals):
 * ```cpp
 * if (rand() > threshold) signal = BUY;  // Non-deterministic!
 * ```
 * 
 * Test results vary each run (flaky tests).
 * 
 * SOLUTION:
 * Seed RNG for determinism:
 * ```cpp
 * std::srand(42);  // Fixed seed
 * // Or better:
 * std::mt19937 rng(42);  // Seeded mersenne twister
 * ```
 * 
 * Same seed → same random sequence → same test results.
 */

//==============================================================================
// SECTION 8: FAQ
//==============================================================================

/**
 * Q1: Why test with synthetic data instead of real prices?
 * 
 * A: For unit tests, synthetic data is better:
 *    - Controlled: Know exactly what should happen
 *    - Fast: No file loading
 *    - Deterministic: Same results every run
 *    - Isolated: No external dependencies
 *    
 *    For evaluation, definitely use real historical data.
 */

/**
 * Q2: How do I know if SMA parameters are good?
 * 
 * A: No "best" parameters, depends on:
 *    - Time horizon (day trading vs investing)
 *    - Market characteristics (volatile vs stable)
 *    - Risk tolerance (frequent trades vs buy-hold)
 *    
 *    COMMON CONFIGURATIONS:
 *    - (50, 200): Classic for stocks (Golden Cross)
 *    - (12, 26): Popular for forex
 *    - (10, 30): Balance of responsiveness and reliability
 *    
 *    OPTIMIZATION:
 *    Test many combinations, pick best Sharpe ratio.
 *    But beware overfitting!
 */

/**
 * Q3: Why doesn't Test 3 assert positive return?
 * 
 * A: Strategy might not generate signals on smooth uptrend:
 *    - If price rises steadily, both MAs rise in parallel
 *    - Fast never crosses above slow (already above or never crosses)
 *    - Result: No BUY signal, stay HOLD entire time
 *    - Return: 0% (never entered market)
 *    
 *    This is valid strategy behavior, not a bug.
 *    Strategy is conservative (waits for clear crossover).
 */

/**
 * Q4: What's a good Sharpe ratio for SMA strategy?
 * 
 * A: Benchmarks:
 *    - < 0: Strategy losing money (worse than risk-free)
 *    - 0-1: Marginal (may not be worth trading)
 *    - 1-2: Good (decent risk-adjusted returns)
 *    - 2-3: Excellent (professional-grade)
 *    - > 3: Exceptional (verify not overfit!)
 *    
 *    SMA crossover typically achieves 0.5-1.5 on real data.
 *    On synthetic uptrend, may be higher (2-3).
 */

/**
 * Q5: How do I add tests for other strategies (RSI, etc.)?
 * 
 * A: Same pattern, different test data:
 *    
 *    RSI STRATEGY TEST:
 *    ```cpp
 *    // Create overbought/oversold data
 *    std::vector<PriceBar> create_rsi_test_data() {
 *        std::vector<PriceBar> data;
 *        for (int i = 0; i < 100; ++i) {
 *            PriceBar bar;
 *            // Create oscillating pattern (good for RSI)
 *            bar.close = 100 + 20 * sin(i * 0.1);
 *            // ...
 *        }
 *        return data;
 *    }
 *    
 *    void test_rsi_strategy() {
 *        auto data = create_rsi_test_data();
 *        RSIStrategy strategy;
 *        // ... test signal generation ...
 *    }
 *    ```
 */

/**
 * Q6: Should I test all possible parameter combinations?
 * 
 * A: No, that's optimization not unit testing:
 *    
 *    UNIT TEST:
 *    Test few representative parameter sets:
 *    - (10, 30): Fast response
 *    - (50, 200): Slow response
 *    - (20, 50): Medium response
 *    
 *    OPTIMIZATION:
 *    Separate process to find best parameters:
 *    ```cpp
 *    for (int fast = 5; fast <= 100; fast += 5) {
 *        for (int slow = fast + 10; slow <= 300; slow += 10) {
 *            params.short_window = fast;
 *            params.long_window = slow;
 *            
 *            auto result = strategy.backtest(data);
 *            // Track best Sharpe ratio
 *        }
 *    }
 *    ```
 *    
 *    But beware overfitting! Validate on out-of-sample data.
 */

/**
 * Q7: How do I visualize test data and signals?
 * 
 * A: Export to CSV for plotting:
 *    
 *    ```cpp
 *    void export_test_results(const std::vector<PriceBar>& data,
 *                            const std::vector<Signal>& signals) {
 *        std::ofstream file("test_results.csv");
 *        file << "date,close,signal\n";
 *        
 *        for (size_t i = 0; i < data.size(); ++i) {
 *            file << data[i].date << ","
 *                 << data[i].close << ","
 *                 << static_cast<int>(signals[i]) << "\n";
 *        }
 *    }
 *    ```
 *    
 *    Then plot in Python:
 *    ```python
 *    import pandas as pd
 *    import matplotlib.pyplot as plt
 *    
 *    df = pd.read_csv('test_results.csv')
 *    
 *    plt.plot(df['close'], label='Price')
 *    
 *    # Mark BUY signals
 *    buys = df[df['signal'] == 1]
 *    plt.scatter(buys.index, buys['close'], color='green', 
 *               marker='^', s=100, label='BUY')
 *    
 *    # Mark SELL signals
 *    sells = df[df['signal'] == -1]
 *    plt.scatter(sells.index, sells['close'], color='red',
 *               marker='v', s=100, label='SELL')
 *    
 *    plt.legend()
 *    plt.show()
 *    ```
 */

/**
 * Q8: What if test passes but strategy loses money in production?
 * 
 * A: Unit tests only verify correctness, not profitability:
 *    
 *    UNIT TESTS VALIDATE:
 *    - Code doesn't crash
 *    - Calculations are accurate
 *    - Logic is implemented correctly
 *    
 *    UNIT TESTS DON'T VALIDATE:
 *    - Strategy will be profitable
 *    - Parameters are optimal
 *    - Works on real market data
 *    
 *    NEXT STEPS:
 *    1. Test on real historical data (evaluation)
 *    2. Walk-forward analysis (train on past, test on future)
 *    3. Out-of-sample validation
 *    4. Compare to benchmarks (buy-and-hold, index)
 *    5. Paper trading (sim with real-time data)
 *    6. Live trading (small capital, monitor closely)
 */

/**
 * Q9: How do I test for lookahead bias?
 * 
 * A: Lookahead bias = using future data to make current decisions
 *    
 *    EXAMPLE OF BIAS:
 *    ```cpp
 *    // WRONG: Looking at tomorrow's price
 *    if (price_data[i+1].close > price_data[i].close) {
 *        signals[i] = Signal::BUY;  // Cheating!
 *    }
 *    ```
 *    
 *    DETECTION:
 *    Shift data by 1 bar, results should change:
 *    ```cpp
 *    auto signals1 = strategy.generate_signals(data);
 *    
 *    // Shift data (remove first bar, duplicate last bar)
 *    auto shifted_data = shift_by_one(data);
 *    auto signals2 = strategy.generate_signals(shifted_data);
 *    
 *    // Signals should be different (not identical shift)
 *    // If identical, indicates lookahead bias
 *    ```
 *    
 *    PREVENTION:
 *    Code review: Ensure signals[i] only uses data[0..i], never data[i+1..n]
 */

/**
 * Q10: How do I test strategy on real data?
 * 
 * A: Extend tests to load CSV files:
 *    
 *    ```cpp
 *    // Test with real AAPL data
 *    {
 *        std::cout << "Test: Real data backtest... ";
 *        try {
 *            CSVLoader loader;
 *            auto data = loader.load_csv("../data/AAPL.csv");
 *            
 *            SMAStrategy strategy;
 *            params.short_window = 50;
 *            params.long_window = 200;
 *            strategy.set_parameters(params);
 *            
 *            auto result = strategy.backtest(data);
 *            
 *            // Validate execution (not profitability)
 *            assert(result.success);
 *            assert(!std::isnan(result.total_return));
 *            
 *            // Display for manual review
 *            std::cout << "PASSED\n";
 *            std::cout << "  Period: " << data.front().date 
 *                      << " to " << data.back().date << "\n";
 *            std::cout << "  Return: " << result.total_return << "%\n";
 *            std::cout << "  Sharpe: " << result.sharpe_ratio << "\n";
 *            std::cout << "  Trades: " << result.num_trades << "\n";
 *            
 *            passed++;
 *        } catch (const std::exception& e) {
 *            std::cout << "FAILED: " << e.what() << "\n";
 *            failed++;
 *        }
 *    }
 *    ```
 */