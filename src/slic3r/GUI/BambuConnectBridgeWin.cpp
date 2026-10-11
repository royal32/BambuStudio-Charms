#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "BambuConnectBridge.hpp"
#include "BambuConnectTransport.hpp"
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <sddl.h>
#include <array>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <set>
#include <thread>
#include <vector>

namespace Slic3r { namespace GUI { namespace BambuConnect {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = handle;
    }
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};

std::wstring registered_executable(HKEY hive) {
    DWORD size = 0;
    constexpr auto key = L"Software\\Classes\\bambu-connect\\shell\\open\\command";
    if (RegGetValueW(hive, key, nullptr, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size > 65536) return {};
    std::vector<wchar_t> value(size / sizeof(wchar_t) + 1);
    if (RegGetValueW(hive, key, nullptr, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) return {};
    const std::wstring command(value.data());
    // Parse only the executable; never execute the registry command template.
    const bool quoted = !command.empty() && command.front() == L'"';
    const auto end = command.find(quoted ? L'"' : L' ', quoted ? 1 : 0);
    return command.substr(quoted ? 1 : 0, end == std::wstring::npos ? end : end - (quoted ? 1 : 0));
}

std::wstring executable_path() {
    std::vector<std::wstring> candidates{registered_executable(HKEY_CURRENT_USER), registered_executable(HKEY_LOCAL_MACHINE)};
    for (const auto* variable : {L"LOCALAPPDATA", L"ProgramFiles", L"ProgramFiles(x86)"}) {
        wchar_t root[32768];
        const DWORD length = GetEnvironmentVariableW(variable, root, 32768);
        if (!length || length >= 32768) continue;
        const auto prefix = std::filesystem::path(root);
        candidates.push_back((prefix / L"Programs" / L"Bambu Connect" / L"Bambu Connect.exe").wstring());
        candidates.push_back((prefix / L"Bambu Connect" / L"Bambu Connect.exe").wstring());
    }
    for (const auto& path : candidates) {
        std::error_code error;
        if (!path.empty() && std::filesystem::is_regular_file(path, error) &&
            _wcsicmp(std::filesystem::path(path).extension().c_str(), L".exe") == 0) return path;
    }
    throw std::runtime_error("Bambu Connect is not installed.");
}

struct Windows { DWORD pid; bool hidden; bool close; bool found = false; };
BOOL CALLBACK visit_window(HWND window, LPARAM parameter) {
    auto& state = *reinterpret_cast<Windows*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != state.pid || GetWindow(window, GW_OWNER)) return TRUE;
    state.found = true;
    if (state.close) PostMessageW(window, WM_CLOSE, 0, 0);
    else if (state.hidden) ShowWindowAsync(window, SW_HIDE);
    else { ShowWindowAsync(window, SW_SHOW); SetForegroundWindow(window); }
    return TRUE;
}

// The parent endpoints use overlapped I/O so an unresponsive Electron process
// cannot block Studio indefinitely. Only the two child endpoints are inherited.
void make_pipe(Handle& parent, Handle& child, bool parent_writes) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Could not create Connect pipes.");
    wchar_t id[40];
    StringFromGUID2(guid, id, 40);
    const auto name = std::wstring(L"\\\\.\\pipe\\studio-connect-") + id;
    Handle token;
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) throw std::runtime_error("Could not secure Connect pipes.");
    token.reset(raw_token);
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    if (!size || !GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) throw std::runtime_error("Could not secure Connect pipes.");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) throw std::runtime_error("Could not secure Connect pipes.");
    const auto sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Could not secure Connect pipes.");
    SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
    parent.reset(CreateNamedPipeW(name.c_str(), (parent_writes ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
        FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security));
    LocalFree(descriptor);
    if (!parent) throw std::runtime_error("Could not create Connect pipes.");
    SECURITY_ATTRIBUTES inheritance{sizeof(inheritance), nullptr, TRUE};
    child.reset(CreateFileW(name.c_str(), parent_writes ? GENERIC_READ : GENERIC_WRITE,
        0, &inheritance, OPEN_EXISTING, 0, nullptr));
    if (!child) throw std::runtime_error("Could not connect Connect pipes.");
}

DWORD transfer_pipe(HANDLE pipe, void* data, DWORD size, bool writing, Transport::Clock::time_point deadline) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw std::runtime_error("Could not wait for Connect.");
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD count = 0;
    const BOOL complete = writing ? WriteFile(pipe, data, size, &count, &operation) : ReadFile(pipe, data, size, &count, &operation);
    if (!complete) {
        if (GetLastError() != ERROR_IO_PENDING) throw std::runtime_error("Connect closed its pipe.");
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Transport::Clock::now()).count();
        const DWORD wait = remaining > 0 ? WaitForSingleObject(event.value, static_cast<DWORD>(remaining)) : WAIT_TIMEOUT;
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &count, TRUE);
            throw std::runtime_error("Connect did not respond in time.");
        }
        if (!GetOverlappedResult(pipe, &operation, &count, FALSE)) throw std::runtime_error("Connect closed its pipe.");
    }
    if (!count) throw std::runtime_error("Connect closed its pipe.");
    return count;
}

class WindowsTransport final : public Transport {
    Handle input, output, process;
    DWORD pid = 0;

public:
    ~WindowsTransport() override { close(); }
    void close() override { input.reset(); output.reset(); process.reset(); pid = 0; }
    bool running() override { return process && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT; }
    void hide(bool hidden) override {
        if (!pid) return;
        Windows state{pid, hidden, false};
        EnumWindows(visit_window, reinterpret_cast<LPARAM>(&state));
    }
    void write(const std::string& bytes, Clock::time_point deadline) override {
        for (size_t written = 0; written < bytes.size();) {
            if (Clock::now() >= deadline) throw std::runtime_error("Connect pipe write timed out.");
            const auto size = static_cast<DWORD>(std::min<size_t>(bytes.size() - written, 65536));
            written += transfer_pipe(input.value, const_cast<char*>(bytes.data() + written), size, true, deadline);
        }
    }
    std::string read(Clock::time_point deadline) override {
        char bytes[65536];
        const auto count = transfer_pipe(output.value, bytes, sizeof(bytes), false, deadline);
        return std::string(bytes, count);
    }
    void launch() override {
        const auto executable = executable_path();
        // Electron's existing singleton cannot acquire our inherited handles.
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        PROCESSENTRY32W entry{sizeof(entry)};
        std::vector<PROCESSENTRY32W> candidates;
        std::set<DWORD> connect_pids;
        if (snapshot && Process32FirstW(snapshot.value, &entry)) do {
            if (_wcsicmp(entry.szExeFile, std::filesystem::path(executable).filename().c_str()) != 0) continue;
            Handle previous(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID));
            wchar_t path[32768]; DWORD size = 32768;
            if (!previous || !QueryFullProcessImageNameW(previous.value, 0, path, &size) || _wcsicmp(path, executable.c_str())) continue;
            candidates.push_back(entry);
            connect_pids.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot.value, &entry));
        for (const auto& candidate : candidates) {
            // Renderer/GPU processes use the same executable. Ask only their
            // browser parent to quit so Electron can close its children cleanly.
            if (connect_pids.count(candidate.th32ParentProcessID)) continue;
            Handle previous(OpenProcess(SYNCHRONIZE, FALSE, candidate.th32ProcessID));
            if (!previous) continue;
            Windows state{candidate.th32ProcessID, false, true};
            EnumWindows(visit_window, reinterpret_cast<LPARAM>(&state));
            if (state.found && WaitForSingleObject(previous.value, 10000) != WAIT_OBJECT_0)
                throw std::runtime_error("Close Bambu Connect once, then try again.");
        }
        Handle child_input, child_output;
        make_pipe(input, child_input, true);
        make_pipe(output, child_output, false);
        std::wstring command = L"\"" + executable + L"\" --remote-debugging-pipe --remote-debugging-io-pipes=" +
            std::to_wstring(reinterpret_cast<uintptr_t>(child_input.value)) + L"," +
            std::to_wstring(reinterpret_cast<uintptr_t>(child_output.value)) +
            L" --disable-background-timer-throttling --disable-renderer-backgrounding --disable-backgrounding-occluded-windows";
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        std::vector<unsigned char> storage(size);
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) throw std::runtime_error("Could not launch Bambu Connect.");
        HANDLE handles[]{child_input.value, child_output.value};
        const bool attributes_ok = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles, sizeof(handles), nullptr, nullptr) != FALSE;
        PROCESS_INFORMATION child{};
        const bool launched = attributes_ok && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &child);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!launched) throw std::runtime_error("Could not launch Bambu Connect.");
        process.reset(child.hProcess);
        CloseHandle(child.hThread);
        pid = child.dwProcessId;
    }
};
}
std::unique_ptr<Transport> create_transport() { return std::make_unique<WindowsTransport>(); }

std::string studio_access_token(void*, void*, const std::string&)
{
    // The Mac plugin's private memory layout is not valid for MSVC. Windows
    // verifies Connect's signed-in UID and requires a matching manual sign-in.
    return {};
}
}}}
