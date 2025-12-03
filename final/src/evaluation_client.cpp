/*******************************************************************************
    Project: Distributed Financial Backtesting & Risk Engine
    Authors: Talif Pathan, Mohamed Samir Shafat Khan
    Course: CS6650 Scalable Distributed Systems SEC 04 Fall 2025 [BOS-1-TR]
    Date: November 27, 2025
    
    File: evaluation_client.cpp
    
    Description:
        This file implements a TCP-based evaluation client that can connect to
        a running controller instance, register itself using the existing
        worker-registration protocol, and load a batch of backtesting jobs
        from a CSV configuration file.

        In its current form, the evaluation client focuses on:
          - Demonstrating how an external process connects to the controller
          - Reusing the Message/Logger infrastructure from the core system
          - Parsing a jobs.csv file into structured JobConfig entries
          - Providing a clean command-line interface and usage guidance
          - Serving as a skeleton for future extensions where jobs are
            submitted to the controller and results are collected and
            persisted (e.g., to results.csv)

        It is intended as a foundation for more advanced evaluation and
        benchmarking tools (such as benchmark_client.cpp), where the protocol
        would be extended to support:
          - Submitting JobRequest messages to the controller
          - Receiving JobResult messages asynchronously
          - Measuring end-to-end latency and throughput
          - Writing evaluation outputs for further analysis.

    Key Responsibilities:
        1. Command-line Interface:
           - Parse host, port, job configuration file, output file path, and
             timeout from command-line arguments.
           - Provide a user-friendly --help flag with usage instructions.

        2. Network Connectivity:
           - Establish a TCP connection to the controller using POSIX sockets.
           - Resolve hostnames via gethostbyname() and connect using IPv4.

        3. Protocol Integration:
           - Register as a "client" by reusing the worker registration
             message format (WorkerRegisterMessage), and receive a
             WORKER_REGISTER_ACK response with an assigned client_id.

        4. Job Configuration Loading:
           - Read a jobs CSV file (jobs.csv by default).
           - Map each row into a strongly-typed JobConfig struct, including
             symbol, strategy, date range, moving-average windows, and
             initial capital.

        5. Developer-Facing Demonstration:
           - Print the loaded jobs and explain that this program currently
             demonstrates the interface and is meant to be extended for
             full job submission and result handling.

    Project Evaluation Context:
        - This client is not directly responsible for the graded evaluation
          experiments (E1–E6), but serves as:
              * A reference implementation of how to talk to the controller
                from an external process.
              * A starting point for building more sophisticated evaluation
                clients that submit real workloads and collect results.
              * A debugging tool when verifying that the controller’s TCP
                interface and message framing behave as expected.

        - Advanced usage could include:
              * Creating scripted evaluation scenarios.
              * Comparing different job mixes by swapping CSV job files.
              * Integrating with benchmark scripts to automate experiments.

    Integration Points:
        - Controller:
            * Connects via TCP to the same host/port used by workers.
            * Uses the same Message and Logger infrastructure shared by the
              distributed system.

        - Jobs CSV:
            * Reads job definitions from a CSV file (jobs.csv by default).
            * Expected columns:
                symbol,strategy,start_date,end_date,short_window,long_window,initial_capital

        - Output:
            * Currently, output_file is parsed and logged but not yet written.
            * Intended for future extensions where job results and metrics
              are persisted to results.csv for plotting (e.g., in Python).

        - Test Suite:
            * Works in tandem with test_integration and test_full_system,
              which demonstrate job submission from within the same process.
            * This file demonstrates how to perform similar logic from a
              separate standalone client.

    Current Status:
        - Fully implemented:
            * TCP connection / registration handshake.
            * Jobs CSV loading and JobConfig construction.
            * Robust command-line parsing and usage output.
            * Logging of key configuration parameters.

        - Intentionally incomplete:
            * Actual job submission to the controller is not yet wired up.
            * JobResult handling is implemented at the message level but
              not yet used by main().
            * Writing results.csv is not yet implemented.

        - For full end-to-end evaluation and grading, use:
            * test_integration
            * test_full_system
          as these already exercise job submission and processing.

    Design Philosophy:
        - Reuse existing infrastructure:
            * Uses the same Message, Logger, and enum types as the rest of
              the system to avoid duplication.

        - Clarity over cleverness:
            * Straightforward control flow and explicit logging for each step.

        - Extensibility:
            * EvaluationClient is designed to be easily extended with new
              message types and job/result handling logic.

*******************************************************************************/

/*==============================================================================
 * TABLE OF CONTENTS
 *==============================================================================
 *
 * 1. High-Level Overview
 *    1.1 Role of evaluation_client in the project
 *    1.2 High-level flow of main()
 *
 * 2. Core Types
 *    2.1 EvaluationClient class
 *        - Members
 *        - connect()
 *        - disconnect()
 *        - send_message()
 *        - receive_message()
 *    2.2 JobConfig struct
 *        - Fields and semantics
 *
 * 3. Utility Functions
 *    3.1 load_jobs()
 *    3.2 print_usage()
 *
 * 4. Program Entry Point (main)
 *    4.1 Argument parsing
 *    4.2 Logging and configuration
 *    4.3 Job loading and demonstration output
 *
 * 5. Usage Examples
 *    5.1 Basic run against local controller
 *    5.2 Custom jobs file and output path
 *
 * 6. Prerequisites and Dependencies
 *    6.1 Build requirements
 *    6.2 Runtime expectations
 *
 * 7. Potential Pitfalls and Common Mistakes
 *
 * 8. FAQ (Frequently Asked Questions)
 *
 * 9. Extension Ideas and Best Practices
 *
 *============================================================================*/

#include "common/message.h"
#include "common/logger.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>

using namespace backtesting;

/*==============================================================================
 * 1. HIGH-LEVEL OVERVIEW
 *==============================================================================
 *
 * 1.1 Role of evaluation_client in the project
 * -------------------------------------------
 * This program is a standalone, TCP-based client for the distributed
 * backtesting system. While workers are long-lived processes that register
 * with the controller and execute jobs, this client is intended as a
 * "front-end" that:
 *
 *   - Connects to the controller like any other node.
 *   - Registers itself (reusing the worker registration message).
 *   - Loads a batch of jobs from a CSV file.
 *   - (Currently) prints those jobs for inspection and hints at how they
 *     could be submitted to the controller.
 *
 * In practice, this is a skeleton that can be evolved into:
 *   - A benchmarking client that programmatically submits jobs and collects
 *     JobResult messages.
 *   - A small UI or CLI tool for ad-hoc backtests.
 *
 * 1.2 High-level flow of main()
 * -----------------------------
 *   - Parse command-line arguments (host, port, jobs file, output file,
 *     timeout, help).
 *   - Configure logging and print the effective configuration.
 *   - Load JobConfig entries from the jobs CSV file.
 *   - If no jobs are found, exit with an error.
 *   - (Currently) print a summary of jobs and indicate that job submission
 *     is handled by existing test binaries.
 *
 *============================================================================*/


/*==============================================================================
 * 2. CORE TYPES
 *==============================================================================
 *
 * 2.1 EvaluationClient class
 * --------------------------
 * Encapsulates the networking logic required to:
 *   - Open a TCP socket.
 *   - Resolve a hostname.
 *   - Connect to the controller.
 *   - Register and receive an assigned client_id.
 *   - Send/receive framed Message objects.
 *
 * This abstraction keeps main() clean and also makes it easy to reuse or
 * extend this client in other tools.
 */

class EvaluationClient {
private:
    std::string host_;     // Controller hostname or IP
    uint16_t    port_;     // Controller TCP port
    int         socket_fd_; // Underlying socket file descriptor
    uint64_t    client_id_; // Assigned id after registration
    
public:
    EvaluationClient(const std::string& host, uint16_t port)
        : host_(host), port_(port), socket_fd_(-1), client_id_(0) {}
    
    ~EvaluationClient() { disconnect(); }
    
    /**
     * Establish a TCP connection and register this client.
     *
     * Steps:
     *   1. Create a socket(AF_INET, SOCK_STREAM).
     *   2. Resolve host_ via gethostbyname().
     *   3. Fill in sockaddr_in and call ::connect().
     *   4. Send a WorkerRegisterMessage with a synthetic hostname
     *      ("eval_client_<pid>") and port 0.
     *   5. Wait for WORKER_REGISTER_ACK, extract assigned id.
     *
     * Returns:
     *   true  on success (socket_fd_ is valid and client_id_ is set).
     *   false on any failure (socket is closed and error is logged).
     */
    bool connect() {
        socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd_ < 0) {
            Logger::error("Failed to create socket");
            return false;
        }
        
        struct hostent* host = gethostbyname(host_.c_str());
        if (!host) {
            Logger::error("Failed to resolve hostname: " + host_);
            close(socket_fd_);
            return false;
        }
        
        struct sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_);
        std::memcpy(&server_addr.sin_addr, host->h_addr, host->h_length);
        
        if (::connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            Logger::error("Failed to connect: " + std::string(strerror(errno)));
            close(socket_fd_);
            return false;
        }
        
        // Register as a "client" (using worker registration for simplicity).
        // This keeps protocol changes minimal at the cost of slightly
        // overloading the semantics of "worker".
        WorkerRegisterMessage reg;
        reg.worker_hostname = "eval_client_" + std::to_string(getpid());
        reg.worker_port = 0;
        
        if (!send_message(reg)) {
            Logger::error("Failed to send registration");
            return false;
        }
        
        auto resp = receive_message();
        if (!resp || resp->type != MessageType::WORKER_REGISTER_ACK) {
            Logger::error("Registration failed");
            return false;
        }
        
        client_id_ = resp->id;
        Logger::info("Connected to controller, client_id=" + std::to_string(client_id_));
        return true;
    }
    
    /**
     * Cleanly shut down the TCP connection, if open.
     */
    void disconnect() {
        if (socket_fd_ >= 0) {
            shutdown(socket_fd_, SHUT_RDWR);
            close(socket_fd_);
            socket_fd_ = -1;
        }
    }
    
    /**
     * Serialize and send a Message over the TCP connection.
     *
     * Wire format:
     *   [4-byte length prefix in network byte order]
     *   [raw serialized message bytes]
     *
     * Parameters:
     *   msg : Message& - Any Message subtype that implements serialize().
     *
     * Returns:
     *   true  if both length and payload are fully written.
     *   false on partial or failed send.
     */
    bool send_message(const Message& msg) {
        auto data = msg.serialize();
        uint32_t size = hton32(static_cast<uint32_t>(data.size()));
        
        if (send(socket_fd_, &size, sizeof(size), MSG_NOSIGNAL) != sizeof(size))
            return false;
        
        return send(socket_fd_, data.data(), data.size(), MSG_NOSIGNAL) == 
               static_cast<ssize_t>(data.size());
    }
    
    /**
     * Receive the next message from the controller.
     *
     * Steps:
     *   1. Read a 4-byte size prefix (blocking).
     *   2. Validate that size is sane (non-zero, <= 1 MB).
     *   3. Read 'size' bytes into a buffer.
     *   4. Inspect the first byte as MessageType and dispatch to the
     *      appropriate deserializer:
     *         - JOB_RESULT            -> JobResultMessage::deserialize()
     *         - WORKER_REGISTER_ACK   -> Minimal Message with id field set
     *
     * Returns:
     *   unique_ptr<Message> on success, nullptr on error or unknown type.
     */
    std::unique_ptr<Message> receive_message() {
        uint32_t size;
        if (recv(socket_fd_, &size, sizeof(size), MSG_WAITALL) != sizeof(size))
            return nullptr;
        
        size = ntoh32(size);
        if (size == 0 || size > 1024 * 1024) return nullptr;
        
        std::vector<uint8_t> data(size);
        if (recv(socket_fd_, data.data(), size, MSG_WAITALL) != static_cast<ssize_t>(size))
            return nullptr;
        
        MessageType type = static_cast<MessageType>(data[0]);
        
        switch (type) {
            case MessageType::JOB_RESULT:
                return JobResultMessage::deserialize(data.data(), data.size());
            case MessageType::WORKER_REGISTER_ACK: {
                auto msg = std::make_unique<Message>(MessageType::WORKER_REGISTER_ACK);
                if (data.size() >= 9) {
                    std::memcpy(&msg->id, data.data() + 1, 8);
                    msg->id = ntoh64(msg->id);
                }
                return msg;
            }
            default:
                return nullptr;
        }
    }
};


/**
 * 2.2 JobConfig struct
 * --------------------
 * Represents a single backtesting job loaded from the jobs CSV file.
 *
 * Fields:
 *   - symbol          : Ticker symbol (e.g., "AAPL")
 *   - strategy        : Strategy identifier (e.g., "SMA_CROSSOVER")
 *   - start_date      : Backtest start date (string format, e.g., "2018-01-01")
 *   - end_date        : Backtest end date (string format, e.g., "2019-01-01")
 *   - short_window    : Short moving-average window (e.g., 20)
 *   - long_window     : Long moving-average window (e.g., 50)
 *   - initial_capital : Initial capital allocated to the strategy
 */
struct JobConfig {
    std::string symbol;
    std::string strategy;
    std::string start_date;
    std::string end_date;
    int short_window;
    int long_window;
    double initial_capital;
};


/*==============================================================================
 * 3. UTILITY FUNCTIONS
 *==============================================================================
 */

/**
 * load_jobs()
 * -----------
 * Load job configurations from a CSV file.
 *
 * Expected CSV format:
 *   The first line is considered a header and skipped.
 *   Each subsequent line should be:
 *
 *       symbol,strategy,start_date,end_date,short_window,long_window,initial_capital
 *
 * Example:
 *       AAPL,SMA,2018-01-01,2019-01-01,20,50,100000
 *       MSFT,SMA,2018-01-01,2019-01-01,10,30,50000
 *
 * Any row with fewer than 7 comma-separated fields is ignored.
 *
 * Parameters:
 *   config_file : Path to the jobs CSV.
 *
 * Returns:
 *   std::vector<JobConfig> with one entry per valid row.
 */
std::vector<JobConfig> load_jobs(const std::string& config_file) {
    std::vector<JobConfig> jobs;
    std::ifstream file(config_file);
    if (!file.is_open()) {
        Logger::error("Failed to open: " + config_file);
        return jobs;
    }
    
    std::string line;
    std::getline(file, line); // Skip header
    
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string token;
        std::vector<std::string> tokens;
        
        while (std::getline(ss, token, ',')) {
            tokens.push_back(token);
        }
        
        if (tokens.size() >= 7) {
            JobConfig job;
            job.symbol = tokens[0];
            job.strategy = tokens[1];
            job.start_date = tokens[2];
            job.end_date = tokens[3];
            job.short_window = std::stoi(tokens[4]);
            job.long_window = std::stoi(tokens[5]);
            job.initial_capital = std::stod(tokens[6]);
            jobs.push_back(job);
        }
    }
    return jobs;
}


/**
 * print_usage()
 * -------------
 * Print command-line usage information to stdout.
 */
void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --host HOST      Controller host (default: localhost)\n"
              << "  --port PORT      Controller port (default: 5000)\n"
              << "  --jobs FILE      Jobs CSV file (default: jobs.csv)\n"
              << "  --output FILE    Results CSV file (default: results.csv)\n"
              << "  --timeout SEC    Job timeout in seconds (default: 300)\n"
              << "  --help           Show this help\n";
}


/*==============================================================================
 * 4. PROGRAM ENTRY POINT (main)
 *==============================================================================
 *
 * 4.1 Argument parsing
 * --------------------
 * Supported flags:
 *   --host    : Controller host (e.g., localhost, linux-072)
 *   --port    : Controller port (default: 5000)
 *   --jobs    : Path to jobs CSV (default: jobs.csv)
 *   --output  : Path to results CSV (currently not written)
 *   --timeout : Per-job timeout in seconds (currently informational)
 *   --help    : Show usage and exit.
 *
 * 4.2 Logging and configuration
 * -----------------------------
 * After parsing, the program logs:
 *   - Controller host:port
 *   - Jobs file and output file
 *   - Timeout configuration
 *
 * 4.3 Job loading and demonstration output
 * ----------------------------------------
 * - load_jobs() is called to parse the jobs CSV.
 * - If no jobs are loaded, the program exits with an error.
 * - Otherwise, it prints a list of jobs that would be submitted, and informs
 *   the user that test_integration and test_full_system are the current
 *   tools for programmatic job submission.
 */

int main(int argc, char* argv[]) {
    std::string host = "localhost";
    uint16_t port = 5000;
    std::string jobs_file = "jobs.csv";
    std::string output_file = "results.csv";
    int timeout = 300;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "--jobs" && i + 1 < argc) {
            jobs_file = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            output_file = argv[++i];
        } else if (arg == "--timeout" && i + 1 < argc) {
            timeout = std::stoi(argv[++i]);
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
    }
    
    Logger::set_level(LogLevel::INFO);
    Logger::info("=== Evaluation Client ===");
    // Use all parsed arguments so we don't get unused-variable warnings
    Logger::info("Controller: " + host + ":" + std::to_string(port));
    Logger::info("Jobs file: " + jobs_file + ", output file: " + output_file);
    Logger::info("Job timeout: " + std::to_string(timeout) + " seconds");
    
    auto jobs = load_jobs(jobs_file);
    if (jobs.empty()) {
        Logger::error("No jobs loaded from " + jobs_file);
        return 1;
    }
    
    Logger::info("Loaded " + std::to_string(jobs.size()) + " jobs");
    
    // Note: This client demonstrates the interface.
    // For actual evaluation, jobs are submitted via Controller::submit_job()
    // which requires running in the same process or extending the protocol.
    
    std::cout << "\nJob submission requires integration with running controller.\n";
    std::cout << "Use test_integration or test_full_system for programmatic testing.\n";
    std::cout << "Jobs to submit:\n";
    
    for (const auto& job : jobs) {
        std::cout << "  " << job.symbol << " (" << job.strategy << ") "
                  << job.start_date << " to " << job.end_date << "\n";
    }
    
    return 0;
}


/*==============================================================================
 * 5. USAGE EXAMPLES
 *==============================================================================
 *
 * Example 1: Basic usage with defaults
 * ------------------------------------
 *   $ ./evaluation_client
 *
 * Assumes:
 *   - Controller is running on localhost:5000
 *   - jobs.csv is present in the current directory
 *
 * Example 2: Custom host, port, and jobs file
 * -------------------------------------------
 *   $ ./evaluation_client \
 *         --host=linux-072 \
 *         --port=5000     \
 *         --jobs=configs/my_jobs.csv \
 *         --output=results/my_results.csv \
 *         --timeout=600
 *
 * Example 3: Show help
 * --------------------
 *   $ ./evaluation_client --help
 *
 *==============================================================================
 * 6. PREREQUISITES AND DEPENDENCIES
 *==============================================================================
 *
 * Build-time:
 *   - C++17 or later
 *   - POSIX-compatible socket APIs (Linux, macOS, etc.)
 *   - Access to common/message.h and common/logger.h
 *
 * Run-time:
 *   - A running controller listening on the given host:port (optional for now,
 *     since this client currently does not call connect() in main()).
 *   - A valid jobs CSV with at least one row after the header.
 *
 *==============================================================================
 * 7. POTENTIAL PITFALLS AND COMMON MISTAKES
 *==============================================================================
 *
 * - Incorrect CSV format:
 *   If a row has fewer than 7 comma-separated fields, it is silently skipped.
 *   Make sure your jobs.csv follows the expected schema.
 *
 * - Invalid numeric fields:
 *   std::stoi/std::stod will throw if short_window, long_window, or
 *   initial_capital cannot be parsed. Validate your CSV values.
 *
 * - Missing jobs file:
 *   If jobs_file does not exist or cannot be opened, you will see:
 *     "Failed to open: <jobs_file>"
 *   and the client will terminate with an error.
 *
 * - Not actually submitting jobs:
 *   This client currently only prints the jobs that *would* be submitted.
 *   For real job submission, integrate with EvaluationClient::connect(),
 *   send_message(), and receive_message(), or rely on the existing test
 *   binaries.
 *
 *==============================================================================
 * 8. FAQ
 *==============================================================================
 *
 * Q1: Why is this client registering as a worker?
 * A1: To keep the protocol surface area small, we reuse the existing
 *     WorkerRegisterMessage and WORKER_REGISTER_ACK flow. Conceptually this
 *     client is "just another participant" that wants a unique id. You can
 *     introduce a dedicated CLIENT_REGISTER message type if you want stricter
 *     semantic separation later.
 *
 * Q2: Where are job results saved?
 * A2: Currently, they are not. The output_file argument is parsed and logged
 *     as a placeholder. To actually save results, you would:
 *       - Extend the protocol to send job submission messages.
 *       - Collect JobResult messages via receive_message().
 *       - Serialize them into results.csv.
 *
 * Q3: How do I submit jobs programmatically today?
 * A3: Use test_integration or test_full_system, which already run within the
 *     same process as the controller and can directly call Controller APIs.
 *
 * Q4: How could I extend this client for benchmarks?
 * A4: Typical steps:
 *       - Add a JobRequestMessage type if not already present.
 *       - In main(), create an EvaluationClient, connect() to the controller.
 *       - Loop over JobConfig entries, send job requests, and record start
 *         timestamps.
 *       - Receive JobResult messages, record completion times, and write CSV.
 *
 * Q5: Do I need this client for the CS6650 evaluation rubric?
 * A5: Not strictly. It is primarily a teaching/extension tool. The grading
 *     scripts will rely more heavily on your automated tests and benchmark
 *     scripts, but this client can be very helpful for debugging and demos.
 *
 *==============================================================================
 * 9. EXTENSION IDEAS AND BEST PRACTICES
 *==============================================================================
 *
 * - Add a --connect flag:
 *     Make it optional to connect to the controller. When enabled, use the
 *     EvaluationClient to verify registration and perform a simple handshake.
 *
 * - Introduce a dedicated "client" registration message:
 *     If you feel overloading WorkerRegisterMessage is confusing, define a
 *     ClientRegisterMessage and a corresponding ACK type to clarify roles.
 *
 * - Use structured logging for results:
 *     When you later add job submission, log job IDs and timings with enough
 *     structure (e.g., CSV or JSON) to support quick offline analysis.
 *
 * - Keep the CSV schema stable:
 *     The jobs.csv format becomes an API contract for anyone using this
 *     client. Document it clearly in your README and avoid breaking changes
 *     late in the project.
 *
 *==============================================================================*/
 