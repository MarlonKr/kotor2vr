# Supplemental dependency licenses

These are unmodified upstream license and notice files for dependencies whose restored NuGet packages contain a license expression/link but no complete license text. `tools/Package.ps1` includes this directory in both release archives (under `licenses/supplemental` in the portable archive). Its automatic NuGet collection separately includes the original package metadata and licenses already supplied in packages.

Retrieved 2026-09-07 from the official repositories listed by the restored `.nuspec` metadata. `provenance.json` records each exact commit-pinned URL and SHA-256 of the downloaded bytes.

| Package(s) | License | Source revision |
| --- | --- | --- |
| LibObjectFile 2.2.0 | BSD-2-Clause | xoofx/LibObjectFile `e864861b132279b1c0d0103f7956104f40358c00`, from package metadata |
| Tomlyn 0.19.0 | BSD-2-Clause | xoofx/Tomlyn `ee3e3ca5b1f016b0db21ceb74d6c85d28a08ca16`, from package metadata |
| MinVer 7.0.0 | Apache-2.0 | adamralph/minver `288e752d82a772660e740178ba11c8adba5e217a`, from package metadata |
| Microsoft.Data.Sqlite and Microsoft.Data.Sqlite.Core 8.0.0 | MIT | dotnet/efcore `e017dc125bef2f604f85befd8ff27544a5a67c38`, from package metadata |
| SQLitePCLRaw.core, SQLitePCLRaw.bundle_e_sqlite3, SQLitePCLRaw.provider.e_sqlite3, SQLitePCLRaw.lib.e_sqlite3 2.1.6 | Apache-2.0; see upstream NOTICE for bundled SQLite | ericsink/SQLitePCL.raw release tag `v2.1.6`, resolved through the GitHub tag API to commit `9c66b4f618d9d6831e83c16a8036daea83df4ddb` (packages omit a commit) |

The complete SQLitePCLRaw upstream NOTICE is preserved, including its SQLite public-domain dedication and notices for other upstream build variants. Its presence does not mean every variant mentioned is shipped in KOTOR2VR. MinVer is a development dependency; its license is retained for source-build completeness.

No upstream license text has been edited. These supplemental copies add attribution and terms; they do not change each dependency's license.
