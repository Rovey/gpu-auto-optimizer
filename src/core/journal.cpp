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

std::optional<int> int_field(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<int>();
}
}

Journal::Journal(const std::vector<std::string>& existing_lines, Append append)
    : append_(std::move(append)) {
    std::map<int, Pending> pending;   // ordered by id = file order
    int max_id = 0;
    for (std::string line : existing_lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const nlohmann::json j = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) continue;
        const auto id = int_field(j, "id");
        const auto state = j.find("state");
        if (!id || state == j.end() || !state->is_string()) continue;
        max_id = std::max(max_id, *id);
        if (*state == "begin") pending[*id] = {int_field(j, "core"), int_field(j, "mem")};
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
    if (!append_(j.dump())) return -1;
    return next_id_++;
}

bool Journal::complete(int id, const std::string& verdict) {
    const nlohmann::json j = {{"id", id}, {"state", "complete"}, {"verdict", verdict}};
    return append_(j.dump());
}

}
