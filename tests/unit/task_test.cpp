#include "taskengine/core/task.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using taskengine::Task;

// Records executions and destructions so lifetime can be asserted rather than
// assumed. Holds references to counters owned by the test, so the counters
// outlive the task.
class CountingTask final : public Task {
public:
    CountingTask(int& executions, int& destructions) noexcept
        : executions_(executions), destructions_(destructions) {}

    ~CountingTask() override { ++destructions_; }

    void execute() override { ++executions_; }

private:
    int& executions_;
    int& destructions_;
};

class ThrowingTask final : public Task {
public:
    void execute() override { throw std::runtime_error("boom"); }
};

// Appends its id on destruction, so destruction order is observable.
class OrderedTask final : public Task {
public:
    OrderedTask(std::vector<int>& log, int id) noexcept : log_(log), id_(id) {}

    ~OrderedTask() override { log_.push_back(id_); }

    void execute() override {}

private:
    std::vector<int>& log_;
    int id_;
};

// --- compile-time properties ------------------------------------------------

// A polymorphic base must be destructible through a base pointer.
static_assert(std::has_virtual_destructor_v<Task>);

// Copy and move assignment are protected, so code holding a Task& cannot assign
// one object over another and slice off the derived part.
static_assert(!std::is_copy_assignable_v<Task>);
static_assert(!std::is_move_assignable_v<Task>);

// A derived type can still copy and move itself: the protected members are
// accessible to it.
static_assert(std::is_move_constructible_v<ThrowingTask>);
static_assert(std::is_copy_constructible_v<ThrowingTask>);

// Task is an interface, not a value. It cannot be instantiated on its own.
static_assert(std::is_abstract_v<Task>);

// --- tests ------------------------------------------------------------------

TEST(Task, ExecuteDispatchesThroughABasePointer) {
    int executions = 0;
    int destructions = 0;
    const std::unique_ptr<Task> task =
        std::make_unique<CountingTask>(executions, destructions);

    task->execute();
    task->execute();

    EXPECT_EQ(executions, 2);
}

TEST(Task, DestroyingThroughABasePointerRunsTheDerivedDestructor) {
    int executions = 0;
    int destructions = 0;
    {
        const std::unique_ptr<Task> task =
            std::make_unique<CountingTask>(executions, destructions);
        EXPECT_EQ(destructions, 0);
    }
    // Without a virtual destructor this would be undefined behaviour and the
    // derived destructor would not run.
    EXPECT_EQ(destructions, 1);
}

TEST(Task, MovingOwnershipDestroysExactlyOnce) {
    int executions = 0;
    int destructions = 0;
    {
        std::unique_ptr<Task> source =
            std::make_unique<CountingTask>(executions, destructions);
        const std::unique_ptr<Task> sink = std::move(source);

        EXPECT_EQ(source, nullptr);  // a moved-from unique_ptr is empty
        ASSERT_NE(sink, nullptr);
        sink->execute();
        EXPECT_EQ(destructions, 0);
    }
    EXPECT_EQ(destructions, 1);  // one destruction, not two and not zero
    EXPECT_EQ(executions, 1);
}

TEST(Task, ResettingAnOwnerDestroysTheTaskImmediately) {
    int executions = 0;
    int destructions = 0;
    std::unique_ptr<Task> task =
        std::make_unique<CountingTask>(executions, destructions);

    task.reset();

    EXPECT_EQ(destructions, 1);
    EXPECT_EQ(task, nullptr);
}

TEST(Task, ExecuteIsAllowedToThrow) {
    // The engine is what catches this and turns it into a Failed result. At
    // this level the contract is only that execute() may throw.
    const std::unique_ptr<Task> task = std::make_unique<ThrowingTask>();
    EXPECT_THROW(task->execute(), std::runtime_error);
}

TEST(Task, AThrowingTaskStillDestroysCleanly) {
    int executions = 0;
    int destructions = 0;
    {
        const std::unique_ptr<Task> thrower = std::make_unique<ThrowingTask>();
        const std::unique_ptr<Task> counter =
            std::make_unique<CountingTask>(executions, destructions);
        EXPECT_THROW(thrower->execute(), std::runtime_error);
    }
    EXPECT_EQ(destructions, 1);
}

TEST(Task, AutomaticObjectsAreDestroyedInReverseOrderOfConstruction) {
    std::vector<int> log;
    {
        const OrderedTask first{log, 1};
        const OrderedTask second{log, 2};
        const OrderedTask third{log, 3};
        EXPECT_TRUE(log.empty());
    }
    EXPECT_EQ(log, (std::vector<int>{3, 2, 1}));
}

TEST(Task, OwnersInAContainerEachDestroyTheirTask) {
    int executions = 0;
    int destructions = 0;
    {
        std::vector<std::unique_ptr<Task>> tasks;
        tasks.reserve(3);
        for (int i = 0; i < 3; ++i) {
            tasks.push_back(std::make_unique<CountingTask>(executions, destructions));
        }
        for (const auto& task : tasks) {
            task->execute();
        }
        EXPECT_EQ(executions, 3);
        EXPECT_EQ(destructions, 0);
    }
    EXPECT_EQ(destructions, 3);
}

}  // namespace
