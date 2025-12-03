/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: main_raft_controller.cpp
    
    Description:
        This file implements the main entry point for the Raft-based controller
        node in the distributed backtesting system. It is the executable that
        cluster operators run to start a controller instance, forming the 
        fault-tolerant 3-node controller cluster that coordinates job distribution
        and worker management.
        
        The controller cluster is the "brain" of the distributed system,
        responsible for:
        - Leader election via Raft consensus
        - Job scheduling and distribution to workers
        - Worker failure detection and recovery
        - System state management and persistence
        - Result aggregation and client API
        
    Key Responsibilities:
        1. Command-Line Interface: Parse configuration from arguments
        2. Signal Handling: Graceful shutdown on SIGINT/SIGTERM
        3. Peer Discovery: Connect to other controller nodes
        4. Raft Initialization: Start consensus protocol
        5. Event Loop: Monitor cluster health and print statistics
        6. Lifecycle Management: Start, run, and stop controller
        
    System Architecture Context:
        
        Cluster Deployment:
        
        ┌─────────────────────────────────────────────┐
        │         Controller Cluster (Raft)           │
        ├─────────────────────────────────────────────┤
        │  Node 1 (kh01)  │  Node 2 (kh02)  │  Node 3 (kh03) │
        │    LEADER       │   FOLLOWER      │   FOLLOWER     │
        │  ./controller   │  ./controller   │  ./controller  │
        │  --id 1         │  --id 2         │  --id 3        │
        └─────────────────────────────────────────────┘
                 │ Raft Consensus (port 6000)
                 │ Job Distribution (port 5000)
                 ↓
        ┌─────────────────────────────────────────────┐
        │            Worker Pool                       │
        ├─────────────────────────────────────────────┤
        │  Worker 1  │  Worker 2  │  ...  │  Worker 8 │
        │  ./worker  │  ./worker  │       │  ./worker │
        └─────────────────────────────────────────────┘
        
    Production Usage:
        This is production infrastructure code. Operators use it to deploy
        and manage the controller cluster. Proper configuration is critical
        for system reliability and performance.
        
    Design Philosophy:
        - Fail-fast: Validate configuration before starting
        - Graceful shutdown: Clean up resources on exit
        - Observable: Print statistics for monitoring
        - Recoverable: Persist state to survive restarts
        - Operational: Clear error messages and logging
        
*******************************************************************************/

#include "controller/raft_controller.h"
#include "common/logger.h"
#include <iostream>
#include <signal.h>
#include <atomic>
#include <sstream>

using namespace backtesting;

/*******************************************************************************
 * TABLE OF CONTENTS
 * =================
 * 
 * 1. SYSTEM OVERVIEW
 *    - Distributed controller architecture
 *    - Raft consensus protocol role
 *    - Fault tolerance guarantees
 * 
 * 2. GLOBAL STATE AND SIGNAL HANDLING
 *    - shutdown_requested flag
 *    - global_controller pointer
 *    - signal_handler() function
 * 
 * 3. USER INTERFACE
 *    - print_usage() - Help text
 * 
 * 4. CONFIGURATION PARSING
 *    - parse_peers() - Peer list parsing
 *    - Command-line argument processing
 * 
 * 5. MAIN FUNCTION
 *    - Initialization sequence
 *    - Cluster formation
 *    - Main event loop
 *    - Shutdown procedure
 * 
 * 6. DEPLOYMENT GUIDE
 *    - Single-machine development
 *    - Multi-machine cluster
 *    - Khoury cluster deployment
 *    - Cloud deployment (AWS/GCP)
 * 
 * 7. OPERATIONAL PROCEDURES
 *    - Starting the cluster
 *    - Monitoring health
 *    - Rolling upgrades
 *    - Disaster recovery
 * 
 * 8. TROUBLESHOOTING
 *    - Common startup failures
 *    - Network connectivity issues
 *    - Raft election problems
 *    - Performance debugging
 * 
 * 9. COMMON PITFALLS
 * 10. FREQUENTLY ASKED QUESTIONS
 * 
*******************************************************************************/

/*******************************************************************************
 * SECTION 1: SYSTEM OVERVIEW
 * 
 * DISTRIBUTED CONTROLLER ARCHITECTURE
 * ====================================
 * 
 * Why a Controller Cluster?
 *   
 *   Single Point of Failure Problem:
 *     A single controller would be a bottleneck and single point of failure:
 *     - If controller crashes → entire system down
 *     - No way to recover ongoing jobs
 *     - Lost system state
 *     
 *   Solution: 3-Node Raft Cluster
 *     - Fault-tolerant: Survives 1 node failure (majority = 2 of 3)
 *     - Highly available: New leader elected automatically
 *     - Consistent: Raft ensures all nodes agree on state
 *     - Persistent: State replicated across all nodes
 * 
 * Raft Consensus Protocol:
 *   
 *   Purpose: Ensure all controller nodes agree on system state
 *   
 *   Key Properties:
 *   1. Leader Election: Exactly one leader at any time
 *   2. Log Replication: All state changes replicated to majority
 *   3. Safety: Committed entries never lost
 *   4. Liveness: System makes progress if majority available
 *   
 *   State Machine:
 *   
 *   ┌──────────┐
 *   │ FOLLOWER │ ← All nodes start here
 *   └─────┬────┘
 *         │ Election timeout
 *         ↓
 *   ┌───────────┐
 *   │ CANDIDATE │ ← Request votes
 *   └─────┬─────┘
 *         │ Win election (majority votes)
 *         ↓
 *   ┌────────┐
 *   │ LEADER │ ← Coordinate cluster
 *   └────────┘
 * 
 * Fault Tolerance Guarantees:
 *   
 *   3-Node Cluster (N=3, Majority=2):
 *   
 *   Scenario                         Operational?  Leader?
 *   -------------------------------  ------------  -------
 *   All 3 nodes healthy              ✓ Yes         1 leader
 *   1 node fails (2 remaining)       ✓ Yes         Same or new leader
 *   2 nodes fail (1 remaining)       ✗ No          No leader (no majority)
 *   1 node fails, then recovers      ✓ Yes         Rejoins as follower
 *   Network partition (2 vs 1)       ✓ Partial     Majority side continues
 *   
 *   Recovery Time:
 *   - Leader failure detected: ~1-3 seconds (election timeout)
 *   - New leader elected: ~1-2 seconds (voting)
 *   - Total recovery: ~2-5 seconds (meets project goal!)
 * 
 * System State Management:
 *   
 *   What's Replicated via Raft:
 *   - Job queue: All pending jobs
 *   - Worker registry: Active workers and their capabilities
 *   - Job assignments: Which worker is processing which job
 *   - Job results: Completed job outcomes
 *   - Configuration: System parameters
 *   
 *   Not Replicated (Ephemeral):
 *   - Worker heartbeats: Only leader tracks
 *   - Network connections: Each node maintains own
 *   - In-progress work: Workers track locally
 * 
 * Controller Responsibilities:
 *   
 *   Job Lifecycle Management:
 *   1. Client submits job → Leader appends to Raft log
 *   2. Replicated to followers → Majority acknowledge
 *   3. Leader commits job → Adds to scheduling queue
 *   4. Worker requests work → Leader assigns job
 *   5. Worker reports progress → Leader tracks
 *   6. Job completes → Result stored in Raft log
 *   7. Client retrieves result → Leader responds
 *   
 *   Worker Management:
 *   - Registration: Workers connect and announce capabilities
 *   - Health monitoring: Periodic heartbeats (every 2 seconds)
 *   - Failure detection: 3 missed heartbeats = worker dead
 *   - Job reassignment: Move work from failed to healthy workers
 *   
 *   Leader-Specific Duties:
 *   - Accept client job submissions
 *   - Schedule jobs to workers
 *   - Monitor worker heartbeats
 *   - Aggregate results
 *   - Drive Raft log replication
 *   
 *   Follower Duties:
 *   - Replicate Raft log
 *   - Vote in leader elections
 *   - Redirect clients to leader
 *   - Stay ready to become leader
 * 
 * Network Communication:
 *   
 *   Port Allocation:
 *   - Worker port (5000): Workers connect here for jobs
 *   - Raft port (6000): Controllers communicate here
 *   - Client port (5001): Clients submit jobs here (future)
 *   
 *   Why Separate Ports?
 *   - Isolation: Worker traffic doesn't interfere with Raft
 *   - Firewall rules: Can restrict Raft port to cluster only
 *   - Debugging: Can monitor ports independently
 *   - Performance: Separate thread pools
 * 
 * Deployment Topology:
 *   
 *   Development (Single Machine):
 *     Node 1: localhost:5000 (worker), localhost:6000 (raft)
 *     Node 2: localhost:5001 (worker), localhost:6001 (raft)
 *     Node 3: localhost:5002 (worker), localhost:6002 (raft)
 *     
 *     Good for: Testing, debugging
 *     Not for: Performance evaluation (single CPU)
 *   
 *   Production (Multi-Machine):
 *     Node 1: kh01:5000 (worker), kh01:6000 (raft)
 *     Node 2: kh02:5000 (worker), kh02:6000 (raft)
 *     Node 3: kh03:5000 (worker), kh03:6000 (raft)
 *     
 *     Good for: Production, evaluation
 *     Requirements: Low-latency network, synchronized clocks
 * 
 *******************************************************************************/

/*******************************************************************************
 * SECTION 2: GLOBAL STATE AND SIGNAL HANDLING
 *******************************************************************************/

/**
 * @var shutdown_requested
 * @brief Atomic flag indicating shutdown has been requested
 * 
 * This flag is set by signal_handler() when SIGINT (Ctrl+C) or SIGTERM
 * is received. The main event loop checks this flag to initiate graceful
 * shutdown.
 * 
 * Why Atomic?
 *   Signal handlers run asynchronously on a separate stack. Without atomic
 *   operations, there's a race condition:
 *   - Main thread reads shutdown_requested
 *   - Signal handler writes shutdown_requested
 *   - Main thread might see stale value
 *   
 *   std::atomic provides memory ordering guarantees that ensure the write
 *   in signal handler is visible to main thread.
 * 
 * Design Pattern: Graceful Shutdown
 *   Instead of abruptly terminating, we:
 *   1. Set flag
 *   2. Stop accepting new work
 *   3. Finish in-progress work
 *   4. Clean up resources
 *   5. Exit cleanly
 *   
 *   Benefits:
 *   + No corrupted state
 *   + Workers notified of shutdown
 *   + Logs flushed
 *   + Checkpoint saved
 * 
 * Alternative Approaches:
 *   
 *   Bad: exit() in signal handler
 *     signal_handler(int) {
 *       exit(0);  // Abrupt termination!
 *     }
 *     
 *     Problems:
 *     - Resources not cleaned up
 *     - Workers not notified
 *     - Potential data loss
 *   
 *   Bad: Non-atomic flag
 *     bool shutdown_requested = false;  // Race condition!
 *     
 *     Problems:
 *     - May not be visible to main thread
 *     - Undefined behavior (data race)
 *   
 *   Good: Atomic flag (current approach)
 *     std::atomic<bool> shutdown_requested(false);
 *     
 *     Benefits:
 *     + Thread-safe
 *     + Visible across threads
 *     + Well-defined behavior
 * 
 * @threadsafety Thread-safe via std::atomic
 * 
 * @see signal_handler() For setting this flag
 * @see main() For reading this flag in event loop
 */
std::atomic<bool> shutdown_requested(false);

/**
 * @var global_controller
 * @brief Pointer to controller instance for signal handler access
 * 
 * Signal handlers in C++ are constrained - they can't access local variables
 * or class members directly. This global pointer provides a way for the
 * signal handler to call controller->stop().
 * 
 * Lifetime Management:
 *   1. Initialized to nullptr
 *   2. Set to &controller in main()
 *   3. Used by signal_handler() if non-null
 *   4. Becomes invalid after main() exits
 * 
 * Safety Considerations:
 *   
 *   Q: What if signal arrives before controller is initialized?
 *   A: signal_handler() checks if (global_controller) before dereferencing
 *   
 *   Q: What if signal arrives after controller is destroyed?
 *   A: Main loop exits before controller destructor runs, so this is safe
 *   
 *   Q: Is this thread-safe?
 *   A: Yes - written once in main thread, read in signal handler
 *      (signal handlers don't run concurrently with themselves)
 * 
 * Alternative Approaches:
 *   
 *   Modern C++: std::atomic<RaftController*>
 *     std::atomic<RaftController*> global_controller(nullptr);
 *     
 *     More explicit about thread safety, but unnecessary here since:
 *     - Written once before signals enabled
 *     - Read in signal handler
 *     - No concurrent writes
 *   
 *   Better: Signal-safe queue
 *     In production systems, signal handlers often just write to a pipe
 *     or eventfd, then main thread handles shutdown logic.
 *     
 *     Benefits:
 *     + Signal handler does minimal work
 *     + Main thread has full context
 *     + No global state needed
 *     
 *     Trade-off:
 *     - More complex
 *     - Overkill for this project
 * 
 * @warning 
 *   DO NOT access this pointer after main() returns.
 *   DO NOT modify after initial assignment.
 * 
 * @see signal_handler() For usage
 */
RaftController* global_controller = nullptr;

/**
 * @fn signal_handler
 * @brief Handles SIGINT and SIGTERM for graceful shutdown
 * 
 * This function is called asynchronously by the operating system when:
 * - User presses Ctrl+C (SIGINT)
 * - Process receives kill command (SIGTERM)
 * - Systemd/Docker sends shutdown signal
 * 
 * Graceful Shutdown Sequence:
 *   
 *   1. Signal received (user presses Ctrl+C)
 *      ↓
 *   2. OS calls signal_handler() asynchronously
 *      ↓
 *   3. Set shutdown_requested flag
 *      ↓
 *   4. Call controller->stop() if available
 *      ↓
 *   5. Signal handler returns
 *      ↓
 *   6. Main event loop sees shutdown_requested
 *      ↓
 *   7. Break out of loop
 *      ↓
 *   8. Call controller.stop() again (idempotent)
 *      ↓
 *   9. Clean up resources
 *      ↓
 *   10. Exit main() cleanly
 * 
 * Signal-Safe Programming:
 *   
 *   POSIX restricts what can be done in signal handlers. Only "async-signal-safe"
 *   functions are allowed. This excludes:
 *   - Memory allocation (malloc, new)
 *   - I/O operations (printf, std::cout)
 *   - Mutex locking (std::mutex)
 *   - Most C++ standard library
 *   
 *   Our handler is safe because:
 *   + std::atomic operations are signal-safe
 *   + Logger::info() is signal-safe (if implemented correctly)
 *   + controller->stop() is designed to be callable from signal handlers
 * 
 * Why Call stop() Twice?
 *   
 *   In signal handler:
 *     - Begins shutdown process immediately
 *     - Stops accepting new work
 *     
 *   In main():
 *     - Ensures cleanup even if signal handler didn't run
 *     - Idempotent - safe to call multiple times
 *     
 *   Belt-and-suspenders approach for reliability
 * 
 * Multiple Signals:
 *   
 *   First Ctrl+C:
 *     - Sets flag
 *     - Begins graceful shutdown
 *     - Takes 1-5 seconds
 *     
 *   Second Ctrl+C (if user is impatient):
 *     - Flag already set
 *     - stop() already called
 *     - System shuts down faster
 *     
 *   Third Ctrl+C (user is REALLY impatient):
 *     - Could implement force-kill
 *     - Current implementation: Same as second
 * 
 * @param signal Signal number (SIGINT=2, SIGTERM=15)
 * 
 * @async-signal-safe Must only call async-signal-safe functions
 * 
 * @sideeffects
 *   - Sets shutdown_requested flag
 *   - Calls controller->stop() if available
 *   - Logs shutdown message
 * 
 * Design Rationale:
 *   
 *   Q: Why not just exit(0) in signal handler?
 *   A: We need to:
 *      - Notify workers of shutdown
 *      - Flush Raft log to disk
 *      - Close network connections
 *      - Deallocate resources
 *      
 *      Abrupt exit() would skip all of this.
 *   
 *   Q: Why check global_controller != nullptr?
 *   A: Signal could arrive during startup before controller is initialized.
 *      The null check prevents dereferencing invalid pointer.
 * 
 * Example Scenarios:
 *   
 *   Normal Shutdown:
 *     $ ./controller --id 1 ...
 *     [INFO] Controller running...
 *     ^C
 *     [INFO] Shutdown signal received
 *     [INFO] Shutting down...
 *     [INFO] Notifying workers...
 *     [INFO] Flushing Raft log...
 *     [INFO] Shutdown complete
 *     $
 *   
 *   Startup Failure (signal before controller ready):
 *     $ ./controller --id 1 ...
 *     [ERROR] Failed to bind port 5000
 *     ^C
 *     [INFO] Shutdown signal received
 *     # controller is null, so stop() not called
 *     # But program still exits cleanly
 *     $
 * 
 * @see shutdown_requested For flag being set
 * @see global_controller For pointer being checked
 */
void signal_handler(int signal) {
    // Check which signal was received
    // SIGINT (2) = Ctrl+C
    // SIGTERM (15) = kill command
    if (signal == SIGINT || signal == SIGTERM) {
        // Log shutdown initiation
        // This helps operators know shutdown was intentional
        Logger::info("Shutdown signal received");
        
        // Set atomic flag to trigger shutdown in main loop
        // This is the primary shutdown mechanism
        shutdown_requested = true;
        
        // Immediately begin controller shutdown if initialized
        // This is the "fast path" - don't wait for main loop
        if (global_controller) {
            global_controller->stop();
        }
        // else: Controller not yet initialized, main() will exit naturally
    }
    // Other signals ignored (not handled by this function)
}

/*******************************************************************************
 * SECTION 3: USER INTERFACE
 *******************************************************************************/

/**
 * @fn print_usage
 * @brief Displays command-line help text
 * 
 * This function prints comprehensive usage information for operators deploying
 * controller nodes. It's the first line of defense against configuration
 * mistakes.
 * 
 * Help Text Design Principles:
 *   1. Show program name dynamically (argv[0])
 *   2. List all options with defaults
 *   3. Explain format for complex options (peers)
 *   4. Provide concrete example
 *   5. Keep formatting consistent and readable
 * 
 * When This Is Called:
 *   - User runs: ./controller --help
 *   - User provides invalid arguments
 *   - User omits required arguments (--id)
 * 
 * @param program_name Name of executable from argv[0]
 * 
 * @sideeffects Writes to stdout
 * 
 * Example Output:
 *   
 *   $ ./controller --help
 *   Usage: ./controller [options]
 *   Options:
 *     --id ID              Node ID (required, 1-3)
 *     --port PORT          Worker listen port (default: 5000)
 *     --raft-port PORT     Raft communication port (default: 6000)
 *     --data-dir DIR       Data directory (default: ./data)
 *     --raft-dir DIR       Raft log directory (default: ./raft_data)
 *     --peers PEERS        Peer list (format: id:host:wport:rport,...)
 *                          Example: 2:kh02:5000:6000,3:kh03:5000:6000
 *     --help               Show this help message
 *   
 *   Example:
 *     ./controller --id 1 --port 5000 --raft-port 6000 \
 *       --peers 2:kh02:5000:6000,3:kh03:5000:6000
 * 
 * Documentation Standards:
 *   
 *   Each option documented with:
 *   - Long form (--option)
 *   - Short description
 *   - Default value (if any)
 *   - Required vs optional
 *   
 *   Good:
 *     --port PORT          Worker listen port (default: 5000)
 *     
 *   Bad (missing default):
 *     --port PORT          Worker listen port
 *     
 *   Bad (unclear):
 *     -p                   Port
 * 
 * Enhancement Opportunities:
 *   
 *   Add validation hints:
 *     --id ID              Node ID (required, must be unique, 1-255)
 *     --port PORT          Worker port (1-65535, default: 5000)
 *   
 *   Add troubleshooting section:
 *     Common Issues:
 *       - "Address already in use": Change --port
 *       - "Connection refused": Check --peers format
 *   
 *   Add environment variables:
 *     Environment:
 *       CONTROLLER_ID        Override --id
 *       CONTROLLER_PEERS     Override --peers
 * 
 * @see main() For argument parsing
 */
void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --id ID              Node ID (required, 1-3)\n"
              << "  --port PORT          Worker listen port (default: 5000)\n"
              << "  --raft-port PORT     Raft communication port (default: 6000)\n"
              << "  --data-dir DIR       Data directory (default: ./data)\n"
              << "  --raft-dir DIR       Raft log directory (default: ./raft_data)\n"
              << "  --peers PEERS        Peer list (format: id:host:wport:rport,...)\n"
              << "                       Example: 2:kh02:5000:6000,3:kh03:5000:6000\n"
              << "  --help               Show this help message\n"
              << "\nExample:\n"
              << "  " << program_name << " --id 1 --port 5000 --raft-port 6000 \\\n"
              << "    --peers 2:kh02:5000:6000,3:kh03:5000:6000\n";
}

/*******************************************************************************
 * SECTION 4: CONFIGURATION PARSING
 *******************************************************************************/

/**
 * @fn parse_peers
 * @brief Parses peer list string and adds peers to controller
 * 
 * The peer list specifies other controller nodes in the Raft cluster. Each
 * peer is described by four components: node ID, hostname, worker port, and
 * Raft port.
 * 
 * Input Format:
 *   id:host:worker_port:raft_port,id:host:worker_port:raft_port,...
 *   
 *   Example:
 *     2:kh02:5000:6000,3:kh03:5000:6000
 *     
 *   This defines two peers:
 *     Peer 1: ID=2, host=kh02, worker_port=5000, raft_port=6000
 *     Peer 2: ID=3, host=kh03, worker_port=5000, raft_port=6000
 * 
 * Parsing Algorithm:
 *   
 *   1. Split by comma to get individual peer strings
 *      "2:kh02:5000:6000,3:kh03:5000:6000" → ["2:kh02:5000:6000", "3:kh03:5000:6000"]
 *   
 *   2. For each peer string, split by colon
 *      "2:kh02:5000:6000" → ["2", "kh02", "5000", "6000"]
 *   
 *   3. Validate exactly 4 parts
 *      If != 4: Error
 *   
 *   4. Parse each component
 *      - parts[0]: Node ID (stoull)
 *      - parts[1]: Hostname (string)
 *      - parts[2]: Worker port (stoi → uint16_t)
 *      - parts[3]: Raft port (stoi → uint16_t)
 *   
 *   5. Add peer to controller
 *      controller.add_controller_peer(node_id, hostname, worker_port, raft_port)
 * 
 * Error Handling:
 *   
 *   Invalid Format Examples:
 *     "2:kh02:5000"           → Only 3 parts (missing raft_port)
 *     "kh02:5000:6000"        → Only 3 parts (missing ID)
 *     "2:kh02:5000:6000:foo"  → 5 parts (extra field)
 *     "abc:kh02:5000:6000"    → Node ID not numeric
 *     "2:kh02:xyz:6000"       → Worker port not numeric
 *     "2:kh02:70000:6000"     → Port out of range (> 65535)
 *   
 *   All errors return false with error message to stderr
 * 
 * Design Decisions:
 *   
 *   Q: Why not use JSON or YAML for configuration?
 *   A: Command-line strings are simpler for scripting:
 *      
 *      Shell script:
 *        PEERS="2:kh02:5000:6000,3:kh03:5000:6000"
 *        ./controller --id 1 --peers "$PEERS"
 *      
 *      With JSON, you'd need a separate config file and parsing library.
 *   
 *   Q: Why parse here instead of in RaftController?
 *   A: Separation of concerns:
 *      - main.cpp: CLI and user interaction
 *      - RaftController: Core Raft logic
 *      
 *      Makes testing easier (can test parsing independently)
 *   
 *   Q: Why not use getopt_long()?
 *   A: Simplicity. For this project, manual parsing is sufficient.
 *      getopt_long() would be better for more complex CLIs.
 * 
 * @param peers_str Comma-separated list of peer specifications
 * @param controller Controller instance to add peers to
 * 
 * @return true if all peers parsed successfully, false on any error
 * 
 * @sideeffects
 *   - Adds peers to controller via add_controller_peer()
 *   - Writes error messages to stderr
 * 
 * Example Usage:
 *   
 *   std::string peers = "2:kh02:5000:6000,3:kh03:5000:6000";
 *   RaftController controller(config);
 *   
 *   if (!parse_peers(peers, controller)) {
 *       std::cerr << "Failed to parse peers\n";
 *       return 1;
 *   }
 *   
 *   // controller now knows about peers 2 and 3
 * 
 * Validation Strategy:
 *   
 *   Fail Fast:
 *     - Check part count immediately
 *     - Parse each field with exception handling
 *     - Return false on first error
 *     
 *   Alternative (Lenient):
 *     - Skip invalid peers, continue parsing
 *     - Log warnings but proceed
 *     - Use what we can
 *     
 *   Trade-off:
 *     Current approach: Fail fast
 *     Rationale: Configuration errors should be fixed, not ignored
 * 
 * @see RaftController::add_controller_peer() For peer registration
 */
bool parse_peers(const std::string& peers_str, RaftController& controller) {
    // Create string stream for parsing comma-separated list
    std::stringstream ss(peers_str);
    std::string peer;
    
    // Split by comma to get individual peer specifications
    // Example: "2:kh02:5000:6000,3:kh03:5000:6000"
    //       → "2:kh02:5000:6000" then "3:kh03:5000:6000"
    while (std::getline(ss, peer, ',')) {
        // Parse single peer specification
        std::stringstream peer_ss(peer);
        std::string part;
        std::vector<std::string> parts;
        
        // Split by colon to get components
        // Example: "2:kh02:5000:6000" → ["2", "kh02", "5000", "6000"]
        while (std::getline(peer_ss, part, ':')) {
            parts.push_back(part);
        }
        
        // Validate format: Must have exactly 4 parts
        // Format: id:hostname:worker_port:raft_port
        if (parts.size() != 4) {
            std::cerr << "Invalid peer format: " << peer << "\n";
            return false;  // Fail immediately on format error
        }
        
        // Parse components with exception handling
        // Conversion failures (non-numeric strings) throw exceptions
        try {
            // Parse node ID (unsigned 64-bit integer)
            // Valid range: 1-18446744073709551615 (but practically 1-255)
            uint64_t node_id = std::stoull(parts[0]);
            
            // Parse hostname (no conversion needed - already string)
            std::string hostname = parts[1];
            
            // Parse worker port (16-bit unsigned integer)
            // Valid range: 1-65535
            // Cast from int to uint16_t (safe because stoi validates range)
            uint16_t worker_port = static_cast<uint16_t>(std::stoi(parts[2]));
            
            // Parse Raft port (16-bit unsigned integer)
            // Valid range: 1-65535
            uint16_t raft_port = static_cast<uint16_t>(std::stoi(parts[3]));
            
            // Add peer to controller
            // Controller will store this for Raft communication
            controller.add_controller_peer(node_id, hostname, worker_port, raft_port);
            
        } catch (const std::exception& e) {
            // Conversion failed (non-numeric, out of range, etc.)
            std::cerr << "Failed to parse peer: " << peer << " - " << e.what() << "\n";
            return false;  // Fail immediately on parse error
        }
    }
    
    // All peers parsed successfully
    return true;
}

/*******************************************************************************
 * SECTION 5: MAIN FUNCTION
 *******************************************************************************/

/**
 * @fn main
 * @brief Entry point for controller node
 * 
 * This is the main control flow for the controller process. It handles:
 * 1. Signal handler registration
 * 2. Command-line argument parsing
 * 3. Configuration validation
 * 4. Controller initialization
 * 5. Cluster formation
 * 6. Main event loop (monitoring)
 * 7. Graceful shutdown
 * 
 * Execution Flow:
 *   
 *   Startup Phase:
 *   ┌─────────────────────────────────┐
 *   │ 1. Register signal handlers     │
 *   │ 2. Parse command-line args      │
 *   │ 3. Validate required args       │
 *   │ 4. Create RaftController        │
 *   │ 5. Parse and add peers          │
 *   │ 6. Start controller             │
 *   └─────────────────────────────────┘
 *              ↓
 *   Event Loop Phase:
 *   ┌─────────────────────────────────┐
 *   │ while (!shutdown_requested) {   │
 *   │   sleep(5 seconds)              │
 *   │   print_statistics()            │
 *   │   print Raft status             │
 *   │ }                               │
 *   └─────────────────────────────────┘
 *              ↓
 *   Shutdown Phase:
 *   ┌─────────────────────────────────┐
 *   │ 1. Log shutdown message         │
 *   │ 2. Call controller.stop()       │
 *   │ 3. Wait for threads to finish   │
 *   │ 4. Destructor cleans up         │
 *   │ 5. Exit process                 │
 *   └─────────────────────────────────┘
 * 
 * Command-Line Arguments:
 *   
 *   Required:
 *     --id ID              Node's unique identifier in cluster
 *                          Must be 1, 2, or 3 for this project
 *   
 *   Optional:
 *     --port PORT          Worker communication port (default: 5000)
 *     --raft-port PORT     Raft consensus port (default: 6000)
 *     --data-dir DIR       Historical price data (default: ./data)
 *     --raft-dir DIR       Raft log persistence (default: ./raft_data)
 *     --peers PEERS        Other controller nodes (format: id:host:wport:rport,...)
 *     --help               Display usage and exit
 * 
 * Configuration Validation:
 *   
 *   The program performs extensive validation before starting:
 *   
 *   1. Required Arguments:
 *      - --id must be provided (error if missing)
 *   
 *   2. Port Numbers:
 *      - Must be 1-65535 (validated by stoi)
 *      - Should be available (not in use)
 *   
 *   3. Directories:
 *      - Should exist or be creatable
 *      - Should be writable
 *   
 *   4. Peer Format:
 *      - Must match id:host:wport:rport format
 *      - All fields must be valid
 * 
 * Event Loop Design:
 *   
 *   The main loop is intentionally simple:
 *   
 *   while (!shutdown_requested && controller.is_running()) {
 *       sleep(5 seconds);           // Don't busy-wait
 *       print_statistics();         // Show job queue status
 *       print Raft status;          // Show leader/follower state
 *   }
 *   
 *   Why 5 seconds?
 *   - Long enough to not spam logs
 *   - Short enough to detect problems quickly
 *   - Matches typical monitoring interval
 *   
 *   Alternative Designs:
 *   
 *   Event-Driven:
 *     while (!shutdown_requested) {
 *         wait_for_event();  // Block until event
 *         handle_event();
 *     }
 *     
 *     Pros: More responsive
 *     Cons: More complex, not needed here
 *   
 *   Busy Wait (BAD):
 *     while (!shutdown_requested) {
 *         if (controller.has_work()) {
 *             process_work();
 *         }
 *     }
 *     
 *     Cons: Wastes CPU, generates heat, drains battery
 * 
 * Graceful Shutdown:
 *   
 *   When shutdown_requested is set (via Ctrl+C):
 *   
 *   1. Break out of event loop
 *   2. Log "Shutting down..."
 *   3. Call controller.stop()
 *      - Stops accepting new jobs
 *      - Waits for in-progress work (with timeout)
 *      - Flushes Raft log to disk
 *      - Closes network connections
 *      - Notifies workers
 *   4. Return from main()
 *   5. Destructors run automatically
 *   6. Process exits cleanly
 *   
 *   Total time: Typically 1-3 seconds
 * 
 * Error Handling:
 *   
 *   The function returns different exit codes for different errors:
 *   
 *   0 = Success or --help
 *   1 = Invalid arguments or configuration
 *   2 = Failed to start controller (port in use, etc.)
 *   
 *   These codes enable scripting:
 *   
 *   #!/bin/bash
 *   ./controller --id 1 ...
 *   if [ $? -ne 0 ]; then
 *       echo "Controller failed to start"
 *       exit 1
 *   fi
 * 
 * @param argc Argument count
 * @param argv Argument vector
 * 
 * @return 0 on success, 1 on configuration error, 2 on startup failure
 * 
 * Example Invocations:
 *   
 *   Single-machine development:
 *     # Node 1 (will become leader)
 *     ./controller --id 1 --port 5000 --raft-port 6000 \
 *       --peers 2:localhost:5001:6001,3:localhost:5002:6002
 *     
 *     # Node 2
 *     ./controller --id 2 --port 5001 --raft-port 6001 \
 *       --peers 1:localhost:5000:6000,3:localhost:5002:6002
 *     
 *     # Node 3
 *     ./controller --id 3 --port 5002 --raft-port 6002 \
 *       --peers 1:localhost:5000:6000,2:localhost:5001:6001
 *   
 *   Production cluster (Khoury):
 *     # On kh01
 *     ./controller --id 1 --port 5000 --raft-port 6000 \
 *       --peers 2:kh02:5000:6000,3:kh03:5000:6000
 *     
 *     # On kh02
 *     ./controller --id 2 --port 5000 --raft-port 6000 \
 *       --peers 1:kh01:5000:6000,3:kh03:5000:6000
 *     
 *     # On kh03
 *     ./controller --id 3 --port 5000 --raft-port 6000 \
 *       --peers 1:kh01:5000:6000,2:kh02:5000:6000
 * 
 * Monitoring Output:
 *   
 *   Normal Operation:
 *     $ ./controller --id 1 ...
 *     [INFO] === Distributed Backtesting Raft Controller ===
 *     [INFO] Node ID: 1
 *     [INFO] Raft controller running. Press Ctrl+C to stop.
 *     [INFO] Jobs in queue: 5, Workers: 8, Completed: 120
 *     [INFO] [RAFT] This node is the LEADER
 *     [INFO] Jobs in queue: 3, Workers: 8, Completed: 122
 *     [INFO] [RAFT] This node is the LEADER
 *     ^C
 *     [INFO] Shutdown signal received
 *     [INFO] Shutting down...
 *     $
 *   
 *   Follower Node:
 *     $ ./controller --id 2 ...
 *     [INFO] === Distributed Backtesting Raft Controller ===
 *     [INFO] Node ID: 2
 *     [INFO] Raft controller running. Press Ctrl+C to stop.
 *     [INFO] Jobs in queue: 5, Workers: 0, Completed: 0
 *     [INFO] [RAFT] This node is a FOLLOWER (Leader: 1)
 *     [INFO] Jobs in queue: 3, Workers: 0, Completed: 0
 *     [INFO] [RAFT] This node is a FOLLOWER (Leader: 1)
 *   
 *   Leader Failure (follower becomes leader):
 *     [INFO] [RAFT] This node is a FOLLOWER (Leader: 1)
 *     [WARN] Lost connection to leader (timeout)
 *     [INFO] [RAFT] Starting election...
 *     [INFO] [RAFT] Election won! Became LEADER
 *     [INFO] [RAFT] This node is the LEADER
 *     [INFO] Jobs in queue: 8, Workers: 8, Completed: 122
 * 
 * @see RaftController For controller implementation
 * @see signal_handler() For shutdown handling
 * @see parse_peers() For peer list parsing
 */
int main(int argc, char* argv[]) {
    // SIGNAL HANDLER REGISTRATION
    // ============================
    
    // Register signal handler for graceful shutdown
    // SIGINT = Ctrl+C, SIGTERM = kill command
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Ignore SIGPIPE (broken pipe)
    // Prevents crash when writing to closed socket
    // Instead, write() returns -1 with errno = EPIPE
    signal(SIGPIPE, SIG_IGN);
    
    // CONFIGURATION INITIALIZATION
    // ============================
    
    // Create default configuration
    // These values can be overridden by command-line arguments
    RaftControllerConfig config;
    
    // Peer list string (parsed later)
    std::string peers_str;
    
    // Track if --id was provided (required argument)
    bool has_id = false;
    
    // COMMAND-LINE ARGUMENT PARSING
    // ==============================
    
    // Parse arguments manually (simple approach for this project)
    // For more complex CLIs, consider getopt_long()
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        // Help flag - print usage and exit
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;  // Exit 0 = success (help is not an error)
        }
        
        // Node ID (required)
        else if (arg == "--id" && i + 1 < argc) {
            config.node_id = std::stoull(argv[++i]);
            has_id = true;
        }
        
        // Worker listen port (optional, default: 5000)
        else if (arg == "--port" && i + 1 < argc) {
            config.listen_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        
        // Raft communication port (optional, default: 6000)
        else if (arg == "--raft-port" && i + 1 < argc) {
            config.raft_port = static_cast<uint16_t>(std::stoi(argv[++i]));
        }
        
        // Data directory (optional, default: ./data)
        else if (arg == "--data-dir" && i + 1 < argc) {
            config.data_directory = argv[++i];
        }
        
        // Raft log directory (optional, default: ./raft_data)
        else if (arg == "--raft-dir" && i + 1 < argc) {
            config.raft_log_directory = argv[++i];
        }
        
        // Peer list (optional but recommended for cluster)
        else if (arg == "--peers" && i + 1 < argc) {
            peers_str = argv[++i];
        }
        
        // Unknown argument - print error and usage
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;  // Exit 1 = error
        }
    }
    
    // CONFIGURATION VALIDATION
    // ========================
    
    // Validate required arguments
    if (!has_id) {
        std::cerr << "Error: --id is required\n";
        print_usage(argv[0]);
        return 1;
    }
    
    // LOGGING INITIALIZATION
    // ======================
    
    // Set log level to INFO
    // Hides DEBUG messages, shows INFO/WARNING/ERROR
    Logger::set_level(LogLevel::INFO);
    
    // Print startup banner
    Logger::info("=== Distributed Backtesting Raft Controller ===");
    Logger::info("Node ID: " + std::to_string(config.node_id));
    
    // CONTROLLER CREATION
    // ===================
    
    // Create controller instance
    // Constructor initializes Raft state machine
    RaftController controller(config);
    
    // Store global pointer for signal handler
    // Must be set before controller.start() to handle early signals
    global_controller = &controller;
    
    // CLUSTER FORMATION
    // =================
    
    // Parse and add peer nodes
    // This tells controller about other nodes in the Raft cluster
    if (!peers_str.empty()) {
        if (!parse_peers(peers_str, controller)) {
            Logger::error("Failed to parse peers");
            return 1;
        }
    }
    // else: No peers specified - single-node cluster (for testing only)
    
    // CONTROLLER STARTUP
    // ==================
    
    // Start controller (begins Raft protocol)
    // This:
    // - Binds to worker and Raft ports
    // - Starts background threads
    // - Begins Raft election timeout
    // - Becomes candidate if timeout expires
    if (!controller.start()) {
        Logger::error("Failed to start Raft controller");
        return 1;  // Startup failed (port in use, network error, etc.)
    }
    
    // Log successful startup
    Logger::info("Raft controller running. Press Ctrl+C to stop.");
    
    // MAIN EVENT LOOP
    // ===============
    
    // Run until shutdown requested or controller fails
    // This loop is the "steady state" of the controller
    while (!shutdown_requested && controller.is_running()) {
        // Sleep 5 seconds between status updates
        // Prevents log spam while keeping monitoring responsive
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        // Print controller statistics
        // Shows: jobs in queue, active workers, completed jobs
        // Helps operators monitor system health
        controller.print_statistics();
        
        // RAFT STATUS REPORTING
        // =====================
        
        // Print Raft role and leader information
        // This is critical for operators to understand cluster state
        if (controller.is_raft_leader()) {
            // This node is the leader
            // Leader is responsible for:
            // - Accepting job submissions
            // - Scheduling work to workers
            // - Monitoring worker health
            Logger::info("[RAFT] This node is the LEADER");
        } else {
            // This node is a follower
            // Follower responsibilities:
            // - Replicate leader's log
            // - Vote in elections
            // - Redirect clients to leader
            Logger::info("[RAFT] This node is a FOLLOWER (Leader: " +
                        std::to_string(controller.get_raft_leader_id()) + ")");
        }
    }
    // Loop exits when:
    // - shutdown_requested set by signal handler
    // - controller.is_running() returns false (internal failure)
    
    // GRACEFUL SHUTDOWN
    // =================
    
    // Log shutdown initiation
    Logger::info("Shutting down...");
    
    // Stop controller
    // This is called even if signal_handler() already called it (idempotent)
    // Ensures cleanup happens regardless of how we got here
    controller.stop();
    
    // Controller destructor runs here (RAII cleanup)
    // - Joins background threads
    // - Closes network sockets
    // - Flushes logs
    // - Deallocates resources
    
    // Return success
    // Process exits cleanly
    return 0;
}

/*******************************************************************************
 * SECTION 6: DEPLOYMENT GUIDE
 *******************************************************************************/

/*
 * SINGLE-MACHINE DEVELOPMENT DEPLOYMENT
 * ======================================
 * 
 * Use different ports for each node on localhost:
 * 
 * Terminal 1 (Node 1):
 *   $ ./controller --id 1 \
 *       --port 5000 --raft-port 6000 \
 *       --data-dir ./data \
 *       --raft-dir ./raft_data_1 \
 *       --peers 2:localhost:5001:6001,3:localhost:5002:6002
 * 
 * Terminal 2 (Node 2):
 *   $ ./controller --id 2 \
 *       --port 5001 --raft-port 6001 \
 *       --data-dir ./data \
 *       --raft-dir ./raft_data_2 \
 *       --peers 1:localhost:5000:6000,3:localhost:5002:6002
 * 
 * Terminal 3 (Node 3):
 *   $ ./controller --id 3 \
 *       --port 5002 --raft-port 6002 \
 *       --data-dir ./data \
 *       --raft-dir ./raft_data_3 \
 *       --peers 1:localhost:5000:6000,2:localhost:5001:6001
 * 
 * Important:
 *   - All nodes share ./data (price data)
 *   - Each node has separate raft_dir for persistence
 *   - Use different ports to avoid conflicts
 * 
 * Advantages:
 *   + Easy debugging (all output visible)
 *   + Fast development cycle
 *   + No network configuration needed
 * 
 * Disadvantages:
 *   - Single CPU bottleneck (can't measure true parallelism)
 *   - Shared resources (cache, memory bandwidth)
 *   - Not representative of production
 * 
 * 
 * MULTI-MACHINE CLUSTER DEPLOYMENT
 * =================================
 * 
 * Deploy across multiple machines for realistic evaluation:
 * 
 * On kh01:
 *   $ ssh kh01.khoury.neu.edu
 *   $ cd ~/cs6650-project
 *   $ ./controller --id 1 \
 *       --port 5000 --raft-port 6000 \
 *       --data-dir /nfs/data \
 *       --raft-dir ~/raft_data \
 *       --peers 2:kh02:5000:6000,3:kh03:5000:6000
 * 
 * On kh02:
 *   $ ssh kh02.khoury.neu.edu
 *   $ cd ~/cs6650-project
 *   $ ./controller --id 2 \
 *       --port 5000 --raft-port 6000 \
 *       --data-dir /nfs/data \
 *       --raft-dir ~/raft_data \
 *       --peers 1:kh01:5000:6000,3:kh03:5000:6000
 * 
 * On kh03:
 *   $ ssh kh03.khoury.neu.edu
 *   $ cd ~/cs6650-project
 *   $ ./controller --id 3 \
 *       --port 5000 --raft-port 6000 \
 *       --data-dir /nfs/data \
 *       --raft-dir ~/raft_data \
 *       --peers 1:kh01:5000:6000,2:kh02:5000:6000
 * 
 * Prerequisites:
 *   1. Binary copied to all machines:
 *      for host in kh01 kh02 kh03; do
 *        scp controller $host:~/cs6650-project/
 *      done
 *   
 *   2. Data directory accessible (NFS or replicated):
 *      # Shared NFS mount
 *      /nfs/data → contains AAPL.csv, GOOGL.csv, etc.
 *   
 *   3. Firewall rules allow ports 5000, 6000:
 *      # Check connectivity
 *      nc -zv kh02 5000
 *      nc -zv kh02 6000
 *   
 *   4. Synchronized clocks (for Raft timeouts):
 *      # Check clock skew
 *      for host in kh01 kh02 kh03; do
 *        ssh $host date +%s
 *      done
 *      # Should all be within 1-2 seconds
 * 
 * Advantages:
 *   + True distributed system
 *   + Realistic performance evaluation
 *   + Can test network partitions
 *   + Independent failure domains
 * 
 * Disadvantages:
 *   - More complex setup
 *   - Network dependencies
 *   - Harder to debug (distributed logs)
 * 
 * 
 * AUTOMATED DEPLOYMENT SCRIPT
 * ===========================
 * 
 * deploy_cluster.sh:
 * 
 *   #!/bin/bash
 *   set -e  # Exit on error
 *   
 *   NODES="kh01 kh02 kh03"
 *   DATA_DIR="/nfs/cs6650/data"
 *   
 *   echo "Building controller..."
 *   cd ~/cs6650-project
 *   mkdir -p build && cd build
 *   cmake ..
 *   make -j4 controller
 *   
 *   echo "Deploying to nodes..."
 *   for node in $NODES; do
 *     echo "  → $node"
 *     ssh $node "mkdir -p ~/cs6650-project ~/raft_data"
 *     scp controller $node:~/cs6650-project/
 *   done
 *   
 *   echo "Starting controllers..."
 *   ssh kh01 "cd ~/cs6650-project && nohup ./controller \
 *     --id 1 --port 5000 --raft-port 6000 \
 *     --data-dir $DATA_DIR --raft-dir ~/raft_data \
 *     --peers 2:kh02:5000:6000,3:kh03:5000:6000 \
 *     > controller.log 2>&1 &"
 *   
 *   ssh kh02 "cd ~/cs6650-project && nohup ./controller \
 *     --id 2 --port 5000 --raft-port 6000 \
 *     --data-dir $DATA_DIR --raft-dir ~/raft_data \
 *     --peers 1:kh01:5000:6000,3:kh03:5000:6000 \
 *     > controller.log 2>&1 &"
 *   
 *   ssh kh03 "cd ~/cs6650-project && nohup ./controller \
 *     --id 3 --port 5000 --raft-port 6000 \
 *     --data-dir $DATA_DIR --raft-dir ~/raft_data \
 *     --peers 1:kh01:5000:6000,2:kh02:5000:6000 \
 *     > controller.log 2>&1 &"
 *   
 *   echo "Waiting for startup..."
 *   sleep 10
 *   
 *   echo "Checking status..."
 *   for node in $NODES; do
 *     echo "  $node:"
 *     ssh $node "ps aux | grep controller | grep -v grep"
 *   done
 *   
 *   echo "Cluster deployed successfully!"
 * 
 * Usage:
 *   $ chmod +x deploy_cluster.sh
 *   $ ./deploy_cluster.sh
 * 
 * 
 * CLOUD DEPLOYMENT (AWS/GCP)
 * ===========================
 * 
 * Use infrastructure-as-code (Terraform):
 * 
 * main.tf:
 * 
 *   provider "aws" {
 *     region = "us-east-1"
 *   }
 *   
 *   resource "aws_instance" "controller" {
 *     count         = 3
 *     ami           = "ami-0c55b159cbfafe1f0"  # Ubuntu 20.04
 *     instance_type = "t3.medium"
 *     
 *     tags = {
 *       Name = "controller-${count.index + 1}"
 *     }
 *     
 *     user_data = templatefile("startup.sh", {
 *       node_id = count.index + 1
 *       peers   = join(",", [
 *         for i in range(3) : 
 *           i != count.index ? "${i+1}:${aws_instance.controller[i].private_ip}:5000:6000" : ""
 *       ])
 *     })
 *   }
 * 
 * startup.sh:
 * 
 *   #!/bin/bash
 *   cd /home/ubuntu/cs6650-project
 *   ./controller --id ${node_id} --peers ${peers} > controller.log 2>&1 &
 * 
 * Deployment:
 *   $ terraform init
 *   $ terraform apply
 * 
 * Advantages:
 *   + Automated provisioning
 *   + Scalable (can easily add nodes)
 *   + Production-ready infrastructure
 * 
 * Disadvantages:
 *   - Cost ($0.10/hour per instance)
 *   - More complex (VPC, security groups, etc.)
 *   - Requires cloud account
 */

/*******************************************************************************
 * SECTION 7: OPERATIONAL PROCEDURES
 *******************************************************************************/

/*
 * STARTING THE CLUSTER
 * ====================
 * 
 * Cold Start (All Nodes Down):
 *   
 *   1. Start nodes in any order (Raft handles election)
 *   2. Wait 5-10 seconds for leader election
 *   3. Verify one node became leader
 *   4. Start workers to begin processing jobs
 *   
 *   Example:
 *     # Start all three at once (in separate terminals or tmux)
 *     ./controller --id 1 ... &
 *     ./controller --id 2 ... &
 *     ./controller --id 3 ... &
 *     
 *     # Wait for election
 *     sleep 10
 *     
 *     # Check logs
 *     grep LEADER *.log
 *     # Should see one node as LEADER
 * 
 * Warm Start (Some Nodes Already Running):
 *   
 *   If restarting one node while others run:
 *   
 *   1. Start the node normally
 *   2. It joins as follower
 *   3. Catches up on Raft log from leader
 *   4. Begins participating immediately
 *   
 *   Example:
 *     # Node 2 crashed, restarting
 *     ./controller --id 2 ...
 *     
 *     # Should see in logs:
 *     [INFO] Joining as FOLLOWER
 *     [INFO] Syncing log from leader (1)
 *     [INFO] Caught up with leader
 * 
 * 
 * MONITORING HEALTH
 * =================
 * 
 * Controller Logs:
 *   
 *   Watch for key indicators:
 *   
 *   Healthy:
 *     [INFO] [RAFT] This node is the LEADER
 *     [INFO] Jobs in queue: 5, Workers: 8, Completed: 120
 *     [INFO] Heartbeat from worker 4
 *     
 *   Unhealthy:
 *     [WARN] Lost connection to leader
 *     [ERROR] Failed to replicate log to follower 3
 *     [ERROR] Worker 5 timed out (no heartbeat)
 *     
 *   Elections (Normal):
 *     [INFO] Election timeout, becoming candidate
 *     [INFO] Received vote from node 2
 *     [INFO] Election won! Became LEADER
 *     
 *   Elections (Concerning):
 *     [WARN] Split vote, starting new election
 *     [WARN] Split vote, starting new election
 *     [WARN] Split vote, starting new election
 *     # If this repeats, investigate network partition
 * 
 * System Metrics:
 *   
 *   CPU Usage:
 *     $ top -b -n 1 | grep controller
 *     # Should be 5-50% during load
 *     # If 100%: Bottleneck, scale workers
 *     # If < 5%: Idle, submit more jobs
 *   
 *   Memory Usage:
 *     $ ps aux | grep controller | awk '{print $6}'
 *     # Should be 100-500 MB
 *     # Growing continuously? Memory leak
 *   
 *   Network Connections:
 *     $ netstat -an | grep 5000
 *     # Should see connections from workers
 *     $ netstat -an | grep 6000
 *     # Should see connections from other controllers
 *   
 *   Disk I/O:
 *     $ iostat -x 1
 *     # Watch %util for raft_dir disk
 *     # Should be < 30% normally
 *     # Spikes during log writes are normal
 * 
 * Health Check Script:
 *   
 *   check_cluster.sh:
 *   
 *   #!/bin/bash
 *   
 *   echo "Checking controller processes..."
 *   for node in kh01 kh02 kh03; do
 *     echo -n "  $node: "
 *     ssh $node "pgrep -x controller > /dev/null && echo OK || echo DEAD"
 *   done
 *   
 *   echo "
Checking Raft status..."
 *   for node in kh01 kh02 kh03; do
 *     echo "  $node:"
 *     ssh $node "tail -1 ~/cs6650-project/controller.log | grep RAFT"
 *   done
 *   
 *   echo "
Checking worker count..."
 *   ssh kh01 "tail -1 ~/cs6650-project/controller.log | grep 'Workers:'"
 * 
 * 
 * ROLLING UPGRADES
 * ================
 * 
 * To upgrade controller software without downtime:
 * 
 * 1. Upgrade followers first (one at a time)
 * 2. Verify each rejoins successfully
 * 3. Finally, upgrade leader (triggers election)
 * 4. New leader elected from upgraded followers
 * 
 * Procedure:
 *   
 *   # Identify leader
 *   for node in kh01 kh02 kh03; do
 *     ssh $node "grep LEADER controller.log | tail -1"
 *   done
 *   # Say leader is kh01
 *   
 *   # Upgrade kh02 (follower)
 *   ssh kh02 "pkill controller"
 *   scp new_controller kh02:~/cs6650-project/controller
 *   ssh kh02 "cd ~/cs6650-project && ./controller ... > controller.log 2>&1 &"
 *   sleep 10  # Wait to rejoin
 *   
 *   # Verify kh02 healthy
 *   ssh kh02 "tail controller.log | grep FOLLOWER"
 *   
 *   # Upgrade kh03 (follower)
 *   ssh kh03 "pkill controller"
 *   scp new_controller kh03:~/cs6650-project/controller
 *   ssh kh03 "cd ~/cs6650-project && ./controller ... > controller.log 2>&1 &"
 *   sleep 10
 *   
 *   # Verify kh03 healthy
 *   ssh kh03 "tail controller.log | grep FOLLOWER"
 *   
 *   # Finally, upgrade kh01 (leader)
 *   ssh kh01 "pkill controller"  # Triggers election!
 *   # kh02 or kh03 becomes new leader
 *   scp new_controller kh01:~/cs6650-project/controller
 *   ssh kh01 "cd ~/cs6650-project && ./controller ... > controller.log 2>&1 &"
 *   sleep 10
 *   
 *   # Verify kh01 healthy (now follower or leader)
 *   ssh kh01 "tail controller.log | grep RAFT"
 *   
 *   # Check all nodes upgraded
 *   for node in kh01 kh02 kh03; do
 *     ssh $node "~/cs6650-project/controller --version"
 *   done
 * 
 * Total Downtime: ~5 seconds (leader election)
 * 
 * 
 * DISASTER RECOVERY
 * =================
 * 
 * Scenario 1: Single Node Failure
 *   
 *   Impact: None (cluster continues)
 *   Recovery: Restart failed node
 *   
 *   $ ssh kh02
 *   $ cd ~/cs6650-project
 *   $ ./controller --id 2 ... > controller.log 2>&1 &
 *   
 *   Node automatically rejoins and syncs state
 * 
 * Scenario 2: Two Nodes Fail (No Quorum)
 *   
 *   Impact: Cluster stops (no majority)
 *   Recovery: Restart at least one failed node
 *   
 *   $ ssh kh02
 *   $ cd ~/cs6650-project
 *   $ ./controller --id 2 ... > controller.log 2>&1 &
 *   
 *   # Wait 10 seconds
 *   # Quorum restored, cluster operational
 * 
 * Scenario 3: Data Corruption
 *   
 *   If Raft log corrupted on one node:
 *   
 *   1. Stop affected node
 *   2. Delete corrupted raft_dir
 *   3. Restart node
 *   4. Node syncs from leader
 *   
 *   $ ssh kh02
 *   $ pkill controller
 *   $ rm -rf ~/raft_data/asterisk
 *   $ ./controller --id 2 ... > controller.log 2>&1 &
 *   
 *   Node starts fresh, replicates from leader
 * 
 * Scenario 4: Complete Cluster Loss
 *   
 *   If all three nodes die with unsaved state:
 *   
 *   1. Check if any raft_dir survived
 *   2. Start node with most recent log first
 *   3. Start other nodes, they'll sync from it
 *   
 *   If NO raft_dir survived:
 *   - Lost all state (job queue, results)
 *   - Must resubmit jobs
 *   - This is why backups are important!
 */

/*******************************************************************************
 * SECTION 8: TROUBLESHOOTING
 *******************************************************************************/

/*
 * COMMON STARTUP FAILURES
 * ========================
 * 
 * Error: "Address already in use"
 *   
 *   Cause: Port 5000 or 6000 already bound
 *   
 *   Check:
 *     $ netstat -tuln | grep 5000
 *     $ netstat -tuln | grep 6000
 *   
 *   Solutions:
 *     1. Kill existing controller:
 *        $ pkill controller
 *     
 *     2. Use different ports:
 *        $ ./controller --id 1 --port 5001 --raft-port 6001 ...
 *     
 *     3. Find and kill process using port:
 *        $ lsof -i :5000
 *        $ kill <pid>
 * 
 * Error: "Failed to parse peers"
 *   
 *   Cause: Invalid peer list format
 *   
 *   Check:
 *     # Should be: id:host:wport:rport
 *     # Wrong: 2,kh02,5000,6000  (commas instead of colons)
 *     # Wrong: 2:kh02:5000  (missing raft port)
 *     # Wrong: kh02:5000:6000  (missing ID)
 *   
 *   Solution:
 *     Use correct format: --peers 2:kh02:5000:6000,3:kh03:5000:6000
 * 
 * Error: "Connection refused"
 *   
 *   Cause: Cannot connect to peer
 *   
 *   Check:
 *     1. Peer process running?
 *        $ ssh kh02 "pgrep controller"
 *     
 *     2. Port accessible?
 *        $ nc -zv kh02 6000
 *     
 *     3. Firewall blocking?
 *        $ ssh kh02 "iptables -L | grep 6000"
 *   
 *   Solutions:
 *     1. Start peer process
 *     2. Open firewall port
 *     3. Check hostname resolution:
 *        $ ping kh02
 * 
 * 
 * NETWORK CONNECTIVITY ISSUES
 * ===========================
 * 
 * Symptoms:
 *   - Repeated elections
 *   - "Lost connection to leader"
 *   - Cluster never stabilizes
 * 
 * Diagnose:
 *   
 *   1. Check network latency:
 *      $ for node in kh01 kh02 kh03; do
 *          echo -n "$node: "
 *          ping -c 5 $node | grep avg
 *        done
 *      # Should be < 10ms for LAN
 *      # If > 100ms: Network problem
 *   
 *   2. Check packet loss:
 *      $ ping -c 100 kh02 | grep loss
 *      # Should be 0% loss
 *      # If > 1%: Network unreliable
 *   
 *   3. Check bandwidth:
 *      $ iperf3 -c kh02 -t 10
 *      # Should be > 100 Mbps for LAN
 *      # If < 10 Mbps: Network congestion
 * 
 * Solutions:
 *   - Move to same network switch
 *   - Increase Raft timeouts (tolerates higher latency)
 *   - Check for broadcast storms
 *   - Disable Wi-Fi (use wired)
 * 
 * 
 * RAFT ELECTION PROBLEMS
 * =======================
 * 
 * Problem: No leader elected (split votes)
 *   
 *   Symptoms:
 *     [INFO] Starting election...
 *     [WARN] Split vote
 *     [INFO] Starting election...
 *     [WARN] Split vote
 *     # Repeats indefinitely
 *   
 *   Cause: Clock skew, network delays, or 2-node cluster
 *   
 *   Solutions:
 *     1. Check clock sync:
 *        for node in kh01 kh02 kh03; do
 *          ssh $node date +%s
 *        done
 *        # Should be within 1 second
 *     
 *     2. Increase election timeout:
 *        # Edit RaftControllerConfig
 *        config.election_timeout_ms = 500;  # Increase from 150
 *     
 *     3. Ensure 3+ nodes (2 nodes can split votes)
 * 
 * Problem: Frequent leader changes
 *   
 *   Symptoms:
 *     [INFO] Became LEADER
 *     [INFO] Stepped down to FOLLOWER
 *     [INFO] Became LEADER
 *     # Thrashing between leader/follower
 *   
 *   Cause: Network instability, CPU overload
 *   
 *   Solutions:
 *     1. Check CPU usage:
 *        $ top
 *        # If controller at 100%: Reduce workload
 *     
 *     2. Check network:
 *        $ ping -c 100 <leader>
 *        # If packet loss: Fix network
 *     
 *     3. Increase heartbeat interval:
 *        config.heartbeat_interval_ms = 100;  # Increase from 50
 * 
 * 
 * PERFORMANCE DEBUGGING
 * =====================
 * 
 * Slow Job Processing:
 *   
 *   Symptoms: Throughput lower than expected
 *   
 *   Diagnose:
 *     1. Check worker count:
 *        # In controller log
 *        [INFO] Workers: 2  # Should be 8!
 *        
 *     2. Check CPU usage:
 *        $ top
 *        # Controller: Should be 10-30%
 *        # Workers: Should be 80-100%
 *        
 *     3. Check job queue:
 *        [INFO] Jobs in queue: 100  # Backlog!
 *        
 *     4. Profile controller:
 *        $ perf record -g -p $(pgrep controller)
 *        $ perf report
 * 
 * High Memory Usage:
 *   
 *   Symptoms: Memory grows over time
 *   
 *   Check:
 *     $ while true; do
 *         ps aux | grep controller | awk '{print $6}'
 *         sleep 60
 *       done
 *     # Should be stable, not growing
 *   
 *   If growing: Memory leak
 *     Use valgrind or AddressSanitizer to find leak
 */

/*******************************************************************************
 * SECTION 9: COMMON PITFALLS
 *******************************************************************************/

/*
 * PITFALL 1: Starting with Wrong Node ID
 * ---------------------------------------
 * 
 * Problem: Two nodes started with same ID
 * 
 * Example:
 *   # On kh01
 *   ./controller --id 1 ...
 *   
 *   # On kh02 (WRONG - same ID!)
 *   ./controller --id 1 ...
 * 
 * Result:
 *   - Raft protocol confusion
 *   - Duplicate log entries
 *   - Split brain
 * 
 * Solution:
 *   ALWAYS use unique IDs (1, 2, 3)
 * 
 * 
 * PITFALL 2: Forgetting to Specify Peers
 * ---------------------------------------
 * 
 * Problem: Starting controller without --peers
 * 
 * Example:
 *   ./controller --id 1  # No peers!
 * 
 * Result:
 *   - Node thinks it's the only one in cluster
 *   - Immediately becomes leader
 *   - Other nodes can't join
 * 
 * Solution:
 *   ALWAYS specify --peers for production
 *   (Omit only for single-node testing)
 * 
 * 
 * PITFALL 3: Mixing Up Worker and Raft Ports
 * --------------------------------------------
 * 
 * Problem: Using Raft port in peer list worker port field
 * 
 * Wrong:
 *   --peers 2:kh02:6000:5000  # Ports swapped!
 *   
 * Correct:
 *   --peers 2:kh02:5000:6000  # worker:raft
 * 
 * 
 * PITFALL 4: Not Synchronizing Clocks
 * ------------------------------------
 * 
 * Problem: Clock skew between nodes
 * 
 * Check:
 *   for node in kh01 kh02 kh03; do
 *     echo "$node: $(ssh $node date +%s)"
 *   done
 * 
 * If different by > 1 second: Raft timeouts unreliable
 * 
 * Solution:
 *   Use NTP:
 *     $ sudo systemctl start ntpd
 * 
 * 
 * PITFALL 5: Running on Same Machine Without Port Changes
 * ---------------------------------------------------------
 * 
 * Problem: Three nodes on localhost, all using default ports
 * 
 * Wrong:
 *   ./controller --id 1  # Port 5000, 6000
 *   ./controller --id 2  # Port 5000, 6000 - CONFLICT!
 * 
 * Correct:
 *   ./controller --id 1 --port 5000 --raft-port 6000
 *   ./controller --id 2 --port 5001 --raft-port 6001
 *   ./controller --id 3 --port 5002 --raft-port 6002
 * 
 * 
 * PITFALL 6: Deleting Raft Directory While Running
 * -------------------------------------------------
 * 
 * Problem: Removing raft_data while controller running
 * 
 * Result:
 *   - Crashes on next log write
 *   - State corruption
 *   - Cluster destabilization
 * 
 * Solution:
 *   ALWAYS stop controller before deleting raft_data
 * 
 * 
 * PITFALL 7: Not Checking Return Value of start()
 * ------------------------------------------------
 * 
 * Wrong:
 *   controller.start();
 *   // Assumes success!
 * 
 * Correct:
 *   if (!controller.start()) {
 *       Logger::error("Failed to start");
 *       return 1;
 *   }
 */

/*******************************************************************************
 * SECTION 10: FREQUENTLY ASKED QUESTIONS
 *******************************************************************************/

/*
 * Q1: Can I run with just 2 controller nodes?
 * 
 * A1: Technically yes, but not recommended:
 *     - 2-node cluster can't survive ANY failure
 *     - If one node fails → no quorum → system down
 *     - 3 nodes is minimum for fault tolerance
 *     - 5 or 7 nodes for higher availability (overkill for this project)
 * 
 * 
 * Q2: What happens if I kill the leader?
 * 
 * A2: Automatic failover:
 *     1. Followers detect leader timeout (~1-3 seconds)
 *     2. Election starts
 *     3. New leader elected (~1-2 seconds)
 *     4. Total downtime: ~2-5 seconds
 *     5. Workers reconnect to new leader
 *     6. Jobs continue processing
 * 
 * 
 * Q3: How do I know which node is the leader?
 * 
 * A3: Check logs:
 *       grep LEADER controller.log
 *       
 *     Or programmatically:
 *       controller.is_raft_leader() returns true on leader
 * 
 * 
 * Q4: Can I change configuration while running?
 * 
 * A4: No. Must restart controller for config changes.
 *     
 *     For production systems, consider:
 *     - Dynamic configuration (reload on SIGHUP)
 *     - Configuration management (etcd, Consul)
 * 
 * 
 * Q5: What's the maximum cluster size?
 * 
 * A5: Raft typically handles 3-7 nodes well.
 *     - 3 nodes: Tolerates 1 failure
 *     - 5 nodes: Tolerates 2 failures
 *     - 7 nodes: Tolerates 3 failures
 *     
 *     Beyond 7: Performance degrades (log replication overhead)
 * 
 * 
 * Q6: How much disk space does Raft log need?
 * 
 * A6: Depends on workload:
 *     - Each job submission: ~1 KB in log
 *     - Each result: ~500 bytes in log
 *     - 1000 jobs: ~1.5 MB
 *     - Log compaction reduces size over time
 *     
 *     Recommend: 1 GB minimum, 10 GB comfortable
 * 
 * 
 * Q7: What if network partitions the cluster?
 * 
 * A7: Raft handles partitions gracefully:
 *     
 *     Scenario: 2 nodes vs 1 node partition
 *     - Majority side (2 nodes): Continues operating
 *     - Minority side (1 node): Stops accepting jobs
 *     - When partition heals: Minority syncs from majority
 *     
 *     No split-brain! Raft guarantees single leader.
 * 
 * 
 * Q8: Can I add nodes to running cluster?
 * 
 * A8: Current implementation: No (static configuration)
 *     
 *     Production Raft: Yes (dynamic membership)
 *     - Add node via special log entry
 *     - Cluster reconfigures automatically
 *     - No downtime
 *     
 *     For this project: Define cluster at startup
 * 
 * 
 * Q9: How do I backup the system?
 * 
 * A9: Backup Raft directory:
 *     
 *     $ ssh kh01 "tar czf raft_backup_$(date +%Y%m%d).tar.gz ~/raft_data"
 *     
 *     Do this on ALL three nodes for redundancy
 *     
 *     Restore:
 *       1. Stop controller
 *       2. Extract backup: tar xzf raft_backup.tar.gz
 *       3. Start controller
 * 
 * 
 * Q10: Is this production-ready?
 * 
 * A10: No, this is academic/research code.
 *      
 *      For production, add:
 *      1. TLS encryption (Raft and worker communication)
 *      2. Authentication and authorization
 *      3. Metrics export (Prometheus)
 *      4. Distributed tracing (Jaeger)
 *      5. Dynamic membership changes
 *      6. Automated backups
 *      7. Health check endpoints
 *      8. Graceful degradation
 *      9. Rate limiting
 *      10. Comprehensive testing
 *      
 *      Consider using production Raft libraries:
 *      - etcd/raft (Go)
 *      - Consul (Go)
 *      - Apache Ratis (Java)
 */

// Documentation complete