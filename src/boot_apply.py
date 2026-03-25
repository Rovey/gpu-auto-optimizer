"""Headless boot-apply script — applies saved GPU settings on login."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime

from .config import UserConfig, save_config


@dataclass
class ApplyDecision:
    skip: bool
    reason: str
    warning: str = ""


def should_apply(
    cfg: UserConfig,
    current_gpu_uuid: str = "",
    current_driver: str = "",
) -> ApplyDecision:
    """Decide whether to apply saved settings on boot."""
    if not cfg.auto_apply_on_boot:
        return ApplyDecision(skip=True, reason="Auto-apply is disabled in settings.")

    if cfg.boot_apply.disabled:
        return ApplyDecision(
            skip=True,
            reason="Auto-apply disabled after 3 consecutive strike failures. Re-run optimizer.",
        )

    if not cfg.per_gpu_results:
        return ApplyDecision(skip=True, reason="No saved optimization results found.")

    if cfg.boot_apply.gpu_uuid and current_gpu_uuid and cfg.boot_apply.gpu_uuid != current_gpu_uuid:
        return ApplyDecision(
            skip=True,
            reason="GPU hardware changed (UUID mismatch). Re-run optimizer for the new GPU.",
        )

    warning = ""
    if cfg.boot_apply.driver_version and current_driver and cfg.boot_apply.driver_version != current_driver:
        warning = f"Driver version changed from {cfg.boot_apply.driver_version} to {current_driver}."

    return ApplyDecision(skip=False, reason="", warning=warning)


def record_boot_result(cfg: UserConfig, success: bool, details: str) -> None:
    """Record boot-apply outcome. Increments strike counter on failure."""
    now = datetime.now().isoformat(timespec="seconds")
    cfg.boot_apply.last_apply_time = now
    cfg.boot_apply.last_apply_result = "success" if success else "failure"

    entry = {
        "timestamp": now,
        "action": "boot_apply",
        "result": "success" if success else "failure",
        "details": details,
    }
    cfg.boot_apply.boot_log.append(entry)

    if success:
        cfg.boot_apply.consecutive_failures = 0
        cfg.boot_apply.disabled = False
    else:
        cfg.boot_apply.consecutive_failures += 1
        if cfg.boot_apply.consecutive_failures >= 3:
            cfg.boot_apply.disabled = True
