using System.Buffers.Binary;
using System.Diagnostics;
using System.Text.Json;
using KPatchCore.Launcher;

namespace Kotor2Vr.Launcher;

internal static class SelfTest
{
    public static int Run()
    {
        var root = Path.Combine(
            Path.GetTempPath(),
            $"kotor2vr-launcher-test-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            TestToml(root);
            TestPortableConfig(root);
            TestPortableStartup(root);
            TestOverrideOwnershipAndRestore(root);
            TestMonitorDefaultOffAndRestore(root);
            TestMonitorRecoveryPreservesSettings(root);
            TestMonitorMsaaOptInAndRestore(root);
            TestMonitorMsaaMergeAndRollback(root);
            TestMonitorMsaaRecovery(root);
            TestMonitorMsaaRejectsAmbiguity(root);
            TestRecoveryRequiresDefinitelyDeadOwner(root);
            TestUncertainOwnerIsNotRecovered(root);
            TestConflictDoesNotOverwrite(root);
            TestCorruptBackupDoesNotOverwrite(root);
            TestDisposeNeverThrows(root);
            TestLegacyJournalIsSurfaced(root);
            TestGate1Qualification(root);
            TestSteamProcessIdentity(root);
            TestSteamPe32Parsing();
            TestSteamExportParsing();
            TestOpenedProcessHandleIdentity();
            TestVrBridgeBootstrapPayloadAndArguments();
            TestRemoteBootstrapArgumentDecisions();
            Console.WriteLine("Launcher self-tests passed (23 groups).");
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"Launcher self-test failed: {exception}");
            return 1;
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static void TestVrBridgeBootstrapPayloadAndArguments()
    {
        const string nonceText = "0123456789abcdefFEDCBA9876543210";
        var payload = VrBridgeBootstrapPayload.CreateFromNonceText(nonceText);
        Assert(payload.Length == 32, "VrBridge payload has packed size");
        Assert(
            payload.SequenceEqual(new byte[]
            {
                0x20, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
                0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
                0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01,
                0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
            }),
            "VrBridge payload bytes exact");
        Assert(
            BinaryPrimitives.ReadUInt32LittleEndian(payload.AsSpan(0, 4)) == 32,
            "VrBridge payload size field");
        Assert(
            BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(4, 2)) == 1 &&
            BinaryPrimitives.ReadUInt16LittleEndian(payload.AsSpan(6, 2)) == 0,
            "VrBridge payload v1.0 fields");
        Assert(
            BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(8, 8)) ==
                0xfedcba9876543210UL,
            "VrBridge nonce text tail stored as little-endian low");
        Assert(
            BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(16, 8)) ==
                0x0123456789abcdefUL,
            "VrBridge nonce text head stored as little-endian high");
        Assert(
            BinaryPrimitives.ReadUInt64LittleEndian(payload.AsSpan(24, 8)) == 1,
            "VrBridge generation is one");
        ExpectThrows<ArgumentException>(
            () => VrBridgeBootstrapPayload.CreateFromNonceText("1234"),
            "short VrBridge nonce rejected");
        ExpectThrows<ArgumentException>(
            () => VrBridgeBootstrapPayload.CreateFromNonceText(new string('0', 32)),
            "zero VrBridge nonce rejected");

        var arguments = Program.Arguments.Parse(
            ["bridge-smoke", "--confirm-vr-bridge", "--dry-run"]);
        Assert(arguments.Command == "bridge-smoke", "VrBridge command parsed");
        Assert(arguments.ConfirmVrBridge, "VrBridge confirmation parsed");
        Assert(arguments.DryRun, "VrBridge dry-run parsed");

        var imageArguments = Program.Arguments.Parse(
            ["game-image-smoke", "--confirm-game-image-smoke", "--dry-run"]);
        Assert(
            imageArguments.Command == "game-image-smoke",
            "game-image smoke command parsed");
        Assert(
            imageArguments.ConfirmGameImageSmoke,
            "game-image smoke confirmation parsed");
        Assert(imageArguments.DryRun, "game-image smoke dry-run parsed");
    }

    private static void TestRemoteBootstrapArgumentDecisions()
    {
        var payload = VrBridgeBootstrapPayload.CreateFromNonceText(
            "0123456789abcdefFEDCBA9876543210");
        var bridge = ProcessInjector.DecideRemoteBootstrapArgument(
            Kotor2VrBootstrapKind.VrBridge,
            payload);
        Assert(
            bridge.Valid && bridge.RequiresRemoteAllocation && bridge.ByteCount == 32,
            "VrBridge selects exact remote argument allocation");
        var gameImage = ProcessInjector.DecideRemoteBootstrapArgument(
            Kotor2VrBootstrapKind.GameImageSmoke,
            payload);
        Assert(
            gameImage.Valid && gameImage.RequiresRemoteAllocation &&
            gameImage.ByteCount == 32,
            "game-image smoke selects exact remote argument allocation");

        foreach (var kind in new[]
                 {
                     Kotor2VrBootstrapKind.Probe,
                     Kotor2VrBootstrapKind.RenderTrace,
                     Kotor2VrBootstrapKind.RenderDoublePass
                 })
        {
            var historical = ProcessInjector.DecideRemoteBootstrapArgument(kind, null);
            Assert(
                historical.Valid && !historical.RequiresRemoteAllocation &&
                historical.ByteCount == 0,
                $"{kind} keeps null remote argument");
            Assert(
                !ProcessInjector.DecideRemoteBootstrapArgument(kind, payload).Valid,
                $"{kind} rejects non-null remote argument");
        }

        Assert(
            !ProcessInjector.DecideRemoteBootstrapArgument(
                Kotor2VrBootstrapKind.VrBridge,
                null).Valid,
            "VrBridge rejects null remote argument");
        Assert(
            !ProcessInjector.DecideRemoteBootstrapArgument(
                Kotor2VrBootstrapKind.GameImageSmoke,
                null).Valid,
            "game-image smoke rejects null remote argument");
        var corrupt = (byte[])payload.Clone();
        corrupt[0] = 31;
        Assert(
            !ProcessInjector.DecideRemoteBootstrapArgument(
                Kotor2VrBootstrapKind.VrBridge,
                corrupt).Valid,
            "VrBridge rejects corrupt payload header");
        Assert(
            !ProcessInjector.DecideRemoteBootstrapArgument(
                Kotor2VrBootstrapKind.GameImageSmoke,
                corrupt).Valid,
            "game-image smoke rejects corrupt payload header");

        const int witnessPid = 4242;
        Assert(
            SteamProcessValidation.HasFreshGameImageSmokeBootstrapWitness(
                $"game-image-smoke-entry pid={witnessPid} tid=99\n" +
                $"game-image-smoke-complete pid={witnessPid} hooks=3 key=F7\n",
                witnessPid,
                out _),
            "game-image smoke fresh witness accepted");
        Assert(
            !SteamProcessValidation.HasFreshGameImageSmokeBootstrapWitness(
                $"game-image-smoke-entry pid={witnessPid + 1} tid=99\n" +
                $"game-image-smoke-complete pid={witnessPid + 1} hooks=3 key=F7\n",
                witnessPid,
                out _),
            "game-image smoke wrong-PID witness rejected");
    }

    private static void TestPortableStartup(string root)
    {
        var environment = Environment.GetEnvironmentVariables().Cast<System.Collections.DictionaryEntry>()
            .Where(x => ((string)x.Key).StartsWith("KOTOR2VR_", StringComparison.OrdinalIgnoreCase))
            .ToDictionary(x => (string)x.Key, x => (string?)x.Value);
        try
        {
            Environment.SetEnvironmentVariable("KOTOR2VR_GAME_EXECUTABLE", "selected-game");
            Environment.SetEnvironmentVariable("KOTOR2VR_COMPOSITOR_DEPTH", "1");
            Environment.SetEnvironmentVariable("KOTOR2VR_NEURAL_WORKERS", "old-workers");
            Environment.SetEnvironmentVariable("KOTOR2VR_FUTURE_EXPERIMENT", "1");
            Environment.SetEnvironmentVariable("KOTOR2VR_EYE_PERCENT", "50");
            PortableStartup.ResetRuntimeEnvironment();
            Assert(Environment.GetEnvironmentVariable("KOTOR2VR_GAME_EXECUTABLE") == "selected-game",
                "portable environment preserves selected game");
            Assert(Environment.GetEnvironmentVariable("KOTOR2VR_EYE_PERCENT") == "75" &&
                Environment.GetEnvironmentVariable("KOTOR2VR_NEURAL_ENABLED") == "0",
                "portable defaults are fixed");
            Assert(Environment.GetEnvironmentVariable("KOTOR2VR_COMPOSITOR_DEPTH") is null &&
                Environment.GetEnvironmentVariable("KOTOR2VR_NEURAL_WORKERS") is null &&
                Environment.GetEnvironmentVariable("KOTOR2VR_FUTURE_EXPERIMENT") is null,
                "portable environment removes all inherited experiments");
        }
        finally
        {
            PortableStartup.ResetRuntimeEnvironment();
            foreach (var name in new[] { "KOTOR2VR_GAME_EXECUTABLE", "KOTOR2VR_EYE_PERCENT", "KOTOR2VR_NEURAL_ENABLED" })
                Environment.SetEnvironmentVariable(name, null);
            foreach (var item in environment) Environment.SetEnvironmentVariable(item.Key, item.Value);
        }
        var libraries = PortableStartup.ParseLibraryPaths("""
            "libraryfolders" {
                "0" { "path" "C:\\Steam" "apps" { "208580" "42" } }
                "1" { "path" "F:\\Steam Library" }
            }
            """);
        Assert(libraries.SequenceEqual(new[] { @"C:\Steam", @"F:\Steam Library" }),
            "Steam libraries preserve escaped paths, spaces, and nested app blocks");
        var library = Path.Combine(root, "Steam Library");
        var manifest = "\"AppState\" { \"appid\" \"208580\" \"installdir\" \"Knights of the Old Republic II\" }";
        var selected = PortableStartup.ResolveManifestExecutable(library, manifest);
        Assert(selected == Path.Combine(library, "steamapps", "common", "Knights of the Old Republic II", "swkotor2.exe"),
            "Steam manifest resolves registered executable");
        PortableStartup.RequireSingleRegisteredPath(selected, new[] { selected });
        void Reject(Action action, string label)
        {
            var rejected = false;
            try { action(); } catch (Exception) { rejected = true; }
            Assert(rejected, label);
        }
        Reject(() => PortableStartup.RequireSingleRegisteredPath(Path.Combine(root, "copy", "swkotor2.exe"), new[] { selected }),
            "copied game path rejected before dispatch");
        Reject(() => PortableStartup.RequireSingleRegisteredPath(selected, Array.Empty<string>()), "unknown registration rejected");
        Reject(() => PortableStartup.RequireSingleRegisteredPath(selected, new[] { selected, Path.Combine(root, "other.exe") }),
            "ambiguous registration rejected");
        Reject(() => PortableStartup.ResolveManifestExecutable(library, manifest.Replace("208580", "123")), "wrong app rejected");
        Reject(() => PortableStartup.ResolveManifestExecutable(library, manifest.Replace("Knights of the Old Republic II", "../copy")),
            "manifest traversal rejected");
        Reject(() => PortableStartup.ParseLibraryPaths("\"libraryfolders\" {"), "truncated libraries rejected");
    }

    private static void TestPortableConfig(string root)
    {
        var directory = Path.Combine(root, "portable package with spaces");
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "kotor2vr.toml");
        File.WriteAllText(path, """
            [game]
            executable = "../swkotor2.exe"
            expected_sha256 = "ABCDEF"
            [paths]
            host_executable = "kotor2vr-host64.exe"
            game_module = "kotor2vr-game32.dll"
            """);
        var previous = Environment.GetEnvironmentVariable("KOTOR2VR_GAME_EXECUTABLE");
        try
        {
            Environment.SetEnvironmentVariable("KOTOR2VR_GAME_EXECUTABLE", null);
            var config = LauncherConfig.Load(path);
            Assert(config.GameExecutable == Path.Combine(root, "swkotor2.exe"),
                "portable game resolves relative to package rather than current directory");
            Assert(config.HostExecutable == Path.Combine(directory, "kotor2vr-host64.exe"),
                "portable host path preserves spaces");
            Assert(!config.DlssEnabled, "absent neural config defaults off");
            var selected = Path.Combine(root, "selected game", "swkotor2.exe");
            Environment.SetEnvironmentVariable("KOTOR2VR_GAME_EXECUTABLE", selected);
            Assert(LauncherConfig.Load(path).GameExecutable == selected,
                "browser selected game overrides relative default without rewriting config");
            var args = Program.Arguments.Parse(new[] { "portable-stereo", "--config", path, "--dry-run" });
            Assert(args.Command == "portable-stereo" && args.DryRun && args.ConfigPath == path,
                "portable dry run arguments preserve package path");
        }
        finally { Environment.SetEnvironmentVariable("KOTOR2VR_GAME_EXECUTABLE", previous); }
    }

    private static void TestToml(string root)
    {
        var path = Path.Combine(root, "sample.toml");
        File.WriteAllText(
            path,
            "[a]\nname = \"hello#world\" # comment\nflag = true\ncount = 72\n");
        var toml = TomlLite.Load(path);
        Assert(toml.RequireString("a.name") == "hello#world", "quoted # parsing");
        Assert(toml.GetString("a.missing", "fallback") == "fallback", "string fallback");
        Assert(toml.GetBoolean("a.flag", false), "boolean parsing");
        Assert(toml.GetInt32("a.count", 0) == 72, "integer parsing");
    }

    private static void TestOverrideOwnershipAndRestore(string root)
    {
        var state = Path.Combine(root, "state-ownership");
        var config = Path.Combine(root, "ownership.cfg");
        const string original = "mode=2\r\nenabled = 1 ; keep this\r\nhdr=1\r\n";
        File.WriteAllText(config, original);
        var lifetime = FakeProcessLifetime.Alive();

        using var transaction = new TcsOverrideTransaction(state, lifetime);
        transaction.BeginDisable(config);
        Assert(transaction.IsActive, "transaction active");
        Assert(transaction.SessionNonce != Guid.Empty, "session nonce assigned");
        Assert(
            File.ReadAllText(config).Contains(
                "enabled = 0 ; keep this",
                StringComparison.Ordinal),
            "disable setting");

        var journalPath = SingleFile(state, "*.journal.json");
        using (var document = JsonDocument.Parse(File.ReadAllBytes(journalPath)))
        {
            var journal = document.RootElement;
            Assert(journal.GetProperty("SchemaVersion").GetInt32() == 2, "journal schema");
            Assert(
                journal.GetProperty("SessionNonce").GetGuid() == transaction.SessionNonce,
                "journal nonce");
            Assert(
                journal.GetProperty("OwnerProcessId").GetInt32() == lifetime.Current.ProcessId,
                "journal owner PID");
            Assert(
                journal.GetProperty("OwnerStartTimeUtcTicks").GetInt64() ==
                lifetime.Current.StartTimeUtcTicks,
                "journal owner start time");
        }

        using (var competing = new TcsOverrideTransaction(state, lifetime))
        {
            ExpectThrows<InvalidOperationException>(
                () => competing.BeginDisable(config),
                "per-target ownership lock");
        }

        Assert(transaction.Restore(), "owned transaction restored");
        Assert(File.ReadAllText(config) == original, "byte-equivalent restore");
        Assert(!Directory.EnumerateFiles(state, "*.journal.json").Any(), "journal removed");
    }

    private static void TestMonitorDefaultOffAndRestore(string root)
    {
        var state = Path.Combine(root, "state-monitor");
        var config = Path.Combine(root, "monitor.ini");
        var preset = Path.Combine(root, "monitor-flat.ini");
        const string flat = "; keep original\r\nTechniques=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx\r\n" +
            "TechniqueSorting=Lumenite_Kernel@lumenite_Kernel.fx,DLSS5_Feed@DLSS5_Feed.fx\r\n" +
            "[DLSS5_Feed.fx]\r\nGain=1.5\r\n";
        var original = $"[GENERAL]\r\nPresetPath={preset}\r\nStartupPresetPath=\r\n" +
            "[INPUT]\r\nKeyEffects=117,0,0,0\r\n";
        File.WriteAllText(preset, flat);
        File.WriteAllText(config, original, new System.Text.UTF8Encoding(true));
        var originalBytes = File.ReadAllBytes(config);
        using var transaction = new TcsOverrideTransaction(state, FakeProcessLifetime.Alive());
        transaction.BeginMonitorDefaultOff(config);
        var active = File.ReadAllText(config);
        var sessionPreset = MonitorDlssDefaults.Get(active, "GENERAL", "StartupPresetPath")!;
        var sessionText = File.ReadAllText(sessionPreset);
        Assert(MonitorDlssDefaults.Get(sessionText, "", "Techniques") == "", "monitor starts off");
        foreach (var technique in new[] { "Lumenite_Kernel@lumenite_Kernel.fx", "DLSS5_Feed@DLSS5_Feed.fx" })
            Assert(MonitorDlssDefaults.Get(sessionText, "", "Key" + technique) == "117,0,0,0",
                "F6 toggles both guides and feed");
        Assert(MonitorDlssDefaults.Get(active, "INPUT", "KeyEffects") == "0,0,0,0",
            "global toggle cannot cancel the technique toggle");
        Assert(MonitorDlssDefaults.Get(sessionText, "DLSS5_Feed.fx", "Gain") == "1.5",
            "monitor quality settings preserved");
        Assert(File.ReadAllText(preset) == flat, "flat preset not modified");
        File.AppendAllText(sessionPreset, "; simulate ReShade autosave after F6\n");
        Assert(transaction.Restore(), "monitor transaction restored");
        Assert(File.ReadAllBytes(config).SequenceEqual(originalBytes), "monitor config restored including BOM");
        Assert(File.ReadAllText(preset) == flat, "session toggles cannot change flat preset");
    }

    private static void TestMonitorRecoveryPreservesSettings(string root)
    {
        var state = Path.Combine(root, "state-monitor-recovery");
        var config = Path.Combine(root, "monitor-recovery.ini");
        var preset = Path.Combine(root, "monitor-recovery-flat.ini");
        File.WriteAllText(preset, "Techniques=DLSS5_Feed@DLSS5_Feed.fx\n");
        var original = $"[GENERAL]\nPresetPath={preset}\n[INPUT]\nKeyEffects=117,0,0,0\n";
        File.WriteAllText(config, original);
        var lifetime = FakeProcessLifetime.Alive();
        var transaction = new TcsOverrideTransaction(state, lifetime);
        transaction.BeginMonitorDefaultOff(config);
        var active = File.ReadAllText(config);
        File.WriteAllText(config, active + "[OVERLAY]\nWindow=new layout\n");
        transaction.AbandonForTesting();
        var live = TcsOverrideTransaction.RecoverAbandonedTransactions(stateDirectory: state,
            processLifetime: lifetime);
        Assert(live.StillOwned == 1, "live monitor override not recovered");
        lifetime.Liveness = OwnerLiveness.DefinitelyDead;
        var recovered = TcsOverrideTransaction.RecoverAbandonedTransactions(stateDirectory: state,
            processLifetime: lifetime);
        Assert(recovered.Restored == 1 && !recovered.HasBlockingProblems, "monitor crash recovery succeeded");
        var restored = File.ReadAllText(config);
        Assert(MonitorDlssDefaults.Get(restored, "GENERAL", "PresetPath") == preset,
            "flat preset restored after ReShade rewrites config");
        Assert(MonitorDlssDefaults.Get(restored, "GENERAL", "StartupPresetPath") is null,
            "absent startup key restored as absent");
        Assert(MonitorDlssDefaults.Get(restored, "OVERLAY", "Window") == "new layout",
            "ReShade window settings preserved");
        Assert(MonitorDlssDefaults.Get(restored, "INPUT", "KeyEffects") == "117,0,0,0",
            "flat F6 binding restored");
        var remapped = MonitorDlssDefaults.Set(active, "INPUT", "KeyEffects", "118,0,0,0");
        var sessionPreset = MonitorDlssDefaults.Get(active, "GENERAL", "PresetPath")!;
        var merged = MonitorDlssDefaults.Restore(remapped, original, config, sessionPreset);
        Assert(MonitorDlssDefaults.Get(merged, "INPUT", "KeyEffects") == "118,0,0,0",
            "intentional user hotkey change preserved");
    }

    private static void TestMonitorMsaaOptInAndRestore(string root)
    {
        Assert(MonitorMsaaDefaults.ShouldApply(true, "1"), "native MSAA explicit opt-in");
        Assert(!MonitorMsaaDefaults.ShouldApply(false, "1"), "flat/diagnostic MSAA not overridden");
        foreach (var setting in new string?[] { null, "", "0", "true", "01", "1 " })
            Assert(!MonitorMsaaDefaults.ShouldApply(true, setting), "MSAA default stays unchanged");
        var state = Path.Combine(root, "state-msaa");
        var config = Path.Combine(root, "msaa.ini");
        const string original = "; test\r\n[Other]\r\nAnti Aliasing=2\r\n" +
            "[Graphics Options]\r\n  Anti Aliasing = 8 ; keep comment\r\nWidth=3440\r\n";
        File.WriteAllText(config, original, new System.Text.UTF8Encoding(true));
        var originalBytes = File.ReadAllBytes(config);
        using var transaction = new TcsOverrideTransaction(state, FakeProcessLifetime.Alive());
        transaction.BeginMonitorMsaaOff(config);
        Assert(File.ReadAllText(config) == original.Replace("= 8 ;", "= 0 ;", StringComparison.Ordinal),
            "MSAA changes only numeric value in Graphics Options");
        using (var journal = JsonDocument.Parse(File.ReadAllBytes(SingleFile(state, "*.journal.json"))))
            Assert(journal.RootElement.GetProperty("SchemaVersion").GetInt32() == 3 &&
                journal.RootElement.GetProperty("MonitorMsaaOff").GetBoolean(), "MSAA journal type/version");
        using (var competitor = new TcsOverrideTransaction(state, FakeProcessLifetime.Alive()))
            ExpectThrows<InvalidOperationException>(() => competitor.BeginMonitorMsaaOff(config), "MSAA target lock");
        Assert(transaction.Restore(), "MSAA normal restore");
        Assert(File.ReadAllBytes(config).SequenceEqual(originalBytes), "MSAA byte-exact/BOM restore");
        Assert(!Directory.EnumerateFiles(state, "*.journal.json").Any(), "MSAA journal cleaned");
    }

    private static void TestMonitorMsaaMergeAndRollback(string root)
    {
        var number = 0;
        foreach (var aa in new string?[] { "0", "4", "8", null })
        {
            var suffix = (number++).ToString();
            var state = Path.Combine(root, "state-msaa-merge-" + suffix);
            var config = Path.Combine(root, "msaa-merge-" + suffix + ".ini");
            File.WriteAllText(config, "[Graphics Options]\nAnti Aliasing=8\nWidth=3440\n");
            // Disposal is the same fallback used when a later launch step throws.
            using (var transaction = new TcsOverrideTransaction(state, FakeProcessLifetime.Alive()))
            {
                transaction.BeginMonitorMsaaOff(config);
                var edited = "[Graphics Options]\n" + (aa is null ? "" : $"Anti Aliasing={aa} ; user comment\n") +
                    "Width=1920\n[Sound Options]\nVolume=37\n";
                File.WriteAllText(config, edited);
            }
            var restored = File.ReadAllText(config);
            var expected = aa == "0" ? "8" : aa;
            Assert(expected is null ? !restored.Contains("Anti Aliasing", StringComparison.Ordinal) :
                restored.Contains($"Anti Aliasing={expected} ; user comment", StringComparison.Ordinal),
                "MSAA merge preserves explicit AA changes/deletion and current comments");
            Assert(restored.Contains("Width=1920", StringComparison.Ordinal) &&
                restored.Contains("Volume=37", StringComparison.Ordinal), "MSAA merge preserves other game edits");
            Assert(!Directory.EnumerateFiles(state, "*.journal.json").Any(), "MSAA dispose cleans journal");
        }
        var missingConfig = Path.Combine(root, "msaa-originally-absent.ini");
        File.WriteAllText(missingConfig, "[Graphics Options]\nWidth=3440\n");
        using (var transaction = new TcsOverrideTransaction(Path.Combine(root, "state-msaa-absent"), FakeProcessLifetime.Alive()))
        {
            transaction.BeginMonitorMsaaOff(missingConfig);
            Assert(File.ReadAllText(missingConfig).Contains("Anti Aliasing=0", StringComparison.Ordinal), "missing AA inserted");
            File.AppendAllText(missingConfig, "Gamma=0.7\n");
        }
        Assert(File.ReadAllText(missingConfig) == "[Graphics Options]\nWidth=3440\nGamma=0.7\n", "absent AA restored without losing edits");
    }

    private static void TestMonitorMsaaRecovery(string root)
    {
        foreach (var changed in new[] { false, true })
        {
            var state = Path.Combine(root, "state-msaa-recovery-" + changed);
            var config = Path.Combine(root, "msaa-recovery-" + changed + ".ini");
            File.WriteAllText(config, "[Graphics Options]\nAnti Aliasing=8\nWidth=3440\n");
            var lifetime = FakeProcessLifetime.Alive();
            var transaction = new TcsOverrideTransaction(state, lifetime);
            transaction.BeginMonitorMsaaOff(config);
            var edited = $"[Graphics Options]\nAnti Aliasing={(changed ? 4 : 0)}\nWidth=1920\n";
            File.WriteAllText(config, edited);
            transaction.AbandonForTesting();
            var alive = TcsOverrideTransaction.RecoverAbandonedTransactions(stateDirectory: state, processLifetime: lifetime);
            Assert(alive.StillOwned == 1 && File.ReadAllText(config) == edited, "live MSAA owner not recovered");
            lifetime.Liveness = OwnerLiveness.Unknown;
            var unknown = TcsOverrideTransaction.RecoverAbandonedTransactions(stateDirectory: state, processLifetime: lifetime);
            Assert(unknown.Uncertain == 1 && File.ReadAllText(config) == edited, "unknown MSAA owner not recovered");
            lifetime.Liveness = OwnerLiveness.DefinitelyDead;
            var recovered = TcsOverrideTransaction.RecoverAbandonedTransactions(stateDirectory: state, processLifetime: lifetime);
            Assert(recovered.Restored == 1 && !recovered.HasBlockingProblems, "MSAA abandoned journal recovered");
            Assert(File.ReadAllText(config) == $"[Graphics Options]\nAnti Aliasing={(changed ? 4 : 8)}\nWidth=1920\n",
                "MSAA crash recovery merges unrelated and deliberate AA edits");
            Assert(TcsOverrideTransaction.RecoverAbandonedTransactions(stateDirectory: state, processLifetime: lifetime).Restored == 0,
                "MSAA recovery idempotent");
        }
    }

    private static void TestMonitorMsaaRejectsAmbiguity(string root)
    {
        foreach (var invalid in new[] {
            "[Other]\nAnti Aliasing=8\n",
            "[Graphics Options]\nAnti Aliasing=8\nAnti Aliasing=4\n",
            "[Graphics Options]\nAnti Aliasing=8\n[graphics options]\nAnti Aliasing=4\n",
            "[Graphics Options]\nAnti Aliasing=unknown\n" })
            ExpectThrows<InvalidDataException>(() => MonitorMsaaDefaults.Create(invalid), "ambiguous MSAA rejected");
        const string original = "[Graphics Options]\nAnti Aliasing=8\n";
        Assert(MonitorMsaaDefaults.Restore("[Graphics Options]\nAnti Aliasing=custom\n", original) ==
            "[Graphics Options]\nAnti Aliasing=custom\n", "unfamiliar user AA value preserved");
        Assert(MonitorMsaaDefaults.Restore("[Other]\nvalue=3\n", original) == "[Other]\nvalue=3\n", "deleted graphics section preserved");
        var state = Path.Combine(root, "state-msaa-invalid-journal");
        var config = Path.Combine(root, "msaa-invalid-journal.ini");
        File.WriteAllText(config, original);
        var lifetime = FakeProcessLifetime.Alive();
        var transaction = new TcsOverrideTransaction(state, lifetime);
        transaction.BeginMonitorMsaaOff(config);
        transaction.AbandonForTesting();
        var path = SingleFile(state, "*.journal.json");
        var journal = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(path))!;
        journal["MonitorMsaaOff"] = false;
        File.WriteAllText(path, journal.ToJsonString());
        lifetime.Liveness = OwnerLiveness.DefinitelyDead;
        var report = TcsOverrideTransaction.RecoverAbandonedTransactions(stateDirectory: state, processLifetime: lifetime);
        Assert(report.Invalid == 1 && File.ReadAllText(config).Contains("Anti Aliasing=0", StringComparison.Ordinal),
            "schema/type mismatch cannot fall back to whole game-INI restore");
    }

    private static void TestRecoveryRequiresDefinitelyDeadOwner(string root)
    {
        var state = Path.Combine(root, "state-liveness");
        var config = Path.Combine(root, "liveness.cfg");
        File.WriteAllText(config, "enabled=1\n");
        var lifetime = FakeProcessLifetime.Alive();
        var transaction = new TcsOverrideTransaction(state, lifetime);
        transaction.BeginDisable(config);
        transaction.AbandonForTesting();

        var aliveReport = TcsOverrideTransaction.RecoverAbandonedTransactions(
            stateDirectory: state,
            processLifetime: lifetime);
        Assert(aliveReport.StillOwned == 1, "live owner skipped");
        Assert(File.ReadAllText(config) == "enabled=0\n", "live owner target untouched");

        lifetime.Liveness = OwnerLiveness.DefinitelyDead;
        var deadReport = TcsOverrideTransaction.RecoverAbandonedTransactions(
            stateDirectory: state,
            processLifetime: lifetime);
        Assert(deadReport.Restored == 1, "dead owner recovered");
        Assert(File.ReadAllText(config) == "enabled=1\n", "dead owner target restored");
    }

    private static void TestUncertainOwnerIsNotRecovered(string root)
    {
        var state = Path.Combine(root, "state-uncertain");
        var config = Path.Combine(root, "uncertain.cfg");
        File.WriteAllText(config, "enabled=1\n");
        var lifetime = FakeProcessLifetime.Alive();
        var transaction = new TcsOverrideTransaction(state, lifetime);
        transaction.BeginDisable(config);
        transaction.AbandonForTesting();

        lifetime.Liveness = OwnerLiveness.Unknown;
        var report = TcsOverrideTransaction.RecoverAbandonedTransactions(
            stateDirectory: state,
            processLifetime: lifetime);
        Assert(report.Uncertain == 1, "uncertain owner reported");
        Assert(report.HasBlockingProblems, "uncertain owner blocks launch");
        Assert(File.ReadAllText(config) == "enabled=0\n", "uncertain target untouched");

        lifetime.Liveness = OwnerLiveness.DefinitelyDead;
        TcsOverrideTransaction.RecoverAbandonedTransactions(
            stateDirectory: state,
            processLifetime: lifetime);
    }

    private static void TestConflictDoesNotOverwrite(string root)
    {
        var state = Path.Combine(root, "state-conflict");
        var config = Path.Combine(root, "conflict.cfg");
        File.WriteAllText(config, "enabled=1\n");
        var lifetime = FakeProcessLifetime.Alive();

        using var transaction = new TcsOverrideTransaction(state, lifetime);
        transaction.BeginDisable(config);
        File.WriteAllText(config, "enabled=0\nuser_change=1\n");
        ExpectThrows<IOException>(() => transaction.Restore(), "restore conflict");
        Assert(
            File.ReadAllText(config) == "enabled=0\nuser_change=1\n",
            "conflict target not overwritten");
        Assert(
            Directory.EnumerateFiles(state, "*restore-conflict*.cfg").Any(),
            "conflict copy created");

        File.WriteAllText(config, "enabled=0\n");
        Assert(transaction.Restore(), "conflict cleanup restore");
    }

    private static void TestCorruptBackupDoesNotOverwrite(string root)
    {
        var state = Path.Combine(root, "state-corrupt");
        var config = Path.Combine(root, "corrupt.cfg");
        File.WriteAllText(config, "enabled=1\n");
        var lifetime = FakeProcessLifetime.Alive();
        var transaction = new TcsOverrideTransaction(state, lifetime);
        transaction.BeginDisable(config);
        transaction.AbandonForTesting();

        File.WriteAllText(SingleFile(state, "*.backup"), "corrupt");
        lifetime.Liveness = OwnerLiveness.DefinitelyDead;
        var report = TcsOverrideTransaction.RecoverAbandonedTransactions(
            stateDirectory: state,
            processLifetime: lifetime);
        Assert(report.Invalid == 1, "bad backup rejected");
        Assert(File.ReadAllText(config) == "enabled=0\n", "bad backup target untouched");
    }

    private static void TestDisposeNeverThrows(string root)
    {
        var state = Path.Combine(root, "state-dispose");
        var config = Path.Combine(root, "dispose.cfg");
        File.WriteAllText(config, "enabled=1\n");
        var transaction = new TcsOverrideTransaction(state, FakeProcessLifetime.Alive());
        transaction.BeginDisable(config);
        File.WriteAllText(config, "enabled=0\nchanged=1\n");
        var previousError = Console.Error;
        using var capturedError = new StringWriter();
        try
        {
            Console.SetError(capturedError);
            transaction.Dispose();
        }
        finally
        {
            Console.SetError(previousError);
        }
        Assert(
            capturedError.ToString().Contains(
                "automatic TCS restore failed",
                StringComparison.Ordinal),
            "Dispose logs cleanup failure");
        Assert(
            File.ReadAllText(config) == "enabled=0\nchanged=1\n",
            "Dispose conflict did not overwrite");
    }

    private static void TestLegacyJournalIsSurfaced(string root)
    {
        var state = Path.Combine(root, "state-legacy");
        Directory.CreateDirectory(state);
        File.WriteAllText(
            Path.Combine(state, "tcs-override.session-journal.json"),
            "{\"SchemaVersion\":1}");
        var report = TcsOverrideTransaction.RecoverAbandonedTransactions(
            stateDirectory: state,
            processLifetime: FakeProcessLifetime.Alive());
        Assert(report.Invalid == 1, "legacy journal is not silently ignored");
        Assert(report.HasBlockingProblems, "legacy journal blocks launch");
    }

    private static void TestGate1Qualification(string root)
    {
        var directory = Path.Combine(root, "gate1");
        Directory.CreateDirectory(directory);
        var game = Path.Combine(directory, "game.exe");
        var module = Path.Combine(directory, "game32.dll");
        var host = Path.Combine(directory, "host64.exe");
        var artifact = Path.Combine(directory, "gate1.json");
        File.WriteAllBytes(game, [1, 2, 3]);
        File.WriteAllBytes(module, [4, 5, 6]);
        File.WriteAllBytes(host, [7, 8, 9]);
        var config = new LauncherConfig(
            Path.Combine(directory, "config.toml"),
            game,
            FileHash.Sha256(game),
            208580,
            "preflight.ps1",
            host,
            module,
            artifact,
            true,
            "legacy.cfg");

        ExpectThrows<InvalidOperationException>(
            () => Gate1Qualification.RequirePassed(config),
            "missing Gate 1 artifact blocks");

        var payload = new
        {
            schemaVersion = 1,
            gate = "gate1",
            result = "pass",
            sameTickStereoVerified = true,
            reentrantWorldPassVerified = true,
            gameSha256 = FileHash.Sha256(game),
            gameModuleSha256 = FileHash.Sha256(module),
            hostSha256 = FileHash.Sha256(host),
            qualificationRunId = Guid.NewGuid().ToString(),
            qualifiedUtc = DateTimeOffset.UtcNow.ToString("O")
        };
        File.WriteAllBytes(artifact, JsonSerializer.SerializeToUtf8Bytes(payload));
        Gate1Qualification.RequirePassed(config);

        File.WriteAllBytes(module, [4, 5, 6, 7]);
        ExpectThrows<InvalidDataException>(
            () => Gate1Qualification.RequirePassed(config),
            "changed module invalidates qualification");
    }

    private static void TestSteamProcessIdentity(string root)
    {
        var expectedPath = Path.Combine(root, "Game", "swkotor2.exe");
        var differentlyCasedPath = Path.Combine(
            root,
            "game",
            "subdirectory",
            "..",
            "SWKOTOR2.EXE");
        var wrongPath = Path.Combine(root, "Other", "swkotor2.exe");
        var boundary = new DateTime(638900000000000000L, DateTimeKind.Utc);
        var noExistingProcesses = new HashSet<int>();

        Assert(
            SteamProcessValidation.IsEligibleIdentity(
                5001,
                boundary,
                differentlyCasedPath,
                expectedPath,
                boundary,
                noExistingProcesses,
                out _),
            "Steam identity accepts boundary time and canonical path case-insensitively");

        Assert(
            !SteamProcessValidation.IsEligibleIdentity(
                5001,
                boundary,
                expectedPath,
                expectedPath,
                boundary,
                new HashSet<int> { 5001 },
                out _),
            "Steam identity rejects pre-existing PID");

        Assert(
            !SteamProcessValidation.IsEligibleIdentity(
                5001,
                boundary.AddTicks(-1),
                expectedPath,
                expectedPath,
                boundary,
                noExistingProcesses,
                out _),
            "Steam identity rejects process older than launch boundary");

        Assert(
            !SteamProcessValidation.IsEligibleIdentity(
                5001,
                boundary.AddTicks(1),
                wrongPath,
                expectedPath,
                boundary,
                noExistingProcesses,
                out _),
            "Steam identity rejects other executable path");

        Assert(
            !SteamProcessValidation.IsEligibleIdentity(
                5001,
                boundary.AddTicks(1),
                "\0",
                expectedPath,
                boundary,
                noExistingProcesses,
                out _),
            "Steam identity fails closed for malformed path");
    }

    private static void TestSteamPe32Parsing()
    {
        var dosHeader = ValidDosHeader();
        Assert(
            SteamProcessValidation.TryGetPeHeaderOffset(
                dosHeader,
                out var peOffset,
                out _),
            "valid DOS header accepted");
        Assert(peOffset == 0x100, "PE offset parsed");
        Assert(
            !SteamProcessValidation.TryGetPeHeaderOffset(
                dosHeader.AsSpan(0, SteamProcessValidation.DosHeaderSize - 1),
                out _,
                out _),
            "truncated DOS header rejected");

        var invalidDosSignature = ValidDosHeader();
        invalidDosSignature[0] = 0;
        Assert(
            !SteamProcessValidation.TryGetPeHeaderOffset(
                invalidDosSignature,
                out _,
                out _),
            "invalid MZ signature rejected");

        var invalidPeOffset = ValidDosHeader();
        BinaryPrimitives.WriteInt32LittleEndian(invalidPeOffset.AsSpan(0x3c, 4), 0x20);
        Assert(
            !SteamProcessValidation.TryGetPeHeaderOffset(
                invalidPeOffset,
                out _,
                out _),
            "out-of-range PE offset rejected");

        var ntHeader = ValidNtHeaderPrefix();
        Assert(
            SteamProcessValidation.IsPe32NtHeader(ntHeader, out _),
            "valid I386 PE32 header accepted");
        Assert(
            !SteamProcessValidation.IsPe32NtHeader(
                ntHeader.AsSpan(0, SteamProcessValidation.NtHeaderPrefixSize - 1),
                out _),
            "truncated NT header rejected");

        var invalidPeSignature = ValidNtHeaderPrefix();
        invalidPeSignature[0] = 0;
        Assert(
            !SteamProcessValidation.IsPe32NtHeader(invalidPeSignature, out _),
            "invalid PE signature rejected");

        var x64Header = ValidNtHeaderPrefix();
        BinaryPrimitives.WriteUInt16LittleEndian(x64Header.AsSpan(4, 2), 0x8664);
        Assert(
            !SteamProcessValidation.IsPe32NtHeader(x64Header, out _),
            "x64 machine rejected");

        var pe32PlusHeader = ValidNtHeaderPrefix();
        BinaryPrimitives.WriteUInt16LittleEndian(pe32PlusHeader.AsSpan(24, 2), 0x020b);
        Assert(
            !SteamProcessValidation.IsPe32NtHeader(pe32PlusHeader, out _),
            "PE32+ optional header rejected");

        var missingOptionalHeader = ValidNtHeaderPrefix();
        BinaryPrimitives.WriteUInt16LittleEndian(missingOptionalHeader.AsSpan(20, 2), 0);
        Assert(
            !SteamProcessValidation.IsPe32NtHeader(missingOptionalHeader, out _),
            "missing optional header rejected");

        var undersizedOptionalHeader = ValidNtHeaderPrefix();
        BinaryPrimitives.WriteUInt16LittleEndian(
            undersizedOptionalHeader.AsSpan(20, 2),
            2);
        Assert(
            !SteamProcessValidation.IsPe32NtHeader(undersizedOptionalHeader, out _),
            "undersized PE32 optional header rejected");

        static byte[] ValidDosHeader()
        {
            var header = new byte[SteamProcessValidation.DosHeaderSize];
            header[0] = (byte)'M';
            header[1] = (byte)'Z';
            BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(0x3c, 4), 0x100);
            return header;
        }

        static byte[] ValidNtHeaderPrefix()
        {
            var header = new byte[SteamProcessValidation.NtHeaderPrefixSize];
            BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(0, 4), 0x00004550);
            BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(4, 2), 0x014c);
            BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(20, 2), 0x00e0);
            BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(24, 2), 0x010b);
            return header;
        }
    }

    private static void TestSteamExportParsing()
    {
        const uint expectedFunctionRva = 0x1100;
        var validImage = CreatePe32ExportFixture();
        Assert(
            SteamProcessValidation.TryFindPe32ExportRva(
                validImage,
                SteamProcessValidation.ProbeBootstrapExportName,
                out var functionRva,
                out _),
            "undecorated probe bootstrap export found");
        Assert(functionRva == expectedFunctionRva, "probe bootstrap RVA parsed");
        Assert(
            SteamProcessValidation.TryGetPe32ImageIdentity(
                validImage,
                out var imageIdentity,
                out _),
            "PE32 module identity parsed");
        Assert(imageIdentity.SectionCount == 1, "PE32 identity section count");
        Assert(imageIdentity.SizeOfImage == 0x2000, "PE32 identity image size");
        Assert(imageIdentity.SizeOfHeaders == 0x0200, "PE32 identity header size");
        Assert(imageIdentity.ExportDirectoryRva == 0x1000, "PE32 identity export RVA");
        Assert(
            SteamProcessValidation.TryReadPe32FileBytesAtRva(
                validImage,
                expectedFunctionRva,
                16,
                out var codeWitness,
                out _),
            "bootstrap code witness mapped from RVA");
        Assert(
            codeWitness.SequenceEqual(Enumerable.Range(1, 16).Select(value => (byte)value)),
            "bootstrap code witness bytes exact");
        Assert(
            !SteamProcessValidation.TryReadPe32FileBytesAtRva(
                validImage,
                0x13f8,
                16,
                out _,
                out _),
            "RVA witness crossing raw section rejected");

        Assert(
            !SteamProcessValidation.TryFindPe32ExportRva(
                validImage,
                "_K2VR_ProbeBootstrap@4",
                out _,
                out _),
            "decorated substitute export rejected");

        var forwardedExport = CreatePe32ExportFixture();
        BinaryPrimitives.WriteUInt32LittleEndian(
            forwardedExport.AsSpan(0x240, 4),
            0x1080);
        Assert(
            !SteamProcessValidation.TryFindPe32ExportRva(
                forwardedExport,
                SteamProcessValidation.ProbeBootstrapExportName,
                out _,
                out _),
            "forwarded bootstrap export rejected");

        var nonExecutableExport = CreatePe32ExportFixture();
        BinaryPrimitives.WriteUInt32LittleEndian(
            nonExecutableExport.AsSpan(0x178 + 36, 4),
            0x40000040);
        Assert(
            !SteamProcessValidation.TryFindPe32ExportRva(
                nonExecutableExport,
                SteamProcessValidation.ProbeBootstrapExportName,
                out _,
                out _),
            "bootstrap export outside executable section rejected");

        var invalidOrdinal = CreatePe32ExportFixture();
        BinaryPrimitives.WriteUInt16LittleEndian(invalidOrdinal.AsSpan(0x248, 2), 1);
        Assert(
            !SteamProcessValidation.TryFindPe32ExportRva(
                invalidOrdinal,
                SteamProcessValidation.ProbeBootstrapExportName,
                out _,
                out _),
            "out-of-range export ordinal rejected");

        Assert(
            SteamProcessValidation.TryAddRemoteModuleRva(
                0x10000000,
                expectedFunctionRva,
                out var remoteAddress,
                out _),
            "remote module base and export RVA combined");
        Assert(remoteAddress == 0x10001100, "remote bootstrap address exact");
        Assert(
            !SteamProcessValidation.TryAddRemoteModuleRva(
                0xfffff000,
                expectedFunctionRva,
                out _,
                out _),
            "x86 remote bootstrap address overflow rejected");
        Assert(
            !SteamProcessValidation.TryAddRemoteModuleRva(
                0,
                expectedFunctionRva,
                out _,
                out _),
            "null remote module base rejected");

        const int targetProcessId = 4242;
        var freshWitness =
            "[K2VR t=10ms] bootstrap-entry source=explicit-export pid=4242 tid=7\n" +
            "[K2VR t=11ms] bootstrap-complete pid=4242 sampler=started hooks=disabled\n";
        Assert(
            SteamProcessValidation.HasFreshProbeBootstrapWitness(
                freshWitness,
                targetProcessId,
                out _),
            "fresh ordered target-PID bootstrap witness accepted");
        Assert(
            !SteamProcessValidation.HasFreshProbeBootstrapWitness(
                freshWitness,
                4243,
                out _),
            "bootstrap witness from other PID rejected");
        Assert(
            !SteamProcessValidation.HasFreshProbeBootstrapWitness(
                "[K2VR] bootstrap-entry source=explicit-export pid=4242 tid=7\n",
                targetProcessId,
                out _),
            "bootstrap witness without completion rejected");
        Assert(
            !SteamProcessValidation.HasFreshProbeBootstrapWitness(
                "[K2VR] bootstrap-complete pid=4242 sampler=started hooks=disabled\n" +
                "[K2VR] bootstrap-entry source=explicit-export pid=4242 tid=7\n",
                targetProcessId,
                out _),
            "out-of-order bootstrap witness rejected");

        var renderTraceWitness =
            "{\"type\":\"session\",\"pid\":4242,\"qpc_frequency\":10000000," +
            "\"hooks\":3,\"duration_ms\":600000,\"double_pass\":false}\n";
        Assert(
            SteamProcessValidation.HasFreshRenderTraceBootstrapWitness(
                renderTraceWitness,
                targetProcessId,
                out _),
            "fresh render-trace bootstrap witness accepted");
        Assert(
            !SteamProcessValidation.HasFreshRenderTraceBootstrapWitness(
                renderTraceWitness.Replace(
                    "\"hooks\":3",
                    "\"hooks\":2",
                    StringComparison.Ordinal),
                targetProcessId,
                out _),
            "partial render-trace hook witness rejected");

        var renderDoublePassWitness =
            "{\"type\":\"session\",\"pid\":4242,\"qpc_frequency\":10000000," +
            "\"hooks\":3,\"duration_ms\":600000,\"double_pass\":true}\n" +
            "{\"type\":\"double_pass_armed\",\"qpc\":1234,\"this\":null," +
            "\"tid\":7,\"primary_return\":null,\"extra_return\":null," +
            "\"duration_qpc\":0,\"duration_us\":0,\"key\":\"F8\"," +
            "\"initial_down\":false}\n";
        Assert(
            SteamProcessValidation.HasFreshRenderDoublePassBootstrapWitness(
                renderDoublePassWitness,
                targetProcessId,
                out _),
            "fresh F8-armed render double-pass witness accepted");
        Assert(
            !SteamProcessValidation.HasFreshRenderDoublePassBootstrapWitness(
                renderDoublePassWitness.Replace(
                    "{\"type\":\"double_pass_armed\"",
                    "{\"type\":\"not_armed\"",
                    StringComparison.Ordinal),
                targetProcessId,
                out _),
            "render double-pass witness without armed event rejected");
        Assert(
            !SteamProcessValidation.HasFreshRenderTraceBootstrapWitness(
                renderDoublePassWitness,
                targetProcessId,
                out _),
            "dangerous double-pass session is not accepted as passive trace");

        var vrBridgeWitness =
            "[K2VR-BRIDGE t=10ms] bootstrap pid=4242 tid=7 version=1.0 " +
            "nonce=0123456789abcdefFEDCBA9876543210 generation=1\n" +
            "[K2VR-BRIDGE t=11ms] bootstrap complete worker=1 " +
            "camera_writes=0 render_writes=0\n";
        Assert(
            SteamProcessValidation.HasFreshVrBridgeBootstrapWitness(
                vrBridgeWitness,
                targetProcessId,
                out _),
            "fresh ordered target-PID VrBridge witness accepted");
        Assert(
            !SteamProcessValidation.HasFreshVrBridgeBootstrapWitness(
                vrBridgeWitness,
                4243,
                out _),
            "VrBridge witness from other PID rejected");
        Assert(
            !SteamProcessValidation.HasFreshVrBridgeBootstrapWitness(
                "[K2VR-BRIDGE] bootstrap pid=4242 tid=7\n",
                targetProcessId,
                out _),
            "VrBridge witness without completion rejected");

        static byte[] CreatePe32ExportFixture()
        {
            var image = new byte[0x600];
            image[0] = (byte)'M';
            image[1] = (byte)'Z';
            BinaryPrimitives.WriteInt32LittleEndian(image.AsSpan(0x3c, 4), 0x80);

            const int peOffset = 0x80;
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(peOffset, 4),
                0x00004550);
            BinaryPrimitives.WriteUInt16LittleEndian(
                image.AsSpan(peOffset + 4, 2),
                0x014c);
            BinaryPrimitives.WriteUInt16LittleEndian(
                image.AsSpan(peOffset + 6, 2),
                1);
            BinaryPrimitives.WriteUInt16LittleEndian(
                image.AsSpan(peOffset + 20, 2),
                0x00e0);

            const int optionalHeaderOffset = peOffset + 24;
            BinaryPrimitives.WriteUInt16LittleEndian(
                image.AsSpan(optionalHeaderOffset, 2),
                0x010b);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(optionalHeaderOffset + 56, 4),
                0x2000);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(optionalHeaderOffset + 60, 4),
                0x0200);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(optionalHeaderOffset + 92, 4),
                1);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(optionalHeaderOffset + 96, 4),
                0x1000);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(optionalHeaderOffset + 100, 4),
                0x0100);

            const int sectionOffset = optionalHeaderOffset + 0x00e0;
            image[sectionOffset] = (byte)'.';
            image[sectionOffset + 1] = (byte)'t';
            image[sectionOffset + 2] = (byte)'e';
            image[sectionOffset + 3] = (byte)'x';
            image[sectionOffset + 4] = (byte)'t';
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(sectionOffset + 8, 4),
                0x0400);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(sectionOffset + 12, 4),
                0x1000);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(sectionOffset + 16, 4),
                0x0400);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(sectionOffset + 20, 4),
                0x0200);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(sectionOffset + 36, 4),
                0x60000020);

            const int exportDirectoryOffset = 0x0200;
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(exportDirectoryOffset + 20, 4),
                1);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(exportDirectoryOffset + 24, 4),
                1);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(exportDirectoryOffset + 28, 4),
                0x1040);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(exportDirectoryOffset + 32, 4),
                0x1044);
            BinaryPrimitives.WriteUInt32LittleEndian(
                image.AsSpan(exportDirectoryOffset + 36, 4),
                0x1048);

            BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(0x240, 4), 0x1100);
            BinaryPrimitives.WriteUInt32LittleEndian(image.AsSpan(0x244, 4), 0x1050);
            BinaryPrimitives.WriteUInt16LittleEndian(image.AsSpan(0x248, 2), 0);
            var exportName = System.Text.Encoding.ASCII.GetBytes(
                SteamProcessValidation.ProbeBootstrapExportName + "\0");
            exportName.CopyTo(image.AsSpan(0x250));
            for (var index = 0; index < 16; index++)
            {
                image[0x300 + index] = (byte)(index + 1);
            }
            return image;
        }
    }

    private static void TestOpenedProcessHandleIdentity()
    {
        using var process = Process.GetCurrentProcess();
        using var mainModule = process.MainModule;
        var executablePath = mainModule?.FileName ?? throw new InvalidOperationException(
            "self-test main-module path is unavailable");
        var processHandle = Win32.OpenProcess(
            Win32.PROCESS_QUERY_INFORMATION | Win32.PROCESS_VM_READ,
            false,
            process.Id);
        Assert(processHandle != IntPtr.Zero, "opened current process for handle validation");
        try
        {
            Assert(
                ProcessInjector.TryValidateOpenedGameProcess(
                    processHandle,
                    process.Id,
                    executablePath,
                    DateTime.MinValue,
                    new HashSet<int>(),
                    out var imageBase,
                    out _),
                "opened-handle identity accepts exact current PE32 process");
            Assert(imageBase != IntPtr.Zero, "opened-handle identity returns image base");

            Assert(
                !ProcessInjector.TryValidateOpenedGameProcess(
                    processHandle,
                    process.Id,
                    executablePath + ".wrong",
                    DateTime.MinValue,
                    new HashSet<int>(),
                    out _,
                    out _),
                "opened-handle identity rejects wrong expected path");
        }
        finally
        {
            Win32.CloseHandle(processHandle);
        }
    }

    private static string SingleFile(string directory, string pattern)
    {
        var files = Directory.GetFiles(directory, pattern, SearchOption.TopDirectoryOnly);
        Assert(files.Length == 1, $"expected one file matching {pattern}");
        return files[0];
    }

    private static void ExpectThrows<TException>(Action action, string label)
        where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException)
        {
            return;
        }
        throw new InvalidOperationException($"Assertion failed: {label} did not throw");
    }

    private static void Assert(bool condition, string label)
    {
        if (!condition)
        {
            throw new InvalidOperationException($"Assertion failed: {label}");
        }
    }

    private sealed class FakeProcessLifetime : IProcessLifetime
    {
        public OwnerIdentity Current { get; } = new(4242, 638900000000000000L);
        public OwnerLiveness Liveness { get; set; }

        public static FakeProcessLifetime Alive() => new()
        {
            Liveness = OwnerLiveness.Alive
        };

        public OwnerLiveness Inspect(OwnerIdentity owner)
        {
            Assert(owner.ProcessId == Current.ProcessId, "inspected owner PID");
            Assert(
                owner.StartTimeUtcTicks == Current.StartTimeUtcTicks,
                "inspected owner start time");
            return Liveness;
        }
    }
}
