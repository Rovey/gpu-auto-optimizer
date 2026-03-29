"""Headless boot-apply script — applies saved GPU settings on login."""
from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime

from .config import UserConfig, load_config, save_config


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


def _apply_on_boot() -> None:
    """Entry point for headless boot-apply. Intended to run via Task Scheduler."""
    import logging
    import sys

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [boot_apply] %(message)s",
    )
    log = logging.getLogger(__name__)

    cfg = load_config()

    # Detect current GPU
    from .detector import detect_gpus
    gpus = detect_gpus()
    if not gpus:
        log.warning("No GPUs detected. Skipping boot-apply.")
        record_boot_result(cfg, False, "No GPUs detected")
        save_config(cfg)
        return

    gpu = gpus[0]
    decision = should_apply(cfg, current_gpu_uuid=gpu.uuid, current_driver=gpu.driver_version)

    if decision.skip:
        log.info("Skipping boot-apply: %s", decision.reason)
        return

    if decision.warning:
        log.warning("Boot-apply warning: %s", decision.warning)

    # Find the saved result for this GPU
    saved = cfg.per_gpu_results.get(gpu.name)
    if not saved:
        log.warning("No saved result for GPU '%s'. Skipping.", gpu.name)
        record_boot_result(cfg, False, f"No saved result for {gpu.name}")
        save_config(cfg)
        return

    # Apply via best available backend
    from .optimizer import _best_backend
    backend = _best_backend(gpu)

    try:
        target_voltage_mv = saved.get("target_voltage_mv", 0)
        target_freq_mhz = saved.get("target_freq_mhz", 0)

        # Check if this was a V/F curve result
        from .backends.nvapi_vfcurve import NVAPIVFCurveBackend
        if target_voltage_mv > 0 and isinstance(backend, NVAPIVFCurveBackend):
            # V/F curve path: re-apply curve at saved voltage/frequency
            result = backend.apply(
                gpu_index=gpu.index,
                mem_offset_mhz=saved.get("mem_offset_mhz", 0),
                power_limit_pct=saved.get("power_limit_pct", 100),
                thermal_limit_c=saved.get("thermal_limit_c", 83),
                target_voltage_mv=target_voltage_mv,
                target_freq_mhz=target_freq_mhz,
            )
        else:
            # PState20 fallback path
            result = backend.apply(
                gpu_index=gpu.index,
                core_offset_mhz=saved.get("core_offset_mhz", 0),
                mem_offset_mhz=saved.get("mem_offset_mhz", 0),
                voltage_offset_mv=saved.get("voltage_offset_mv", 0),
                power_limit_pct=saved.get("power_limit_pct", 100),
                thermal_limit_c=saved.get("thermal_limit_c", 83),
            )

        if result.success:
            log.info("Boot-apply succeeded: %s", result.notes)
            record_boot_result(cfg, True, result.notes)
        else:
            log.error("Boot-apply failed: %s", result.notes)
            record_boot_result(cfg, False, result.notes)
    except Exception as exc:
        log.error("Boot-apply exception: %s", exc)
        record_boot_result(cfg, False, str(exc))

    # Update GPU UUID and driver for future checks
    cfg.boot_apply.gpu_uuid = gpu.uuid
    cfg.boot_apply.driver_version = gpu.driver_version
    save_config(cfg)


if __name__ == "__main__":
    import sys
    from pathlib import Path
    # Ensure project root is importable
    sys.path.insert(0, str(Path(__file__).parent.parent))
    _apply_on_boot()
