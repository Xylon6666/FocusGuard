param(
    [string]$Out = "build\traymenu_new.png",
    [switch]$Click,
    [int]$ItemIndex = -1
)

Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class TM {
  [DllImport("user32.dll")] public static extern bool SetThreadDpiAwarenessContext(IntPtr v);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string name);
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
}
"@

[void][TM]::SetThreadDpiAwarenessContext([IntPtr]-4)

$fgPid = (Get-Process FocusGuard).Id
$main = [IntPtr]::Zero
[void][TM]::EnumWindows({
    param($h, $p)
    $q = 0
    [void][TM]::GetWindowThreadProcessId($h, [ref]$q)
    if ($q -eq $fgPid) {
        $sb = New-Object System.Text.StringBuilder 128
        [void][TM]::GetClassNameW($h, $sb, 128)
        if ($sb.ToString() -eq "FocusGuard.MainWindow") { $script:main = $h; return $false }
    }
    return $true
}, [IntPtr]::Zero)

if ($main -eq [IntPtr]::Zero) { throw "main window not found" }

# 弹托盘菜单
[void][TM]::PostMessageW($main, 32869, [IntPtr]1, [IntPtr]0x0205)
Start-Sleep -Milliseconds 350

$menu = [IntPtr]::Zero
[void][TM]::EnumWindows({
    param($h, $p)
    $sb = New-Object System.Text.StringBuilder 128
    [void][TM]::GetClassNameW($h, $sb, 128)
    if ($sb.ToString() -eq "#32768") { $script:menu = $h; return $false }
    return $true
}, [IntPtr]::Zero)

if ($menu -eq [IntPtr]::Zero) { throw "menu window not found" }

$r = New-Object TM+RECT
[void][TM]::GetWindowRect($menu, [ref]$r)
$w = $r.right - $r.left; $h = $r.bottom - $r.top
Write-Output ("menu rect: " + $r.left + "," + $r.top + " " + $w + "x" + $h)

Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r.left, $r.top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Output ("saved " + $Out)

if ($Click -and $ItemIndex -ge 0) {
    # 每行高度实测（菜单行高随 DPI 变化，按 rect 高度除以行数估），这里按 32px@144 的经验值线性推算：
    # 由外部直接给出 y 偏移更稳，故用参数 $Y
}
