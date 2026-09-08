using System.Diagnostics;

namespace Kotor2Vr.Launcher;

internal static class PreflightRunner
{
    public static async Task<int> RunAsync(LauncherConfig config, bool json, CancellationToken cancellationToken)
    {
        if (!File.Exists(config.PreflightScript))
        {
            Console.Error.WriteLine($"Preflight script missing: {config.PreflightScript}");
            return 1;
        }

        var start = new ProcessStartInfo
        {
            FileName = "pwsh",
            UseShellExecute = false,
            RedirectStandardOutput = false,
            RedirectStandardError = false
        };
        start.ArgumentList.Add("-NoLogo");
        start.ArgumentList.Add("-NoProfile");
        start.ArgumentList.Add("-File");
        start.ArgumentList.Add(config.PreflightScript);
        start.ArgumentList.Add("-GameExe");
        start.ArgumentList.Add(config.GameExecutable);
        if (json)
        {
            start.ArgumentList.Add("-Json");
        }

        using var process = Process.Start(start);
        if (process is null)
        {
            Console.Error.WriteLine("Could not start PowerShell preflight.");
            return 1;
        }
        await process.WaitForExitAsync(cancellationToken);
        return process.ExitCode;
    }
}

