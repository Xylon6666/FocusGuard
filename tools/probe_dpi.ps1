param(
    [int]$X = 2800,
    [int]$Y = 300,
    [string]$Out = "build\dpi150.png"
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class DP {
  [DllImport("user32.dll")] public static extern bool SetThreadDpiAwarenessContext(IntPtr v);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
  [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr h, IntPtr dc);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr dc);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int w, int h);
  [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr dc, IntPtr o);
  [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
  [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr dc);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
  public static IntPtr Find(string cls) {
    IntPtr found = IntPtr.Zero;
    DP.EnumWindows(delegate(IntPtr h, IntPtr p) {
      var sb = new StringBuilder(256);
      DP.GetClassNameW(h, sb, 256);
      if (sb.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
}
"@

[void][DP]::SetThreadDpiAwarenessContext([IntPtr]-4)

$h = [DP]::Find("FocusGuard.SettingsWindow")
if ($h -eq [IntPtr]::Zero) { "NO SETTINGS WINDOW"; exit 1 }

function Show-State([string]$tag) {
    $wr = New-Object DP+RECT
    [void][DP]::GetWindowRect($h, [ref]$wr)
    $cr = New-Object DP+RECT
    [void][DP]::GetClientRect($h, [ref]$cr)
    $dpi = [DP]::GetDpiForWindow($h)
    $n = 0
    [void][DP]::EnumChildWindows($h, { param($c,$p) $script:n++; return $true }.GetNewClosure(), [IntPtr]::Zero)
    "--- $tag : window=($($wr.left),$($wr.top)) $($wr.right-$wr.left)x$($wr.bottom-$wr.top)  client=$($cr.right)x$($cr.bottom)  dpi=$dpi  controls=$script:n"
}

Show-State "移动前"
[void][DP]::SetWindowPos($h, [IntPtr]::Zero, $X, $Y, 0, 0, 0x0001 -bor 0x0004 -bor 0x0010)
Start-Sleep -Milliseconds 1500
Show-State "移动后"

# 逐个子窗列出相对客户区的位置，找越界 / 错位
$cx = 0; $cy = 0
$cr = New-Object DP+RECT
[void][DP]::GetClientRect($h, [ref]$cr)
$cx = $cr.right; $cy = $cr.bottom
$rows = New-Object System.Collections.ArrayList
$cb = {
    param($c, $p)
    $sb = New-Object System.Text.StringBuilder 256
    [void][DP]::GetClassNameW($c, $sb, 256)
    $wbuf = New-Object System.Text.StringBuilder 128
    [void][DP]::GetWindowTextW($c, $wbuf, 128)
    $r = New-Object DP+RECT
    [void][DP]::GetWindowRect($c, [ref]$r)
    $pt = New-Object DP+POINT
    $pt.x = $r.left; $pt.y = $r.top
    [void][DP]::ScreenToClient($h, [ref]$pt)
    $id = [DP]::GetDlgCtrlID($c)
    [void]$rows.Add([pscustomobject]@{
        id = $id; cls = $sb.ToString(); x = $pt.x; y = $pt.y;
        w = $r.right - $r.left; h = $r.bottom - $r.top;
        txt = ($wbuf.ToString() -replace "`r`n", " ")
        over = (($pt.x + ($r.right - $r.left)) -gt ($cx + 1)) -or (($pt.y + ($r.bottom - $r.top)) -gt ($cy + 1))
    })
    return $true
}
[void][DP]::EnumChildWindows($h, $cb, [IntPtr]::Zero)
$rows | Sort-Object y, x | Format-Table -AutoSize | Out-String -Width 200
"越界控件数 = " + ($rows | Where-Object { $_.over }).Count
$rows | Where-Object { $_.over } | Format-Table -AutoSize | Out-String -Width 200

# 截图
$wr = New-Object DP+RECT
[void][DP]::GetWindowRect($h, [ref]$wr)
$w = $wr.right - $wr.left; $hh = $wr.bottom - $wr.top
$screen = [DP]::GetDC([IntPtr]::Zero)
$mem = [DP]::CreateCompatibleDC($screen)
$bmp = [DP]::CreateCompatibleBitmap($screen, $w, $hh)
[void][DP]::SelectObject($mem, $bmp)
[void][DP]::PrintWindow($h, $mem, 2)
[void][DP]::DeleteObject($bmp)
[void][DP]::DeleteDC($mem)
[void][DP]::ReleaseDC([IntPtr]::Zero, $screen)
"（PrintWindow 结果无法直接存盘，改用 tools\capture.ps1 另行抓图）"
