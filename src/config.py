"""
Configuration: risk levels, user preferences, and optimization profiles.
"""
from __future__ import annotations

import json
import os
from dataclasses import dataclass, field, asdict
from enum import Enum
from shutil import copy2
from dataclasses import fields as dataclass_fields


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
class BootApplyState:
    consecutive_failures:  int   = 0
    disabled:              bool  = False
    gpu_uuid:              str   = ""
    driver_version:        str   = ""
    last_apply_time:       str   = ""
    last_apply_result:     str   = ""
    boot_log:              list  = field(default_factory=list)


@dataclass
class UserConfig:
    risk_level:            str   = RiskLevel.BALANCED.value
    auto_apply_on_boot:    bool  = False
    save_profiles:         bool  = True
    max_temp_limit_c:      int   = 90       # hard safety ceiling
    boot_apply:            BootApplyState = field(default_factory=BootApplyState)
    per_gpu_results:       dict  = field(default_factory=dict)


# ---------------------------------------------------------------------------
# Application directories
# ---------------------------------------------------------------------------

_OLD_CONFIG_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "optimizer_config.json",
)


def get_app_dir() -> str:
    """Return the root application data directory (%LOCALAPPDATA%/GPUOptimizer)."""
    return os.path.join(os.environ.get("LOCALAPPDATA", ""), "GPUOptimizer")


def get_config_dir() -> str:
    """Return the configuration directory."""
    return os.path.join(get_app_dir(), "config")


def get_log_dir() -> str:
    """Return the log directory."""
    return os.path.join(get_app_dir(), "logs")


def _default_config_path() -> str:
    return os.path.join(get_config_dir(), "optimizer_config.json")


# ---------------------------------------------------------------------------
# Persistence
# ---------------------------------------------------------------------------

_BOOT_LOG_MAX = 20


def load_config(path: str = "") -> UserConfig:
    """Load a UserConfig from *path* (defaults to the standard config location).

    Returns a default ``UserConfig`` when the file does not exist or cannot be
    parsed.
    """
    path = path or _default_config_path()
    if os.path.exists(path):
        try:
            with open(path, "r", encoding="utf-8") as fh:
                data = json.load(fh)
            # Strip unknown keys (e.g. removed fields from legacy configs)
            valid_user_fields = {f.name for f in dataclass_fields(UserConfig)}
            boot_raw = data.pop("boot_apply", None)
            data = {k: v for k, v in data.items() if k in valid_user_fields}

            if isinstance(boot_raw, dict):
                valid_boot_fields = {f.name for f in dataclass_fields(BootApplyState)}
                boot_raw = {k: v for k, v in boot_raw.items() if k in valid_boot_fields}
                cfg = UserConfig(**data, boot_apply=BootApplyState(**boot_raw))
            else:
                cfg = UserConfig(**data)
            return cfg
        except Exception:
            pass
    return UserConfig()


def save_config(cfg: UserConfig, path: str = "") -> None:
    """Serialize *cfg* to JSON at *path* (defaults to the standard config location)."""
    path = path or _default_config_path()
    # Ensure parent directory exists
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    # Trim boot log to last N entries
    cfg.boot_apply.boot_log = cfg.boot_apply.boot_log[-_BOOT_LOG_MAX:]
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(asdict(cfg), fh, indent=2)


def save_result(cfg: UserConfig, result: GPUOptimizationResult, path: str = "") -> None:
    """Persist *result* into *cfg* and write to disk."""
    cfg.per_gpu_results[result.gpu_name] = asdict(result)
    save_config(cfg, path)


# ---------------------------------------------------------------------------
# Migration
# ---------------------------------------------------------------------------

def migrate_config_if_needed() -> None:
    """Copy the legacy project-root config to the new location if needed."""
    new_path = _default_config_path()
    if os.path.exists(_OLD_CONFIG_PATH) and not os.path.exists(new_path):
        os.makedirs(os.path.dirname(new_path), exist_ok=True)
        copy2(_OLD_CONFIG_PATH, new_path)
