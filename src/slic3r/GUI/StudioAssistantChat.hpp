#ifndef slic3r_StudioAssistantChat_hpp_
#define slic3r_StudioAssistantChat_hpp_

#include <wx/panel.h>
#include <wx/textctrl.h>
#include <wx/textwrapper.h>
#include <wx/sizer.h>
#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <algorithm>

namespace Slic3r { namespace GUI {

// Native, selectable chat cards. No webview, HTML, or model-generated
// markup is involved. The original strings are retained so resizing can rewrap.
class AssistantBubble : public wxPanel {
public:
    AssistantBubble(wxWindow* parent, bool user, const wxString& text, bool dark)
        : wxPanel(parent), m_user(user), m_body(text)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        auto* column = new wxBoxSizer(wxVERTICAL);
        const long style=wxTE_MULTILINE|wxTE_READONLY|wxTE_NO_VSCROLL|wxBORDER_NONE;
        m_text = new wxTextCtrl(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, style);
        m_activity = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, style);
        m_text->SetName(user?"You":"Assistant"); m_activity->SetName("Studio actions");
        auto small = m_activity->GetFont(); small.SetPointSize(std::max(9, small.GetPointSize()-1));
        m_activity->SetFont(small);
        column->Add(m_text, 0, wxEXPAND|wxALL, FromDIP(12));
        column->Add(m_activity, 0, wxEXPAND|wxLEFT|wxRIGHT|wxBOTTOM, FromDIP(12));
        m_activity->Hide(); SetSizer(column);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour())); dc.Clear();
            dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(GetBackgroundColour()));
            dc.DrawRoundedRectangle(GetClientRect(), FromDIP(12));
        });
        auto copy = [this](wxContextMenuEvent&) {
            wxMenu menu;
            if (m_text->HasSelection()) {
                auto* selected=menu.Append(wxID_ANY,"Copy selected text");
                menu.Bind(wxEVT_MENU,[this](wxCommandEvent&) { m_text->Copy(); },selected->GetId());
            }
            auto* item=menu.Append(wxID_ANY,"Copy message");
            menu.Bind(wxEVT_MENU,[this](wxCommandEvent&) {
                if (wxTheClipboard->Open()) { wxTheClipboard->SetData(new wxTextDataObject(m_body)); wxTheClipboard->Close(); }
            }, item->GetId());
            PopupMenu(&menu);
        };
        Bind(wxEVT_CONTEXT_MENU,copy); m_text->Bind(wxEVT_CONTEXT_MENU,copy);
        theme(dark);
    }
    void theme(bool dark) {
        const wxColour background=m_user ? (dark?wxColour("#234d40"):wxColour("#dff3e9")) :
                                                   (dark?wxColour("#343839"):wxColour("#f0f2f3"));
        SetBackgroundColour(background);
        m_text->SetBackgroundColour(background); m_activity->SetBackgroundColour(background);
        m_text->SetForegroundColour(dark?wxColour("#f3f5f5"):wxColour("#262e30"));
        m_activity->SetForegroundColour(dark?wxColour("#b8c2c1"):wxColour("#53625f"));
        Refresh();
    }
    bool empty() const { return m_body.empty(); }
    void text(const wxString& value) { if (m_body!=value) { m_body=value; m_dirty=true; } }
    void delta(const wxString& value, bool paragraph) { if (paragraph && !m_body.empty()) m_body+="\n\n"; m_body+=value; m_dirty=true; }
    void activity(const wxString& value) { if (m_steps!=value) { m_steps=value; m_dirty=true; } }
    void rewrap(int width) {
        // Streaming updates only need to measure the changing reply. Rewrapping
        // the entire transcript for every token makes longer chats sluggish.
        const auto text_font=m_text->GetFont(), activity_font=m_activity->GetFont();
        if (!m_dirty && m_wrap_width==width && m_wrap_text_font==text_font && m_wrap_activity_font==activity_font) return;
        m_dirty=false; m_wrap_width=width;
        m_wrap_text_font=text_font; m_wrap_activity_font=activity_font;
        const int inner=std::max(FromDIP(80),width-FromDIP(24));
        if (m_text->GetValue()!=m_body) m_text->ChangeValue(m_body);
        if (m_activity->GetValue()!=m_steps) m_activity->ChangeValue(m_steps);
        m_text->Show(!m_body.empty()); m_activity->Show(!m_steps.empty());
        // Measure wrapped lines with the same font as each native text view.
        // Leave room for Cocoa/Windows' text-container insets and line leading.
        struct Lines : wxTextWrapper {
            int count{0};
            void OnOutputLine(const wxString&) override { ++count; }
        };
        for (auto* view:{m_text,m_activity}) {
            Lines lines; lines.Wrap(view,view->GetValue(),inner-FromDIP(12));
            view->SetMinSize(wxSize(inner,std::max(1,lines.count)*(view->GetCharHeight()+FromDIP(2))+FromDIP(8)));
        }
        SetMinSize(wxDefaultSize);
        SetMinSize(wxSize(width,GetSizer()->CalcMin().y));
        Layout();
    }
private:
    bool m_user;
    bool m_dirty{true};
    int m_wrap_width{0};
    wxFont m_wrap_text_font, m_wrap_activity_font;
    wxString m_body, m_steps;
    wxTextCtrl *m_text, *m_activity;
};
}}
#endif
