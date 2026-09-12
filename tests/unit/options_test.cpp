// Argument parsing and the exit-code contract. No engine and no process here:
// parse_args is a pure mapping from text to a decision, which is why it takes
// already-split views rather than argc/argv.

#include "taskengine/cli/options.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace {

using taskengine::exit_code_for;
using taskengine::kExitSuccess;
using taskengine::kExitTaskFailure;
using taskengine::kExitUsage;
using taskengine::Options;
using taskengine::parse_args;
using taskengine::ParseResult;

ParseResult parse(const std::vector<std::string_view>& args) { return parse_args(args); }

// Asserts a failure and returns the message, so each test can check that the
// diagnostic actually names the problem rather than merely being non-empty.
std::string failure_message(const std::vector<std::string_view>& args) {
    const ParseResult result = parse(args);
    EXPECT_EQ(result.action, ParseResult::Action::Fail);
    EXPECT_FALSE(result.error.empty());
    return result.error;
}

// --- defaults ---------------------------------------------------------------

TEST(ParseArgs, NoArgumentsIsAValidRunWithDefaults) {
    const ParseResult result = parse({});
    ASSERT_EQ(result.action, ParseResult::Action::Run);
    EXPECT_EQ(result.options.workers, taskengine::default_worker_count());
    EXPECT_EQ(result.options.tasks, 1000u);
    EXPECT_EQ(result.options.work, 1000u);
    EXPECT_EQ(result.options.queue_capacity, 1024u);
    EXPECT_EQ(result.options.fail_every, 0u);
}

TEST(ParseArgs, DefaultWorkerCountIsAtLeastOne) {
    // hardware_concurrency is allowed to return 0 when it cannot tell. The
    // default must never be a value the engine would reject.
    EXPECT_GE(taskengine::default_worker_count(), 1u);
}

// --- each option ------------------------------------------------------------

TEST(ParseArgs, AcceptsEveryOption) {
    const ParseResult result = parse({"--workers", "4", "--tasks", "50", "--work", "7",
                                      "--queue-capacity", "16", "--fail-every", "5"});
    ASSERT_EQ(result.action, ParseResult::Action::Run);
    EXPECT_EQ(result.options.workers, 4u);
    EXPECT_EQ(result.options.tasks, 50u);
    EXPECT_EQ(result.options.work, 7u);
    EXPECT_EQ(result.options.queue_capacity, 16u);
    EXPECT_EQ(result.options.fail_every, 5u);
}

TEST(ParseArgs, OptionOrderDoesNotMatter) {
    const ParseResult a = parse({"--workers", "2", "--tasks", "9"});
    const ParseResult b = parse({"--tasks", "9", "--workers", "2"});
    ASSERT_EQ(a.action, ParseResult::Action::Run);
    ASSERT_EQ(b.action, ParseResult::Action::Run);
    EXPECT_EQ(a.options.workers, b.options.workers);
    EXPECT_EQ(a.options.tasks, b.options.tasks);
}

TEST(ParseArgs, ARepeatedOptionTakesTheLastValue) {
    const ParseResult result = parse({"--workers", "2", "--workers", "8"});
    ASSERT_EQ(result.action, ParseResult::Action::Run);
    EXPECT_EQ(result.options.workers, 8u);
}

TEST(ParseArgs, ZeroIsAcceptedWhereItIsMeaningful) {
    // --work 0 submits empty tasks, which is how the engine overhead floor gets
    // measured. --tasks 0 is an empty but well-defined run. Neither is a typo
    // the parser should reject on the users behalf.
    const ParseResult result = parse({"--work", "0", "--tasks", "0", "--fail-every", "0"});
    ASSERT_EQ(result.action, ParseResult::Action::Run);
    EXPECT_EQ(result.options.work, 0u);
    EXPECT_EQ(result.options.tasks, 0u);
    EXPECT_EQ(result.options.fail_every, 0u);
}

// --- help and version -------------------------------------------------------

TEST(ParseArgs, HelpIsRecognisedInBothForms) {
    EXPECT_EQ(parse({"-h"}).action, ParseResult::Action::ShowHelp);
    EXPECT_EQ(parse({"--help"}).action, ParseResult::Action::ShowHelp);
}

TEST(ParseArgs, HelpWinsOverOtherArgumentsAndIsNeverAnError) {
    // Somebody asking for help while also having typed something wrong should
    // get the help, not a complaint.
    EXPECT_EQ(parse({"--help", "--nonsense"}).action, ParseResult::Action::ShowHelp);
    EXPECT_EQ(parse({"--workers", "4", "--help"}).action, ParseResult::Action::ShowHelp);
}

TEST(ParseArgs, VersionIsRecognised) {
    EXPECT_EQ(parse({"--version"}).action, ParseResult::Action::ShowVersion);
}

TEST(Usage, MentionsEveryOptionAndEveryExitCode) {
    const std::string text{taskengine::usage()};
    for (const char* option : {"--workers", "--tasks", "--work", "--queue-capacity",
                               "--fail-every", "--help", "--version"}) {
        EXPECT_NE(text.find(option), std::string::npos) << "help text omits " << option;
    }
    // The exit-code contract is part of the interface, so it belongs in --help
    // rather than only in a document nobody reads at the prompt.
    EXPECT_NE(text.find("Exit codes"), std::string::npos);
    EXPECT_NE(text.find("\n  0 "), std::string::npos);
    EXPECT_NE(text.find("\n  1 "), std::string::npos);
    EXPECT_NE(text.find("\n  2 "), std::string::npos);
}

// --- rejection --------------------------------------------------------------

TEST(ParseArgs, RejectsAnUnknownOption) {
    const std::string message = failure_message({"--turbo", "1"});
    EXPECT_NE(message.find("--turbo"), std::string::npos) << message;
}

TEST(ParseArgs, RejectsAPositionalArgument) {
    // There are no positional arguments. A stray word is far more likely a
    // mistake than something to be ignored.
    const std::string message = failure_message({"run"});
    EXPECT_NE(message.find("run"), std::string::npos) << message;
}

TEST(ParseArgs, RejectsAMissingValue) {
    const std::string message = failure_message({"--workers"});
    EXPECT_NE(message.find("--workers"), std::string::npos) << message;
    EXPECT_NE(message.find("value"), std::string::npos) << message;
}

TEST(ParseArgs, RejectsANonNumericValue) {
    EXPECT_NE(failure_message({"--workers", "eight"}).find("--workers"), std::string::npos);
    EXPECT_NE(failure_message({"--tasks", ""}).find("--tasks"), std::string::npos);
}

TEST(ParseArgs, RejectsTrailingRubbishRatherThanSilentlyTruncating) {
    // "12abc" must not quietly become 12. This is the difference between
    // from_chars with an end-pointer check and a naive conversion.
    const std::string message = failure_message({"--tasks", "12abc"});
    EXPECT_NE(message.find("12abc"), std::string::npos) << message;
}

TEST(ParseArgs, RejectsANegativeValue) {
    const std::string message = failure_message({"--workers", "-1"});
    EXPECT_NE(message.find("-1"), std::string::npos) << message;
}

TEST(ParseArgs, RejectsAValueTooLargeToRepresent) {
    // Overflow has to be an error, not a wrapped-around small number.
    const std::string message = failure_message({"--tasks", "99999999999999999999999999"});
    EXPECT_NE(message.find("--tasks"), std::string::npos) << message;
}

TEST(ParseArgs, RejectsConfigurationsTheEngineCouldNotRun) {
    // Caught here rather than left to the engine constructor, so the message
    // names the flag that was typed instead of an internal precondition.
    EXPECT_NE(failure_message({"--workers", "0"}).find("--workers"), std::string::npos);
    EXPECT_NE(failure_message({"--queue-capacity", "0"}).find("--queue-capacity"),
              std::string::npos);
}

TEST(ParseArgs, AFailureCarriesNoUsableOptions) {
    const ParseResult result = parse({"--workers", "0"});
    ASSERT_EQ(result.action, ParseResult::Action::Fail);
    // Nothing downstream should be tempted to use these.
    EXPECT_EQ(result.options.workers, 0u);
}

// --- exit codes -------------------------------------------------------------

TEST(ExitCode, SuccessOnlyWhenNothingFailedOrWasRejected) {
    EXPECT_EQ(exit_code_for(0, 0), kExitSuccess);
}

TEST(ExitCode, AnyFailureOrRejectionIsNotSuccess) {
    // A partially successful run is never reported as success.
    EXPECT_EQ(exit_code_for(1, 0), kExitTaskFailure);
    EXPECT_EQ(exit_code_for(0, 1), kExitTaskFailure);
    EXPECT_EQ(exit_code_for(1, 1), kExitTaskFailure);
    EXPECT_EQ(exit_code_for(9999, 0), kExitTaskFailure);
}

TEST(ExitCode, TheThreeCodesAreDistinct) {
    EXPECT_NE(kExitSuccess, kExitTaskFailure);
    EXPECT_NE(kExitTaskFailure, kExitUsage);
    EXPECT_NE(kExitSuccess, kExitUsage);
    EXPECT_EQ(kExitSuccess, 0);
    EXPECT_EQ(kExitTaskFailure, 1);
    EXPECT_EQ(kExitUsage, 2);
}

}  // namespace
