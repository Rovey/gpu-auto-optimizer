#include "doctest/doctest.h"
#include "core/task_xml.hpp"
#include <string>

using namespace gao;

namespace {
bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
}

TEST_CASE("the boot task XML carries every required setting") {
    const std::string x = boot_task_xml(R"(C:\Program Files\GpuAutoOptimizer\gao.exe)", "S-1-5-21-1-2-3-1001");
    CHECK(has(x, "<LogonTrigger>"));
    CHECK(has(x, "<Delay>PT15S</Delay>"));
    CHECK(has(x, "<UserId>S-1-5-21-1-2-3-1001</UserId>"));
    CHECK(has(x, "<LogonType>InteractiveToken</LogonType>"));
    CHECK(has(x, "<RunLevel>HighestAvailable</RunLevel>"));
    CHECK(has(x, "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"));
    CHECK(has(x, "<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"));
    CHECK(has(x, "<ExecutionTimeLimit>PT5M</ExecutionTimeLimit>"));
    CHECK(has(x, "<MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"));
    CHECK(has(x, R"(<Command>C:\Program Files\GpuAutoOptimizer\gao.exe</Command>)"));
    CHECK(has(x, "<Arguments>--boot-apply</Arguments>"));
    CHECK(x.rfind("<?xml", 0) == 0);
}

TEST_CASE("special characters in the path are escaped") {
    const std::string x = boot_task_xml(R"(C:\A&B <x> "q"\gao.exe)", "S-1");
    CHECK(has(x, R"(<Command>C:\A&amp;B &lt;x&gt; &quot;q&quot;\gao.exe</Command>)"));
    CHECK_FALSE(has(x, "A&B"));
}
