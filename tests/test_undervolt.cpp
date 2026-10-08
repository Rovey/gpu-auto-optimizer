#include "core/undervolt.hpp"
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace gao;

namespace {

// A card for the undervolt search: a curve shaped like the reference RTX
// 4070's (15 MHz grid, about 6 mV per point), a load that the power limit
// holds at 2760 MHz at stock, and an edge: a point raised by more than
// edge_khz above its built-in frequency computes wrong values.
struct UvCard {
    std::vector<int> volt_uv, base_khz, raw;
    int stock_load_khz = 2760000;
    int edge_khz = 210000;        // 3 s probes fail above this raise
    int long_edge_khz = 210000;   // probes of 30 s and longer fail above this one
    int lost_above_khz = -1;      // >= 0: a probe above this raise loses the device instead
    int long_lost_above_khz = -1; // the same for probes of 30 s and longer; -1: as the short ones
    int losses_left = 1;
    bool dead = false;            // after a driver reset, until recover()
    bool slow = false;            // the card accepts the curve but does not run the frequency
    bool write_ok = true;
    int write_fails_above_khz = -1;   // >= 0: a curve write that raises a point by more than this fails
    bool baseline_ok = true;
    int abort_at_probe = -1;
    bool aborted_now = false;
    bool with_recover = true;

    int probes = 0, recovers = 0, resets_to_stock = 0, rests = 0;
    std::vector<double> probe_seconds;
    std::vector<int> probe_raise_mhz;   // the raise each probe ran at (0 at stock)
    std::vector<std::string> journal, log;

    UvCard() {
        for (int i = 0; i < 127; ++i) {
            volt_uv.push_back(450000 + i * 6250);
            base_khz.push_back(std::min(2925000, (390000 + i * 22500) / 15000 * 15000));
            raw.push_back(0);
        }
    }
    int freq(std::size_t i) const { return base_khz[i] + raw[i]; }
    bool at_stock() const { return std::all_of(raw.begin(), raw.end(), [](int v) { return v == 0; }); }
    // Where the card runs under load: at stock the power limit decides; with
    // a curve it takes the lowest voltage that gives the most frequency.
    std::size_t operating_point() const {
        if (at_stock()) {
            for (std::size_t i = 0; i < base_khz.size(); ++i)
                if (base_khz[i] >= stock_load_khz) return i;
            return base_khz.size() - 1;
        }
        int top = 0;
        for (std::size_t i = 0; i < raw.size(); ++i) top = std::max(top, freq(i));
        for (std::size_t i = 0; i < raw.size(); ++i)
            if (freq(i) == top) return i;
        return 0;
    }
    std::size_t stock_point() const {
        for (std::size_t i = 0; i < base_khz.size(); ++i)
            if (base_khz[i] >= stock_load_khz) return i;
        return 0;
    }
    GpuControl gpu() {
        GpuControl g;
        g.read_vf_curve = [this]() -> std::optional<std::vector<VfPoint>> {
            if (dead) return std::nullopt;
            std::vector<VfPoint> out;
            for (std::size_t i = 0; i < raw.size(); ++i) out.push_back({static_cast<int>(i), volt_uv[i], freq(i), raw[i]});
            return out;
        };
        g.write_vf_offsets = [this](const std::vector<VfOffset>& offsets) {
            if (dead || !write_ok) return false;
            if (write_fails_above_khz >= 0)
                for (const VfOffset& o : offsets)
                    if (o.raw > write_fails_above_khz) return false;
            for (const VfOffset& o : offsets) raw[static_cast<std::size_t>(o.index)] = o.raw;
            return true;
        };
        g.reset_to_stock = [this] {
            if (dead) return false;
            ++resets_to_stock;
            std::fill(raw.begin(), raw.end(), 0);
            return true;
        };
        if (with_recover)
            g.recover = [this] { ++recovers; dead = false; std::fill(raw.begin(), raw.end(), 0); return true; };
        return g;
    }
    StabilityResult probe(double seconds) {
        ++probes;
        probe_seconds.push_back(seconds);
        StabilityResult s;
        s.seconds = seconds;
        if (probes == abort_at_probe) { aborted_now = true; s.verdict = Verdict::Aborted; return s; }
        const std::size_t op = operating_point();
        // At stock the power limit holds the clock between two points of the curve.
        const int f = at_stock() ? stock_load_khz : slow ? freq(op) - 105000 : freq(op);
        const int raise = freq(op) - base_khz[op];
        probe_raise_mhz.push_back(raise / 1000);
        const double v = volt_uv[op] / static_cast<double>(volt_uv[stock_point()]);
        s.avg_core_mhz = f / 1000;
        s.score = 5700.0 * f / stock_load_khz;
        s.avg_power_w = static_cast<int>(std::lround(188 * v * v));
        s.peak_temp_c = 65;
        if (!baseline_ok && at_stock()) { s.verdict = Verdict::WrongResult; return s; }
        const int lost_above = seconds >= 30 && long_lost_above_khz >= 0 ? long_lost_above_khz : lost_above_khz;
        if (lost_above >= 0 && raise > lost_above && losses_left > 0) {
            --losses_left;
            dead = true;   // the driver reset: the card is at stock and the connections are gone
            std::fill(raw.begin(), raw.end(), 0);
            s.verdict = Verdict::DeviceLost;
            return s;
        }
        if (raise > (seconds >= 30 ? long_edge_khz : edge_khz)) s.verdict = Verdict::WrongResult;
        return s;
    }
    UndervoltResult go(std::vector<std::string> earlier_journal = {}) {
        Journal j(earlier_journal, [this](const std::string& line) { journal.push_back(line); return true; });
        UndervoltIo io;
        io.probe = [this](double seconds, int, double) { return probe(seconds); };
        io.aborted = [this] { return aborted_now; };
        io.log = [this](const std::string& m) { log.push_back(m); };
        io.rest = [this](double) { ++rests; return true; };
        return find_undervolt(gpu(), j, io, 75, 0.7f);
    }
    bool logged(const std::string& what) const {
        for (const auto& m : log) if (m.find(what) != std::string::npos) return true;
        return false;
    }
    int count(const std::string& what) const {
        int n = 0;
        for (const auto& l : journal) n += l.find(what) != std::string::npos;
        return n;
    }
    // Raise (MHz) at the point the curve is anchored on now; 0 at stock.
    int applied_raise_mhz() const {
        if (at_stock()) return 0;
        const std::size_t op = operating_point();
        return (freq(op) - base_khz[op]) / 1000;
    }
};

}

TEST_CASE("the journal remembers an undervolt that froze the machine as a ceiling on the raise") {
    std::vector<std::string> lines;
    Journal j({}, [&](const std::string& l) { lines.push_back(l); return true; });
    const int id = j.begin(std::nullopt, std::nullopt, 150);
    REQUIRE(id > 0);
    CHECK(lines.back().find("\"uv\":150") != std::string::npos);

    Journal frozen(lines, [](const std::string&) { return true; });   // the begin was never completed
    CHECK(frozen.ceilings().uv_mhz == 150);
    CHECK(frozen.ceilings().core_mhz == INT_MAX);
    REQUIRE(frozen.freezes().size() == 1);
    CHECK(frozen.freezes()[0] == "undervolt +150");
    CHECK(frozen.freeze_entries()[0].caps_anything);

    REQUIRE(j.complete(id, "STABLE"));
    Journal finished(lines, [](const std::string&) { return true; });
    CHECK(finished.ceilings().uv_mhz == INT_MAX);
    CHECK(finished.freezes().empty());
}

TEST_CASE("find_undervolt: the lowest voltage that holds the stock load clock, minus the margin") {
    UvCard card;
    const auto r = card.go();
    REQUIRE(r.ok);
    // The load ran 2760 MHz at stock; the highest point of the curve that is
    // not above that is 2745 MHz, and that is what the card must keep.
    CHECK(r.freq_khz == 2745000);
    const auto own = static_cast<std::size_t>(
        std::find_if(card.base_khz.begin(), card.base_khz.end(), [](int f) { return f >= 2745000; }) - card.base_khz.begin());
    CHECK(r.stock_uv == card.volt_uv[own]);
    // The descent takes one point at a time, each a little more raise, and
    // stops at the first that fails: nothing above +210 passes, and nothing
    // beyond the first failure is tried.
    const auto first_fail = std::find_if(card.probe_raise_mhz.begin(), card.probe_raise_mhz.end(), [](int v) { return v > 210; });
    REQUIRE(first_fail != card.probe_raise_mhz.end());
    CHECK(std::count_if(card.probe_raise_mhz.begin(), card.probe_raise_mhz.end(), [](int v) { return v > 210; }) == 1);
    CHECK(std::is_sorted(card.probe_raise_mhz.begin() + 1, first_fail));   // after the baseline: ascending raise
    // Confirmed at the edge (it holds 30 s on this card), applied with the
    // same margin as a core offset: 70 % of the confirmed raise.
    CHECK(r.edge_uv == r.confirmed_uv);
    CHECK(r.applied_uv > r.confirmed_uv);
    CHECK(r.applied_uv < r.stock_uv);
    const int confirmed_raise = (r.freq_khz - card.base_khz[static_cast<std::size_t>(std::find(card.volt_uv.begin(), card.volt_uv.end(), r.confirmed_uv) - card.volt_uv.begin())]) / 1000;
    CHECK(card.applied_raise_mhz() <= apply_margin(confirmed_raise, 15, 0.7f));
    CHECK(card.applied_raise_mhz() > 0);
    // The result is what the soak ran on, and it is still applied.
    CHECK(card.probe_seconds.back() == kUvSoakS);
    CHECK(card.probe_raise_mhz.back() == card.applied_raise_mhz());
    CHECK(r.after.avg_core_mhz == 2745);
    CHECK(r.after.avg_power_w < r.baseline.avg_power_w);
    // Every entry was closed; the journal names the raise of each candidate.
    CHECK(card.count("\"begin\"") == card.count("\"complete\""));
    CHECK(card.count("\"uv\":") == card.count("\"begin\""));
    CHECK(card.logged("baseline: 30 s at stock"));
}

TEST_CASE("find_undervolt: an edge that does not hold 30 s is given up one point at a time") {
    UvCard card;
    card.long_edge_khz = 180000;   // 3 s probes pass up to +210, the long ones only up to +180
    const auto r = card.go();
    REQUIRE(r.ok);
    CHECK(r.confirmed_uv > r.edge_uv);
    CHECK(card.logged("confirm"));
    CHECK(card.count("\"begin\"") == card.count("\"complete\""));
}

TEST_CASE("find_undervolt: a stop request ends the run with the card at stock") {
    UvCard card;
    card.abort_at_probe = 5;
    const auto r = card.go();
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "aborted");
    CHECK(r.stock_restored);
    CHECK(card.at_stock());
    CHECK(card.count("\"begin\"") == card.count("\"complete\""));
}

TEST_CASE("find_undervolt: a raise that froze the machine before is never reached again") {
    UvCard card;
    const auto r = card.go({R"({"id":7,"state":"begin","uv":150})"});
    REQUIRE(r.ok);
    CHECK(*std::max_element(card.probe_raise_mhz.begin(), card.probe_raise_mhz.end()) < 150);
    CHECK(card.logged("a previous run froze the machine at undervolt +150"));
}

TEST_CASE("find_undervolt: a driver reset ends the descent; the card must prove it is back") {
    UvCard card;
    card.lost_above_khz = 150000;   // the first candidate above +150 loses the device, once
    const auto r = card.go();
    REQUIRE(r.ok);
    CHECK(card.recovers == 1);
    CHECK(card.rests == 1);
    CHECK(card.logged("the driver was reset; reconnecting"));
    CHECK(card.logged("health:"));
    // Nothing above the lost raise afterwards, and the confirm probe keeps
    // its distance from it.
    const auto lost = std::find_if(card.probe_raise_mhz.begin(), card.probe_raise_mhz.end(), [](int v) { return v > 150; });
    REQUIRE(lost != card.probe_raise_mhz.end());
    CHECK(std::all_of(lost + 1, card.probe_raise_mhz.end(), [](int v) { return v <= 150; }));
    CHECK(card.count("\"begin\"") == card.count("\"complete\""));
}

TEST_CASE("find_undervolt: a second driver reset ends the run at stock, nothing applied") {
    UvCard card;
    card.lost_above_khz = 150000;    // the first, in the descent
    card.long_lost_above_khz = 0;    // the second, in the confirm probe
    card.losses_left = 2;
    const auto r = card.go();
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver reset twice");
    CHECK(card.at_stock());
    CHECK(card.count("\"begin\"") == card.count("\"complete\""));
}

TEST_CASE("find_undervolt: a reset without a way to reconnect ends the run") {
    UvCard card;
    card.lost_above_khz = 150000;
    card.with_recover = false;
    const auto r = card.go();
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the driver did not come back after a reset");
}

TEST_CASE("find_undervolt: a card that takes the curve but does not run the frequency holds no undervolt") {
    UvCard card;
    card.slow = true;
    const auto r = card.go();
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "the card held no undervolt: the first step below its own voltage already failed");
    CHECK(card.at_stock());
    CHECK(card.logged("SLOW"));
}

TEST_CASE("find_undervolt: refusals before anything is tried") {
    {
        UvCard card;
        card.baseline_ok = false;
        const auto r = card.go();
        CHECK_FALSE(r.ok);
        CHECK(r.reason == "the card is not stable at stock (WRONG RESULT)");
        CHECK(card.count("\"uv\":") == 0);
    }
    {
        UvCard card;
        Journal j({}, [](const std::string&) { return true; });
        UndervoltIo io;
        io.probe = [&](double seconds, int, double) { return card.probe(seconds); };
        GpuControl none;   // a card without the curve calls
        const auto r = find_undervolt(none, j, io, 75, 0.7f);
        CHECK_FALSE(r.ok);
        CHECK(r.reason == "this card or driver does not offer the voltage/frequency curve");
        CHECK(card.probes == 0);
    }
    {
        UvCard card;
        card.write_ok = false;
        const auto r = card.go();
        CHECK_FALSE(r.ok);
        CHECK(card.at_stock());
        CHECK(card.count("SET FAILED") == 1);
    }
}

TEST_CASE("find_undervolt: a journal that cannot be written stops before the card is touched") {
    UvCard card;
    Journal j({}, [](const std::string&) { return false; });
    UndervoltIo io;
    io.probe = [&](double seconds, int, double) { return card.probe(seconds); };
    const auto r = find_undervolt(card.gpu(), j, io, 75, 0.7f);
    CHECK_FALSE(r.ok);
    CHECK(r.reason == "could not write the journal");
    CHECK(card.at_stock());
    CHECK(card.probes == 1);   // the baseline only
}

// The curve of the reference RTX 4070 as `gao --curve` printed it on driver
// 617.14 (hardware check 61), with the +135 MHz core offset that was applied
// taken off again: millivolts, and the built-in MHz of each point.
namespace {
const int kRtx4070Mv[] = {
    450, 460, 465, 470, 475, 485, 490, 495, 500, 510, 515, 520, 525, 535, 540, 545, 550, 560, 565, 570, 575, 585,
    590, 595, 600, 610, 615, 620, 625, 635, 640, 645, 650, 660, 665, 670, 675, 685, 690, 695, 700, 710, 715, 720,
    725, 735, 740, 745, 750, 760, 765, 770, 775, 785, 790, 795, 800, 810, 815, 820, 825, 835, 840, 845, 850, 860,
    865, 870, 875, 885, 890, 895, 900, 910, 915, 920, 925, 935, 940, 945, 950, 960, 965, 970, 975, 985, 990, 995,
    1000, 1010, 1015, 1020, 1025, 1035, 1040, 1045, 1050, 1060, 1065, 1070, 1075, 1085, 1090, 1095, 1100, 1110, 1115,
    1120, 1125, 1135, 1140, 1145, 1150, 1160, 1165, 1170, 1175, 1185, 1190, 1195, 1200, 1210, 1215, 1220, 1225, 1235,
    1240
};
const int kRtx4070Mhz[] = {
    255, 300, 330, 375, 405, 450, 480, 525, 555, 600, 630, 675, 705, 735, 780, 810, 840, 885, 915, 945, 975, 1020,
    1050, 1080, 1110, 1155, 1185, 1215, 1245, 1275, 1305, 1335, 1365, 1395, 1425, 1455, 1485, 1515, 1545, 1575, 1590,
    1620, 1650, 1665, 1695, 1725, 1740, 1770, 1770, 1785, 1815, 1830, 1860, 1890, 1920, 1950, 1980, 1995, 2025, 2055,
    2085, 2100, 2130, 2160, 2175, 2205, 2220, 2250, 2280, 2295, 2325, 2340, 2370, 2385, 2415, 2430, 2460, 2475, 2505,
    2520, 2535, 2565, 2580, 2595, 2610, 2625, 2640, 2655, 2670, 2685, 2700, 2700, 2715, 2730, 2745, 2760, 2760, 2775,
    2790, 2790, 2805, 2820, 2820, 2835, 2835, 2850, 2850, 2865, 2865, 2880, 2880, 2895, 2895, 2895, 2910, 2910, 2910,
    2910, 2925, 2925, 2925, 2925, 2925, 2925, 2925, 2925, 2925
};
}

TEST_CASE("find_undervolt on the reference RTX 4070's curve, with its measured core edge") {
    UvCard card;
    card.volt_uv.clear();
    card.base_khz.clear();
    for (int v : kRtx4070Mv) card.volt_uv.push_back(v * 1000);
    for (int f : kRtx4070Mhz) card.base_khz.push_back(f * 1000);
    card.raw.assign(card.base_khz.size(), 0);
    card.stock_load_khz = 2766000;   // what the card ran under the stress load at stock
    card.edge_khz = card.long_edge_khz = 210000;   // core +210 was confirmed, +225 computed wrong values
    const auto r = card.go();
    REQUIRE(r.ok);
    CHECK(r.freq_khz == 2760000);
    CHECK(r.stock_uv == 1045000);       // where the card runs 2760 MHz by itself
    CHECK(r.edge_uv == 960000);         // +195 MHz at that point; the next one down asks +225
    CHECK(r.confirmed_uv == 960000);
    CHECK(r.applied_uv == 985000);      // the margin keeps +135 MHz, as for the core offset
    CHECK(card.applied_raise_mhz() == 135);
    // Sixty millivolts less for the same clock: about a ninth of the power.
    CHECK(r.after.avg_power_w < r.baseline.avg_power_w * 0.92);
}

TEST_CASE("find_undervolt: a candidate the card will not take ends the descent; what passed is kept") {
    UvCard card;
    card.write_fails_above_khz = 120000;   // the driver refuses to raise a point by more than 120 MHz
    const auto r = card.go();
    REQUIRE(r.ok);
    CHECK(card.count("SET FAILED") == 1);
    CHECK(card.logged("not set -- writing the curve failed -- curve at stock"));
    CHECK(*std::max_element(card.probe_raise_mhz.begin(), card.probe_raise_mhz.end()) <= 120);
    CHECK(card.count("\"begin\"") == card.count("\"complete\""));
}
