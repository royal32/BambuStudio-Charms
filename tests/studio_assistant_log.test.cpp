#include "slic3r/GUI/StudioAssistantLog.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <thread>

using namespace Slic3r::GUI::Assistant;
namespace fs=std::filesystem;

std::vector<Json> records(const fs::path& directory) {
    std::ifstream input(directory/"events.jsonl");
    assert(input);
    std::vector<Json> result;std::string line;
    while (std::getline(input,line)) result.push_back(Json::parse(line));
    return result;
}
int main() {
    const auto root=fs::temp_directory_path()/("charms-conversation-check-"+std::to_string(std::random_device{}())+"-"+std::to_string(std::random_device{}()));
    struct Cleanup { fs::path path;~Cleanup(){std::error_code ignored;fs::remove_all(path,ignored);} } cleanup{root};
    fs::path first;
    {
        ConversationLog log(root);
        assert(log.problem().empty());first=log.directory();
        log.append("turn_start",{{"message","Please recolor the inner pieces — café"},{"context",{{"revision","r1"},{"objects",Json::array({{{"id","part-17"},{"filament_slot",2}}})}}}});
        log.append("tool_call",{{"arguments",Json({{"revision","r1"},{"slot",3},{"access_token","token-secret"},{"nested",{{"Password","password-secret"}}}}).dump()}});
        const std::string png="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl6E3sAAAAASUVORK5CYII=";
        log.append("tool_result",{{"result",{{"image_data",png},{"direction","bottom"},{"refresh_token","refresh-secret"}}}});
        log.append("assistant_delta",{{"text","Please ask-for a mask-outline. Bearer abcdef-secret sk-12345678901234567890"}});
        // These must already be readable, without closing the logger.
        auto rows=records(first);assert(rows.size()==4);
        assert(rows[0]["data"]["context"]["objects"][0]["id"]=="part-17");
        assert(rows[1]["data"]["arguments"]["slot"]==3);
        assert(rows[1]["data"]["arguments"]["access_token"]=="[redacted]");
        assert(rows[1]["data"]["arguments"]["nested"]["Password"]=="[redacted]");
        assert(rows[2]["data"]["result"]["refresh_token"]=="[redacted]");
        assert(rows[3]["data"]["text"]=="Please ask-for a mask-outline. [omitted] [omitted]");
        const auto image=rows[2]["data"]["result"]["image_data"];
        assert(image["attachment"]=="image-1.png");
        assert(fs::file_size(first/image["attachment"].get<std::string>())==image["bytes"].get<size_t>());
        std::ifstream pixels(first/"image-1.png",std::ios::binary);std::string signature(8,'\0');pixels.read(signature.data(),8);
        assert(signature==std::string("\x89PNG\r\n\x1a\n",8));
#ifndef _WIN32
        for(const auto& path:{root,first,first/"events.jsonl",first/"image-1.png"}) {
            const auto permissions=fs::status(path).permissions();
            assert((permissions&(fs::perms::group_all|fs::perms::others_all))==fs::perms::none);
        }
#endif
        // UI confirmation and worker output can arrive on different threads.
        auto writer=[&](int thread){for(int i=0;i<30;++i)log.append("parallel",{{"writer",thread},{"index",i}});};
        std::thread a(writer,1),b(writer,2);a.join();b.join();
        rows=records(first);assert(rows.size()==64);
        for(size_t i=0;i<rows.size();++i) {
            assert(rows[i]["sequence"]==i+1);assert(rows[i]["schema_version"]==1);
            assert(rows[i]["time_utc"].get<std::string>().back()=='Z');
            if(i)assert(rows[i]["elapsed_ms"]>=rows[i-1]["elapsed_ms"]);
        }
        // Refuse an attachment collision instead of overwriting another file.
        {std::ofstream sentinel(first/"image-2.png");sentinel<<"keep me";}
        log.append("tool_result",{{"image_data",png}});
        assert(!log.problem().empty());
        log.append("after_failure");assert(records(first).size()==64);
        std::ifstream sentinel(first/"image-2.png");std::string content;std::getline(sentinel,content);assert(content=="keep me");
    }
    {
        ConversationLog next(root);assert(next.problem().empty());
        assert(next.directory()!=first);next.append("new_conversation");
        assert(records(next.directory()).size()==1);assert(records(first).size()==64);
        next.append("tool_result",{{"image_data","data:image/png;base64,not-a-png"}});
        assert(records(next.directory()).back()["data"]["image_data"].contains("omitted"));
        assert(!fs::exists(next.directory()/"image-1.png"));
    }
    {
        ConversationLog unavailable(first/"events.jsonl");
        assert(!unavailable.problem().empty());unavailable.append("ignored");
    }
    std::cout<<"PASS: flushed persistent events, separate images, redaction, concurrent ordering, separate conversations and nonfatal storage failure.\n";
}
