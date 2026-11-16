#include "core/journal.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <map>

namespace gao {

namespace {
struct Pending {
    std::optional<int> core, mem, uv;
    bool recovering = false;   // opened by begin_recovery
};

// True when a clock of the entry is above stock: only then is there a
// setting a later run can stay below.
bool caps_anything(const Pending& p) {
    return (p.core && *p.core > 0) || (p.mem && *p.mem > 0) || (p.uv && *p.uv > 0);
}

std::string describe(const Pending& p) {
    if (!caps_anything(p)) return "stock clocks";
    std::string s;
    if (p.core) s += "core +" + std::to_string(*p.core);
    if (p.mem) s += (s.empty() ? "" : " / ") + std::string("mem +") + std::to_string(*p.mem);
    if (p.uv) s += (s.empty() ? "" : " / ") + std::string("undervolt +") + std::to_string(*p.uv);
    if (p.recovering) s += " (while the driver recovered from a reset there)";
    return s;
}

// Bounds for hand-edited or damaged files: a value outside them makes the
// line invalid rather than overflowing next_id or a ceiling.
constexpr int kMaxJournalId = 1'000'000'000;
constexpr int kMaxClockMhz = 100'000;

std::optional<int> int_field(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    // Read wide and range-check: get<int>() would silently truncate a 64-bit
    // value (4294967305 -> 9) and slip it past the bounds below.
    if (it->is_number_unsigned() && it->get<unsigned long long>() > static_cast<unsigned long long>(INT_MAX))
        return std::nullopt;
    const long long v = it->get<long long>();
    if (v < INT_MIN || v > INT_MAX) return std::nullopt;
    return static_cast<int>(v);
}
}

Journal::Journal(const std::vector<std::string>& existing_lines, Append append, std::string rig)
    : append_(std::move(append)), rig_(std::move(rig)) {
    std::map<int, Pending> pending;   // ordered by id = file order
    int max_id = 0;
    for (std::string line : existing_lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        const bool parsed = !j.is_discarded() && j.is_object();
        torn_tail_ = !line.empty() && !parsed;   // only the last line's value survives the loop
        if (!parsed) continue;
        if (const auto edge = j.find("mem_edge"); edge != j.end()) {
            // A line of its own, without an id: builds that do not know it skip it.
            const auto mhz = int_field(j, "mem_edge");
            const auto rig_of = j.find("rig");
            if (mhz && *mhz > 0 && *mhz <= kMaxClockMhz && !rig_.empty() && rig_of != j.end() && rig_of->is_string() && *rig_of == rig_)
                mem_edge_ = std::min(mem_edge_, *mhz);
            continue;
        }
        const auto id = int_field(j, "id");
        const auto state = j.find("state");
        if (!id || *id <= 0 || *id >= kMaxJournalId || state == j.end() || !state->is_string()) continue;
        const auto core = int_field(j, "core"), mem = int_field(j, "mem"), uv = int_field(j, "uv");
        auto sane = [](std::optional<int> v) { return !v || (*v >= 0 && *v <= kMaxClockMhz); };
        if (!sane(core) || !sane(mem) || !sane(uv)) continue;
        max_id = std::max(max_id, *id);
        if (*state == "begin") {
            const auto rec = j.find("recovering");
            pending[*id] = {core, mem, uv, rec != j.end() && rec->is_boolean() && rec->get<bool>()};
        }
        else if (*state == "complete") pending.erase(*id);
    }
    for (const auto& [id, p] : pending) {
        // An offset of 0 never lowers a ceiling: a ceiling of 0 would mean
        // "never search this clock", which no freeze at stock can justify.
        if (p.core && *p.core > 0) ceilings_.core_mhz = std::min(ceilings_.core_mhz, *p.core);
        if (p.mem && *p.mem > 0) ceilings_.mem_mhz = std::min(ceilings_.mem_mhz, *p.mem);
        if (p.uv && *p.uv > 0) ceilings_.uv_mhz = std::min(ceilings_.uv_mhz, *p.uv);
        freezes_.push_back(describe(p));
        freeze_entries_.push_back({freezes_.back(), caps_anything(p)});
    }
    next_id_ = max_id + 1;
}

int Journal::begin(std::optional<int> core, std::optional<int> mem, std::optional<int> uv) {
    nlohmann::json j = {{"id", next_id_}, {"state", "begin"}};
    if (core) j["core"] = *core;
    if (mem) j["mem"] = *mem;
    if (uv) j["uv"] = *uv;
    if (!write(j.dump())) return -1;
    open_id_ = next_id_;
    open_clocks_ = {core, mem, uv};
    reset_.reset();
    return next_id_++;
}

bool Journal::complete(int id, const std::string& verdict) {
    const nlohmann::json j = {{"id", id}, {"state", "complete"}, {"verdict", verdict}};
    if (!write(j.dump())) return false;
    const bool reset = verdict == "DEVICE LOST" || verdict == "STALLED" || verdict == "NO TELEMETRY";
    if (id == open_id_ && reset) reset_ = open_clocks_;
    else reset_.reset();
    if (id == open_id_) open_id_ = -1;
    return true;
}

int Journal::begin_recovery() {
    if (!reset_ || !caps_anything({reset_->core, reset_->mem, reset_->uv})) return 0;
    const Clocks c = *reset_;
    nlohmann::json j = {{"id", next_id_}, {"state", "begin"}, {"recovering", true}};
    if (c.core) j["core"] = *c.core;
    if (c.mem) j["mem"] = *c.mem;
    if (c.uv) j["uv"] = *c.uv;
    if (!write(j.dump())) return -1;
    open_id_ = next_id_;
    open_clocks_ = c;
    reset_.reset();   // one per reset
    return next_id_++;
}

bool Journal::note_mem_edge(int mhz) {
    if (mhz <= 0 || rig_.empty()) return true;   // nothing to bind it to: nothing is kept
    const nlohmann::json j = {{"mem_edge", mhz}, {"rig", rig_}};
    if (!write(j.dump())) return false;
    mem_edge_ = std::min(mem_edge_, mhz);
    return true;
}

bool Journal::write(const std::string& line) {
    if (!append_(torn_tail_ ? "\n" + line : line)) return false;
    torn_tail_ = false;
    return true;
}

}
