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
