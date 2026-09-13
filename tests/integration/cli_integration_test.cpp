// End-to-end tests of the task-engine binary: the exit-code contract, where
// output goes, and graceful handling of SIGINT and SIGTERM.
//
// These run the real executable in a child process. The unit suite already
// covers parse_args and exit_code_for as functions; what only a process can
// show is that main() wires them together correctly, that diagnostics reach
// stderr and results reach stdout, and that a signal produces an orderly,
// truthfully reported stop rather than a dead process.
//
// POSIX process APIs are used directly. The project is Linux-first (D14), and
// these are the plainest tools for the job.

#include <gtest/gtest.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <string>
#include <utility>

namespace {

// Supplied by CMake as the path of the built executable.
constexpr const char* kBinary = TASKENGINE_CLI_PATH;

struct ProcessResult {
    int exit_code{-1};
    std::string output;
};

// Runs the binary through the shell with the given argument string and
// redirection, capturing whatever the redirection sends to our pipe.
ProcessResult run_shell(const std::string& arguments, const std::string& redirection) {
    const std::string command = std::string{kBinary} + " " + arguments + " " + redirection;
    ProcessResult result;

    FILE* const pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        ADD_FAILURE() << "popen failed for: " << command;
        return result;
    }
    std::array<char, 4096> buffer{};
    std::size_t bytes = 0;
    while ((bytes = std::fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        result.output.append(buffer.data(), bytes);
    }
    const int status = ::pclose(pipe);
    if (status != -1 && WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    }
    return result;
}

ProcessResult run_stdout_only(const std::string& arguments) {
    return run_shell(arguments, "2>/dev/null");
}

ProcessResult run_stderr_only(const std::string& arguments) {
    return run_shell(arguments, "2>&1 >/dev/null");
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// --- successful paths ---------------------------------------------------------

TEST(CliProcess, HelpExitsZeroAndStatesTheExitCodeContract) {
    const ProcessResult result = run_stdout_only("--help");
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_TRUE(contains(result.output, "Exit codes"));
    EXPECT_TRUE(contains(result.output, "--fail-every"));
}

TEST(CliProcess, VersionExitsZero) {
    const ProcessResult result = run_stdout_only("--version");
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_TRUE(contains(result.output, "task-engine "));
}

TEST(CliProcess, ACleanRunExitsZeroAndReportsEveryTaskSucceeded) {
    const ProcessResult result = run_stdout_only("--workers 4 --tasks 2000 --work 100");
    EXPECT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(contains(result.output, "submitted        2000"));
    EXPECT_TRUE(contains(result.output, "succeeded        2000"));
    EXPECT_TRUE(contains(result.output, "failed           0"));
    EXPECT_TRUE(contains(result.output, "rejected         0"));
}

TEST(CliProcess, AnEmptyRunIsASuccess) {
    const ProcessResult result = run_stdout_only("--tasks 0");
    EXPECT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(contains(result.output, "submitted        0"));
}

TEST(CliProcess, AnOverheadRunWithNoWorkIsASuccess) {
    const ProcessResult result = run_stdout_only("--workers 1 --tasks 5000 --work 0");
    EXPECT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(contains(result.output, "succeeded        5000"));
}

// --- failure paths ------------------------------------------------------------

TEST(CliProcess, InjectedFailuresExitOneWithAnExactCount) {
    // Every 10th of 100 tasks throws: exactly 10 failures, and a partially
    // successful run is not reported as success.
    const ProcessResult result = run_stdout_only("--workers 4 --tasks 100 --work 10 --fail-every 10");
    EXPECT_EQ(result.exit_code, 1) << result.output;
    EXPECT_TRUE(contains(result.output, "succeeded        90"));
    EXPECT_TRUE(contains(result.output, "failed           10"));
}

TEST(CliProcess, ARunWhereEveryTaskFailsExitsOne) {
    const ProcessResult result = run_stdout_only("--tasks 50 --fail-every 1");
    EXPECT_EQ(result.exit_code, 1) << result.output;
    EXPECT_TRUE(contains(result.output, "succeeded        0"));
    EXPECT_TRUE(contains(result.output, "failed           50"));
}

TEST(CliProcess, EveryInvalidInputExitsTwo) {
    const char* const cases[] = {
        "--workers 0",       "--queue-capacity 0", "--turbo 1",   "run",
        "--workers",         "--workers eight",    "--tasks 12abc", "--workers -1",
        "--tasks 99999999999999999999999999",       "-x",
    };
    for (const char* arguments : cases) {
        const ProcessResult result = run_stdout_only(arguments);
        EXPECT_EQ(result.exit_code, 2) << "arguments: " << arguments;
    }
}

TEST(CliProcess, AUsageErrorWritesNothingToStdoutAndExplainsItselfOnStderr) {
    // A caller redirecting stdout must still learn why nothing came out of it.
    const ProcessResult stdout_side = run_stdout_only("--workers 0");
    EXPECT_EQ(stdout_side.exit_code, 2);
    EXPECT_TRUE(stdout_side.output.empty()) << stdout_side.output;

    const ProcessResult stderr_side = run_stderr_only("--workers 0");
    EXPECT_EQ(stderr_side.exit_code, 2);
    EXPECT_TRUE(contains(stderr_side.output, "--workers must be at least 1")) << stderr_side.output;
}

// --- interrupted runs ---------------------------------------------------------

// Starts a long run in a child process, waits until the child announces that it
// is running, delivers the signal, and collects the exit status and output.
//
// Deterministic by construction. The child installs its signal handlers before
// printing "running..." and flushes that line, so the parent reading it is proof
// the handlers are in place. The parent never guesses at timing.
ProcessResult interrupt_long_run(int signal_number) {
    ProcessResult result;

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        ADD_FAILURE() << "pipe failed";
        return result;
    }

    const pid_t child = ::fork();
    if (child < 0) {
        ADD_FAILURE() << "fork failed";
        return result;
    }

    if (child == 0) {
        // Child. Only async-signal-safe calls between fork and exec, since the
        // parent is a multithreaded test process.
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[1]);
        // Far more work than can finish before the signal arrives.
        ::execl(kBinary, kBinary, "--workers", "2", "--tasks", "1000000000", "--work", "2000",
                static_cast<char*>(nullptr));
        ::_exit(127);  // exec failed
    }

    ::close(fds[1]);

    std::array<char, 4096> buffer{};
    bool signalled = false;
    for (;;) {
        const ssize_t bytes = ::read(fds[0], buffer.data(), buffer.size());
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        if (bytes <= 0) {
            break;  // end of output: the child has exited
        }
        result.output.append(buffer.data(), static_cast<std::size_t>(bytes));
        if (!signalled && contains(result.output, "running...")) {
            ::kill(child, signal_number);
            signalled = true;
        }
    }
    ::close(fds[0]);

    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }

    EXPECT_TRUE(signalled) << "the child never reported that it was running";
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        ADD_FAILURE() << "child was killed by signal " << WTERMSIG(status)
                      << " instead of shutting down gracefully";
    }
    return result;
}

TEST(CliProcess, SigintStopsTheRunGracefullyAndReportsItTruthfully) {
    const ProcessResult result = interrupt_long_run(SIGINT);

    // Exit code 1: the run did not do what was asked, so it is not a success.
    EXPECT_EQ(result.exit_code, 1) << result.output;
    // The process shut down and reported, rather than dying mid-run.
    EXPECT_TRUE(contains(result.output, "interrupted      yes")) << result.output;
    EXPECT_TRUE(contains(result.output, "outcome")) << result.output;
    // Nowhere near the billion tasks requested were submitted.
    EXPECT_FALSE(contains(result.output, "submitted        1000000000")) << result.output;
}

TEST(CliProcess, SigtermIsHandledTheSameWay) {
    const ProcessResult result = interrupt_long_run(SIGTERM);
    EXPECT_EQ(result.exit_code, 1) << result.output;
    EXPECT_TRUE(contains(result.output, "interrupted      yes")) << result.output;
}

}  // namespace
