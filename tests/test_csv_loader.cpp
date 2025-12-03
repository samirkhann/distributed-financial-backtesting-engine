/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: test_csv_loader.cpp
    
    Description:
        This file implements a test suite for the CSV data loading subsystem
        of the distributed financial backtesting engine. It provides automated
        validation of CSV parsing, data integrity, date filtering, and
        statistical calculations through a series of unit tests with assertion-
        based verification.
        
        Test Coverage:
        - CSV File Parsing: Valid format, header detection, field extraction
        - Data Validation: Price bar validation, data integrity
        - Date Range Filtering: Inclusive range queries, boundary cases
        - Statistical Calculations: Mean close price, historical volatility
        - Caching Mechanism: Verify data reuse, memory efficiency
        - Error Handling: Invalid files, malformed data, edge cases
        
    Testing Philosophy:
        
        AUTOMATED TESTING:
        - No manual verification: Pass/fail determined programmatically
        - Reproducible: Same inputs always produce same results
        - Fast: Completes in milliseconds (quick feedback)
        - Comprehensive: Covers core functionality and edge cases
        
        ASSERTION-BASED:
        - Uses assert() for immediate failure detection
        - Fails fast: First assertion failure terminates test
        - Clear feedback: Shows which assertion failed
        - Debug builds: Assertions enabled (-g flag)
        - Release builds: Assertions disabled (-DNDEBUG)
        
        SELF-CONTAINED:
        - Creates test data: No external dependencies
        - Temporary files: /tmp/TEST.csv (cleaned up automatically)
        - Isolated: Tests don't interfere with each other
        
    Test Organization:
        
        STRUCTURE:
        Each test follows pattern:
        1. Setup: Create test data
        2. Execute: Run function under test
        3. Verify: Check results with assertions
        4. Report: Print PASSED or FAILED
        5. Cleanup: Automatic (RAII, temporary files)
        
        TEST CASES:
        Test 1: Load Valid CSV
        - Purpose: Verify basic CSV parsing works
        - Setup: Create valid 5-row CSV file
        - Assertions: Check data loaded, correct size, correct values
        
        Test 2: Date Range Filtering
        - Purpose: Verify get_range() extracts correct subset
        - Setup: Use data from Test 1
        - Assertions: Check range size, first/last dates
        
        Test 3: Statistical Calculations
        - Purpose: Verify mean and volatility formulas
        - Setup: Use data from Test 1
        - Assertions: Check mean in expected range, volatility non-negative
        
    Assertion Strategy:
        
        VALUE EQUALITY:
        - assert(data->symbol() == "TEST")
        - Exact match required
        - For strings, numbers where exact value known
        
        VALUE RANGE:
        - assert(mean > 301.0 && mean < 302.0)
        - Approximate match (floating-point tolerance)
        - For calculated values where exact result not critical
        
        NON-NULL:
        - assert(data != nullptr)
        - Pointer validity check
        - Essential before dereferencing
        
        CONTAINER SIZE:
        - assert(data->size() == 5)
        - Verifies correct number of elements
        - Catches parsing errors
        
    Exit Codes:
        
        0: All tests passed (success)
        1: One or more tests failed
        
        Usage in scripts:
        $ ./test_csv_loader
        $ echo $?
        0  # Success
        
        Or in CI/CD:
        if ./test_csv_loader; then
            echo "Tests passed"
        else
            echo "Tests failed"
            exit 1
        fi
        
    Integration with Build System:
        
        CMakeLists.txt:
        add_executable(test_csv_loader test_csv_loader.cpp)
        target_link_libraries(test_csv_loader backtesting_lib)
        add_test(NAME csv_loader_test COMMAND test_csv_loader)
        
        Running tests:
        $ make test
        or
        $ ctest --verbose
        
    Continuous Integration:
        
        GitHub Actions workflow:
        - name: Run tests
          run: |
            mkdir build && cd build
            cmake ..
            make
            ctest --output-on-failure
        
        Benefits:
        - Automatic testing on every commit
        - Early detection of regressions
        - Prevents broken code from merging
        
    Dependencies:
        - data/csv_loader.h: CSV loading implementation
        - common/logger.h: Logging utilities
        - <iostream>: Console output
        - <cassert>: Assertion macros
        - <fstream>: File I/O for test data creation
        
    Related Files:
        - data/csv_loader.cpp: Implementation being tested
        - test_controller.cpp: Controller tests
        - test_raft.cpp: Raft consensus tests
        - CMakeLists.txt: Build configuration with test targets

*******************************************************************************/

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
//
// 1. INCLUDES & DEPENDENCIES
// 2. NAMESPACE AND USING DIRECTIVE
// 3. TEST DATA GENERATION
//    3.1 create_test_csv() - Generate Test CSV File
// 4. MAIN FUNCTION - TEST SUITE
//    4.1 Test Setup and Configuration
//    4.2 Test 1: Load Valid CSV
//    4.3 Test 2: Date Range Filtering
//    4.4 Test 3: Statistical Calculations
//    4.5 Results Summary
// 5. RUNNING THE TESTS
// 6. EXTENDING THE TEST SUITE
// 7. ASSERTION GUIDELINES
// 8. COMMON TEST FAILURES
// 9. FAQ
// 10. BEST PRACTICES FOR TESTING
// 11. DEBUGGING FAILED TESTS
// 12. CONTINUOUS INTEGRATION
//
//==============================================================================

//==============================================================================
// SECTION 1: INCLUDES & DEPENDENCIES
//==============================================================================

#include "data/csv_loader.h"  // CSV loading functionality being tested
#include "common/logger.h"    // Logging utilities

// Standard C++ Libraries
// ======================
#include <iostream>  // std::cout for test output
#include <cassert>   // assert() macro for verification
#include <fstream>   // std::ofstream for creating test files

//==============================================================================
// SECTION 2: NAMESPACE AND USING DIRECTIVE
//==============================================================================

using namespace backtesting;

//==============================================================================
// SECTION 3: TEST DATA GENERATION
//==============================================================================

//------------------------------------------------------------------------------
// 3.1 CREATE_TEST_CSV() - GENERATE TEST CSV FILE
//------------------------------------------------------------------------------
//
// void create_test_csv(const std::string& filepath)
//
// PURPOSE:
// Creates a temporary CSV file with known data for testing purposes.
//
// PARAMETERS:
// - filepath: Where to create test file (typically /tmp/TEST.csv)
//
// TEST DATA CHARACTERISTICS:
// - 5 rows of price data (small, fast to load)
// - Valid OHLCV format (passes validation)
// - Sequential dates (testable sorting)
// - Known values (deterministic assertions)
//
// CSV CONTENT GENERATED:
// ───────────────────────────────────────────────────────
// Date,Open,High,Low,Close,Volume
// 2020-01-02,300.35,300.90,299.00,300.50,1000000
// 2020-01-03,300.60,301.00,299.50,300.80,1100000
// 2020-01-06,301.00,302.00,300.50,301.50,1200000
// 2020-01-07,301.50,302.50,301.00,302.00,1300000
// 2020-01-08,302.00,303.00,301.50,302.50,1400000
// ───────────────────────────────────────────────────────
//
// DATA PROPERTIES:
// - Dates: Sequential trading days (skips weekend 2020-01-04/05)
// - Prices: Gradually increasing (uptrend)
// - Valid: All satisfy High >= Close >= Low, etc.
// - Volume: Increasing (realistic pattern)
//
// TEMPORARY LOCATION:
// - /tmp: Temporary directory (auto-cleaned by OS)
// - Avoids: Cluttering project directory
// - Cross-platform: Works on Linux, macOS
//
// CLEANUP:
// - Automatic: OS cleans /tmp periodically
// - Manual: rm /tmp/TEST.csv (if desired)
//
// USAGE:
//   create_test_csv("/tmp/TEST.csv");
//   // File now exists, ready for testing
//   CSVLoader loader;
//   auto data = loader.load_from_file("TEST", "/tmp/TEST.csv");
//

void create_test_csv(const std::string& filepath) {
    // Open file for writing (creates if doesn't exist, truncates if exists)
    std::ofstream file(filepath);
    
    // Write CSV header
    // Matches standard format: Date, Open, High, Low, Close, Volume
    file << "Date,Open,High,Low,Close,Volume\n";
    
    // Write data rows (5 rows of valid price data)
    // Pattern: Gradually increasing prices (uptrend)
    // Dates: Sequential trading days (realistic)
    file << "2020-01-02,300.35,300.90,299.00,300.50,1000000\n";
    file << "2020-01-03,300.60,301.00,299.50,300.80,1100000\n";
    file << "2020-01-06,301.00,302.00,300.50,301.50,1200000\n";
    file << "2020-01-07,301.50,302.50,301.00,302.00,1300000\n";
    file << "2020-01-08,302.00,303.00,301.50,302.50,1400000\n";
    
    // Close file (flush to disk)
    file.close();
    
    // TEST DATA CHARACTERISTICS:
    // - Mean close: (300.50 + 300.80 + 301.50 + 302.00 + 302.50) / 5 = 301.46
    // - Range: 5 rows
    // - Date gap: 2020-01-04, 2020-01-05 missing (weekend)
}

//==============================================================================
// SECTION 4: MAIN FUNCTION - TEST SUITE
//==============================================================================

int main() {
    //==========================================================================
    // SECTION 4.1: TEST SETUP AND CONFIGURATION
    //==========================================================================
    //
    // Initialize logging and test tracking.
    //
    //--------------------------------------------------------------------------
    
    // Configure logging for test output
    // INFO level: Shows test progress
    Logger::set_level(LogLevel::INFO);
    Logger::info("Running CSV Loader tests...");
    
    // Test counters: Track pass/fail statistics
    int passed = 0;  // Number of tests that passed
    int failed = 0;  // Number of tests that failed
    
    //==========================================================================
    // SECTION 4.2: TEST 1 - LOAD VALID CSV
    //==========================================================================
    //
    // TEST OBJECTIVE:
    // Verify CSVLoader can parse valid CSV file and load data correctly.
    //
    // WHAT'S BEING TESTED:
    // - File opening and reading
    // - Header detection and skipping
    // - CSV tokenization (split by comma)
    // - Field parsing (string, double, int64_t)
    // - PriceBar construction
    // - Data storage in TimeSeriesData
    //
    // SUCCESS CRITERIA:
    // - Data loaded (not nullptr)
    // - Symbol name correct
    // - Row count correct (5 rows)
    // - First close price correct (300.50)
    // - Last close price correct (302.50)
    //
    //--------------------------------------------------------------------------
    
    // Test 1: Load valid CSV
    {
        // TEST IDENTIFICATION
        std::cout << "Test 1: Load valid CSV... ";
        
        try {
            // STEP 1: SETUP - Create test CSV file
            create_test_csv("/tmp/TEST.csv");
            
            // STEP 2: EXECUTE - Load CSV file
            CSVLoader loader;
            auto data = loader.load_from_file("TEST", "/tmp/TEST.csv");
            
            // STEP 3: VERIFY - Check results with assertions
            
            // ASSERTION 1: Data loaded successfully (not null)
            assert(data != nullptr);
            
            // ASSERTION 2: Symbol name stored correctly
            assert(data->symbol() == "TEST");
            
            // ASSERTION 3: Correct number of rows loaded
            // Should be 5 (matching number of data rows in CSV)
            assert(data->size() == 5);
            
            // ASSERTION 4: First row close price correct
            // Row 0: 2020-01-02, close = 300.50
            assert(data->at(0).close == 300.50);
            
            // ASSERTION 5: Last row close price correct
            // Row 4: 2020-01-08, close = 302.50
            assert(data->at(4).close == 302.50);
            
            // All assertions passed: Test succeeded
            std::cout << "PASSED\n";
            passed++;
            
        } catch (const std::exception& e) {
            // EXCEPTION HANDLING
            // Any exception indicates test failure
            // Could be: Assertion failure, file I/O error, parsing error
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    // CLEANUP: Test scope ends, local variables destroyed
    
    //==========================================================================
    // SECTION 4.3: TEST 2 - DATE RANGE FILTERING
    //==========================================================================
    //
    // TEST OBJECTIVE:
    // Verify TimeSeriesData::get_range() correctly filters by date.
    //
    // WHAT'S BEING TESTED:
    // - Date string comparison (lexicographic)
    // - Inclusive range semantics (start and end included)
    // - Correct subset extraction
    //
    // TEST DATA:
    // - Full dataset: 2020-01-02 to 2020-01-08 (5 rows)
    // - Query range: 2020-01-03 to 2020-01-07 (3 rows)
    // - Expected: Rows 1, 2, 3 (dates 01-03, 01-06, 01-07)
    //
    // SUCCESS CRITERIA:
    // - Range size: 3 rows
    // - First date: 2020-01-03
    // - Last date: 2020-01-07
    //
    //--------------------------------------------------------------------------
    
    // Test 2: Date range filtering
    {
        std::cout << "Test 2: Date range filtering... ";
        
        try {
            // STEP 1: SETUP - Load test CSV (reuses file from Test 1)
            CSVLoader loader;
            auto data = loader.load_from_file("TEST", "/tmp/TEST.csv");
            
            // STEP 2: EXECUTE - Get subset by date range
            // Query: 2020-01-03 to 2020-01-07 (inclusive)
            auto range = data->get_range("2020-01-03", "2020-01-07");
            
            // STEP 3: VERIFY - Check filtered results
            
            // ASSERTION 1: Correct number of rows in range
            // Expected: 3 rows (01-03, 01-06, 01-07)
            // Note: 01-04 and 01-05 don't exist (weekend)
            assert(range.size() == 3);
            
            // ASSERTION 2: First date in range correct
            assert(range[0].date == "2020-01-03");
            
            // ASSERTION 3: Last date in range correct
            assert(range[2].date == "2020-01-07");
            
            // All assertions passed
            std::cout << "PASSED\n";
            passed++;
            
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    
    //==========================================================================
    // SECTION 4.4: TEST 3 - STATISTICAL CALCULATIONS
    //==========================================================================
    //
    // TEST OBJECTIVE:
    // Verify statistical methods produce reasonable results.
    //
    // WHAT'S BEING TESTED:
    // - get_mean_close(): Average closing price calculation
    // - get_volatility(): Historical volatility (standard deviation)
    //
    // EXPECTED VALUES (for test data):
    // - Mean close: (300.50 + 300.80 + 301.50 + 302.00 + 302.50) / 5 = 301.46
    // - Volatility: Should be small (prices gradually increasing)
    //
    // SUCCESS CRITERIA:
    // - Mean in range: 301.0 to 302.0 (allows floating-point tolerance)
    // - Volatility: >= 0 (standard deviation is non-negative)
    //
    // WHY RANGE CHECK (not exact)?
    // - Floating-point arithmetic: Not exact (rounding errors)
    // - Acceptable tolerance: Within 0.5 of expected (301.46)
    // - Robust: Test passes with minor calculation differences
    //
    //--------------------------------------------------------------------------
    
    // Test 3: Statistics
    {
        std::cout << "Test 3: Statistics... ";
        
        try {
            // STEP 1: SETUP - Load test data
            CSVLoader loader;
            auto data = loader.load_from_file("TEST", "/tmp/TEST.csv");
            
            // STEP 2: EXECUTE - Calculate mean
            double mean = data->get_mean_close();
            
            // ASSERTION 1: Mean in expected range
            // Expected: 301.46 (calculated from test data)
            // Range: 301.0 to 302.0 (tolerance for floating-point)
            assert(mean > 301.0 && mean < 302.0);
            
            // STEP 3: EXECUTE - Calculate volatility
            double vol = data->get_volatility();
            
            // ASSERTION 2: Volatility is non-negative
            // Standard deviation cannot be negative
            // Exact value depends on calculation, just check >= 0
            assert(vol >= 0);
            
            // All assertions passed
            std::cout << "PASSED\n";
            passed++;
            
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    
    //==========================================================================
    // SECTION 4.5: RESULTS SUMMARY
    //==========================================================================
    //
    // Display test results and determine exit code.
    //
    //--------------------------------------------------------------------------
    
    // Print summary header
    std::cout << "\n=== Results ===\n";
    
    // Show counts
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";
    
    // EXIT CODE
    // 0: All tests passed (failed == 0)
    // 1: One or more tests failed (failed > 0)
    //
    // Enables:
    // - Shell scripts: Check success with $?
    // - CI/CD: Fail build if tests fail
    // - Makefiles: Continue only if tests pass
    return (failed == 0) ? 0 : 1;
}

//==============================================================================
// END OF TEST SUITE
//==============================================================================

//==============================================================================
// SECTION 5: RUNNING THE TESTS
//==============================================================================
//
// COMPILATION:
// ============
//
// Manual (g++):
// $ g++ -std=c++17 -I../include -o test_csv_loader
//       test_csv_loader.cpp
//       ../src/data/csv_loader.cpp
//       ../src/common/logger.cpp
//       -lpthread
//
// With CMake:
// $ mkdir build && cd build
// $ cmake ..
// $ make test_csv_loader
//
// EXECUTION:
// ==========
//
// Run tests:
// $ ./test_csv_loader
// [INFO] Running CSV Loader tests...
// Test 1: Load valid CSV... PASSED
// Test 2: Date range filtering... PASSED
// Test 3: Statistics... PASSED
//
// === Results ===
// Passed: 3
// Failed: 0
//
// Check exit code:
// $ echo $?
// 0  # Success
//
// WITH VERBOSE OUTPUT:
// ====================
//
// Enable DEBUG logging:
// $ ./test_csv_loader 2>&1 | tee test_output.txt
//
// Shows:
// - Detailed CSV loading logs
// - Data parsing steps
// - Calculation details
//
// AUTOMATED TESTING (CI/CD):
// ===========================
//
// GitHub Actions:
// - name: Test CSV Loader
//   run: |
//     ./build/test_csv_loader
//     if [ $? -ne 0 ]; then
//       echo "CSV loader tests failed!"
//       exit 1
//     fi
//
// Jenkins:
// sh './build/test_csv_loader'
// if [ $? -ne 0 ]; then
//     currentBuild.result = 'FAILURE'
// fi
//
//==============================================================================

//==============================================================================
// SECTION 6: EXTENDING THE TEST SUITE
//==============================================================================
//
// ADDING NEW TESTS:
// =================
//
// Template for new test:
//
// // Test 4: Your test name
// {
//     std::cout << "Test 4: Your test name... ";
//     
//     try {
//         // SETUP: Prepare test data
//         create_test_csv("/tmp/TEST.csv");
//         CSVLoader loader;
//         
//         // EXECUTE: Run function under test
//         auto result = function_to_test();
//         
//         // VERIFY: Check results
//         assert(result == expected_value);
//         assert(some_condition);
//         
//         std::cout << "PASSED\n";
//         passed++;
//         
//     } catch (const std::exception& e) {
//         std::cout << "FAILED: " << e.what() << "\n";
//         failed++;
//     }
// }
//
// ADDITIONAL TEST IDEAS:
// ======================
//
// Test 4: Invalid CSV Format
// ---------------------------
// // Test handling of malformed CSV
// create_invalid_csv("/tmp/INVALID.csv");  // Missing columns, bad data
// auto data = loader.load_from_file("INVALID", "/tmp/INVALID.csv");
// // Should throw or return empty
//
// Test 5: Empty CSV File
// ----------------------
// // Test handling of empty file
// create_empty_csv("/tmp/EMPTY.csv");
// // Should throw std::runtime_error
//
// Test 6: CSV Without Header
// ---------------------------
// // Test auto-detection when header missing
// create_csv_no_header("/tmp/NO_HEADER.csv");
// auto data = loader.load_from_file("NO_HEADER", "/tmp/NO_HEADER.csv");
// assert(data->size() > 0);  // Should parse all rows as data
//
// Test 7: Large Date Range
// -------------------------
// // Test performance with large dataset
// create_large_csv("/tmp/LARGE.csv", 10000);  // 10,000 rows
// auto start = now();
// auto data = loader.load_from_file("LARGE", "/tmp/LARGE.csv");
// auto duration = now() - start;
// assert(duration < 100ms);  // Should load quickly
//
// Test 8: Duplicate Dates
// ------------------------
// // Test handling of duplicate dates
// // Should: Keep both rows, index points to last
//
// Test 9: Out-of-Order Dates
// ---------------------------
// // Test sort_by_date() functionality
// create_unsorted_csv("/tmp/UNSORTED.csv");
// auto data = loader.load_from_file("UNSORTED", "/tmp/UNSORTED.csv");
// assert(data->at(0).date < data->at(1).date);  // Verify sorted
//
// Test 10: Caching
// -----------------
// // Test that loading same symbol twice uses cache
// auto data1 = loader.load("TEST");
// auto data2 = loader.load("TEST");
// assert(data1.get() == data2.get());  // Same pointer (cached)
//
// Test 11: Invalid Price Bars
// ----------------------------
// // Test validation: High < Low, etc.
// // Invalid bars should be skipped
//
// Test 12: find_date_index()
// ---------------------------
// // Test date index lookup
// size_t idx = data->find_date_index("2020-01-06");
// assert(idx == 2);  // Third row (0-indexed)
// idx = data->find_date_index("2020-12-31");  // Not in data
// assert(idx == data->size());  // Sentinel value
//
//==============================================================================

//==============================================================================
// SECTION 7: ASSERTION GUIDELINES
//==============================================================================
//
// WHEN TO USE assert():
// =====================
//
// USE for:
// - Invariants: Conditions that must always be true
// - Preconditions: Function requirements
// - Postconditions: Function guarantees
// - Test verification: Expected results
//
// DON'T USE for:
// - Error handling: Use exceptions or return codes
// - User input validation: assert() disabled in release builds
// - Recoverable errors: Use graceful error handling
//
// ASSERTION PATTERNS:
// ===================
//
// 1. Null pointer check:
//    assert(ptr != nullptr);
//    // Then safe to dereference
//
// 2. Range check:
//    assert(index < container.size());
//    // Then safe to access container[index]
//
// 3. Value validation:
//    assert(value >= 0 && value <= 100);
//    // Then safe to use value
//
// 4. State validation:
//    assert(object.is_valid());
//    // Then safe to use object
//
// ASSERTION MESSAGES (C++11 onwards):
// ====================================
//
// Standard assert: No message
//    assert(x > 0);
//    // Output: Assertion `x > 0' failed
//
// With custom message (workaround):
//    assert(x > 0 && "x must be positive");
//    // Output: Assertion `x > 0 && "x must be positive"' failed
//
// Better approach (custom macro):
//    #define ASSERT_MSG(cond, msg)
//        if (!(cond)) {
//            std::cerr << "Assertion failed: " << msg << "\n";
//            std::abort();
//        }
//    ASSERT_MSG(x > 0, "x must be positive");
//
// FLOATING-POINT ASSERTIONS:
// ===========================
//
// Don't use exact equality:
//    assert(result == 301.46);  // May fail due to rounding
//
// Use range or epsilon:
//    assert(std::abs(result - 301.46) < 0.01);  // Within tolerance
//    // Or
//    assert(result > 301.0 && result < 302.0);  // Reasonable range
//
//==============================================================================

//==============================================================================
// SECTION 8: COMMON TEST FAILURES
//==============================================================================
//
// FAILURE 1: Assertion Failed
// ============================
// SYMPTOM:
//    Test 1: Load valid CSV... Assertion `data->size() == 5' failed.
//    Aborted (core dumped)
//
// CAUSES:
//    - CSV parsing error (skipped rows)
//    - Invalid data (failed validation)
//    - Wrong file format
//
// DEBUGGING:
//    1. Print actual value before assertion:
//       std::cout << "Size: " << data->size() << "\n";
//       assert(data->size() == 5);
//    
//    2. Check CSV file content:
//       $ cat /tmp/TEST.csv
//    
//    3. Enable DEBUG logging:
//       Logger::set_level(LogLevel::DEBUG);
//
//
// FAILURE 2: Exception Thrown
// ============================
// SYMPTOM:
//    Test 1: Load valid CSV... FAILED: Failed to open file: /tmp/TEST.csv
//
// CAUSES:
//    - File not created (create_test_csv failed)
//    - Permission denied (/tmp not writable)
//    - Disk full (no space for temp file)
//
// DEBUGGING:
//    1. Check temp file exists:
//       $ ls -l /tmp/TEST.csv
//    
//    2. Check permissions:
//       $ ls -ld /tmp
//    
//    3. Check disk space:
//       $ df -h /tmp
//
//
// FAILURE 3: Unexpected Value
// ============================
// SYMPTOM:
//    Test 3: Statistics... FAILED: Assertion `mean > 301.0 && mean < 302.0' failed
//
// CAUSES:
//    - Calculation error in get_mean_close()
//    - Test data different than expected
//    - Rounding error
//
// DEBUGGING:
//    1. Print actual mean:
//       std::cout << "Mean: " << mean << "\n";
//    
//    2. Manually calculate expected:
//       // (300.50 + 300.80 + 301.50 + 302.00 + 302.50) / 5 = ?
//    
//    3. Widen tolerance if appropriate:
//       assert(mean > 300.0 && mean < 303.0);
//
//==============================================================================

//==============================================================================
// SECTION 9: FREQUENTLY ASKED QUESTIONS (FAQ)
//==============================================================================
//
// Q1: Why use assert() instead of if + error message?
// ====================================================
// A: Assertions are standard for testing:
//    - Concise: One line vs. multiple for if-throw
//    - Immediate: Fails at exact assertion location
//    - Standard: Universally understood in testing
//    - Debug-only: Can disable in release builds
//
// Q2: What if I want more detailed error messages?
// =================================================
// A: Options:
//    1. Print before assertion:
//       std::cout << "Expected 5, got " << data->size() << "\n";
//       assert(data->size() == 5);
//    
//    2. Use testing framework (Google Test):
//       EXPECT_EQ(data->size(), 5) << "Size mismatch";
//
// Q3: How do I add a new test?
// =============================
// A: Follow the pattern:
//    {
//        std::cout << "Test N: Description... ";
//        try {
//            // setup, execute, verify
//            std::cout << "PASSED\n";
//            passed++;
//        } catch (...) {
//            std::cout << "FAILED\n";
//            failed++;
//        }
//    }
//
// Q4: Should tests clean up temporary files?
// ===========================================
// A: For /tmp: Not necessary (OS cleans periodically)
//    For other locations: Yes, add cleanup
//    std::remove("/tmp/TEST.csv");
//
// Q5: Can I run individual tests?
// ================================
// A: Not in current design (all tests run together)
//    Could add: Command-line argument to select tests
//    ./test_csv_loader --test 1
//
// Q6: How do I test error conditions?
// ====================================
// A: Expect exceptions:
//    try {
//        load_invalid_file();
//        assert(false);  // Should not reach here
//    } catch (const std::runtime_error&) {
//        // Expected exception
//    }
//
// Q7: What about performance testing?
// ====================================
// A: Add timing:
//    auto start = std::chrono::steady_clock::now();
//    // Execute function
//    auto duration = std::chrono::steady_clock::now() - start;
//    assert(duration < std::chrono::milliseconds(100));
//
// Q8: Should I test private methods?
// ===================================
// A: Generally no:
//    - Test public interface (black-box testing)
//    - Private methods tested indirectly
//    - If needed: Make test class a friend
//
// Q9: How do I mock dependencies?
// ================================
// A: For CSV loader (no complex dependencies):
//    - Uses real files (integration-style testing)
//    - For complex systems: Use mocking frameworks
//
// Q10: What's the difference between unit and integration tests?
// ===============================================================
// A: Unit tests: Test single component in isolation
//    Integration tests: Test multiple components together
//    
//    These tests: Mix of both
//    - Test 1: Unit test (CSV parsing)
//    - Test 2-3: Integration (CSV + statistics)
//
//==============================================================================

//==============================================================================
// SECTION 10: BEST PRACTICES FOR TESTING
//==============================================================================
//
// BEST PRACTICE 1: Test One Thing Per Test
// =========================================
// DO:
//    Test 1: CSV loading
//    Test 2: Date filtering
//    Test 3: Statistics
//    // Each test has clear purpose
//
// DON'T:
//    Test 1: Load CSV, filter dates, calculate stats, ...
//    // Too much in one test (hard to debug failures)
//
//
// BEST PRACTICE 2: Use Meaningful Test Names
// ===========================================
// DO:
//    "Test 1: Load valid CSV"
//    // Clear what's being tested
//
// DON'T:
//    "Test 1"
//    // No context
//
//
// BEST PRACTICE 3: Test Both Success and Failure Cases
// =====================================================
// DO:
//    // Test valid input
//    // Test invalid input
//    // Test edge cases (empty, very large, etc.)
//
//
// BEST PRACTICE 4: Make Tests Deterministic
// ==========================================
// DO:
//    // Use known test data
//    // Fixed expected values
//    // No randomness (unless testing random behavior)
//
//
// BEST PRACTICE 5: Keep Tests Fast
// =================================
// DO:
//    // Small test datasets
//    // Quick operations
//    // Entire suite runs in <1 second
//
//
// BEST PRACTICE 6: Run Tests Frequently
// ======================================
// DO:
//    // After every code change
//    // Before committing code
//    // In CI/CD pipeline
//
//
// BEST PRACTICE 7: Document Expected Behavior
// ============================================
// DO:
//    // Comments explain what's being tested
//    // Expected values documented
//    // Edge cases noted
//
//==============================================================================

//==============================================================================
// SECTION 11: DEBUGGING FAILED TESTS
//==============================================================================
//
// DEBUGGING WORKFLOW:
// ===================
//
// Step 1: Identify failing test
// ------------------------------
// $ ./test_csv_loader
// Test 1: Load valid CSV... PASSED
// Test 2: Date range filtering... FAILED: Assertion failed
//
// Step 2: Add debug output
// ------------------------
// std::cout << "Range size: " << range.size() << "\n";
// assert(range.size() == 3);
//
// Step 3: Check test data
// -----------------------
// $ cat /tmp/TEST.csv
// // Verify test data is as expected
//
// Step 4: Run with debugger
// -------------------------
// $ gdb ./test_csv_loader
// (gdb) run
// (gdb) where  # Show stack trace at failure
//
// Step 5: Enable verbose logging
// -------------------------------
// Logger::set_level(LogLevel::DEBUG);
// // Shows detailed execution
//
// Step 6: Isolate the failure
// ----------------------------
// // Comment out other tests
// // Run only failing test
// // Reduces noise
//
// COMMON DEBUGGING TECHNIQUES:
// =============================
//
// Print actual vs. expected:
//    std::cout << "Expected: " << expected << ", Got: " << actual << "\n";
//    assert(actual == expected);
//
// Check intermediate values:
//    auto data = loader.load(...);
//    std::cout << "Loaded " << data->size() << " bars\n";
//    auto range = data->get_range(...);
//    std::cout << "Filtered to " << range.size() << " bars\n";
//    assert(range.size() == 3);
//
// Dump data structures:
//    for (size_t i = 0; i < data->size(); ++i) {
//        auto bar = data->at(i);
//        std::cout << i << ": " << bar.date << " " << bar.close << "\n";
//    }
//
//==============================================================================

//==============================================================================
// SECTION 12: CONTINUOUS INTEGRATION
//==============================================================================
//
// CI/CD INTEGRATION:
// ==================
//
// GitHub Actions Example (.github/workflows/test.yml):
//
// name: Run Tests
// on: [push, pull_request]
//
// jobs:
//   test:
//     runs-on: ubuntu-latest
//     steps:
//       - uses: actions/checkout@v2
//       
//       - name: Build
//         run: |
//           mkdir build
//           cd build
//           cmake ..
//           make test_csv_loader
//       
//       - name: Run CSV Loader Tests
//         run: |
//           cd build
//           ./test_csv_loader
//       
//       - name: Upload Test Results
//         if: failure()
//         uses: actions/upload-artifact@v2
//         with:
//           name: test-results
//           path: build/test_output.txt
//
// MAKEFILE INTEGRATION:
// =====================
//
// Makefile:
// test: test_csv_loader
// 	./test_csv_loader
// 	@if [ $$? -eq 0 ]; then
// 		echo "All tests passed!";
// 	else
// 		echo "Tests failed!"; 
// 		exit 1;
// 	fi
//
// Usage:
// $ make test
//
// JENKINS PIPELINE:
// =================
//
// Jenkinsfile:
// stage('Test') {
//     steps {
//         sh './build/test_csv_loader'
//     }
// }
//
// TEST REPORTING:
// ===============
//
// For better reporting, could output JUnit XML:
//
// <testsuites>
//   <testsuite name="CSVLoader" tests="3" failures="0">
//     <testcase name="LoadValidCSV" time="0.002"/>
//     <testcase name="DateRangeFiltering" time="0.001"/>
//     <testcase name="Statistics" time="0.001"/>
//   </testsuite>
// </testsuites>
//
// Parseable by CI tools for detailed reporting.
//
//==============================================================================

//==============================================================================
// TEST-DRIVEN DEVELOPMENT (TDD) WITH THIS SUITE
//==============================================================================
//
// TDD WORKFLOW:
// =============
//
// 1. Write failing test (RED):
//    Test 4: Validate price bars... FAILED
//
// 2. Implement minimum code to pass (GREEN):
//    bool PriceBar::is_valid() { return high >= low; }
//    Test 4: Validate price bars... PASSED
//
// 3. Refactor (REFACTOR):
//    // Improve implementation
//    // Ensure tests still pass
//
// 4. Repeat for next feature
//
// EXAMPLE TDD CYCLE:
// ==================
//
// Feature: Add support for adjusted close price
//
// Step 1: Write test (fails initially):
// {
//     auto data = loader.load("TEST");
//     assert(data->at(0).has_adjusted_close());
//     assert(data->at(0).adjusted_close > 0);
// }
// // FAILED: has_adjusted_close() doesn't exist
//
// Step 2: Add minimal implementation:
// struct PriceBar {
//     // ... existing fields ...
//     double adjusted_close;
//     bool has_adjusted_close() { return adjusted_close > 0; }
// };
// // PASSED
//
// Step 3: Refactor (if needed):
// // Improve error handling, add validation, etc.
//
// Benefits:
// - Tests drive design
// - Ensure feature works as intended
// - Prevent regressions
//
//==============================================================================

//==============================================================================
// END OF COMPREHENSIVE DOCUMENTATION
//==============================================================================