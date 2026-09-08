# Third-party components

The original KOTOR2VR source is covered by the root MIT LICENSE.

Vendored source dependencies are included in this repository so that local modifications are available to builders. Kotor Patch Manager is represented by the compiled KPatchCore source, required address data and license; its separate applications, patch collection and prebuilt native libraries are not part of this release:

- **Kotor Patch Manager**, Lane Dibello and contributors, MIT. Upstream base `32290d5879b4f543a3392670fb2151ef76b855a4`, https://github.com/LaneDibello/Kotor-Patch-Manager. Local changes in `src/KPatchCore/Launcher` implement the Steam-aware exact-process injection and bootstrap handshake. See `third_party/KotorPatchManager/LICENSE`.
- **OpenXR-SDK**, Khronos Group and contributors, Apache-2.0 and component-specific licenses. Base `f2448a8797c85814aa892efc1ab8707900fbcc78` (release-1.1.63), https://github.com/KhronosGroup/OpenXR-SDK. The loader is built from the vendored source and linked statically. Retain its LICENSE and accompanying notices, including licenses for bundled jsoncpp and other incorporated components.

The self-contained launcher also carries .NET and its restored NuGet dependencies. Their own licenses apply; package metadata records exact versions. Binary packages must retain runtime notices and dependency license texts.

No KOTOR executable/assets, NVIDIA runtime/models, DLSS5-Feeder binaries, ReShade or TCS-DLSSV installation is included.
