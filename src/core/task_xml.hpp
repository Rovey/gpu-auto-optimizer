#pragma once
#include <string>

namespace gao {

// Task Scheduler definition for the logon task: starts on battery, at most
// 5 minutes, one instance, 15 s after logon of `user_id` (a SID string),
// with highest privileges. The caller writes it as UTF-16LE with a BOM.
// The path goes in <Command> on its own (arguments are separate), so it
// needs XML escaping, not shell quoting.
std::string boot_task_xml(const std::string& exe_path_utf8, const std::string& user_id);

}
