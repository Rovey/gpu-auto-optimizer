#include "core/config.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>

namespace gao {

std::string to_json(const Config& c) {
    nlohmann::json j = {{"boot_strikes", c.boot_strikes}};
    if (c.profile) {
        const Profile& p = *c.profile;
        j["profile"] = {
            {"preset", preset_name(p.preset)},
            {"power_pct", p.power_pct},
            {"core_mhz", p.core_mhz},
            {"mem_mhz", p.mem_mhz},
            {"driver", p.driver},
            {"gpu", p.gpu},
            {"saved_at", p.saved_at},
        };
    }
    return j.dump(2);
}

Config from_json(const std::string& text) {
    Config c;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return c;
    if (const auto it = j.find("boot_strikes"); it != j.end() && it->is_number_integer())
        c.boot_strikes = std::max(0, it->get<int>());

    const auto pj = j.find("profile");
    if (pj == j.end() || !pj->is_object()) return c;
    auto num = [&](const char* key) -> std::optional<int> {
        const auto it = pj->find(key);
        if (it == pj->end() || !it->is_number_integer()) return std::nullopt;
        return it->get<int>();
    };
    auto str = [&](const char* key) -> std::optional<std::string> {
        const auto it = pj->find(key);
        if (it == pj->end() || !it->is_string()) return std::nullopt;
        return it->get<std::string>();
    };
    const auto name = str("preset");
    const auto preset = name ? preset_from_name(*name) : std::nullopt;
    const auto power = num("power_pct"), core = num("core_mhz"), mem = num("mem_mhz");
    const auto driver = str("driver"), gpu = str("gpu"), saved_at = str("saved_at");
    if (!preset || !power || !core || !mem || !driver || !gpu || !saved_at) return c;
    c.profile = Profile{*preset, *power, *core, *mem, *driver, *gpu, *saved_at};
    return c;
}

}
