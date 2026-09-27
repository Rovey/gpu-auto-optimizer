#include "doctest/doctest.h"
#include "core/task_xml.hpp"
#include <string>

using namespace gao;

namespace {
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
}

TEST_CASE("the logon task XML carries every required setting") {
    const std::string x = boot_task_xml(R"(C:\Program Files\GpuAutoOptimizer\GpuAutoOptimizer.exe)", "--tray",
                                        "S-1-5-21-1-2-3-1001", "PT0S");
    CHECK(has(x, "<LogonTrigger>"));
    CHECK(has(x, "<Delay>PT15S</Delay>"));
    CHECK(has(x, "<UserId>S-1-5-21-1-2-3-1001</UserId>"));
    CHECK(has(x, "<LogonType>InteractiveToken</LogonType>"));
    CHECK(has(x, "<RunLevel>HighestAvailable</RunLevel>"));
    CHECK(has(x, "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"));
    CHECK(has(x, "<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"));
    CHECK(has(x, "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"));
    CHECK(has(x, "<Priority>5</Priority>"));   // normal, not the background default of 7
    CHECK(has(x, R"(<Command>C:\Program Files\GpuAutoOptimizer\GpuAutoOptimizer.exe</Command>)"));
    CHECK(x.rfind("<?xml", 0) == 0);
}

TEST_CASE("arguments and the time limit are the caller's") {
    // The resident tray process must never be stopped by Task Scheduler (PT0S);
    // a one-shot apply gets a short limit.
    const std::string tray = boot_task_xml(R"(C:\x\GpuAutoOptimizer.exe)", "--tray", "S-1", "PT0S");
    CHECK(has(tray, "<Arguments>--tray</Arguments>"));
    CHECK(has(tray, "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"));
    const std::string once = boot_task_xml(R"(C:\x\gao.exe)", "--boot-apply", "S-1", "PT5M");
    CHECK(has(once, "<Arguments>--boot-apply</Arguments>"));
    CHECK(has(once, "<ExecutionTimeLimit>PT5M</ExecutionTimeLimit>"));
}

TEST_CASE("special characters in the path are escaped") {
    const std::string x = boot_task_xml(R"(C:\A&B <x> "q"\gao.exe)", "--tray", "S-1", "PT0S");
    CHECK(has(x, R"(<Command>C:\A&amp;B &lt;x&gt; &quot;q&quot;\gao.exe</Command>)"));
    CHECK_FALSE(has(x, "A&B"));
}
