#ifndef slic3r_StudioAssistantProfiles_hpp_
#define slic3r_StudioAssistantProfiles_hpp_
#include "libslic3r/PrintConfig.hpp"
#include <set>

namespace Slic3r { namespace GUI { namespace Assistant {
inline bool readable_profile_parameter(const std::string& key,const ConfigOptionDef& definition)
{
    if(definition.is_code)return false;
    for(const auto* hidden:{"gcode","script","path","url","host","password","token","api_key","access_code","device_id","serial"})
        if(key.find(hidden)!=std::string::npos)return false;
    if(definition.type==coString || definition.type==coStrings) {
        static const std::set<std::string> labels={"filament_type","filament_vendor","filament_settings_id","printer_model","printer_variant","printer_settings_id","print_settings_id"};
        return labels.count(key);
    }
    return true;
}
}}}
#endif
