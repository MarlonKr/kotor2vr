[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$RepositoryRoot = ''
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
    $RepositoryRoot = Split-Path -Parent $PSScriptRoot
}

function Find-DumpBin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'dumpbin.exe is unavailable and vswhere.exe was not found.'
    }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ Build Tools were not found.' }
    $candidate = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Tools\MSVC') -Directory |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\dumpbin.exe' } |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        Select-Object -First 1
    if (-not $candidate) { throw 'dumpbin.exe was not found in Visual Studio.' }
    return $candidate
}

function Invoke-DumpBin([string]$DumpBin, [string]$Mode, [string]$Path) {
    $output = & $DumpBin $Mode $Path 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for '$Path'."
    }
    return ($output -join "`n")
}

$root = [System.IO.Path]::GetFullPath($RepositoryRoot)
$hostBinary = Join-Path $root "build\x64\bin\$Configuration\kotor2vr-host64.exe"
$gameBinary = Join-Path $root "build\x86\bin\$Configuration\kotor2vr-game32.dll"
foreach ($path in @($hostBinary, $gameBinary)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required binary is missing: $path"
    }
}

$dumpbin = Find-DumpBin
$hostHeaders = Invoke-DumpBin $dumpbin '/HEADERS' $hostBinary
$gameHeaders = Invoke-DumpBin $dumpbin '/HEADERS' $gameBinary
$hostDeps = Invoke-DumpBin $dumpbin '/DEPENDENTS' $hostBinary
$gameDeps = Invoke-DumpBin $dumpbin '/DEPENDENTS' $gameBinary
$gameExports = Invoke-DumpBin $dumpbin '/EXPORTS' $gameBinary

if ($hostHeaders -notmatch '(?im)^\s+8664 machine \(x64\)') {
    throw 'Host binary is not x64.'
}
if ($gameHeaders -notmatch '(?im)^\s+14C machine \(x86\)') {
    throw 'Game module is not x86.'
}
if ($hostDeps -match '(?im)openxr_loaderd?\.dll') {
    throw 'Host has an app-local OpenXR loader DLL dependency; the vendored loader must be static.'
}
if (($hostDeps + $gameDeps) -match '(?im)\b(?:msvcp\d+[^\s]*|vcruntime\d+[^\s]*)\.dll') {
    throw 'Native binaries require an external MSVC runtime; the portable build must use the static CRT.'
}
foreach ($name in @('K2VR_ProbeBootstrap', 'K2VR_ProbeInitialize', 'K2VR_RunReadOnlyProbe', 'K2VR_ProbeShutdown', 'K2VR_ProtocolVersion', 'K2VR_RenderTraceBootstrap', 'K2VR_RenderDoublePassBootstrap', 'K2VR_RenderTraceStop', 'K2VR_VrBridgeBootstrap', 'K2VR_VrBridgeStop')) {
    if ($gameExports -notmatch "(?m)\b$([Regex]::Escape($name))\b") {
        throw "Game module export is missing: $name"
    }
}

Write-Host 'Binary layout PASS: x64 host, static OpenXR loader, x86 game module, explicit probe/render-trace/double-pass/VR-bridge exports.'
