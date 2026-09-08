[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Version = '0.1.0',
    [switch]$SkipBuild,
    [ValidateRange(1,32)]
    [int]$MaxParallel = 1
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) { & (Join-Path $PSScriptRoot 'Build.ps1') -Configuration Release -MaxParallel $MaxParallel }

# Fail rather than mix an earlier release with this one. No files are deleted.
$name = "KOTOR2VR-$Version"
$stageContainer = Join-Path $root "dist\$name"
$stage = Join-Path $stageContainer 'KOTOR2VR'
$sourceStage = Join-Path $root "dist\$name-source"
$zip = "$stageContainer-win64.zip"
$sourceZip = "$sourceStage.zip"
foreach ($path in @($stageContainer, $sourceStage, $zip, $sourceZip)) {
    if (Test-Path -LiteralPath $path) { throw "Output exists: $path. Use a new -Version or move the previous output." }
}
$publish = Join-Path $root 'build\launcher\Release\win-x86'
$required = @{
    'kotor2vr-game32.dll' = 'build\x86\bin\Release\kotor2vr-game32.dll'
    'kotor2vr-host64.exe' = 'build\x64\bin\Release\kotor2vr-host64.exe'
    'kotor2vr-camera.ini' = 'kotor2vr-camera.ini'
    'kotor2vr.toml' = 'tools\release-config.toml'
    'Start-VR.cmd' = 'tools\Start-VR.cmd'
    'Start-VR.ps1' = 'tools\Start-VR.ps1'
    'README.md' = 'README.md'
    'LICENSE' = 'LICENSE'
    'THIRD_PARTY_NOTICES.md' = 'THIRD_PARTY_NOTICES.md'
}
foreach ($relative in $required.Values) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relative) -PathType Leaf)) { throw "Required release input missing: $relative" }
}
if (-not (Test-Path -LiteralPath (Join-Path $publish 'kotor2vr-launcher.exe'))) { throw 'Self-contained win-x86 launcher publish is missing. Run tools\Build.ps1.' }
& (Join-Path $PSScriptRoot 'Test-BinaryLayout.ps1') -Configuration Release -RepositoryRoot $root

New-Item -ItemType Directory -Path $stage, $sourceStage -Force | Out-Null
Get-ChildItem -LiteralPath $publish -Force | Copy-Item -Destination $stage -Recurse -Force
foreach ($entry in $required.GetEnumerator()) { Copy-Item -LiteralPath (Join-Path $root $entry.Value) -Destination (Join-Path $stage $entry.Key) }
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination (Join-Path $stage 'docs') -Recurse
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'licenses') -Destination (Join-Path $licenses 'supplemental') -Recurse
Copy-Item -LiteralPath (Join-Path $root 'third_party\KotorPatchManager\LICENSE') -Destination (Join-Path $licenses 'KotorPatchManager-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'third_party\OpenXR-SDK\LICENSE') -Destination (Join-Path $licenses 'OpenXR-SDK-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $root 'third_party\OpenXR-SDK\COPYING.adoc') -Destination $licenses
Copy-Item -LiteralPath (Join-Path $root 'third_party\OpenXR-SDK\LICENSES') -Destination (Join-Path $licenses 'OpenXR-SDK') -Recurse

Get-ChildItem -LiteralPath (Join-Path $root 'third_party\OpenXR-SDK\src\external') -Filter LICENSE -Recurse -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $licenses ((Split-Path -Leaf $_.DirectoryName) + '-LICENSE.txt'))
}

# Preserve the exact license declarations and bundled license/notice text of
# every restored package (including transitives and self-contained runtime).
$assets = Get-Content -LiteralPath (Join-Path $root 'src\launcher\obj\project.assets.json') -Raw | ConvertFrom-Json
$packageRoots = @($assets.packageFolders.PSObject.Properties.Name)
$inventory = @('NuGet dependencies included in the launcher publish:', '')
$packageEntries = @($assets.libraries.PSObject.Properties)
foreach ($framework in $assets.project.frameworks.PSObject.Properties) {
    foreach ($download in $framework.Value.downloadDependencies) {
        if ($download.version -notmatch '^\[([^,\]\s]+)(?:,\s*\1)?\]$') {
            throw "Runtime pack version is not pinned: $($download.name) $($download.version)"
        }
        $packageVersion = $Matches[1]
        $id = "$($download.name)/$packageVersion"
        if ($packageEntries.Name -contains $id) { continue }
        $packageEntries += [pscustomobject]@{ Name = $id; Value = [pscustomobject]@{ type = 'package'; path = $id.ToLowerInvariant() } }
    }
}
foreach ($library in $packageEntries) {
    if ($library.Value.type -ne 'package') { continue }
    $package = $null
    foreach ($packageRoot in $packageRoots) {
        $candidate = Join-Path $packageRoot $library.Value.path
        if (Test-Path -LiteralPath $candidate -PathType Container) { $package = $candidate; break }
    }
    if (-not $package) { throw "Restored package missing for license collection: $($library.Name)" }
    $destination = Join-Path $licenses ('NuGet\' + $library.Name.Replace('/', '-'))
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $nuspec = Get-ChildItem -LiteralPath $package -Filter '*.nuspec' | Select-Object -First 1
    if (-not $nuspec) { throw "Package metadata missing: $($library.Name)" }
    Copy-Item -LiteralPath $nuspec.FullName -Destination $destination
    [xml]$metadata = Get-Content -LiteralPath $nuspec.FullName -Raw
    $licenseNode = $metadata.SelectSingleNode("//*[local-name()='metadata']/*[local-name()='license']")
    $licenseUrl = $metadata.SelectSingleNode("//*[local-name()='metadata']/*[local-name()='licenseUrl']")
    $inventory += "$($library.Name): $($licenseNode.InnerText) $($licenseUrl.InnerText)"
    $noticeFiles = @(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object { $_.Name -match '(?i)^(license|licence|copying|notice|third.?party)' })
    if ($licenseNode -and $licenseNode.type -eq 'file') { $noticeFiles += Get-Item -LiteralPath (Join-Path $package $licenseNode.InnerText) }
    foreach ($notice in ($noticeFiles | Sort-Object FullName -Unique)) {
        $relative = $notice.FullName.Substring($package.Length).TrimStart('\', '/')
        $noticeTarget = Join-Path $destination $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $noticeTarget) -Force | Out-Null
        Copy-Item -LiteralPath $notice.FullName -Destination $noticeTarget
    }
}
$inventory | Set-Content -LiteralPath (Join-Path $licenses 'NuGet-dependencies.txt') -Encoding UTF8

# Ship a buildable snapshot, including vendored source, but no build products,
# local git metadata, logs, machine paths, or experimental proprietary feeder.
$sourceFolders = @('src', 'tests', 'shaders', 'tools', 'docs', 'licenses', 'third_party', '.github')
$excludedSegments = '(^|[\\/])(bin|obj|build|dist|\.git|\.vs|DLSS5-Feeder)([\\/]|$)'
$sourceFiles = @(Get-ChildItem -LiteralPath $root -File | Where-Object { $_.Name -match '^(CMakeLists\.txt|CMakePresets\.json|NuGet\.Config|LICENSE|README\.md|BUILD(?:ING)?\.md|THIRD_PARTY.*|Start-VR\.(cmd|ps1)|release-config\.toml|kotor2vr-camera\.ini|\.gitignore)$' })
foreach ($folder in $sourceFolders) {
    $path = Join-Path $root $folder
    if (Test-Path -LiteralPath $path) { $sourceFiles += Get-ChildItem -LiteralPath $path -File -Recurse -Force }
}
foreach ($file in $sourceFiles) {
    $relative = $file.FullName.Substring($root.Length).TrimStart('\', '/')
    if ($relative -match $excludedSegments -or $file.Extension -match '^\.(dll|exe|pdb|obj|lib|exp)$') { continue }
    if ($relative -match '^third_party[\\/]KotorPatchManager[\\/]' -and
        $relative -notmatch '^third_party[\\/]KotorPatchManager[\\/](src[\\/]KPatchCore[\\/]|AddressDatabases[\\/]|LICENSE$)') { continue }
    $target = Join-Path $sourceStage $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $target
}
foreach ($tree in @($stage, $sourceStage)) {
    $manifest = Get-ChildItem -LiteralPath $tree -Recurse -File | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($tree.Length).TrimStart('\', '/').Replace('\', '/')
        "$( (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant())  $relative"
    }
    $manifest | Set-Content -LiteralPath (Join-Path $tree 'SHA256SUMS.txt') -Encoding UTF8
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stageContainer, $zip, [System.IO.Compression.CompressionLevel]::Optimal, $false)
[System.IO.Compression.ZipFile]::CreateFromDirectory($sourceStage, $sourceZip, [System.IO.Compression.CompressionLevel]::Optimal, $true)
Get-FileHash -LiteralPath $zip, $sourceZip -Algorithm SHA256 | Format-Table -AutoSize
Write-Host "Portable release: $zip"
Write-Host "Buildable sources: $sourceZip"





