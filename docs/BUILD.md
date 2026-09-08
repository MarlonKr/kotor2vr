# Build the 0.1.0 early preview

Use 64-bit Windows with Git (for repository checkouts), Visual Studio 2022 C++ Build Tools including x86/x64 tools, a Windows SDK, CMake 3.25 or newer, and the .NET 8 SDK. The default presets use Visual Studio 2022. Network access to NuGet is needed for the initial launcher restore.

Download the source archive from the same release as the binary ZIP, or check out the corresponding source tag. Open PowerShell in the extracted repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Build.ps1 -Configuration Release -MaxParallel 1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Package.ps1 -SkipBuild -Version 0.1.0
```

The first command builds x64 host and x86 game module, runs offline CTest suites, verifies their PE architecture/exports and static OpenXR loader, then publishes a self-contained x86 launcher and runs launcher self-tests. It does not launch KOTOR or create a VR session. The second creates binary and source ZIPs under `dist` and includes SHA-256 file manifests. Existing version output directories are not overwritten; use a new candidate version for a revised build.

The binary ZIP contains a single `KOTOR2VR` folder. Follow the root README to use it. Building from source requires developer tools; playing the resulting ZIP does not require a separate .NET installation or C++ development tools.

Kotor Patch Manager and OpenXR source are vendored, including local injection changes. Do not replace them with current upstream branches. The early preview CMake build excludes the optional NGX/DLSS direct implementation and uses an explicit unavailable backend; no NVIDIA SDK is needed.

This is a documented self-build path, not a claim of byte-identical reproducibility. Compiler/SDK versions, build paths, timestamps and restored runtime versions can affect bytes. Match the release source and inspect the dependency manifest when comparing builds.

## Offline package validation

From inside the extracted `KOTOR2VR` folder:

```powershell
.\kotor2vr-launcher.exe self-test
powershell -NoProfile -File .\Start-VR.ps1 -DryRun -NoBrowse -GameExecutable 'D:\SteamLibrary\steamapps\common\Knights of the Old Republic II\swkotor2.exe'
```

Replace the example game path with your supported Steam installation. Dry-run validates the package and executable without starting the host, Steam game or DLL injection. It does not establish headset compatibility or gameplay stability.
