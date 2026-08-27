<#
.SYNOPSIS
    Packages a validated Release build of AmethystTool into a distributable zip, release notes,
    and a local annotated git tag.

.DESCRIPTION
    Takes a build already produced by build.ps1 (or produces one itself) and assembles everything
    needed to hand out a release: the three DLLs, the example config (renamed to the file the tool
    actually reads), the docs a user needs before injecting this into Steam, an INSTALL.txt, a
    RELEASE_NOTES.md generated from the commit log since the previous tag, and a local `v<Version>`
    annotated tag.

    This script never commits, never pushes, and never talks to GitHub. It only prepares files
    under release/ and creates a LOCAL git tag. Re-running it for the same -Version overwrites the
    package directory and zip (idempotent); it will refuse to move or delete an existing tag that
    already points somewhere else.

.PARAMETER Version
    Release version, e.g. "1.1.0". Used for the tag name (v<Version>), package folder, and zip name.

.PARAMETER SkipBuild
    Skip invoking build.ps1. Requires build\Release\AmethystTool.dll to already exist.

.PARAMETER SkipTests
    Only meaningful together with -SkipBuild (build.ps1 itself always runs both test suites).
    When -SkipBuild is passed and -SkipTests is NOT passed, this script independently re-runs
    `ctest --preset test-release` / `test-debug` against the existing build before packaging.
    When -SkipBuild is passed and -SkipTests IS passed, packaging proceeds without any test
    verification - use only when you already know the existing build is good.

.EXAMPLE
    .\release.ps1 -Version 1.2.0
    Full pipeline: clean build, test, then package.

.EXAMPLE
    .\release.ps1 -Version 1.2.0 -SkipBuild -SkipTests
    Package whatever is already sitting in build\Release, no rebuild, no re-test.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [switch]$SkipBuild,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'

$RepoRoot   = $PSScriptRoot
$BuildDir   = Join-Path $RepoRoot 'build'
$ReleaseDir = Join-Path $RepoRoot 'release'

function Write-Step  ([string]$Message) { Write-Host "`n== $Message ==" -ForegroundColor Cyan }
function Write-Ok    ([string]$Message) { Write-Host "[OK]   $Message" -ForegroundColor Green }
function Write-Warn2 ([string]$Message) { Write-Host "[WARN] $Message" -ForegroundColor Yellow }
function Fail([string]$Message) {
    Write-Host "`n[ERROR] $Message" -ForegroundColor Red
    exit 1
}

if ($Version -notmatch '^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$') {
    Fail "Version '$Version' does not look like a semantic version (expected e.g. 1.2.0 or 1.2.0-rc1)."
}
$tagName = "v$Version"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail 'git is not on PATH. release.ps1 needs git for release notes and local tagging.'
}

# ---------------------------------------------------------------------------
# 1. Build (or verify an existing build)
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Step 'Building (build.ps1 -Clean)'
    if ($SkipTests) {
        Write-Warn2 '-SkipTests has no effect here: build.ps1 always runs both test suites. Pass -SkipBuild too to actually skip testing.'
    }
    $buildScript = Join-Path $RepoRoot 'build.ps1'
    & $buildScript -Clean
    if ($LASTEXITCODE -ne 0) {
        Fail "build.ps1 failed (exit $LASTEXITCODE). Fix the build before packaging a release."
    }
    Write-Ok 'Build + tests passed.'
} else {
    Write-Step 'Verifying existing build (-SkipBuild)'

    $releaseDll = Join-Path $BuildDir 'Release\AmethystTool.dll'
    if (-not (Test-Path -LiteralPath $releaseDll)) {
        Fail "-SkipBuild was passed but $releaseDll does not exist. Run without -SkipBuild first."
    }

    $newestSource = Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src') -Recurse -Include '*.cpp', '*.h', '*.hpp' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($newestSource -and $newestSource.LastWriteTimeUtc -gt (Get-Item -LiteralPath $releaseDll).LastWriteTimeUtc) {
        Write-Warn2 "$releaseDll is older than $($newestSource.FullName.Substring($RepoRoot.Length + 1)) - it may not reflect the latest source."
    }
    Write-Ok "Found existing build output: $releaseDll"

    if ($SkipTests) {
        Write-Warn2 'Skipping test verification (-SkipBuild -SkipTests). Packaging an unverified build.'
    } else {
        Write-Step 'Re-running tests against the existing build (ctest)'

        # Minimal environment bootstrap - just enough for ctest.exe to run the already-built
        # test binaries. Full toolchain discovery/compile-environment loading lives in build.ps1.
        function Find-VcVars64Minimal {
            if ($env:BST_VCVARS64 -and (Test-Path -LiteralPath $env:BST_VCVARS64)) { return $env:BST_VCVARS64 }
            foreach ($edition in @('BuildTools', 'Community', 'Professional', 'Enterprise')) {
                foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
                    $c = "$pf\Microsoft Visual Studio\2022\$edition\VC\Auxiliary\Build\vcvars64.bat"
                    if (Test-Path -LiteralPath $c) { return $c }
                }
            }
            return $null
        }

        $vcvars64 = Find-VcVars64Minimal
        if (-not $vcvars64) {
            Fail 'Could not find vcvars64.bat to run ctest. Install VS 2022 Build Tools, or pass -SkipTests.'
        }
        $marker = '__BST_VCVARS_ENV_START__'
        $lines = & cmd.exe /c "call `"$vcvars64`" >nul 2>&1 && echo $marker && set"
        $started = $false
        foreach ($line in $lines) {
            if (-not $started) { if ($line.TrimEnd() -eq $marker) { $started = $true }; continue }
            $idx = $line.IndexOf('=')
            if ($idx -lt 1) { continue }
            $name = $line.Substring(0, $idx)
            if ($name -eq 'PROMPT') { continue }
            Set-Item -Path "Env:$name" -Value $line.Substring($idx + 1)
        }
        if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
            $vsInstallRoot = $vcvars64 -replace [regex]::Escape('\VC\Auxiliary\Build\vcvars64.bat'), ''
            $bundledCmakeBin = "$vsInstallRoot\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
            if (Test-Path -LiteralPath "$bundledCmakeBin\ctest.exe") { $env:Path = "$bundledCmakeBin;$env:Path" }
        }
        if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
            Fail 'ctest.exe not found even after loading vcvars64. Install CMake, or pass -SkipTests.'
        }

        Push-Location (Join-Path $RepoRoot 'src')
        try {
            foreach ($preset in @('test-release', 'test-debug')) {
                & ctest --preset $preset
                if ($LASTEXITCODE -ne 0) { Fail "Tests failed for preset '$preset' (exit $LASTEXITCODE)." }
            }
        } finally {
            Pop-Location
        }
        Write-Ok 'Existing build passed both test suites.'
    }
}

# ---------------------------------------------------------------------------
# 2. Collect artifacts
# ---------------------------------------------------------------------------
Write-Step 'Collecting release artifacts'

$artifacts = @(
    @{ Source = Join-Path $BuildDir 'Release\AmethystTool.dll'; Dest = 'AmethystTool.dll' }
    @{ Source = Join-Path $BuildDir 'Release\dwmapi.dll';       Dest = 'dwmapi.dll' }
    @{ Source = Join-Path $BuildDir 'Release\xinput1_4.dll';    Dest = 'xinput1_4.dll' }
    @{ Source = Join-Path $RepoRoot 'amethysttool.example.toml'; Dest = 'amethysttool.toml' }
    @{ Source = Join-Path $RepoRoot 'README.md';                 Dest = 'README.md' }
    @{ Source = Join-Path $RepoRoot 'TESTING.md';                Dest = 'TESTING.md' }
    @{ Source = Join-Path $RepoRoot 'AV_WHITELIST.md';           Dest = 'AV_WHITELIST.md' }
)

$missing = $artifacts | Where-Object { -not (Test-Path -LiteralPath $_.Source) }
if ($missing.Count -gt 0) {
    Fail "Missing expected artifact(s):`n$(($missing | ForEach-Object { '  - ' + $_.Source }) -join "`n")"
}
Write-Ok "All $($artifacts.Count) artifacts present."

# ---------------------------------------------------------------------------
# 3. Assemble the package directory (idempotent: wipe and recreate our own
#    generated output directory so a re-run never mixes stale + fresh files).
# ---------------------------------------------------------------------------
$packageName = "AmethystTool-v$Version"
$packageDir  = Join-Path $ReleaseDir $packageName
$zipPath     = Join-Path $ReleaseDir "$packageName.zip"

Write-Step "Assembling package: $packageName"

New-Item -ItemType Directory -Path $ReleaseDir -Force | Out-Null
if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Path $packageDir -Force | Out-Null

foreach ($artifact in $artifacts) {
    Copy-Item -LiteralPath $artifact.Source -Destination (Join-Path $packageDir $artifact.Dest) -Force
}

$installTxtSource = Join-Path $RepoRoot 'INSTALL.txt'
if (Test-Path -LiteralPath $installTxtSource) {
    Copy-Item -LiteralPath $installTxtSource -Destination (Join-Path $packageDir 'INSTALL.txt') -Force
} else {
    $installTxt = @"
AmethystTool v$Version - Installation
======================================

1. Copy AmethystTool.dll, dwmapi.dll, and xinput1_4.dll into your Steam
   installation directory (the folder that contains steam.exe).

2. Copy amethysttool.toml into the same directory and edit it as needed.
   Built-in defaults are used if you skip this step - no file is
   auto-created for you.

3. Restart Steam.

Before doing this on an account you care about, read TESTING.md - it walks
through testing in an isolated VM with a disposable account first. This
tool injects a DLL into the Steam client and hooks its internals; that is
inherently risky for your account and is exactly the kind of behavior
anti-cheat/anti-fraud systems look for.

If your antivirus quarantines these DLLs, see AV_WHITELIST.md - this is a
known, expected false positive for injection/hooking code, not a sign the
files are unsafe.

Full documentation: README.md
"@
    Set-Content -LiteralPath (Join-Path $packageDir 'INSTALL.txt') -Value $installTxt -Encoding utf8
}
Write-Ok "Package directory ready: $packageDir"

# ---------------------------------------------------------------------------
# 4. Zip it
# ---------------------------------------------------------------------------
Write-Step 'Creating zip archive'
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $packageDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
$zipSizeMb = [math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 2)
$zipHash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash
Write-Ok "Zip created: $zipPath ($zipSizeMb MB)"

# ---------------------------------------------------------------------------
# 5. Release notes from git log since the previous tag
# ---------------------------------------------------------------------------
Write-Step 'Generating RELEASE_NOTES.md'

Push-Location $RepoRoot
try {
    $previousTag = git tag -l --sort=-creatordate |
        Where-Object { $_ -ne $tagName } | Select-Object -First 1

    if ($previousTag) {
        $range = "$previousTag..HEAD"
        $commitLines = git log --pretty=format:'- %s (%h)' $range
    } else {
        $range = 'the beginning of history'
        $commitLines = git log --pretty=format:'- %s (%h)'
    }
    $currentCommit = git rev-parse --short HEAD
} finally {
    Pop-Location
}

$commitList = if ($commitLines) { ($commitLines -join "`n") } else { '(no commits in this range)' }
$verificationNote = if ($SkipBuild -and $SkipTests) {
    'Packaged from an existing build; tests were NOT re-verified by this run.'
} elseif ($SkipBuild) {
    'Packaged from an existing build; test suites were re-verified by this run.'
} else {
    'Built and tested from a clean rebuild by this run (build.ps1 -Clean).'
}

$releaseNotes = @"
# AmethystTool v$Version

Generated: $(Get-Date -Format 'yyyy-MM-dd HH:mm K')
Commit: $currentCommit
Changes since ${previousTag}:
$(if (-not $previousTag) { '(no previous tag found - showing full history)' })

$commitList

## Artifacts

- ``$packageName.zip`` ($zipSizeMb MB)
  SHA256: ``$zipHash``

## Verification

$verificationNote

## Install

See INSTALL.txt inside the package, or README.md / TESTING.md in this repository.
"@

$releaseNotesPath = Join-Path $ReleaseDir "RELEASE_NOTES-v$Version.md"
Set-Content -LiteralPath $releaseNotesPath -Value $releaseNotes -Encoding utf8
Write-Ok "Release notes: $releaseNotesPath"

# ---------------------------------------------------------------------------
# 6. Local annotated tag (never moved, never pushed)
# ---------------------------------------------------------------------------
Write-Step "Creating local tag $tagName"

Push-Location $RepoRoot
try {
    # `^{commit}` dereferences an annotated tag to the commit it points at - without it,
    # rev-parse returns the tag OBJECT's own hash, which never equals a commit hash and
    # would make this check always report a "moved" tag.
    $existingTagCommit = git rev-parse -q --verify "refs/tags/$tagName^{commit}" 2>$null
    $headCommit = git rev-parse HEAD

    if ($LASTEXITCODE -eq 0 -and $existingTagCommit) {
        if ($existingTagCommit -eq $headCommit) {
            Write-Ok "Tag $tagName already exists and points at HEAD ($($headCommit.Substring(0,7))) - nothing to do."
        } else {
            Fail (
                "Tag $tagName already exists but points at $($existingTagCommit.Substring(0,7)), not the " +
                "current HEAD ($($headCommit.Substring(0,7))). Not moving an existing tag automatically - " +
                "delete it yourself first if that's intended (git tag -d $tagName), or use a different -Version."
            )
        }
    } else {
        git tag -a $tagName -m "Release $tagName"
        if ($LASTEXITCODE -ne 0) { Fail "git tag failed (exit $LASTEXITCODE)." }
        Write-Ok "Created local annotated tag $tagName at $($headCommit.Substring(0,7))."
    }
} finally {
    Pop-Location
}

# ---------------------------------------------------------------------------
# 7. Summary + manual next steps (nothing below this line is executed)
# ---------------------------------------------------------------------------
Write-Host "`n================ RELEASE SUMMARY ================" -ForegroundColor Cyan
Write-Host ("Version         : {0}" -f $Version)
Write-Host ("Package dir     : {0}" -f $packageDir)
Write-Host ("Zip             : {0} ({1} MB)" -f $zipPath, $zipSizeMb)
Write-Host ("Zip SHA256      : {0}" -f $zipHash)
Write-Host ("Release notes   : {0}" -f $releaseNotesPath)
Write-Host ("Local tag       : {0}" -f $tagName)
Write-Host "===================================================" -ForegroundColor Cyan
Write-Host "`nNothing was pushed or published. To finish the release yourself:" -ForegroundColor Yellow
Write-Host "  git push origin $tagName"
Write-Host "  gh release create $tagName `"$zipPath`" -F `"$releaseNotesPath`" -t `"AmethystTool $tagName`""
Write-Host "`n[OK] Release package ready." -ForegroundColor Green
exit 0
