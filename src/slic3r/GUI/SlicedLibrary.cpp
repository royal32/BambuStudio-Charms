#include "WebViewDialog.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "GLToolbar.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <wx/base64.h>
#include <wx/dirdlg.h>
#include <wx/weakref.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace Slic3r { namespace GUI {
namespace fs = std::filesystem;
using LibraryJson = nlohmann::json;

struct SlicedLibraryEntry {
    std::string id;
    std::string path;
    std::string name;
    std::string folder;
    uintmax_t size{0};
    std::string stamp;
};

// Workers own only this shared data, never a window or application object.
// An unavailable network folder must not block the UI or application shutdown.
struct SlicedLibraryState {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::string folder;
    std::string token;
    std::vector<SlicedLibraryEntry> entries;
    std::map<std::string, std::string> thumbnails;
    LibraryJson pending_thumbnails = LibraryJson::array();
    std::string error;
    bool scanning{true};
    bool dirty{false};
    bool reading_thumbnails{false};
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
};

static void send_library_message(WebViewPanel& panel, const LibraryJson& message)
{
    // ASCII JSON also escapes line separators and arbitrary filename characters.
    panel.RunScript(wxString::FromUTF8("window.postMessage(" +
        message.dump(-1, ' ', true, LibraryJson::error_handler_t::replace) + ")"));
}

void WebViewPanel::StopSlicedLibrary()
{
    if (m_sliced_library) m_sliced_library->cancelled = true;
    m_sliced_library.reset();
    if (m_sliced_library_timer) {
        m_sliced_library_timer->Stop();
        delete m_sliced_library_timer;
        m_sliced_library_timer = nullptr;
    }
}

void WebViewPanel::StartSlicedLibraryScan(const std::string& folder)
{
    if (m_sliced_library) m_sliced_library->cancelled = true;
    auto state = std::make_shared<SlicedLibraryState>();
    state->folder = folder;
    if (m_sliced_library && m_sliced_library->folder == folder) {
        std::lock_guard<std::mutex> lock(m_sliced_library->mutex);
        state->thumbnails = m_sliced_library->thumbnails;
    }
    state->token = std::to_string(state->started.time_since_epoch().count());
    m_sliced_library = state;
    if (!m_sliced_library_timer) {
        m_sliced_library_timer = new wxTimer(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { PollSlicedLibrary(); }, m_sliced_library_timer->GetId());
    }
    m_sliced_library_timer->Start(200);
    send_library_message(*this, {{"command", "sliced_library_status"}, {"folder", folder}, {"scanning", true}});
    std::thread([state] {
        std::vector<SlicedLibraryEntry> entries;
        std::string error;
        try {
            if (!state->folder.empty()) {
                const auto root = fs::canonical(fs::u8path(state->folder));
                if (!fs::is_directory(root)) throw std::runtime_error("The library folder is unavailable.");
                for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied), end; it != end; ++it) {
                    if (state->cancelled) return;
                    const auto& item = *it;
                    const auto filename = item.path().filename().u8string();
                    if (!filename.empty() && filename.front() == '.') {
                        if (item.is_directory()) it.disable_recursion_pending();
                        continue;
                    }
                    // Do not follow files or directories linked outside the chosen folder.
                    if (fs::is_symlink(item.symlink_status()) || !item.is_regular_file()) continue;
                    std::string lower = filename;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
                    const std::string suffix = ".gcode.3mf";
                    if (lower.size() <= suffix.size() || lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
                    const auto relative = item.path().lexically_relative(root);
                    entries.push_back({relative.generic_u8string(), item.path().u8string(),
                        filename.substr(0, filename.size() - suffix.size()), relative.parent_path().generic_u8string(), item.file_size(),
                        std::to_string(item.file_size()) + ":" + std::to_string(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(item.last_write_time().time_since_epoch()).count())});
                }
                std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            }
        } catch (const std::exception&) {
            error = "The library folder could not be read. Check that the drive is connected, then refresh.";
        }
        if (state->cancelled) return;
        std::lock_guard<std::mutex> lock(state->mutex);
        std::map<std::string, std::string> retained_thumbnails;
        for (const auto& entry : entries) {
            const auto key = entry.id + "\n" + entry.stamp;
            const auto cached = state->thumbnails.find(key);
            if (cached != state->thumbnails.end()) retained_thumbnails.emplace(*cached);
        }
        state->thumbnails = std::move(retained_thumbnails);
        state->entries = std::move(entries);
        state->error = std::move(error);
        state->scanning = false;
        state->dirty = true;
    }).detach();
}

void WebViewPanel::PollSlicedLibrary()
{
    auto state = m_sliced_library;
    if (!state) return;
    LibraryJson list, thumbnails;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->dirty) {
            LibraryJson entries = LibraryJson::array();
            for (const auto& entry : state->entries)
                entries.push_back({{"id", entry.id}, {"name", entry.name}, {"folder", entry.folder}, {"size", entry.size}, {"stamp", entry.stamp}});
            list = {{"command", "sliced_library_list"}, {"token", state->token}, {"folder", state->folder},
                {"entries", entries}, {"error", state->error}, {"scanning", state->scanning}};
            state->dirty = false;
        }
        if (!state->pending_thumbnails.empty()) {
            thumbnails = {{"command", "sliced_library_thumbnails"}, {"token", state->token}, {"entries", state->pending_thumbnails}};
            state->pending_thumbnails = LibraryJson::array();
        }
        if (!state->scanning && !state->reading_thumbnails) m_sliced_library_timer->Stop();
    }
    if (!list.is_null()) send_library_message(*this, list);
    if (!thumbnails.is_null()) send_library_message(*this, thumbnails);
}

bool WebViewPanel::HandleSlicedLibraryMessage(const std::string& message)
{
    // This handler is called only for the local home-page webview.
    if (message.find("sliced_library_") == std::string::npos) return false;
    const auto request = LibraryJson::parse(message, nullptr, false);
    if (!request.is_object() || !request.contains("command") || !request["command"].is_string()) return false;
    const auto command = request["command"].get<std::string>();
    if (command.compare(0, 15, "sliced_library_") != 0) return false;

    if (command == "sliced_library_choose") {
        wxWeakRef<WebViewPanel> weak(this);
        CallAfter([weak] {
            if (!weak) return;
            auto& config = *wxGetApp().app_config;
            wxDirDialog dialog(weak.get(), _L("Choose your sliced print library folder"),
                wxString::FromUTF8(config.get("sliced_library_folder")), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
            if (dialog.ShowModal() != wxID_OK) return;
            const auto folder = std::string(dialog.GetPath().utf8_string());
            config.set("sliced_library_folder", folder);
            config.save();
            weak->StartSlicedLibraryScan(folder);
        });
    } else if (command == "sliced_library_get" || command == "sliced_library_refresh") {
        if (!IsShownOnScreen() && m_sliced_library) return true;
        const auto folder = wxGetApp().app_config->get("sliced_library_folder");
        bool scan = !m_sliced_library || m_sliced_library->folder != folder;
        if (m_sliced_library) {
            std::lock_guard<std::mutex> lock(m_sliced_library->mutex);
            scan = scan || (!m_sliced_library->scanning && (command == "sliced_library_refresh" ||
                std::chrono::steady_clock::now() - m_sliced_library->started > std::chrono::seconds(30)));
            m_sliced_library->dirty = true;
        }
        if (scan) StartSlicedLibraryScan(folder);
        else PollSlicedLibrary();
    } else if (command == "sliced_library_thumbnails") {
        auto state = m_sliced_library;
        if (!state || !request.contains("ids") || !request["ids"].is_array() ||
            !request.contains("token") || !request["token"].is_string() ||
            request["token"].get<std::string>() != state->token) return true;
        std::vector<SlicedLibraryEntry> wanted;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->reading_thumbnails || state->scanning) return true;
            for (const auto& id : request["ids"]) {
                if (!id.is_string() || wanted.size() >= 60) continue;
                const auto name = id.get<std::string>();
                const auto it = std::find_if(state->entries.begin(), state->entries.end(), [&](const auto& e) { return e.id == name; });
                if (it == state->entries.end()) continue;
                auto cached = state->thumbnails.find(name + "\n" + it->stamp);
                if (cached != state->thumbnails.end()) {
                    state->pending_thumbnails.push_back({{"id", name}, {"image", cached->second}});
                    continue;
                }
                wanted.push_back(*it);
            }
            state->reading_thumbnails = !wanted.empty();
        }
        m_sliced_library_timer->Start(200);
        if (!wanted.empty()) std::thread([state, wanted] {
            for (const auto& entry : wanted) {
                if (state->cancelled) return;
                std::string image;
                try {
                    const auto png = bbs_3mf_get_thumbnail(entry.path.c_str());
                    if (!png.empty()) image = "data:image/png;base64," + std::string(wxBase64Encode(png.data(), png.size()).utf8_string());
                } catch (const std::exception&) { /* Missing thumbnails do not hide a job. */ }
                std::lock_guard<std::mutex> lock(state->mutex);
                state->thumbnails[entry.id + "\n" + entry.stamp] = image;
                state->pending_thumbnails.push_back({{"id", entry.id}, {"image", image}});
            }
            std::lock_guard<std::mutex> lock(state->mutex);
            state->reading_thumbnails = false;
        }).detach();
    } else if (command == "sliced_library_print") {
        auto state = m_sliced_library;
        if (m_sliced_library_opening) return true;
        if (!state || !request.contains("id") || !request["id"].is_string() ||
            !request.contains("token") || !request["token"].is_string() ||
            request["token"].get<std::string>() != state->token) {
            // A refresh can finish between the click and this message. Always
            // release the page's Opening state so the user can select again.
            send_library_message(*this, {{"command", "sliced_library_opened"}});
            return true;
        }
        std::string path;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const auto id = request["id"].get<std::string>();
            const auto it = std::find_if(state->entries.begin(), state->entries.end(), [&](const auto& e) { return e.id == id; });
            if (it != state->entries.end()) path = it->path;
        }
        if (path.empty()) {
            send_library_message(*this, {{"command", "sliced_library_opened"}});
            return true;
        }
        m_sliced_library_opening = true;
        wxWeakRef<WebViewPanel> weak(this);
        CallAfter([weak, state, path] {
            if (!weak) return;
            bool print_queued = false;
            try {
                const auto root = fs::canonical(fs::u8path(state->folder));
                const auto file = fs::canonical(fs::u8path(path));
                const auto relative = file.lexically_relative(root);
                if (relative.empty() || *relative.begin() == ".." || !fs::is_regular_file(file))
                    throw std::runtime_error("This file is no longer available in the library folder. Refresh the library and try again.");
                auto* plater = wxGetApp().plater();
                if (plater && wxGetApp().can_load_project() && plater->load_project(wxString::FromUTF8(path)) != wxID_CANCEL &&
                    plater->is_gcode_3mf() && plater->get_3mf_filename() == path) {
                    auto& plates = plater->get_partplate_list();
                    int valid = 0, selected = -1;
                    for (int i = 0; i < plates.get_plate_count(); ++i) {
                        if (plates.get_plate(i)->is_slice_result_ready_for_print()) { ++valid; selected = i; }
                    }
                    wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
                    if (valid == 1) {
                        if (selected != plates.get_curr_plate_index()) plates.select_plate(selected);
                        // Let loading events unwind, then reuse the normal dialog.
                        // Library jobs are temporary: unload them after the dialog
                        // closes so Home does not retain a read-only preview document.
                        wxWeakRef<Plater> weak_plater(plater);
                        weak->CallAfter([weak, weak_plater, path] {
                            if (!weak || !weak_plater) return;
                            if (weak_plater->get_3mf_filename() == path && weak_plater->is_gcode_3mf()) {
                                SimpleEvent print_event(EVT_GLTOOLBAR_PRINT_PLATE);
                                weak_plater->GetEventHandler()->ProcessEvent(print_event);
                                // The modal loop can process other document actions.
                                // Clear only the library job we opened, never a replacement.
                                // Connect has its own exported archive by the time a
                                // successful handoff returns, so this also applies to Send.
                                if (weak_plater && weak_plater->is_gcode_3mf() &&
                                    weak_plater->get_3mf_filename() == path)
                                    weak_plater->new_project(/*skip_confirm=*/true, /*silent=*/true);
                                if (weak && wxGetApp().mainframe)
                                    wxGetApp().mainframe->select_tab(MainFrame::tpHome);
                            }
                            if (weak) {
                                weak->m_sliced_library_opening = false;
                                send_library_message(*weak, {{"command", "sliced_library_opened"}});
                            }
                        });
                        print_queued = true;
                    } else {
                        MessageDialog dialog(plater, valid > 1 ?
                            _L("This file contains multiple sliced plates. Choose a plate in Preview, then click Print plate.") :
                            _L("This file has no plate ready to print. Check its slicing results in Preview."), _L("Sliced library"), wxOK);
                        dialog.ShowModal();
                    }
                }
            } catch (const std::exception& error) {
                MessageDialog dialog(weak.get(), wxString::FromUTF8(error.what()), _L("Sliced library"), wxOK | wxICON_ERROR);
                dialog.ShowModal();
            }
            if (weak && !print_queued) {
                weak->m_sliced_library_opening = false;
                send_library_message(*weak, {{"command", "sliced_library_opened"}});
            }
        });
    }
    return true;
}

}} // namespace Slic3r::GUI
