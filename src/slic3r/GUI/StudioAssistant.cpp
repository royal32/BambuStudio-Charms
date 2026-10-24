#include "StudioAssistant.hpp"
#include "StudioAssistantActions.hpp"
#include "StudioAssistantChat.hpp"
#include "StudioAssistantKeychain.hpp"
#include "StudioAssistantWork.hpp"
#include "StudioAssistantLog.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "Widgets/StateColor.hpp"
#include <wx/frame.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/scrolwin.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/secretstore.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <condition_variable>
#include <deque>
#include <thread>
#include <filesystem>

namespace Slic3r { namespace GUI {
namespace {
using namespace Assistant;
std::string action_label(const std::string& tool, const Json& args = Json::object()) {
    if (tool=="document") {
        static const std::map<std::string,std::string> documents={{"open","Open a project"},{"import","Add a model file"},
            {"save","Save the project"},{"save_as","Save a project copy"},{"export_sliced","Export the sliced plate"}};
        auto action=documents.find(args.value("action",std::string()));
        if (action!=documents.end()) return action->second;
    }
    static const std::map<std::string,std::string> labels = {
        {"inspect_project","Inspect project and warnings"},{"inspect_settings","Look up process settings"},{"list_profiles","Find installed profiles"},
        {"use_profile","Choose a profile"},{"model_view","Look at the model"},{"set_settings","Change process settings"},{"select_objects","Focus objects"},
        {"select_parts","Highlight model parts"},{"split_parts","Split disconnected pieces into parts"},{"assign_part_filament","Assign part colors"},{"set_filament_color","Change a project filament color"},
        {"select_plate","Select a plate"},{"view","Show a Studio view"},{"arrange","Arrange parts"},{"orient","Orient parts"},{"move_to_plate","Move parts to a plate"},
        {"slice","Slice and check warnings"},{"copies","Make copies"},{"rename","Rename an item"},{"new_plate","Create a plate"},{"transform","Move, rotate, or scale parts"},
        {"undo","Undo a change"},{"redo","Redo a change"},{"delete_selected","Delete selected parts"},{"search_library","Search the library"},
        {"inspect_library_source","Inspect an editable source"},{"add_library_model","Add a library model"},{"open_library_project","Open an editable project"},{"document","Open a project or file dialog"}};
    auto found=labels.find(tool); return found==labels.end()?"Studio action":found->second;
}
std::string confirmation_text(const std::string& tool,const Json& args) {
    if (tool=="delete_selected") return "Delete the selected parts? You can restore them with Undo.";
    if (tool=="open_library_project") return "Open this library item's editable source project? Studio will ask about any unsaved changes.";
    if (tool=="use_profile") return "Choose the profile “"+args.value("name",std::string())+"”? This changes the project's print or material settings.";
    if (tool=="set_filament_color") return "Change project filament slot "+std::to_string(args.at("slot").get<int>())+" to "+args.at("color").get<std::string>()+"? All model parts using this slot will display that color. Physical AMS settings stay unchanged.";
    if (tool=="assign_part_filament" && args.value("replace_color_paint",false)) return "Replace color painting on the selected model parts with one uniform filament color? Other parts and support painting stay unchanged. Undo is available.";
    if (tool=="set_settings") {
        std::string text="Apply these "+args.value("scope",std::string("project"))+" process settings?";
        const auto& config=wxGetApp().preset_bundle->prints.get_edited_preset().config;
        for (const auto& setting : args.value("settings",Json::array())) {
            const auto key=setting.value("key",std::string()); auto value=setting.value("value",std::string());
            const auto* definition=config.def()->get(key);
            if (definition) {
                if (definition->type==coBool) value=value=="1"?"On":value=="0"?"Off":value;
                else for (size_t i=0;i<definition->enum_values.size();++i)
                    if (definition->enum_values[i]==value && i<definition->enum_labels.size()) { value=definition->enum_labels[i]; break; }
            }
            text+="\n"+(definition && !definition->label.empty()?definition->label:key)+": "+value;
        }
        return text;
    }
    if (tool=="document") {
        const auto action=args.value("action",std::string());
        if (action=="save") return "Save the current project?";
        if (action=="save_as") return "Choose where to save a copy of this project?";
        if (action=="export_sliced") return "Choose where to export the current sliced plate?";
        if (action=="import") return "Choose a model file to add to this project?";
        return "Choose a project to open? Studio will ask about any unsaved changes.";
    }
    return action_label(tool,args)+"? You can review the result in Studio and use Undo.";
}
class AssistantWindow : public wxFrame {
public:
    explicit AssistantWindow(wxWindow* parent);
    ~AssistantWindow() override {
        m_timer.Stop(); if (m_work) m_work->stop();
        if (m_log) m_log->append("window_destroyed",{{"turn",m_log_turn},{"task_running",m_logging_turn}});
    }
    void focus_composer() { m_input->SetFocus(); }
private:
    void poll();
    void connect(bool add);
    void fetch_models();
    void send(const std::string& text, bool clear_draft = true);
    void accounts();
    void menu();
    void clear_chat();
    void layout_chat(bool follow = true);
    void activity(const std::string& text, bool done = false, bool failed = false, bool unconfirmed = false);
    void busy(bool value);
    void stop();
    void update_composer();
    void status(const wxString& text);
    void append(const std::string& who, const std::string& text);
    void start(const std::function<void(const std::shared_ptr<Work>&)>& fn);
    void handle_tool(const Json& event);
    Json registration() const;
    void save_accounts();
    void check_log();
    Actions m_actions;
    std::shared_ptr<Client> m_client;
    std::shared_ptr<Work> m_work;
    std::shared_ptr<ConversationLog> m_log;
    size_t m_log_turn{0};
    bool m_logging_turn{false},m_log_warned{false};
    wxTimer m_timer;
    wxTextCtrl* m_input;
    wxChoice* m_models;
    wxButton *m_connect, *m_menu, *m_send;
    std::vector<wxButton*> m_quick;
    wxScrolledWindow* m_chat;
    wxBoxSizer* m_messages;
    std::vector<std::pair<AssistantBubble*,bool>> m_bubbles;
    AssistantBubble* m_reply{nullptr};
    // 0: running, 1: completed, 2: failed/skipped, 3: check result.
    // An error or unconfirmed file operation must never get a completion tick.
    std::vector<std::pair<std::string,int>> m_steps;
    bool m_allow_changes{true}, m_action_done{true};
    Json m_registrations = Json::array(), m_model_catalog = Json::array(), m_history = Json::array();
    Json m_problem = Json::object();
    int m_active{-1};
    bool m_busy{false}, m_streaming{false};
    bool m_stopped_native{false};
    bool m_fetching_models{false}, m_retry_models{false};
    DeferredModelsRefresh m_deferred_models_refresh;
    std::string m_pending;
    std::chrono::steady_clock::time_point m_pending_since;
};

AssistantWindow::AssistantWindow(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, "Studio assistant", wxDefaultPosition, wxSize(520, 760), wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT), m_timer(this)
{
    SetMinSize(FromDIP(wxSize(400, 440)));
    auto* panel = new wxPanel(this);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* model_row = new wxBoxSizer(wxHORIZONTAL);
    m_models = new wxChoice(panel, wxID_ANY); model_row->Add(m_models, 1, wxRIGHT|wxALIGN_CENTER_VERTICAL, FromDIP(8));
    m_models->SetName("Assistant model");
    m_connect = new wxButton(panel, wxID_ANY, "Continue with ChatGPT"); model_row->Add(m_connect, 1, wxRIGHT|wxALIGN_CENTER_VERTICAL, FromDIP(8));
    m_menu = new wxButton(panel, wxID_ANY, wxString::FromUTF8("⋯"), wxDefaultPosition, FromDIP(wxSize(38,-1)));
    m_menu->SetName("Assistant options"); m_menu->SetToolTip("Accounts and conversation options"); model_row->Add(m_menu,0,wxALIGN_CENTER_VERTICAL);
    outer->Add(model_row, 0, wxEXPAND|wxALL, FromDIP(12));
    m_chat = new wxScrolledWindow(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL|wxBORDER_NONE);
    m_chat->SetScrollRate(0, FromDIP(10)); m_messages=new wxBoxSizer(wxVERTICAL); m_chat->SetSizer(m_messages);
    m_chat->Bind(wxEVT_SIZE,[this](wxSizeEvent& event) { layout_chat(false); event.Skip(); });
    outer->Add(m_chat, 1, wxEXPAND|wxLEFT|wxRIGHT, FromDIP(8));
    auto* quick = new wxBoxSizer(wxHORIZONTAL);
    for (const auto& item : std::vector<std::pair<std::string,std::string>>{{"Explain warnings", "Explain the warnings on this plate."},
        {"Help with supports", "Help me set up supports for this plate."}}) {
        auto* button = new wxButton(panel, wxID_ANY, wxString::FromUTF8(item.first));
        m_quick.push_back(button);
        quick->Add(button, 1, wxRIGHT, FromDIP(4));
        button->Bind(wxEVT_BUTTON, [this, text=item.second](wxCommandEvent&) { send(text,false); });
    }
    outer->Add(quick, 0, wxEXPAND|wxALL, FromDIP(12));
    auto* composer = new wxBoxSizer(wxHORIZONTAL);
    m_input = new wxTextCtrl(panel, wxID_ANY, "", wxDefaultPosition, FromDIP(wxSize(-1, 72)), wxTE_MULTILINE);
    m_input->SetName("Message the assistant"); m_input->SetHint("Ask me to help with your print…");
    m_input->SetToolTip("Enter to send. Shift+Enter for a new line.");
    composer->Add(m_input,1,wxEXPAND|wxRIGHT,FromDIP(6));
    m_send = new wxButton(panel, wxID_ANY, "Send"); composer->Add(m_send,0,wxALIGN_BOTTOM);
    outer->Add(composer,0,wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM,FromDIP(12));
    m_allow_changes=wxGetApp().app_config->get("studio_assistant_allow_changes")!="false";
    panel->SetSizer(outer);
    auto* frame_sizer = new wxBoxSizer(wxVERTICAL); frame_sizer->Add(panel, 1, wxEXPAND); SetSizer(frame_sizer);
    const auto background = StateColor::darkModeColorFor(*wxWHITE);
    SetBackgroundColour(background);
    panel->SetBackgroundColour(background);
    const auto foreground = StateColor::darkModeColorFor(wxColour("#262e30"));
    panel->SetForegroundColour(foreground);
    for (auto* child : panel->GetChildren()) {
        child->SetForegroundColour(foreground);
#ifdef __APPLE__
        // Cocoa draws the bezel; a forced fill adds a second rectangular layer.
        if (dynamic_cast<wxButton*>(child) || dynamic_cast<wxChoice*>(child)) continue;
#endif
        child->SetBackgroundColour(background);
    }
    Bind(wxEVT_SYS_COLOUR_CHANGED,[this,panel](wxSysColourChangedEvent& event) {
        const auto background=StateColor::darkModeColorFor(*wxWHITE);
        const auto foreground=StateColor::darkModeColorFor(wxColour("#262e30"));
        SetBackgroundColour(background); panel->SetBackgroundColour(background);
        for (auto* child:panel->GetChildren()) {
            child->SetForegroundColour(foreground);
#ifdef __APPLE__
            if (dynamic_cast<wxButton*>(child) || dynamic_cast<wxChoice*>(child)) continue;
#endif
            child->SetBackgroundColour(background);
        }
        for (const auto& bubble:m_bubbles) bubble.first->theme(wxGetApp().dark_mode());
        Refresh(); event.Skip();
    });
    auto& config = *wxGetApp().app_config;
    auto host = config.get("studio_assistant_host");
    if (host.empty()) { host = "urn:uuid:" + boost::uuids::to_string(boost::uuids::random_generator()()); config.set("studio_assistant_host", host); config.save(); }
    m_registrations = Json::parse(config.get("studio_assistant_accounts"), nullptr, false);
    if (!m_registrations.is_array()) m_registrations = Json::array();
    const auto active = config.get("studio_assistant_active");
    for (size_t i=0;i<m_registrations.size();++i) if (m_registrations[i].value("client_id", "") == active) m_active = int(i);
    if (m_active < 0 && !m_registrations.empty()) m_active = 0;
    const std::string service_prefix = "Charms Studio Assistant/" + base64url(sha256(host)) + "/";
    Callbacks callbacks;
#ifdef __APPLE__
    callbacks.load_secret=[service_prefix](const std::string& client) { return keychain_load(service_prefix+base64url(sha256(client))); };
    callbacks.save_secret=[service_prefix](const std::string& client,const Json& data) { keychain_save(service_prefix+base64url(sha256(client)),data); };
    callbacks.delete_secret=[service_prefix](const std::string& client) { return keychain_delete(service_prefix+base64url(sha256(client))); };
    callbacks.authorize_secret=[service_prefix](const std::string& client) { return keychain_load(service_prefix+base64url(sha256(client)),true); };
#else
    callbacks.load_secret = [service_prefix](const std::string& client) {
        auto store = wxSecretStore::GetDefault(); wxString user; wxSecretValue secret;
        if (client.empty() || !store.Load(wxString::FromUTF8(service_prefix + base64url(sha256(client))), user, secret)) return Json::object();
        auto data = Json::parse(std::string(static_cast<const char*>(secret.GetData()), secret.GetSize()), nullptr, false);
        if (!data.is_object()) throw std::runtime_error("The saved ChatGPT connection could not be read. Sign in again.");
        return data;
    };
    callbacks.save_secret = [service_prefix](const std::string& client, const Json& data) {
        auto store = wxSecretStore::GetDefault(); const auto text = data.dump();
        if (!store.IsOk() || !store.Save(wxString::FromUTF8(service_prefix + base64url(sha256(client))), "ChatGPT", wxSecretValue(text.size(), text.data())))
            throw std::runtime_error("ChatGPT credentials could not be saved in the OS credential store. Unlock it and try again.");
    };
    callbacks.delete_secret = [service_prefix](const std::string& client) {
        auto store = wxSecretStore::GetDefault();
        return store.Delete(wxString::FromUTF8(service_prefix + base64url(sha256(client))));
    };
#endif
    const auto lock_directory=std::filesystem::u8path(data_dir())/"assistant";
    std::filesystem::create_directories(lock_directory);
    m_client = std::make_shared<Client>(host, callbacks,(lock_directory/"credentials.lock").u8string());
    m_connect->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { if (m_retry_models) fetch_models(); else connect(false); });
    m_menu->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { menu(); });
    m_models->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
        const int selected = m_models->GetSelection();
        if (selected >= 0) { wxGetApp().app_config->set("studio_assistant_model", m_model_catalog.at(selected).at("slug").get<std::string>()); wxGetApp().app_config->save(); }
    });
    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (!m_busy) send(std::string(m_input->GetValue().utf8_string()));
        else stop();
    });
    m_input->Bind(wxEVT_TEXT,[this](wxCommandEvent& event) { update_composer(); event.Skip(); });
    m_input->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (!m_busy && (event.GetKeyCode()==WXK_RETURN || event.GetKeyCode()==WXK_NUMPAD_ENTER) && !event.ShiftDown() && !event.AltDown()) send(std::string(m_input->GetValue().utf8_string()));
        else event.Skip();
    });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        if (event.CanVeto()) { stop(); Hide(); event.Veto(); }
        else event.Skip();
    });
    Bind(wxEVT_CHAR_HOOK,[this](wxKeyEvent& event) {
        if (event.GetKeyCode()==WXK_ESCAPE && m_busy) stop();
        else event.Skip();
    });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll(); }, m_timer.GetId());
    m_timer.Start(100);
    accounts(); busy(false);
    append("Assistant", "What would you like to make? I can help with colors, arranging pieces, or fixing warnings.");
    if (m_active >= 0) fetch_models();
    CentreOnParent();
}
void AssistantWindow::append(const std::string& who, const std::string& text)
{
    if (who=="Studio") { activity(text,true); return; }
    const bool user=who=="You";
    auto* bubble=new AssistantBubble(m_chat,user,wxString::FromUTF8(text),wxGetApp().dark_mode());
    auto* row=new wxBoxSizer(wxHORIZONTAL);
    if (user) row->AddStretchSpacer();
    row->Add(bubble,0,wxALL,FromDIP(4));
    if (!user) row->AddStretchSpacer();
    m_messages->Add(row,0,wxEXPAND|wxBOTTOM,FromDIP(4));
    m_bubbles.emplace_back(bubble,user);
    if (!user) { m_reply=bubble; m_steps.clear(); m_action_done=true; }
    layout_chat();
}
void AssistantWindow::clear_chat() {
    if (m_log) m_log->append("conversation_reset");
    m_log.reset(); m_log_turn=0; m_logging_turn=false; m_log_warned=false;
    m_reply=nullptr; m_bubbles.clear(); m_steps.clear(); m_messages->Clear(true);
    m_history=Json::array(); m_problem=Json::object(); m_streaming=false; layout_chat();
}
void AssistantWindow::layout_chat(bool follow) {
    // Preserve the reader's position when resizing or reading earlier messages.
    const int unit=std::max(1,FromDIP(10));
    const bool at_end=m_chat->GetViewStart().y*unit+m_chat->GetClientSize().y >= m_chat->GetVirtualSize().y-FromDIP(40);
    const int width=std::max(FromDIP(200),int((m_chat->GetClientSize().x-FromDIP(24))*0.88));
    for (const auto& bubble:m_bubbles) bubble.first->rewrap(width);
    m_chat->FitInside(); m_chat->Layout();
    if (follow && at_end) m_chat->Scroll(-1,(m_chat->GetVirtualSize().y+unit-1)/unit);
}
void AssistantWindow::activity(const std::string& text,bool done,bool failed,bool unconfirmed) {
    if (!m_reply) append("Assistant","");
    const int state=failed?2:unconfirmed?3:done?1:0;
    if (!text.empty()) m_steps.emplace_back(text,state);
    else if (!m_steps.empty()) m_steps.back().second=state;
    m_action_done=done;
    wxString summary;
    // The latest few steps stay readable without turning the transcript into a log.
    const size_t start=m_steps.size()>3?m_steps.size()-3:0;
    if (start) summary=wxString::Format("%zu earlier steps · ",start);
    for (size_t i=start;i<m_steps.size();++i) {
        if (i>start) summary+="\n";
        summary+=wxString::FromUTF8(m_steps[i].second==0?"◦ ":m_steps[i].second==2?"× ":m_steps[i].second==3?"? ":"✓ ")+wxString::FromUTF8(m_steps[i].first);
        if (m_steps[i].second==3) summary+=wxString::FromUTF8(" · check result");
    }
    m_reply->activity(summary); layout_chat();
}
void AssistantWindow::status(const wxString& text)
{
    m_send->SetToolTip(text);
    m_models->SetToolTip(text);
}
Json AssistantWindow::registration() const { return m_active >= 0 && size_t(m_active) < m_registrations.size() ? m_registrations.at(m_active) : Json::object(); }
void AssistantWindow::save_accounts() {
    auto& config = *wxGetApp().app_config;
    config.set("studio_assistant_accounts", m_registrations.dump()); config.set("studio_assistant_active", registration().value("client_id", "")); config.save(); accounts();
}
void AssistantWindow::accounts() {
    m_connect->Show(m_model_catalog.empty()); m_models->Show(!m_model_catalog.empty());
    m_connect->SetLabel(m_busy?(m_fetching_models?"Checking models…":"Connecting…"):
        m_retry_models?"Try connection again":"Continue with ChatGPT");
    Layout();
}
void AssistantWindow::menu() {
    wxMenu options;
    auto add=[&](const wxString& label,const std::function<void()>& action,bool enabled=true) {
        auto* item=options.Append(wxID_ANY,label); item->Enable(enabled);
        options.Bind(wxEVT_MENU,[action](wxCommandEvent&) { action(); },item->GetId());
    };
    add("New conversation",[this] { clear_chat(); append("Assistant","Ready for something new."); },!m_busy);
    add("Open conversation logs…",[] {
        const auto folder=std::filesystem::u8path(data_dir())/"assistant"/"conversations";
        std::error_code error; std::filesystem::create_directories(folder,error);
#ifndef _WIN32
        if (!error) std::filesystem::permissions(folder,std::filesystem::perms::owner_all,std::filesystem::perm_options::replace,error);
#endif
        if (error || !wxLaunchDefaultApplication(wxString::FromUTF8(folder.u8string())))
            wxMessageBox("The conversation log folder could not be opened.","Studio assistant",wxOK|wxICON_INFORMATION);
    });
    auto* confirm=options.AppendCheckItem(wxID_ANY,"Confirm every project edit"); confirm->Check(!m_allow_changes); confirm->Enable(!m_busy);
    options.Bind(wxEVT_MENU,[this](wxCommandEvent& event) {
        m_allow_changes=!event.IsChecked(); wxGetApp().app_config->set_bool("studio_assistant_allow_changes",m_allow_changes); wxGetApp().app_config->save();
    },confirm->GetId());
    options.AppendSeparator();
    for (size_t i=0;i<m_registrations.size();++i) {
        auto* item=options.AppendRadioItem(wxID_ANY,wxString::FromUTF8(m_registrations[i].value("email","ChatGPT account")+" · "+std::to_string(i+1)));
        item->Check(int(i)==m_active); item->Enable(!m_busy);
        options.Bind(wxEVT_MENU,[this,i](wxCommandEvent&) {
            m_active=int(i); clear_chat(); m_model_catalog=Json::array(); m_models->Clear(); save_accounts(); fetch_models();
        },item->GetId());
    }
    add("Sign in to ChatGPT…",[this] { connect(false); },!m_busy);
#ifdef __APPLE__
    add("Authorize Keychain access…",[this] {
        auto client=m_client; const auto account=registration();
        start([client,account](const auto& work) {
            client->authorize_credentials(account,work->cancel);
            work->post({{"event","models"},{"models",client->models(account,work->cancel)}});
        });
    },!m_busy && m_active>=0);
#endif
    add("Add account…",[this] { connect(true); },!m_busy);
    add("Sign out",[this] {
        const auto account=registration(); auto client=m_client;
        start([client,account](const auto& work) { work->post({{"event","signed_out"},{"revoked",client->sign_out(account,work->cancel)}}); });
    },!m_busy && m_active>=0);
    add("Manage usage…",[] { wxLaunchDefaultBrowser("https://chatgpt.com/settings/usage"); });
    if (!m_problem.empty()) add("Copy last problem details",[this] {
        const auto text=wxString::FromUTF8(m_problem.dump(2,' ',false,Json::error_handler_t::replace));
        if (wxTheClipboard->Open()) {
            wxTheClipboard->SetData(new wxTextDataObject(text));
            wxTheClipboard->Close();
        }
    });
    PopupMenu(&options,m_menu->GetPosition()+wxPoint(0,m_menu->GetSize().y));
}
void AssistantWindow::busy(bool value) {
    m_busy = value;
    m_connect->Enable(!value); m_models->Enable(!value);
    update_composer();
    for (auto* button:m_quick) button->Enable(!value && !m_model_catalog.empty());
    accounts();
}
void AssistantWindow::update_composer() {
    const bool stopping=m_busy && m_work && m_work->cancel;
    m_send->SetLabel(stopping?"Stopping…":m_busy?"Stop":"Send");
    const auto draft=std::string(m_input->GetValue().utf8_string());
    m_send->Enable(m_busy?!stopping:!m_model_catalog.empty() && draft.find_first_not_of(" \r\n\t")!=std::string::npos);
    m_input->SetToolTip(m_busy?"You can write your next message while I work. Escape stops further steps.":"Enter to send. Shift+Enter for a new line.");
}
void AssistantWindow::stop() {
    if (!m_busy || !m_work || m_work->cancel) return;
    m_stopped_native=!m_pending.empty() && m_pending!="library";
    if (m_logging_turn && m_log) m_log->append("stop_requested",{{"turn",m_log_turn},{"pending_operation",m_pending},
        {"native_operation_may_finish",m_stopped_native}});
    m_work->stop(); m_actions.cancel_library(); m_pending.clear();
    update_composer();
    status("Stopping further steps. An operation already running in Studio may finish.");
}
void AssistantWindow::start(const std::function<void(const std::shared_ptr<Work>&)>& fn) {
    if (m_busy) return;
    m_work = std::make_shared<Work>(); m_stopped_native=false; busy(true); m_pending.clear();
    auto work = m_work;
    std::thread([work, fn] {
        try { fn(work); }
        catch (const RequestError& error) { work->post({{"event","error"},{"message",error.what()},{"details",error.diagnostics()}}); }
        catch (const Json::exception&) { work->post({{"event","error"},{"message","ChatGPT returned an unreadable reply. Please try again."}}); }
        catch (const std::exception& error) { work->post({{"event", "error"}, {"message", error.what()}}); }
        catch (...) { work->post({{"event", "error"}, {"message", "The assistant could not complete this operation."}}); }
        work->post({{"event", "idle"}});
    }).detach();
}
void AssistantWindow::connect(bool add) {
    if (m_busy) return;
    m_retry_models=false;
    const auto account = add ? Json::object() : registration(); auto client = m_client;
    status("Finish ChatGPT sign-in in your browser. Authorize plan usage for Charms Studio Assistant.");
    start([client, account](const auto& work) {
        // This client instance is used by one task at a time. Browser requests are
        // queued to the main thread; no GUI APIs run on the network worker.
        work->post({{"event", "connect_start"}});
        const auto connected = client->sign_in(account, work->cancel, [work](const std::string& url) {
            work->post({{"event", "open_browser"}, {"url", url}});
        }, [work](const Json& pending) {
            work->post({{"event", "registration_issued"}, {"account", pending}});
        });
        work->post({{"event", "signed_in"}, {"account", connected}});
    });
}
void AssistantWindow::fetch_models() {
    if (m_busy || m_active < 0) return;
    m_fetching_models=true;
    auto client = m_client; const auto account = registration();
    status("Checking models available to this ChatGPT account…");
    start([client, account](const auto& work) { work->post({{"event", "models"}, {"models", client->models(account, work->cancel)}}); });
}
void AssistantWindow::send(const std::string& text, bool clear_draft) {
    if (m_busy || text.find_first_not_of(" \r\n\t") == std::string::npos) return;
    if (text.size() > 16000) { append("Studio", "Please shorten the message to 16,000 characters."); return; }
    const int selected = m_models->GetSelection();
    if (m_active < 0 || selected < 0) { append("Studio", "Connect ChatGPT and select an available model first."); return; }
    if (!m_log) {
        m_log=std::make_shared<ConversationLog>(std::filesystem::u8path(data_dir())/"assistant"/"conversations");
        Json metadata={{"tools",tool_definitions()},{"app_version",SLIC3R_VERSION}};
#ifdef SLIC3R_COMPILE_VERSION
        metadata["build_version"]=SLIC3R_COMPILE_VERSION;
#endif
        m_log->append("conversation_start",metadata);
    }
    check_log();
    const auto log=m_log; const auto turn=++m_log_turn;
    const auto model = m_model_catalog.at(selected).at("slug").get<std::string>();
    log->append("user_message",{{"turn",turn},{"model",model},{"text",text}});
    log->append("turn_options",{{"turn",turn},{"confirm_every_project_edit",!m_allow_changes}});
    Json context;
    try { context = m_actions.inspect(); }
    catch (const std::exception& error) {
        log->append("turn_end",{{"turn",turn},{"status","error"},{"phase","initial_inspection"},{"message",error.what()}});
        append("Assistant",error.what());check_log();return;
    }
    m_logging_turn=true;
    append("You", text); append("Assistant", ""); activity("Thinking…");
    if (clear_draft) m_input->Clear();
    m_streaming = false;
    auto client = m_client; const auto account = registration(), history = m_history;
    status("Inspecting the project and working on your request…");
    start([client, account, history, context, text, model,log,turn](const auto& work) {
        SessionCallbacks callbacks;
        callbacks.trace=[log,turn](const std::string& event,Json data) { data["turn"]=turn;log->append(event,data); };
        callbacks.cancelled=[work] { return work->cancel.load(); };
        callbacks.tool=[work](const std::string& name,const Json& args) { return work->tool(name,args); };
        callbacks.respond=[client,account,work,log,turn](const Json& request) {
            return client->respond(account,request,work->cancel,[work,log,turn](const std::string& delta) {
                log->append("assistant_delta",{{"turn",turn},{"text",delta}});
                work->post({{"event","delta"},{"text",delta}});
            });
        };
        const auto input=run_turn(history,context,text,model,callbacks);
        work->post({{"event","done"},{"history",input}});
    });
}
void AssistantWindow::handle_tool(const Json& event) {
    if (!m_work || m_work->cancel) return;
    const auto tool = event.at("name").get<std::string>(); const auto args = event.at("arguments");
    m_streaming = false;
    try {
        // Confirm high-impact actions; the preference can also make all edits
        // reviewable. Inspection, selection, and slicing need no extra dialog.
        if (requires_confirmation(tool) || (tool=="assign_part_filament" && args.value("replace_color_paint",false)) || (changes_project(tool) && !m_allow_changes)) {
            const std::string question = confirmation_text(tool,args);
            if (m_log) m_log->append("confirmation_requested",{{"turn",m_log_turn},{"tool",tool},{"arguments",args},{"question",question}});
            const bool approved=wxMessageBox(wxString::FromUTF8(question), "Studio assistant", wxYES_NO|wxNO_DEFAULT|wxICON_QUESTION, this)==wxYES;
            if (m_log) m_log->append("confirmation_decision",{{"turn",m_log_turn},{"tool",tool},{"approved",approved}});
            if (!approved) {
                if (m_steps.size()==1 && m_steps.front().first=="Thinking…") m_steps.clear();
                activity(action_label(tool,args)+" skipped",true,true);
                m_work->answer({{"cancelled", true}, {"message", "The person declined this action. Do not retry it without a new request."}}); return;
            }
        }
        // Replace the initial placeholder; actual actions stay under this reply.
        if (m_steps.size()==1 && m_steps.front().first=="Thinking…") m_steps.clear();
        activity(action_label(tool,args));
        status(wxString::FromUTF8(action_label(tool,args)+"…"));
        auto result = m_actions.execute(tool, args);
        if (result.contains("pending")) {
            m_pending = result.at("pending"); m_pending_since = std::chrono::steady_clock::now();
            if (m_log) m_log->append("tool_pending",{{"turn",m_log_turn},{"tool",tool},{"operation",m_pending}});
            return;
        }
        activity("",true,result.contains("error") || result.contains("operation_error") || result.value("cancelled",false),result.value("outcome","")=="unconfirmed"); m_work->answer(result);
    } catch (const std::exception& e) {
        activity("",true,true); m_work->answer({{"error", e.what()}, {"instruction", "Inspect the current state before deciding whether another action is appropriate."}});
    }
}
void AssistantWindow::poll() {
    if (!m_work) return;
    if (!m_pending.empty() && !m_work->cancel) {
        const auto elapsed = std::chrono::steady_clock::now() - m_pending_since;
        auto* p = wxGetApp().plater();
        if (elapsed > std::chrono::minutes(5)) { m_actions.cancel_library(); m_pending.clear(); activity("",true,true); m_work->answer({{"error", "Studio's operation did not finish within five minutes. Check Studio before trying again."}}); }
        else if (m_pending == "library") {
            try { if (auto result = m_actions.poll_library()) { m_pending.clear(); activity("",true,result->contains("error") || result->contains("operation_error") || result->value("cancelled",false)); m_work->answer(*result); } }
            catch (const std::exception& e) { m_pending.clear(); activity("",true,true); m_work->answer({{"error",e.what()}}); }
        }
        else if (elapsed > std::chrono::milliseconds(700) && p && !p->is_background_process_slicing() && !p->is_any_job_running() && !p->is_background_process_update_scheduled()) {
            const auto pending = m_pending; m_pending.clear();
            try {
                auto result = m_actions.inspect();
                if (pending == "slice" && !p->get_partplate_list().get_curr_plate()->is_slice_result_valid()) result["operation_error"] = "Slicing did not produce a valid result. Inspect diagnostics; do not claim success.";
                activity("",true,result.contains("operation_error")); m_work->answer(result);
            } catch (const std::exception& e) { activity("",true,true); m_work->answer({{"error", e.what()}}); }
        }
    }
    std::deque<Json> events;
    { std::lock_guard<std::mutex> lock(m_work->mutex); events.swap(m_work->events); }
    for (const auto& event : events) {
        const auto type = event.at("event").get<std::string>();
        if (type == "delta") {
            if (m_work->cancel) continue;
            if (!m_reply) append("Assistant", "");
            if (m_steps.size()==1 && m_steps.front().first=="Thinking…") { m_steps.clear(); m_reply->activity(""); m_action_done=true; }
            m_reply->delta(wxString::FromUTF8(event.at("text").get<std::string>()),!m_streaming);
            m_streaming=true; layout_chat();
        } else if (type == "tool") handle_tool(event);
        else if (type == "open_browser") {
            if (!m_work->cancel && !wxLaunchDefaultBrowser(wxString::FromUTF8(event.at("url").get<std::string>()))) m_work->stop();
        } else if (type == "signed_in" || type == "registration_issued") {
            const auto account = event.at("account"); int index = -1;
            for (size_t i=0;i<m_registrations.size();++i) if (m_registrations[i].at("client_id") == account.at("client_id")) index = int(i);
            if (index < 0) { m_registrations.push_back(account); index = int(m_registrations.size())-1; } else m_registrations[index] = account;
            m_active = index; m_history = Json::array(); m_model_catalog = Json::array(); m_models->Clear(); save_accounts();
            if (type == "signed_in") {
                m_retry_models=true;
                append("Studio", "ChatGPT connected. Checking available models…");
                m_deferred_models_refresh.request();
            }
        } else if (type == "signed_out") {
            m_retry_models=false;
            m_history = Json::array(); m_model_catalog = Json::array(); m_models->Clear();
            append("Studio", event.at("revoked").get<bool>() ? "Signed out of ChatGPT." : "Signed out locally. Remote revocation could not be confirmed; you can disconnect the app in ChatGPT Settings.");
            status("Signed out. Continue with ChatGPT to reconnect this account.");
        } else if (type == "models") {
            m_retry_models=false;
            m_model_catalog = event.at("models"); m_models->Clear(); int selected = 0;
            const auto saved = wxGetApp().app_config->get("studio_assistant_model");
            for (size_t i=0;i<m_model_catalog.size();++i) {
                const auto slug=m_model_catalog[i].at("slug").get<std::string>();
                m_models->Append(wxString::FromUTF8(m_model_catalog[i].at("name").get<std::string>()+(slug=="gpt-6-luna"?" (Max)":"")));
                if (m_model_catalog[i].at("slug") == saved) selected = int(i);
            }
            if (!m_model_catalog.empty()) m_models->SetSelection(selected);
            status("Ready. Requests use the connected ChatGPT plan; Manage usage controls its allowance.");
        } else if (type == "done") {
            if (m_work->cancel) continue;
            m_history = event.at("history");
            // Never silently trim tool-call pairs. A fresh conversation is safer
            // than replaying orphan outputs or sending unbounded project history.
            if (m_history.dump().size() > 1500000) {
                m_history = Json::array();
                if (m_log) m_log->append("context_reset",{{"turn",m_log_turn},{"reason","history_size_limit"}});
                append("Studio", "This chat got long. Your next message will start a fresh conversation.");
            }
            status("Ready. Review changes in Studio; Undo is available.");
        } else if (type == "error") {
            // Cancellation is acknowledged once at idle, after all queued text
            // and completion events have been discarded. A curl error caused
            // by Stop is not a new failure to ask the person to troubleshoot.
            if (m_work->cancel) continue;
            if (event.value("details",Json::object()).value("recovery","")=="sign_in") {
                m_retry_models=false; m_model_catalog=Json::array(); m_models->Clear();
            }
            else if (m_fetching_models) m_retry_models=true;
            m_problem=event.value("details",Json::object());
            if (m_reply && !m_action_done) activity("",true,true);
            const auto message=event.at("message").get<std::string>();
            if (m_reply && m_reply->empty()) { m_reply->text(wxString::FromUTF8(message)); layout_chat(); }
            else append("Assistant",message);
            m_streaming = false; m_history = Json::array();
            if (m_logging_turn && m_log) m_log->append("context_reset",{{"turn",m_log_turn},{"reason","error"}});
            status("Stopped. Review the current project before retrying. Changes already applied remain available through Undo.");
        } else if (type == "idle") {
            if (m_work->cancel) {
                if (m_fetching_models) m_retry_models=true;
                if (m_reply && !m_action_done) activity("",true,true);
                std::string message="Stopped. You can review any changes in Studio and use Undo.";
                if (m_stopped_native) message+=" An operation already running may finish.";
                if (m_reply && m_reply->empty()) { m_reply->text(wxString::FromUTF8(message)); layout_chat(); }
                else append("Assistant",message);
                m_history=Json::array(); m_streaming=false;
                status("Stopped. Ready for your next message.");
            }
            m_logging_turn=false; m_fetching_models=false; busy(false); m_pending.clear();
        }
    }
    if (m_deferred_models_refresh.take(m_busy,m_work->cancel.load())) fetch_models();
    if (!m_busy) check_log();
}
void AssistantWindow::check_log() {
    if (!m_log || m_log_warned) return;
    const auto problem=m_log->problem();
    if (problem.empty()) return;
    m_log_warned=true; append("Assistant",problem+" Your assistant can still work; this conversation's log may be incomplete.");
}
}

void show_studio_assistant(wxWindow* parent)
{
    static wxWeakRef<AssistantWindow> window;
    if (!window) window = new AssistantWindow(parent);
    window->Show(); window->Raise(); window->focus_composer();
}
}}
