# CPU-only isolated build. Never starts the GPU branch or deploys binaries.
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$cmake='C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
function Invoke-CleanBuildProcess([string]$Program,[string[]]$Arguments) {
    $start=[Diagnostics.ProcessStartInfo]::new()
    $start.FileName=$Program
    $start.WorkingDirectory=$repo
    $start.UseShellExecute=$false
    $start.CreateNoWindow=$true
    $start.RedirectStandardOutput=$true
    $start.RedirectStandardError=$true
    # MSBuild's .NET Framework environment dictionary rejects duplicate
    # case variants inherited from the agent. Normalize only this child.
    $start.Environment.Clear()
    foreach($item in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $start.Environment[$item.Key.ToString().ToUpperInvariant()]=$item.Value.ToString()
    }
    foreach($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process=[Diagnostics.Process]::Start($start)
    $stdout=$process.StandardOutput.ReadToEndAsync()
    $stderr=$process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    Write-Output $stdout.GetAwaiter().GetResult()
    Write-Output $stderr.GetAwaiter().GetResult()
    if($process.ExitCode -ne 0) { throw "CPU build/check failed ($($process.ExitCode)): $Program" }
    $process.Dispose()
}
Invoke-CleanBuildProcess $cmake @('-S','.','-B','build/monitor-discard-x86','-G','Visual Studio 17 2022','-A','Win32','-DBUILD_TESTING=ON','-DKOTOR2VR_WARNINGS_AS_ERRORS=ON')
Invoke-CleanBuildProcess $cmake @('--build','build/monitor-discard-x86','--config','Release','--target','kotor2vr-game32','kotor2vr-scene-replay-live-smoke','--parallel','2')
Invoke-CleanBuildProcess (Join-Path $repo 'build/monitor-discard-x86/bin/Release/kotor2vr-scene-replay-live-smoke.exe') @('--cpu')
