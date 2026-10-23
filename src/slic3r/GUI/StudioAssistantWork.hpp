#ifndef slic3r_StudioAssistantWork_hpp_
#define slic3r_StudioAssistantWork_hpp_

#include "StudioAssistantClient.hpp"
#include <condition_variable>
#include <deque>
#include <stdexcept>

namespace Slic3r { namespace GUI { namespace Assistant {
// UI-thread state: signed_in and idle can arrive in different timer polls.
class DeferredModelsRefresh {
public:
    void request() { m_pending=true; }
    void clear() { m_pending=false; }
    bool take(bool busy,bool cancelled) {
        if (cancelled) { clear(); return false; }
        if (busy || !m_pending) return false;
        clear(); return true;
    }
private:
    bool m_pending{false};
};

struct Work {
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Json> events;
    Json result;
    bool answered{false};
    void post(Json event) {
        std::lock_guard<std::mutex> lock(mutex);
        // Network chunks can arrive much faster than the UI's timer. Preserve
        // event order while painting adjacent text chunks together once a tick.
        if (event.value("event","")=="delta" && !events.empty() && events.back().value("event","")=="delta")
            events.back().at("text").get_ref<std::string&>()+=event.at("text").get<std::string>();
        else events.push_back(std::move(event));
    }
    Json tool(const std::string& name, const Json& arguments) {
        std::unique_lock<std::mutex> lock(mutex);
        answered = false;
        events.push_back({{"event", "tool"}, {"name", name}, {"arguments", arguments}});
        changed.wait(lock, [&] { return answered || cancel.load(); });
        if (cancel) throw std::runtime_error("Stopped. Changes already applied remain available through Undo.");
        return result;
    }
    void answer(Json value) {
        std::lock_guard<std::mutex> lock(mutex);
        result = std::move(value); answered = true; changed.notify_all();
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex); cancel=true; }
        // Use the same mutex as tool()'s wait predicate, so Stop cannot notify
        // between its predicate check and sleep and leave the worker stranded.
        changed.notify_all();
    }
};
}}}
#endif
