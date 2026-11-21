# 用 PrintWindow 抓取 FocusGuard 窗口的原生分辨率截图
param(
    [string]$Class = "FocusGuard.SettingsWindow",
    [string]$Out = ""
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace Cap -Name W -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder sb, int n);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
'@

[void][Cap.W]::SetThreadDpiAwarenessContext([IntPtr](-4))
$pidTarget = (Get-Process FocusGuard).Id
$hwnds = [System.Collections.Generic.List[System.IntPtr]]::new()
$cb = [Cap.W+EnumProc]{
    param($h, $l)
    $p = 0
    [void][Cap.W]::GetWindowThreadProcessId($h, [ref]$p)
    if ($p -eq $pidTarget) {
        $sb = New-Object System.Text.StringBuilder 128
        [void][Cap.W]::GetClassName($h, $sb, 128)
        if ($sb.ToString() -eq $Class) { $hwnds.Add($h) }
    }
    return $true
}
[void][Cap.W]::EnumWindows($cb, [IntPtr]::Zero)
if ($hwnds.Count -eq 0) { throw "no window of class $Class" }
$h = $hwnds[0]
$r = New-Object Cap.W+RECT
[void][Cap.W]::GetWindowRect($h, [ref]$r)
$w = $r.R - $r.L; $ht = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap($w, $ht)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Cap.W]::PrintWindow($h, $hdc, 2)
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"captured $Class ${w}x${ht} printwindow=$ok -> $Out"
