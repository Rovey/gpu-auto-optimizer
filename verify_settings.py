#!/usr/bin/env python3
"""Verify that GPU optimization settings are actually applied to hardware."""
from __future__ import annotations

import sys
import time
from pathlib import Path

# Ensure src/ is importable
sys.path.insert(0, str(Path(__file__).parent))


def main() -> None:
    print("=" * 60)
    print("  GPU Settings Verification")
    print("=" * 60)
    print()

    # ---- Step 1: Load saved config ----
    from src.config import load_config
    cfg = load_config()

    if not cfg.per_gpu_results:
        print("[!] No saved optimization results found in config.")
        print("    Run the optimizer first.")
        return

    saved = list(cfg.per_gpu_results.values())[-1]
    gpu_name = saved.get("gpu_name", "Unknown")
    expected_core = saved.get("core_offset_mhz", 0)
    expected_mem = saved.get("mem_offset_mhz", 0)
    expected_voltage = saved.get("voltage_offset_mv", 0)
    expected_power = saved.get("power_limit_pct", 100)

    print(f"GPU: {gpu_name}")
    print()
    print("--- Expected (from saved config) ---")
    print(f"  Core offset:    +{expected_core} MHz")
    print(f"  Memory offset:  +{expected_mem} MHz")
    print(f"  Voltage offset: {expected_voltage:+d} mV")
    print(f"  Power limit:    {expected_power}%")
    print()

    # ---- Step 2: NVAPI register read-back ----
    print("--- Check 1: NVAPI PState20 Register Read-back ---")
    nvapi_ok = False
    try:
        from src.backends.nvapi import NVAPIBackend
        backend = NVAPIBackend()
        if backend.is_available():
            readback = backend.verify(0)
            if readback is not None:
                actual_core = readback["core_offset_khz"] // 1000
                actual_mem = readback["mem_offset_khz"] // 1000

                core_match = abs(actual_core - expected_core) <= 1
                mem_match = abs(actual_mem - expected_mem) <= 1

                status_core = "OK" if core_match else "MISMATCH"
                status_mem = "OK" if mem_match else "MISMATCH"

                print(f"  Core offset: +{actual_core} MHz [{status_core}]")
                print(f"  Mem offset:  +{actual_mem} MHz [{status_mem}]")

                if core_match and mem_match:
                    print("  >> Offsets are programmed correctly in the driver.")
                    nvapi_ok = True
                else:
                    print("  >> WARNING: Offsets do NOT match saved settings!")
            else:
                print("  Could not read PState20 buffer.")
        else:
            print("  NVAPI not available (not on Windows or no nvapi64.dll).")
    except Exception as e:
        print(f"  Error: {e}")
    print()

    # ---- Step 1b: V/F Curve read-back ----
    if saved.get("target_voltage_mv", 0) > 0:
        print("--- Check 1b: V/F Curve Read-back ---")
        try:
            from src.backends.nvapi_vfcurve import NVAPIVFCurveBackend
            vf_backend = NVAPIVFCurveBackend()
            if vf_backend.is_available():
                curve = vf_backend.read_vf_curve(0)
                if curve:
                    target_v = saved["target_voltage_mv"]
                    target_f = saved["target_freq_mhz"]
                    # Find the point closest to target voltage
                    closest = min(curve, key=lambda p: abs(p.voltage_mv - target_v))
                    eff_freq = closest.effective_freq_mhz
                    print(f"  Target:  {target_f} MHz @ {target_v} mV")
                    print(f"  Actual:  {eff_freq} MHz @ {closest.voltage_mv} mV")
                    print(f"  Curve points: {len(curve)}")
                    if abs(eff_freq - target_f) <= 25 and abs(closest.voltage_mv - target_v) <= 13:
                        print("  >> V/F curve is applied correctly!")
                    else:
                        print("  >> WARNING: V/F curve does not match saved settings.")
                else:
                    print("  Could not read V/F curve.")
            else:
                print("  V/F curve backend not available.")
        except Exception as e:
            print(f"  Error: {e}")
        print()
    elif expected_core > 0:
        # Only check PState20 if not using V/F curve
        pass  # existing PState20 check already ran above

    # ---- Step 3: Power limit check via pynvml ----
    print("--- Check 2: Power Limit (pynvml) ---")
    try:
        import pynvml
        pynvml.nvmlInit()
        try:
            h = pynvml.nvmlDeviceGetHandleByIndex(0)
            current_mw = pynvml.nvmlDeviceGetPowerManagementLimit(h)
            default_mw = pynvml.nvmlDeviceGetPowerManagementDefaultLimit(h)
            min_mw, max_mw = pynvml.nvmlDeviceGetPowerManagementLimitConstraints(h)

            current_w = current_mw / 1000
            default_w = default_mw / 1000
            actual_pct = (current_mw / default_mw) * 100

            pct_match = abs(actual_pct - expected_power) < 2
            status = "OK" if pct_match else "MISMATCH"

            print(f"  Default:  {default_w:.0f} W")
            print(f"  Current:  {current_w:.0f} W ({actual_pct:.0f}%) [{status}]")
            print(f"  Range:    {min_mw/1000:.0f} - {max_mw/1000:.0f} W")

            if pct_match:
                print("  >> Power limit matches saved setting.")
            else:
                print(f"  >> WARNING: Expected {expected_power}%, got {actual_pct:.0f}%")
        finally:
            pynvml.nvmlShutdown()
    except Exception as e:
        print(f"  Error: {e}")
    print()

    # ---- Step 4: Live clock reading (idle) ----
    print("--- Check 3: Current Clocks (idle snapshot) ---")
    try:
        from src.monitor import GPUMonitor
        mon = GPUMonitor(0)
        m = mon.read_once()
        print(f"  Core clock:  {m.core_clock_mhz} MHz")
        print(f"  Mem clock:   {m.mem_clock_mhz} MHz")
        print(f"  Boost clock: {m.boost_clock_mhz} MHz (max at current P-state)")
        print(f"  Temperature: {m.temp_c:.0f} C")
        print(f"  Power draw:  {m.power_w:.1f} W")
        print(f"  GPU util:    {m.gpu_util_pct}%")
        print()
        print("  Note: Idle clocks are usually low due to power saving.")
        print("  Offsets only show at load. Running load test next...")
    except Exception as e:
        print(f"  Error: {e}")
    print()

    # ---- Step 5: Brief load test to see boosted clocks ----
    print("--- Check 4: Clocks Under Load (5s stress) ---")
    try:
        import warnings
        with warnings.catch_warnings():
            warnings.filterwarnings("ignore", message="CUDA path could not be detected.*", category=UserWarning)
            import cupy as cp

        from src.monitor import GPUMonitor
        import threading

        mon = GPUMonitor(0)
        abort = threading.Event()

        def _stress():
            try:
                with cp.cuda.Device(0):
                    a = cp.random.random((4096, 4096), dtype=cp.float32)
                    b = cp.random.random((4096, 4096), dtype=cp.float32)
                    c = cp.empty((4096, 4096), dtype=cp.float32)
                    while not abort.is_set():
                        cp.matmul(a, b, out=c)
                        cp.cuda.Stream.null.synchronize()
            except Exception:
                pass

        t = threading.Thread(target=_stress, daemon=True)
        t.start()

        # Let clocks ramp up
        time.sleep(2)

        # Sample 3 seconds of load clocks
        clocks = []
        mem_clocks = []
        for _ in range(6):
            m = mon.read_once()
            if m.gpu_util_pct > 50:
                clocks.append(m.core_clock_mhz)
                mem_clocks.append(m.mem_clock_mhz)
            time.sleep(0.5)

        abort.set()
        t.join(timeout=3)

        if clocks:
            avg_core = sum(clocks) / len(clocks)
            max_core = max(clocks)
            avg_mem = sum(mem_clocks) / len(mem_clocks)

            print(f"  Core clock (avg):  {avg_core:.0f} MHz")
            print(f"  Core clock (max):  {max_core} MHz")
            print(f"  Mem clock (avg):   {avg_mem:.0f} MHz")
            print(f"  Samples: {len(clocks)}")

            # Compare against boost clock from idle reading
            from src.monitor import GPUMonitor as M2
            # Stock boost is the max clock reported by driver
            print()
            if nvapi_ok:
                print(f"  >> Offsets verified in NVAPI and GPU is boosting to {max_core} MHz.")
                print(f"     Settings are taking effect.")
            else:
                print(f"  >> GPU is boosting to {max_core} MHz under load.")
                print(f"     Compare this against your GPU's stock boost clock")
                print(f"     to confirm the offset is working.")
        else:
            print("  Could not capture load clocks (GPU util stayed low).")

    except ImportError:
        print("  CuPy not available -- cannot run load test.")
        print("  Run a game or benchmark and check clocks with:")
        print("    nvidia-smi --query-gpu=clocks.current.graphics --format=csv -l 1")
    except Exception as e:
        print(f"  Error: {e}")

    print()
    print("=" * 60)
    print("  Verification complete")
    print("=" * 60)


if __name__ == "__main__":
    main()
