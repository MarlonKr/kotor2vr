[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [switch]$SkipLauncher,
    [ValidateRange(1,32)]
    [int]$MaxParallel = 1
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Find-CMake {
    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    $candidate = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installation) {
            $candidate = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate) { return $candidate }
        }
    }
    throw 'CMake 3.24+ was not found. Install it or the Visual Studio CMake component.'
}

$cmake = Find-CMake
$savedEnvironment = @{}
foreach ($key in @('TEMP', 'TMP', 'DOTNET_CLI_HOME', 'NUGET_PACKAGES')) {
    $savedEnvironment[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
    $directory = Join-Path $root "build\environment\$key"
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    [Environment]::SetEnvironmentVariable($key, $directory, 'Process')
}
Push-Location $root
try {
    foreach ($architecture in @('x64', 'x86')) {
        $configurePreset = "windows-$architecture"
        $buildPreset = "windows-$architecture-$($Configuration.ToLowerInvariant())"
        if ($Configuration -ne 'Release') {
            & $cmake --preset $configurePreset
            if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $configurePreset" }
            & $cmake --build "build/$architecture" --config $Configuration --parallel $MaxParallel
        } else {
            & $cmake --preset $configurePreset
            if ($LASTEXITCODE -ne 0) { throw "CMake configure failed: $configurePreset" }
            & $cmake --build --preset $buildPreset --parallel $MaxParallel
        }
        if ($LASTEXITCODE -ne 0) { throw "Native build failed: $architecture/$Configuration" }
        & (Join-Path (Split-Path $cmake) 'ctest.exe') --test-dir "build/$architecture" -C $Configuration --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "Native tests failed: $architecture/$Configuration" }
    }

    $binaryLayoutTest = Join-Path $PSScriptRoot 'Test-BinaryLayout.ps1'
    & $binaryLayoutTest -Configuration $Configuration -RepositoryRoot $root

    if (-not $SkipLauncher) {
        $publish = Join-Path $root "build\launcher\$Configuration\win-x86"
        dotnet publish src/launcher/Kotor2Vr.Launcher.csproj -c $Configuration -r win-x86 --self-contained true -o $publish -p:PublishSingleFile=false -p:PublishTrimmed=false "-p:RestoreConfigFile=$root\NuGet.Config" -m:1
        if ($LASTEXITCODE -ne 0) { throw 'Launcher publish failed.' }
        & (Join-Path $publish 'kotor2vr-launcher.exe') self-test
        if ($LASTEXITCODE -ne 0) { throw 'Launcher self-tests failed.' }
    }
} finally {
    Pop-Location
    foreach ($key in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key, $savedEnvironment[$key], 'Process') }
}

Write-Host "KOTOR2VR $Configuration build and tests PASS."


