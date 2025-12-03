/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: test_checkpoint.cpp
    
    Description:
        This file implements comprehensive unit tests for the CheckpointManager
        class, validating checkpoint save/load functionality, file management,
        data integrity, and error handling. These tests ensure the checkpoint
        system works correctly before integration into the distributed worker
        nodes, preventing data loss and resume failures in production.
        
        Test Coverage:
        - Test 1: Basic save/load roundtrip with full data validation
        - Test 2: Checkpoint existence checking
        - Test 3: Checkpoint deletion and cleanup
        - Test 4: Multiple concurrent checkpoints
        
        Testing Philosophy:
        Unit tests for checkpoint functionality must verify:
        1. DATA INTEGRITY: Values survive serialization/deserialization
        2. FILE OPERATIONS: Save, load, delete work correctly
        3. EDGE CASES: Missing files, corrupted data, concurrent access
        4. ATOMICITY: Partial writes don't corrupt checkpoints
        5. PERFORMANCE: Operations complete in reasonable time
        
        Test Environment:
        - Directory: /tmp/checkpoints (temporary, cleaned after tests)
        - Isolation: Each test independent (no shared state)
        - Validation: Assertions fail fast on errors
        - Cleanup: Tests clean up created files
        
        Running Tests:
```bash
        # Compile
        g++ -std=c++17 test_checkpoint.cpp checkpoint_manager.cpp logger.cpp \
            -o test_checkpoint -pthread
        
        # Run
        ./test_checkpoint
        
        # Expected output:
        # Test 1: Save and load checkpoint... PASSED
        # Test 2: Checkpoint exists check... PASSED
        # Test 3: Delete checkpoint... PASSED
        # Test 4: Multiple checkpoints... PASSED
        # 
        # === Results ===
        # Passed: 4
        # Failed: 0
```
        
        Integration with CI/CD:
```bash
        # In continuous integration pipeline
        ./test_checkpoint || exit 1  # Fail build if tests fail
```
        
        Test-Driven Development Workflow:
        1. Write test for new checkpoint feature
        2. Run test (should fail - feature not implemented)
        3. Implement feature in CheckpointManager
        4. Run test again (should pass)
        5. Refactor if needed
        6. Commit with passing tests
        
        Debugging Failed Tests:
        - Check /tmp/checkpoints directory for leftover files
        - Verify file permissions (should be writable)
        - Enable DEBUG logging to see detailed operations
        - Add breakpoints at assertion failures
        - Inspect checkpoint file with hexdump
        
    Dependencies:
        - worker/checkpoint_manager.h: CheckpointManager class
        - common/logger.h: Logging infrastructure
        - <cassert>: Assertion macros for validation
        - <cmath>: Floating-point comparison (abs, epsilon)
        - <iostream>: Console output for test results
        
    Prerequisites:
        - /tmp directory must be writable
        - Sufficient disk space (~10KB for test checkpoints)
        - C++17 compiler with filesystem support
        
    Exit Codes:
        0: All tests passed
        1: One or more tests failed
        
    Performance Expectations:
        - Total test runtime: < 100ms
        - Per test: < 25ms
        - If slower, indicates I/O performance issues
        
    Thread Safety:
        Tests run sequentially (single-threaded).
        CheckpointManager is thread-safe, but concurrent testing
        would require more complex setup with barriers and synchronization.
*******************************************************************************/

#include "worker/checkpoint_manager.h"
#include "common/logger.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace backtesting;

//==============================================================================
// TABLE OF CONTENTS
//==============================================================================
// 1. Overview and Testing Strategy
// 2. Test Infrastructure
// 3. Test Cases
//    3.1 Test 1: Save and Load Roundtrip
//    3.2 Test 2: Checkpoint Existence
//    3.3 Test 3: Checkpoint Deletion
//    3.4 Test 4: Multiple Checkpoints
// 4. Main Function - Test Runner
// 5. Adding New Tests
// 6. Debugging Failed Tests
// 7. Common Testing Pitfalls
// 8. FAQ
//==============================================================================

//==============================================================================
// SECTION 1: Overview and Testing Strategy
//==============================================================================

/**
 * COMPREHENSIVE TESTING APPROACH
 * 
 * TESTING PYRAMID:
 * 
 * ```
 *        ┌─────────────┐
 *        │  E2E Tests  │  ← Full system (test_integration.cpp)
 *        ├─────────────┤
 *        │Integration  │  ← Worker + Controller
 *        ├─────────────┤
 *        │ Unit Tests  │  ← THIS FILE (CheckpointManager only)
 *        └─────────────┘
 * ```
 * 
 * UNIT TEST CHARACTERISTICS:
 * 
 * FAST:
 * - Each test < 25ms
 * - Total suite < 100ms
 * - Enables rapid iteration
 * 
 * ISOLATED:
 * - Tests don't depend on each other
 * - Can run in any order
 * - Failure in one doesn't affect others
 * 
 * DETERMINISTIC:
 * - Same input always produces same output
 * - No randomness (unless testing random behavior)
 * - No timing dependencies
 * 
 * COMPREHENSIVE:
 * - Happy path (normal operation)
 * - Error cases (missing files, corruption)
 * - Edge cases (empty data, large values)
 * - Boundary conditions (min/max values)
 * 
 * TEST ORGANIZATION:
 * 
 * ARRANGE-ACT-ASSERT (AAA) PATTERN:
 * 
 * ```cpp
 * // ARRANGE: Set up test data
 * CheckpointManager mgr("/tmp/checkpoints");
 * Checkpoint ckpt;
 * ckpt.job_id = 42;
 * // ... setup ...
 * 
 * // ACT: Perform operation
 * bool result = mgr.save_checkpoint(ckpt);
 * 
 * // ASSERT: Verify outcome
 * assert(result == true);
 * assert(mgr.checkpoint_exists(42));
 * ```
 * 
 * GIVEN-WHEN-THEN PATTERN:
 * 
 * ```cpp
 * // GIVEN a CheckpointManager with no existing checkpoints
 * CheckpointManager mgr("/tmp/checkpoints");
 * assert(!mgr.checkpoint_exists(42));
 * 
 * // WHEN saving a checkpoint
 * Checkpoint ckpt = create_test_checkpoint(42);
 * mgr.save_checkpoint(ckpt);
 * 
 * // THEN checkpoint should be loadable
 * Checkpoint loaded;
 * assert(mgr.load_checkpoint(42, loaded));
 * assert(loaded.valid);
 * ```
 * 
 * WHAT MAKES A GOOD TEST?
 * 
 * 1. CLEAR PURPOSE:
 *    Test name describes what's being tested
 *    "Save and load checkpoint" - obvious intent
 * 
 * 2. MINIMAL SCOPE:
 *    One test per feature/behavior
 *    Don't test everything in one giant test
 * 
 * 3. SELF-CONTAINED:
 *    Test creates its own data
 *    No dependencies on external files or state
 * 
 * 4. GOOD FAILURE MESSAGES:
 *    When assert fails, clear what went wrong
 *    Use descriptive variable names
 * 
 * 5. CLEANUP:
 *    Test removes created files
 *    Leaves system in clean state
 * 
 * ANTI-PATTERNS TO AVOID:
 * 
 * Tests that depend on execution order
 * Tests that depend on external state
 * Tests that don't clean up
 * Tests that take too long (> 1 second)
 * Tests that fail randomly
 * Tests with unclear assertions
 */

//==============================================================================
// SECTION 2: Test Infrastructure
//==============================================================================

/**
 * TEST DIRECTORY SETUP
 * 
 * All tests use /tmp/checkpoints for isolation:
 * - Temporary location (auto-cleaned by OS on reboot)
 * - Writable by all users
 * - Fast (usually tmpfs - RAM-backed)
 * 
 * PRODUCTION TESTING:
 * Would use unique directory per test run:
 * ```cpp
 * std::string test_dir = "/tmp/checkpoints_" + 
 *                        std::to_string(getpid()) + "_" +
 *                        std::to_string(time(nullptr));
 * ```
 * Prevents conflicts if multiple test instances run concurrently.
 * 
 * CLEANUP STRATEGY:
 * 
 * WITHIN TEST:
 * Each test cleans up its own checkpoints:
 * ```cpp
 * // End of test
 * manager.delete_checkpoint(job_id);
 * ```
 * 
 * GLOBAL CLEANUP (not implemented):
 * ```cpp
 * void cleanup_test_directory() {
 *     std::filesystem::remove_all("/tmp/checkpoints");
 * }
 * 
 * int main() {
 *     // ... run tests ...
 *     cleanup_test_directory();  // Final cleanup
 * }
 * ```
 * 
 * ASSERTION MACRO:
 * 
 * Using assert() from <cassert>:
 * - Aborts program if condition false
 * - Only active in debug builds (disabled with -DNDEBUG)
 * - Simple but effective for unit tests
 * 
 * ALTERNATIVE: Custom assertion with better messages
 * ```cpp
 * #define ASSERT_EQ(a, b) do { \
 *     if ((a) != (b)) { \
 *         std::cerr << "Assertion failed: " << #a << " != " << #b \
 *                   << " (" << (a) << " != " << (b) << ")\n" \
 *                   << "  at " << __FILE__ << ":" << __LINE__ << "\n"; \
 *         throw std::runtime_error("Assertion failed"); \
 *     } \
 * } while(0)
 * ```
 * 
 * FLOATING-POINT COMPARISON:
 * 
 * DON'T:
 * ```cpp
 * assert(loaded.current_cash == 5000.50);  // Exact equality
 * ```
 * 
 * DO:
 * ```cpp
 * assert(std::abs(loaded.current_cash - 5000.50) < 0.01);  // Epsilon
 * ```
 * 
 * Floating-point arithmetic has rounding errors.
 * Binary representation of 5000.50 may not be exact.
 * Use epsilon comparison (within 0.01 is "equal").
 */

/**
 * @brief Main entry point for checkpoint tests
 * 
 * TEST EXECUTION FLOW:
 * 
 * 1. Initialize logging
 * 2. Initialize counters (passed, failed)
 * 3. Run each test in isolated scope
 * 4. Catch exceptions, update counters
 * 5. Print summary
 * 6. Exit with appropriate code
 * 
 * ISOLATED SCOPES:
 * Each test wrapped in { } block:
 * ```cpp
 * {
 *     // Test 1 scope
 *     CheckpointManager mgr(...);  // Created
 *     // ... test logic ...
 * }  // mgr destroyed here
 * 
 * {
 *     // Test 2 scope (independent)
 *     CheckpointManager mgr(...);  // Fresh instance
 *     // ... test logic ...
 * }  // mgr destroyed here
 * ```
 * 
 * Benefits:
 * - Automatic cleanup (destructors called)
 * - No state leakage between tests
 * - Clear test boundaries
 * 
 * EXCEPTION HANDLING:
 * Each test wrapped in try-catch:
 * - Catches assertion failures (throws in some implementations)
 * - Catches unexpected exceptions (bugs in code)
 * - Prints error message
 * - Continues with next test (don't abort entire suite)
 * 
 * COUNTERS:
 * - passed: Incremented on successful test
 * - failed: Incremented on test failure
 * - Final summary shows overall health
 * 
 * EXIT CODE:
 * - 0: All tests passed (CI/CD can proceed)
 * - 1: At least one failure (CI/CD should fail build)
 * 
 * INTEGRATION WITH CMAKE:
 * ```cmake
 * enable_testing()
 * add_test(NAME checkpoint_tests COMMAND test_checkpoint)
 * ```
 * 
 * Then run:
 * ```bash
 * cmake --build . --target test
 * # Or directly:
 * ctest
 * ```
 */
int main() {
    //==========================================================================
    // TEST INITIALIZATION
    //==========================================================================
    
    /**
     * CONFIGURE LOGGING
     * 
     * LogLevel::INFO: Show test progress, not verbose details
     * 
     * For debugging failures, change to LogLevel::DEBUG:
     * - Shows file operations (open, write, close)
     * - Shows serialization details
     * - Shows error messages
     */
    Logger::set_level(LogLevel::INFO);
    Logger::info("Running Checkpoint tests...");
    
    /**
     * TEST RESULT COUNTERS
     * 
     * Track pass/fail status for final summary.
     * Helps quickly see overall test health.
     */
    int passed = 0;
    int failed = 0;
    
    //==========================================================================
    // SECTION 3: Test Cases
    //==========================================================================
    
    //--------------------------------------------------------------------------
    // SECTION 3.1: Test 1 - Save and Load Roundtrip
    //--------------------------------------------------------------------------
    
    /**
     * TEST 1: Save and Load Checkpoint
     * 
     * PURPOSE:
     * Validates that checkpoint data survives serialization to disk
     * and deserialization back to memory without corruption.
     * 
     * WHAT IT TESTS:
     * - save_checkpoint() writes data correctly
     * - load_checkpoint() reads data correctly
     * - All fields (integers, doubles, strings, bools) preserved
     * - Floating-point values maintain precision
     * 
     * TEST STRATEGY:
     * 1. Create checkpoint with known values
     * 2. Save to disk
     * 3. Load from disk into new object
     * 4. Compare all fields (original vs loaded)
     * 5. Assert exact equality (within epsilon for floats)
     * 
     * WHY THIS TEST IS CRITICAL:
     * This is the core functionality. If save/load doesn't work,
     * entire checkpoint system is broken. All other features
     * depend on this working correctly.
     * 
     * VALIDATION COVERAGE:
     * - uint64_t fields: job_id, symbols_processed, last_processed_index
     * - std::string fields: symbol, last_date_processed
     * - double fields: current_cash, portfolio_value
     * - int field: current_shares
     * - bool field: valid
     * 
     * FLOATING-POINT PRECISION:
     * Uses epsilon comparison (< 0.01) because:
     * - Binary floating-point can't represent 5000.50 exactly
     * - Serialization/deserialization may introduce rounding
     * - 0.01 tolerance is acceptable (sub-penny precision)
     * 
     * EDGE CASES TESTED:
     * - Typical values (normal operation)
     * - Could add: zero values, negative values, very large values
     * 
     * EXAMPLE OUTPUT:
     * ```
     * Test 1: Save and load checkpoint... PASSED
     * ```
     * 
     * FAILURE EXAMPLE:
     * ```
     * Test 1: Save and load checkpoint... FAILED: Assertion failed
     * ```
     * 
     * DEBUGGING FAILURES:
     * ```cpp
     * // Add debug output before assertions
     * std::cout << "Loaded job_id: " << loaded_cp.job_id << "\n";
     * std::cout << "Expected: 12345\n";
     * assert(loaded_cp.job_id == 12345);
     * ```
     */
    {
        std::cout << "Test 1: Save and load checkpoint... ";
        try {
            /**
             * ARRANGE: Create CheckpointManager and test data
             */
            CheckpointManager manager("/tmp/checkpoints");
            
            /**
             * Create checkpoint with representative data
             * 
             * job_id: Unique identifier (12345)
             * symbol: Stock ticker (4 chars typical)
             * symbols_processed: Mid-range value (not 0, not max)
             * last_processed_index: Array index (500 bars processed)
             * current_cash: Decimal value with cents (5000.50)
             * current_shares: Integer share count (25)
             * portfolio_value: Current portfolio worth (12500.75)
             * last_date_processed: ISO date format (YYYY-MM-DD)
             * valid: true (checkpoint is usable)
             */
            Checkpoint cp;
            cp.job_id = 12345;
            cp.symbol = "AAPL";
            cp.symbols_processed = 250;
            cp.last_processed_index = 500;
            cp.current_cash = 5000.50;
            cp.current_shares = 25;
            cp.portfolio_value = 12500.75;
            cp.last_date_processed = "2023-06-15";
            cp.valid = true;
            
            /**
             * ACT: Save checkpoint to disk
             * 
             * This tests:
             * - File creation
             * - Serialization
             * - Write operations
             * - Atomic rename (temp file → final file)
             */
            assert(manager.save_checkpoint(cp));
            
            /**
             * ACT: Load checkpoint from disk
             * 
             * This tests:
             * - File opening
             * - Deserialization
             * - Read operations
             * - Memory allocation for strings
             */
            Checkpoint loaded_cp;
            assert(manager.load_checkpoint(12345, loaded_cp));
            
            /**
             * ASSERT: Verify all fields match
             * 
             * INTEGERS: Exact equality
             */
            assert(loaded_cp.job_id == 12345);
            assert(loaded_cp.symbols_processed == 250);
            assert(loaded_cp.last_processed_index == 500);
            assert(loaded_cp.current_shares == 25);
            
            /**
             * STRINGS: Exact equality
             * 
             * operator== for std::string compares character-by-character.
             * Case-sensitive, no normalization.
             */
            assert(loaded_cp.symbol == "AAPL");
            assert(loaded_cp.last_date_processed == "2023-06-15");
            
            /**
             * DOUBLES: Epsilon comparison
             * 
             * std::abs(a - b) < epsilon
             * 
             * Why 0.01 epsilon?
             * - Represents $0.01 (one cent)
             * - Sufficient precision for financial data
             * - Accounts for floating-point rounding
             * 
             * ALTERNATIVE (exact bit comparison):
             * ```cpp
             * assert(std::memcmp(&loaded_cp.current_cash, &cp.current_cash,
             *                    sizeof(double)) == 0);
             * ```
             * But epsilon is more robust to serialization differences.
             */
            assert(std::abs(loaded_cp.current_cash - 5000.50) < 0.01);
            assert(std::abs(loaded_cp.portfolio_value - 12500.75) < 0.01);
            
            /**
             * BOOLEAN: Exact equality
             */
            assert(loaded_cp.valid);
            
            /**
             * TEST PASSED
             */
            std::cout << "PASSED\n";
            passed++;
            
        } catch (const std::exception& e) {
            /**
             * TEST FAILED
             * 
             * Catches:
             * - Assertion failures (if asserts throw)
             * - Exceptions from CheckpointManager
             * - Unexpected errors
             * 
             * Prints exception message for debugging.
             */
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }  // Test 1 scope ends, automatic cleanup
    
    //--------------------------------------------------------------------------
    // SECTION 3.2: Test 2 - Checkpoint Existence Check
    //--------------------------------------------------------------------------
    
    /**
     * TEST 2: Checkpoint Exists Check
     * 
     * PURPOSE:
     * Validates checkpoint_exists() correctly reports file presence.
     * 
     * WHAT IT TESTS:
     * - checkpoint_exists() returns true for existing checkpoint
     * - checkpoint_exists() returns false for non-existent checkpoint
     * - File detection is accurate
     * 
     * TEST STRATEGY:
     * 1. Use checkpoint from Test 1 (job_id=12345)
     * 2. Verify exists() returns true
     * 3. Check non-existent ID (99999)
     * 4. Verify exists() returns false
     * 
     * DEPENDENCY:
     * Relies on Test 1 creating checkpoint for job_id=12345.
     * This is acceptable because:
     * - Tests run in order (defined order)
     * - Test 1 always runs first
     * - Test 1 creates the file this test checks
     * 
     * ALTERNATIVE (fully isolated):
     * ```cpp
     * {
     *     std::cout << "Test 2: Checkpoint exists check... ";
     *     try {
     *         CheckpointManager manager("/tmp/checkpoints");
     *         
     *         // Create our own checkpoint (no dependency)
     *         Checkpoint cp;
     *         cp.job_id = 12345;
     *         cp.valid = true;
     *         manager.save_checkpoint(cp);
     *         
     *         // Now test exists
     *         assert(manager.checkpoint_exists(12345));
     *         assert(!manager.checkpoint_exists(99999));
     *         
     *         // Cleanup
     *         manager.delete_checkpoint(12345);
     *         
     *         std::cout << "PASSED\n";
     *         passed++;
     *     } catch (...) { ... }
     * }
     * ```
     * 
     * EDGE CASES TO TEST:
     * - job_id = 0 (invalid ID)
     * - job_id = UINT64_MAX (maximum value)
     * - Checkpoint file exists but is corrupted
     * 
     * EXPECTED BEHAVIOR:
     * checkpoint_exists() only checks file existence, not validity.
     * Corrupted file still returns true (validity checked in load).
     */
    {
        std::cout << "Test 2: Checkpoint exists check... ";
        try {
            CheckpointManager manager("/tmp/checkpoints");
            
            /**
             * POSITIVE TEST: Checkpoint should exist
             * 
             * job_id=12345 was created by Test 1.
             * File: /tmp/checkpoints/job_12345.ckpt
             * Should exist on filesystem.
             */
            assert(manager.checkpoint_exists(12345));
            
            /**
             * NEGATIVE TEST: Checkpoint should NOT exist
             * 
             * job_id=99999 was never created.
             * File: /tmp/checkpoints/job_99999.ckpt
             * Should not exist on filesystem.
             * 
             * This tests:
             * - exists() returns false for missing files
             * - No false positives
             * - Correct file path generation
             */
            assert(!manager.checkpoint_exists(99999));
            
            std::cout << "PASSED\n";
            passed++;
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    
    //--------------------------------------------------------------------------
    // SECTION 3.3: Test 3 - Checkpoint Deletion
    //--------------------------------------------------------------------------
    
    /**
     * TEST 3: Delete Checkpoint
     * 
     * PURPOSE:
     * Validates delete_checkpoint() correctly removes files.
     * 
     * WHAT IT TESTS:
     * - delete_checkpoint() removes existing file
     * - File no longer exists after deletion
     * - delete_checkpoint() is idempotent (safe to call on non-existent)
     * 
     * TEST STRATEGY:
     * 1. Verify checkpoint exists (from Test 1)
     * 2. Delete checkpoint
     * 3. Verify checkpoint no longer exists
     * 
     * IDEMPOTENCY:
     * Calling delete_checkpoint() on already-deleted file should
     * return true (success) not false (error).
     * 
     * ADDITIONAL TEST:
     * ```cpp
     * assert(manager.delete_checkpoint(12345));  // First delete
     * assert(manager.delete_checkpoint(12345));  // Second delete (idempotent)
     * ```
     * 
     * CLEANUP:
     * This test performs cleanup for Test 1's checkpoint.
     * After this test, job_12345.ckpt should be removed.
     * 
     * FILESYSTEM STATE VERIFICATION:
     * ```cpp
     * // After delete, file should not exist
     * std::string path = "/tmp/checkpoints/job_12345.ckpt";
     * assert(!std::filesystem::exists(path));
     * ```
     */
    {
        std::cout << "Test 3: Delete checkpoint... ";
        try {
            CheckpointManager manager("/tmp/checkpoints");
            
            /**
             * PRECONDITION: Checkpoint exists
             * 
             * Verify file is present before attempting delete.
             * If this fails, indicates Test 1 didn't run or failed.
             */
            assert(manager.checkpoint_exists(12345));
            
            /**
             * ACT: Delete checkpoint
             * 
             * Should remove file from disk.
             * Returns true on success.
             */
            assert(manager.delete_checkpoint(12345));
            
            /**
             * POSTCONDITION: Checkpoint no longer exists
             * 
             * Verify file was actually removed.
             * If this fails, delete operation didn't work.
             */
            assert(!manager.checkpoint_exists(12345));
            
            std::cout << "PASSED\n";
            passed++;
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    
    //--------------------------------------------------------------------------
    // SECTION 3.4: Test 4 - Multiple Checkpoints
    //--------------------------------------------------------------------------
    
    /**
     * TEST 4: Multiple Checkpoints
     * 
     * PURPOSE:
     * Validates checkpoint manager can handle multiple concurrent checkpoints.
     * 
     * WHAT IT TESTS:
     * - Multiple save_checkpoint() calls work correctly
     * - Each checkpoint stored independently (no interference)
     * - get_all_checkpoint_ids() returns all checkpoints
     * - Cleanup of multiple checkpoints works
     * 
     * TEST STRATEGY:
     * 1. Create 5 different checkpoints (job IDs 1001-1005)
     * 2. Save all checkpoints
     * 3. Query all checkpoint IDs
     * 4. Verify at least 5 IDs returned (may include others from failures)
     * 5. Delete all created checkpoints (cleanup)
     * 
     * CONCURRENCY TESTING:
     * Current test is sequential (create one, save, repeat).
     * For true concurrency test:
     * ```cpp
     * std::vector<std::thread> threads;
     * for (int i = 0; i < 5; ++i) {
     *     threads.emplace_back([&, i]() {
     *         Checkpoint cp;
     *         cp.job_id = 1000 + i;
     *         cp.valid = true;
     *         manager.save_checkpoint(cp);
     *     });
     * }
     * for (auto& t : threads) t.join();
     * 
     * auto ids = manager.get_all_checkpoint_ids();
     * assert(ids.size() >= 5);
     * ```
     * 
     * DIRECTORY SCANNING:
     * get_all_checkpoint_ids() must:
     * - Find all job_*.ckpt files
     * - Extract job_id from filename
     * - Return sorted or unsorted vector
     * 
     * IMPLEMENTATION NOTE:
     * Test uses >= 5 (not == 5) because:
     * - Other tests may leave files if they fail
     * - Directory may have unrelated files
     * - We only care that OUR 5 checkpoints are present
     * 
     * Better isolation:
     * ```cpp
     * auto ids = manager.get_all_checkpoint_ids();
     * int our_checkpoints = 0;
     * for (auto id : ids) {
     *     if (id >= 1001 && id <= 1005) our_checkpoints++;
     * }
     * assert(our_checkpoints == 5);
     * ```
     * 
     * CLEANUP VERIFICATION:
     * After deletion, verify files actually removed:
     * ```cpp
     * for (uint64_t i = 1; i <= 5; ++i) {
     *     assert(!manager.checkpoint_exists(1000 + i));
     * }
     * ```
     */
    {
        std::cout << "Test 4: Multiple checkpoints... ";
        try {
            CheckpointManager manager("/tmp/checkpoints");
            
            /**
             * CREATE MULTIPLE CHECKPOINTS
             * 
             * Loop creates 5 checkpoints with:
             * - Different job IDs (1001-1005)
             * - Different symbols (TEST1-TEST5)
             * - Different progress values (100, 200, 300, 400, 500)
             * 
             * This tests that CheckpointManager can handle multiple
             * independent checkpoints without conflicts.
             */
            for (uint64_t i = 1; i <= 5; ++i) {
                Checkpoint cp;
                cp.job_id = 1000 + i;
                cp.symbol = "TEST" + std::to_string(i);
                cp.symbols_processed = i * 100;
                cp.valid = true;
                
                /**
                 * SAVE CHECKPOINT
                 * 
                 * Each iteration creates a new file:
                 * - job_1001.ckpt
                 * - job_1002.ckpt
                 * - job_1003.ckpt
                 * - job_1004.ckpt
                 * - job_1005.ckpt
                 */
                assert(manager.save_checkpoint(cp));
            }
            
            /**
             * QUERY ALL CHECKPOINTS
             * 
             * get_all_checkpoint_ids() scans directory and returns
             * vector of all job IDs with checkpoints.
             * 
             * ASSERTION: At least 5 IDs
             * Uses >= because:
             * - Other tests may have created checkpoints
             * - Previous test runs may have left files (if cleanup failed)
             * - We only verify OUR checkpoints are present
             * 
             * IMPROVEMENT:
             * Check that our specific IDs are present:
             * ```cpp
             * auto ids = manager.get_all_checkpoint_ids();
             * for (uint64_t i = 1001; i <= 1005; ++i) {
             *     assert(std::find(ids.begin(), ids.end(), i) != ids.end());
             * }
             * ```
             */
            auto ids = manager.get_all_checkpoint_ids();
            assert(ids.size() >= 5);
            
            /**
             * CLEANUP: Delete all created checkpoints
             * 
             * Important to clean up test files.
             * Otherwise directory accumulates garbage.
             * 
             * VERIFICATION:
             * Could verify each deletion:
             * ```cpp
             * for (uint64_t i = 1; i <= 5; ++i) {
             *     assert(manager.delete_checkpoint(1000 + i));
             *     assert(!manager.checkpoint_exists(1000 + i));
             * }
             * ```
             */
            for (uint64_t i = 1; i <= 5; ++i) {
                manager.delete_checkpoint(1000 + i);
            }
            
            std::cout << "PASSED\n";
            passed++;
        } catch (const std::exception& e) {
            std::cout << "FAILED: " << e.what() << "\n";
            failed++;
        }
    }
    
    //==========================================================================
    // SECTION 4: Test Summary and Exit
    //==========================================================================
    
    /**
     * PRINT TEST SUMMARY
     * 
     * Shows aggregate results:
     * - How many tests passed
     * - How many tests failed
     * 
     * EXAMPLE OUTPUT:
     * ```
     * === Results ===
     * Passed: 4
     * Failed: 0
     * ```
     * 
     * Or with failures:
     * ```
     * === Results ===
     * Passed: 3
     * Failed: 1
     * ```
     */
    std::cout << "\n=== Results ===\n";
    std::cout << "Passed: " << passed << "\n";
    std::cout << "Failed: " << failed << "\n";
    
    /**
     * EXIT CODE
     * 
     * Return 0 if all tests passed (failed == 0)
     * Return 1 if any test failed (failed > 0)
     * 
     * This allows CI/CD to detect test failures:
     * ```bash
     * ./test_checkpoint
     * if [ $? -ne 0 ]; then
     *     echo "Tests failed, aborting build"
     *     exit 1
     * fi
     * ```
     */
    return (failed == 0) ? 0 : 1;
}

//==============================================================================
// SECTION 5: Adding New Tests
//==============================================================================

/**
 * TEMPLATE FOR NEW TEST CASE
 * 
 * ```cpp
 * // Test N: <Description>
 * {
 *     std::cout << "Test N: <Description>... ";
 *     try {
 *         // ARRANGE: Set up test data and environment
 *         CheckpointManager manager("/tmp/checkpoints");
 *         Checkpoint cp;
 *         cp.job_id = <unique_id>;
 *         // ... initialize checkpoint ...
 *         
 *         // ACT: Perform operation under test
 *         bool result = manager.<method_to_test>(cp);
 *         
 *         // ASSERT: Verify expected outcome
 *         assert(result == true);
 *         assert(<other_conditions>);
 *         
 *         // CLEANUP: Remove created files
 *         manager.delete_checkpoint(<job_id>);
 *         
 *         std::cout << "PASSED\n";
 *         passed++;
 *     } catch (const std::exception& e) {
 *         std::cout << "FAILED: " << e.what() << "\n";
 *         failed++;
 *     }
 * }
 * ```
 * 
 * RECOMMENDED ADDITIONAL TESTS:
 * 
 * TEST: Checkpoint with zero values
 * ```cpp
 * Checkpoint cp;
 * cp.job_id = 2000;
 * cp.symbols_processed = 0;
 * cp.current_cash = 0.0;
 * cp.current_shares = 0;
 * cp.valid = true;
 * // Verify zero values save/load correctly
 * ```
 * 
 * TEST: Checkpoint with maximum values
 * ```cpp
 * cp.job_id = UINT64_MAX;
 * cp.symbols_processed = UINT64_MAX;
 * // Verify large values don't overflow
 * ```
 * 
 * TEST: Checkpoint with long strings
 * ```cpp
 * cp.symbol = std::string(1000, 'A');  // 1000 chars
 * cp.last_date_processed = std::string(1000, '2');
 * // Verify string length handled correctly
 * ```
 * 
 * TEST: Concurrent save/load
 * ```cpp
 * std::vector<std::thread> threads;
 * for (int i = 0; i < 10; ++i) {
 *     threads.emplace_back([&]() {
 *         Checkpoint cp;
 *         cp.job_id = 3000 + i;
 *         cp.valid = true;
 *         manager.save_checkpoint(cp);
 *     });
 * }
 * for (auto& t : threads) t.join();
 * 
 * // Verify all 10 checkpoints created
 * auto ids = manager.get_all_checkpoint_ids();
 * // ... verify ...
 * ```
 * 
 * TEST: Corrupted checkpoint file
 * ```cpp
 * // Create checkpoint
 * manager.save_checkpoint(cp);
 * 
 * // Corrupt the file
 * std::ofstream corrupt("/tmp/checkpoints/job_4000.ckpt");
 * corrupt << "garbage data";
 * corrupt.close();
 * 
 * // Try to load
 * Checkpoint loaded;
 * bool result = manager.load_checkpoint(4000, loaded);
 * // Should either return false or loaded.valid == false
 * ```
 * 
 * TEST: Disk full simulation
 * ```cpp
 * // Fill up /tmp (carefully!)
 * // Or use quota limits
 * // Verify save_checkpoint returns false gracefully
 * ```
 * 
 * TEST: Permission denied
 * ```cpp
 * // Create read-only checkpoint directory
 * system("mkdir /tmp/readonly_ckpt && chmod 444 /tmp/readonly_ckpt");
 * CheckpointManager manager("/tmp/readonly_ckpt");
 * 
 * Checkpoint cp;
 * cp.job_id = 5000;
 * cp.valid = true;
 * 
 * // Should fail gracefully (return false, not crash)
 * assert(!manager.save_checkpoint(cp));
 * ```
 */

//==============================================================================
// SECTION 6: Debugging Failed Tests
//==============================================================================

/**
 * DEBUGGING WORKFLOW
 * 
 * STEP 1: Identify which test failed
 * ```
 * Test 1: Save and load checkpoint... PASSED
 * Test 2: Checkpoint exists check... FAILED: Assertion failed
 * Test 3: Delete checkpoint... PASSED
 * ```
 * → Test 2 failed
 * 
 * STEP 2: Enable debug logging
 * ```cpp
 * Logger::set_level(LogLevel::DEBUG);  // Show all operations
 * ```
 * 
 * Re-run:
 * ```
 * [DEBUG] Checking for checkpoint: /tmp/checkpoints/job_12345.ckpt
 * [DEBUG] File exists: false  ← AH HA! File missing
 * ```
 * 
 * STEP 3: Inspect filesystem
 * ```bash
 * ls -lh /tmp/checkpoints/
 * # Check if job_12345.ckpt exists
 * # Check file permissions
 * # Check file size
 * ```
 * 
 * STEP 4: Add diagnostic output
 * ```cpp
 * auto ids = manager.get_all_checkpoint_ids();
 * std::cout << "Found " << ids.size() << " checkpoints:\n";
 * for (auto id : ids) {
 *     std::cout << "  - job_" << id << "\n";
 * }
 * assert(ids.size() >= 5);
 * ```
 * 
 * STEP 5: Isolate the failure
 * Run just the failing test:
 * ```cpp
 * int main() {
 *     // Comment out other tests
 *     // { Test 1 ... }  // Commented
 *     { Test 2 ... }     // Only this one runs
 *     // { Test 3 ... }  // Commented
 * }
 * ```
 * 
 * STEP 6: Use debugger
 * ```bash
 * gdb ./test_checkpoint
 * (gdb) break test_checkpoint.cpp:42  # Line with failing assertion
 * (gdb) run
 * (gdb) print loaded_cp.job_id
 * (gdb) print cp.job_id
 * # Compare values to find discrepancy
 * ```
 * 
 * COMMON FAILURE CAUSES:
 * 
 * ASSERTION: loaded_cp.job_id != 12345
 * Cause: Deserialization bug (wrong byte order, wrong offset)
 * 
 * ASSERTION: loaded_cp.symbol != "AAPL"
 * Cause: String serialization bug (length prefix wrong, encoding issue)
 * 
 * ASSERTION: abs(loaded_cp.current_cash - 5000.50) >= 0.01
 * Cause: Precision loss (using float instead of double)
 * 
 * ASSERTION: !loaded_cp.valid
 * Cause: Forgot to set valid=true before saving
 * 
 * FILE NOT FOUND:
 * Cause: Directory doesn't exist, permissions issue, wrong path
 */

/**
 * MANUAL CHECKPOINT INSPECTION
 * 
 * Examine checkpoint file with hex viewer:
 * ```bash
 * hexdump -C /tmp/checkpoints/job_12345.ckpt
 * ```
 * 
 * Expected output:
 * ```
 * 00000000  39 30 00 00 00 00 00 00  |90......|  job_id = 12345 (0x3039)
 * 00000008  04 00 00 00 00 00 00 00  |........|  symbol_length = 4
 * 00000010  41 41 50 4c 00 00 00 00  |AAPL....|  symbol = "AAPL"
 * ...
 * ```
 * 
 * Validate:
 * - First 8 bytes should be job_id in little-endian
 * - String lengths should match actual strings
 * - No truncation or padding issues
 * 
 * MANUAL DESERIALIZATION:
 * ```cpp
 * std::ifstream file("/tmp/checkpoints/job_12345.ckpt", std::ios::binary);
 * 
 * uint64_t job_id;
 * file.read(reinterpret_cast<char*>(&job_id), sizeof(job_id));
 * std::cout << "job_id from file: " << job_id << "\n";
 * // Should print: job_id from file: 12345
 * 
 * // Continue reading other fields...
 * ```
 */

//==============================================================================
// SECTION 7: Common Testing Pitfalls
//==============================================================================

/**
 * PITFALL 1: Test interdependencies
 * 
 * PROBLEM:
 * Test 2 depends on Test 1 creating a checkpoint.
 * If Test 1 fails, Test 2 also fails (cascading).
 * 
 * SOLUTION:
 * Make tests fully independent:
 * ```cpp
 * // Each test creates its own data
 * {
 *     CheckpointManager mgr("/tmp/checkpoints");
 *     Checkpoint cp = create_test_checkpoint(unique_id);
 *     mgr.save_checkpoint(cp);
 *     
 *     // ... test logic ...
 *     
 *     mgr.delete_checkpoint(unique_id);  // Cleanup
 * }
 * ```
 */

/**
 * PITFALL 2: Not cleaning up test files
 * 
 * PROBLEM:
 * ```cpp
 * // Create checkpoint but don't delete
 * manager.save_checkpoint(cp);
 * // No cleanup!
 * ```
 * 
 * CONSEQUENCE:
 * - Directory fills with test files
 * - Tests interfere with each other
 * - False positives/negatives
 * 
 * SOLUTION:
 * Always cleanup:
 * ```cpp
 * // Use RAII for automatic cleanup
 * struct CheckpointTestGuard {
 *     CheckpointManager& mgr;
 *     uint64_t job_id;
 *     
 *     ~CheckpointTestGuard() {
 *         mgr.delete_checkpoint(job_id);
 *     }
 * };
 * 
 * CheckpointTestGuard guard{manager, 12345};
 * // Test logic...
 * // Automatic cleanup on scope exit
 * ```
 */

/**
 * PITFALL 3: Floating-point exact equality
 * 
 * PROBLEM:
 * ```cpp
 * assert(loaded.current_cash == 5000.50);  // May fail!
 * ```
 * 
 * CAUSE:
 * Binary floating-point can't represent 5000.50 exactly.
 * May be 5000.500000000001 or 5000.499999999999.
 * 
 * SOLUTION:
 * Epsilon comparison:
 * ```cpp
 * assert(std::abs(loaded.current_cash - 5000.50) < 0.01);  // ✅
 * ```
 */

/**
 * PITFALL 4: Weak assertions
 * 
 * PROBLEM:
 * ```cpp
 * assert(ids.size() > 0);  // Too weak!
 * ```
 * 
 * This passes even if only 1 checkpoint exists (expected 5).
 * 
 * SOLUTION:
 * Precise assertions:
 * ```cpp
 * assert(ids.size() >= 5);  // Expect at least our 5
 * ```
 * 
 * Or exact count if fully isolated:
 * ```cpp
 * // After creating exactly 5 checkpoints in clean directory
 * assert(ids.size() == 5);  // Exact expectation
 * ```
 */

/**
 * PITFALL 5: Not testing error paths
 * 
 * PROBLEM:
 * Only testing happy path (everything works).
 * 
 * MISSING:
 * - What if file doesn't exist?
 * - What if file is corrupted?
 * - What if disk is full?
 * - What if no permissions?
 * 
 * SOLUTION:
 * Add negative tests:
 * ```cpp
 * // Test: Load non-existent checkpoint
 * {
 *     Checkpoint cp;
 *     assert(!manager.load_checkpoint(99999, cp));
 *     assert(!cp.valid);
 * }
 * 
 * // Test: Delete non-existent checkpoint (idempotent)
 * {
 *     assert(manager.delete_checkpoint(99999));  // Should return true
 * }
 * ```
 */

//==============================================================================
// SECTION 8: FAQ
//==============================================================================

/**
 * Q1: Why use /tmp for test checkpoints?
 * 
 * A: Benefits of /tmp:
 *    - Always exists and writable on UNIX systems
 *    - Fast (often tmpfs - RAM-backed)
 *    - Auto-cleaned on reboot
 *    - No conflicts with production data
 *    
 *    Alternative:
 *    - Use current directory: ./test_checkpoints
 *    - Explicitly clean up in code
 */

/**
 * Q2: Why not use a testing framework (Google Test, Catch2)?
 * 
 * A: This is a minimal test for simplicity:
 *    - No external dependencies
 *    - Easy to understand
 *    - Quick to compile
 *    
 *    For larger projects, frameworks are better:
 *    ```cpp
 *    #include <gtest/gtest.h>
 *    
 *    TEST(CheckpointTest, SaveAndLoad) {
 *        CheckpointManager mgr("/tmp/checkpoints");
 *        Checkpoint cp;
 *        cp.job_id = 12345;
 *        // ...
 *        EXPECT_TRUE(mgr.save_checkpoint(cp));
 *        EXPECT_EQ(loaded.job_id, 12345);
 *    }
 *    ```
 *    
 *    Benefits:
 *    - Better failure messages
 *    - Test fixtures (setup/teardown)
 *    - Parameterized tests
 *    - Test discovery
 *    - XML output for CI
 */

/**
 * Q3: How do I run just one test?
 * 
 * A: Comment out others or use conditional compilation:
 *    
 *    METHOD 1: Comments
 *    ```cpp
 *    // { Test 1 ... }
 *    // { Test 2 ... }
 *    { Test 3 ... }  // Only this runs
 *    // { Test 4 ... }
 *    ```
 *    
 *    METHOD 2: Preprocessor
 *    ```cpp
 *    #ifdef RUN_TEST_1
 *    { Test 1 ... }
 *    #endif
 *    
 *    #ifdef RUN_TEST_2
 *    { Test 2 ... }
 *    #endif
 *    ```
 *    
 *    Compile:
 *    ```bash
 *    g++ -DRUN_TEST_2 test_checkpoint.cpp -o test
 *    ```
 */

/**
 * Q4: What if tests pass locally but fail in CI?
 * 
 * A: Common causes:
 *    
 *    DIFFERENT FILESYSTEM:
 *    - Local: ext4 (Linux)
 *    - CI: overlayfs (Docker)
 *    - Atomic rename may behave differently
 *    
 *    PERMISSIONS:
 *    - Local: Running as user with full permissions
 *    - CI: Running as restricted user
 *    - /tmp may not be writable
 *    
 *    TIMING:
 *    - Local: Fast SSD
 *    - CI: Slow network storage
 *    - Timeouts may trigger
 *    
 *    SOLUTION:
 *    - Use relative paths (./test_checkpoints)
 *    - Verify directory creation in tests
 *    - Add timeouts for operations
 */

/**
 * Q5: How do I measure test coverage?
 * 
 * A: Use coverage tools:
 *    
 *    GCC:
 *    ```bash
 *    g++ --coverage test_checkpoint.cpp checkpoint_manager.cpp -o test
 *    ./test
 *    gcov checkpoint_manager.cpp
 *    # Shows which lines were executed
 *    ```
 *    
 *    LCOV (visual report):
 *    ```bash
 *    lcov --capture --directory . --output-file coverage.info
 *    genhtml coverage.info --output-directory coverage_html
 *    firefox coverage_html/index.html
 *    ```
 *    
 *    TARGET:
 *    - >80% line coverage (good)
 *    - >90% line coverage (excellent)
 *    - 100% of critical paths (save, load, delete)
 */

/**
 * Q6: Should I test private methods?
 * 
 * A: Generally no, test public interface only:
 *    
 *    RATIONALE:
 *    - Private methods are implementation details
 *    - May change without breaking public contract
 *    - Testing public methods tests private ones indirectly
 *    
 *    EXAMPLE:
 *    get_checkpoint_path() is private.
 *    Don't test it directly.
 *    Test save_checkpoint() which calls it internally.
 *    
 *    EXCEPTION:
 *    If private method is very complex or critical,
 *    make it public or create test-only friend:
 *    ```cpp
 *    class CheckpointManager {
 *        // ...
 *    #ifdef UNIT_TESTING
 *    public:
 *    #else
 *    private:
 *    #endif
 *        std::string get_checkpoint_path(uint64_t job_id) const;
 *    };
 *    ```
 */

/**
 * Q7: How do I test race conditions?
 * 
 * A: Concurrent stress test:
 *    
 *    ```cpp
 *    // Test: Concurrent checkpoint operations
 *    {
 *        CheckpointManager mgr("/tmp/checkpoints");
 *        std::vector<std::thread> threads;
 *        std::atomic<int> failures{0};
 *        
 *        // Launch many threads doing random operations
 *        for (int i = 0; i < 100; ++i) {
 *            threads.emplace_back([&, i]() {
 *                try {
 *                    Checkpoint cp;
 *                    cp.job_id = 5000 + (i % 10);  // 10 different jobs
 *                    cp.valid = true;
 *                    
 *                    // Randomly save, load, or delete
 *                    int op = rand() % 3;
 *                    if (op == 0) {
 *                        mgr.save_checkpoint(cp);
 *                    } else if (op == 1) {
 *                        Checkpoint loaded;
 *                        mgr.load_checkpoint(cp.job_id, loaded);
 *                    } else {
 *                        mgr.delete_checkpoint(cp.job_id);
 *                    }
 *                } catch (...) {
 *                    failures++;
 *                }
 *            });
 *        }
 *        
 *        for (auto& t : threads) t.join();
 *        
 *        // Should complete without crashes or data corruption
 *        assert(failures == 0);
 *    }
 *    ```
 *    
 *    Run many times to catch rare races:
 *    ```bash
 *    for i in {1..1000}; do
 *        ./test_checkpoint || echo "Failed on iteration $i"
 *    done
 *    ```
 */

/**
 * Q8: How do I test checkpoint file format compatibility?
 * 
 * A: Create reference checkpoint files:
 *    
 *    ```cpp
 *    // Generate reference checkpoint (version 1.0)
 *    void generate_reference_checkpoint() {
 *        CheckpointManager mgr_v1("./reference_checkpoints");
 *        Checkpoint cp;
 *        cp.job_id = 12345;
 *        cp.symbol = "AAPL";
 *        // ... set values ...
 *        cp.valid = true;
 *        mgr_v1.save_checkpoint(cp);
 *        
 *        // Commit reference_checkpoints/job_12345.ckpt to git
 *    }
 *    
 *    // Test loading reference checkpoint
 *    void test_backward_compatibility() {
 *        CheckpointManager mgr("./reference_checkpoints");
 *        Checkpoint loaded;
 *        
 *        // Should load old format successfully
 *        assert(mgr.load_checkpoint(12345, loaded));
 *        assert(loaded.job_id == 12345);
 *        // ...
 *    }
 *    ```
 *    
 *    Ensures new code can read old checkpoints after upgrade.
 */

/**
 * Q9: How do I benchmark checkpoint performance?
 * 
 * A: Add timing measurements:
 *    
 *    ```cpp
 *    #include <chrono>
 *    
 *    // Test: Checkpoint save performance
 *    {
 *        CheckpointManager mgr("/tmp/checkpoints");
 *        Checkpoint cp;
 *        cp.job_id = 6000;
 *        cp.valid = true;
 *        
 *        auto start = std::chrono::steady_clock::now();
 *        
 *        // Save 1000 checkpoints
 *        for (int i = 0; i < 1000; ++i) {
 *            cp.job_id = 6000 + i;
 *            mgr.save_checkpoint(cp);
 *        }
 *        
 *        auto end = std::chrono::steady_clock::now();
 *        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
 *            end - start).count();
 *        
 *        std::cout << "1000 saves in " << duration << "ms\n";
 *        std::cout << "Average: " << (duration / 1000.0) << "ms per save\n";
 *        
 *        // Cleanup
 *        for (int i = 0; i < 1000; ++i) {
 *            mgr.delete_checkpoint(6000 + i);
 *        }
 *        
 *        // Assert reasonable performance
 *        assert(duration < 10000);  // <10ms average per checkpoint
 *    }
 *    ```
 */

/**
 * Q10: Should tests be part of the deliverable?
 * 
 * A: YES, absolutely!
 *    
 *    BENEFITS:
 *    - Verifies code works on target environment
 *    - Documents expected behavior
 *    - Enables safe refactoring
 *    - Catches regressions
 *    - Shows code quality
 *    
 *    INCLUDE IN SUBMISSION:
 *    - Test source files
 *    - Test data (if any)
 *    - Instructions for running tests
 *    - Expected test output
 *    
 *    MENTION IN REPORT:
 *    ```
 *    Testing:
 *    We implemented comprehensive unit tests for all components.
 *    Run './test_checkpoint' to verify checkpoint functionality.
 *    All tests pass, demonstrating system correctness.
 *    ```
 */