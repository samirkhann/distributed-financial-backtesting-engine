/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: csv_loader.cpp
    
    Description:
        This file implements the CSV data loading and management system for
        the distributed financial backtesting engine. It provides efficient
        parsing, caching, and statistical analysis of historical stock price
        data from CSV files containing OHLCV (Open, High, Low, Close, Volume)
        trading data.
        
        Core Components:
        1. TimeSeriesData: Container for historical price bars with indexing
        2. CSVLoader: File parsing engine with caching and validation
        
        Key Features:
        - Efficient CSV parsing with error recovery
        - In-memory caching to avoid redundant file I/O
        - Date-based indexing for O(1) lookup
        - Statistical computations (mean, volatility)
        - Robust error handling and data validation
        - Support for flexible CSV formats (with/without headers)
        
    Data Flow in Distributed System:
        
        CSV Files (NFS/Local Storage)
              ↓
        CSVLoader::load(symbol)
              ↓
        Parse & Validate → TimeSeriesData
              ↓
        Cache in Memory (shared_ptr)
              ↓
        Worker Loads Data → Executes Backtest
              ↓
        Results → Controller
        
    Design Philosophy:
        
        SHARED OWNERSHIP:
        - TimeSeriesData managed via shared_ptr
        - Multiple workers can hold references to same data
        - Automatic cleanup when all references released
        - Cache maintains long-lived references
        
        DEFENSIVE PARSING:
        - Skip invalid lines (don't crash on bad data)
        - Validate each price bar before adding
        - Detailed error logging for debugging
        - Continue on errors (load as much as possible)
        
        PERFORMANCE OPTIMIZATION:
        - Cache loaded data (avoid repeated file I/O)
        - Date indexing for fast lookups
        - Reserve capacity for known data sizes
        - Minimal allocations during parsing
        
        STATISTICAL ACCURACY:
        - Proper numerical stability in variance calculation
        - Handle edge cases (empty data, single point)
        - Use double precision throughout
        
    File Format Support:
        
        Expected CSV format:
        ─────────────────────────────────────────────────────────
        Date,Open,High,Low,Close,Volume
        2020-01-02,300.35,305.13,299.00,303.00,33911900
        2020-01-03,297.15,300.58,296.30,297.43,36028600
        ...
        ─────────────────────────────────────────────────────────
        
        Alternative formats supported:
        - With or without header row (auto-detection)
        - Extra columns ignored (only first 6 used)
        - Whitespace in fields handled (trimmed)
        - Empty lines skipped
        - Comments not supported (treat as data)
        
    Data Structures:
        
        PriceBar (defined in csv_loader.h):
        ├── date: "YYYY-MM-DD" (ISO 8601)
        ├── open: Opening price (double)
        ├── high: Highest price (double)
        ├── low: Lowest price (double)
        ├── close: Closing price (double)
        └── volume: Trading volume (int64_t)
        
        TimeSeriesData:
        ├── bars_: vector<PriceBar> (chronologically sorted)
        ├── date_index_: map<string, size_t> (date → index)
        └── symbol_: string (stock ticker)
        
    Memory Management:
        
        Typical memory usage:
        - Single symbol (5 years, ~1260 trading days):
          * PriceBar: ~80 bytes each
          * Total: 1260 × 80 = ~100 KB
        - Cache with 500 symbols: ~50 MB
        - Acceptable for distributed system (workers have GB of RAM)
        
        Shared ownership:
        - Cache holds shared_ptr (prevents deletion)
        - Workers hold shared_ptr (safe concurrent access)
        - Automatic cleanup when last reference released
        
    Thread Safety:
        
        TimeSeriesData:
        - Immutable after loading (thread-safe to read)
        - Mutable methods (add_bar, sort_by_date) not thread-safe
        - Pattern: Load once, read many times
        
        CSVLoader:
        - Cache access not thread-safe (single-threaded use assumed)
        - Future: Add mutex around cache_ for multi-threaded loading
        - Workaround: Pre-load all symbols before spawning workers
        
    Performance Characteristics:
        - CSV parsing: ~1-5 ms per file (1260 rows)
        - Cache lookup: ~100 ns (hash map)
        - Date lookup: O(log n) in date_index_ map
        - Range query: O(n) linear scan (could optimize with binary search)
        - Statistics: O(n) single pass
        
    Integration Points:
        - worker/job_executor.cpp: Loads data for backtest computation
        - strategy/asterisk.cpp: Consumes TimeSeriesData for signal generation
        - common/message.h: Job parameters include symbol names
        
    Dependencies:
        - data/csv_loader.h: Class declarations, PriceBar definition
        - common/logger.h: Error reporting and diagnostics
        - <algorithm>: std::sort, std::transform
        - <sstream>: String parsing
        - <cmath>: sqrt for volatility calculation
        
    Related Files:
        - data/csv_loader.h: Class and structure definitions
        - worker/data_loader.cpp: Higher-level data loading logic
        - strategy/sma_strategy.cpp: Consumes TimeSeriesData

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. INCLUDES & DEPENDENCIES
// 2. NAMESPACE DECLARATION
// 3. TIMESERIESDATA IMPLEMENTATION
//    3.1 add_bar() - Add Price Bar with Validation
//    3.2 sort_by_date() - Chronological Sorting
//    3.3 get_range() - Date Range Extraction
//    3.4 find_date_index() - Fast Date Lookup
//    3.5 get_mean_close() - Average Closing Price
//    3.6 get_volatility() - Historical Volatility Calculation
// 4. CSVLOADER IMPLEMENTATION
//    4.1 split() - String Tokenization
//    4.2 trim() - Whitespace Removal
//    4.3 load() - Load with Caching
//    4.4 load_from_file() - CSV File Parsing
// 5. CSV FORMAT SPECIFICATIONS
// 6. USAGE EXAMPLES
//    6.1 Loading Data in Worker
//    6.2 Using TimeSeriesData in Strategy
//    6.3 Pre-loading and Caching
//    6.4 Error Handling
// 7. STATISTICAL ALGORITHMS
// 8. COMMON PITFALLS & SOLUTIONS
// 9. FAQ
// 10. BEST PRACTICES
// 11. PERFORMANCE OPTIMIZATION GUIDE
// 12. TESTING STRATEGIES
// 13. TROUBLESHOOTING GUIDE
// 14. DATA QUALITY VALIDATION
//
//==============================================================================

//==============================================================================
// SECTION 1: INCLUDES & DEPENDENCIES
//==============================================================================

#include "data/csv_loader.h"
#include "common/logger.h"

// <algorithm> - std::sort, std::transform
// For sorting price bars chronologically and string manipulation
#include <algorithm>

// <sstream> - std::stringstream
// For string tokenization (splitting CSV fields)
#include <sstream>

// <cmath> - std::sqrt
// For volatility calculation (square root of variance)
#include <cmath>

// <stdexcept> - std::runtime_error
// For error handling (file not found, invalid data, etc.)
#include <stdexcept>

//==============================================================================
// SECTION 2: NAMESPACE DECLARATION
//==============================================================================

namespace backtesting {

//==============================================================================
// SECTION 3: TIMESERIESDATA IMPLEMENTATION
//==============================================================================
//
// TimeSeriesData manages a collection of price bars (OHLCV data) for a
// single stock symbol, providing efficient access and statistical analysis.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 3.1 ADD_BAR() - ADD PRICE BAR WITH VALIDATION
//------------------------------------------------------------------------------
//
// void TimeSeriesData::add_bar(const PriceBar& bar)
//
// PURPOSE:
// Adds a price bar to the time series after validation.
//
// PARAMETERS:
// - bar: PriceBar to add (date, OHLCV data)
//
// VALIDATION:
// - Calls bar.is_valid() to check price relationships
// - Invalid bars: Logged and skipped (not added)
// - Valid bars: Added to bars_ vector and date_index_ map
//
// PRICE BAR VALIDATION RULES (from is_valid()):
// 1. High >= Low (high must be at or above low)
// 2. High >= Close (close must be at or below high)
// 3. Low <= Close (close must be at or above low)
// 4. High >= Open (open must be at or below high)
// 5. Low <= Open (open must be at or above low)
// 6. All prices > 0 (no negative or zero prices)
// 7. Volume >= 0 (trading volume non-negative)
//
// Example of invalid data:
//   High = 100, Low = 105  → INVALID (low > high)
//   Close = 110, High = 105 → INVALID (close > high)
//
// DATA STRUCTURES UPDATED:
// - bars_: vector append (O(1) amortized)
// - date_index_: map insertion (O(log n))
//
// INDEX MANAGEMENT:
// - date_index_[bar.date] = bars_.size() - 1
// - Maps date string to vector index
// - Enables O(log n) date lookup (vs O(n) linear scan)
//
// DUPLICATE DATES:
// - If bar with same date exists: Overwrites index
// - Only most recent bar for date kept in index
// - Both bars remain in bars_ vector
// - Could be improved: Detect and reject duplicates
//
// THREAD SAFETY:
// - NOT thread-safe: Modifies bars_ and date_index_
// - Pattern: Load data once, then read-only
// - If multi-threaded loading needed: Add mutex
//
// PERFORMANCE:
// - Time: O(log n) for map insertion
// - Space: O(n) for vector + O(n) for map
// - Typical: ~1-2 μs per bar
//

// TimeSeriesData implementation
void TimeSeriesData::add_bar(const PriceBar& bar) {
    // VALIDATION: Check price relationships and constraints
    // is_valid() ensures:
    // - Price hierarchy: low <= open/close <= high
    // - Positive values: All prices > 0
    // - Non-negative volume: volume >= 0
    if (!bar.is_valid()) {
        // Log warning but continue processing
        // Allows loading files with some bad data (defensive parsing)
        Logger::warning("Invalid price bar for " + symbol_ + " on " + bar.date);
        return;  // Skip this bar
    }
    
    // Add to vector (chronological order maintained by caller via sort_by_date)
    // push_back: Amortized O(1) - occasional reallocation
    // Could optimize: bars_.reserve(estimated_size) before loading
    bars_.push_back(bar);
    
    // Add to date index for fast lookup
    // Key: date string (e.g., "2020-01-02")
    // Value: index in bars_ vector
    // Complexity: O(log n) insertion in map
    //
    // Example:
    //   bars_ = [bar0, bar1, bar2]
    //   date_index_ = {
    //       "2020-01-02": 0,
    //       "2020-01-03": 1,
    //       "2020-01-06": 2
    //   }
    //
    // NOTE: If duplicate date, this overwrites index
    // Last bar with this date wins in index
    // Both bars remain in vector (could be data quality issue)
    date_index_[bar.date] = bars_.size() - 1;
}

//------------------------------------------------------------------------------
// 3.2 SORT_BY_DATE() - CHRONOLOGICAL SORTING
//------------------------------------------------------------------------------
//
// void TimeSeriesData::sort_by_date()
//
// PURPOSE:
// Sorts price bars chronologically and rebuilds date index.
//
// WHY SORT?
// - CSV files may have out-of-order dates
// - Backtesting requires chronological data
// - Strategies compute based on previous bars
//
// ALGORITHM:
// - std::sort with custom comparator
// - Compares date strings lexicographically
// - ISO 8601 format: "YYYY-MM-DD" sorts correctly as strings
//
// EXAMPLE:
//   Before: ["2020-01-06", "2020-01-02", "2020-01-03"]
//   After:  ["2020-01-02", "2020-01-03", "2020-01-06"]
//
// INDEX REBUILD:
// - After sorting, indices change
// - Must rebuild date_index_ to match new positions
// - Clear old index, recreate from sorted vector
//
// TIME COMPLEXITY:
// - Sort: O(n log n) comparisons
// - Index rebuild: O(n log n) map insertions
// - Total: O(n log n)
//
// SPACE COMPLEXITY:
// - In-place sort: O(1) extra space (excluding temp)
// - Index rebuild: O(n) space for map
//
// STABILITY:
// - std::sort is not stable (equal dates may reorder)
// - For this data: Dates should be unique
// - If duplicates exist: Undefined which is indexed
//
// WHEN TO CALL:
// - After loading CSV file (data may be unsorted)
// - Before using data for backtest
// - Only once per dataset (immutable after sorting)
//
// THREAD SAFETY:
// - NOT thread-safe: Modifies bars_ and date_index_
// - Call before sharing with other threads
//

void TimeSeriesData::sort_by_date() {
    // Sort bars vector by date (chronological order)
    // Lambda comparator: Returns true if a.date < b.date
    //
    // ISO 8601 dates sort correctly lexicographically:
    //   "2020-01-02" < "2020-01-03" < "2020-01-06" < "2020-02-01"
    //
    // Why lambda? Cleaner syntax than separate compare function
    // Capture: None needed (compare only parameters)
    std::sort(bars_.begin(), bars_.end(), 
              [](const PriceBar& a, const PriceBar& b) {
                  return a.date < b.date;  // Chronological order
              });
    
    // Rebuild date index after sorting
    // Indices changed due to reordering, must recreate mapping
    
    // STEP 1: Clear existing index
    // Old indices are invalid after sort
    date_index_.clear();
    
    // STEP 2: Rebuild index from sorted vector
    // Iterate through bars, map each date to its new position
    for (size_t i = 0; i < bars_.size(); ++i) {
        date_index_[bars_[i].date] = i;
    }
    
    // After this:
    // - bars_ is sorted chronologically
    // - date_index_ maps dates to correct indices
    // - Ready for efficient date-based queries
    //
    // Example:
    //   bars_ = [
    //       PriceBar{date: "2020-01-02", close: 303.00},
    //       PriceBar{date: "2020-01-03", close: 297.43},
    //       PriceBar{date: "2020-01-06", close: 299.80}
    //   ]
    //   date_index_ = {
    //       "2020-01-02": 0,
    //       "2020-01-03": 1,
    //       "2020-01-06": 2
    //   }
}

//------------------------------------------------------------------------------
// 3.3 GET_RANGE() - DATE RANGE EXTRACTION
//------------------------------------------------------------------------------
//
// std::vector<PriceBar> TimeSeriesData::get_range(const std::string& start_date,
//                                                  const std::string& end_date) const
//
// PURPOSE:
// Extracts price bars within specified date range (inclusive).
//
// PARAMETERS:
// - start_date: Start of range (inclusive), format "YYYY-MM-DD"
// - end_date: End of range (inclusive), format "YYYY-MM-DD"
//
// RETURNS:
// - vector<PriceBar>: All bars where start_date <= bar.date <= end_date
// - Empty vector: If no bars in range
//
// ALGORITHM:
// - Linear scan through all bars (O(n))
// - Check date range for each bar
// - Collect matching bars in result vector
//
// OPTIMIZATION OPPORTUNITY:
// - Could use binary search if data is sorted:
//   * Find start index: O(log n)
//   * Find end index: O(log n)
//   * Copy range: O(k) where k = bars in range
//   * Total: O(log n + k) vs. current O(n)
//
// - Not implemented: Simplicity over performance
// - For typical use: Entire dataset is small (~1260 bars)
//
// DATE COMPARISON:
// - Lexicographic string comparison
// - Works correctly for ISO 8601 format
// - Example: "2020-01-02" < "2020-12-31" < "2021-01-01"
//
// INCLUSIVITY:
// - Range is inclusive on both ends
// - start_date and end_date bars included if they exist
//
// EXAMPLE:
//   auto data = loader.load("AAPL");
//   auto bars = data->get_range("2020-01-01", "2020-12-31");
//   // Returns all bars for 2020
//
// THREAD SAFETY:
// - const method: Read-only, thread-safe
// - Returns copy of bars (not reference)
// - Multiple threads can call concurrently
//
// MEMORY:
// - Creates new vector (copy of matching bars)
// - For 1 year: ~250 bars × 80 bytes = ~20 KB
// - Acceptable overhead for typical usage
//

std::vector<PriceBar> TimeSeriesData::get_range(const std::string& start_date,
                                                 const std::string& end_date) const {
    // Result vector: Collect matching bars
    std::vector<PriceBar> result;
    
    // Optional: Reserve capacity if typical range size known
    // result.reserve(250);  // Typical: ~250 trading days per year
    
    // Linear scan: Check each bar's date
    for (const auto& bar : bars_) {
        // Check if bar falls within range (inclusive)
        // Relies on ISO 8601 format for correct lexicographic comparison
        if (bar.date >= start_date && bar.date <= end_date) {
            // Bar is in range: Add to result
            result.push_back(bar);
        }
    }
    
    // Return collected bars
    // Move semantics: Efficient (no copy)
    return result;
    
    // ALTERNATIVE IMPLEMENTATION (with binary search):
    //
    // if (bars_.empty()) return result;
    //
    // // Find start index using lower_bound
    // auto start_it = std::lower_bound(bars_.begin(), bars_.end(), start_date,
    //                                  [](const PriceBar& bar, const std::string& date) {
    //                                      return bar.date < date;
    //                                  });
    //
    // // Find end index using upper_bound
    // auto end_it = std::upper_bound(bars_.begin(), bars_.end(), end_date,
    //                                [](const std::string& date, const PriceBar& bar) {
    //                                    return date < bar.date;
    //                                });
    //
    // // Copy range
    // result.assign(start_it, end_it);
    // return result;
    //
    // Complexity: O(log n + k) vs. O(n) where k = result size
}

//------------------------------------------------------------------------------
// 3.4 FIND_DATE_INDEX() - FAST DATE LOOKUP
//------------------------------------------------------------------------------
//
// size_t TimeSeriesData::find_date_index(const std::string& date) const
//
// PURPOSE:
// Finds index of bar with specified date, or size() if not found.
//
// PARAMETERS:
// - date: Date to find, format "YYYY-MM-DD"
//
// RETURNS:
// - size_t: Index in bars_ vector (0 to size()-1)
// - bars_.size(): Sentinel value if date not found
//
// ALGORITHM:
// - O(log n) lookup in date_index_ map
// - Returns index if found
// - Returns size() if not found (past-the-end marker)
//
// USAGE PATTERN:
//   size_t idx = data->find_date_index("2020-01-15");
//   if (idx < data->size()) {
//       PriceBar bar = data->get_bar(idx);
//       // Use bar
//   } else {
//       // Date not found
//   }
//
// WHY RETURN size() (not -1 or throw)?
// - size_t is unsigned (no negative values)
// - size() is natural sentinel (one-past-end)
// - Consistent with STL conventions (vector::find)
//
// THREAD SAFETY:
// - const method: Read-only, thread-safe
// - date_index_ immutable after sort_by_date()
//
// PERFORMANCE:
// - O(log n) map lookup
// - Typically: ~100-200 ns for 1000-element map
//

size_t TimeSeriesData::find_date_index(const std::string& date) const {
    // Lookup date in index map
    auto it = date_index_.find(date);
    
    // Return index if found, size() if not found
    // Ternary operator: condition ? if_true : if_false
    return (it != date_index_.end()) ? it->second : bars_.size();
    
    // Equivalent if-else:
    // if (it != date_index_.end()) {
    //     return it->second;  // Found: return index
    // } else {
    //     return bars_.size();  // Not found: return sentinel
    // }
}

//------------------------------------------------------------------------------
// 3.5 GET_MEAN_CLOSE() - AVERAGE CLOSING PRICE
//------------------------------------------------------------------------------
//
// double TimeSeriesData::get_mean_close() const
//
// PURPOSE:
// Calculates arithmetic mean of closing prices across all bars.
//
// RETURNS:
// - double: Average closing price
// - 0.0: If no data (empty dataset)
//
// FORMULA:
//   mean = (sum of all close prices) / (number of bars)
//
// EXAMPLE:
//   Closes: [100, 105, 110]
//   Mean: (100 + 105 + 110) / 3 = 105.0
//
// USE CASES:
// - Baseline comparison for returns
// - Mean reversion strategies
// - Data quality checks (extreme means indicate bad data)
//
// NUMERICAL STABILITY:
// - Direct summation (no Kahan summation)
// - Acceptable: Stock prices are well-behaved (no precision issues)
// - For extreme precision: Could use Kahan summation algorithm
//
// EDGE CASES:
// - Empty data: Returns 0.0 (could throw, but 0 is reasonable default)
// - Single bar: Returns that bar's close price
//
// THREAD SAFETY:
// - const method: Read-only, thread-safe
// - No modification of object state
//
// PERFORMANCE:
// - O(n) single pass through data
// - Typical: ~1-2 μs for 1260 bars
//

double TimeSeriesData::get_mean_close() const {
    // EDGE CASE: Empty data
    if (bars_.empty()) return 0.0;
    
    // Accumulate sum of closing prices
    double sum = 0.0;
    for (const auto& bar : bars_) {
        sum += bar.close;
    }
    
    // Calculate arithmetic mean
    // Division by zero prevented by empty check above
    return sum / bars_.size();
    
    // ALTERNATIVE (C++17 accumulate):
    // return std::accumulate(bars_.begin(), bars_.end(), 0.0,
    //                       [](double sum, const PriceBar& bar) {
    //                           return sum + bar.close;
    //                       }) / bars_.size();
}

//------------------------------------------------------------------------------
// 3.6 GET_VOLATILITY() - HISTORICAL VOLATILITY CALCULATION
//------------------------------------------------------------------------------
//
// double TimeSeriesData::get_volatility() const
//
// PURPOSE:
// Calculates historical volatility (standard deviation of returns).
//
// RETURNS:
// - double: Annualized volatility (standard deviation of daily returns)
// - 0.0: If insufficient data (< 2 bars)
//
// ALGORITHM:
// 1. Calculate daily returns: (price[i] - price[i-1]) / price[i-1]
// 2. Calculate mean of returns
// 3. Calculate variance: mean of squared deviations
// 4. Calculate std dev: sqrt(variance)
//
// RETURNS CALCULATION:
// - Simple returns (not log returns)
// - Formula: r[i] = (close[i] - close[i-1]) / close[i-1]
// - Example: 100 → 105: return = 0.05 (5%)
//
// VARIANCE FORMULA:
//   variance = Σ(r[i] - mean)² / n
//
// STANDARD DEVIATION:
//   volatility = √variance
//
// ANNUALIZATION (NOT IMPLEMENTED):
// - Daily volatility: As calculated
// - Annual volatility: daily_vol × √252 (trading days)
// - Sharpe ratio typically uses annual volatility
// - Could add parameter: annualize = true
//
// NUMERICAL STABILITY:
// - Two-pass algorithm (mean, then variance)
// - Numerically stable for typical stock data
// - Alternative: One-pass algorithm (more efficient, less stable)
//
// EDGE CASES:
// - < 2 bars: Cannot calculate returns, return 0.0
// - Zero prices: Skip (would cause division by zero)
// - No valid returns: Return 0.0
//
// THREAD SAFETY:
// - const method: Read-only, thread-safe
//
// PERFORMANCE:
// - O(n) for returns calculation
// - O(n) for mean calculation
// - O(n) for variance calculation
// - Total: O(n) - three passes through data
// - Typical: ~5-10 μs for 1260 bars
//
// USE CASES:
// - Sharpe ratio calculation
// - Risk assessment
// - Position sizing
// - Data quality validation
//

double TimeSeriesData::get_volatility() const {
    // EDGE CASE: Need at least 2 bars to calculate return
    if (bars_.size() < 2) return 0.0;
    
    // STEP 1: Calculate daily returns
    // Returns vector stores percentage changes day-to-day
    std::vector<double> returns;
    
    // Optional: Reserve capacity (avoid reallocations)
    // returns.reserve(bars_.size() - 1);
    
    // Calculate returns from consecutive price pairs
    for (size_t i = 1; i < bars_.size(); ++i) {
        // VALIDATION: Avoid division by zero
        // If previous close is 0 or negative, skip this return
        if (bars_[i-1].close > 0) {
            // Simple return formula: (new - old) / old
            // Example: 100 → 105: (105 - 100) / 100 = 0.05 (5%)
            double ret = (bars_[i].close - bars_[i-1].close) / bars_[i-1].close;
            returns.push_back(ret);
        }
    }
    
    // EDGE CASE: No valid returns calculated
    if (returns.empty()) return 0.0;
    
    // STEP 2: Calculate mean return
    double mean = 0.0;
    for (double r : returns) {
        mean += r;
    }
    mean /= returns.size();
    
    // STEP 3: Calculate variance (mean of squared deviations)
    double variance = 0.0;
    for (double r : returns) {
        double deviation = r - mean;
        variance += deviation * deviation;  // Squared deviation
    }
    variance /= returns.size();  // Mean of squared deviations
    
    // STEP 4: Calculate standard deviation (square root of variance)
    // This is the volatility measure
    return std::sqrt(variance);
    
    // RESULT INTERPRETATION:
    // - Value: Daily volatility (e.g., 0.02 = 2% daily volatility)
    // - Annualized: daily_vol × √252 ≈ daily_vol × 15.87
    // - Example: 0.02 daily → 0.317 (31.7%) annualized
    //
    // TYPICAL VALUES:
    // - Low volatility: 0.01-0.015 daily (15-24% annual)
    // - Medium volatility: 0.015-0.025 daily (24-40% annual)
    // - High volatility: 0.025+ daily (40%+ annual)
    //
    // ALTERNATIVE ALGORITHMS:
    //
    // 1. Exponentially Weighted Moving Average (EWMA):
    //    More weight on recent data
    //    Better for changing volatility
    //
    // 2. GARCH models:
    //    Conditional heteroskedasticity
    //    More complex, better forecasting
    //
    // 3. Log returns (instead of simple returns):
    //    log(price[i] / price[i-1])
    //    Better statistical properties
    //    Time-additive (can sum)
}

//==============================================================================
// SECTION 4: CSVLOADER IMPLEMENTATION
//==============================================================================
//
// CSVLoader parses CSV files containing historical stock price data and
// provides caching to avoid redundant file I/O.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// 4.1 SPLIT() - STRING TOKENIZATION
//------------------------------------------------------------------------------
//
// std::vector<std::string> CSVLoader::split(const std::string& str, 
//                                            char delimiter)
//
// PURPOSE:
// Splits string into tokens based on delimiter (CSV field separator).
//
// PARAMETERS:
// - str: Input string to split (one CSV line)
// - delimiter: Field separator (typically ',' for CSV)
//
// RETURNS:
// - vector<string>: Tokens (empty if str is empty)
//
// ALGORITHM:
// - Uses std::stringstream and std::getline
// - std::getline with delimiter: Reads until delimiter or EOF
// - Each token is trimmed (whitespace removed)
//
// EXAMPLE:
//   Input: "2020-01-02, 100.5 , 105.0,99.0,103.5,1000000"
//   Delimiter: ','
//   Output: ["2020-01-02", "100.5", "105.0", "99.0", "103.5", "1000000"]
//   (Note: Whitespace trimmed from each field)
//
// EDGE CASES:
// - Empty string: Returns empty vector
// - No delimiter: Returns single-element vector (entire string)
// - Consecutive delimiters: Returns empty string tokens
//   Example: "a,,b" → ["a", "", "b"]
//
// TRIMMING:
// - Each token trimmed to remove whitespace
// - Handles: spaces, tabs, newlines, carriage returns
// - Robust: Works with various CSV export formats
//
// PERFORMANCE:
// - O(n) where n = string length
// - Typical: ~1-5 μs per line
// - std::stringstream overhead acceptable for readability
//
// ALTERNATIVE IMPLEMENTATIONS:
//
// 1. Manual parsing (faster but more code):
//    size_t start = 0;
//    while (start < str.size()) {
//        size_t end = str.find(delimiter, start);
//        tokens.push_back(trim(str.substr(start, end - start)));
//        start = (end == std::string::npos) ? str.size() : end + 1;
//    }
//
// 2. Regex (more flexible but slower):
//    std::regex re(",");
//    std::sregex_token_iterator it(str.begin(), str.end(), re, -1);
//
// Current implementation: Good balance of simplicity and performance
//

// CSVLoader implementation
std::vector<std::string> CSVLoader::split(const std::string& str, char delimiter) {
    // Result vector: Will hold all tokens
    std::vector<std::string> tokens;
    
    // Create string stream from input
    // Allows using getline to read until delimiter
    std::stringstream ss(str);
    std::string token;
    
    // Read tokens until end of stream
    // getline(stream, output, delimiter): Reads until delimiter or EOF
    // Returns: false when no more data
    while (std::getline(ss, token, delimiter)) {
        // Trim whitespace from token
        // Handles: "  100.5  " → "100.5"
        tokens.push_back(trim(token));
    }
    
    // Return vector of trimmed tokens
    return tokens;
}

//------------------------------------------------------------------------------
// 4.2 TRIM() - WHITESPACE REMOVAL
//------------------------------------------------------------------------------
//
// std::string CSVLoader::trim(const std::string& str)
//
// PURPOSE:
// Removes leading and trailing whitespace from string.
//
// PARAMETERS:
// - str: Input string (may have whitespace)
//
// RETURNS:
// - string: Trimmed string (no leading/trailing whitespace)
// - Empty string: If str contains only whitespace
//
// WHITESPACE CHARACTERS:
// - Space: ' '
// - Tab: '\t'
// - Carriage return: '\r'
// - Newline: '\n'
//
// ALGORITHM:
// - Find first non-whitespace character (start)
// - Find last non-whitespace character (end)
// - Extract substring [start, end]
//
// EXAMPLES:
//   "  hello  " → "hello"
//   "\t100.5\n" → "100.5"
//   "   " → "" (only whitespace)
//   "hello" → "hello" (no whitespace)
//
// EDGE CASES:
// - Empty string: Returns empty
// - Only whitespace: Returns empty (start = npos)
// - No whitespace: Returns original (substr)
//
// PERFORMANCE:
// - O(n) to find first/last non-whitespace
// - O(m) to create substring where m = trimmed length
// - Typical: ~50-100 ns per string
//
// THREAD SAFETY:
// - Stateless function: Thread-safe
// - Returns new string (no shared state)
//
// WHY NOT USE STL ALGORITHM?
// - No std::trim in standard library (surprisingly)
// - Boost has boost::trim
// - Custom implementation: No external dependencies
//

std::string CSVLoader::trim(const std::string& str) {
    // Find first non-whitespace character
    // Searches for: not space, tab, CR, LF
    // Returns: Index of first match, or npos if none
    size_t start = str.find_first_not_of(" \t\r\n");
    
    // EDGE CASE: String is all whitespace (or empty)
    if (start == std::string::npos) return "";
    
    // Find last non-whitespace character
    // Searches backward from end
    // Returns: Index of last match
    //
    // Note: If start != npos, then end != npos (at least one non-whitespace exists)
    size_t end = str.find_last_not_of(" \t\r\n");
    
    // Extract substring from [start, end] inclusive
    // substr(start, length): Start at 'start', take 'length' characters
    // Length: end - start + 1 (inclusive range)
    //
    // Example:
    //   str = "  hello  "
    //   start = 2 (index of 'h')
    //   end = 6 (index of 'o')
    //   length = 6 - 2 + 1 = 5
    //   result = "hello"
    return str.substr(start, end - start + 1);
}

//------------------------------------------------------------------------------
// 4.3 LOAD() - LOAD WITH CACHING
//------------------------------------------------------------------------------
//
// std::shared_ptr<TimeSeriesData> CSVLoader::load(const std::string& symbol)
//
// PURPOSE:
// Loads historical price data for symbol, using cache if available.
//
// PARAMETERS:
// - symbol: Stock ticker (e.g., "AAPL", "GOOGL")
//
// RETURNS:
// - shared_ptr<TimeSeriesData>: Loaded data
// - Throws: std::runtime_error if file not found or invalid
//
// CACHING STRATEGY:
// - Check cache first (O(log n) map lookup)
// - If found: Return cached data (no file I/O)
// - If not found: Load from file, add to cache, return
//
// CACHE BENEFITS:
// - Avoids redundant file I/O (1-5 ms per file)
// - Multiple workers can share same data (shared_ptr)
// - Memory efficient: One copy per symbol
//
// CACHE INVALIDATION:
// - Not implemented: Cache lives until CSVLoader destroyed
// - For production: Add TTL, manual invalidation, or LRU eviction
//
// FILE PATH CONSTRUCTION:
// - Pattern: data_directory + "/" + symbol + ".csv"
// - Example: "./data/AAPL.csv"
// - Assumes: Symbols are valid filenames
// - Security: No path traversal protection (trusted input assumed)
//
// SHARED OWNERSHIP:
// - Returns shared_ptr (not unique_ptr)
// - Cache holds reference: Data not deleted while cached
// - Workers hold reference: Safe concurrent access
// - Automatic cleanup: Deleted when last reference released
//
// THREAD SAFETY:
// - NOT thread-safe: cache_ access not synchronized
// - Mitigation: Single-threaded loading (one CSVLoader per worker)
// - Future: Add mutex around cache_ for multi-threaded use
//
// EXAMPLE:
//   CSVLoader loader("./data");
//   auto aapl = loader.load("AAPL");  // Loads from file
//   auto aapl2 = loader.load("AAPL"); // Returns cached (same pointer)
//   assert(aapl.get() == aapl2.get()); // Same object
//

std::shared_ptr<TimeSeriesData> CSVLoader::load(const std::string& symbol) {
    // STEP 1: Check cache first (avoid file I/O if possible)
    auto it = cache_.find(symbol);
    if (it != cache_.end()) {
        // Cache hit: Return existing data
        Logger::debug("Loading " + symbol + " from cache");
        return it->second;  // shared_ptr copied (reference count incremented)
    }
    
    // STEP 2: Cache miss: Load from file
    // Construct file path using data directory
    // Pattern: <directory>/<symbol>.csv
    std::string filepath = data_directory_ + "/" + symbol + ".csv";
    
    // Load from file (may throw exception)
    // On success: Data added to cache in load_from_file()
    return load_from_file(symbol, filepath);
    
    // CACHE STATISTICS (not implemented, but useful):
    // - Hit rate: cache_hits / (cache_hits + cache_misses)
    // - Memory usage: cache_.size() × avg_data_size
    // - Could log: "Cache hit rate: 85% (340 hits, 60 misses)"
}

//------------------------------------------------------------------------------
// 4.4 LOAD_FROM_FILE() - CSV FILE PARSING
//------------------------------------------------------------------------------
//
// std::shared_ptr<TimeSeriesData> CSVLoader::load_from_file(
//     const std::string& symbol,
//     const std::string& filepath)
//
// PURPOSE:
// Loads and parses CSV file containing historical price data.
//
// PARAMETERS:
// - symbol: Stock ticker for logging and data object
// - filepath: Full path to CSV file
//
// RETURNS:
// - shared_ptr<TimeSeriesData>: Parsed data (sorted, indexed)
// - Throws: std::runtime_error if file cannot be opened or no valid data
//
// CSV FORMAT EXPECTED:
// ─────────────────────────────────────────────────────────
// Date,Open,High,Low,Close,Volume
// 2020-01-02,300.35,305.13,299.00,303.00,33911900
// 2020-01-03,297.15,300.58,296.30,297.43,36028600
// ─────────────────────────────────────────────────────────
//
// HEADER DETECTION:
// - Checks first line for "date", "open", etc. (case-insensitive)
// - If detected: Skips header line
// - If not detected: Treats as data
// - Robust: Works with or without header
//
// PARSING PROCESS:
// 1. Open file (throw if fails)
// 2. Read line by line
// 3. Skip empty lines
// 4. Detect and skip header (first line only)
// 5. Split by comma
// 6. Validate column count (need at least 6)
// 7. Parse each field (date, OHLCV)
// 8. Validate bar (is_valid())
// 9. Add to TimeSeriesData
// 10. Sort by date
// 11. Cache and return
//
// ERROR HANDLING PHILOSOPHY:
// - Skip invalid lines (log warning, continue)
// - Don't crash on single bad line
// - Throw only if: File not found or NO valid data
// - Defensive: Assume some data corruption possible
//
// FIELD PARSING:
// - tokens[0]: Date (string, no conversion)
// - tokens[1]: Open (std::stod - string to double)
// - tokens[2]: High (std::stod)
// - tokens[3]: Low (std::stod)
// - tokens[4]: Close (std::stod)
// - tokens[5]: Volume (std::stoll - string to long long)
//
// PARSE EXCEPTIONS:
// - std::stod throws: invalid_argument (bad format), out_of_range (too large)
// - std::stoll throws: Same
// - Caught per line: Log warning, skip line, continue
//
// DATA QUALITY:
// - Invalid bars skipped (is_valid() check in add_bar())
// - Missing dates: Gaps in data (weekends, holidays, missing data)
// - Duplicate dates: Both kept in bars_, last indexed
//
// POST-PROCESSING:
// - sort_by_date(): Ensures chronological order
// - Caching: Adds to cache_ for reuse
// - Logging: Reports number of bars loaded
//
// THREAD SAFETY:
// - NOT thread-safe: Modifies cache_
// - Pattern: Single CSVLoader per worker thread
//
// PERFORMANCE:
// - File I/O: ~1-3 ms (dominant cost)
// - Parsing: ~1-2 ms for 1260 lines
// - Sorting: ~100-200 μs (n log n)
// - Total: ~2-5 ms per file (cached after first load)
//
// MEMORY:
// - Temporary: Line buffer, tokens vector
// - Permanent: TimeSeriesData in cache (100 KB per symbol)
// - No leaks: RAII containers, shared_ptr
//

std::shared_ptr<TimeSeriesData> CSVLoader::load_from_file(const std::string& symbol,
                                                           const std::string& filepath) {
    Logger::info("Loading " + symbol + " from " + filepath);
    
    // STEP 1: Open file for reading
    std::ifstream file(filepath);
    if (!file.is_open()) {
        // File not found or permission denied
        // This is a fatal error (can't proceed without data)
        throw std::runtime_error("Failed to open file: " + filepath);
    }
    
    // STEP 2: Create TimeSeriesData container
    // Uses shared_ptr: Enables caching and shared ownership
    auto data = std::make_shared<TimeSeriesData>(symbol);
    
    // STEP 3: Parse file line by line
    std::string line;
    size_t line_num = 0;
    
    while (std::getline(file, line)) {
        line_num++;  // Track line number for error reporting
        
        // Skip empty lines
        // Handles: Blank lines, lines with only whitespace
        if (trim(line).empty()) continue;
        
        // HEADER DETECTION (first line only)
        // Check if first line looks like header
        if (line_num == 1) {
            // Convert to lowercase for case-insensitive detection
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            
            // Check for common header keywords
            // If found: Assume this is header, skip it
            if (lower.find("date") != std::string::npos ||
                lower.find("open") != std::string::npos) {
                continue;  // Skip header line
            }
            // If not header: Fall through to parse as data
        }
        
        // STEP 4: Split line into fields
        auto tokens = split(line, ',');
        
        // VALIDATION: Need at least 6 columns (Date, OHLCV)
        if (tokens.size() < 6) {
            Logger::warning("Invalid line " + std::to_string(line_num) + 
                          " in " + filepath + ": insufficient columns");
            continue;  // Skip this line, continue with next
        }
        
        // STEP 5: Parse each field with exception handling
        try {
            PriceBar bar;
            
            // Field 0: Date (string, no conversion)
            bar.date = tokens[0];
            
            // Field 1: Open price (double)
            // std::stod: String to double conversion
            // Throws: invalid_argument (bad format), out_of_range (too large)
            bar.open = std::stod(tokens[1]);
            
            // Field 2: High price (double)
            bar.high = std::stod(tokens[2]);
            
            // Field 3: Low price (double)
            bar.low = std::stod(tokens[3]);
            
            // Field 4: Close price (double)
            bar.close = std::stod(tokens[4]);
            
            // Field 5: Volume (int64_t)
            // std::stoll: String to long long conversion
            bar.volume = std::stoll(tokens[5]);
            
            // STEP 6: Add bar to data
            // add_bar() validates price relationships
            // Invalid bars are logged and skipped
            data->add_bar(bar);
            
        } catch (const std::exception& e) {
            // Parsing failed (invalid number format, etc.)
            // Log warning with line number and error
            // Continue processing other lines (defensive parsing)
            Logger::warning("Failed to parse line " + std::to_string(line_num) +
                          " in " + filepath + ": " + e.what());
        }
    }
    
    // STEP 7: Close file
    file.close();
    
    // STEP 8: Validate that we loaded some data
    if (data->empty()) {
        // No valid bars loaded
        // This is fatal: Cannot backtest with no data
        throw std::runtime_error("No valid data loaded from " + filepath);
    }
    
    // STEP 9: Sort data chronologically
    // Ensures bars are in order for backtesting
    // Also rebuilds date_index_ with correct indices
    data->sort_by_date();
    
    // STEP 10: Log success
    Logger::info("Loaded " + std::to_string(data->size()) + 
                " bars for " + symbol);
    
    // STEP 11: Cache the data for future use
    // Avoids re-parsing file if requested again
    cache_[symbol] = data;
    
    // STEP 12: Return data
    // shared_ptr: Cache and caller both hold references
    return data;
    
    // PARSING STATISTICS (could be added):
    // - Lines read: line_num
    // - Lines parsed: data->size()
    // - Lines skipped: line_num - data->size() - 1 (minus header)
    // - Parse success rate: data->size() / (line_num - 1)
}

} // namespace backtesting

//==============================================================================
// END OF IMPLEMENTATION
//==============================================================================

//==============================================================================
// SECTION 5: CSV FORMAT SPECIFICATIONS
//==============================================================================
//
// STANDARD FORMAT:
// ================
// Column Order: Date, Open, High, Low, Close, Volume
// Separator: Comma (,)
// Header: Optional (auto-detected)
// Encoding: ASCII or UTF-8
//
// EXAMPLE FILE (AAPL.csv):
// ─────────────────────────────────────────────────────────
// Date,Open,High,Low,Close,Volume
// 2020-01-02,300.35,305.13,299.00,303.00,33911900
// 2020-01-03,297.15,300.58,296.30,297.43,36028600
// 2020-01-06,293.79,299.96,292.75,299.80,36566500
// ─────────────────────────────────────────────────────────
//
// FIELD SPECIFICATIONS:
//
// Date (Column 0):
//   - Format: YYYY-MM-DD (ISO 8601)
//   - Examples: "2020-01-02", "2023-12-31"
//   - Validation: None (treated as string)
//   - Sorting: Lexicographic (works for ISO 8601)
//   - Missing dates: Gaps allowed (weekends, holidays)
//
// Open (Column 1):
//   - Type: Double precision floating point
//   - Units: US Dollars (USD)
//   - Example: 300.35
//   - Range: > 0.0 (must be positive)
//   - Precision: Typically 2-4 decimal places
//
// High (Column 2):
//   - Type: Double
//   - Constraint: >= max(Open, Close)
//   - Highest price during trading day
//
// Low (Column 3):
//   - Type: Double
//   - Constraint: <= min(Open, Close)
//   - Lowest price during trading day
//
// Close (Column 4):
//   - Type: Double
//   - Most important: Used for return calculations
//   - Must satisfy: Low <= Close <= High
//
// Volume (Column 5):
//   - Type: int64_t (long long)
//   - Units: Number of shares traded
//   - Example: 33911900
//   - Range: >= 0 (non-negative)
//   - Typical: Millions of shares for liquid stocks
//
// VARIATIONS SUPPORTED:
//
// 1. No header:
//    2020-01-02,300.35,305.13,299.00,303.00,33911900
//    (First line parsed as data if no header keywords detected)
//
// 2. Extra columns:
//    Date,Open,High,Low,Close,Volume,AdjClose
//    (Only first 6 used, rest ignored)
//
// 3. Whitespace:
//    "2020-01-02 , 300.35 , 305.13, ..."
//    (Trimmed automatically)
//
// VARIATIONS NOT SUPPORTED:
//
// 1. Different column order:
//    Close,Open,High,Low,Date,Volume
//    (Would parse incorrectly - columns assumed in standard order)
//
// 2. Different date formats:
//    "01/02/2020" or "2-Jan-2020"
//    (Treated as string, but sorting would break)
//
// 3. Different separators:
//    Tab-delimited or semicolon-separated
//    (Hard-coded ',' delimiter)
//
// 4. Quoted fields:
//    "2020-01-02","300.35","305.13"
//    (Quotes included in field, causes parse error)
//
// DATA SOURCES:
// - Yahoo Finance: Compatible format
// - Alpha Vantage: Compatible
// - IEX Cloud: Compatible
// - Custom exports: Must match column order
//
//==============================================================================

//==============================================================================
// SECTION 6: USAGE EXAMPLES
//==============================================================================
//
// EXAMPLE 1: Loading Data in Worker
// ==================================
//
// #include "data/csv_loader.h"
//
// void Worker::ExecuteJob(const JobParams& params) {
//     // Create CSV loader with data directory
//     CSVLoader loader("/mnt/nfs/data");
//     
//     // Load symbol data (may throw if file missing)
//     try {
//         auto data = loader.load(params.symbol);
//         
//         Logger::info("Loaded " + std::to_string(data->size()) + 
//                     " bars for " + params.symbol);
//         
//         // Get data for backtest period
//         auto bars = data->get_range(params.start_date, params.end_date);
//         
//         if (bars.size() < 100) {
//             throw std::runtime_error("Insufficient data: need 100+, have " +
//                                    std::to_string(bars.size()));
//         }
//         
//         // Execute strategy with data
//         auto result = strategy->Backtest(bars, params);
//         
//     } catch (const std::runtime_error& e) {
//         Logger::error("Data loading failed: " + std::string(e.what()));
//         // Send error result to controller
//     }
// }
//
//
// EXAMPLE 2: Using TimeSeriesData in Strategy
// ============================================
//
// #include "data/csv_loader.h"
//
// class SMAStrategy {
//     JobResult Backtest(const TimeSeriesData& data,
//                       const JobParams& params) {
//         // Get data for backtest period
//         auto bars = data.get_range(params.start_date, params.end_date);
//         
//         // Validate sufficient data
//         if (bars.size() < params.long_window) {
//             return CreateErrorResult("Insufficient data for " +
//                                     std::to_string(params.long_window) +
//                                     "-day MA");
//         }
//         
//         // Calculate moving averages
//         for (size_t i = params.long_window; i < bars.size(); ++i) {
//             double short_ma = CalculateMA(bars, i, params.short_window);
//             double long_ma = CalculateMA(bars, i, params.long_window);
//             
//             // Generate signals
//             if (short_ma > long_ma) {
//                 // Buy signal
//             }
//         }
//         
//         return result;
//     }
// };
//
//
// EXAMPLE 3: Pre-loading and Caching
// ===================================
//
// void Worker::PreloadData(const std::vector<std::string>& symbols) {
//     CSVLoader loader("./data");
//     
//     Logger::info("Pre-loading " + std::to_string(symbols.size()) + " symbols");
//     
//     for (const auto& symbol : symbols) {
//         try {
//             auto data = loader.load(symbol);
//             
//             // Compute statistics for validation
//             double mean = data->get_mean_close();
//             double vol = data->get_volatility();
//             
//             Logger::info(symbol + ": " + std::to_string(data->size()) +
//                         " bars, mean=$" + std::to_string(mean) +
//                         ", vol=" + std::to_string(vol * 100) + "%");
//             
//         } catch (const std::runtime_error& e) {
//             Logger::error("Failed to load " + symbol + ": " + e.what());
//         }
//     }
//     
//     Logger::info("Pre-loading complete, cache size: " + 
//                 std::to_string(loader.cache_size()));
// }
//
//
// EXAMPLE 4: Error Handling and Recovery
// =======================================
//
// std::shared_ptr<TimeSeriesData> safe_load(CSVLoader& loader,
//                                            const std::string& symbol) {
//     try {
//         return loader.load(symbol);
//         
//     } catch (const std::runtime_error& e) {
//         Logger::error("Failed to load " + symbol + ": " + e.what());
//         
//         // Try alternative file locations
//         try {
//             std::string alt_path = "/backup/data/" + symbol + ".csv";
//             return loader.load_from_file(symbol, alt_path);
//         } catch (const std::runtime_error&) {
//             // Both locations failed
//             Logger::error("Symbol " + symbol + " not available");
//             return nullptr;  // Caller must handle nullptr
//         }
//     }
// }
//
//
// EXAMPLE 5: Data Quality Validation
// ===================================
//
// bool validate_data(const TimeSeriesData& data) {
//     // Check minimum data points
//     if (data.size() < 100) {
//         Logger::error("Insufficient data: " + std::to_string(data.size()));
//         return false;
//     }
//     
//     // Check for gaps (missing data)
//     auto bars = data.get_all_bars();
//     int max_gap = 0;
//     for (size_t i = 1; i < bars.size(); ++i) {
//         // Simplified: Count days between dates (not perfect)
//         max_gap = std::max(max_gap, date_diff(bars[i-1].date, bars[i].date));
//     }
//     if (max_gap > 10) {
//         Logger::warning("Large data gap: " + std::to_string(max_gap) + " days");
//     }
//     
//     // Check statistics
//     double mean = data.get_mean_close();
//     if (mean <= 0.0 || mean > 10000.0) {
//         Logger::error("Suspicious mean price: " + std::to_string(mean));
//         return false;
//     }
//     
//     double vol = data.get_volatility();
//     if (vol < 0.001 || vol > 0.5) {
//         Logger::warning("Unusual volatility: " + std::to_string(vol * 100) + "%");
//     }
//     
//     return true;
// }
//
//==============================================================================

//==============================================================================
// SECTION 7: STATISTICAL ALGORITHMS
//==============================================================================
//
// VOLATILITY CALCULATION (get_volatility)
// ========================================
//
// Mathematical Foundation:
//
// 1. Daily Returns:
//    r[i] = (P[i] - P[i-1]) / P[i-1]
//    
//    Where:
//    - P[i]: Close price on day i
//    - r[i]: Return from day i-1 to day i
//
// 2. Mean Return:
//    μ = (1/n) × Σ r[i]
//
// 3. Variance (Population):
//    σ² = (1/n) × Σ (r[i] - μ)²
//
// 4. Standard Deviation (Volatility):
//    σ = √(σ²)
//
// INTERPRETATION:
// - Daily volatility: σ (as calculated)
// - Annualized volatility: σ × √252 (252 trading days/year)
//
// Example:
//   Daily vol = 0.02 (2%)
//   Annual vol = 0.02 × √252 ≈ 0.02 × 15.87 ≈ 0.317 (31.7%)
//
// NUMERICAL CONSIDERATIONS:
//
// 1. Catastrophic Cancellation:
//    - Two-pass algorithm avoids one-pass instability
//    - One-pass: var = E[r²] - E[r]² (numerically unstable)
//    - Two-pass: Compute mean first, then squared deviations (stable)
//
// 2. Overflow Protection:
//    - Stock returns typically -50% to +50% per day
//    - Squared: 0.25 (well within double range)
//    - No overflow risk for typical data
//
// 3. Division by Zero:
//    - Protected: if (bars_.size() < 2) return 0.0
//    - Protected: if (bars_[i-1].close > 0) before division
//
// ALTERNATIVE VOLATILITY MEASURES:
//
// 1. Exponentially Weighted Moving Average (EWMA):
//    σ²[t] = λ × r²[t] + (1 - λ) × σ²[t-1]
//    More weight on recent data
//
// 2. Parkinson's High-Low Estimator:
//    Uses high and low prices (more efficient)
//    σ² = (1/(4n ln 2)) × Σ (ln(H/L))²
//
// 3. Garman-Klass Estimator:
//    Uses OHLC data (most efficient)
//    More complex formula
//
// Current implementation: Simple historical volatility (industry standard)
//
//==============================================================================

//==============================================================================
// SECTION 8: COMMON PITFALLS & SOLUTIONS
//==============================================================================
//
// PITFALL 1: File Not Found
// ==========================
// PROBLEM:
//    auto data = loader.load("AAPL");
//    // Throws: "Failed to open file: ./data/AAPL.csv"
//
// SOLUTIONS:
//    1. Check file exists before loading:
//       if (!std::filesystem::exists(filepath)) {
//           Logger::error("File not found: " + filepath);
//       }
//    
//    2. Try-catch and handle gracefully:
//       try {
//           auto data = loader.load(symbol);
//       } catch (const std::runtime_error& e) {
//           Logger::error(e.what());
//           // Try alternative source or skip symbol
//       }
//    
//    3. Pre-validate data directory:
//       for (const auto& symbol : symbols) {
//           std::string path = data_dir + "/" + symbol + ".csv";
//           if (!file_exists(path)) {
//               Logger::warning("Missing data: " + path);
//           }
//       }
//
//
// PITFALL 2: Insufficient Data After Date Filtering
// ==================================================
// PROBLEM:
//    auto bars = data->get_range("2024-01-01", "2024-12-31");
//    // Returns empty: No data for 2024
//    ComputeMA(bars, 200);  // Crash: Not enough bars
//
// SOLUTION:
//    auto bars = data->get_range(start_date, end_date);
//    if (bars.size() < params.long_window) {
//        throw std::runtime_error("Insufficient data: need " +
//                                std::to_string(params.long_window) +
//                                ", have " + std::to_string(bars.size()));
//    }
//
//
// PITFALL 3: Unsorted Data
// =========================
// PROBLEM:
//    // CSV file has out-of-order dates
//    // Forgot to call sort_by_date()
//    // Moving average calculation broken (assumes chronological)
//
// SOLUTION:
//    // Always call sort_by_date() after loading
//    data->sort_by_date();
//    
//    // Or verify data is sorted:
//    bool is_sorted = std::is_sorted(bars.begin(), bars.end(),
//                                    [](const PriceBar& a, const PriceBar& b) {
//                                        return a.date < b.date;
//                                    });
//
//
// PITFALL 4: Thread Safety of Cache
// ==================================
// PROBLEM:
//    // Multiple threads using same CSVLoader
//    Thread 1: loader.load("AAPL");  // Modifies cache_
//    Thread 2: loader.load("GOOGL"); // Modifies cache_
//    // Race condition: Concurrent map access
//
// SOLUTIONS:
//    1. One CSVLoader per thread:
//       thread_local CSVLoader loader("./data");
//    
//    2. Pre-load all data before threading:
//       for (auto& symbol : symbols) {
//           loader.load(symbol);
//       }
//       // Now cache is immutable, safe to share
//    
//    3. Add mutex to CSVLoader (not implemented):
//       std::lock_guard<std::mutex> lock(cache_mutex_);
//       cache_[symbol] = data;
//
//
// PITFALL 5: Invalid Price Relationships
// =======================================
// PROBLEM:
//    // CSV has bad data: High < Low, Close > High, etc.
//    // Silently skipped by add_bar()
//    // Result: Missing bars, gaps in data
//
// SOLUTION:
//    // Check data completeness after loading
//    auto data = loader.load(symbol);
//    
//    size_t expected = DaysInRange(start_date, end_date);
//    size_t actual = data->size();
//    
//    if (actual < expected * 0.9) {  // Allow 10% missing
//        Logger::warning("Missing data: expected ~" + 
//                       std::to_string(expected) + ", got " +
//                       std::to_string(actual));
//    }
//
//
// PITFALL 6: Memory Exhaustion from Large Cache
// ==============================================
// PROBLEM:
//    // Load 10,000 symbols
//    // Each ~100 KB = 1 GB total
//    // Out of memory
//
// SOLUTIONS:
//    1. Limit cache size (LRU eviction):
//       if (cache_.size() > MAX_CACHE_SIZE) {
//           // Evict least recently used
//       }
//    
//    2. Load on demand, don't cache:
//       auto data = loader.load_from_file(symbol, path);
//       // Don't add to cache
//    
//    3. Clear cache periodically:
//       loader.clear_cache();
//
//
// PITFALL 7: Date Format Mismatch
// ================================
// PROBLEM:
//    // CSV has: "01/02/2020" (US format)
//    // Code expects: "2020-01-02" (ISO 8601)
//    // Sorting breaks, date lookups fail
//
// SOLUTION:
//    // Convert dates during parsing:
//    std::string convert_date(const std::string& date) {
//        if (date.find('/') != std::string::npos) {
//            // Parse MM/DD/YYYY, convert to YYYY-MM-DD
//            // ... implementation ...
//        }
//        return date;
//    }
//    
//    bar.date = convert_date(tokens[0]);
//
//==============================================================================

//==============================================================================
// SECTION 9: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Why use shared_ptr instead of unique_ptr?
// ==============================================
// A: Shared ownership model:
//    - Cache holds reference (keeps data alive)
//    - Multiple workers can use same data concurrently
//    - Automatic cleanup when last reference released
//    
//    With unique_ptr:
//    - Cache would need to give up ownership
//    - Or workers would need to copy data
//    - shared_ptr is the right choice here
//
// Q2: Is CSVLoader thread-safe?
// ==============================
// A: No, cache_ access is not synchronized.
//    Solutions:
//    - One CSVLoader per thread
//    - Pre-load all data before threading
//    - Add mutex around cache operations (future enhancement)
//
// Q3: What if CSV file is very large (millions of rows)?
// =======================================================
// A: Current implementation loads entire file into memory:
//    - 1M rows × 80 bytes = ~80 MB (manageable)
//    - For larger: Implement streaming or chunked loading
//    - For this project: Stock data is small (~1260 rows/5 years)
//
// Q4: How are missing dates handled (weekends, holidays)?
// ========================================================
// A: Gaps in data are normal:
//    - Stocks don't trade weekends/holidays
//    - Backtest must handle missing bars
//    - Example: 2020-01-03 (Fri) → 2020-01-06 (Mon), no weekend data
//
// Q5: What if two bars have the same date?
// =========================================
// A: Both kept in bars_ vector, but only last is indexed:
//    - Duplicate detection not implemented
//    - find_date_index() returns last occurrence
//    - Data quality issue: Should investigate source
//
// Q6: Can I add custom columns to CSV?
// =====================================
// A: Extra columns are ignored:
//    - Only first 6 columns used (Date, OHLCV)
//    - Could extend: Parse additional columns if needed
//    - Example: Adjusted close, dividends, splits
//
// Q7: How do I handle different CSV formats?
// ===========================================
// A: Current implementation is rigid (assumes column order).
//    For flexibility:
//    - Parse header row, map column names to indices
//    - Use indices for field extraction
//    - More complex but handles reordered columns
//
// Q8: What's the performance of get_range()?
// ===========================================
// A: O(n) linear scan (entire dataset).
//    For typical use (1260 bars): ~5-10 μs (negligible).
//    Could optimize to O(log n + k) with binary search.
//
// Q9: Why not use a CSV parsing library (RapidCSV, csv-parser)?
// ==============================================================
// A: For academic project:
//    - Learning: Understand parsing from first principles
//    - Control: Exact behavior, no external dependencies
//    - Simplicity: ~100 lines vs. library complexity
//    
//    For production: Libraries are excellent (handle edge cases)
//
// Q10: How do I clear the cache?
// ===============================
// A: Not implemented, but would be:
//    void CSVLoader::clear_cache() {
//        cache_.clear();  // Releases all shared_ptr references
//    }
//    
//    Or clear specific symbol:
//    void CSVLoader::evict(const std::string& symbol) {
//        cache_.erase(symbol);
//    }
//
//==============================================================================

//==============================================================================
// SECTION 10: BEST PRACTICES
//==============================================================================
//
// BEST PRACTICE 1: Always Validate Data After Loading
// ====================================================
// DO:
//    auto data = loader.load(symbol);
//    
//    if (data->empty()) {
//        throw std::runtime_error("No data loaded");
//    }
//    
//    if (data->size() < required_bars) {
//        throw std::runtime_error("Insufficient data");
//    }
//
//
// BEST PRACTICE 2: Use Date Range, Don't Access by Index
// =======================================================
// DO:
//    auto bars = data->get_range("2020-01-01", "2024-12-31");
//    // Clear intent, handles missing dates
//
// DON'T:
//    auto bars = data->get_all_bars();
//    // Includes data outside desired range
//
//
// BEST PRACTICE 3: Pre-load Data Before Backtest
// ===============================================
// DO:
//    // Load all symbols at startup
//    for (auto& symbol : symbols) {
//        loader.load(symbol);  // Cached
//    }
//    
//    // Later: Fast cache access
//    auto data = loader.load("AAPL");  // ~100 ns
//
//
// BEST PRACTICE 4: Log Data Loading Statistics
// =============================================
// DO:
//    auto data = loader.load(symbol);
//    Logger::info(symbol + ": " + std::to_string(data->size()) + " bars, " +
//                "mean=$" + std::to_string(data->get_mean_close()) + ", " +
//                "vol=" + std::to_string(data->get_volatility() * 100) + "%");
//
//
// BEST PRACTICE 5: Handle Parse Errors Gracefully
// ================================================
// DO:
//    try {
//        auto data = loader.load(symbol);
//    } catch (const std::runtime_error& e) {
//        Logger::error("Load failed: " + std::string(e.what()));
//        // Try alternative or skip symbol
//    }
//
// DON'T:
//    auto data = loader.load(symbol);  // May throw, not caught
//
//
// BEST PRACTICE 6: Verify Data Quality
// =====================================
// After loading, check:
// - Sufficient bars (>= strategy requirements)
// - Reasonable price ranges (not 0, not extreme)
// - Reasonable volatility (not too low/high)
// - Date continuity (not excessive gaps)
//
//
// BEST PRACTICE 7: Use Const Methods When Possible
// =================================================
// TimeSeriesData has const methods for read-only operations:
// - get_range(), find_date_index(), get_mean_close(), get_volatility()
// - Enables use with const references
// - Documents immutability (data doesn't change)
//
//==============================================================================

//==============================================================================
// SECTION 11: PERFORMANCE OPTIMIZATION GUIDE
//==============================================================================
//
// OPTIMIZATION 1: Reserve Vector Capacity
// ========================================
// Problem: Vector reallocations during parsing
//
// Solution: Reserve capacity if size known
//    auto data = std::make_shared<TimeSeriesData>(symbol);
//    data->reserve(1260);  // Typical: 5 years × 252 days
//    // Now add_bar() won't reallocate
//
// Benefit: ~20% faster loading (fewer allocations)
//
//
// OPTIMIZATION 2: Binary Search for get_range()
// ==============================================
// Problem: O(n) linear scan for range queries
//
// Solution: Use binary search (requires sorted data)
//    auto start_it = std::lower_bound(bars_.begin(), bars_.end(), start_date,
//                                     [](const PriceBar& bar, const std::string& date) {
//                                         return bar.date < date;
//                                     });
//    // O(log n) instead of O(n)
//
// Benefit: 10-100x faster for large datasets
//
//
// OPTIMIZATION 3: Memory-Mapped Files
// ====================================
// Problem: File I/O overhead (1-3 ms per file)
//
// Solution: Use memory-mapped files (mmap)
//    - OS handles caching automatically
//    - Lazy loading (only read needed pages)
//    - Shared across processes
//
// Benefit: 2-5x faster for repeated access
// Complexity: Higher (OS-specific, requires parsing binary format)
//
//
// OPTIMIZATION 4: Parallel CSV Parsing
// =====================================
// Problem: Loading many symbols serially is slow
//
// Solution: Parse multiple files in parallel
//    std::vector<std::future<std::shared_ptr<TimeSeriesData>>> futures;
//    for (auto& symbol : symbols) {
//        futures.push_back(std::async(std::launch::async,
//                                    [&loader, symbol]() {
//                                        return loader.load(symbol);
//                                    }));
//    }
//    
//    for (auto& f : futures) {
//        auto data = f.get();
//    }
//
// Benefit: N× faster on N-core machine
// Note: Requires thread-safe cache (add mutex)
//
//
// OPTIMIZATION 5: Use Faster Parsing
// ===================================
// Problem: std::stringstream and stod are slow
//
// Solution: Custom parsing (strtod, manual field extraction)
//    - 2-3x faster than stringstream
//    - More error-prone (need careful validation)
//    - Not recommended unless profiling shows bottleneck
//
//==============================================================================

//==============================================================================
// SECTION 12: TESTING STRATEGIES
//==============================================================================
//
// UNIT TESTS: TimeSeriesData
// ===========================
//
// TEST(TimeSeriesDataTest, AddValidBar) {
//     TimeSeriesData data("AAPL");
//     
//     PriceBar bar;
//     bar.date = "2020-01-02";
//     bar.open = 300.0;
//     bar.high = 305.0;
//     bar.low = 299.0;
//     bar.close = 303.0;
//     bar.volume = 1000000;
//     
//     data.add_bar(bar);
//     
//     EXPECT_EQ(data.size(), 1);
//     EXPECT_EQ(data.get_bar(0).close, 303.0);
// }
//
// TEST(TimeSeriesDataTest, RejectInvalidBar) {
//     TimeSeriesData data("AAPL");
//     
//     PriceBar bad_bar;
//     bad_bar.date = "2020-01-02";
//     bad_bar.high = 100.0;
//     bad_bar.low = 105.0;  // INVALID: low > high
//     
//     data.add_bar(bad_bar);
//     
//     EXPECT_EQ(data.size(), 0);  // Bar rejected
// }
//
// TEST(TimeSeriesDataTest, SortByDate) {
//     TimeSeriesData data("AAPL");
//     
//     // Add bars in random order
//     data.add_bar(CreateBar("2020-01-06", 100.0));
//     data.add_bar(CreateBar("2020-01-02", 105.0));
//     data.add_bar(CreateBar("2020-01-03", 102.0));
//     
//     data.sort_by_date();
//     
//     EXPECT_EQ(data.get_bar(0).date, "2020-01-02");
//     EXPECT_EQ(data.get_bar(1).date, "2020-01-03");
//     EXPECT_EQ(data.get_bar(2).date, "2020-01-06");
// }
//
// TEST(TimeSeriesDataTest, GetRange) {
//     TimeSeriesData data("AAPL");
//     data.add_bar(CreateBar("2020-01-02", 100.0));
//     data.add_bar(CreateBar("2020-01-15", 105.0));
//     data.add_bar(CreateBar("2020-02-01", 110.0));
//     
//     auto bars = data.get_range("2020-01-10", "2020-01-20");
//     
//     EXPECT_EQ(bars.size(), 1);  // Only 2020-01-15
//     EXPECT_EQ(bars[0].date, "2020-01-15");
// }
//
//
// UNIT TESTS: CSVLoader
// =====================
//
// TEST(CSVLoaderTest, Split) {
//     auto tokens = CSVLoader::split("a,b,c", ',');
//     EXPECT_EQ(tokens.size(), 3);
//     EXPECT_EQ(tokens[0], "a");
// }
//
// TEST(CSVLoaderTest, Trim) {
//     EXPECT_EQ(CSVLoader::trim("  hello  "), "hello");
//     EXPECT_EQ(CSVLoader::trim("\t\n"), "");
// }
//
// TEST(CSVLoaderTest, LoadValidFile) {
//     CSVLoader loader("test_data");
//     auto data = loader.load("TEST_SYMBOL");
//     
//     EXPECT_GT(data->size(), 0);
//     EXPECT_TRUE(std::is_sorted(data->get_all_bars().begin(),
//                                data->get_all_bars().end(),
//                                [](const PriceBar& a, const PriceBar& b) {
//                                    return a.date < b.date;
//                                }));
// }
//
// TEST(CSVLoaderTest, Caching) {
//     CSVLoader loader("test_data");
//     
//     auto data1 = loader.load("AAPL");
//     auto data2 = loader.load("AAPL");
//     
//     EXPECT_EQ(data1.get(), data2.get());  // Same pointer
// }
//
//
// INTEGRATION TESTS:
// ==================
//
// TEST(IntegrationTest, WorkerLoadsAndBacktests) {
//     Worker worker;
//     CSVLoader loader("./data");
//     
//     auto data = loader.load("AAPL");
//     auto result = worker.ExecuteBacktest(data, params);
//     
//     EXPECT_TRUE(result.success);
//     EXPECT_GT(result.num_trades, 0);
// }
//
//
// DATA VALIDATION TESTS:
// ======================
//
// TEST(DataValidationTest, PriceRelationships) {
//     auto data = loader.load("AAPL");
//     
//     for (size_t i = 0; i < data->size(); ++i) {
//         auto bar = data->get_bar(i);
//         EXPECT_GE(bar.high, bar.low);
//         EXPECT_GE(bar.high, bar.close);
//         EXPECT_LE(bar.low, bar.close);
//     }
// }
//
//==============================================================================

//==============================================================================
// SECTION 13: TROUBLESHOOTING GUIDE
//==============================================================================
//
// PROBLEM: "Failed to open file: ./data/AAPL.csv"
// ================================================
// CAUSES:
//    ☐ File doesn't exist at specified path
//    ☐ Incorrect data directory
//    ☐ Permission denied (read permissions)
//    ☐ Symbol name mismatch (case-sensitive: "AAPL" ≠ "aapl")
//
// SOLUTIONS:
//    ☐ Verify file exists: ls -la ./data/AAPL.csv
//    ☐ Check data directory configuration
//    ☐ Check file permissions: chmod +r ./data/*.csv
//    ☐ Check symbol name (exact case match)
//
//
// PROBLEM: "No valid data loaded from file"
// ==========================================
// CAUSES:
//    ☐ All lines are invalid (bad data)
//    ☐ File is empty
//    ☐ Header only, no data rows
//    ☐ Wrong CSV format (wrong columns)
//
// SOLUTIONS:
//    ☐ Check file contents: cat ./data/AAPL.csv | head
//    ☐ Verify CSV format matches expected
//    ☐ Check logs for parse warnings
//    ☐ Validate source data
//
//
// PROBLEM: Wrong number of bars loaded
// =====================================
// CAUSES:
//    ☐ Invalid bars skipped (failed validation)
//    ☐ Parse errors (bad number format)
//    ☐ Missing data in source
//
// SOLUTIONS:
//    ☐ Check logs for warnings (invalid bars, parse errors)
//    ☐ Count lines in CSV: wc -l ./data/AAPL.csv
//    ☐ Compare expected vs. actual count
//    ☐ Validate source data quality
//
//
// PROBLEM: Backtest uses wrong date range
// ========================================
// CAUSES:
//    ☐ Data not sorted chronologically
//    ☐ get_range() returned unexpected bars
//    ☐ Date format mismatch
//
// SOLUTIONS:
//    ☐ Verify sort_by_date() called after loading
//    ☐ Check date format (must be "YYYY-MM-DD")
//    ☐ Log range query results: bars.size(), first/last dates
//    ☐ Verify date comparison logic
//
//
// PROBLEM: High memory usage
// ===========================
// CAUSES:
//    ☐ Large cache (many symbols loaded)
//    ☐ Symbols not released (shared_ptr references held)
//
// SOLUTIONS:
//    ☐ Check cache size: loader.cache_size()
//    ☐ Clear cache periodically (if implemented)
//    ☐ Use unique_ptr instead of shared_ptr (if sharing not needed)
//    ☐ Monitor with: ps aux | grep worker
//
//
// PROBLEM: Parsing is slow
// =========================
// CAUSES:
//    ☐ Large files (millions of rows)
//    ☐ Many files loaded
//    ☐ Slow disk I/O
//
// SOLUTIONS:
//    ☐ Profile with: time ./worker
//    ☐ Pre-load data at startup
//    ☐ Use faster parsing (custom vs. stringstream)
//    ☐ Consider binary format (faster than CSV)
//
//==============================================================================

//==============================================================================
// SECTION 14: DATA QUALITY VALIDATION
//==============================================================================
//
// VALIDATION CHECKS IMPLEMENTED:
// ===============================
//
// 1. Price Relationship Validation (in PriceBar::is_valid()):
//    ✓ High >= Low
//    ✓ Close between Low and High
//    ✓ Open between Low and High
//    ✓ All prices > 0
//    ✓ Volume >= 0
//
// 2. Column Count Validation (in load_from_file()):
//    ✓ At least 6 columns required
//    ✓ Extra columns ignored
//
// 3. Empty Data Check (in load_from_file()):
//    ✓ Throws if no valid bars loaded
//
// ADDITIONAL VALIDATIONS (NOT IMPLEMENTED):
//
// 1. Date Format Validation:
//    bool validate_date_format(const std::string& date) {
//        std::regex iso8601("\\d{4}-\\d{2}-\\d{2}");
//        return std::regex_match(date, iso8601);
//    }
//
// 2. Price Range Validation:
//    if (bar.close < 0.01 || bar.close > 100000.0) {
//        // Suspicious price
//    }
//
// 3. Volume Range Validation:
//    if (bar.volume == 0) {
//        // Zero volume unusual (but possible)
//    }
//
// 4. Duplicate Date Detection:
//    if (date_index_.count(bar.date) > 0) {
//        Logger::warning("Duplicate date: " + bar.date);
//    }
//
// 5. Chronological Order Validation:
//    if (i > 0 && bars_[i].date < bars_[i-1].date) {
//        Logger::warning("Out-of-order date");
//    }
//
// 6. Gap Detection:
//    if (days_between(bars_[i-1].date, bars_[i].date) > 10) {
//        Logger::warning("Large gap in data");
//    }
//
// DATA QUALITY METRICS:
// =====================
//
// Calculate and log:
// - Completeness: bars_loaded / expected_bars
// - Error rate: invalid_bars / total_lines
// - Coverage: date_range_in_data / requested_range
// - Mean/volatility: For sanity checking
//
//==============================================================================

//==============================================================================
// ARCHITECTURAL INTEGRATION NOTES
//==============================================================================
//
// DATA FLOW IN DISTRIBUTED SYSTEM:
// =================================
//
// 1. STORAGE LAYER (NFS or local):
//    - CSV files: AAPL.csv, GOOGL.csv, etc.
//    - Location: Shared across cluster or replicated per worker
//
// 2. LOADING LAYER (CSVLoader):
//    - Parses CSV files
//    - Caches in memory
//    - Provides TimeSeriesData objects
//
// 3. COMPUTATION LAYER (Strategy):
//    - Consumes TimeSeriesData
//    - Generates trading signals
//    - Calculates performance metrics
//
// 4. RESULT LAYER (JobResult):
//    - Returns to controller
//    - Aggregated for client
//
// DEPLOYMENT SCENARIOS:
// =====================
//
// Scenario 1: NFS Shared Storage
//    - All workers access same CSV files via NFS
//    - Benefit: Single data source, consistent
//    - Drawback: Network I/O overhead, NFS latency
//
// Scenario 2: Local Replication
//    - Each worker has local copy of CSV files
//    - Benefit: Fast local I/O, no network
//    - Drawback: Data synchronization complexity
//
// Scenario 3: Controller Distributes Data
//    - Controller loads CSV, sends to workers
//    - Benefit: Centralized data management
//    - Drawback: Network bandwidth for data transfer
//
// Current implementation: Assumes workers have data access (NFS or local)
//
// MEMORY CONSIDERATIONS:
// ======================
//
// Per-symbol memory:
//   - 1 year (~252 bars): ~20 KB
//   - 5 years (~1260 bars): ~100 KB
//   - 20 years (~5040 bars): ~400 KB
//
// Cache capacity:
//   - 100 symbols × 100 KB = 10 MB
//   - 500 symbols × 100 KB = 50 MB
//   - Acceptable for modern systems (GB of RAM)
//
// Shared across workers:
//   - If workers in same process: shared_ptr enables sharing
//   - If workers in separate processes: Each has own cache
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================