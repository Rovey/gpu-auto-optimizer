# Pictures of the app's own window, for checking the GUI without a real run.
#
# Starts GpuAutoOptimizer.exe in its demo mode (a played run: it touches no card and writes no file),
# sizes the window, takes a picture of that window only at the given seconds after its start, and closes
# it. The pictures come from PrintWindow on the app's window handle, never from the screen, so nothing
# else on the desktop ends up in them.
#
# Run it NOT elevated (the demo refuses to start elevated), from the repository root, after a build:
#   Set-ExecutionPolicy -Scope Process Bypass
#   & .\tools\window-shot.ps1 -Exe .\build\Release\GpuAutoOptimizer.exe -Prefix .\out\shots\run -Mode '--demo=run' -At @(9, 28, 88)
# Call it with `&`, not `powershell -File`: an -At array does not survive -File.
#
# -Mode '--demo' plays the run with the busy-card question first; '--demo=run' plays it without the question.
# -Width/-Height set the outer window size in physical pixels (the process is DPI aware).
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$Prefix,
    [string]$Mode = '--demo',
    [int[]]$At = @(6),
    [int]$Width = 1180,
    [int]$Height = 900
)
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Drawing; using System.Drawing.Imaging;
public static class Shot {
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static string Take(IntPtr h, string path) {
    RECT r; if (!GetWindowRect(h, out r)) return "no rect";
    int w = r.R - r.L, ht = r.B - r.T; if (w <= 0 || ht <= 0) return "empty rect";
    using (var bmp = new Bitmap(w, ht, PixelFormat.Format32bppArgb)) {
      bool ok; using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); ok = PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
      if (!ok) return "no picture (PrintWindow failed)";
      bmp.Save(path, ImageFormat.Png); return "PrintWindow " + w + "x" + ht;
    }
  }
}
'@
[void][Shot]::SetProcessDPIAware()
$dir = Split-Path -Parent $Prefix
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$p = Start-Process $Exe -ArgumentList $Mode -PassThru
$started = Get-Date
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 40 -and $h -eq [IntPtr]::Zero -and -not $p.HasExited; $i++) { Start-Sleep -Milliseconds 250; $p.Refresh(); $h = $p.MainWindowHandle }
# A process that ended at once (for example in a session without a desktop, such as SSH) has no handle.
if ($p.HasExited) { "no window: the program ended with exit code $($p.ExitCode)"; exit 1 }
if ($h -eq [IntPtr]::Zero) { "no window"; Stop-Process -Id $p.Id -Force; exit 1 }
[void][Shot]::SetWindowPos($h, [IntPtr]::Zero, 0, 0, $Width, $Height, 0x16)   # SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE: size only
foreach ($t in $At) {
    $wait = $t - ((Get-Date) - $started).TotalSeconds
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
    "$t s: " + [Shot]::Take($h, "$Prefix-$t.png")
}
[void][Shot]::PostMessageW($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)   # WM_CLOSE: the demo ends when its window is closed
for ($i = 0; $i -lt 20 -and -not $p.HasExited; $i++) { Start-Sleep -Milliseconds 500 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; "ended by force" } else { "closed" }
