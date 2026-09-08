[CmdletBinding()]
param(
    [string]$GameExecutable,
    [switch]$DryRun,
    [switch]$NoBrowse
)
$ErrorActionPreference = 'Stop'
try {
    # In the ZIP this script lives beside the launcher in the KOTOR2VR folder.
    $packageRoot = $PSScriptRoot
    $launcher = Join-Path $packageRoot 'kotor2vr-launcher.exe'
    $config = Join-Path $packageRoot 'kotor2vr.toml'
    foreach ($required in @($launcher, $config,
            (Join-Path $packageRoot 'kotor2vr-host64.exe'),
            (Join-Path $packageRoot 'kotor2vr-game32.dll'))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Package file missing: $required. Extract the complete ZIP before starting VR."
        }
    }
    if (-not $GameExecutable) {
        $candidate = Join-Path (Split-Path -Parent $packageRoot) 'swkotor2.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $GameExecutable = $candidate }
    }
    if (-not $GameExecutable -and -not $NoBrowse -and -not $DryRun) {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        try {
            $dialog.Title = 'Select swkotor2.exe in your registered Steam KOTOR II installation'
            $dialog.Filter = 'KOTOR II (swkotor2.exe)|swkotor2.exe'
            $dialog.CheckFileExists = $true
            if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
                $GameExecutable = $dialog.FileName
            }
        } finally { $dialog.Dispose() }
    }
    if (-not $GameExecutable) { throw 'No game selected. Extract KOTOR2VR into the Steam game folder or supply -GameExecutable.' }
    $gamePath = (Resolve-Path -LiteralPath $GameExecutable).Path
    if ([IO.Path]::GetFileName($gamePath) -ine 'swkotor2.exe') { throw 'Select swkotor2.exe.' }
    $env:KOTOR2VR_GAME_EXECUTABLE = $gamePath
    $env:KOTOR2VR_NEURAL_ENABLED = '0'
    Remove-Item Env:KOTOR2VR_NEURAL_WORKERS -ErrorAction SilentlyContinue
    Write-Host 'KOTOR2VR V1 - native stereo'
    Write-Host "Game: $gamePath"
    Write-Host 'Steam launches its registered installation. A separate copied folder is not supported.'
    Write-Host 'Connect your headset and activate its OpenXR runtime before launching.'
    $launcherArgs = @('portable-stereo', '--config', $config)
    if ($DryRun) { $launcherArgs += '--dry-run' }
    & $launcher @launcherArgs
    exit $LASTEXITCODE
} catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
