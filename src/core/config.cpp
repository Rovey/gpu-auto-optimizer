#include "core/config.hpp"
#include "core/boot.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>

namespace gao {

namespace {
nlohmann::json curve_json(const FanCurve& c) {
    nlohmann::json points = nlohmann::json::array();
    for (const FanPoint& p : c.points) points.push_back({p.temp_c, p.pct});
    return {{"stop_below_c", c.stop_below_c ? nlohmann::json(*c.stop_below_c) : nlohmann::json(nullptr)},
            {"points", points}};
}

// nullopt for anything that is not a valid curve: a bad curve never reaches the fans.
std::optional<FanCurve> curve_from(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    FanCurve c;
    const auto stop = j.find("stop_below_c");
    if (stop != j.end() && stop->is_number_integer()) c.stop_below_c = stop->get<int>();
    else if (stop != j.end() && !stop->is_null()) return std::nullopt;
    const auto pts = j.find("points");
    if (pts == j.end() || !pts->is_array()) return std::nullopt;
    for (const auto& p : *pts) {
        if (!p.is_array() || p.size() != 2 || !p[0].is_number_integer() || !p[1].is_number_integer()) return std::nullopt;
        c.points.push_back({p[0].get<int>(), p[1].get<int>()});
    }
    if (!valid(c)) return std::nullopt;
    return c;
}
}

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
        if (p.fan_curve) j["profile"]["fan_curve"] = curve_json(*p.fan_curve);
    }
    if (c.fan_curve) j["fan_curve"] = curve_json(*c.fan_curve);
    j["fan_control"] = c.fan_control;
    j["update_check"] = c.update_check;
    if (c.fan_min_pct > 0) {
        j["fan_min_pct"] = c.fan_min_pct;
        j["fan_min_gpu"] = c.fan_min_gpu;
    }
    return j.dump(2);
}

Config from_json(const std::string& text) {
    Config c;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return c;
    // Fail closed: a strike count that is present but not a whole number in
    // 0..1000 disables boot-apply instead of silently re-enabling it.
    if (const auto it = j.find("boot_strikes"); it != j.end()) {
        const bool sane = it->is_number_integer() && it->get<long long>() >= 0 && it->get<long long>() <= 1000 &&
                          !(it->is_number_unsigned() && it->get<unsigned long long>() > 1000);
        c.boot_strikes = sane ? static_cast<int>(it->get<long long>()) : kMaxBootStrikes;
    }

    if (const auto it = j.find("fan_curve"); it != j.end()) c.fan_curve = curve_from(*it);
    if (const auto it = j.find("fan_control"); it != j.end() && it->is_boolean()) c.fan_control = it->get<bool>();
    if (const auto it = j.find("update_check"); it != j.end() && it->is_boolean()) c.update_check = it->get<bool>();
    const auto fmin = j.find("fan_min_pct"), fgpu = j.find("fan_min_gpu");
    if (fmin != j.end() && fgpu != j.end() && fmin->is_number_integer() && fgpu->is_string() && fmin->get<long long>() > 0 &&
        fmin->get<long long>() <= 100) {
        c.fan_min_pct = fmin->get<int>();
        c.fan_min_gpu = fgpu->get<std::string>();
    }

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
    if (const auto it = pj->find("fan_curve"); it != pj->end()) c.profile->fan_curve = curve_from(*it);
    return c;
}

std::optional<FanCurve> active_fan_curve(const Config& c) {
    if (!c.profile) return std::nullopt;
    if (c.fan_curve) return c.fan_curve;
    if (c.profile->fan_curve) return c.profile->fan_curve;
    return default_curve(c.profile->preset);
}

int fan_min_for(const Config& c, const std::string& gpu, const int nvml_min) {
    return c.fan_min_pct > 0 && !gpu.empty() && c.fan_min_gpu == gpu ? std::max(nvml_min, c.fan_min_pct) : nvml_min;
}

}
