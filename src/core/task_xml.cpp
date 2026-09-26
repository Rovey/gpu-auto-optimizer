#include "core/task_xml.hpp"

namespace gao {

namespace {
std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}
}

std::string boot_task_xml(const std::string& exe_path_utf8, const std::string& arguments,
                          const std::string& user_id, const std::string& time_limit) {
    const std::string user = xml_escape(user_id);
    return "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
           "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
           "  <RegistrationInfo><Description>Re-applies the saved GPU Auto Optimizer profile at logon.</Description></RegistrationInfo>\n"
           "  <Triggers>\n"
           "    <LogonTrigger><Enabled>true</Enabled><UserId>" + user + "</UserId><Delay>PT15S</Delay></LogonTrigger>\n"
           "  </Triggers>\n"
           "  <Principals>\n"
           "    <Principal id=\"Author\"><UserId>" + user + "</UserId><LogonType>InteractiveToken</LogonType>"
           "<RunLevel>HighestAvailable</RunLevel></Principal>\n"
           "  </Principals>\n"
           "  <Settings>\n"
           "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
           "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
           "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
           "    <ExecutionTimeLimit>" + xml_escape(time_limit) + "</ExecutionTimeLimit>\n"
           "    <Enabled>true</Enabled>\n"
           "  </Settings>\n"
           "  <Actions Context=\"Author\">\n"
           "    <Exec><Command>" + xml_escape(exe_path_utf8) + "</Command><Arguments>" + xml_escape(arguments) + "</Arguments></Exec>\n"
           "  </Actions>\n"
           "</Task>\n";
}

}
