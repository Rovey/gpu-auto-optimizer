"""The monitor must survive an NVML handle invalidated by another component's
nvmlShutdown (which surfaces as an OSError access violation), by re-acquiring
the handle and retrying — never propagating the crash."""
import src.monitor as mm


class _FakeNVMLError(Exception):
    pass


class _FakeUtil:
    gpu = 50
    memory = 30


class _FakeMem:
    used = 1024 * 1024 * 1024
    total = 12 * 1024 * 1024 * 1024


class FakeNvml:
    NVMLError = _FakeNVMLError
    NVML_CLOCK_GRAPHICS = 0
    NVML_CLOCK_MEM = 2
    NVML_TEMPERATURE_GPU = 0
    NVML_MEMORY_ERROR_TYPE_UNCORRECTED = 1
    NVML_VOLATILE_ECC = 0

    def __init__(self):
        self.stale = False
        self.init_count = 0
        self.fail_reacquire = False

    def invalidate(self):
        self.stale = True

    def nvmlInit(self):
        if self.fail_reacquire:
            raise OSError("nvml init failed")
        self.init_count += 1
        self.stale = False

    def nvmlDeviceGetHandleByIndex(self, i):
        return object()

    def nvmlDeviceGetClockInfo(self, h, t):
        if self.stale:
            raise OSError("access violation reading 0x0")
        return 1500 if t == self.NVML_CLOCK_GRAPHICS else 8000

    def nvmlDeviceGetMaxClockInfo(self, h, t): return 3000
    def nvmlDeviceGetTemperature(self, h, s): return 55
    def nvmlDeviceGetPowerUsage(self, h): return 150000
    def nvmlDeviceGetPowerManagementLimit(self, h): return 200000
    def nvmlDeviceGetFanSpeed(self, h): return 40
    def nvmlDeviceGetUtilizationRates(self, h): return _FakeUtil()
    def nvmlDeviceGetMemoryInfo(self, h): return _FakeMem()
    def nvmlDeviceGetTotalEccErrors(self, h, a, b): return 0
    def nvmlDeviceGetCurrentClocksThrottleReasons(self, h): return 0


def _make_monitor(monkeypatch, fake):
    monkeypatch.setattr(mm, "pynvml", fake)
    monkeypatch.setattr(mm, "_NVML_AVAILABLE", True)
    return mm.GPUMonitor(0)


def test_read_reacquires_handle_after_external_shutdown(monkeypatch):
    fake = FakeNvml()
    mon = _make_monitor(monkeypatch, fake)
    assert fake.init_count == 1            # initial acquire at construction

    fake.invalidate()                      # another component called nvmlShutdown
    m = mon.read_once()                    # must NOT raise OSError

    assert m.core_clock_mhz == 1500        # re-acquired and read succeeded
    assert fake.init_count == 2            # re-acquire happened exactly once


def test_read_returns_empty_metrics_if_reacquire_also_fails(monkeypatch):
    fake = FakeNvml()
    mon = _make_monitor(monkeypatch, fake)
    fake.invalidate()
    fake.fail_reacquire = True             # re-acquire can't recover either

    m = mon.read_once()                    # still must not raise

    assert m.core_clock_mhz == 0           # graceful empty metrics
    assert m.gpu_index == 0
