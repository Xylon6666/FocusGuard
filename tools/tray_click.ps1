param(
    [int]$Item = 4,            # 1-based，要点击的菜单项序号（不含分隔线的从上往下）
    [string]$Shot = ""         # 可选：点击前抓一张菜单图
)
$ErrorActionPreference = "Stop"

Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class TC {
  [DllImport("user32.dll")] public static extern bool SetThreadDpiAwarenessContext(IntPtr v);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
}
"@

[void][TC]::SetThreadDpiAwarenessContext([IntPtr]-4)
$fg = (Get-Process FocusGuard).Id

$script:main = [IntPtr]::Zero
[void][TC]::EnumWindows({
    param($h, $p)
    $q = 0
    [void][TC]::GetWindowThreadProcessId($h, [ref]$q)
    if ($q -eq $fg) {
        $sb = New-Object System.Text.StringBuilder 128
        [void][TC]::GetClassNameW($h, $sb, 128)
        if ($sb.ToString() -eq "FocusGuard.MainWindow") { $script:main = $h; return $false }
    }
    return $true
}, [IntPtr]::Zero)
if ($script:main -eq [IntPtr]::Zero) { throw "main not found" }

[void][TC]::PostMessageW($script:main, 32869, [IntPtr]1, [IntPtr]0x0205)   # WM_RBUTTONUP
Start-Sleep -Milliseconds 400

$script:menu = [IntPtr]::Zero
[void][TC]::EnumWindows({
    param($h, $p)
    $sb = New-Object System.Text.StringBuilder 128
    [void][TC]::GetClassNameW($h, $sb, 128)
    if ($sb.ToString() -eq "#32768") { $script:menu = $h; return $false }
    return $true
}, [IntPtr]::Zero)
if ($script:menu -eq [IntPtr]::Zero) { throw "menu not found" }

$r = New-Object TC+RECT
[void][TC]::GetWindowRect($script:menu, [ref]$r)
$w = $r.right - $r.left
$h = $r.bottom - $r.top
Write-Output ("menu=(" + $r.left + "," + $r.top + ") " + $w + "x" + $h)

# 行高实测：菜单里 7 项 + 1 条分隔线 + 上下边框；分隔线约 10 DIP（按 DPI 折算）
$rowH = [int](($h - 6 - 12) / 7)
$cy   = $r.top + 3 + ($Item - 1) * $rowH + [int]($rowH / 2)
$cx   = $r.left + 80

if ($Shot -ne "") {
    Add-Type -AssemblyName System.Drawing
    [void][TC]::SetCursorPos($cx, $cy)
    Start-Sleep -Milliseconds 250
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.left, $r.top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Output ("shot -> " + $Shot)
}

Write-Output ("click at " + $cx + "," + $cy + " (rowH=" + $rowH + ")")
[void][TC]::SetCursorPos($cx, $cy)
Start-Sleep -Milliseconds 250
[void][TC]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 120
[void][TC]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 700
