using System.Buffers.Binary;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using KPatchCore.Models;

namespace KPatchCore.Launcher;

internal enum Kotor2VrBootstrapKind
{
    Probe,
    RenderTrace,
    RenderDoublePass,
    VrBridge,
    GameImageSmoke
}

internal readonly record struct RemoteBootstrapArgumentDecision(
    bool Valid,
    bool RequiresRemoteAllocation,
    int ByteCount,
    string Failure);

/// <summary>
/// Internal implementation of DLL injection for game processes using Windows API
/// </summary>
internal static class ProcessInjector
{
    private const uint RemoteThreadTimeoutMilliseconds = 30_000;
    private const long MaximumBootstrapModuleBytes = 64L * 1024L * 1024L;
    internal const int VrBridgeBootstrapPayloadSize = 32;
    internal const ushort VrBridgeBootstrapMajor = 1;
    internal const ushort VrBridgeBootstrapMinor = 0;

    internal static RemoteBootstrapArgumentDecision DecideRemoteBootstrapArgument(
        Kotor2VrBootstrapKind? bootstrapKind,
        byte[]? argument)
    {
        if (!bootstrapKind.HasValue)
        {
            return argument is null
                ? new(true, false, 0, string.Empty)
                : new(false, false, 0,
                    "a remote argument was supplied without a bootstrap kind");
        }

        var carriesSessionPayload =
            bootstrapKind.Value == Kotor2VrBootstrapKind.VrBridge ||
            bootstrapKind.Value == Kotor2VrBootstrapKind.GameImageSmoke;
        if (!carriesSessionPayload)
        {
            return argument is null
                ? new(true, false, 0, string.Empty)
                : new(false, false, 0,
                    $"{bootstrapKind.Value} requires the historical null thread argument");
        }

        if (argument is null || argument.Length != VrBridgeBootstrapPayloadSize)
        {
            return new(false, false, 0,
                $"{bootstrapKind.Value} requires exactly " +
                $"{VrBridgeBootstrapPayloadSize} payload bytes");
        }

        var payload = argument.AsSpan();
        var structureSize = BinaryPrimitives.ReadUInt32LittleEndian(payload.Slice(0, 4));
        var major = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(4, 2));
        var minor = BinaryPrimitives.ReadUInt16LittleEndian(payload.Slice(6, 2));
        var low = BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(8, 8));
        var high = BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(16, 8));
        var generation = BinaryPrimitives.ReadUInt64LittleEndian(payload.Slice(24, 8));
        if (structureSize != VrBridgeBootstrapPayloadSize ||
            major != VrBridgeBootstrapMajor ||
            minor != VrBridgeBootstrapMinor)
        {
            return new(false, false, 0,
                $"{bootstrapKind.Value} payload size/version fields do not describe packed v1.0");
        }
        if ((low == 0 && high == 0) || generation == 0)
        {
            return new(false, false, 0,
                $"{bootstrapKind.Value} payload nonce and generation must be nonzero");
        }

        return new(true, true, VrBridgeBootstrapPayloadSize, string.Empty);
    }

    /// <summary>
    /// Launches a game executable with DLL injection
    /// </summary>
    /// <param name="gameExePath">Path to the game executable</param>
    /// <param name="dllPath">Path to the DLL to inject</param>
    /// <param name="commandLineArgs">Optional command line arguments for the game</param>
    /// <param name="distribution">Game distribution (GOG, Steam, etc.) to determine injection method</param>
    /// <returns>Launch result containing Process object or error</returns>
    internal static LaunchResult LaunchWithInjection(
        string gameExePath,
        string dllPath,
        string? commandLineArgs,
        Distribution distribution,
        Kotor2VrBootstrapKind? requiredKotor2VrBootstrap = null,
        byte[]? requiredKotor2VrBootstrapArgument = null)
    {
        if (!File.Exists(gameExePath))
        {
            return LaunchResult.Fail($"Game executable not found: {gameExePath}");
        }

        if (!File.Exists(dllPath))
        {
            return LaunchResult.Fail($"DLL not found: {dllPath}");
        }

        var argumentDecision = DecideRemoteBootstrapArgument(
            requiredKotor2VrBootstrap,
            requiredKotor2VrBootstrapArgument);
        if (!argumentDecision.Valid)
        {
            return LaunchResult.Fail(
                $"Invalid KOTOR2VR bootstrap argument contract: {argumentDecision.Failure}");
        }
        var frozenBootstrapArgument = requiredKotor2VrBootstrapArgument is null
            ? null
            : (byte[])requiredKotor2VrBootstrapArgument.Clone();

        if (distribution == Distribution.Steam)
        {
            Console.WriteLine("[KPatchCore] Detected Steam distribution, using delayed injection method");
            return LaunchSteamWithInjection(
                gameExePath,
                dllPath,
                commandLineArgs,
                requiredKotor2VrBootstrap,
                frozenBootstrapArgument);
        }
        else
        {
            return LaunchDirectWithInjection(
                gameExePath,
                dllPath,
                commandLineArgs,
                requiredKotor2VrBootstrap,
                frozenBootstrapArgument);
        }
    }

    /// <summary>
    /// Launches a game executable with direct DLL injection (GOG/Physical/Other distributions)
    /// Uses CREATE_SUSPENDED to inject before the game starts
    /// </summary>
    private static LaunchResult LaunchDirectWithInjection(
        string gameExePath,
        string dllPath,
        string? commandLineArgs,
        Kotor2VrBootstrapKind? requiredKotor2VrBootstrap,
        byte[]? requiredKotor2VrBootstrapArgument)
    {
        try
        {
            var absGamePath = Path.GetFullPath(gameExePath);
            var absDllPath = Path.GetFullPath(dllPath);

            var si = new Win32.STARTUPINFO
            {
                cb = Marshal.SizeOf(typeof(Win32.STARTUPINFO))
            };

            var pi = new Win32.PROCESS_INFORMATION();

            var commandLine = $"\"{absGamePath}\"";
            if (!string.IsNullOrWhiteSpace(commandLineArgs))
            {
                commandLine += $" {commandLineArgs}";
            }

            // Create the process suspended
            var success = Win32.CreateProcess(
                absGamePath,
                commandLine,
                IntPtr.Zero,
                IntPtr.Zero,
                false,
                Win32.CREATE_SUSPENDED,
                IntPtr.Zero,
                Path.GetDirectoryName(absGamePath),
                ref si,
                out pi);

            if (!success)
            {
                var error = Marshal.GetLastWin32Error();
                return LaunchResult.Fail(
                    $"Failed to create process (error {error}): {gameExePath}");
            }

            try
            {
                // Inject the DLL into the suspended process
                var injectResult = InjectDllIntoProcess(
                    pi.hProcess,
                    absDllPath,
                    pi.dwProcessId,
                    requiredKotor2VrBootstrap,
                    requiredKotor2VrBootstrapArgument);

                if (!injectResult.Success)
                {
                    Process.GetProcessById(pi.dwProcessId).Kill();
                    return LaunchResult.Fail(
                        $"DLL injection failed: {injectResult.Error}");
                }

                // Debug mode: Set to 'true' if you want to hook a debugger to the process
                if (false)
                {
                    Console.WriteLine("========================================");
                    Console.WriteLine("DEBUG MODE ENABLED");
                    Console.WriteLine($"Game process created (PID: {pi.dwProcessId})");
                    Console.WriteLine("Process is SUSPENDED - DLL has been injected");
                    Console.WriteLine("");
                    Console.WriteLine("You can now:");
                    Console.WriteLine("  1. Attach your debugger (Cheat Engine, x32dbg, etc.)");
                    Console.WriteLine("  2. Set breakpoints in KotorPatcher.dll or game code");
                    Console.WriteLine("  3. Press ENTER to resume the game");
                    Console.WriteLine("========================================");
                    Console.ReadLine();
                    Console.WriteLine("[DEBUG] Resuming game process...");
                }

                // Resume the main thread
                var resumeResult = Win32.ResumeThread(pi.hThread);
                if (resumeResult == unchecked((uint)-1))
                {
                    var error = Marshal.GetLastWin32Error();
                    Process.GetProcessById(pi.dwProcessId).Kill();
                    return LaunchResult.Fail(
                        $"Failed to resume thread (error {error})");
                }

                var process = Process.GetProcessById(pi.dwProcessId);

                return LaunchResult.Ok(
                    process,
                    injectionPerformed: true,
                    $"Successfully launched {Path.GetFileName(gameExePath)} with DLL injection");
            }
            finally
            {
                if (pi.hProcess != IntPtr.Zero) Win32.CloseHandle(pi.hProcess);
                if (pi.hThread != IntPtr.Zero) Win32.CloseHandle(pi.hThread);
            }
        }
        catch (Exception ex)
        {
            return LaunchResult.Fail($"Launch failed: {ex.Message}");
        }
    }

    /// <summary>
    /// Injects a DLL into a running process
    /// </summary>
    /// <param name="process">Previously discovered target process</param>
    /// <param name="dllPath">Path to the DLL to inject</param>
    /// <param name="expectedGamePath">Exact expected main-module path</param>
    /// <param name="launchNotBeforeUtc">Earliest allowed process start time</param>
    /// <param name="preexistingProcessIds">PIDs present before Steam was invoked</param>
    /// <returns>Result indicating success or failure</returns>
    private static PatchResult InjectIntoRunningProcess(
        Process process,
        string dllPath,
        string expectedGamePath,
        DateTime launchNotBeforeUtc,
        IReadOnlySet<int> preexistingProcessIds,
        Kotor2VrBootstrapKind? requiredKotor2VrBootstrap,
        byte[]? requiredKotor2VrBootstrapArgument)
    {
        if (!File.Exists(dllPath))
        {
            return PatchResult.Fail($"DLL not found: {dllPath}");
        }

        try
        {
            // Repeat every identity and image check immediately before opening the
            // injection handle. A process found earlier is not trusted merely because
            // it retained the same PID while Steam finished initialization.
            if (!IsValidGameProcess(
                    process,
                    expectedGamePath,
                    launchNotBeforeUtc,
                    preexistingProcessIds,
                    out var validationError))
            {
                return PatchResult.Fail(
                    $"Process {process.Id} failed final identity validation: {validationError}");
            }

            var processId = process.Id;
            // Open the target process
            var hProcess = Win32.OpenProcess(
                Win32.PROCESS_CREATE_THREAD | Win32.PROCESS_QUERY_INFORMATION |
                Win32.PROCESS_VM_OPERATION | Win32.PROCESS_VM_WRITE | Win32.PROCESS_VM_READ,
                false,
                processId);

            if (hProcess == IntPtr.Zero)
            {
                var error = Marshal.GetLastWin32Error();
                return PatchResult.Fail($"Failed to open process {processId} (error {error})");
            }

            try
            {
                // The PID can exit and be reused between the Process-object check
                // above and OpenProcess. Pin every final identity decision to this
                // opened handle, including creation time, paths, image base, and PE.
                if (!TryValidateOpenedGameProcess(
                        hProcess,
                        processId,
                        expectedGamePath,
                        launchNotBeforeUtc,
                        preexistingProcessIds,
                        out _,
                        out var handleValidationError))
                {
                    return PatchResult.Fail(
                        $"Process {processId} failed opened-handle identity validation: " +
                        handleValidationError);
                }

                return InjectDllIntoProcess(
                    hProcess,
                    Path.GetFullPath(dllPath),
                    processId,
                    requiredKotor2VrBootstrap,
                    requiredKotor2VrBootstrapArgument);
            }
            finally
            {
                Win32.CloseHandle(hProcess);
            }
        }
        catch (Exception ex)
        {
            return PatchResult.Fail($"Injection failed: {ex.Message}");
        }
    }

    /// <summary>
    /// Internal method to inject DLL into an open process handle
    /// </summary>
    private static PatchResult InjectDllIntoProcess(
        IntPtr hProcess,
        string dllPath,
        int targetProcessId,
        Kotor2VrBootstrapKind? requiredKotor2VrBootstrap,
        byte[]? requiredKotor2VrBootstrapArgument)
    {
        FileStream? lockedBootstrapModule = null;
        try
        {
            Console.WriteLine($"[Injector] Injecting: {dllPath}");

            ProbeLogCursor probeLogCursor = default;
            var bootstrapKindForWitness =
                requiredKotor2VrBootstrap.GetValueOrDefault();
            var requiresFreshLogWitness =
                requiredKotor2VrBootstrap.HasValue &&
                bootstrapKindForWitness !=
                    Kotor2VrBootstrapKind.GameImageSmoke;
            if (requiresFreshLogWitness &&
                !TryCaptureBootstrapLogCursor(
                    bootstrapKindForWitness,
                    out probeLogCursor,
                    out var cursorError))
            {
                return PatchResult.Fail(
                    $"Could not establish a fresh probe-log witness boundary: {cursorError}");
            }

            byte[]? localBootstrapImage = null;
            if (requiredKotor2VrBootstrap.HasValue)
            {
                // Hold a deny-write/delete handle from before LoadLibrary until the
                // remote export returns. The parsed contract therefore describes the
                // immutable path contents that the target was asked to load.
                lockedBootstrapModule = new FileStream(
                    dllPath,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.Read,
                    bufferSize: 4096,
                    FileOptions.SequentialScan);
                if (lockedBootstrapModule.Length <= 0 ||
                    lockedBootstrapModule.Length > MaximumBootstrapModuleBytes)
                {
                    return PatchResult.Fail(
                        $"Local game module size {lockedBootstrapModule.Length} is " +
                        "outside the accepted range");
                }
                localBootstrapImage =
                    new byte[checked((int)lockedBootstrapModule.Length)];
                lockedBootstrapModule.ReadExactly(localBootstrapImage);
            }

            var hKernel32 = Win32.GetModuleHandle("kernel32.dll");
            if (hKernel32 == IntPtr.Zero)
            {
                var msg = "Failed to get kernel32.dll module handle";
                Console.WriteLine($"[Injector] ERROR: {msg}");
                return PatchResult.Fail(msg);
            }
            Console.WriteLine($"[Injector] kernel32.dll handle: 0x{hKernel32:X}");

            var pLoadLibraryA = Win32.GetProcAddress(hKernel32, "LoadLibraryA");
            if (pLoadLibraryA == IntPtr.Zero)
            {
                var msg = "Failed to get LoadLibraryA address";
                Console.WriteLine($"[Injector] ERROR: {msg}");
                return PatchResult.Fail(msg);
            }
            Console.WriteLine($"[Injector] LoadLibraryA address: 0x{pLoadLibraryA:X}");

            // Allocate memory in the target process for the DLL path
            var dllPathBytes = Encoding.ASCII.GetBytes(dllPath + '\0');
            Console.WriteLine($"[Injector] Allocating {dllPathBytes.Length} bytes in target process...");

            var pDllPath = Win32.VirtualAllocEx(
                hProcess,
                IntPtr.Zero,
                (uint)dllPathBytes.Length,
                Win32.MEM_COMMIT | Win32.MEM_RESERVE,
                Win32.PAGE_READWRITE);

            if (pDllPath == IntPtr.Zero)
            {
                var error = Marshal.GetLastWin32Error();
                var msg = $"Failed to allocate memory in target process (error {error})";
                Console.WriteLine($"[Injector] ERROR: {msg}");
                return PatchResult.Fail(msg);
            }
            Console.WriteLine($"[Injector] Allocated memory at: 0x{pDllPath:X}");

            var remoteThreadMayUseBuffer = false;
            try
            {
                // Write the DLL path to the allocated memory
                Console.WriteLine($"[Injector] Writing DLL path to target process memory...");
                var writeSuccess = Win32.WriteProcessMemory(
                    hProcess,
                    pDllPath,
                    dllPathBytes,
                    (uint)dllPathBytes.Length,
                    out var bytesWritten);

                if (!writeSuccess || bytesWritten.ToUInt32() != dllPathBytes.Length)
                {
                    var error = Marshal.GetLastWin32Error();
                    var msg = $"Failed to write DLL path to target process (error {error})";
                    Console.WriteLine($"[Injector] ERROR: {msg}");
                    return PatchResult.Fail(msg);
                }
                Console.WriteLine($"[Injector] Wrote {bytesWritten} bytes successfully");

                // Create a remote thread that calls LoadLibraryA with the DLL path
                Console.WriteLine($"[Injector] Creating remote thread to call LoadLibraryA...");
                var hThread = Win32.CreateRemoteThread(
                    hProcess,
                    IntPtr.Zero,
                    0,
                    pLoadLibraryA,
                    pDllPath,
                    0,
                    out var threadId);

                if (hThread == IntPtr.Zero)
                {
                    var error = Marshal.GetLastWin32Error();
                    var msg = $"Failed to create remote thread (error {error})";
                    Console.WriteLine($"[Injector] ERROR: {msg}");
                    return PatchResult.Fail(msg);
                }
                remoteThreadMayUseBuffer = true;
                Console.WriteLine($"[Injector] Remote thread created with ID: {threadId}");

                uint waitResult;
                uint remoteThreadExitCode = 0;
                var exitCodeRead = false;
                var exitCodeError = 0;
                try
                {
                    // Wait for the thread to complete (LoadLibrary to finish)
                    Console.WriteLine($"[Injector] Waiting for remote thread to complete...");
                    waitResult = Win32.WaitForSingleObject(
                        hThread,
                        RemoteThreadTimeoutMilliseconds);
                    if (waitResult == 0)
                    {
                        remoteThreadMayUseBuffer = false;
                        exitCodeRead = Win32.GetExitCodeThread(
                            hThread,
                            out remoteThreadExitCode);
                        if (!exitCodeRead)
                        {
                            exitCodeError = Marshal.GetLastWin32Error();
                        }
                    }
                }
                finally
                {
                    Win32.CloseHandle(hThread);
                }

                if (waitResult != 0) // 0 = WAIT_OBJECT_0
                {
                    var msg = $"Remote thread did not complete successfully (wait result: {waitResult})";
                    Console.WriteLine($"[Injector] ERROR: {msg}");
                    return PatchResult.Fail(msg);
                }
                if (!exitCodeRead)
                {
                    var msg =
                        $"Could not read LoadLibraryA thread exit code (error {exitCodeError})";
                    Console.WriteLine($"[Injector] ERROR: {msg}");
                    return PatchResult.Fail(msg);
                }
                if (remoteThreadExitCode == 0)
                {
                    const string msg =
                        "LoadLibraryA returned null; the target did not load the DLL";
                    Console.WriteLine($"[Injector] ERROR: {msg}");
                    return PatchResult.Fail(msg);
                }

                if (requiredKotor2VrBootstrap.HasValue)
                {
                    var bootstrapResult = InvokeRemoteProbeBootstrap(
                        hProcess,
                        dllPath,
                        remoteThreadExitCode,
                        localBootstrapImage!,
                        requiredKotor2VrBootstrap.Value,
                        requiredKotor2VrBootstrapArgument);
                    if (!bootstrapResult.Success)
                    {
                        return bootstrapResult;
                    }
                    if (requiresFreshLogWitness &&
                        !TryVerifyFreshProbeLogWitness(
                            probeLogCursor,
                            targetProcessId,
                            requiredKotor2VrBootstrap.Value,
                            out var witnessError))
                    {
                        return PatchResult.Fail(
                            $"Remote bootstrap returned success without a fresh persistent " +
                            $"target-process witness: {witnessError}");
                    }

                    if (requiredKotor2VrBootstrap.Value ==
                        Kotor2VrBootstrapKind.GameImageSmoke)
                    {
                        // This diagnostic publishes its useful proof only after
                        // F7: a session-nonce-bound shared image mapping consumed
                        // by the already-running host. A LOCALAPPDATA log cursor
                        // is not a reliable cross-process witness when Steam gives
                        // the game a different environment path. Remote module,
                        // export, payload, and zero exit-code validation above all
                        // remain mandatory.
                        Console.WriteLine(
                            "[Injector] GameImageSmoke bootstrap accepted; " +
                            "the nonce-bound F7 image mapping is its runtime witness");
                    }

                    Console.WriteLine(
                        $"[Injector] SUCCESS: DLL loaded and explicit " +
                        $"{requiredKotor2VrBootstrap.Value} bootstrap acknowledged");
                    return PatchResult.Ok(
                        $"Successfully loaded and bootstrapped {Path.GetFileName(dllPath)}");
                }

                Console.WriteLine("[Injector] SUCCESS: DLL loaded");
                return PatchResult.Ok(
                    $"Successfully loaded {Path.GetFileName(dllPath)}");
            }
            finally
            {
                // Never free a buffer that an incompletely observed remote thread may
                // still read. Successful waits and all pre-thread failures are safe.
                if (!remoteThreadMayUseBuffer &&
                    !Win32.VirtualFreeEx(
                        hProcess,
                        pDllPath,
                        UIntPtr.Zero,
                        Win32.MEM_RELEASE))
                {
                    Console.WriteLine(
                        "[Injector] WARNING: Could not release the remote DLL-path buffer.");
                }
            }
        }
        catch (Exception ex)
        {
            var msg = $"DLL injection failed: {ex.Message}";
            Console.WriteLine($"[Injector] EXCEPTION: {msg}");
            return PatchResult.Fail(msg);
        }
        finally
        {
            lockedBootstrapModule?.Dispose();
        }
    }

    private static bool TryCaptureBootstrapLogCursor(
        Kotor2VrBootstrapKind bootstrapKind,
        out ProbeLogCursor cursor,
        out string failure)
    {
        cursor = default;
        try
        {
            var localAppData = Environment.GetFolderPath(
                Environment.SpecialFolder.LocalApplicationData);
            if (string.IsNullOrWhiteSpace(localAppData))
            {
                failure = "LOCALAPPDATA is unavailable";
                return false;
            }

            var fileName = bootstrapKind switch
            {
                Kotor2VrBootstrapKind.Probe => "game32-probe.log",
                Kotor2VrBootstrapKind.RenderTrace => "render-trace.jsonl",
                Kotor2VrBootstrapKind.RenderDoublePass => "render-trace.jsonl",
                Kotor2VrBootstrapKind.VrBridge => "vr-bridge.log",
                Kotor2VrBootstrapKind.GameImageSmoke => "game32-probe.log",
                _ => string.Empty
            };
            if (fileName.Length == 0)
            {
                failure = $"unknown KOTOR2VR bootstrap kind {bootstrapKind}";
                return false;
            }
            var path = Path.Combine(
                localAppData,
                "Kotor2VR",
                "logs",
                fileName);
            var length = File.Exists(path) ? new FileInfo(path).Length : 0L;
            cursor = new ProbeLogCursor(path, length);
            failure = string.Empty;
            return true;
        }
        catch (Exception ex)
        {
            failure = ex.Message;
            return false;
        }
    }

    private static bool TryVerifyFreshProbeLogWitness(
        ProbeLogCursor cursor,
        int targetProcessId,
        Kotor2VrBootstrapKind bootstrapKind,
        out string failure)
    {
        const long maximumFreshWitnessBytes = 4L * 1024L * 1024L;
        try
        {
            using var stream = new FileStream(
                cursor.Path,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite);
            var finalLength = stream.Length;
            if (finalLength <= cursor.Length)
            {
                failure = "probe log did not grow after injection";
                return false;
            }

            var appendedLength = finalLength - cursor.Length;
            if (appendedLength > maximumFreshWitnessBytes)
            {
                failure = $"fresh probe-log region is unexpectedly large ({appendedLength} bytes)";
                return false;
            }

            stream.Position = cursor.Length;
            var appendedBytes = new byte[checked((int)appendedLength)];
            var totalRead = 0;
            while (totalRead < appendedBytes.Length)
            {
                var bytesRead = stream.Read(
                    appendedBytes,
                    totalRead,
                    appendedBytes.Length - totalRead);
                if (bytesRead == 0)
                {
                    failure = "fresh probe-log region was truncated while reading";
                    return false;
                }
                totalRead += bytesRead;
            }

            var appendedText = Encoding.UTF8.GetString(appendedBytes);
            return bootstrapKind switch
            {
                Kotor2VrBootstrapKind.Probe =>
                    SteamProcessValidation.HasFreshProbeBootstrapWitness(
                        appendedText,
                        targetProcessId,
                        out failure),
                Kotor2VrBootstrapKind.RenderTrace =>
                    SteamProcessValidation.HasFreshRenderTraceBootstrapWitness(
                        appendedText,
                        targetProcessId,
                        out failure),
                Kotor2VrBootstrapKind.RenderDoublePass =>
                    SteamProcessValidation.HasFreshRenderDoublePassBootstrapWitness(
                        appendedText,
                        targetProcessId,
                        out failure),
                Kotor2VrBootstrapKind.VrBridge =>
                    SteamProcessValidation.HasFreshVrBridgeBootstrapWitness(
                        appendedText,
                        targetProcessId,
                        out failure),
                Kotor2VrBootstrapKind.GameImageSmoke =>
                    SteamProcessValidation.HasFreshGameImageSmokeBootstrapWitness(
                        appendedText,
                        targetProcessId,
                        out failure),
                _ => FailUnknownBootstrapKind(bootstrapKind, out failure)
            };
        }
        catch (Exception ex)
        {
            failure = ex.Message;
            return false;
        }
    }

    private static bool FailUnknownBootstrapKind(
        Kotor2VrBootstrapKind bootstrapKind,
        out string failure)
    {
        failure = $"unknown KOTOR2VR bootstrap kind {bootstrapKind}";
        return false;
    }

    private static PatchResult InvokeRemoteProbeBootstrap(
        IntPtr hProcess,
        string dllPath,
        uint remoteModuleBase,
        byte[] localImage,
        Kotor2VrBootstrapKind bootstrapKind,
        byte[]? bootstrapArgument)
    {
        var argumentDecision = DecideRemoteBootstrapArgument(
            bootstrapKind,
            bootstrapArgument);
        if (!argumentDecision.Valid)
        {
            return PatchResult.Fail(
                $"Invalid KOTOR2VR bootstrap argument contract: {argumentDecision.Failure}");
        }

        var bootstrapExportName = bootstrapKind switch
        {
            Kotor2VrBootstrapKind.Probe =>
                SteamProcessValidation.ProbeBootstrapExportName,
            Kotor2VrBootstrapKind.RenderTrace =>
                SteamProcessValidation.RenderTraceBootstrapExportName,
            Kotor2VrBootstrapKind.RenderDoublePass =>
                SteamProcessValidation.RenderDoublePassBootstrapExportName,
            Kotor2VrBootstrapKind.VrBridge =>
                SteamProcessValidation.VrBridgeBootstrapExportName,
            Kotor2VrBootstrapKind.GameImageSmoke =>
                SteamProcessValidation.GameImageSmokeBootstrapExportName,
            _ => string.Empty
        };
        if (bootstrapExportName.Length == 0)
        {
            return PatchResult.Fail($"Unknown KOTOR2VR bootstrap kind {bootstrapKind}");
        }
        if (!SteamProcessValidation.TryFindPe32ExportRva(
                localImage,
                bootstrapExportName,
                out var bootstrapRva,
                out var exportError))
        {
            return PatchResult.Fail(
                $"Local game module bootstrap export is invalid: {exportError}");
        }
        if (!SteamProcessValidation.TryGetPe32ImageIdentity(
                localImage,
                out var localIdentity,
                out var identityError))
        {
            return PatchResult.Fail(
                $"Local game module PE identity is invalid: {identityError}");
        }
        if (!TryValidateRemoteBootstrapModule(
                hProcess,
                dllPath,
                remoteModuleBase,
                localIdentity,
                bootstrapExportName,
                bootstrapRva,
                out var remoteBootstrapAddress,
                out var remoteBootstrapAddressValue,
                out var remoteValidationError))
        {
            return PatchResult.Fail(
                $"Remote game module validation failed closed: {remoteValidationError}");
        }
        Console.WriteLine(
            $"[Injector] Calling {bootstrapExportName} " +
            $"at remote 0x{remoteBootstrapAddressValue:X8} " +
            $"(module 0x{remoteModuleBase:X8} + RVA 0x{bootstrapRva:X8})");

        var remoteArgument = IntPtr.Zero;
        var remoteThreadMayUseArgument = false;
        var remoteArgumentReleaseAttempted = false;
        try
        {
            if (argumentDecision.RequiresRemoteAllocation)
            {
                remoteArgument = Win32.VirtualAllocEx(
                    hProcess,
                    IntPtr.Zero,
                    checked((uint)argumentDecision.ByteCount),
                    Win32.MEM_COMMIT | Win32.MEM_RESERVE,
                    Win32.PAGE_READWRITE);
                if (remoteArgument == IntPtr.Zero)
                {
                    return PatchResult.Fail(
                        $"Could not allocate the remote {bootstrapKind} argument " +
                        $"(error {Marshal.GetLastWin32Error()})");
                }

                var writeSucceeded = Win32.WriteProcessMemory(
                    hProcess,
                    remoteArgument,
                    bootstrapArgument!,
                    checked((uint)argumentDecision.ByteCount),
                    out var bytesWritten);
                if (!writeSucceeded ||
                    bytesWritten.ToUInt64() != (ulong)argumentDecision.ByteCount)
                {
                    return PatchResult.Fail(
                        $"Could not write the exact remote {bootstrapKind} argument " +
                        $"(error {Marshal.GetLastWin32Error()}, wrote {bytesWritten} bytes)");
                }
                Console.WriteLine(
                    $"[Injector] Wrote exact {argumentDecision.ByteCount}-byte " +
                    $"{bootstrapKind} remote argument at 0x{UnsignedAddress(remoteArgument):X}");
            }

            var bootstrapThread = Win32.CreateRemoteThread(
                hProcess,
                IntPtr.Zero,
                0,
                remoteBootstrapAddress,
                remoteArgument,
                0,
                out var bootstrapThreadId);
            if (bootstrapThread == IntPtr.Zero)
            {
                var error = Marshal.GetLastWin32Error();
                return PatchResult.Fail(
                    $"Could not create the remote {bootstrapKind} bootstrap thread " +
                    $"(error {error})");
            }
            remoteThreadMayUseArgument = remoteArgument != IntPtr.Zero;
            Console.WriteLine(
                $"[Injector] {bootstrapKind} bootstrap thread created with ID: " +
                $"{bootstrapThreadId}");

            uint waitResult;
            uint bootstrapExitCode = uint.MaxValue;
            var exitCodeRead = false;
            var exitCodeError = 0;
            try
            {
                waitResult = Win32.WaitForSingleObject(
                    bootstrapThread,
                    RemoteThreadTimeoutMilliseconds);
                if (waitResult == 0)
                {
                    remoteThreadMayUseArgument = false;
                    exitCodeRead = Win32.GetExitCodeThread(
                        bootstrapThread,
                        out bootstrapExitCode);
                    if (!exitCodeRead)
                    {
                        exitCodeError = Marshal.GetLastWin32Error();
                    }
                }
            }
            finally
            {
                Win32.CloseHandle(bootstrapThread);
            }

            if (waitResult != 0)
            {
                return PatchResult.Fail(
                    $"Remote {bootstrapKind} bootstrap thread did not complete " +
                    $"(wait result {waitResult})");
            }

            if (remoteArgument != IntPtr.Zero)
            {
                remoteArgumentReleaseAttempted = true;
                if (!Win32.VirtualFreeEx(
                        hProcess,
                        remoteArgument,
                        UIntPtr.Zero,
                        Win32.MEM_RELEASE))
                {
                    return PatchResult.Fail(
                        $"Remote {bootstrapKind} bootstrap returned, but its argument " +
                        $"could not be released safely (error {Marshal.GetLastWin32Error()})");
                }
                remoteArgument = IntPtr.Zero;
            }

            if (!exitCodeRead)
            {
                return PatchResult.Fail(
                    $"Could not read {bootstrapKind} bootstrap exit code " +
                    $"(error {exitCodeError})");
            }
            if (bootstrapExitCode != 0)
            {
                return PatchResult.Fail(
                    $"{bootstrapKind} bootstrap failed closed with code " +
                    $"{bootstrapExitCode} " +
                    $"({DescribeBootstrapExitCode(bootstrapKind, bootstrapExitCode)})");
            }

            return PatchResult.Ok(
                $"Explicit remote {bootstrapKind} bootstrap returned success");
        }
        finally
        {
            // Once a remote thread exists, retain the payload unless its return was
            // observed. The export copies it synchronously, so WAIT_OBJECT_0 makes
            // the exact allocation safe to release.
            if (remoteArgument != IntPtr.Zero &&
                !remoteThreadMayUseArgument &&
                !remoteArgumentReleaseAttempted &&
                !Win32.VirtualFreeEx(
                    hProcess,
                    remoteArgument,
                    UIntPtr.Zero,
                    Win32.MEM_RELEASE))
            {
                Console.WriteLine(
                    $"[Injector] WARNING: Could not release the remote " +
                    $"{bootstrapKind} argument buffer.");
            }
        }
    }

    private static bool TryValidateRemoteBootstrapModule(
        IntPtr hProcess,
        string expectedDllPath,
        uint remoteModuleBase,
        SteamProcessValidation.Pe32ImageIdentity expectedIdentity,
        string bootstrapExportName,
        uint bootstrapRva,
        out IntPtr remoteBootstrapAddress,
        out uint remoteBootstrapAddressValue,
        out string failure)
    {
        remoteBootstrapAddress = IntPtr.Zero;
        remoteBootstrapAddressValue = 0;
        try
        {
            if (remoteModuleBase == 0)
            {
                failure = "LoadLibrary returned a null module base";
                return false;
            }

            var moduleBaseAddress = UInt32Address(remoteModuleBase);
            const int maximumPathCharacters = 32768;
            var remoteModulePath = new StringBuilder(maximumPathCharacters);
            var remoteModulePathLength = Win32.K32GetModuleFileNameEx(
                hProcess,
                moduleBaseAddress,
                remoteModulePath,
                (uint)remoteModulePath.Capacity);
            if (remoteModulePathLength == 0 ||
                remoteModulePathLength >= remoteModulePath.Capacity - 1)
            {
                failure =
                    $"could not resolve the loaded remote module path " +
                    $"(error {Marshal.GetLastWin32Error()})";
                return false;
            }

            var actualPath = SteamProcessValidation.CanonicalizeExecutablePath(
                remoteModulePath.ToString());
            var expectedPath = SteamProcessValidation.CanonicalizeExecutablePath(
                expectedDllPath);
            if (!actualPath.Equals(expectedPath, StringComparison.OrdinalIgnoreCase))
            {
                failure =
                    $"loaded module path mismatch (expected '{expectedPath}', " +
                    $"found '{actualPath}')";
                return false;
            }

            var dosHeader = new byte[SteamProcessValidation.DosHeaderSize];
            if (!ReadProcessMemoryExactly(hProcess, moduleBaseAddress, dosHeader))
            {
                failure = "could not read the remote module DOS header";
                return false;
            }
            if (!SteamProcessValidation.TryGetPeHeaderOffset(
                    dosHeader,
                    out var peOffset,
                    out failure))
            {
                return false;
            }

            if (!SteamProcessValidation.TryAddRemoteModuleRva(
                    remoteModuleBase,
                    (uint)peOffset,
                    out var remoteNtHeaderValue,
                    out failure))
            {
                return false;
            }
            var remoteNtHeader = UInt32Address(remoteNtHeaderValue);
            var ntHeaderPrefix = new byte[SteamProcessValidation.NtHeaderPrefixSize];
            if (!ReadProcessMemoryExactly(hProcess, remoteNtHeader, ntHeaderPrefix))
            {
                failure = "could not read the remote module NT/COFF header prefix";
                return false;
            }
            if (!SteamProcessValidation.IsPe32NtHeader(ntHeaderPrefix, out failure))
            {
                return false;
            }

            var optionalHeaderSize = BinaryPrimitives.ReadUInt16LittleEndian(
                ntHeaderPrefix.AsSpan(20, 2));
            var remoteHeaderByteCount = checked(peOffset + 24 + optionalHeaderSize);
            if (remoteHeaderByteCount <= 0 ||
                (uint)remoteHeaderByteCount > expectedIdentity.SizeOfHeaders ||
                remoteHeaderByteCount > MaximumBootstrapModuleBytes)
            {
                failure =
                    $"remote PE header span {remoteHeaderByteCount} exceeds expected bounds";
                return false;
            }

            var remoteHeaders = new byte[remoteHeaderByteCount];
            if (!ReadProcessMemoryExactly(hProcess, moduleBaseAddress, remoteHeaders))
            {
                failure = "could not read the complete remote PE identity";
                return false;
            }
            if (!SteamProcessValidation.TryGetPe32ImageIdentity(
                    remoteHeaders,
                    out var remoteIdentity,
                    out failure))
            {
                return false;
            }
            // The Windows loader and compatibility layers may normalize the
            // preferred ImageBase or checksum in the mapped header. Bind to the
            // load-stable COFF/optional-header identity instead; the exact export
            // code bytes and MEM_IMAGE ownership are checked immediately below.
            var sameLoadStableIdentity =
                remoteIdentity.SectionCount == expectedIdentity.SectionCount &&
                remoteIdentity.OptionalHeaderSize == expectedIdentity.OptionalHeaderSize &&
                remoteIdentity.TimeDateStamp == expectedIdentity.TimeDateStamp &&
                remoteIdentity.AddressOfEntryPoint == expectedIdentity.AddressOfEntryPoint &&
                remoteIdentity.SizeOfImage == expectedIdentity.SizeOfImage &&
                remoteIdentity.SizeOfHeaders == expectedIdentity.SizeOfHeaders &&
                remoteIdentity.ExportDirectoryRva == expectedIdentity.ExportDirectoryRva &&
                remoteIdentity.ExportDirectorySize == expectedIdentity.ExportDirectorySize;
            if (!sameLoadStableIdentity)
            {
                failure =
                    $"remote PE identity differs from the locked local module " +
                    $"(expected SizeOfImage=0x{expectedIdentity.SizeOfImage:X8}, " +
                    $"remote=0x{remoteIdentity.SizeOfImage:X8}, " +
                    $"expected timestamp=0x{expectedIdentity.TimeDateStamp:X8}, " +
                    $"remote=0x{remoteIdentity.TimeDateStamp:X8}, " +
                    $"expected export=0x{expectedIdentity.ExportDirectoryRva:X8}/" +
                    $"0x{expectedIdentity.ExportDirectorySize:X8}, " +
                    $"remote=0x{remoteIdentity.ExportDirectoryRva:X8}/" +
                    $"0x{remoteIdentity.ExportDirectorySize:X8})";
                return false;
            }

            if (!TryResolveRemotePe32ExportRva(
                    hProcess,
                    remoteModuleBase,
                    remoteIdentity,
                    bootstrapExportName,
                    out var remoteExportRva,
                    out failure))
            {
                return false;
            }
            if (remoteExportRva != bootstrapRva)
            {
                failure =
                    $"remote export RVA 0x{remoteExportRva:X8} differs from " +
                    $"locked local RVA 0x{bootstrapRva:X8}";
                return false;
            }

            if (bootstrapRva >= expectedIdentity.SizeOfImage ||
                !SteamProcessValidation.TryAddRemoteModuleRva(
                    remoteModuleBase,
                    bootstrapRva,
                    out remoteBootstrapAddressValue,
                    out failure))
            {
                return false;
            }
            remoteBootstrapAddress = UInt32Address(remoteBootstrapAddressValue);

            var querySize = (uint)Marshal.SizeOf<Win32.MEMORY_BASIC_INFORMATION>();
            var queried = Win32.VirtualQueryEx(
                hProcess,
                remoteBootstrapAddress,
                out var memory,
                new UIntPtr(querySize));
            if (queried == UIntPtr.Zero || queried.ToUInt64() < querySize)
            {
                failure =
                    $"VirtualQueryEx failed for the remote bootstrap target " +
                    $"(error {Marshal.GetLastWin32Error()})";
                return false;
            }

            var protection = memory.Protect & 0xffU;
            var executable = protection is Win32.PAGE_EXECUTE or
                Win32.PAGE_EXECUTE_READ or
                Win32.PAGE_EXECUTE_READWRITE or
                Win32.PAGE_EXECUTE_WRITECOPY;
            if (memory.AllocationBase != moduleBaseAddress ||
                memory.State != Win32.MEM_COMMIT_STATE ||
                memory.Type != Win32.MEM_IMAGE ||
                (memory.Protect & Win32.PAGE_GUARD) != 0 ||
                !executable)
            {
                failure =
                    $"remote bootstrap target is not committed executable MEM_IMAGE " +
                    $"storage owned by the returned module base " +
                    $"(state=0x{memory.State:X}, protect=0x{memory.Protect:X}, " +
                    $"type=0x{memory.Type:X})";
                return false;
            }

            var regionStart = UnsignedAddress(memory.BaseAddress);
            var regionSize = memory.RegionSize.ToUInt64();
            var targetStart = (ulong)remoteBootstrapAddressValue;
            var targetEnd = targetStart + 1;
            if (regionStart > targetStart ||
                regionSize > ulong.MaxValue - regionStart ||
                targetEnd < targetStart ||
                targetEnd > regionStart + regionSize)
            {
                failure = "remote bootstrap code witness crosses its executable region";
                return false;
            }

            failure = string.Empty;
            return true;
        }
        catch (Exception ex)
        {
            remoteBootstrapAddress = IntPtr.Zero;
            remoteBootstrapAddressValue = 0;
            failure = $"remote module validation was unavailable: {ex.Message}";
            return false;
        }
    }

    private static bool TryResolveRemotePe32ExportRva(
        IntPtr hProcess,
        uint remoteModuleBase,
        SteamProcessValidation.Pe32ImageIdentity identity,
        string exportName,
        out uint functionRva,
        out string failure)
    {
        const uint maximumExportEntries = 65536;
        functionRva = 0;
        if (string.IsNullOrEmpty(exportName) ||
            exportName.Any(character => character == '\0' || character > 0x7f))
        {
            failure = "remote export name must be non-empty ASCII";
            return false;
        }

        var exportDirectory = new byte[40];
        if (!TryReadRemoteImageRva(
                hProcess,
                remoteModuleBase,
                identity.SizeOfImage,
                identity.ExportDirectoryRva,
                exportDirectory,
                out failure))
        {
            return false;
        }

        var functionCount = BinaryPrimitives.ReadUInt32LittleEndian(
            exportDirectory.AsSpan(20, 4));
        var nameCount = BinaryPrimitives.ReadUInt32LittleEndian(
            exportDirectory.AsSpan(24, 4));
        var functionTableRva = BinaryPrimitives.ReadUInt32LittleEndian(
            exportDirectory.AsSpan(28, 4));
        var nameTableRva = BinaryPrimitives.ReadUInt32LittleEndian(
            exportDirectory.AsSpan(32, 4));
        var ordinalTableRva = BinaryPrimitives.ReadUInt32LittleEndian(
            exportDirectory.AsSpan(36, 4));
        if (functionCount == 0 || nameCount == 0 ||
            functionCount > maximumExportEntries ||
            nameCount > maximumExportEntries ||
            nameCount > functionCount ||
            functionTableRva == 0 || nameTableRva == 0 ||
            ordinalTableRva == 0)
        {
            failure = "remote export directory counts or table RVAs are invalid";
            return false;
        }

        var nameTable = new byte[checked((int)nameCount * sizeof(uint))];
        var ordinalTable = new byte[checked((int)nameCount * sizeof(ushort))];
        if (!TryReadRemoteImageRva(
                hProcess,
                remoteModuleBase,
                identity.SizeOfImage,
                nameTableRva,
                nameTable,
                out failure) ||
            !TryReadRemoteImageRva(
                hProcess,
                remoteModuleBase,
                identity.SizeOfImage,
                ordinalTableRva,
                ordinalTable,
                out failure))
        {
            return false;
        }

        for (var index = 0U; index < nameCount; ++index)
        {
            var offset = checked((int)index * sizeof(uint));
            var nameRva = BinaryPrimitives.ReadUInt32LittleEndian(
                nameTable.AsSpan(offset, sizeof(uint)));
            if (!TryRemoteAsciiNameEquals(
                    hProcess,
                    remoteModuleBase,
                    identity.SizeOfImage,
                    nameRva,
                    exportName,
                    out var matches,
                    out failure))
            {
                return false;
            }
            if (!matches)
            {
                continue;
            }

            var ordinalOffset = checked((int)index * sizeof(ushort));
            var ordinal = BinaryPrimitives.ReadUInt16LittleEndian(
                ordinalTable.AsSpan(ordinalOffset, sizeof(ushort)));
            if (ordinal >= functionCount)
            {
                failure = $"remote export '{exportName}' has an invalid ordinal";
                return false;
            }

            var functionEntry = new byte[sizeof(uint)];
            var functionEntryRva64 =
                (ulong)functionTableRva + (uint)ordinal * sizeof(uint);
            if (functionEntryRva64 > uint.MaxValue ||
                !TryReadRemoteImageRva(
                    hProcess,
                    remoteModuleBase,
                    identity.SizeOfImage,
                    (uint)functionEntryRva64,
                    functionEntry,
                    out failure))
            {
                return false;
            }

            functionRva = BinaryPrimitives.ReadUInt32LittleEndian(functionEntry);
            var exportEnd =
                (ulong)identity.ExportDirectoryRva + identity.ExportDirectorySize;
            if (functionRva == 0 || functionRva >= identity.SizeOfImage ||
                (functionRva >= identity.ExportDirectoryRva &&
                 (ulong)functionRva < exportEnd))
            {
                functionRva = 0;
                failure =
                    $"remote export '{exportName}' is null, out of image, or forwarded";
                return false;
            }

            failure = string.Empty;
            return true;
        }

        failure = $"remote export '{exportName}' is missing";
        return false;
    }

    private static bool TryReadRemoteImageRva(
        IntPtr hProcess,
        uint remoteModuleBase,
        uint imageSize,
        uint rva,
        byte[] buffer,
        out string failure)
    {
        failure = string.Empty;
        if (buffer.Length == 0 ||
            (ulong)rva + (uint)buffer.Length > imageSize ||
            !SteamProcessValidation.TryAddRemoteModuleRva(
                remoteModuleBase,
                rva,
                out var address,
                out failure))
        {
            if (string.IsNullOrEmpty(failure))
            {
                failure = "remote image RVA range is outside the mapped module";
            }
            return false;
        }
        if (!ReadProcessMemoryExactly(hProcess, UInt32Address(address), buffer))
        {
            failure =
                $"could not read remote image RVA 0x{rva:X8} ({buffer.Length} bytes)";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    private static bool TryRemoteAsciiNameEquals(
        IntPtr hProcess,
        uint remoteModuleBase,
        uint imageSize,
        uint nameRva,
        string expected,
        out bool matches,
        out string failure)
    {
        const int maximumNameBytes = 512;
        failure = string.Empty;
        matches = true;
        var character = new byte[1];
        for (var index = 0; index < maximumNameBytes; ++index)
        {
            var characterRva = (ulong)nameRva + (uint)index;
            if (characterRva > uint.MaxValue ||
                !TryReadRemoteImageRva(
                    hProcess,
                    remoteModuleBase,
                    imageSize,
                    (uint)characterRva,
                    character,
                    out failure))
            {
                matches = false;
                return false;
            }
            if (character[0] == 0)
            {
                matches &= index == expected.Length;
                failure = string.Empty;
                return true;
            }
            if (index >= expected.Length || character[0] != (byte)expected[index])
            {
                matches = false;
            }
        }

        matches = false;
        failure = "remote export name is not null-terminated within 512 bytes";
        return false;
    }

    private static IntPtr UInt32Address(uint address) =>
        IntPtr.Size == 4
            ? new IntPtr(unchecked((int)address))
            : new IntPtr((long)address);

    private static ulong UnsignedAddress(IntPtr address) =>
        IntPtr.Size == 4
            ? unchecked((uint)address.ToInt32())
            : unchecked((ulong)address.ToInt64());

    private static string DescribeBootstrapExitCode(
        Kotor2VrBootstrapKind bootstrapKind,
        uint exitCode)
    {
        if (bootstrapKind == Kotor2VrBootstrapKind.VrBridge)
        {
            return exitCode switch
            {
                0 => "Ok",
                1 => "InvalidArgument",
                2 => "VersionMismatch",
                3 => "InvalidSession",
                4 => "AlreadyRunning",
                5 => "Busy",
                6 => "PersistentLogFailure",
                7 => "ModulePinFailure",
                8 => "ChannelOpenFailure",
                9 => "SynchronizationFailure",
                10 => "WorkerStartFailure",
                11 => "WorkerReadyTimeout",
                12 => "InitialHealthFailure",
                13 => "StopTimeout",
                14 => "AlreadyStopped",
                _ => "UnknownVrBridgeResult"
            };
        }

        if (bootstrapKind == Kotor2VrBootstrapKind.GameImageSmoke)
        {
            return exitCode switch
            {
                0 => "Ok",
                1 => "InvalidArgument",
                2 => "VersionMismatch",
                3 => "InvalidSession",
                4 => "Busy",
                5 => "PersistentLogFailure",
                _ => "UnknownGameImageSmokeResult"
            };
        }

        return exitCode switch
        {
            0 => "OkExactBuildProbeOnly",
            1 => "AlreadyInitialized",
            2 => "UnknownExecutable",
            3 => "UnsupportedArchitecture",
            4 => "HashFailure",
            5 => "PeInspectionFailure",
            6 => "NotInitialized",
            7 => "ProbeUnavailable",
            8 => "LifetimePinFailure",
            9 => "PersistentLogFailure",
            10 => "SamplerStartFailure",
            11 => "InvalidBootstrapArgument",
            12 => "SamplerStopTimeout",
            _ => "UnknownProbeResult"
        };
    }

    /// <summary>
    /// Launches a game executable with delayed DLL injection (Steam distribution)
    /// Launches normally, waits for Steam to decrypt, then injects into the running process
    /// </summary>
    private static LaunchResult LaunchSteamWithInjection(
        string gameExePath,
        string dllPath,
        string? commandLineArgs,
        Kotor2VrBootstrapKind? requiredKotor2VrBootstrap,
        byte[]? requiredKotor2VrBootstrapArgument)
    {
        Process? gameProcess = null;
        var transferProcessOwnership = false;
        try
        {
            var absGamePath = Path.GetFullPath(gameExePath);
            var absDllPath = Path.GetFullPath(dllPath);

            if (!SteamLauncher.TryResolveAppId(absGamePath, out var appId))
            {
                return LaunchResult.Fail(
                    $"No known Steam app id for {Path.GetFileName(absGamePath)}; " +
                    "refusing to start a DRM-protected executable directly.");
            }

            if (!string.IsNullOrWhiteSpace(commandLineArgs))
            {
                return LaunchResult.Fail(
                    "Command-line arguments for delayed Steam injection are not supported; " +
                    "refusing to silently drop them.");
            }

            Console.WriteLine(
                $"[KPatchCore] Launching Steam game {Path.GetFileName(absGamePath)} " +
                $"through app {appId}");

            var preexistingProcessIds = SnapshotProcessIds(absGamePath);
            var launchNotBeforeUtc = DateTime.UtcNow;

            // Step 1: Ask Steam to create the game process. UseShellExecute on the
            // protected EXE itself does not establish Steam's app context and makes
            // the game exit with "This game needs Steam", even while Steam is open.
            SteamLauncher.Launch(appId);

            // Step 2: Wait for the game process to appear (with validation to skip bootstrap)
            Console.WriteLine("[KPatchCore] Waiting for game process (detecting and skipping Steam bootstrap)...");
            gameProcess = FindGameProcess(
                absGamePath,
                preexistingProcessIds,
                launchNotBeforeUtc,
                TimeSpan.FromSeconds(90));

            if (gameProcess == null)
            {
                return LaunchResult.Fail(
                    "Could not find valid game process after Steam launch. " +
                    "Ensure Steam is running and the game launches correctly. " +
                    "Check console output for validation details.");
            }

            Console.WriteLine($"[KPatchCore] Validated game process found (PID: {gameProcess.Id})");

            // Step 3: Wait for game initialization (window creation)
            Console.WriteLine("[KPatchCore] Waiting for game window initialization...");
            if (!WaitForProcessInitialization(gameProcess, TimeSpan.FromSeconds(30)))
            {
                return LaunchResult.Fail(
                    "Timeout waiting for game initialization. " +
                    "The game may have failed to start or Steam decryption took too long.");
            }

            Console.WriteLine("[KPatchCore] Game initialized, injecting DLL...");

            // Step 4: Inject DLL into the running process
            var injectResult = InjectIntoRunningProcess(
                gameProcess,
                absDllPath,
                absGamePath,
                launchNotBeforeUtc,
                preexistingProcessIds,
                requiredKotor2VrBootstrap,
                requiredKotor2VrBootstrapArgument);

            if (!injectResult.Success)
            {
                return LaunchResult.Fail(
                    $"Failed to inject DLL into running Steam process: {injectResult.Error}");
            }

            Console.WriteLine("[KPatchCore] DLL injected successfully into Steam game");

            var launchResult = LaunchResult.Ok(
                gameProcess,
                injectionPerformed: true,
                $"Successfully launched {Path.GetFileName(gameExePath)} with delayed injection (Steam)");
            transferProcessOwnership = true;
            return launchResult;
        }
        catch (Exception ex)
        {
            return LaunchResult.Fail($"Steam launch failed: {ex.Message}");
        }
        finally
        {
            if (!transferProcessOwnership)
            {
                gameProcess?.Dispose();
            }
        }
    }

    /// <summary>
    /// Finds a new game process by exact executable path and validates its in-memory PE image.
    /// Transiently inaccessible candidates remain eligible on later polling iterations.
    /// </summary>
    private static Process? FindGameProcess(
        string exePath,
        IReadOnlySet<int> preexistingProcessIds,
        DateTime launchNotBeforeUtc,
        TimeSpan timeout)
    {
        var executableName = Path.GetFileNameWithoutExtension(exePath);
        var expectedGamePath = SteamProcessValidation.CanonicalizeExecutablePath(exePath);
        var stopwatch = Stopwatch.StartNew();
        var loggedFailures = new HashSet<string>(StringComparer.Ordinal);

        Console.WriteLine(
            $"[KPatchCore] Searching for a new exact process: {expectedGamePath}");

        while (stopwatch.Elapsed < timeout)
        {
            try
            {
                var processes = Process.GetProcessesByName(executableName);
                Process? selectedProcess = null;
                try
                {
                    foreach (var process in processes)
                    {
                        if (IsValidGameProcess(
                                process,
                                expectedGamePath,
                                launchNotBeforeUtc,
                                preexistingProcessIds,
                                out var validationError))
                        {
                            // Recheck after a short stability interval. This remains a
                            // read-only qualification; injection performs its own final
                            // validation after the main window appears.
                            Thread.Sleep(500);
                            if (IsValidGameProcess(
                                    process,
                                    expectedGamePath,
                                    launchNotBeforeUtc,
                                    preexistingProcessIds,
                                    out validationError))
                            {
                                selectedProcess = process;
                                Console.WriteLine(
                                    $"[KPatchCore] Validated exact stable process: PID {process.Id}");
                                return process;
                            }
                        }

                        var failureKey = $"{process.Id}:{validationError}";
                        if (loggedFailures.Add(failureKey))
                        {
                            Console.WriteLine(
                                $"[KPatchCore] Candidate PID {process.Id} not yet eligible: " +
                                validationError);
                        }
                    }
                }
                finally
                {
                    foreach (var process in processes)
                    {
                        if (!ReferenceEquals(process, selectedProcess))
                        {
                            process.Dispose();
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[KPatchCore] Error during process search: {ex.Message}");
            }

            Thread.Sleep(100);  // Poll every 100ms
        }

        Console.WriteLine($"[KPatchCore] Timeout: No valid process found after {timeout.TotalSeconds}s");
        return null;
    }

    private static HashSet<int> SnapshotProcessIds(string exePath)
    {
        var executableName = Path.GetFileNameWithoutExtension(exePath);
        var processIds = new HashSet<int>();
        foreach (var process in Process.GetProcessesByName(executableName))
        {
            using (process)
            {
                processIds.Add(process.Id);
            }
        }

        if (processIds.Count != 0)
        {
            Console.WriteLine(
                $"[KPatchCore] Excluding {processIds.Count} pre-existing " +
                $"{executableName} process(es) from this Steam launch.");
        }

        return processIds;
    }

    /// <summary>
    /// Waits for a process to complete initialization
    /// Detects initialization by checking for main window handle creation
    /// </summary>
    private static bool WaitForProcessInitialization(Process process, TimeSpan timeout)
    {
        var stopwatch = Stopwatch.StartNew();

        while (stopwatch.Elapsed < timeout)
        {
            try
            {
                process.Refresh();

                // Check if process has exited (failed to initialize)
                if (process.HasExited)
                {
                    Console.WriteLine("[KPatchCore] Process exited before initialization completed");
                    return false;
                }

                // Window handle creation indicates the game has initialized
                if (process.MainWindowHandle != IntPtr.Zero)
                {
                    Console.WriteLine("[KPatchCore] Main window detected, game initialized");
                    return true;
                }
            }
            catch
            {
                // Process may not be accessible - continue waiting
            }

            Thread.Sleep(100);  // Poll every 100ms
        }

        return false;
    }

    /// <summary>
    /// Validates launch identity, exact main-module path, and the in-memory PE32 image.
    /// Any inaccessible or incomplete state fails closed for this polling iteration.
    /// </summary>
    private static bool IsValidGameProcess(
        Process process,
        string expectedGamePath,
        DateTime launchNotBeforeUtc,
        IReadOnlySet<int> preexistingProcessIds,
        out string failure)
    {
        try
        {
            process.Refresh();
            if (process.HasExited)
            {
                failure = "process exited";
                return false;
            }

            var startTimeUtc = process.StartTime.ToUniversalTime();
            using var mainModule = process.MainModule;
            if (mainModule is null || mainModule.BaseAddress == IntPtr.Zero)
            {
                failure = "main module or image base is unavailable";
                return false;
            }

            if (!SteamProcessValidation.IsEligibleIdentity(
                    process.Id,
                    startTimeUtc,
                    mainModule.FileName,
                    expectedGamePath,
                    launchNotBeforeUtc,
                    preexistingProcessIds,
                    out failure))
            {
                return false;
            }

            var hProcess = Win32.OpenProcess(
                Win32.PROCESS_VM_READ | Win32.PROCESS_QUERY_INFORMATION,
                false,
                process.Id);

            if (hProcess == IntPtr.Zero)
            {
                failure = "could not open process for read-only validation";
                return false;
            }

            try
            {
                return IsValidPe32Image(hProcess, mainModule.BaseAddress, out failure);
            }
            finally
            {
                Win32.CloseHandle(hProcess);
            }
        }
        catch (Exception ex)
        {
            failure = $"validation was unavailable: {ex.Message}";
            return false;
        }
    }

    internal static bool TryValidateOpenedGameProcess(
        IntPtr hProcess,
        int processId,
        string expectedGamePath,
        DateTime launchNotBeforeUtc,
        IReadOnlySet<int> preexistingProcessIds,
        out IntPtr imageBase,
        out string failure)
    {
        imageBase = IntPtr.Zero;
        try
        {
            if (!Win32.GetProcessTimes(
                    hProcess,
                    out var creationTime,
                    out _,
                    out _,
                    out _))
            {
                failure =
                    $"GetProcessTimes failed (error {Marshal.GetLastWin32Error()})";
                return false;
            }

            var creationFileTime =
                ((ulong)creationTime.dwHighDateTime << 32) |
                creationTime.dwLowDateTime;
            if (creationFileTime > long.MaxValue)
            {
                failure = "process creation FILETIME is outside the DateTime range";
                return false;
            }
            var startTimeUtc = DateTime.FromFileTimeUtc((long)creationFileTime);

            const int maximumPathCharacters = 32768;
            var processPath = new StringBuilder(maximumPathCharacters);
            var processPathLength = (uint)processPath.Capacity;
            if (!Win32.QueryFullProcessImageName(
                    hProcess,
                    0,
                    processPath,
                    ref processPathLength) ||
                processPathLength == 0)
            {
                failure =
                    $"QueryFullProcessImageName failed (error {Marshal.GetLastWin32Error()})";
                return false;
            }

            if (!SteamProcessValidation.IsEligibleIdentity(
                    processId,
                    startTimeUtc,
                    processPath.ToString(),
                    expectedGamePath,
                    launchNotBeforeUtc,
                    preexistingProcessIds,
                    out failure))
            {
                return false;
            }

            var modules = new IntPtr[1];
            if (!Win32.K32EnumProcessModules(
                    hProcess,
                    modules,
                    (uint)IntPtr.Size,
                    out var moduleBytesNeeded) ||
                moduleBytesNeeded < IntPtr.Size ||
                modules[0] == IntPtr.Zero)
            {
                failure =
                    $"K32EnumProcessModules could not identify the main image " +
                    $"(error {Marshal.GetLastWin32Error()})";
                return false;
            }
            imageBase = modules[0];

            var modulePath = new StringBuilder(maximumPathCharacters);
            var modulePathLength = Win32.K32GetModuleFileNameEx(
                hProcess,
                imageBase,
                modulePath,
                (uint)modulePath.Capacity);
            if (modulePathLength == 0 || modulePathLength >= modulePath.Capacity)
            {
                failure =
                    $"K32GetModuleFileNameEx could not identify the main module " +
                    $"(error {Marshal.GetLastWin32Error()})";
                return false;
            }

            var canonicalModulePath =
                SteamProcessValidation.CanonicalizeExecutablePath(modulePath.ToString());
            var canonicalExpectedPath =
                SteamProcessValidation.CanonicalizeExecutablePath(expectedGamePath);
            if (!canonicalModulePath.Equals(
                    canonicalExpectedPath,
                    StringComparison.OrdinalIgnoreCase))
            {
                failure =
                    $"opened-handle main-module path mismatch (expected " +
                    $"'{canonicalExpectedPath}', found '{canonicalModulePath}')";
                return false;
            }

            if (!IsValidPe32Image(hProcess, imageBase, out failure))
            {
                return false;
            }

            failure = string.Empty;
            return true;
        }
        catch (Exception ex)
        {
            imageBase = IntPtr.Zero;
            failure = $"opened-handle validation was unavailable: {ex.Message}";
            return false;
        }
    }

    private static bool IsValidPe32Image(
        IntPtr hProcess,
        IntPtr imageBase,
        out string failure)
    {
        if (imageBase == IntPtr.Zero)
        {
            failure = "main-module image base is null";
            return false;
        }

        var dosHeader = new byte[SteamProcessValidation.DosHeaderSize];
        if (!ReadProcessMemoryExactly(hProcess, imageBase, dosHeader))
        {
            failure = "could not read the complete DOS header";
            return false;
        }
        if (!SteamProcessValidation.TryGetPeHeaderOffset(
                dosHeader,
                out var peOffset,
                out failure))
        {
            return false;
        }

        // Signature (4), COFF header (20), and OptionalHeader.Magic (2).
        var ntHeaderPrefix = new byte[SteamProcessValidation.NtHeaderPrefixSize];
        var ntHeaderAddress = IntPtr.Add(imageBase, peOffset);
        if (!ReadProcessMemoryExactly(hProcess, ntHeaderAddress, ntHeaderPrefix))
        {
            failure = "could not read the complete NT/COFF header prefix";
            return false;
        }

        return SteamProcessValidation.IsPe32NtHeader(ntHeaderPrefix, out failure);
    }

    private static bool ReadProcessMemoryExactly(
        IntPtr hProcess,
        IntPtr address,
        byte[] buffer)
    {
        return Win32.ReadProcessMemory(
                   hProcess,
                   address,
                   buffer,
                   buffer.Length,
                   out var bytesRead) &&
               bytesRead.ToInt64() == buffer.Length;
    }

    private readonly record struct ProbeLogCursor(string Path, long Length);

}
