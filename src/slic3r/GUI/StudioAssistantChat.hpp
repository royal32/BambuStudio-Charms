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
#include <cstdlib>
#include "StudioAssistantAppearance.hpp"

namespace Slic3r { namespace GUI {

// Native, selectable chat cards. No webview, HTML, or model-generated
// markup is involved. The original strings are retained so resizing can rewrap.
class AssistantBubble : public wxPanel {
public:
    AssistantBubble(wxWindow* parent, bool user, const wxString& text, bool dark)
        : wxPanel(parent), m_user(user), m_body(text)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        assistant_clip_children(GetHandle());
        auto* column = new wxBoxSizer(wxVERTICAL);
        const long style=wxTE_MULTILINE|wxTE_READONLY|wxTE_NO_VSCROLL|wxBORDER_NONE;
        m_text = new wxTextCtrl(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, style);
        m_activity = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, style);
        m_text->SetName(user?"You":"Assistant"); m_activity->SetName("Studio actions");
        auto activity_font = m_activity->GetFont();
        activity_font.SetPointSize(std::max(9, activity_font.GetPointSize()-1));
        m_activity->SetFont(activity_font);
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
        assistant_transparent_text_background(m_text->GetHandle());
        assistant_transparent_text_background(m_activity->GetHandle());
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
        const int inner=std::max(FromDIP(1),width-FromDIP(24));
        const int wrap_width=std::max(FromDIP(1),inner-FromDIP(12));
        if (const auto previous=m_text->GetValue(); previous!=m_body) {
            long from=0,to=0; m_text->GetSelection(&from,&to);
            // New stream text must not discard or extend the reader's range.
            // A replacement reply keeps the native selection reset instead.
            const bool preserve=from>=0 && from<to && to<=m_text->GetLastPosition() && m_body.StartsWith(previous);
            m_text->ChangeValue(m_body);
            if (preserve) m_text->SetSelection(from,to);
        }
        if (m_activity->GetValue()!=m_steps) m_activity->ChangeValue(m_steps);
        m_text->Show(!m_body.empty()); m_activity->Show(!m_steps.empty());
        // Measure wrapped lines with the same font as each native text view.
        // Leave room for Cocoa/Windows' text-container insets and line leading.
        struct Lines : wxTextWrapper {
            Lines(wxWindow* view,int width) : dc(view), width(width) { dc.SetFont(view->GetFont()); }
            int count{0};
            wxClientDC dc;
            int width;
            void OnOutputLine(const wxString& line) override {
                if (dc.GetTextExtent(line).x<=width) { ++count; return; }
                wxArrayInt extents;
                if (!dc.GetPartialTextExtents(line,extents) || extents.size()!=line.length()) {
                    // A port returning differently indexed metrics must not
                    // index outside wxString or underestimate the text height.
                    count+=std::max<size_t>(1,line.length()); return;
                }
                // Make advances monotonic even for combining/bidirectional
                // glyph positions. Measure once, then count without substrings.
                int advance=0,previous=0;
                for (auto& extent:extents) { const int position=extent; advance+=std::abs(position-previous); extent=advance; previous=position; }
                const wxString separators(" \t/\\-._?&=#");
                size_t start=0;
                do {
                    ++count;
                    // wxTextWrapper leaves an oversized word on one line.
                    // Native text controls still wrap filenames, URLs and CJK.
                    // Work in wxString characters, preserving the display text.
                    const int origin=start?extents[start-1]:0;
                    if (extents.back()-origin<=width) return;
                    size_t cut=std::max(start+1,size_t(std::lower_bound(extents.begin()+start,extents.end(),origin+width)-extents.begin()));
                    for (size_t boundary=cut;boundary>start;--boundary) {
                        if (separators.Find(line[boundary-1])!=wxNOT_FOUND) { cut=boundary; break; }
                    }
                    start=cut;
                } while (start<extents.size());
            }
        };
        for (auto* view:{m_text,m_activity}) {
            Lines lines(view,wrap_width); lines.Wrap(view,view->GetValue(),wrap_width);
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
