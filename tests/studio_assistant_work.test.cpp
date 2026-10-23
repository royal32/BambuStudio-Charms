#include "slic3r/GUI/StudioAssistantWork.hpp"
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

using namespace Slic3r::GUI::Assistant;
using namespace std::chrono_literals;

void wait_for_tool(Work& work)
{
    const auto deadline=std::chrono::steady_clock::now()+2s;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(work.mutex);
            for (const auto& event:work.events)
                if (event.at("event")=="tool") return;
        }
        assert(std::chrono::steady_clock::now()<deadline);
        std::this_thread::sleep_for(1ms);
    }
}

template<class F> bool stopped(F&& fn)
{
    try { fn(); }
    catch (const std::runtime_error& error) {
        return std::string(error.what()).find("Stopped.")==0;
    }
    return false;
}

int main()
{
    {
        DeferredModelsRefresh refresh;
        assert(!refresh.take(false,false));
        // signed_in is polled before idle. Busy polls must retain the follow-up.
        refresh.request();
        assert(!refresh.take(true,false));
        assert(!refresh.take(true,false));
        assert(refresh.take(false,false));
        // A subsequent model-fetch error/idle does not automatically retry.
        assert(!refresh.take(false,false));
        // signed_in and idle in the same poll produce exactly one follow-up.
        refresh.request();
        assert(refresh.take(false,false));
        assert(!refresh.take(false,false));
        // Stop wins whether idle is in the same poll or a later poll.
        refresh.request();
        assert(!refresh.take(false,true));
        assert(!refresh.take(false,false));
        refresh.request();
        assert(!refresh.take(true,true));
        assert(!refresh.take(false,false));
        // Clearing/cancelling an old request does not block a fresh sign-in.
        refresh.request(); refresh.clear();
        assert(!refresh.take(false,false));
        refresh.request();
        assert(refresh.take(false,false));
    }
    // Stop must acquire the predicate mutex before setting cancellation.
    // Without that synchronization it can notify between wait's predicate
    // check and sleeping, leaving a pending tool blocked indefinitely.
    {
        Work work;
        std::unique_lock<std::mutex> lock(work.mutex);
        std::promise<void> started;
        auto stop=std::async(std::launch::async,[&] { started.set_value(); work.stop(); });
        started.get_future().wait();
        assert(stop.wait_for(100ms)==std::future_status::timeout);
        assert(!work.cancel.load());
        lock.unlock();
        assert(stop.wait_for(2s)==std::future_status::ready);
        stop.get();
        assert(work.cancel.load());
    }
    {
        Work work;
        work.stop();
        assert(stopped([&] { work.tool("inspect_project",Json::object()); }));
    }
    {
        Work work;
        auto pending=std::async(std::launch::async,[&] {
            return stopped([&] { work.tool("slice",Json::object()); });
        });
        wait_for_tool(work);
        work.stop();
        assert(pending.wait_for(2s)==std::future_status::ready);
        assert(pending.get());
    }
    {
        Work work;
        work.post({{"event","delta"},{"text","Before "}});
        work.post({{"event","delta"},{"text","tool"}});
        const Json arguments={{"revision","r1"}};
        auto pending=std::async(std::launch::async,[&] { return work.tool("arrange",arguments); });
        wait_for_tool(work);
        work.post({{"event","delta"},{"text","After "}});
        work.post({{"event","delta"},{"text","tool"}});
        {
            std::lock_guard<std::mutex> lock(work.mutex);
            assert(work.events.size()==3);
            assert(work.events.at(0)==Json({{"event","delta"},{"text","Before tool"}}));
            assert(work.events.at(1)==Json({{"event","tool"},{"name","arrange"},{"arguments",arguments}}));
            assert(work.events.at(2)==Json({{"event","delta"},{"text","After tool"}}));
        }
        const Json result={{"revision","r2"},{"ok",true}};
        work.answer(result);
        assert(pending.wait_for(2s)==std::future_status::ready);
        assert(pending.get()==result);
        // A previous answer must not satisfy the following request.
        {
            std::lock_guard<std::mutex> lock(work.mutex);
            work.events.clear();
        }
        auto next=std::async(std::launch::async,[&] { return work.tool("inspect_project",Json::object()); });
        wait_for_tool(work);
        assert(next.wait_for(20ms)==std::future_status::timeout);
        work.answer({{"revision","r3"}});
        assert(next.wait_for(2s)==std::future_status::ready);
        assert(next.get()==Json({{"revision","r3"}}));
    }
    // Exercise both orders around the first wait without depending on a
    // particular scheduler ordering. Every waiter must finish after Stop.
    for (int i=0;i<128;++i) {
        Work work;
        std::promise<void> start;
        auto gate=start.get_future().share();
        auto pending=std::async(std::launch::async,[&] {
            gate.wait();
            return stopped([&] { work.tool("inspect_project",Json::object()); });
        });
        auto stop=std::async(std::launch::async,[&] { gate.wait(); work.stop(); });
        start.set_value();
        assert(stop.wait_for(2s)==std::future_status::ready);
        stop.get();
        assert(pending.wait_for(2s)==std::future_status::ready);
        assert(pending.get());
    }
    std::cout<<"Assistant worker cancellation, answers, delta coalescing, event ordering, and deferred model refresh passed.\n";
}
