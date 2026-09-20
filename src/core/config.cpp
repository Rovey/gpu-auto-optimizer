#include "core/config.hpp"
#include "nlohmann/json.hpp"

namespace gao {

std::string to_json(const Config& c) {
    const nlohmann::json j = {
        {"preset", static_cast<int>(c.preset)},
        {"objectives", {
            {"max_temp_c", c.objectives.max_temp_c},
            {"max_fan_pct", c.objectives.max_fan_pct},
            {"perf_push", c.objectives.perf_push},
            {"core_oc", c.objectives.core_oc},
            {"mem_oc", c.objectives.mem_oc},
            {"power", c.objectives.power},
            {"undervolt", c.objectives.undervolt},
        }},
        {"core_offset_mhz", c.core_offset_mhz},
        {"mem_offset_mhz", c.mem_offset_mhz},
        {"power_limit_pct", c.power_limit_pct},
        {"blacklisted_core_offsets", c.blacklisted_core_offsets},
    };
    return j.dump(2);
}

Config from_json(const std::string& text) {
    Config c;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return c;

    c.preset = static_cast<Preset>(j.value("preset", 0));
    c.objectives = objectives_for(c.preset);
    if (const auto it = j.find("objectives"); it != j.end() && it->is_object()) {
        c.objectives.max_temp_c = it->value("max_temp_c", c.objectives.max_temp_c);
        c.objectives.max_fan_pct = it->value("max_fan_pct", c.objectives.max_fan_pct);
        c.objectives.perf_push = it->value("perf_push", c.objectives.perf_push);
        c.objectives.core_oc = it->value("core_oc", c.objectives.core_oc);
        c.objectives.mem_oc = it->value("mem_oc", c.objectives.mem_oc);
        c.objectives.power = it->value("power", c.objectives.power);
        c.objectives.undervolt = it->value("undervolt", c.objectives.undervolt);
    }
    c.core_offset_mhz = j.value("core_offset_mhz", 0);
    c.mem_offset_mhz = j.value("mem_offset_mhz", 0);
    c.power_limit_pct = j.value("power_limit_pct", 100);
    c.blacklisted_core_offsets = j.value("blacklisted_core_offsets", std::vector<int>{});
    return c;
}

}
