#include <wx/app.h>
#include <wx/frame.h>
#include "slic3r/GUI/StudioAssistantChat.hpp"
#include <iostream>
#include <stdexcept>
#include <vector>

using Slic3r::GUI::AssistantBubble;
namespace {
int result=1;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
wxTextCtrl* view(AssistantBubble* bubble,const wxString& name) {
    for (auto* child:bubble->GetChildren())
        if (auto* text=dynamic_cast<wxTextCtrl*>(child); text && text->GetName()==name) return text;
    throw std::runtime_error("Missing accessible chat text view");
}
class ChatChecks : public wxApp {
public:
    bool OnInit() override {
        bool review=false;
        for (int i=1;i<argc;++i) review|=argv[i]=="--review";
        auto* frame=new wxFrame(nullptr,wxID_ANY,"Synthetic assistant chat checks",wxDefaultPosition,wxSize(1000,850));
        auto* columns=new wxBoxSizer(wxHORIZONTAL);
        wxString long_text;
        for (int i=0;i<4;++i) long_text+=wxString::FromUTF8("The inner pieces are now blue. The outer shape stays white. Check the loop in Preview before printing. 🌸\n");
        std::vector<AssistantBubble*> samples;
        for (bool dark:{false,true}) {
            auto* panel=new wxPanel(frame);
            panel->SetBackgroundColour(dark?wxColour("#262e30"):*wxWHITE);
            auto* rows=new wxBoxSizer(wxVERTICAL);
            auto* request=new AssistantBubble(panel,true,"Make the inner pieces a different color.",dark);
            request->rewrap(440); rows->Add(request,0,wxALL,8);
            for (int width:{260,440}) {
                auto* bubble=new AssistantBubble(panel,false,long_text,dark);
                bubble->activity(wxString::FromUTF8("✓ Colors updated\n× Slicing needs attention"));
                bubble->rewrap(width);
                rows->Add(bubble,0,wxALL,8); samples.push_back(bubble);
            }
            panel->SetSizer(rows); columns->Add(panel,1,wxEXPAND);
        }
        frame->SetSizer(columns); frame->Show();
        CallAfter([=] {
            try {
                for (size_t i:{0u,2u}) {
                    require(samples[i]->GetMinSize().y>samples[i+1]->GetMinSize().y,"Narrow messages must grow vertically");
                    for (size_t j=i;j<i+2;++j) {
                        auto* text=view(samples[j],"Assistant");
                        require(text->GetValue()==long_text,"Unicode/line breaks changed while wrapping");
                        require(text->IsShown() && view(samples[j],"Studio actions")->IsShown(),"Message or actions are hidden");
                        text->SetSelection(4,18);
                        const int width=(j%2)?440:260;
                        for (int repeat=0;repeat<10;++repeat) samples[j]->rewrap(width);
                        samples[j]->theme(i==0); samples[j]->rewrap(width);
                        long from=0,to=0; text->GetSelection(&from,&to);
                        require(from==4 && to==18,"Unchanged rewrap/theme reset the reader's selection");
                        samples[j]->theme(i!=0);
                    }
                }
                auto* stream=new AssistantBubble(frame,false,"",false);
                stream->rewrap(260);
                require(stream->empty() && !view(stream,"Assistant")->IsShown(),"Empty reply should hide its body");
                stream->activity("Checking the pieces..."); stream->rewrap(260);
                require(view(stream,"Studio actions")->IsShown(),"Action-only reply disappeared");
                stream->delta("Colors updated.",false); stream->rewrap(260);
                const int short_height=stream->GetMinSize().y;
                stream->delta(long_text,true); stream->rewrap(260);
                require(stream->GetMinSize().y>short_height,"Streaming failed to grow the reply");
                require(view(stream,"Assistant")->GetValue()=="Colors updated.\n\n"+long_text,"Stream paragraphs changed");
                stream->text("Stopped."); stream->activity(""); stream->rewrap(260);
                require(view(stream,"Assistant")->GetValue()=="Stopped." && !view(stream,"Studio actions")->IsShown(),"Final reply replacement left stale content");
                stream->Destroy();
                result=0;
                std::cout<<"PASS: light/dark narrow/wide messages, Unicode, selection retention, action-only replies, streaming growth, and final replacement."<<std::endl;
            } catch (const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<'\n'; }
            if (!review || result) frame->Destroy();
        });
        return true;
    }
};
}
wxIMPLEMENT_APP_NO_MAIN(ChatChecks);
int main(int argc,char** argv) { wxEntry(argc,argv); return result; }
