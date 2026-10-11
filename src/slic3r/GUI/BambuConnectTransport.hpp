#pragma once
#include <chrono>
#include <memory>
#include <string>

namespace Slic3r { namespace GUI { namespace BambuConnect {
// Private inherited CDP pipes; implementations must not open a TCP listener.
class Transport {
public:
    using Clock = std::chrono::steady_clock;
    virtual ~Transport() = default;
    virtual void launch() = 0;
    virtual void close() = 0;
    virtual bool running() = 0;
    virtual void hide(bool hidden) = 0;
    virtual void write(const std::string& bytes, Clock::time_point deadline) = 0;
    virtual std::string read(Clock::time_point deadline) = 0;
};
std::unique_ptr<Transport> create_transport();
}}}
