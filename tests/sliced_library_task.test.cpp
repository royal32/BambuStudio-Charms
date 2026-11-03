#include "slic3r/GUI/SlicedLibraryTask.hpp"
#include <cassert>
#include <future>
#include <iostream>

using namespace Slic3r::GUI::LibraryTasks;
using namespace std::chrono_literals;

template<class Predicate> void until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}
int main() {
    // Model an uninterruptible File Provider read. Cancelling/destruction must
    // return immediately, and its eventual result must never publish anything.
    std::promise<void> unblock, entered;
    auto block = unblock.get_future().share();
    std::atomic<int> writes{0};
    {
        Task<int> task([&](const Context& context) {
            entered.set_value();
            block.wait();
            context.begin_commit();
            ++writes;
            return 1;
        });
        entered.get_future().wait();
        const auto start = std::chrono::steady_clock::now();
        assert(task.cancel());
        assert(std::chrono::steady_clock::now() - start < 100ms);
        bool rejected = false;
        try { Task<int> duplicate([](const Context&) { return 2; }); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected); // An abandoned OS read cannot accumulate workers.
    }
    assert(writes == 0);
    unblock.set_value();
    until([] { return !file_worker_active; });
    assert(writes == 0);
    {
        Task<int> task([&](const Context& context) {
            context.await_snapshot({1, 3});
            context.report("Ready to publish");
            context.begin_commit();
            ++writes;
            return 42;
        });
        until([&] { return task.phase() == Phase::awaiting_snapshot; });
        assert(task.snapshot_indices() == std::vector<int>({1, 3}));
        assert(writes == 0);
        task.snapshot_ready();
        until([&] { return task.phase() == Phase::awaiting_commit; });
        assert(writes == 0);
        assert(task.message() == "Ready to publish");
        task.approve_commit();
        assert(!task.cancel()); // Never abandon replacement/rollback in flight.
        until([&] { return task.done(); });
        assert(task.get() == 42 && writes == 1);
    }
    until([] { return !file_worker_active; });
    {
        Task<int> task([&](const Context& context) {
            context.begin_commit();
            ++writes;
            return 0;
        });
        until([&] { return task.phase() == Phase::awaiting_commit; });
        assert(task.cancel()); // The final GUI validation can refuse publication.
        until([&] { return task.done(); });
        bool cancelled = false;
        try { task.get(); } catch (const Cancelled&) { cancelled = true; }
        assert(cancelled && writes == 1);
    }
    until([] { return !file_worker_active; });
    {
        Task<int> task([](const Context&) -> int { throw std::runtime_error("disk failed"); });
        until([&] { return task.done(); });
        bool reported = false;
        try { task.get(); } catch (const std::runtime_error& e) { reported = std::string(e.what()) == "disk failed"; }
        assert(reported);
    }
    until([] { return !file_worker_active; });
    std::cout << "Library worker cancellation, lifetime, commit and error checks passed.\n";
}
