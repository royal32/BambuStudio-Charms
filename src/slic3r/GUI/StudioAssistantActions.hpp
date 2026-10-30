#ifndef slic3r_StudioAssistantActions_hpp_
#define slic3r_StudioAssistantActions_hpp_
#include "StudioAssistantClient.hpp"
#include <map>
#include <optional>
#include <string>
namespace Slic3r { namespace GUI { namespace Assistant {
// Constructed, inspected, and invoked only on Studio's main thread.
class Actions {
public:
    ~Actions();
    Json inspect();
    Json execute(const std::string& tool, const Json& arguments);
    std::optional<Json> poll_library();
    void cancel_library();
    std::optional<Json> poll_model_work();
    void cancel_model_work();
private:
    struct LibraryTransfer;
    std::shared_ptr<LibraryTransfer> m_transfer;
    struct ModelWork;
    std::shared_ptr<ModelWork> m_model_work;
    Json start_model_work(const std::string& tool,const Json& arguments);
    Json start_library(const std::string& tool, const Json& arguments);
    Json add_library_object(const std::string& staged, const std::string& original, const Json& arguments);
    std::optional<Json> execute_workbench(const std::string& tool,const Json& arguments);
    std::map<std::string,Json> m_surface_targets;
    size_t m_surface_face_count{0};
    std::map<std::string, std::string> m_library_entries;
    std::string m_library_folder;
    std::string library_source(const std::string& id);
};
}}}
#endif
