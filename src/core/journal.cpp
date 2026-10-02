#include "core/journal.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <map>

namespace gao {

namespace {
struct Pending {
    std::optional<int> core, mem;
};

std::string describe(const Pending& p) {
    std::string s;
    if (p.core) s += "core +" + std::to_string(*p.core);
    if (p.mem) s += (s.empty() ? "" : " / ") + std::string("mem +") + std::to_string(*p.mem);
    return s.empty() ? "settings" : s;
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

Journal::Journal(const std::vector<std::string>& existing_lines, Append append)
    : append_(std::move(append)) {
    std::map<int, Pending> pending;   // ordered by id = file order
    int max_id = 0;
    for (std::string line : existing_lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        const bool parsed = !j.is_discarded() && j.is_object();
        torn_tail_ = !line.empty() && !parsed;   // only the last line's value survives the loop
        if (!parsed) continue;
        const auto id = int_field(j, "id");
        const auto state = j.find("state");
        if (!id || *id <= 0 || *id >= kMaxJournalId || state == j.end() || !state->is_string()) continue;
        const auto core = int_field(j, "core"), mem = int_field(j, "mem");
        auto sane = [](std::optional<int> v) { return !v || (*v >= 0 && *v <= kMaxClockMhz); };
        if (!sane(core) || !sane(mem)) continue;
        max_id = std::max(max_id, *id);
        if (*state == "begin") pending[*id] = {core, mem};
        else if (*state == "complete") pending.erase(*id);
    }
    for (const auto& [id, p] : pending) {
        if (p.core) ceilings_.core_mhz = std::min(ceilings_.core_mhz, *p.core);
        if (p.mem) ceilings_.mem_mhz = std::min(ceilings_.mem_mhz, *p.mem);
        freezes_.push_back(describe(p));
    }
    next_id_ = max_id + 1;
}

int Journal::begin(std::optional<int> core, std::optional<int> mem) {
    nlohmann::json j = {{"id", next_id_}, {"state", "begin"}};
    if (core) j["core"] = *core;
    if (mem) j["mem"] = *mem;
    if (!write(j.dump())) return -1;
    open_id_ = next_id_;
    return next_id_++;
}

bool Journal::complete(int id, const std::string& verdict) {
    const nlohmann::json j = {{"id", id}, {"state", "complete"}, {"verdict", verdict}};
    if (!write(j.dump())) return false;
    if (id == open_id_) open_id_ = -1;
    return true;
}

bool Journal::write(const std::string& line) {
    if (!append_(torn_tail_ ? "\n" + line : line)) return false;
    torn_tail_ = false;
    return true;
}

}
