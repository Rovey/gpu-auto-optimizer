"""
AMD GPU backend stub.
Full AMD support (via ADL/ADLX SDK or ROCm) is planned for a future release.
"""
from __future__ import annotations

from .base import GPUBackend, AppliedSettings


class AMDBackend(GPUBackend):
    """Placeholder – AMD support coming soon."""

    name     = "amd-adlx"
    priority = 0

    def is_available(self) -> bool:
        return False   # Not yet implemented

    def apply(self, gpu_index, **kwargs) -> AppliedSettings:
        return AppliedSettings(
            success=False,
            notes=(
                "AMD GPU control is not yet implemented. "
                "Future support planned via AMD ADLX SDK. "
                "For now, use AMD Software: Adrenalin Edition manually."
            ),
        )

    def reset(self, gpu_index: int) -> bool:
        return False
