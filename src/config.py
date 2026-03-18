"""
Configuration: risk levels, user preferences, and optimization profiles.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field, asdict
from enum import Enum
from typing import Optional


# ---------------------------------------------------------------------------
# Risk levels
# ---------------------------------------------------------------------------

class RiskLevel(Enum):
    SAFE        = "safe"
    BALANCED    = "balanced"
    PERFORMANCE = "performance"
    EXTREME     = "extreme"


RISK_PROFILES: dict[RiskLevel, dict] = {
    RiskLevel.SAFE: {
        "label":                  "SAFE",
        "color":                  "green",
        "description": (
            "Power-limit and thermal optimisation only. "
            "Zero risk of instability. "
            "Reduces temperatures and noise while maintaining stock performance."
        ),
        "warning": None,
        # Tuning limits
        "core_offset_mhz_max":    0,
        "mem_offset_mhz_max":     0,
        "voltage_offset_mv_min":  0,   # no undervolt in safe mode
        "power_limit_delta_pct":  (-20, 0),   # only reduce
        "thermal_limit_max_c":    85,
        # Stability test
        "test_duration_sec":      30,
        "test_passes":            2,
    },
    RiskLevel.BALANCED: {
        "label":                  "BALANCED",
        "color":                  "yellow",
        "description": (
            "Mild overclock + moderate undervolt. "
            "Typical gains: +5–15 % performance, -10 °C, -20 % power draw. "
            "Low risk; will roll back automatically on instability."
        ),
        "warning": (
            "WARNING: Overclocking may void your GPU warranty. "
            "The tool tests each step before committing."
        ),
        "core_offset_mhz_max":    150,
        "mem_offset_mhz_max":     800,
        "voltage_offset_mv_min":  -150,
        "power_limit_delta_pct":  (-10, 15),
        "thermal_limit_max_c":    87,
        "test_duration_sec":      60,
        "test_passes":            3,
    },
    RiskLevel.PERFORMANCE: {
        "label":                  "PERFORMANCE",
        "color":                  "orange1",
        "description": (
            "Aggressive overclock + deep undervolt. "
            "Expects +15–30 % performance uplift. "
            "Moderate risk; crashes / driver TDRs possible during tuning."
        ),
        "warning": (
            "WARNING: HIGH RISK MODE - The tool will push clocks and reduce voltage "
            "aggressively. System crashes or driver resets WILL occur during "
            "the search phase. Save all open work before continuing. "
            "This may void your GPU warranty."
        ),
        "core_offset_mhz_max":    250,
        "mem_offset_mhz_max":     1500,
        "voltage_offset_mv_min":  -200,
        "power_limit_delta_pct":  (-5, 25),
        "thermal_limit_max_c":    90,
        "test_duration_sec":      120,
        "test_passes":            5,
    },
    RiskLevel.EXTREME: {
        "label":                  "EXTREME",
        "color":                  "red",
        "description": (
            "Maximum possible clocks. Silicon-lottery push. "
            "High probability of crashes, artifacts, and possible permanent "
            "reduction in GPU lifespan."
        ),
        "warning": (
            "EXTREME RISK - THIS MODE CAN PERMANENTLY DAMAGE YOUR GPU.\n"
            "Excessive voltage or heat may degrade transistors over time. "
            "System crashes, data corruption, or hardware failure are possible. "
            "You accept full responsibility. Save all work, close all applications, "
            "and ensure adequate cooling before continuing."
        ),
        "core_offset_mhz_max":    400,
        "mem_offset_mhz_max":     2000,
        "voltage_offset_mv_min":  -250,
        "power_limit_delta_pct":  (0, 50),
        "thermal_limit_max_c":    93,
        "test_duration_sec":      180,
        "test_passes":            8,
    },
}


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------

@dataclass
class GPUOptimizationResult:
    gpu_index:             int
    gpu_name:              str
    risk_level:            str
    # Applied settings
    core_offset_mhz:       int   = 0
    mem_offset_mhz:        int   = 0
    voltage_offset_mv:     int   = 0
    power_limit_pct:       int   = 100
    thermal_limit_c:       int   = 83
    # Measured outcomes
    baseline_boost_mhz:    int   = 0
    achieved_boost_mhz:    int   = 0
    baseline_temp_c:       float = 0.0
    achieved_temp_c:       float = 0.0
    baseline_power_w:      float = 0.0
    achieved_power_w:      float = 0.0
    stability_passed:      bool  = False
    notes:                 str   = ""


@dataclass
class UserConfig:
    risk_level:            str   = RiskLevel.BALANCED.value
    auto_apply_on_boot:    bool  = False
    stability_test_tool:   str   = "auto"   # auto | furmark | heaven | cupy | none
    save_profiles:         bool  = True
    max_temp_limit_c:      int   = 90       # hard safety ceiling
    fan_curve_enabled:     bool  = False
    per_gpu_results:       dict  = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Persistence
# ---------------------------------------------------------------------------

_CONFIG_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "optimizer_config.json",
)


def load_config() -> UserConfig:
    if os.path.exists(_CONFIG_PATH):
        try:
            with open(_CONFIG_PATH, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            return UserConfig(**data)
        except Exception:
            pass
    return UserConfig()


def save_config(cfg: UserConfig) -> None:
    with open(_CONFIG_PATH, "w", encoding="utf-8") as fh:
        json.dump(asdict(cfg), fh, indent=2)


def save_result(cfg: UserConfig, result: GPUOptimizationResult) -> None:
    cfg.per_gpu_results[result.gpu_name] = asdict(result)
    save_config(cfg)
