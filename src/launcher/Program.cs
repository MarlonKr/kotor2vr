using System.Diagnostics;
using System.Runtime.ExceptionServices;
using System.Text;
using KPatchCore.Launcher;
using KPatchCore.Models;

namespace Kotor2Vr.Launcher;

internal static class Program
{
    private const string DefaultConfigName = "kotor2vr.toml";
    private const int SupportedSteamAppId = 208580;

    public static async Task<int> Main(string[] args)
    {
        try
        {
            var parsed = Arguments.Parse(args);
            if (parsed.Command == "self-test")
            {
                return SelfTest.Run();
            }
            if (parsed.Command == "help")
            {
                return Usage();
            }

            // A portable release owns no legacy transactions. Never recover or alter
            // another installation's monitor configuration from this entry point.
            if (parsed.Command == "portable-stereo")
            {
                var portableConfig = LauncherConfig.Load(parsed.ConfigPath);
                PortableStartup.ResetRuntimeEnvironment();
                PortableStartup.RequireRegisteredSteamGame(portableConfig.GameExecutable);
                return await GameImageSmokeAsync(portableConfig, new RecoveryReport(0, 0, 0, 0),
                    parsed.DryRun, confirmed: true, nativeStereo: true, portable: true);
            }

            // Recovery intentionally precedes configuration parsing. A broken or moved
            // TOML file must never prevent restoration after a launcher/host crash.
            var recovery = TcsOverrideTransaction.RecoverAbandonedTransactions(
                Console.Out);
            if (parsed.Command == "restore")
            {
                return PrintRecoveryResult(recovery);
            }

            var config = LauncherConfig.Load(parsed.ConfigPath);
            return parsed.Command switch
            {
                "preflight" => await PreflightRunner.RunAsync(
                    config, parsed.Json, CancellationToken.None),
                "status" => PrintStatus(config, recovery),
                "probe" => await ProbeAsync(
                    config, recovery, parsed.DryRun, parsed.ConfirmReadOnlyProbe),
                "trace-render" => await RenderTraceAsync(
                    config, recovery, parsed.DryRun, parsed.ConfirmRenderTrace,
                    enableDoublePass: false),
                "trace-double-pass" => await RenderTraceAsync(
                    config, recovery, parsed.DryRun,
                    parsed.ConfirmRenderDoublePass,
                    enableDoublePass: true),
                "bridge-smoke" => await BridgeSmokeAsync(
                    config, recovery, parsed.DryRun, parsed.ConfirmVrBridge),
                "native-stereo" => await GameImageSmokeAsync(
                    config, recovery, parsed.DryRun, parsed.ConfirmGameImageSmoke, nativeStereo: true),
                "game-image-smoke" => await GameImageSmokeAsync(
                    config,
                    recovery,
                    parsed.DryRun,
                    parsed.ConfirmGameImageSmoke),
                "launch" => await LaunchAsync(config, recovery, parsed.DryRun),
                "help" => Usage(),
                _ => Usage($"Unknown command '{parsed.Command}'.")
            };
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine($"ERROR: {exception.Message}");
            return 1;
        }
    }

    private static int PrintStatus(
        LauncherConfig config,
        RecoveryReport recovery)
    {
        Console.WriteLine($"Config:             {config.ConfigPath}");
        Console.WriteLine($"Game:               {config.GameExecutable}");
        Console.WriteLine($"Game exists:        {File.Exists(config.GameExecutable)}");
        Console.WriteLine($"Host:               {config.HostExecutable}");
        Console.WriteLine($"Host exists:        {File.Exists(config.HostExecutable)}");
        Console.WriteLine($"Game module:        {config.GameModule}");
        Console.WriteLine($"Game module exists: {File.Exists(config.GameModule)}");
        Console.WriteLine($"Gate 1 artifact:    {config.Gate1QualificationArtifact}");
        Console.WriteLine($"Gate 1 qualified:   {File.Exists(config.Gate1QualificationArtifact)}");
        Console.WriteLine($"DLSS requested:     {config.DlssEnabled}");
        Console.WriteLine(
            $"Recovery:           restored={recovery.Restored}, owned={recovery.StillOwned}, " +
            $"uncertain={recovery.Uncertain}, invalid={recovery.Invalid}");
        Console.WriteLine($"Launcher process:   {(Environment.Is64BitProcess ? "x64 (injection blocked)" : "x86")}");
        return 0;
    }

    private static int PrintRecoveryResult(RecoveryReport recovery)
    {
        Console.WriteLine(
            $"TCS recovery: restored={recovery.Restored}, owned={recovery.StillOwned}, " +
            $"uncertain={recovery.Uncertain}, invalid={recovery.Invalid}");
        return recovery.HasBlockingProblems || recovery.StillOwned != 0 ? 1 : 0;
    }

    private static async Task<int> LaunchAsync(
        LauncherConfig config,
        RecoveryReport recovery,
        bool dryRun)
    {
        RefuseUnsafeRecoveryState(recovery);
        ValidateSupportedGame(config);
        RequireFile(config.HostExecutable, "x64 host");
        RequireStaticOpenXrLoader(config.HostExecutable);
        RequireFile(config.GameModule, "x86 game module");
        Gate1Qualification.RequirePassed(config);
        await RequirePassingPreflightAsync(config);

        if (dryRun)
        {
            Console.WriteLine(
                "Dry run passed, including the Gate 1 qualification lock. " +
                "No host, game, injection, or config override was started.");
            return 0;
        }

        RequireX86Launcher();
        EnsureGameIsNotAlreadyRunning(config.GameExecutable);
        using var transaction = new TcsOverrideTransaction();
        Process? host = null;
        Exception? primaryFailure = null;
        var cleanupFailures = new List<Exception>();
        var gameExitCode = 0;

        try
        {
            // The legacy feed consumes a single changing view. It must be off for every
            // actual VR session, including sessions where stereo DLSS starts disabled.
            transaction.BeginDisable(config.LegacyDlssConfig);
            Console.WriteLine(
                "Temporarily disabled the legacy single-view TCS feed " +
                $"(session {transaction.SessionNonce:N}).");

            host = StartHost(config, transaction.SessionNonce);
            await Task.Delay(750);
            if (host.HasExited)
            {
                throw new InvalidOperationException(
                    $"The x64 host exited early with code {host.ExitCode}.");
            }

            using var game = StartInjectedGame(config);
            Console.WriteLine($"Monitoring exact KOTOR II process {game.Id}...");
            await game.WaitForExitAsync();
            gameExitCode = game.ExitCode;
        }
        catch (Exception exception)
        {
            primaryFailure = exception;
        }

        try
        {
            StopHost(host);
        }
        catch (Exception exception)
        {
            cleanupFailures.Add(new IOException(
                $"Could not stop the VR host cleanly: {exception.Message}",
                exception));
        }

        try
        {
            transaction.Restore(Console.Out);
        }
        catch (Exception exception)
        {
            cleanupFailures.Add(new IOException(
                $"Could not restore the legacy TCS config: {exception.Message}",
                exception));
        }

        if (primaryFailure is not null)
        {
            foreach (var cleanupFailure in cleanupFailures)
            {
                Console.Error.WriteLine($"ERROR during cleanup: {cleanupFailure.Message}");
            }
            ExceptionDispatchInfo.Capture(primaryFailure).Throw();
        }
        if (cleanupFailures.Count == 1)
        {
            throw cleanupFailures[0];
        }
        if (cleanupFailures.Count > 1)
        {
            throw new AggregateException("Multiple launch cleanup operations failed.", cleanupFailures);
        }

        return gameExitCode;
    }

    private static async Task<int> ProbeAsync(
        LauncherConfig config,
        RecoveryReport recovery,
        bool dryRun,
        bool confirmed)
    {
        RefuseUnsafeRecoveryState(recovery);
        ValidateSupportedGame(config);
        RequireFile(config.GameModule, "x86 read-only probe module");
        await RequirePassingPreflightAsync(config);

        if (dryRun)
        {
            Console.WriteLine(
                "Probe dry run passed. No game was launched and no DLL was injected.");
            return 0;
        }
        if (!confirmed)
        {
            throw new InvalidOperationException(
                "The probe launches KOTOR II and injects the read-only game32 probe DLL. " +
                "It installs no hooks and modifies no engine memory, but it is still a live DLL injection. " +
                "Re-run with --confirm-read-only-probe after closing any existing game instance.");
        }

        RequireX86Launcher();
        EnsureGameIsNotAlreadyRunning(config.GameExecutable);
        using var game = StartInjectedGame(config);
        Console.WriteLine(
            $"Read-only probe injected into exact process {game.Id}. " +
            "No VR host was started and no TCS config was changed.");
        Console.WriteLine(
            "Probe log: %LOCALAPPDATA%\\Kotor2VR\\logs\\game32-probe.log");
        await game.WaitForExitAsync();
        return game.ExitCode;
    }

    private static async Task<int> RenderTraceAsync(
        LauncherConfig config,
        RecoveryReport recovery,
        bool dryRun,
        bool confirmed,
        bool enableDoublePass)
    {
        RefuseUnsafeRecoveryState(recovery);
        ValidateSupportedGame(config);
        RequireFile(config.GameModule, "x86 render-trace module");

        // This diagnostic validates the exact EXE again inside game32 and never
        // touches campaign data, TCS, OpenXR, or the VR host. Do not block render
        // reverse engineering on the campaign-mod acceptance profile.

        if (dryRun)
        {
            Console.WriteLine(
                $"{(enableDoublePass ? "Render double-pass" : "Render-trace")} dry run passed. " +
                "No game was launched and no DLL was injected.");
            return 0;
        }
        if (!confirmed)
        {
            var confirmation = enableDoublePass
                ? "--confirm-render-double-pass"
                : "--confirm-render-trace";
            var extraWarning = enableDoublePass
                ? " In this mode a fresh F8 press during loaded 3D gameplay calls the " +
                  "original Scene+0xB8 render method one additional time, at most once."
                : string.Empty;
            throw new InvalidOperationException(
                "The render trace launches KOTOR II, injects game32, and temporarily swaps " +
                "three exact-build render VTable cells for bounded logging wrappers." +
                extraWarning + $" Re-run with {confirmation} after closing any existing game instance.");
        }

        RequireX86Launcher();
        EnsureGameIsNotAlreadyRunning(config.GameExecutable);
        using var game = StartInjectedGame(
            config,
            renderTrace: !enableDoublePass,
            renderDoublePass: enableDoublePass);
        Console.WriteLine(
            $"Bounded {(enableDoublePass ? "render double-pass" : "render trace")} active " +
            $"in exact process {game.Id}. No VR host was started " +
            "and no TCS config was changed.");
        Console.WriteLine(enableDoublePass
            ? "Load a save and wait for controllable 3D gameplay. Then press F8 once; " +
              "do not press F8 in a menu or loading screen. Exit normally afterward. " +
              "Trace: %LOCALAPPDATA%\\Kotor2VR\\logs\\render-trace.jsonl"
            : "Load a save, look around briefly, then exit the game normally. " +
              "Trace: %LOCALAPPDATA%\\Kotor2VR\\logs\\render-trace.jsonl");
        await game.WaitForExitAsync();
        return game.ExitCode;
    }

    private static async Task<int> BridgeSmokeAsync(
        LauncherConfig config,
        RecoveryReport recovery,
        bool dryRun,
        bool confirmed)
    {
        RefuseUnsafeRecoveryState(recovery);
        ValidateSupportedGame(config);
        RequireFile(config.HostExecutable, "x64 host");
        RequireStaticOpenXrLoader(config.HostExecutable);
        RequireFile(config.GameModule, "x86 VrBridge game module");
        RequireX86Launcher();

        // Deliberately no campaign preflight, Gate 1 qualification, or TCS
        // transaction: this only proves the versioned host/game bridge.
        if (dryRun)
        {
            Console.WriteLine(
                "VrBridge smoke dry run passed. Exact game and bridge binaries are present, " +
                "the host has no dynamic OpenXR-loader import, and this launcher is x86. " +
                "No host, game, injection, or TCS override was started.");
            return 0;
        }
        if (!confirmed)
        {
            throw new InvalidOperationException(
                "The VrBridge smoke starts the visible OpenXR host and KOTOR II, then " +
                "injects the 32-byte v1 bridge bootstrap payload. Re-run with " +
                "--confirm-vr-bridge after closing any existing game instance.");
        }

        EnsureGameIsNotAlreadyRunning(config.GameExecutable);
        var sessionNonce = Guid.NewGuid();
        var bootstrapPayload = VrBridgeBootstrapPayload.Create(sessionNonce);
        Process? host = null;
        try
        {
            host = StartHost(config, sessionNonce, visibleSmoke: true);
            await Task.Delay(750);
            if (host.HasExited)
            {
                throw new InvalidOperationException(
                    $"The x64 host exited early with code {host.ExitCode}.");
            }

            using var game = StartInjectedGame(
                config,
                vrBridgeBootstrapPayload: bootstrapPayload);
            Console.WriteLine(
                $"VrBridge smoke active in exact process {game.Id}; host PID {host.Id}, " +
                $"session {sessionNonce:N}. No TCS config was changed.");
            Console.WriteLine(
                "Bridge log: %LOCALAPPDATA%\\Kotor2VR\\logs\\vr-bridge.log");
            await game.WaitForExitAsync();
            return game.ExitCode;
        }
        finally
        {
            StopHost(host);
        }
    }

    private static async Task<int> GameImageSmokeAsync(
        LauncherConfig config,
        RecoveryReport recovery,
        bool dryRun,
        bool confirmed,
        bool nativeStereo = false,
        bool portable = false)
    {
        RefuseUnsafeRecoveryState(recovery);
        ValidateSupportedGame(config);
        RequireFile(config.HostExecutable, "x64 host");
        RequireStaticOpenXrLoader(config.HostExecutable);
        RequireFile(config.GameModule, "x86 game-image module");
        RequireX86Launcher();

        // F7 arms the render-thread GPU path after a controllable world is loaded.
        if (dryRun)
        {
            Console.WriteLine(
                (nativeStereo ? "Native stereo dry run passed. " : "Game-image smoke dry run passed. ") +
                "Exact game, host, and game32 " +
                "binaries are present. No host, game, injection, or TCS override was started.");
            return 0;
        }
        if (!confirmed)
        {
            throw new InvalidOperationException(
                "The game-image smoke starts Meta OpenXR and KOTOR II, injects the " +
                "F7-armed camera renderer. Re-run with --confirm-game-image-smoke " +
                "after closing any existing game instance.");
        }

        EnsureGameIsNotAlreadyRunning(config.GameExecutable);
        var sessionNonce = Guid.NewGuid();
        var bootstrapPayload = VrBridgeBootstrapPayload.Create(sessionNonce);
        using var monitorDefaults = new TcsOverrideTransaction();
        using var monitorMsaa = new TcsOverrideTransaction();
        Process? host = null;
        try
        {
            if (!portable && MonitorMsaaDefaults.ShouldApply(nativeStereo,
                    Environment.GetEnvironmentVariable(MonitorMsaaDefaults.EnvironmentVariable)))
            {
                monitorMsaa.BeginMonitorMsaaOff(Path.Combine(
                    Path.GetDirectoryName(config.GameExecutable)!, "swkotor2.ini"));
                Console.WriteLine("Optional monitor MSAA=0 for this VR launch; native eye quality is unchanged. " +
                    "The previous AA setting returns on exit unless changed during the session.");
            }
            var reshadeConfig = Path.Combine(Path.GetDirectoryName(config.GameExecutable)!, "ReShade.ini");
            if (!portable && nativeStereo && File.Exists(reshadeConfig))
            {
                monitorDefaults.BeginMonitorDefaultOff(reshadeConfig);
                Console.WriteLine("Monitor DLSS5 starts OFF; F6 toggles the existing monitor effects. " +
                    "The flat preset and DLSS feed configuration are preserved.");
            }
            host = StartHost(
                config,
                sessionNonce,
                gameImageSmoke: true,
                nativeStereo: nativeStereo);
            await Task.Delay(750);
            if (host.HasExited)
            {
                throw new InvalidOperationException(
                    $"The x64 game-image host exited early with code {host.ExitCode}.");
            }

            using var game = StartInjectedGame(
                config,
                gameImageSmokeBootstrapPayload: bootstrapPayload);
            Console.WriteLine(
                (portable ? "KOTOR2VR active" : "Game-image smoke active") +
                $" in exact process {game.Id}; host PID {host.Id}, " +
                $"session {sessionNonce:N}. " +
                (nativeStereo ? "VR starts automatically when Link is active; F11 recenters. " :
                    "F7 starts HMD camera capture; F11 recenters. ") +
                "The monitor retains the authored game camera. TCS config is unchanged.");
            Console.WriteLine(
                portable ? "Load a save. F10 / View+Y: camera; F11 / View+A: recenter; F8 / View+D-pad Down: VR UI." :
                nativeStereo ? "Load a save. F10 / View+Y: camera; F11 / View+A: recenter; " +
                    "F8 / View+D-pad Down: VR UI; F9 / View+B: VR DLSS5; F6: monitor effects." :
                    "Load a save, then press F7 once. The HMD camera image should replace the four-color quad.");
            await game.WaitForExitAsync();
            return game.ExitCode;
        }
        finally
        {
            try { StopHost(host); }
            finally
            {
                try { monitorDefaults.Restore(Console.Out); }
                finally { monitorMsaa.Restore(Console.Out); }
            }
        }
    }

    private static void RefuseUnsafeRecoveryState(RecoveryReport recovery)
    {
        if (recovery.StillOwned != 0 || recovery.HasBlockingProblems)
        {
            throw new InvalidOperationException(
                "A TCS override journal is still owned, uncertain, or invalid. " +
                "No game launch is allowed until recovery is unambiguous.");
        }
    }

    private static async Task RequirePassingPreflightAsync(LauncherConfig config)
    {
        var preflightExit = await PreflightRunner.RunAsync(
            config, json: false, CancellationToken.None);
        if (preflightExit == 2)
        {
            Console.Error.WriteLine(
                "WARNING: Preflight reported non-blocking qualification warnings; " +
                "review the listed checks before campaign acceptance.");
            return;
        }
        if (preflightExit != 0)
        {
            throw new InvalidOperationException(
                $"Preflight exit code was {preflightExit}; one or more mandatory gates failed.");
        }
    }

    private static void ValidateSupportedGame(LauncherConfig config)
    {
        if (config.SteamAppId != SupportedSteamAppId)
        {
            throw new InvalidOperationException(
                $"Unsupported Steam app id {config.SteamAppId}. " +
                $"This launcher is pinned to KOTOR II app {SupportedSteamAppId}.");
        }

        RequireFile(config.GameExecutable, "KOTOR II executable");
        var actual = FileHash.Sha256(config.GameExecutable);
        if (!actual.Equals(config.ExpectedSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                $"Unsupported swkotor2.exe. Expected {config.ExpectedSha256}, found {actual}.");
        }
    }

    private static void RequireX86Launcher()
    {
        if (Environment.Is64BitProcess)
        {
            throw new InvalidOperationException(
                "DLL injection into 32-bit KOTOR II requires the win-x86 launcher build. " +
                "This process is 64-bit, so injection was refused.");
        }
    }

    private static void RequireStaticOpenXrLoader(string hostPath)
    {
        var content = File.ReadAllBytes(hostPath);
        if (ContainsAsciiIgnoreCase(content, "openxr_loaderd.dll") ||
            ContainsAsciiIgnoreCase(content, "openxr_loader.dll"))
        {
            throw new InvalidOperationException(
                "The configured host imports a dynamic OpenXR loader. " +
                "Build/package it against the pinned static Khronos loader before launching VR.");
        }
    }

    private static void EnsureGameIsNotAlreadyRunning(string gameExecutable)
    {
        var processName = Path.GetFileNameWithoutExtension(gameExecutable);
        using var candidates = new ProcessCollection(
            Process.GetProcessesByName(processName));
        if (candidates.Count != 0)
        {
            throw new InvalidOperationException(
                $"{processName} is already running. Close it before injection; " +
                "the launcher will never guess which existing process is safe to modify.");
        }
    }

    private static bool ContainsAsciiIgnoreCase(byte[] haystack, string needle)
    {
        var pattern = Encoding.ASCII.GetBytes(needle);
        for (var offset = 0; offset <= haystack.Length - pattern.Length; offset++)
        {
            var matches = true;
            for (var index = 0; index < pattern.Length; index++)
            {
                if (ToLowerAscii(haystack[offset + index]) !=
                    ToLowerAscii(pattern[index]))
                {
                    matches = false;
                    break;
                }
            }
            if (matches)
            {
                return true;
            }
        }
        return false;
    }

    private static byte ToLowerAscii(byte value) =>
        value is >= (byte)'A' and <= (byte)'Z'
            ? (byte)(value + ((byte)'a' - (byte)'A'))
            : value;

    private static Process StartHost(
        LauncherConfig config,
        Guid sessionNonce,
        bool visibleSmoke = false,
        bool gameImageSmoke = false,
        bool nativeStereo = false)
    {
        var start = new ProcessStartInfo
        {
            FileName = config.HostExecutable,
            WorkingDirectory = Path.GetDirectoryName(config.HostExecutable),
            UseShellExecute = false
        };
        // The host validates and records the exact launcher-selected config. Keep
        // this as two ArgumentList entries so spaces cannot change CLI parsing.
        start.ArgumentList.Add("--config");
        start.ArgumentList.Add(config.ConfigPath);
        if (nativeStereo)
        {
            start.ArgumentList.Add("--native-stereo");
        }
        else if (gameImageSmoke)
        {
            start.ArgumentList.Add("--game-image-smoke");
        }
        else if (visibleSmoke)
        {
            start.ArgumentList.Add("--visible-smoke");
        }
        start.Environment["KOTOR2VR_SESSION_NONCE"] = sessionNonce.ToString("N");
        return Process.Start(start) ?? throw new InvalidOperationException(
            "Could not start the x64 host.");
    }

    private static Process StartInjectedGame(
        LauncherConfig config,
        bool renderTrace = false,
        bool renderDoublePass = false,
        byte[]? vrBridgeBootstrapPayload = null,
        byte[]? gameImageSmokeBootstrapPayload = null)
    {
        // Use KPatchCore's runtime injector directly, without its destructive
        // '--patches' install flow. The pinned Steam strategy waits through the
        // bootstrap/decryption stage, injects only after PE validation, and returns
        // the exact Process object it injected.
        var selectedModes = (renderTrace ? 1 : 0) +
            (renderDoublePass ? 1 : 0) +
            (vrBridgeBootstrapPayload is null ? 0 : 1) +
            (gameImageSmokeBootstrapPayload is null ? 0 : 1);
        if (selectedModes > 1)
        {
            throw new ArgumentException(
                "Only one explicit KOTOR2VR bootstrap mode may be requested.");
        }
        var result = gameImageSmokeBootstrapPayload is not null
            ? GameLauncher.LaunchWithRequiredKotor2VrGameImageSmokeBootstrap(
                config.GameExecutable,
                config.GameModule,
                Distribution.Steam,
                gameImageSmokeBootstrapPayload,
                commandLineArgs: null)
            : vrBridgeBootstrapPayload is not null
            ? GameLauncher.LaunchWithRequiredKotor2VrVrBridgeBootstrap(
                config.GameExecutable,
                config.GameModule,
                Distribution.Steam,
                vrBridgeBootstrapPayload,
                commandLineArgs: null)
            : renderDoublePass
            ? GameLauncher.LaunchWithRequiredKotor2VrRenderDoublePassBootstrap(
                config.GameExecutable,
                config.GameModule,
                Distribution.Steam,
                commandLineArgs: null)
            : renderTrace
            ? GameLauncher.LaunchWithRequiredKotor2VrRenderTraceBootstrap(
                config.GameExecutable,
                config.GameModule,
                Distribution.Steam,
                commandLineArgs: null)
            : GameLauncher.LaunchWithRequiredKotor2VrProbeBootstrap(
                config.GameExecutable,
                config.GameModule,
                Distribution.Steam,
                commandLineArgs: null);
        if (!result.Success)
        {
            throw new InvalidOperationException(
                $"Pinned KPatchCore injection failed: {result.Error}");
        }
        if (!result.InjectionPerformed || result.GameProcess is null)
        {
            throw new InvalidOperationException(
                "Pinned KPatchCore did not return an exact injected game process.");
        }
        return result.GameProcess;
    }

    private static void StopHost(Process? host)
    {
        if (host is null)
        {
            return;
        }
        using (host)
        {
            host.Refresh();
            if (host.HasExited)
            {
                return;
            }

            host.CloseMainWindow();
            if (!host.WaitForExit(1500))
            {
                host.Kill(entireProcessTree: true);
                host.WaitForExit(3000);
            }
        }
    }

    private static void RequireFile(string path, string label)
    {
        if (!File.Exists(path))
        {
            throw new FileNotFoundException($"Required {label} is missing.", path);
        }
    }

    private static int Usage(string? error = null)
    {
        if (error is not null)
        {
            Console.Error.WriteLine(error);
        }
        Console.WriteLine(
            "Usage: kotor2vr-launcher " +
            "[preflight|status|launch|probe|trace-render|trace-double-pass|bridge-smoke|game-image-smoke|native-stereo|portable-stereo|restore|self-test] " +
            "[--config PATH] [--json] [--dry-run] " +
            "[--confirm-read-only-probe] [--confirm-render-trace] " +
            "[--confirm-render-double-pass] [--confirm-vr-bridge] " +
            "[--confirm-game-image-smoke]");
        return error is null ? 0 : 2;
    }

    internal sealed record Arguments(
        string Command,
        string ConfigPath,
        bool Json,
        bool DryRun,
        bool ConfirmReadOnlyProbe,
        bool ConfirmRenderTrace,
        bool ConfirmRenderDoublePass,
        bool ConfirmVrBridge,
        bool ConfirmGameImageSmoke)
    {
        public static Arguments Parse(string[] args)
        {
            var command = "preflight";
            var config = Path.GetFullPath(DefaultConfigName);
            var json = false;
            var dryRun = false;
            var confirmReadOnlyProbe = false;
            var confirmRenderTrace = false;
            var confirmRenderDoublePass = false;
            var confirmVrBridge = false;
            var confirmGameImageSmoke = false;
            var commandSet = false;

            for (var i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--config":
                        if (++i >= args.Length)
                        {
                            throw new ArgumentException("--config requires a path.");
                        }
                        config = Path.GetFullPath(args[i]);
                        break;
                    case "--json":
                        json = true;
                        break;
                    case "--dry-run":
                        dryRun = true;
                        break;
                    case "--confirm-read-only-probe":
                        confirmReadOnlyProbe = true;
                        break;
                    case "--confirm-render-trace":
                        confirmRenderTrace = true;
                        break;
                    case "--confirm-render-double-pass":
                        confirmRenderDoublePass = true;
                        break;
                    case "--confirm-vr-bridge":
                        confirmVrBridge = true;
                        break;
                    case "--confirm-game-image-smoke":
                        confirmGameImageSmoke = true;
                        break;
                    case "--help":
                    case "-h":
                        return new Arguments(
                            "help",
                            config,
                            json,
                            dryRun,
                            confirmReadOnlyProbe,
                            confirmRenderTrace,
                            confirmRenderDoublePass,
                            confirmVrBridge,
                            confirmGameImageSmoke);
                    default:
                        if (args[i].StartsWith('-'))
                        {
                            throw new ArgumentException($"Unknown option '{args[i]}'.");
                        }
                        if (commandSet)
                        {
                            throw new ArgumentException($"Unexpected argument '{args[i]}'.");
                        }
                        command = args[i].ToLowerInvariant();
                        commandSet = true;
                        break;
                }
            }
            return new Arguments(
                command,
                config,
                json,
                dryRun,
                confirmReadOnlyProbe,
                confirmRenderTrace,
                confirmRenderDoublePass,
                confirmVrBridge,
                confirmGameImageSmoke);
        }
    }

    private sealed class ProcessCollection : IDisposable
    {
        private readonly Process[] _processes;

        public ProcessCollection(Process[] processes)
        {
            _processes = processes;
        }

        public int Count => _processes.Length;

        public void Dispose()
        {
            foreach (var process in _processes)
            {
                process.Dispose();
            }
        }
    }
}
