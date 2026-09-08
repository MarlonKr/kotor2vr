# V1 candidate validation — 2026-09-07

The isolated release source was built using Visual Studio 2022 Build Tools (MSVC 14.44.35207), Windows SDK 10.0.26100.0, CMake 3.31.6-msvc6 and .NET SDK 8.0.424. The launcher publish resolved the .NET 8.0.30 runtime. Exact restored dependency versions are included in the package license inventory.

- x64 native build: passed; 10 CTest cases passed.
- x86 native build: passed; 10 CTest cases passed.
- Published self-contained win-x86 launcher: passed; 23 self-test groups passed.
- PE checks: x64 host, x86 game module, required bootstrap exports; no dynamic OpenXR-loader import or MSVC runtime import.
- Binary ZIP extracted into a separate validation directory: all SHA-256 entries matched; expected portable files present.
- Source ZIP: generated binary output and DLSS5-Feeder excluded.
- Packaged `Start-VR.ps1 -DryRun` against the registered supported Steam executable: passed. No host, game, injection or TCS override started.

The portable Steam registration tests cover manifest parsing, path mismatch/ambiguity, traversal rejection and process-only environment reset. The launcher uses the registered Steam path; this does not validate launching an arbitrary copied game folder.

The native tests exercise contracts and decision logic. No hardware rendering test or new headset session was started for this release preparation. Passing offline tests does not replace human clean-install and headset testing.

The inherited KPatchCore source emits two nullable-list warnings and one warning for an intentionally disabled debug block. The launcher and native build checks pass. These warnings do not establish or rule out runtime issues.
