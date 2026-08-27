<#
.SYNOPSIS
    Builds AmethystTool (Release + Debug), runs the test suites, and verifies DLL ABI/CRT integrity.

.DESCRIPTION
    Automates the pipeline that has so far been run by hand: locate the VS 2022 C++ toolchain
    (BuildTools/Community/Professional/Enterprise) and its bundled cmake/ninja, load the vcvars64
    compiler environment into this process, configure the `ninja-multi` CMake preset from src/,
    build and test both Release and Debug, then verify the exported DLL's ABI (TokeerUri @
    ordinal 1) and CRT linkage (no vcruntime140/msvcp140/ucrtbase dependency, per the /MT decision
    in src/CMakePresets.json).

    This script only runs existing build tooling - it never edits C++ source, never commits, and
    never touches git. Safe to re-run: without -Clean it is a normal incremental CMake/Ninja build.

.PARAMETER Clean
    Before configuring, delete the per-target object directories for this project's own targets
    (AmethystTool, OpenSteamTool, OSTPlatform, dwmapi, xinput1_4, AmethystToolTests) for both
    Debug and Release. Third-party FetchContent dependencies (lua_static, detours, googletest,
    spdlog, protobuf, tomlplusplus, and the .deps/ source cache) are left untouched, so they are
    not rebuilt from source on every clean build. This mirrors the manual "authoritative warning
    count" clean documented for this repo.

.EXAMPLE
    .\build.ps1
    Incremental build + test + integrity check.

.EXAMPLE
    .\build.ps1 -Clean
    Clean rebuild of this project's own targets, then build + test + integrity check.

.NOTES
    Set $env:BST_VCVARS64 to override auto-detection of vcvars64.bat (useful for a nonstandard
    Visual Studio install location).
#>
[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

$RepoRoot = $PSScriptRoot
$SrcDir   = Join-Path $RepoRoot 'src'
$BuildDir = Join-Path $RepoRoot 'build'
$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

function Write-Step  ([string]$Message) { Write-Host "`n== $Message ==" -ForegroundColor Cyan }
function Write-Ok    ([string]$Message) { Write-Host "[OK]   $Message" -ForegroundColor Green }
function Write-Warn2 ([string]$Message) { Write-Host "[WARN] $Message" -ForegroundColor Yellow }
function Fail([string]$Message) {
    Write-Host "`n[ERROR] $Message" -ForegroundColor Red
    exit 1
}

# ---------------------------------------------------------------------------
# 1. Locate the VS 2022 C++ toolchain (vcvars64.bat)
# ---------------------------------------------------------------------------
Write-Step 'Locating Visual Studio 2022 C++ toolchain'

function Find-VcVars64 {
    if ($env:BST_VCVARS64 -and (Test-Path -LiteralPath $env:BST_VCVARS64)) {
        return (Resolve-Path -LiteralPath $env:BST_VCVARS64).Path
    }

    $candidates = New-Object System.Collections.Generic.List[string]

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installPaths = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        foreach ($p in $installPaths) {
            if ($p) { $candidates.Add((Join-Path $p 'VC\Auxiliary\Build\vcvars64.bat')) }
        }
    }

    foreach ($edition in @('BuildTools', 'Community', 'Professional', 'Enterprise')) {
        $candidates.Add("${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat")
        $candidates.Add("${env:ProgramFiles}\Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat")
    }

    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    return $null
}

$vcvars64 = Find-VcVars64
if (-not $vcvars64) {
    Fail (
        "Could not find vcvars64.bat (Visual Studio 2022 C++ toolchain).`n" +
        "Install 'Desktop development with C++' via Visual Studio 2022 Build Tools:`n" +
        "  https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022`n" +
        "Or set `$env:BST_VCVARS64 to the full path of vcvars64.bat and re-run."
    )
}
Write-Ok "vcvars64.bat: $vcvars64"
$vsInstallRoot = $vcvars64 -replace [regex]::Escape('\VC\Auxiliary\Build\vcvars64.bat'), ''

# ---------------------------------------------------------------------------
# 2. Load the compiler environment (cl.exe, link.exe, dumpbin.exe, ...) into
#    THIS PowerShell process by parsing `vcvars64.bat && set`.
# ---------------------------------------------------------------------------
Write-Step 'Loading MSVC compiler environment'

function Import-VcVarsEnvironment([string]$VcVarsPath) {
    $marker = '__BST_VCVARS_ENV_START__'
    $lines = & cmd.exe /c "call `"$VcVarsPath`" >nul 2>&1 && echo $marker && set"
    if ($LASTEXITCODE -ne 0) {
        Fail "vcvars64.bat failed to initialize (exit $LASTEXITCODE). The toolchain install may be damaged."
    }

    $started = $false
    foreach ($line in $lines) {
        if (-not $started) {
            # cmd's `echo` includes the trailing space before `&&`, so compare trimmed.
            if ($line.TrimEnd() -eq $marker) { $started = $true }
            continue
        }
        $idx = $line.IndexOf('=')
        if ($idx -lt 1) { continue }
        $name = $line.Substring(0, $idx)
        $value = $line.Substring($idx + 1)
        if ($name -eq 'PROMPT') { continue }
        Set-Item -Path "Env:$name" -Value $value
    }
}

Import-VcVarsEnvironment $vcvars64

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Fail 'vcvars64.bat ran but cl.exe is still not resolvable. The compiler environment did not load correctly.'
}
Write-Ok "MSVC compiler environment loaded ($((Get-Command cl.exe).Source))"

# ---------------------------------------------------------------------------
# 3. Locate cmake.exe / ninja.exe (vcvars64 on this toolchain also puts the
#    VS-bundled CMake component on PATH, but don't assume that everywhere).
# ---------------------------------------------------------------------------
Write-Step 'Locating cmake and ninja'

function Find-ToolExe([string]$ExeName, [string]$VsInstallRoot) {
    $onPath = Get-Command $ExeName -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $candidates = @(
        "$VsInstallRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\$ExeName",
        "$VsInstallRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\$ExeName",
        "${env:ProgramFiles}\CMake\bin\$ExeName",
        "${env:ProgramFiles(x86)}\CMake\bin\$ExeName",
        "${env:ProgramData}\chocolatey\bin\$ExeName"
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }

    # Last resort: bounded recursive search under the VS install root.
    $found = Get-ChildItem -LiteralPath $VsInstallRoot -Recurse -Filter $ExeName -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($found) { return $found.FullName }

    return $null
}

$cmakeExe = Find-ToolExe -ExeName 'cmake.exe' -VsInstallRoot $vsInstallRoot
if (-not $cmakeExe) {
    Fail (
        "cmake.exe not found on PATH, under the VS install, or under the standard CMake install " +
        "locations. Install CMake 3.25+ (https://cmake.org/download/) or the 'C++ CMake tools for " +
        "Windows' VS component."
    )
}
$ninjaExe = Find-ToolExe -ExeName 'ninja.exe' -VsInstallRoot $vsInstallRoot
if (-not $ninjaExe) {
    Fail (
        "ninja.exe not found on PATH, under the VS install, or under the standard install locations. " +
        "Install the 'C++ CMake tools for Windows' VS component (bundles Ninja), or Ninja itself " +
        "(https://github.com/ninja-build/ninja/releases)."
    )
}

foreach ($exe in @($cmakeExe, $ninjaExe)) {
    $dir = Split-Path -Parent $exe
    if (($env:Path -split ';') -notcontains $dir) {
        $env:Path = "$dir;$env:Path"
    }
}

$cmakeVersion = (& $cmakeExe --version | Select-Object -First 1)
$ninjaVersion = (& $ninjaExe --version)
Write-Ok "cmake: $cmakeExe ($cmakeVersion)"
Write-Ok "ninja: $ninjaExe (v$ninjaVersion)"

# ---------------------------------------------------------------------------
# 4. Optional clean: only this project's own target object directories.
# ---------------------------------------------------------------------------
if ($Clean) {
    Write-Step 'Cleaning own-target object directories (-Clean)'
    $ownTargets = @('AmethystTool', 'OpenSteamTool', 'OSTPlatform', 'dwmapi', 'xinput1_4', 'AmethystToolTests')
    $removed = 0

    if (Test-Path -LiteralPath $BuildDir) {
        $targetDirs = Get-ChildItem -LiteralPath $BuildDir -Recurse -Directory -Filter '*.dir' -ErrorAction SilentlyContinue |
            Where-Object { $ownTargets -contains ($_.Name -replace '\.dir$', '') }

        foreach ($dir in $targetDirs) {
            foreach ($config in @('Debug', 'Release')) {
                $configDir = Join-Path $dir.FullName $config
                if (Test-Path -LiteralPath $configDir) {
                    Remove-Item -LiteralPath $configDir -Recurse -Force
                    $removed++
                }
            }
        }
    }
    Write-Ok "Removed $removed own-target object director$(if ($removed -eq 1) {'y'} else {'ies'}) (third-party deps and .deps/ preserved)."
}

# ---------------------------------------------------------------------------
# 5. Configure
# ---------------------------------------------------------------------------
Write-Step 'Configuring (ninja-multi preset)'
Push-Location $SrcDir
try {
    & cmake --preset ninja-multi
    if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed (exit $LASTEXITCODE)." }
} finally {
    Pop-Location
}
Write-Ok 'Configure succeeded.'

# ---------------------------------------------------------------------------
# 6. Build Release + Debug
# ---------------------------------------------------------------------------
$warningCounts = @{}

foreach ($preset in @('release', 'debug')) {
    Write-Step "Building ($preset)"
    Push-Location $SrcDir
    try {
        $output = & cmake --build --preset $preset 2>&1 | ForEach-Object { Write-Host $_; $_ }
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne 0) {
            Fail "Build failed for preset '$preset' (exit $exitCode). See output above."
        }
        $warnings = $output | Select-String -Pattern ': warning C\d+:'
        $warningCounts[$preset] = $warnings.Count
    } finally {
        Pop-Location
    }
    Write-Ok "Build succeeded ($preset) - $($warningCounts[$preset]) compiler warning(s)."
}

# ---------------------------------------------------------------------------
# 7. Test Release + Debug
# ---------------------------------------------------------------------------
$testResults = @{}

foreach ($preset in @('test-release', 'test-debug')) {
    Write-Step "Testing ($preset)"
    Push-Location $SrcDir
    try {
        & ctest --preset $preset
        $exitCode = $LASTEXITCODE
        $testResults[$preset] = $exitCode
        if ($exitCode -ne 0) {
            Fail "Tests failed for preset '$preset' (exit $exitCode). See output above."
        }
    } finally {
        Pop-Location
    }
    Write-Ok "Tests passed ($preset)."
}

# ---------------------------------------------------------------------------
# 8. Integrity checks: ABI export + static-CRT linkage (Release DLL)
# ---------------------------------------------------------------------------
Write-Step 'Verifying ABI and CRT linkage (Release/AmethystTool.dll)'

$releaseDll = Join-Path $BuildDir 'Release\AmethystTool.dll'
if (-not (Test-Path -LiteralPath $releaseDll)) {
    Fail "Expected build output missing: $releaseDll"
}

$dumpbinCmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbinCmd) {
    Fail 'dumpbin.exe not found even after loading vcvars64 - the compiler environment is incomplete.'
}
$dumpbin = $dumpbinCmd.Source

$exportsOutput = & $dumpbin /exports $releaseDll
$tokeerLine = $exportsOutput | Select-String -Pattern '\bTokeerUri\b'
if (-not $tokeerLine) {
    Fail "TokeerUri export not found in AmethystTool.dll. ABI is broken."
}
if ($tokeerLine.Line -notmatch '^\s*1\s+0\s+[0-9A-Fa-f]+\s+TokeerUri\s*$') {
    Fail "TokeerUri is exported but not at ordinal 1 (found: '$($tokeerLine.Line.Trim())'). This breaks the fixed ABI contract."
}
Write-Ok 'TokeerUri confirmed at ordinal 1 (the only export, per the fixed ABI contract).'

$dependentsOutput = & $dumpbin /dependents $releaseDll
$forbiddenCrtDlls = @('vcruntime140.dll', 'msvcp140.dll', 'ucrtbase.dll')
$foundForbidden = $forbiddenCrtDlls | Where-Object { $dependentsOutput -match [regex]::Escape($_) }
if ($foundForbidden.Count -gt 0) {
    Fail "AmethystTool.dll depends on dynamic CRT DLL(s): $($foundForbidden -join ', '). Expected static /MT linkage (see src/CMakePresets.json)."
}
Write-Ok 'No dynamic CRT dependency (vcruntime140/msvcp140/ucrtbase) - static /MT linkage confirmed.'

# ---------------------------------------------------------------------------
# 9. Summary
# ---------------------------------------------------------------------------
$stopwatch.Stop()
$elapsed = '{0:mm\:ss}' -f $stopwatch.Elapsed

Write-Host "`n================ BUILD SUMMARY ================" -ForegroundColor Cyan
Write-Host ("Elapsed        : {0}" -f $elapsed)
Write-Host ("Clean rebuild  : {0}" -f $Clean.IsPresent)
Write-Host ("Warnings       : Release={0}  Debug={1}" -f $warningCounts['release'], $warningCounts['debug'])
Write-Host ("Tests          : test-release=PASS  test-debug=PASS")
Write-Host ("ABI check      : TokeerUri @ ordinal 1 -> PASS")
Write-Host ("CRT check      : static /MT (no vcruntime/msvcp/ucrtbase) -> PASS")
Write-Host ("Release output : $releaseDll")
Write-Host ("Debug output   : {0}" -f (Join-Path $BuildDir 'Debug\AmethystTool.dll'))
Write-Host "=================================================" -ForegroundColor Cyan
Write-Host "`n[OK] Build completed successfully." -ForegroundColor Green
exit 0
