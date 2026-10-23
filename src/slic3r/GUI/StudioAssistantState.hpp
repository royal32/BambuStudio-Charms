#ifndef slic3r_StudioAssistantState_hpp_
#define slic3r_StudioAssistantState_hpp_

#include "StudioAssistantClient.hpp"
#include <set>

namespace Slic3r { namespace GUI { namespace Assistant {
inline Json selection_revision(int mode,const std::map<int,std::set<int>>& content,const std::set<unsigned int>& volumes)
{
    // Object/instance membership alone cannot distinguish two selected parts
    // in the same instance, or full-instance selection from part selection.
    return {{"mode",mode},{"instances",content},{"volumes",volumes}};
}

inline Json unconfirmed_project_open(Json project)
{
    // Plater's return code describes its close/save dialog, and its input path
    // is assigned before parsing. Neither confirms that loading succeeded.
    project["outcome"]="unconfirmed";
    project["status"]="The project load operation returned. This does not confirm the requested project opened.";
    project["instruction"]="Check the current project and its objects and parts before claiming the requested source opened. Native loading may have failed or been cancelled. Do not reopen the project without a new request from the person.";
    return project;
}
}}}
#endif
