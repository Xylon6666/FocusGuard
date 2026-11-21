# 编译专注守卫（需要 Visual Studio C++ 工具链 + Windows SDK）
# 用法： & .\build.ps1

$ErrorActionPreference = "Stop"
$root     = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir   = Join-Path $root "src"
$buildDir = Join-Path $root "build"
$distDir  = Join-Path $root "dist"

# ---------- 1) 定位工具链 ----------
$vcvars = $null
$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
if (Test-Path -LiteralPath $vswhere) {
    $inst = & $vswhere -latest -products * -property installationPath
    if ($inst) {
        $cand = Join-Path $inst "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path -LiteralPath $cand) { $vcvars = $cand }
    }
}
if (-not $vcvars) {
    foreach ($p in @(
        "D:\Developer\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat")) {
        if (Test-Path -LiteralPath $p) { $vcvars = $p; break }
    }
}
if (-not $vcvars) { throw "找不到 vcvarsall.bat，请安装 Visual Studio 的『使用 C++ 的桌面开发』工作负载。" }

$vsRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $vcvars)))

# 最新的可用 MSVC 工具集（要求含 vcruntime.h）
$msvcDir = Get-ChildItem -LiteralPath (Join-Path $vsRoot "VC\Tools\MSVC") -Directory |
           Sort-Object Name -Descending |
           Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "include\vcruntime.h") } |
           Select-Object -First 1
if (-not $msvcDir) { throw "找不到可用的 MSVC 工具集。" }
$msvc = $msvcDir.FullName

# Windows SDK（优先 10，回退 8.1）
$sdkRoot = "C:\Program Files (x86)\Windows Kits\10"
$sdkVer = Get-ChildItem -LiteralPath (Join-Path $sdkRoot "Include") -Directory |
          Sort-Object Name -Descending |
          Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "um\windows.h") } |
          Select-Object -First 1
if (-not $sdkVer) { throw "找不到 Windows SDK 头文件。" }
$sdkVerName = $sdkVer.Name

$sdkBin = Get-ChildItem -LiteralPath (Join-Path $sdkRoot "bin") -Directory |
          Sort-Object Name -Descending |
          ForEach-Object { Join-Path $_.FullName "x64" } |
          Where-Object { Test-Path -LiteralPath (Join-Path $_ "rc.exe") } |
          Select-Object -First 1
if (-not $sdkBin) { throw "找不到 Windows SDK 的 rc.exe。" }

# ---------- 2) 目录与图标 ----------
foreach ($d in @($buildDir, $distDir)) {
    if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
}
if (-not (Test-Path -LiteralPath (Join-Path $srcDir "app.ico"))) {
    python (Join-Path $root "tools\make_icon.py")
}

# ---------- 3) 生成批处理并编译 ----------
$sources = @("main.cpp", "config.cpp", "audio.cpp", "popup.cpp", "edge.cpp", "settings.cpp") |
           ForEach-Object { Join-Path $srcDir $_ }

$inc = @(
    (Join-Path $msvc "include"),
    (Join-Path $sdkRoot "Include\$sdkVerName\ucrt"),
    (Join-Path $sdkRoot "Include\$sdkVerName\shared"),
    (Join-Path $sdkRoot "Include\$sdkVerName\um"),
    (Join-Path $sdkRoot "Include\$sdkVerName\winrt")
) -join ';'

$lib = @(
    (Join-Path $msvc "lib\x64"),
    (Join-Path $sdkRoot "Lib\$sdkVerName\ucrt\x64"),
    (Join-Path $sdkRoot "Lib\$sdkVerName\um\x64")
) -join ';'

$bat = @"
call "$vcvars" x64 >nul
set "PATH=%PATH%;$sdkBin"
set "INCLUDE=$inc"
set "LIB=$lib"
cd /d "$root"
rc /nologo /c 65001 /fo "$buildDir\app.res" "$srcDir\app.rc"
if errorlevel 1 exit /b 2
cd /d "$buildDir"
cl /nologo /O2 /utf-8 /W3 /EHsc /MD /DNDEBUG /GS- /Fe"$distDir\FocusGuard.exe" $($sources -join ' ') "$buildDir\app.res" /link /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO
if errorlevel 1 exit /b 3
exit /b 0
"@

$batPath = Join-Path $buildDir "build.bat"
[IO.File]::WriteAllText($batPath, ($bat -replace "`r?`n", "`r`n"), (New-Object Text.UTF8Encoding $true))

$cmdExe = Join-Path $env:SystemRoot "System32\cmd.exe"
& $cmdExe /c $batPath 2>&1 | ForEach-Object { $_ }
$code = $LASTEXITCODE

if ($code -ne 0) { throw "编译失败，退出码 $code" }

$exe = Join-Path $distDir "FocusGuard.exe"
$size = (Get-Item -LiteralPath $exe).Length
Write-Host ("Built: {0}  ({1:N1} KB)" -f $exe, ($size / 1KB)) -ForegroundColor Green
