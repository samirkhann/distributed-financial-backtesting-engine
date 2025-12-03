/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: csv_loader.h
    Module: Worker Node - Data Loader Component
    
    Description:
        This header file defines the CSV data loading infrastructure for the
        distributed backtesting system. It provides efficient loading, parsing,
        and caching of historical price data from CSV files. The CSVLoader is
        designed to run on worker nodes and load financial time series data
        (OHLCV format) for backtest strategy execution.
        
        Key Features:
        - Memory-efficient CSV parsing with validation
        - In-memory caching to avoid repeated disk I/O
        - Fast date-based lookups using indexed maps
        - Range queries for time-windowed analysis
        - Statistical utilities for price data
        
        Architecture Context:
        This component is part of Module 2 (Worker Nodes) in the system
        architecture. Workers use this loader to access historical S&P 500
        price data (2020-2024, ~500 symbols) stored as CSV files on shared
        NFS or local storage. The caching mechanism is critical for performance
        since backtests may process the same symbol multiple times across
        different strategies or parameter sets.
        
    Performance Considerations:
        - Data is cached after first load to minimize I/O overhead
        - Date indexing enables O(1) lookups instead of O(n) scans
        - Memory usage scales with number of cached symbols
        - Typical dataset: ~500 symbols × 1260 trading days × 48 bytes ≈ 30MB
        
    Thread Safety:
        - This class is NOT thread-safe
        - Each worker thread should maintain its own CSVLoader instance
        - Alternatively, wrap cache access with mutex if sharing across threads
*******************************************************************************/

#ifndef CSV_LOADER_H
#define CSV_LOADER_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <fstream>

namespace backtesting {

/*******************************************************************************
 * TABLE OF CONTENTS
 *******************************************************************************
 * 
 * 1. OVERVIEW & ARCHITECTURE
 *    - System Context
 *    - Data Flow
 *    - Design Philosophy
 * 
 * 2. DATA STRUCTURES
 *    2.1 PriceBar - Individual OHLCV data point
 *    2.2 TimeSeriesData - Symbol's complete time series
 *    2.3 CSVLoader - File loading and caching
 * 
 * 3. USAGE EXAMPLES
 *    3.1 Basic Loading
 *    3.2 Range Queries
 *    3.3 Caching Strategies
 *    3.4 Error Handling
 * 
 * 4. COMMON PITFALLS & BEST PRACTICES
 *    4.1 Memory Management
 *    4.2 Date Format Requirements
 *    4.3 Data Validation
 *    4.4 Performance Optimization
 * 
 * 5. FAQ
 * 
 * 6. INTEGRATION WITH DISTRIBUTED SYSTEM
 ******************************************************************************/

/*******************************************************************************
 * SECTION 1: OVERVIEW & ARCHITECTURE
 *******************************************************************************
 * 
 * SYSTEM CONTEXT:
 * ---------------
 * The CSVLoader sits within the Worker Node's data management layer. When
 * the controller assigns a backtest job to a worker, the workflow is:
 * 
 *   1. Worker receives JOB_ASSIGN message with symbol list
 *   2. CSVLoader loads historical data for each symbol
 *   3. Strategy engine processes the time series
 *   4. Results sent back to controller
 * 
 * DATA FLOW:
 * ----------
 *   CSV Files (NFS/Local Storage)
 *          ↓
 *     CSVLoader.load(symbol)
 *          ↓
 *   TimeSeriesData (in-memory cache)
 *          ↓
 *   Strategy Engine (SMA, RSI, etc.)
 *          ↓
 *   Performance Metrics (PnL, Sharpe, Drawdown)
 * 
 * DESIGN PHILOSOPHY:
 * ------------------
 * - Load Once, Use Many: Caching prevents repeated file I/O
 * - Validate Early: Price bars are validated on construction
 * - Fast Lookups: Date indexing enables efficient range queries
 * - Memory Conscious: shared_ptr enables safe sharing without duplication
 * - Fail Fast: Invalid data is detected during parsing, not during backtest
 ******************************************************************************/

/*******************************************************************************
 * SECTION 2.1: PriceBar Structure
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * Represents a single trading day's OHLCV (Open, High, Low, Close, Volume)
 * data. This is the atomic unit of price information used in backtesting.
 * 
 * FIELD DESCRIPTIONS:
 * -------------------
 * - date: Trading date in ISO format (YYYY-MM-DD), e.g., "2023-01-15"
 *         Must be lexicographically sortable for chronological ordering
 * 
 * - open: Opening price of the trading session
 *         First trade price of the day
 * 
 * - high: Highest price reached during the session
 *         Critical for stop-loss and breakout strategies
 * 
 * - low: Lowest price reached during the session
 *        Important for support level analysis
 * 
 * - close: Closing price of the trading session
 *          Most commonly used price for strategy signals
 * 
 * - volume: Number of shares traded
 *           Used for liquidity analysis and volume-based strategies
 * 
 * VALIDATION RULES:
 * -----------------
 * The is_valid() method enforces OHLCV data integrity:
 * 1. high >= low (high must be highest price)
 * 2. high >= open (high must contain opening price)
 * 3. high >= close (high must contain closing price)
 * 4. low <= open (low must be lowest price)
 * 5. low <= close (low must contain closing price)
 * 6. volume >= 0 (volume cannot be negative)
 * 
 * WHY VALIDATION MATTERS:
 * -----------------------
 * Invalid data can cause:
 * - Incorrect strategy signals (false breakouts)
 * - Misleading backtest results (phantom profits)
 * - Runtime errors in strategy calculations (divide by zero)
 * - Checkpoint corruption (if invalid data is serialized)
 * 
 * MEMORY LAYOUT:
 * --------------
 * sizeof(PriceBar) ≈ 48 bytes on 64-bit systems:
 * - std::string date: 32 bytes (small string optimization)
 * - 4 doubles: 32 bytes
 * - int64_t: 8 bytes
 * - Padding/alignment: ~8 bytes
 ******************************************************************************/

struct PriceBar {
    std::string date;      // YYYY-MM-DD format (ISO 8601)
    double open;           // Opening price
    double high;           // Highest price of the day
    double low;            // Lowest price of the day
    double close;          // Closing price (most important for strategies)
    int64_t volume;        // Trading volume (number of shares)
    
    // Default constructor: Initializes all numeric fields to zero
    // Required for vector resizing and default initialization
    PriceBar() : open(0), high(0), low(0), close(0), volume(0) {}
    
    // VALIDATION METHOD
    // Checks OHLCV relationship constraints
    // Returns: true if data is logically consistent, false otherwise
    // 
    // Usage in CSVLoader:
    //   if (!bar.is_valid()) {
    //       // Log error and skip this bar, or throw exception
    //   }
    bool is_valid() const {
        return high >= low && high >= open && high >= close &&
               low <= open && low <= close && volume >= 0;
    }
};
 
class TimeSeriesData {

private:

    std::string symbol_;                          // Stock ticker (e.g., "AAPL")

    std::vector<PriceBar> bars_;                  // Chronologically ordered price data

    std::map<std::string, size_t> date_index_;    // Maps date -> vector index

public:

    // Default constructor for map/vector storage

    TimeSeriesData() = default;

    // Constructor: Initialize with symbol name

    explicit TimeSeriesData(const std::string& symbol) : symbol_(symbol) {}

    void add_bar(const PriceBar& bar);

    void sort_by_date();

    const std::string& symbol() const { return symbol_; }

    size_t size() const { return bars_.size(); }

    bool empty() const { return bars_.empty(); }

    const PriceBar& operator[](size_t idx) const { return bars_[idx]; }

    const PriceBar& at(size_t idx) const { return bars_.at(idx); }

    std::vector<PriceBar> get_range(const std::string& start_date,

                                    const std::string& end_date) const;

    size_t find_date_index(const std::string& date) const;

    const std::vector<PriceBar>& get_all_bars() const { return bars_; }

    double get_mean_close() const;

    double get_volatility() const;

};

/*******************************************************************************
 * SECTION 2.3: CSVLoader Class
 *******************************************************************************
 * 
 * PURPOSE:
 * --------
 * Central file I/O and caching manager for historical price data. Handles:
 * - Parsing CSV files into PriceBar/TimeSeriesData structures
 * - Caching loaded data to avoid redundant disk reads
 * - Providing consistent interface for data access across workers
 * 
 * ARCHITECTURE ROLE:
 * ------------------
 * In the distributed system, each worker node instantiates a CSVLoader:
 * 
 *   Worker Process
 *   ├── Job Executor (receives work from controller)
 *   ├── CSVLoader (loads data for assigned symbols)
 *   ├── Strategy Engine (processes time series)
 *   └── Heartbeat Manager (reports status to controller)
 * 
 * CACHING STRATEGY:
 * -----------------
 * The cache_ map stores shared_ptr<TimeSeriesData> for each loaded symbol.
 * 
 * Benefits of shared_ptr:
 * 1. Multiple strategy instances can share same data (no duplication)
 * 2. Automatic memory cleanup when all references dropped
 * 3. Safe to return from methods (caller shares ownership)
 * 
 * Cache Lifecycle:
 * 1. Worker starts → empty cache
 * 2. First job assigned → load required symbols into cache
 * 3. Subsequent jobs → reuse cached data (instant access)
 * 4. Worker shutdown → cache destructor frees all data
 * 
 * WHY NOT USE SINGLETON?
 * ----------------------
 * - Each worker may have different data directories (NFS vs local)
 * - Testing requires isolated loader instances
 * - Multiple workers on same physical machine don't share memory anyway
 * 
 * CSV FORMAT REQUIREMENTS:
 * ------------------------
 * Expected format:
 *   Date,Open,High,Low,Close,Volume
 *   2023-01-03,147.96,148.50,146.85,147.50,52000000
 *   2023-01-04,148.00,149.25,147.80,148.95,48500000
 * 
 * - Header row is required
 * - Date format: YYYY-MM-DD (ISO 8601)
 * - Prices: Decimal numbers (dot as separator)
 * - Volume: Integer
 * - Delimiter: Comma (not tab or semicolon)
 * - No quoted fields unless they contain commas
 ******************************************************************************/

class CSVLoader {
private:
    // Base directory containing CSV files
    // Example: "/shared/nfs/price_data/"
    // Files expected: {data_directory_}/AAPL.csv, {data_directory_}/MSFT.csv, etc.
    std::string data_directory_;
    
    // Cache of loaded time series data
    // Key: Symbol (e.g., "AAPL")
    // Value: shared_ptr to loaded data
    // 
    // Why shared_ptr instead of unique_ptr?
    //   - Multiple code paths may hold references to same data
    //   - Allows returning references without forcing caller to copy
    //   - Automatic cleanup when last reference goes out of scope
    std::map<std::string, std::shared_ptr<TimeSeriesData>> cache_;
    
    // UTILITY: Split string by delimiter
    // Params:
    //   str: Input string (e.g., CSV line)
    //   delimiter: Character to split on (e.g., ',')
    // Returns:
    //   Vector of substrings
    // 
    // Performance: O(n) where n is string length
    // 
    // Example:
    //   split("2023-01-03,147.96,148.50", ',') 
    //   → ["2023-01-03", "147.96", "148.50"]
    static std::vector<std::string> split(const std::string& str, char delimiter);
    
    // UTILITY: Remove leading/trailing whitespace
    // Params:
    //   str: Input string
    // Returns:
    //   String with whitespace removed
    // 
    // Why Needed:
    //   CSV files may have inconsistent spacing:
    //   "2023-01-03, 147.96 , 148.50" should parse correctly
    static std::string trim(const std::string& str);
    
public:
    // Default constructor
    CSVLoader() = default;
    
    // CONSTRUCTOR: Initialize with data directory
    // Params:
    //   data_dir: Path to directory containing CSV files
    // 
    // Example:
    //   CSVLoader loader("/shared/nfs/sp500_data/");
    explicit CSVLoader(const std::string& data_dir) : data_directory_(data_dir) {}
    
    // SET DATA DIRECTORY: Change the base path for CSV files
    // Useful for:
    //   - Switching between test and production data
    //   - Using local copy vs NFS for performance testing
    void set_data_directory(const std::string& dir) { data_directory_ = dir; }
    
    // LOAD: Load symbol data from default location
    // Params:
    //   symbol: Stock ticker (e.g., "AAPL")
    // Returns:
    //   shared_ptr to TimeSeriesData (nullptr if load fails)
    // 
    // Behavior:
    //   1. Check cache - if symbol already loaded, return cached data
    //   2. Otherwise, construct filepath: {data_directory_}/{symbol}.csv
    //   3. Parse CSV file into TimeSeriesData
    //   4. Validate all bars
    //   5. Sort by date
    //   6. Add to cache
    //   7. Return shared_ptr
    // 
    // Error Handling:
    //   - File not found → return nullptr, log error
    //   - Parse error → return nullptr, log which line failed
    //   - Invalid data → skip bad bars, log warnings
    // 
    // Performance:
    //   - First call: O(n) where n = file size (disk I/O + parsing)
    //   - Subsequent calls: O(1) (cache hit)
    // 
    // Thread Safety:
    //   - NOT thread-safe (cache access not protected)
    //   - Each thread should have its own CSVLoader instance
    // 
    // Example Usage:
    //   auto aapl_data = loader.load("AAPL");
    //   if (aapl_data && !aapl_data->empty()) {
    //       // Process data
    //   } else {
    //       // Handle error
    //   }
    std::shared_ptr<TimeSeriesData> load(const std::string& symbol);
    
    // LOAD FROM FILE: Load symbol data from explicit filepath
    // Params:
    //   symbol: Stock ticker (for labeling the data)
    //   filepath: Full path to CSV file
    // Returns:
    //   shared_ptr to TimeSeriesData (nullptr if load fails)
    // 
    // When to Use:
    //   - Testing with specific data files
    //   - Loading data from non-standard locations
    //   - Processing files with different naming conventions
    // 
    // Example:
    //   auto test_data = loader.load_from_file(
    //       "AAPL", 
    //       "/tmp/test_scenarios/aapl_2020_crash.csv"
    //   );
    std::shared_ptr<TimeSeriesData> load_from_file(const std::string& symbol,
                                                    const std::string& filepath);
    
    // CLEAR CACHE: Remove all cached data
    // 
    // When to Use:
    //   - Between different backtest runs to ensure fresh data
    //   - When memory usage is high (cache grows with symbol count)
    //   - During testing to force reload
    // 
    // Warning:
    //   Any shared_ptr references returned by previous load() calls
    //   remain valid (they still own their data). Clear_cache() only
    //   removes the cache's reference. Data is freed when last
    //   shared_ptr is destroyed.
    // 
    // Example Pattern:
    //   // Run backtest
    //   auto data = loader.load("AAPL");
    //   run_strategy(data);
    //   
    //   // Later, free memory
    //   data.reset();              // Release our reference
    //   loader.clear_cache();       // Release cache's reference
    void clear_cache() { cache_.clear(); }
    
    // IS CACHED: Check if symbol is already loaded
    // Params:
    //   symbol: Stock ticker to check
    // Returns:
    //   true if symbol is in cache, false otherwise
    // 
    // Use Cases:
    //   - Deciding whether to preload data
    //   - Monitoring cache hit rates
    //   - Debugging data loading issues
    // 
    // Example:
    //   if (!loader.is_cached("TSLA")) {
    //       std::cout << "Loading TSLA for first time...\n";
    //   }
    bool is_cached(const std::string& symbol) const {
        return cache_.find(symbol) != cache_.end();
    }
};

} // namespace backtesting

/*******************************************************************************
 * SECTION 3: USAGE EXAMPLES
 *******************************************************************************
 * 
 * EXAMPLE 3.1: Basic Loading and Iteration
 * -----------------------------------------
 * 
 * #include "csv_loader.h"
 * #include <iostream>
 * 
 * int main() {
 *     using namespace backtesting;
 *     
 *     // Initialize loader with data directory
 *     CSVLoader loader("/shared/nfs/sp500_data/");
 *     
 *     // Load Apple stock data
 *     auto aapl = loader.load("AAPL");
 *     
 *     if (!aapl || aapl->empty()) {
 *         std::cerr << "Failed to load AAPL data\n";
 *         return 1;
 *     }
 *     
 *     std::cout << "Loaded " << aapl->size() << " bars for AAPL\n";
 *     
 *     // Iterate through all bars
 *     for (size_t i = 0; i < aapl->size(); ++i) {
 *         const PriceBar& bar = (*aapl)[i];
 *         std::cout << bar.date << ": close=" << bar.close << "\n";
 *     }
 *     
 *     return 0;
 * }
 * 
 * 
 * EXAMPLE 3.2: Range Queries for Time-Windowed Analysis
 * ------------------------------------------------------
 * 
 * // Analyze COVID-19 market crash period
 * auto spy = loader.load("SPY");
 * 
 * auto crash_period = spy->get_range("2020-02-19", "2020-03-23");
 * 
 * double max_drawdown = 0.0;
 * double peak = crash_period[0].close;
 * 
 * for (const auto& bar : crash_period) {
 *     peak = std::max(peak, bar.close);
 *     double drawdown = (peak - bar.close) / peak;
 *     max_drawdown = std::max(max_drawdown, drawdown);
 * }
 * 
 * std::cout << "Max drawdown during crash: " 
 *           << (max_drawdown * 100) << "%\n";
 * 
 * 
 * EXAMPLE 3.3: Efficient Multi-Symbol Loading with Caching
 * ---------------------------------------------------------
 * 
 * // Worker receives job to backtest strategy on 100 symbols
 * std::vector<std::string> symbols = {
 *     "AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", ...
 * };
 * 
 * CSVLoader loader("/data/sp500/");
 * 
 * // First pass - load all symbols (disk I/O)
 * for (const auto& symbol : symbols) {
 *     auto data = loader.load(symbol);
 *     if (data) {
 *         std::cout << "Loaded " << symbol << "\n";
 *     }
 * }
 * 
 * // Second pass - instant access from cache
 * for (const auto& symbol : symbols) {
 *     auto data = loader.load(symbol);  // Cache hit! No I/O
 *     run_backtest(data, "SMA_Crossover");
 * }
 * 
 * // Report cache efficiency
 * int cached_count = 0;
 * for (const auto& symbol : symbols) {
 *     if (loader.is_cached(symbol)) cached_count++;
 * }
 * std::cout << "Cache hit rate: " 
 *           << (100.0 * cached_count / symbols.size()) << "%\n";
 * 
 * 
 * EXAMPLE 3.4: Robust Error Handling Pattern
 * -------------------------------------------
 * 
 * std::shared_ptr<TimeSeriesData> load_symbol_safely(
 *     CSVLoader& loader,
 *     const std::string& symbol
 * ) {
 *     try {
 *         auto data = loader.load(symbol);
 *         
 *         if (!data) {
 *             std::cerr << "ERROR: Failed to load " << symbol << "\n";
 *             return nullptr;
 *         }
 *         
 *         if (data->empty()) {
 *             std::cerr << "WARNING: " << symbol << " has no data\n";
 *             return nullptr;
 *         }
 *         
 *         if (data->size() < 252) {  // Less than 1 year of trading days
 *             std::cerr << "WARNING: " << symbol 
 *                       << " has insufficient data (" 
 *                       << data->size() << " bars)\n";
 *             // Decide if this is acceptable for your use case
 *         }
 *         
 *         return data;
 *         
 *     } catch (const std::exception& e) {
 *         std::cerr << "EXCEPTION loading " << symbol 
 *                   << ": " << e.what() << "\n";
 *         return nullptr;
 *     }
 * }
 * 
 * 
 * EXAMPLE 3.5: Integration with Worker Node Job Processing
 * ---------------------------------------------------------
 * 
 * // Simulated worker job handler
 * void Worker::handle_job(const JobMessage& job) {
 *     CSVLoader loader(config_.data_directory);
 *     
 *     // Load all required symbols
 *     std::vector<std::shared_ptr<TimeSeriesData>> datasets;
 *     
 *     for (const auto& symbol : job.symbols) {
 *         auto data = loader.load(symbol);
 *         
 *         if (!data || data->empty()) {
 *             // Report failure to controller
 *             send_error(job.id, "Failed to load " + symbol);
 *             return;
 *         }
 *         
 *         datasets.push_back(data);
 *     }
 *     
 *     // Process all symbols with strategy
 *     for (size_t i = 0; i < job.symbols.size(); ++i) {
 *         BacktestResult result = run_strategy(
 *             job.strategy_name,
 *             datasets[i],
 *             job.parameters
 *         );
 *         
 *         send_partial_result(job.id, job.symbols[i], result);
 *         
 *         // Checkpoint every 1000 symbols (per project plan)
 *         if ((i + 1) % 1000 == 0) {
 *             create_checkpoint(job.id, i + 1);
 *         }
 *     }
 *     
 *     send_job_complete(job.id);
 * }
 ******************************************************************************/

/*******************************************************************************
 * SECTION 4: COMMON PITFALLS & BEST PRACTICES
 *******************************************************************************
 * 
 * PITFALL 4.1: Memory Leaks from Caching Everything
 * --------------------------------------------------
 * 
 * Problem:
 *   CSVLoader caches all loaded symbols indefinitely. If a worker processes
 *   thousands of symbols over its lifetime, memory usage grows unbounded.
 * 
 * Solution:
 *   Periodically clear the cache or implement an LRU (Least Recently Used)
 *   eviction policy.
 * 
 * Example:
 *   // After completing a large batch job
 *   loader.clear_cache();
 *   
 *   // Or implement smart cache management:
 *   if (cache_size_mb > 500) {
 *       loader.clear_cache();
 *   }
 * 
 * 
 * PITFALL 4.2: Incorrect Date Format Assumptions
 * -----------------------------------------------
 * 
 * Problem:
 *   Assuming dates are always in YYYY-MM-DD format without validation.
 *   Real-world CSV files may use:
 *   - MM/DD/YYYY (US format)
 *   - DD/MM/YYYY (European format)
 *   - YYYYMMDD (compact format)
 * 
 * Solution:
 *   Always validate date format during parsing. Consider adding a date
 *   format parameter to CSVLoader constructor.
 * 
 * Best Practice:
 *   // In CSV parsing code:
 *   if (!validate_date_format(date_str)) {
 *       std::cerr << "Invalid date format: " << date_str 
 *                 << " (expected YYYY-MM-DD)\n";
 *       continue;  // Skip this bar
 *   }
 * 
 * 
 * PITFALL 4.3: Not Handling Missing Data
 * ---------------------------------------
 * 
 * Problem:
 *   Stock markets are closed on weekends and holidays. A strategy expecting
 *   continuous daily data will fail when accessing date ranges.
 * 
 * Solution:
 *   Always check if a date exists before using it.
 * 
 * Example:
 *   // BAD: Assumes date exists
 *   size_t idx = data->find_date_index("2023-07-04");  // July 4th holiday
 *   const PriceBar& bar = (*data)[idx];  // Undefined behavior if idx == size()
 *   
 *   // GOOD: Check before access
 *   size_t idx = data->find_date_index("2023-07-04");
 *   if (idx < data->size()) {
 *       const PriceBar& bar = (*data)[idx];
 *       // Process bar...
 *   } else {
 *       // Market was closed, use last available data or skip
 *   }
 * 
 * 
 * PITFALL 4.4: Thread Safety Violations
 * --------------------------------------
 * 
 * Problem:
 *   Sharing a single CSVLoader instance across multiple worker threads
 *   can cause race conditions in the cache.
 * 
 * Solution:
 *   Either:
 *   1. Give each thread its own CSVLoader instance (recommended)
 *   2. Add mutex protection around cache access (adds overhead)
 *   3. Load all data before spawning threads (no concurrent loading)
 * 
 * Example Pattern (Recommended):
 *   // In worker thread function
 *   void worker_thread(const std::vector<std::string>& symbols) {
 *       CSVLoader loader("/data/sp500/");  // Thread-local instance
 *       
 *       for (const auto& symbol : symbols) {
 *           auto data = loader.load(symbol);
 *           process(data);
 *       }
 *   }
 * 
 * 
 * PITFALL 4.5: Forgetting to Sort After Loading
 * ----------------------------------------------
 * 
 * Problem:
 *   CSV files might not be chronologically sorted. Strategies assume
 *   forward time progression, leading to incorrect signals.
 * 
 * Solution:
 *   CSVLoader implementation MUST call sort_by_date() after parsing.
 *   User code should verify data is sorted if loading from external source.
 * 
 * Verification:
 *   // Sanity check after loading
 *   auto data = loader.load("AAPL");
 *   if (data->size() > 1) {
 *       if (data->at(0).date > data->at(1).date) {
 *           std::cerr << "ERROR: Data not sorted chronologically!\n";
 *       }
 *   }
 * 
 * 
 * BEST PRACTICE 4.6: Validate Data Immediately After Loading
 * -----------------------------------------------------------
 * 
 * // Comprehensive validation function
 * bool validate_time_series(const std::shared_ptr<TimeSeriesData>& data) {
 *     if (!data || data->empty()) {
 *         std::cerr << "Data is null or empty\n";
 *         return false;
 *     }
 *     
 *     // Check minimum data points (e.g., 1 year for annual strategies)
 *     if (data->size() < 252) {
 *         std::cerr << "Insufficient data: " << data->size() << " bars\n";
 *         return false;
 *     }
 *     
 *     // Validate all bars
 *     for (size_t i = 0; i < data->size(); ++i) {
 *         const PriceBar& bar = (*data)[i];
 *         
 *         if (!bar.is_valid()) {
 *             std::cerr << "Invalid bar at index " << i 
 *                       << " date=" << bar.date << "\n";
 *             return false;
 *         }
 *     }
 *     
 *     // Check chronological order
 *     for (size_t i = 1; i < data->size(); ++i) {
 *         if ((*data)[i-1].date >= (*data)[i].date) {
 *             std::cerr << "Data not chronologically sorted at index " << i << "\n";
 *             return false;
 *         }
 *     }
 *     
 *     return true;
 * }
 * 
 * 
 * BEST PRACTICE 4.7: Performance Optimization Tips
 * -------------------------------------------------
 * 
 * 1. Preload All Symbols:
 *    // Load all data before starting intensive computation
 *    for (const auto& symbol : all_symbols) {
 *        loader.load(symbol);  // Warms up cache
 *    }
 *    // Now process - all data is in memory
 * 
 * 2. Use References to Avoid Copies:
 *    // BAD: Creates copy of entire vector
 *    std::vector<PriceBar> bars = data->get_all_bars();
 *    
 *    // GOOD: Reference to existing data
 *    const std::vector<PriceBar>& bars = data->get_all_bars();
 * 
 * 3. Reserve Vector Capacity:
 *    // In CSVLoader implementation, if file size is known:
 *    std::vector<PriceBar> bars;
 *    bars.reserve(estimated_rows);  // Avoids reallocations
 * 
 * 4. Minimize Cache Thrashing:
 *    // Process symbols in batches if memory is limited
 *    const size_t BATCH_SIZE = 50;
 *    for (size_t i = 0; i < symbols.size(); i += BATCH_SIZE) {
 *        // Load and process batch
 *        for (size_t j = i; j < i + BATCH_SIZE && j < symbols.size(); ++j) {
 *            process(loader.load(symbols[j]));
 *        }
 *        loader.clear_cache();  // Free memory between batches
 *    }
 ******************************************************************************/

/*******************************************************************************
 * SECTION 5: FAQ
 *******************************************************************************
 * 
 * Q1: Can I use CSVLoader with multi-threaded strategies?
 * --------------------------------------------------------
 * A: Yes, but with caution. The CSVLoader class itself is NOT thread-safe.
 *    Best practice is to give each thread its own CSVLoader instance, or
 *    load all data in a single thread before spawning worker threads.
 *    
 *    If you must share a loader, wrap all load() calls with a mutex.
 * 
 * 
 * Q2: How much memory will caching use?
 * -------------------------------------
 * A: Rule of thumb: ~100KB per symbol for 5 years of daily data.
 *    For 500 symbols (S&P 500), expect ~50MB of memory usage.
 *    
 *    Memory calculation:
 *    - PriceBar: 48 bytes
 *    - 5 years ≈ 1260 trading days
 *    - Per symbol: 1260 × 48 ≈ 60KB
 *    - Plus indexing overhead: ~40KB
 *    - Total: ~100KB per symbol
 * 
 * 
 * Q3: What happens if a CSV file is corrupted or has bad data?
 * -------------------------------------------------------------
 * A: The loader should skip invalid rows and log warnings. Check the
 *    implementation of load() and load_from_file() for error handling.
 *    
 *    Best practice: Always validate returned data:
 *    - Check if shared_ptr is non-null
 *    - Check if size() > 0
 *    - Verify expected date range exists
 * 
 * 
 * Q4: Can I load intraday data (minute/hour bars)?
 * -------------------------------------------------
 * A: Yes, as long as the CSV format matches (Date,Open,High,Low,Close,Volume).
 *    For intraday data, use datetime format: "YYYY-MM-DD HH:MM:SS"
 *    
 *    Note: Date indexing and sorting still work with datetime strings since
 *    ISO 8601 format is lexicographically sortable.
 * 
 * 
 * Q5: How do I handle symbols with limited history?
 * --------------------------------------------------
 * A: After loading, check data->size() against your strategy's minimum
 *    requirements:
 *    
 *    auto data = loader.load("NEWIPO");
 *    if (data->size() < 252) {  // Less than 1 year
 *        // Skip this symbol or use alternative strategy
 *    }
 * 
 * 
 * Q6: Can I preload data from multiple directories?
 * --------------------------------------------------
 * A: Yes, load from different directories by changing data_directory:
 *    
 *    CSVLoader loader;
 *    loader.set_data_directory("/data/stocks/");
 *    auto aapl = loader.load("AAPL");
 *    
 *    loader.set_data_directory("/data/etfs/");
 *    auto spy = loader.load("SPY");
 *    
 *    Or use load_from_file() with explicit paths.
 * 
 * 
 * Q7: How does caching interact with worker failure recovery?
 * ------------------------------------------------------------
 * A: If a worker crashes and a new worker takes over (per your project's
 *    fault tolerance design), the new worker starts with an empty cache.
 *    
 *    The checkpoint system (Module 2) should save which symbols were
 *    processed, allowing the new worker to resume without reprocessing.
 *    The cache is purely for performance within a single worker's lifetime.
 * 
 * 
 * Q8: Should I clear the cache between different backtest jobs?
 * --------------------------------------------------------------
 * A: It depends:
 *    - Same symbols, different strategies: Keep cache (fast)
 *    - Different symbols: May want to clear to free memory
 *    - Memory constrained: Clear after each job
 *    - Memory abundant: Keep cache indefinitely
 *    
 *    Monitor memory usage and clear when needed:
 *    if (memory_usage_mb > threshold) loader.clear_cache();
 * 
 * 
 * Q9: Can I use this loader for live trading data?
 * -------------------------------------------------
 * A: No, this loader is designed for historical backtesting only. For live
 *    trading, you would need:
 *    - Real-time data feeds (websockets, APIs)
 *    - Incremental updates (append new bars)
 *    - Lower latency requirements
 *    
 *    This loader is optimized for batch processing of static historical data.
 * 
 * 
 * Q10: How do I handle corporate actions (splits, dividends)?
 * ------------------------------------------------------------
 * A: Price data should be pre-adjusted for splits. For dividends:
 *    - Total return backtests: Use adjusted prices
 *    - Price return backtests: Use unadjusted prices
 *    
 *    The CSV files you load should already contain the appropriate
 *    adjustment. This loader doesn't perform adjustment calculations.
 ******************************************************************************/

/*******************************************************************************
 * SECTION 6: INTEGRATION WITH DISTRIBUTED SYSTEM
 *******************************************************************************
 * 
 * WORKER NODE INTEGRATION:
 * ------------------------
 * In the context of your distributed backtesting system (per project plan),
 * CSVLoader integrates as follows:
 * 
 * 1. SYSTEM INITIALIZATION (Worker Startup):
 *    ```cpp
 *    class Worker {
 *        CSVLoader data_loader_;
 *        
 *        Worker(const Config& config) 
 *            : data_loader_(config.data_directory) {
 *            // Worker initializes with data directory from config
 *        }
 *    };
 *    ```
 * 
 * 2. JOB PROCESSING WORKFLOW:
 *    ```cpp
 *    void Worker::handle_job_assign(const JobMessage& job) {
 *        std::vector<BacktestResult> results;
 *        
 *        for (const auto& symbol : job.symbols) {
 *            // Load data (cache hit after first load)
 *            auto data = data_loader_.load(symbol);
 *            
 *            if (!data || data->empty()) {
 *                log_error("Failed to load data for " + symbol);
 *                continue;
 *            }
 *            
 *            // Execute strategy
 *            auto result = strategy_engine_.run(
 *                job.strategy_name, 
 *                data, 
 *                job.parameters
 *            );
 *            
 *            results.push_back(result);
 *            
 *            // Checkpoint every 1000 symbols (per project requirements)
 *            if (results.size() % 1000 == 0) {
 *                checkpoint_manager_.save(job.id, results);
 *            }
 *            
 *            // Send heartbeat to controller
 *            if (should_send_heartbeat()) {
 *                send_heartbeat(job.id, results.size());
 *            }
 *        }
 *        
 *        // Send final results to controller
 *        send_results(job.id, results);
 *    }
 *    ```
 * 
 * 3. FAULT TOLERANCE CONSIDERATIONS:
 *    Per your project design, workers can crash during execution. The cache
 *    provides two benefits:
 *    
 *    a) Performance: Same symbol loaded multiple times uses cached data
 *    b) Recovery: If worker fails and job is reassigned, new worker can
 *       load data quickly since data is read-only
 *    
 *    However, cache state is NOT preserved across worker restarts. The
 *    checkpoint system (Module 2) tracks progress independent of cache.
 * 
 * 4. DATA DISTRIBUTION STRATEGIES:
 *    Your project plan mentions two approaches:
 *    
 *    Option A: Shared NFS Storage
 *    - All workers access same NFS-mounted directory
 *    - Pros: Simple, consistent data
 *    - Cons: Network I/O overhead, NFS contention
 *    
 *    Option B: Local Replication
 *    - Pre-copy CSVs to each worker node's local disk
 *    - Pros: Fast local I/O, no network dependency
 *    - Cons: Storage duplication, sync complexity
 *    
 *    CSVLoader works with both approaches - just point data_directory to
 *    the appropriate location:
 *    ```cpp
 *    #ifdef USE_LOCAL_STORAGE
 *        CSVLoader loader("/local/data/sp500/");
 *    #else
 *        CSVLoader loader("/nfs/shared/sp500/");
 *    #endif
 *    ```
 * 
 * 5. PERFORMANCE MONITORING:
 *    Track cache effectiveness for optimization:
 *    
 *    ```cpp
 *    struct LoaderStats {
 *        size_t cache_hits = 0;
 *        size_t cache_misses = 0;
 *        size_t total_loads = 0;
 *        double total_load_time_ms = 0;
 *    };
 *    
 *    // Wrapper around load() to track stats
 *    auto load_with_stats(const std::string& symbol) {
 *        auto start = std::chrono::high_resolution_clock::now();
 *        
 *        bool was_cached = loader.is_cached(symbol);
 *        auto data = loader.load(symbol);
 *        
 *        auto end = std::chrono::high_resolution_clock::now();
 *        double elapsed_ms = std::chrono::duration<double, std::milli>(
 *            end - start
 *        ).count();
 *        
 *        stats.total_loads++;
 *        stats.total_load_time_ms += elapsed_ms;
 *        
 *        if (was_cached) {
 *            stats.cache_hits++;
 *        } else {
 *            stats.cache_misses++;
 *        }
 *        
 *        return data;
 *    }
 *    
 *    // Report in heartbeat to controller
 *    void send_heartbeat() {
 *        HeartbeatMessage msg;
 *        msg.cache_hit_rate = stats.cache_hits / (double)stats.total_loads;
 *        msg.avg_load_time_ms = stats.total_load_time_ms / stats.total_loads;
 *        // ... send to controller
 *    }
 *    ```
 * 
 * 6. SCALABILITY ANALYSIS:
 *    Based on your project goals (8 workers, 0.85 parallel efficiency):
 *    
 *    Bottleneck Analysis:
 *    - If using NFS: Concurrent reads from 8 workers may saturate NFS
 *    - If using local: No I/O contention, excellent scaling
 *    
 *    Data Loading Time Estimate:
 *    - Single CSV (1 symbol, 5 years): ~10ms (local disk)
 *    - 100 symbols: 1 second (first load)
 *    - Subsequent access: <1ms (cached)
 *    
 *    For your target (100 symbols backtest):
 *    - Cold start: ~1 second loading
 *    - Computation: ~60 seconds (varies by strategy)
 *    - Total: Loading is only 1.6% overhead (acceptable)
 * 
 * COMPATIBILITY WITH PROJECT REQUIREMENTS:
 * -----------------------------------------
 * ✓ Supports S&P 500 data (2020-2024, ~500 symbols)
 * ✓ Handles multiple strategies (data reuse via cache)
 * ✓ Works on Khoury Linux cluster (standard C++, no special deps)
 * ✓ Compatible with NFS and local storage
 * ✓ No authentication needed (file-based access control)
 * ✓ Integrates with checkpoint system (data is reloadable)
 * ✓ Supports worker failure recovery (stateless loading)
 ******************************************************************************/

#endif // CSV_LOADER_H