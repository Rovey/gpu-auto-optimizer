#include "hw/nvml.hpp"
#include "hw/afterburner.hpp"
#include "core/ab_profile.hpp"
#include <cstdio>
#include <thread>
#include <chrono>
#include <string>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    int slot = 5;
    std::string targetPower = (argc > 1) ? argv[1] : "80";  // % power limit to set

    gao::Nvml nvml;
    if (!nvml.Init()) { std::printf("SPIKE FAIL: NVML init\n"); return 1; }

    auto loc = gao::FindAfterburner(slot);
    if (!loc) { std::printf("SPIKE FAIL: Afterburner/profile not found (is it installed + profile %d saved?)\n", slot); return 2; }
    std::printf("Afterburner: %s\n  profile: %s\n", loc->exePath.c_str(), loc->profileCfgPath.c_str());

    if (!gao::BackupProfile(*loc)) { std::printf("SPIKE FAIL: backup\n"); return 3; }

    auto before = nvml.Read(0);
    std::printf("BEFORE: powerLimit=%u W\n", before.powerLimitW);

    std::string text = gao::ReadProfileText(*loc);
    auto curPower = gao::ReadProfileKey(text, "PowerLimit");
    std::printf("profile PowerLimit was: %s\n", curPower ? curPower->c_str() : "(absent)");

    std::string edited = gao::SetProfileKey(text, "PowerLimit", targetPower);
    if (!gao::WriteProfileText(*loc, edited)) { std::printf("SPIKE FAIL: write\n"); return 4; }

    if (!gao::ApplyProfile(*loc)) { std::printf("SPIKE FAIL: apply launch\n"); return 5; }
    std::this_thread::sleep_for(3s);

    auto after = nvml.Read(0);
    std::printf("AFTER:  powerLimit=%u W\n", after.powerLimitW);

    bool changed = (after.powerLimitW != before.powerLimitW);
    std::printf("%s: power limit %s (%u W -> %u W)\n",
                changed ? "SPIKE PASS" : "SPIKE FAIL",
                changed ? "CHANGED" : "did NOT change",
                before.powerLimitW, after.powerLimitW);
    return changed ? 0 : 10;
}
