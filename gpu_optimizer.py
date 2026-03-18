#!/usr/bin/env python3
"""
GPU Optimizer – Auto Overclock & Undervolt
==========================================

Usage
-----
  One-click (interactive):
      python gpu_optimizer.py

  Specify risk level directly:
      python gpu_optimizer.py --risk balanced
      python gpu_optimizer.py --risk performance

  List detected GPUs only:
      python gpu_optimizer.py --list

  Target a specific GPU by index:
      python gpu_optimizer.py --gpu 0 --risk balanced

  Monitor only (no changes):
      python gpu_optimizer.py --monitor

  Reset all GPUs to stock:
      python gpu_optimizer.py --reset

Requirements
------------
  pip install -r requirements.txt
  Run as Administrator for full NVAPI/power-limit control.
"""
from __future__ import annotations

import sys
import time

import click

# Add src/ to path so we can import our modules
sys.path.insert(0, str(__import__("pathlib").Path(__file__).parent))

from src.config import (
    RiskLevel,
    UserConfig,
    load_config,
    save_config,
    save_result,
)
from src.detector import detect_gpus, GPUInfo
from src.monitor import GPUMonitor
from src.optimizer import GPUOptimizer, optimize_all_gpus
from src.ui import (
    console,
    print_banner,
    print_gpu_list,
    print_risk_menu,
    prompt_risk_level,
    show_risk_warning,
    print_all_results,
    OptimizerProgressUI,
    metrics_row,
)
from src.backends.nvidia_smi import NvidiaSMIBackend
from src.backends.nvapi import NVAPIBackend
from src.backends.afterburner import AfterburnerBackend


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

@click.command(context_settings={"help_option_names": ["-h", "--help"]})
@click.option(
    "--risk", "-r",
    type=click.Choice(["safe", "balanced", "performance", "extreme"], case_sensitive=False),
    default=None,
    help="Optimization risk level. Skips the interactive menu.",
)
@click.option(
    "--gpu", "-g",
    type=int,
    default=None,
    help="GPU index to optimize (0-based). Default: all NVIDIA GPUs.",
)
@click.option(
    "--list", "-l", "list_only",
    is_flag=True,
    default=False,
    help="List detected GPUs and exit.",
)
@click.option(
    "--monitor", "-m",
    is_flag=True,
    default=False,
    help="Live monitoring mode – display GPU metrics without making changes.",
)
@click.option(
    "--reset",
    is_flag=True,
    default=False,
    help="Reset all GPUs to stock settings.",
)
@click.option(
    "--backends",
    is_flag=True,
    default=False,
    help="Show available control backends and exit.",
)
def main(
    risk:      str | None,
    gpu:       int | None,
    list_only: bool,
    monitor:   bool,
    reset:     bool,
    backends:  bool,
) -> None:
    """GPU Auto Optimizer – overclock, undervolt, and tune NVIDIA GPUs automatically."""

    print_banner()

    # --- Detect GPUs -------------------------------------------------------
    gpus = detect_gpus()
    if not gpus:
        console.print(
            "[bold red]No NVIDIA GPUs detected.\n"
            "Make sure the NVIDIA driver and nvidia-smi are installed.[/bold red]"
        )
        sys.exit(1)

    # Filter to requested GPU
    if gpu is not None:
        matching = [g for g in gpus if g.index == gpu]
        if not matching:
            console.print(f"[red]GPU index {gpu} not found. Available: {[g.index for g in gpus]}[/red]")
            sys.exit(1)
        gpus = matching

    # --- List mode ----------------------------------------------------------
    if list_only:
        print_gpu_list(gpus)
        sys.exit(0)

    # --- Backends info mode -------------------------------------------------
    if backends:
        _show_backends(gpus)
        sys.exit(0)

    # --- Reset mode ---------------------------------------------------------
    if reset:
        _do_reset(gpus)
        sys.exit(0)

    # --- Monitor mode -------------------------------------------------------
    if monitor:
        _do_monitor(gpus)
        sys.exit(0)

    # --- Full optimization --------------------------------------------------
    print_gpu_list(gpus)
    console.print()

    # Determine risk level
    cfg = load_config()
    if risk:
        chosen_risk = RiskLevel(risk.lower())
    else:
        chosen_risk = prompt_risk_level()

    # Show warning and get confirmation for non-safe levels
    if not show_risk_warning(chosen_risk):
        console.print("[yellow]Aborted.[/yellow]")
        sys.exit(0)

    console.print()
    console.print(f"[bold]Optimizing {len(gpus)} GPU(s) at [cyan]{chosen_risk.value.upper()}[/cyan] level…[/bold]")
    console.print(
        "[dim]The tool will test incrementally. "
        "Temporary glitches / screen flickers are normal during the search phase.[/dim]\n"
    )

    results = []
    with OptimizerProgressUI() as progress_ui:
        for g in gpus:
            console.print(f"\n[bold cyan]▶  {g.name} (GPU {g.index})[/bold cyan]")
            opt = GPUOptimizer(g, chosen_risk, progress_ui)
            try:
                result = opt.run()
            except RuntimeError as exc:
                console.print(f"[bold red]Optimization aborted:[/bold red] {exc}")
                sys.exit(2)
            except KeyboardInterrupt:
                console.print(
                    f"\n[bold yellow]⚠  Optimization interrupted for {g.name}[/bold yellow]"
                    " (system signal, not a user keypress)"
                )
                partial = opt.partial_result()
                if any([partial.core_offset_mhz, partial.mem_offset_mhz,
                        partial.voltage_offset_mv != 0, partial.power_limit_pct != 100]):
                    console.print("[bold]Best settings found before interrupt:[/bold]")
                    console.print(f"  Core offset:   [cyan]+{partial.core_offset_mhz} MHz[/cyan]")
                    console.print(f"  Mem offset:    [magenta]+{partial.mem_offset_mhz} MHz[/magenta]")
                    console.print(f"  Voltage offset:[yellow] {partial.voltage_offset_mv:+d} mV[/yellow]")
                    console.print(f"  Power limit:   [green]{partial.power_limit_pct} %[/green]")
                    console.print(
                        "\n[dim]These settings passed stability tests but were not saved."
                        "  Run again to complete and save.[/dim]"
                    )
                sys.exit(130)
            results.append(result)
            save_result(cfg, result)

    console.print()
    print_all_results(results)

    console.print()
    console.print(
        "[bold green]Optimization complete![/bold green] "
        "Settings are active until next reboot.\n"
        "To re-apply on boot, set [cyan]auto_apply_on_boot = true[/cyan] "
        "in optimizer_config.json."
    )


# ---------------------------------------------------------------------------
# Sub-commands
# ---------------------------------------------------------------------------

def _show_backends(gpus: list[GPUInfo]) -> None:
    from rich.table import Table

    t = Table(title="Control Backends", border_style="cyan")
    t.add_column("Backend",     style="bold")
    t.add_column("Priority",   justify="center")
    t.add_column("Available",  justify="center")
    t.add_column("Core OC",    justify="center")
    t.add_column("Mem OC",     justify="center")
    t.add_column("Undervolt",  justify="center")
    t.add_column("Notes")

    candidates = [
        NVAPIBackend(),
        AfterburnerBackend(),
        NvidiaSMIBackend(),
    ]

    idx = gpus[0].index if gpus else 0

    for b in candidates:
        avail = b.is_available()
        t.add_row(
            b.name,
            str(b.priority),
            "[green]✓[/green]" if avail else "[red]✗[/red]",
            "[green]✓[/green]" if avail and b.supports_core_oc(idx) else "[dim]–[/dim]",
            "[green]✓[/green]" if avail and b.supports_mem_oc(idx)  else "[dim]–[/dim]",
            "[green]✓[/green]" if avail and b.supports_voltage(idx)  else "[dim]–[/dim]",
            _backend_notes(b, avail),
        )

    console.print(t)


def _backend_notes(b, available: bool) -> str:
    if not available:
        if b.name == "nvapi-direct":
            return "Needs Windows + NVIDIA driver + admin rights"
        if b.name == "msi-afterburner":
            return "MSI Afterburner not installed"
        return "nvidia-smi not found"
    return "Ready"


def _do_reset(gpus: list[GPUInfo]) -> None:
    console.print("[bold yellow]Resetting all GPUs to stock settings…[/bold yellow]")
    candidates = [NVAPIBackend(), AfterburnerBackend(), NvidiaSMIBackend()]
    for g in gpus:
        for b in sorted(candidates, key=lambda x: -x.priority):
            if b.is_available():
                ok = b.reset(g.index)
                if ok:
                    console.print(f"[green]GPU {g.index} ({g.name}) reset via {b.name}[/green]")
                    break
                else:
                    console.print(f"[yellow]GPU {g.index}: {b.name} reset failed, trying next…[/yellow]")
        else:
            console.print(f"[red]GPU {g.index}: reset failed (no working backend)[/red]")


def _do_monitor(gpus: list[GPUInfo]) -> None:
    from rich.live import Live
    from rich.table import Table

    console.print("[bold]Live GPU monitoring – press Ctrl+C to stop.[/bold]\n")

    monitors = {g.index: GPUMonitor(g.index, poll_interval_sec=0.5) for g in gpus}

    def _make_table() -> Table:
        t = Table(border_style="dim", show_header=True)
        t.add_column("GPU",  style="bold cyan", width=6)
        t.add_column("Name", width=24)
        t.add_column("Core",   justify="right", width=10)
        t.add_column("Mem",    justify="right", width=10)
        t.add_column("Temp",   justify="right", width=8)
        t.add_column("Power",  justify="right", width=9)
        t.add_column("Fan",    justify="right", width=6)
        t.add_column("Util",   justify="right", width=6)
        t.add_column("Status", width=20)

        for g in gpus:
            mon = monitors[g.index]
            m   = mon.read_once()
            status = ""
            if m.is_thermal_limit:  status = "[yellow]THERMAL[/yellow]"
            elif m.is_power_limit:  status = "[blue]POWER[/blue]"
            elif m.is_throttling:   status = "[red]THROTTLE[/red]"
            else:                   status = "[green]OK[/green]"

            t.add_row(
                str(g.index),
                g.name[:24],
                f"{m.core_clock_mhz} MHz",
                f"{m.mem_clock_mhz} MHz",
                f"{m.temp_c:.0f} °C",
                f"{m.power_w:.0f} W",
                f"{m.fan_speed_pct} %",
                f"{m.gpu_util_pct} %",
                status,
            )
        return t

    try:
        with Live(console=console, refresh_per_second=2) as live:
            while True:
                live.update(_make_table())
                time.sleep(0.5)
    except KeyboardInterrupt:
        console.print("\n[dim]Monitoring stopped.[/dim]")


# ---------------------------------------------------------------------------

if __name__ == "__main__":
    main()
