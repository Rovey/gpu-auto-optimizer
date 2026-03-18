"""
Rich-based terminal UI helpers: GPU info panels, live monitoring table,
progress display, risk warnings, and result summaries.
"""
from __future__ import annotations

import time
from typing import List, Optional

from rich.align import Align
from rich.columns import Columns
from rich.console import Console
from rich.live import Live
from rich.panel import Panel
from rich.progress import (
    BarColumn,
    Progress,
    SpinnerColumn,
    TaskProgressColumn,
    TextColumn,
    TimeElapsedColumn,
)
from rich.prompt import Confirm
from rich.rule import Rule
from rich.table import Table
from rich.text import Text

from .config import RiskLevel, RISK_PROFILES, GPUOptimizationResult
from .detector import GPUInfo
from .monitor import GPUMetrics

console = Console()


# ---------------------------------------------------------------------------
# Banner
# ---------------------------------------------------------------------------

BANNER = r"""
  ______ _____  _    _    ___        _   _           _
 / _____|  __ \| |  | |  / _ \      | | (_)         (_)
| |  __ | |__) | |  | | | | | |_ __ | |_ _ _ __ ___  _ _______ _ __
| | |_ ||  ___/| |  | | | | | | '_ \| __| | '_ ` _ \| |_  / _ \ '__|
| |__| || |    | |__| | | |_| | |_) | |_| | | | | | | |/ /  __/ |
 \_____||_|     \____/   \___/| .__/ \__|_|_| |_| |_|_/___\___|_|
                               | |
                               |_|   NVIDIA GPU Auto-Overclock/Undervolt
"""


def print_banner() -> None:
    console.print(Text(BANNER, style="bold cyan"))
    console.print(
        "[dim]Supports all NVIDIA GPUs (GTX/RTX) - AMD support coming soon[/dim]\n"
    )


# ---------------------------------------------------------------------------
# GPU info panel
# ---------------------------------------------------------------------------

def gpu_info_panel(gpu: GPUInfo) -> Panel:
    t = Table.grid(padding=(0, 2))
    t.add_column(style="bold cyan", min_width=22)
    t.add_column(style="white")

    t.add_row("GPU index",      str(gpu.index))
    t.add_row("Name",           gpu.name)
    t.add_row("Vendor",         gpu.vendor)
    t.add_row("Architecture",   gpu.architecture)
    t.add_row("VRAM",           f"{gpu.vram_mb // 1024} GB ({gpu.vram_mb} MB)")
    t.add_row("Default TDP",    f"{gpu.tdp_w} W" if gpu.tdp_w else "unknown")
    t.add_row("Power range",    f"{gpu.min_power_limit_w}-{gpu.max_power_limit_w} W" if gpu.max_power_limit_w else "unknown")
    t.add_row("Driver",         gpu.driver_version)
    t.add_row("OC support",     "Yes" if gpu.supports_oc  else "No")
    t.add_row("UV support",     "Yes (VF curve)" if gpu.supports_uv else "No")

    return Panel(t, title=f"[bold green]{gpu.name}[/bold green]", border_style="cyan")


def print_gpu_list(gpus: List[GPUInfo]) -> None:
    if not gpus:
        console.print("[red]No GPUs detected! Is nvidia-smi or pynvml available?[/red]")
        return
    cols = [gpu_info_panel(g) for g in gpus]
    console.print(Columns(cols, equal=False, expand=False))


# ---------------------------------------------------------------------------
# Risk level menu
# ---------------------------------------------------------------------------

def print_risk_menu() -> None:
    t = Table(title="Choose Optimization Level", border_style="dim", show_header=True)
    t.add_column("#",      style="bold", justify="center", width=4)
    t.add_column("Level",  style="bold", width=14)
    t.add_column("Description")
    t.add_column("Core OC",   justify="center", width=10)
    t.add_column("Mem OC",    justify="center", width=10)
    t.add_column("Undervolt", justify="center", width=10)

    rows = [
        (RiskLevel.SAFE,        "1"),
        (RiskLevel.BALANCED,    "2"),
        (RiskLevel.PERFORMANCE, "3"),
        (RiskLevel.EXTREME,     "4"),
    ]
    for lvl, num in rows:
        p = RISK_PROFILES[lvl]
        core = f"+{p['core_offset_mhz_max']} MHz" if p["core_offset_mhz_max"] else "none"
        mem  = f"+{p['mem_offset_mhz_max']} MHz" if p["mem_offset_mhz_max"] else "none"
        uv   = f"{p['voltage_offset_mv_min']} mV" if p["voltage_offset_mv_min"] else "none"
        t.add_row(
            num,
            f"[{p['color']}]{p['label']}[/{p['color']}]",
            p["description"][:80],
            core, mem, uv,
        )
    console.print(t)


def prompt_risk_level() -> RiskLevel:
    print_risk_menu()
    mapping = {"1": RiskLevel.SAFE, "2": RiskLevel.BALANCED,
               "3": RiskLevel.PERFORMANCE, "4": RiskLevel.EXTREME}
    while True:
        choice = console.input("\n[bold]Select level [1-4] (default 2): [/bold]").strip()
        if choice == "":
            return RiskLevel.BALANCED
        if choice in mapping:
            return mapping[choice]
        console.print("[red]Invalid choice, enter 1-4.[/red]")


# ---------------------------------------------------------------------------
# Risk warning
# ---------------------------------------------------------------------------

def show_risk_warning(risk: RiskLevel) -> bool:
    """Show warning for non-safe levels. Returns True if user accepts."""
    p = RISK_PROFILES[risk]
    if p["warning"] is None:
        return True

    console.print()
    console.print(
        Panel(
            p["warning"],
            title=f"[bold {p['color']}]!  {p['label']} MODE WARNING[/bold {p['color']}]",
            border_style=p["color"],
        )
    )
    return Confirm.ask("\nDo you accept the risks and wish to continue?", default=False)


# ---------------------------------------------------------------------------
# Live metrics row
# ---------------------------------------------------------------------------

def metrics_row(m: GPUMetrics) -> str:
    throttle = " [red]THROTTLE[/red]" if m.is_throttling else ""
    tdr      = " [bold red]TDR![/bold red]" if m.ecc_errors > 0 else ""
    return (
        f"[cyan]{m.core_clock_mhz:>5} MHz[/cyan]  "
        f"[magenta]{m.mem_clock_mhz:>5} MHz[/magenta]  "
        f"[yellow]{m.temp_c:>4.0f} °C[/yellow]  "
        f"[green]{m.power_w:>5.0f} W[/green]  "
        f"Fan [white]{m.fan_speed_pct:>3}%[/white]  "
        f"Util [white]{m.gpu_util_pct:>3}%[/white]"
        f"{throttle}{tdr}"
    )


# ---------------------------------------------------------------------------
# Progress callback for the optimizer
# ---------------------------------------------------------------------------

class OptimizerProgressUI:
    """Wraps `rich.progress.Progress` for use as an optimizer progress_cb."""

    def __init__(self) -> None:
        self._progress = Progress(
            SpinnerColumn(),
            TextColumn("[bold blue]{task.description}"),
            BarColumn(bar_width=40),
            TaskProgressColumn(),
            TimeElapsedColumn(),
            console=console,
        )
        self._task_id = None
        self._live    = None

    def __enter__(self) -> "OptimizerProgressUI":
        self._progress.__enter__()
        self._task_id = self._progress.add_task("Initialising…", total=10)
        return self

    def __exit__(self, *args) -> None:
        self._progress.__exit__(*args)

    def __call__(
        self,
        phase:  str,
        step:   int,
        total:  int,
        m:      Optional[GPUMetrics],
    ) -> None:
        if self._task_id is None:
            return
        self._progress.update(self._task_id, completed=step, total=total, description=phase)
        if m:
            self._progress.console.print(
                f"  [dim]→ {metrics_row(m)}[/dim]",
                highlight=False,
            )


# ---------------------------------------------------------------------------
# Result summary
# ---------------------------------------------------------------------------

def print_result_summary(result: GPUOptimizationResult) -> None:
    console.print(Rule(style="cyan"))
    status_color = "bold green" if result.stability_passed else "bold red"
    status_text  = "OK  STABLE" if result.stability_passed else "FAIL  UNSTABLE - rolled back"

    t = Table(title=f"Results: {result.gpu_name}", border_style="cyan", show_header=True)
    t.add_column("Setting",  style="bold", width=24)
    t.add_column("Baseline", justify="right", width=14)
    t.add_column("Achieved", justify="right", width=14)
    t.add_column("Delta",    justify="right", width=12)

    def _row(label: str, base: float, achieved: float, unit: str, invert: bool = False) -> None:
        delta = achieved - base
        if abs(delta) < 0.5:
            delta_str = "[dim]~0[/dim]"
        else:
            is_good   = (delta > 0) != invert
            color     = "green" if is_good else "red"
            sign      = "+" if delta > 0 else ""
            delta_str = f"[{color}]{sign}{delta:.0f} {unit}[/{color}]"
        t.add_row(
            label,
            f"{base:.0f} {unit}",
            f"{achieved:.0f} {unit}",
            delta_str,
        )

    _row("Boost clock",   result.baseline_boost_mhz,  result.achieved_boost_mhz,  "MHz")
    _row("Temperature",   result.baseline_temp_c,      result.achieved_temp_c,     "°C",  invert=True)
    _row("Power draw",    result.baseline_power_w,     result.achieved_power_w,    "W",   invert=True)

    console.print(t)

    settings_t = Table.grid(padding=(0, 3))
    settings_t.add_column(style="bold cyan", min_width=22)
    settings_t.add_column()

    settings_t.add_row("Core offset",     f"+{result.core_offset_mhz} MHz")
    settings_t.add_row("Memory offset",   f"+{result.mem_offset_mhz} MHz")
    settings_t.add_row("Voltage offset",  f"{result.voltage_offset_mv:+d} mV")
    settings_t.add_row("Power limit",     f"{result.power_limit_pct} %")
    settings_t.add_row("Status",          f"[{status_color}]{status_text}[/{status_color}]")
    if result.notes:
        settings_t.add_row("Notes", f"[dim]{result.notes}[/dim]")

    console.print(Panel(settings_t, title="Applied Settings", border_style="green"))


def print_all_results(results: List[GPUOptimizationResult]) -> None:
    for r in results:
        print_result_summary(r)
