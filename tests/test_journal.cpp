#include "doctest/doctest.h"
#include "core/journal.hpp"
#include <climits>
#include <string>
#include <vector>

using namespace gao;

namespace {
struct Sink {
    std::vector<std::string> lines;
    bool ok = true;
    Journal::Append append() { return [this](const std::string& l) { if (ok) lines.push_back(l); return ok; }; }
};
}

TEST_CASE("an empty journal has no ceilings and starts at id 1") {
    Sink s;
    Journal j({}, s.append());
    CHECK(j.ceilings().core_mhz == INT_MAX);
    CHECK(j.ceilings().mem_mhz == INT_MAX);
    CHECK(j.freezes().empty());
    CHECK(j.next_id() == 1);
}

TEST_CASE("begin and complete write lines that parse back as a finished candidate") {
    Sink s;
    Journal j({}, s.append());
    const int id = j.begin(165, std::nullopt);
    CHECK(id == 1);
    REQUIRE(s.lines.size() == 1);
    CHECK(s.lines[0].find("\"core\":165") != std::string::npos);
    CHECK(s.lines[0].find("\"begin\"") != std::string::npos);
    CHECK(j.complete(id, "STABLE"));
    Journal reread(s.lines, s.append());
    CHECK(reread.ceilings().core_mhz == INT_MAX);
    CHECK(reread.freezes().empty());
    CHECK(reread.next_id() == 2);
}

TEST_CASE("a begin without complete becomes a ceiling") {
    Sink s;
    Journal first({}, s.append());
    first.begin(165, std::nullopt);   // the machine "froze" here
    Journal after(s.lines, s.append());
    CHECK(after.ceilings().core_mhz == 165);
    CHECK(after.ceilings().mem_mhz == INT_MAX);
    REQUIRE(after.freezes().size() == 1);
    CHECK(after.freezes()[0] == "core +165");
}

TEST_CASE("a soak freeze lowers both ceilings and the lowest freeze wins") {
    Sink s;
    Journal first({}, s.append());
    first.begin(105, 700);
    first.begin(200, std::nullopt);
    Journal after(s.lines, s.append());
    CHECK(after.ceilings().core_mhz == 105);
    CHECK(after.ceilings().mem_mhz == 700);
    CHECK(after.freezes().size() == 2);
    CHECK(after.freezes()[0] == "core +105 / mem +700");
}

TEST_CASE("blank, CRLF and malformed lines are ignored") {
    Sink s;
    const std::vector<std::string> lines = {
        "",
        "{\"id\":4,\"mem\":900,\"state\":\"begin\"}\r",
        "garbage",
        "{\"id\":5,\"co",                     // half-written after a power cut
        "{\"state\":\"begin\",\"core\":60}",  // no id
    };
    Journal j(lines, s.append());
    CHECK(j.ceilings().mem_mhz == 900);
    CHECK(j.ceilings().core_mhz == INT_MAX);
    CHECK(j.next_id() == 5);
}

TEST_CASE("next id follows the highest id seen, including completes") {
    Sink s;
    Journal j({"{\"id\":9,\"state\":\"complete\",\"verdict\":\"STABLE\"}"}, s.append());
    CHECK(j.next_id() == 10);
    CHECK(j.begin(15, std::nullopt) == 10);
    CHECK(j.next_id() == 11);
}

TEST_CASE("a failed append reports -1 and does not advance") {
    Sink s;
    s.ok = false;
    Journal j({}, s.append());
    CHECK(j.begin(15, std::nullopt) == -1);
    CHECK(j.next_id() == 1);
}

TEST_CASE("an append after a torn last line starts on a fresh line") {
    // A power cut mid-append leaves a line without its newline; the next
    // begin must not be glued onto it, or a freeze during it is lost.
    const std::string torn = "{\"id\":4,\"co";
    Sink s;
    Journal j({torn}, s.append());
    j.begin(15, std::nullopt);
    std::string file = torn;
    for (const auto& l : s.lines) file += l + "\n";   // what append_line_durable writes
    std::vector<std::string> lines;
    for (size_t a = 0, b; a < file.size(); a = b + 1) {
        b = file.find('\n', a);
        if (b == std::string::npos) b = file.size();
        lines.push_back(file.substr(a, b - a));
    }
    Journal after(lines, s.append());
    CHECK(after.ceilings().core_mhz == 15);
}

TEST_CASE("ids and clock values outside sane bounds are ignored") {
    // A hand-edited journal must not overflow next_id or ceiling - 1.
    Sink s;
    const std::vector<std::string> lines = {
        "{\"id\":2147483647,\"core\":150,\"state\":\"begin\"}",
        "{\"id\":0,\"core\":150,\"state\":\"begin\"}",
        "{\"id\":-3,\"mem\":400,\"state\":\"begin\"}",
        "{\"id\":7,\"core\":-2147483648,\"state\":\"begin\"}",
        "{\"id\":8,\"mem\":200000,\"state\":\"begin\"}",
        "{\"id\":4294967305,\"core\":60,\"state\":\"begin\"}",   // would truncate to id 9
        "{\"id\":6,\"core\":4294967356,\"state\":\"begin\"}",    // would truncate to core 60
        "{\"id\":9,\"core\":90,\"state\":\"begin\"}",
    };
    Journal j(lines, s.append());
    CHECK(j.ceilings().core_mhz == 90);
    CHECK(j.ceilings().mem_mhz == INT_MAX);
    CHECK(j.next_id() == 10);
    CHECK(j.begin(15, std::nullopt) == 10);
}

TEST_CASE("open_id names the entry that is begun and not yet completed") {
    Sink s;
    Journal j({}, s.append());
    CHECK(j.open_id() == -1);
    const int id = j.begin(150, std::nullopt);
    CHECK(j.open_id() == id);
    CHECK(j.complete(id, "STABLE"));
    CHECK(j.open_id() == -1);
}

TEST_CASE("open_id keeps the entry when its complete line could not be written") {
    Sink s;
    Journal j({}, s.append());
    const int id = j.begin(150, std::nullopt);
    s.ok = false;
    CHECK(!j.complete(id, "STABLE"));
    CHECK(j.open_id() == id);
    CHECK(j.begin(165, std::nullopt) == -1);   // a failed begin opens nothing new
    CHECK(j.open_id() == id);
}
