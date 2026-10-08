#pragma once
#include "core/types.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace gao {

// The card's voltage/frequency curve: for each voltage the clock generator
// may use, the frequency the card runs there. The driver lets a program add
// an offset to each point; the voltages themselves cannot be written. An
// undervolt is therefore a shape: one point (the anchor) is raised to the
// wanted frequency, and no point above it runs more than that, so the card
// has no reason to ask for more voltage than the anchor's ("flat top").
// Points below the anchor keep the built-in curve, so the card clocks down
// at idle as it always did. No voltage lock is set: the lock call hard-froze
// the reference RTX 4070 in the project's Python days.

// The three driver buffers this is read from (NVAPI ClockBoostMask, VFP
// curve, ClockBoostTable). Not documented by NVIDIA; sizes and offsets are
// the ones the project's Python implementation read real curves with
// (src/backends/nvapi_vfcurve.py, tag v0.9-python).
inline constexpr std::size_t kVfMaskSize = 6188;
inline constexpr std::size_t kVfStatusSize = 7208;
inline constexpr std::size_t kVfControlSize = 9248;
// Bytes [kVfHeaderBegin, kVfEntries) of the mask answer say which slots
// exist; the other two requests carry a copy of them.
inline constexpr std::size_t kVfHeaderBegin = 4;
inline constexpr std::size_t kVfEntries = 68;        // where the per-slot entries start, in all three
inline constexpr std::size_t kVfMaskEntry = 24;      // uint32 clock domain, uint8 enabled at +4
inline constexpr std::size_t kVfStatusEntry = 28;    // uint32 kHz at +4, uint32 microvolts at +8
inline constexpr std::size_t kVfControlEntry = 36;   // int32 raw offset at +kVfControlOffset
inline constexpr std::size_t kVfControlOffset = 20;
inline constexpr int kVfSlots = 255;
inline constexpr unsigned kVfDomainGraphics = 0;

// The enabled points of the graphics clock, lowest voltage first.
std::vector<VfPoint> parse_vf_curve(const unsigned char* mask, const unsigned char* status, const unsigned char* control);
void put_vf_raw_offset(unsigned char* control, int index, int raw);

// How close a point must read to its target. Cards quantise the clock in
// steps of up to 15 MHz (RTX 50), so half a step is as close as a target
// that lies between two steps can get.
inline constexpr int kVfToleranceKhz = 8000;
// The largest raise of a point this code writes. A sanity bound, not a
// promise that it is stable: the search stops long before, at the first
// candidate that fails.
inline constexpr int kVfMaxRaiseKhz = 500000;
// Write, read back, correct: at most this many rounds.
inline constexpr int kVfPasses = 4;
// No offset is ever written further than this above what the plan asks of
// its point: room to follow a built-in curve that shifted by a step or two,
// and no more.
inline constexpr int kVfSlackKhz = 30000;

struct VfApplyResult {
    bool ok = false;
    std::string why;         // when !ok, including whether the curve is back at stock
    bool at_stock = false;   // when !ok: the curve carries no offset (refused before a write, or cleared after it)
    int passes = 0;          // write rounds it took
};

// Writes a flat top: the point in slot `anchor_index` and every point above
// it run freq_khz, the points below keep the built-in curve. The target must
// lie between the anchor's own frequency and the highest the card runs by
// itself (more is an overclock, not an undervolt).
//
// Offsets are written in kHz, one to one, as the reference RTX 4070 stores
// them (hardware check 61); nothing estimates other units. What protects a
// card that stores them differently is the order of the writes: the first
// round raises a point only half of the way, so a card that moves twice as
// far as asked lands on the target, not above it; every later round adds
// each point's own remaining gap as read back, which also follows a built-in
// curve that shifted with temperature; and no offset is ever written beyond
// what the plan asks of its point plus kVfSlackKhz. A card that does not
// arrive within kVfPasses rounds, or whose read-out does not show the change
// at all, is refused. (Whether the anchor moved by exactly what the half
// step wrote is not checked: on the reference card two reads a moment apart
// differ by up to two grid steps per point as it warms or cools, which is as
// much as such a check could look for.)
//
// A curve that is not at stock is cleared first. Any failure clears the
// curve again.
VfApplyResult apply_flat_top(const GpuControl& gpu, int anchor_index, int freq_khz);

// Every offset back to zero. Judged by reading the curve back, not by the
// driver's return code. True without writing when the curve already is at
// stock, or when the card has no curve calls.
bool clear_vf_curve(const GpuControl& gpu, std::string* why);

// For a reset to stock. A plain core offset is the same offset on every
// point, and the offset reset that follows removes it, as it always did. A
// shape (offsets that differ between points: a flat top) is not known to go
// that way, so it is zeroed here first. True when the curve cannot be read
// (not this program's doing), has no shape, or reads without one afterwards.
bool remove_vf_shape(const GpuControl& gpu, std::string* why);

// The slot of the lowest-voltage point that runs at least freq_khz: where
// the card reaches that frequency by itself. -1 when no point does.
int stock_point_for(const std::vector<VfPoint>& curve, int freq_khz);

}
