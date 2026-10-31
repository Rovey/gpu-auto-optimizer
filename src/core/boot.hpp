#pragma once
#include "core/config.hpp"
#include "core/types.hpp"
#include <cstddef>
#include <string>

namespace gao {

// Logons that crashed within 2 minutes of applying before boot-apply gives up.
inline constexpr int kMaxBootStrikes = 3;

enum class BootDecision { Apply, NoProfile, SeveralGpus, TooManyStrikes, DriverChanged, GpuChanged };

// One NVIDIA GPU per PC is supported. NVML, NVAPI and DXGI each list the cards
// in their own order and the app takes the first of each, so with two NVIDIA
// cards the one that is tuned and the one the stress load runs on can differ.
// NVML counts NVIDIA devices only: an Intel or AMD integrated GPU is not one.
// An unknown count (-1, a failed query) does not refuse; the calls that follow
// report that failure themselves.
bool several_gpus(int nvidia_gpus);
inline constexpr const char* kSeveralGpusRefusal =
    "this PC has more than one NVIDIA GPU, and GPU Auto Optimizer supports one: with two, the card it tunes and the card "
    "it tests can be different ones. Nothing was changed. If you have two NVIDIA cards and want to help make this work, "
    "open an issue at https://github.com/Rovey/gpu-auto-optimizer/issues";
// The tray balloon for that refusal at logon. A balloon holds 255 characters
// (NOTIFYICONDATAW::szInfo, 256 with the terminator) and the refusal is
// longer, so the balloon only says what happened; a click opens the window,
// whose log has the whole refusal.
inline constexpr size_t kTrayBalloonMaxChars = 255;
inline constexpr const char* kSeveralGpusBalloon =
    "Your tune is not applied: this PC has more than one NVIDIA GPU. Click for details.";

// Order: no profile, several GPUs, strikes, driver, GPU. An empty (unknown)
// driver or GPU id never matches, so a failed NVML query cannot apply offsets
// that were tested on some other driver or card. nvidia_gpus: what NVML
// counts (Nvml::DeviceCount).
BootDecision decide_boot(const Config& c, const std::string& driver, const std::string& gpu, int nvidia_gpus = 1);

// Sets power, core and memory from the profile; each GpuControl setter
// verifies by read-back. A setter the card lacks is fine only when the
// profile asks for stock on that dimension. Values outside the search
// bounds are refused before anything is written (gao.json is user-writable
// and boot-apply runs elevated). Any failure resets to stock and returns
// false with the reason in *why, including whether that reset worked.
//
// A flat top that is on the card is removed first, whatever the profile. A
// profile with an undervolt then gets its own, written by apply_flat_top
// (core/vf_curve.hpp) on the point with the saved voltage. It is refused
// when the card has no such point, and when the point would have to be
// raised further than the undervolt was tested with plus what the built-in
// curve moves with temperature (kUvClockSlackKhz): the saved number is then
// not this curve's. With a core offset in the same profile the flat top is
// written over that offset: the two are one table on the card, the offset
// stays on the points below the anchor, and the raise counts from the
// built-in curve, offset included.
// *left_clean (optional) says whether a failed apply left nothing of the
// profile on the card: refused before any write, or reset to stock after a
// failed one. False after a reset that failed, and after a successful apply.
bool apply_profile(const GpuControl& gpu, const Profile& p, std::string* why, bool* left_clean = nullptr);

}
