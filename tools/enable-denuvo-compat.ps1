<#
.SYNOPSIS
    Creates (or removes) the OpenSteamTool.dll / opensteamtool.toml compatibility
    aliases that external Denuvo activation tooling (LuaTools Validator) looks for.

.DESCRIPTION
    Fallback for users who prefer not to edit amethysttool.toml. Does the same
    thing as "[compatibility] denuvo_aliases = true", once, without restarting
    Steam:

      AmethystTool.dll   -> OpenSteamTool.dll    (byte copy)
      amethysttool.toml  -> opensteamtool.toml   (copy with a 3-line marker header)

    Safety rules (identical to the in-DLL feature, see CompatAliasLogic.h):
      * An existing OpenSteamTool.dll / opensteamtool.toml that was NOT created
        by AmethystTool (e.g. a real upstream OpenSteamTool install, or a config
        you renamed by hand) is never overwritten or deleted.
      * Nothing in AmethystTool reads the aliases; they only exist so external
        tools recognise the activation engine. Privacy and runtime behaviour of
        AmethystTool are unchanged.

.PARAMETER SteamPath
    Steam install folder (the one containing steam.exe). Defaults to the value of
    HKCU:\Software\Valve\Steam\SteamPath.

.PARAMETER Remove
    Delete the aliases instead of creating them (only files AmethystTool created).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\enable-denuvo-compat.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\enable-denuvo-compat.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$SteamPath,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Must stay byte-identical to CompatAliasLogic::kTomlAliasHeader (LF line endings),
# so the DLL recognises (and refreshes) an alias written by this script and vice versa.
$TomlAliasHeader = "# AmethystTool compatibility alias -- generated from amethysttool.toml.`n" +
                   "# Only read by external tools (e.g. LuaTools Validator); AmethystTool`n" +
                   "# itself always reads amethysttool.toml. Safe to delete.`n"

# Present in every AmethystTool.dll build, never in the upstream OpenSteamTool.dll
# (CompatAliasLogic::kAmethystDllMarker).
$DllMarker = 'amethysttool.toml'

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
# ISO-8859-1 maps every byte 1:1 to a char, so a byte-level substring search is exact.
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Write-Ok   ($msg) { Write-Host "[ OK ] $msg" -ForegroundColor Green }
function Write-Info ($msg) { Write-Host "[INFO] $msg" -ForegroundColor Cyan }
function Write-Warn ($msg) { Write-Host "[WARN] $msg" -ForegroundColor Yellow }
function Fail       ($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red; exit 1 }

function Resolve-SteamPath {
    if ($SteamPath) { return $SteamPath }
    try {
        $p = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name 'SteamPath' -ErrorAction Stop).SteamPath
    } catch {
        Fail 'Steam path not found in HKCU:\Software\Valve\Steam (SteamPath). Pass -SteamPath "C:\Path\To\Steam".'
    }
    # Steam stores it with forward slashes (e.g. c:/program files (x86)/steam).
    return ($p -replace '/', '\')
}

function Test-OurDll([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    return $Latin1.GetString($bytes).Contains($DllMarker)
}

function Test-OurToml([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $header = $Utf8NoBom.GetBytes($TomlAliasHeader)
    if ($bytes.Length -lt $header.Length) { return $false }
    for ($i = 0; $i -lt $header.Length; $i++) {
        if ($bytes[$i] -ne $header[$i]) { return $false }
    }
    return $true
}

function Test-SameBytes([byte[]]$a, [byte[]]$b) {
    if ($a.Length -ne $b.Length) { return $false }
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { return $false } }
    return $true
}

# Write to a temp file next to the alias, then move it into place, so a failure
# never leaves a truncated alias behind.
function Set-AliasBytes([string]$aliasPath, [byte[]]$bytes) {
    $tmp = "$aliasPath.amethyst-tmp"
    try {
        [System.IO.File]::WriteAllBytes($tmp, $bytes)
        Move-Item -LiteralPath $tmp -Destination $aliasPath -Force
    } finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    }
}

$steam = Resolve-SteamPath
if (-not (Test-Path -LiteralPath (Join-Path $steam 'steam.exe'))) {
    Fail "steam.exe not found in '$steam'. Pass -SteamPath with your Steam install folder."
}
Write-Info "Steam folder: $steam"

$srcDll   = Join-Path $steam 'AmethystTool.dll'
$aliasDll = Join-Path $steam 'OpenSteamTool.dll'
$srcToml  = Join-Path $steam 'amethysttool.toml'
$aliasToml = Join-Path $steam 'opensteamtool.toml'

# ---------------------------------------------------------------------------
# -Remove
# ---------------------------------------------------------------------------
if ($Remove) {
    foreach ($entry in @(
        @{ Path = $aliasDll;  IsOurs = { param($p) Test-OurDll $p } },
        @{ Path = $aliasToml; IsOurs = { param($p) Test-OurToml $p } }
    )) {
        $p = $entry.Path
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) {
            Write-Info "$(Split-Path -Leaf $p) not present; nothing to remove."
            continue
        }
        if (& $entry.IsOurs $p) {
            try {
                Remove-Item -LiteralPath $p -Force
                Write-Ok "Removed $(Split-Path -Leaf $p)."
            } catch {
                Write-Warn "Could not remove $(Split-Path -Leaf $p): $($_.Exception.Message) (close Steam and retry)."
            }
        } else {
            Write-Warn "$(Split-Path -Leaf $p) was not created by AmethystTool; left untouched."
        }
    }
    Write-Host ''
    Write-Host 'If amethysttool.toml has [compatibility] denuvo_aliases = true, set it to false,'
    Write-Host 'otherwise the aliases are recreated the next time Steam starts.'
    exit 0
}

# ---------------------------------------------------------------------------
# Create / refresh
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $srcDll -PathType Leaf)) {
    Fail "AmethystTool.dll not found in '$steam'. Install AmethystTool first."
}

$problems = 0

# DLL alias
$srcDllBytes = [System.IO.File]::ReadAllBytes($srcDll)
if (Test-Path -LiteralPath $aliasDll) {
    if (-not (Test-Path -LiteralPath $aliasDll -PathType Leaf) -or -not (Test-OurDll $aliasDll)) {
        Write-Warn 'OpenSteamTool.dll already exists and is NOT an AmethystTool copy (real OpenSteamTool install?). Left untouched.'
        $problems++
    } elseif (Test-SameBytes ([System.IO.File]::ReadAllBytes($aliasDll)) $srcDllBytes) {
        Write-Ok 'OpenSteamTool.dll alias already up to date.'
    } else {
        try { Set-AliasBytes $aliasDll $srcDllBytes; Write-Ok 'OpenSteamTool.dll alias refreshed.' }
        catch { Write-Warn "Could not refresh OpenSteamTool.dll: $($_.Exception.Message)"; $problems++ }
    }
} else {
    try { Set-AliasBytes $aliasDll $srcDllBytes; Write-Ok 'OpenSteamTool.dll alias created.' }
    catch { Write-Warn "Could not create OpenSteamTool.dll: $($_.Exception.Message)"; $problems++ }
}

# TOML alias
if (Test-Path -LiteralPath $srcToml -PathType Leaf) {
    $srcTomlBytes = [System.IO.File]::ReadAllBytes($srcToml)
    $headerBytes  = $Utf8NoBom.GetBytes($TomlAliasHeader)
    $aliasBytes   = New-Object byte[] ($headerBytes.Length + $srcTomlBytes.Length)
    [System.Array]::Copy($headerBytes, 0, $aliasBytes, 0, $headerBytes.Length)
    [System.Array]::Copy($srcTomlBytes, 0, $aliasBytes, $headerBytes.Length, $srcTomlBytes.Length)
} else {
    # No amethysttool.toml (built-in defaults in use): the alias is just the header,
    # which is still a valid (empty) TOML document.
    Write-Info 'amethysttool.toml not found; creating a minimal opensteamtool.toml.'
    $aliasBytes = $Utf8NoBom.GetBytes($TomlAliasHeader)
}

if (Test-Path -LiteralPath $aliasToml) {
    if (-not (Test-Path -LiteralPath $aliasToml -PathType Leaf) -or -not (Test-OurToml $aliasToml)) {
        # A user-owned opensteamtool.toml (e.g. from upstream, or a manual rename)
        # already satisfies the validator's file check; never overwrite it.
        Write-Info 'opensteamtool.toml already exists (not created by AmethystTool). Left untouched.'
    } elseif (Test-SameBytes ([System.IO.File]::ReadAllBytes($aliasToml)) $aliasBytes) {
        Write-Ok 'opensteamtool.toml alias already up to date.'
    } else {
        try { Set-AliasBytes $aliasToml $aliasBytes; Write-Ok 'opensteamtool.toml alias refreshed.' }
        catch { Write-Warn "Could not refresh opensteamtool.toml: $($_.Exception.Message)"; $problems++ }
    }
} else {
    try { Set-AliasBytes $aliasToml $aliasBytes; Write-Ok 'opensteamtool.toml alias created.' }
    catch { Write-Warn "Could not create opensteamtool.toml: $($_.Exception.Message)"; $problems++ }
}

Write-Host ''
if ($problems -eq 0) {
    Write-Host 'You can now run the LuaTools Validator.' -ForegroundColor Green
} else {
    Write-Host "Finished with $problems warning(s) - see above." -ForegroundColor Yellow
}
Write-Host 'After generating the ticket you can delete the aliases (run this script with -Remove)'
Write-Host 'or turn them off with [compatibility] denuvo_aliases = false in amethysttool.toml.'
