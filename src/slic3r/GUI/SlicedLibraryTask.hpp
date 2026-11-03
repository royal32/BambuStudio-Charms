#pragma once

// File Provider reads can block inside the OS. Never join one on the GUI
// thread, or let an abandoned read publish files after Cancel/timeout.
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace Slic3r { namespace GUI { namespace LibraryTasks {
enum class Phase { preparing, awaiting_snapshot, awaiting_commit, committing, cancelled };
struct Cancelled : std::exception {};
inline std::atomic<bool> file_worker_active{false};

struct State {
    std::atomic<Phase> phase{Phase::preparing};
    std::atomic<bool> done{false};
    std::mutex mutex;
    std::string message;
    std::vector<int> snapshot_indices;
    std::exception_ptr error;
};

class Context {
    std::shared_ptr<State> state;
public:
    explicit Context(std::shared_ptr<State> value) : state(std::move(value)) {}
    void checkpoint() const {
        if (state->phase == Phase::cancelled) throw Cancelled();
    }
    void report(std::string message) const {
        checkpoint();
        std::lock_guard<std::mutex> lock(state->mutex);
        state->message = std::move(message);
    }
    void await_snapshot(std::vector<int> indices = {}) const {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->snapshot_indices = std::move(indices);
        }
        auto expected = Phase::preparing;
        if (!state->phase.compare_exchange_strong(expected, Phase::awaiting_snapshot)) throw Cancelled();
        while (state->phase == Phase::awaiting_snapshot)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        checkpoint();
    }
    void begin_commit() const {
        auto expected = Phase::preparing;
        if (!state->phase.compare_exchange_strong(expected, Phase::awaiting_commit)) throw Cancelled();
        // The GUI validates its model snapshot before approving replacement.
        while (state->phase == Phase::awaiting_commit)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        checkpoint();
        if (state->phase != Phase::committing) throw std::logic_error("Library commit was not approved.");
    }
};

template<class Result> class Task {
    struct ResultState : State { std::optional<Result> result; };
    std::shared_ptr<ResultState> state = std::make_shared<ResultState>();
public:
    explicit Task(std::function<Result(const Context&)> work) {
        bool expected = false;
        if (!file_worker_active.compare_exchange_strong(expected, true))
            throw std::runtime_error("The previous library operation is still finishing a disk read. Please try again shortly.");
        try {
            std::thread([value = state, work = std::move(work)] {
                struct Release { ~Release() { file_worker_active = false; } } release;
                try { value->result.emplace(work(Context(value))); }
                catch (...) { value->error = std::current_exception(); }
                value->done = true; // Release/acquire publishes result and error.
            }).detach();
        } catch (...) { file_worker_active = false; throw; }
    }
    ~Task() { cancel(); }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    bool done() const { return state->done.load(); }
    Phase phase() const { return state->phase.load(); }
    std::string message() const {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->message;
    }
    std::vector<int> snapshot_indices() const {
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->snapshot_indices;
    }
    bool cancel() {
        auto phase = state->phase.load();
        while (phase == Phase::preparing || phase == Phase::awaiting_snapshot || phase == Phase::awaiting_commit) {
            if (state->phase.compare_exchange_weak(phase, Phase::cancelled)) return true;
        }
        return phase == Phase::cancelled;
    }
    void snapshot_ready() {
        auto expected = Phase::awaiting_snapshot;
        state->phase.compare_exchange_strong(expected, Phase::preparing);
    }
    void approve_commit() {
        auto expected = Phase::awaiting_commit;
        state->phase.compare_exchange_strong(expected, Phase::committing);
    }
    Result get() {
        if (!done()) throw std::logic_error("Library task is still running.");
        if (state->error) std::rethrow_exception(state->error);
        return std::move(state->result.value());
    }
};
}}}
