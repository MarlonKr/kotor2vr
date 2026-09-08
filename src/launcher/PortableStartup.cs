using System.Collections;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Kotor2Vr.Launcher;

internal static class PortableStartup
{
    internal static void ResetRuntimeEnvironment()
    {
        // Process scope only: never persist settings in the user's environment.
        foreach (DictionaryEntry entry in Environment.GetEnvironmentVariables())
        {
            var name = (string)entry.Key;
            if (name.StartsWith("KOTOR2VR_", StringComparison.OrdinalIgnoreCase) &&
                !name.Equals("KOTOR2VR_GAME_EXECUTABLE", StringComparison.OrdinalIgnoreCase))
            {
                Environment.SetEnvironmentVariable(name, null);
            }
        }
        Environment.SetEnvironmentVariable("KOTOR2VR_EYE_PERCENT", "75");
        Environment.SetEnvironmentVariable("KOTOR2VR_NEURAL_ENABLED", "0");
    }

    internal static void RequireRegisteredSteamGame(string gameExecutable)
    {
        if (!OperatingSystem.IsWindows())
            throw new PlatformNotSupportedException("Portable VR startup requires Windows.");

        var roots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        using (var userKey = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
        {
            if (userKey?.GetValue("SteamPath") is string root && !string.IsNullOrWhiteSpace(root))
                roots.Add(Path.GetFullPath(root));
        }
        foreach (var view in new[] { RegistryView.Registry32, RegistryView.Registry64 })
        {
            using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
            using var key = machine.OpenSubKey(@"SOFTWARE\Valve\Steam");
            if (key?.GetValue("InstallPath") is string root && !string.IsNullOrWhiteSpace(root))
                roots.Add(Path.GetFullPath(root));
        }

        var libraries = new HashSet<string>(roots, StringComparer.OrdinalIgnoreCase);
        foreach (var root in roots)
        {
            var libraryFile = Path.Combine(root, "steamapps", "libraryfolders.vdf");
            if (File.Exists(libraryFile))
            {
                foreach (var library in ParseLibraryPaths(File.ReadAllText(libraryFile)))
                    libraries.Add(Path.GetFullPath(library));
            }
        }
        var registered = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var library in libraries)
        {
            var manifest = Path.Combine(library, "steamapps", "appmanifest_208580.acf");
            if (!File.Exists(manifest)) continue;
            registered.Add(ResolveManifestExecutable(library, File.ReadAllText(manifest)));
        }
        RequireSingleRegisteredPath(gameExecutable, registered);
    }

    internal static void RequireSingleRegisteredPath(string selected, IEnumerable<string> registered)
    {
        var paths = registered.Select(Path.GetFullPath)
            .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        if (paths.Length != 1 || !paths[0].Equals(Path.GetFullPath(selected), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                "Cannot verify that the selected game is Steam's single registered KOTOR II installation. " +
                "Steam was not started. Select the installation shown by Steam > Manage > Browse local files; " +
                "separate copied game folders are unsupported.");
        }
    }

    internal static string[] ParseLibraryPaths(string text)
    {
        var root = ParseVdf(text);
        var libraries = root.SingleOrDefault(x => x.Key.Equals("libraryfolders", StringComparison.OrdinalIgnoreCase));
        if (libraries?.Children is null) throw new FormatException("Invalid Steam libraryfolders.vdf.");
        return libraries.Children.Where(x => int.TryParse(x.Key, out _))
            .Select(x => x.Value ?? SingleValue(x.Children!, "path"))
            .Where(x => !string.IsNullOrWhiteSpace(x)).ToArray();
    }

    internal static string ResolveManifestExecutable(string library, string text)
    {
        var root = ParseVdf(text);
        var state = root.SingleOrDefault(x => x.Key.Equals("AppState", StringComparison.OrdinalIgnoreCase));
        if (state?.Children is null || SingleValue(state.Children, "appid") != "208580")
            throw new FormatException("Invalid KOTOR II Steam manifest app ID.");
        var directory = SingleValue(state.Children, "installdir");
        if (string.IsNullOrWhiteSpace(directory) || directory is "." or ".." ||
            directory.IndexOfAny(new[] { '/', '\\', ':' }) >= 0 || Path.IsPathRooted(directory))
            throw new FormatException("Invalid Steam manifest installation directory.");
        return Path.GetFullPath(Path.Combine(library, "steamapps", "common", directory, "swkotor2.exe"));
    }

    private sealed record VdfEntry(string Key, string? Value, List<VdfEntry>? Children);

    private static string SingleValue(List<VdfEntry> entries, string key) =>
        entries.Single(x => x.Key.Equals(key, StringComparison.OrdinalIgnoreCase)).Value
        ?? throw new FormatException($"Invalid Steam value: {key}.");

    private static List<VdfEntry> ParseVdf(string text)
    {
        // Steam's manifests use quoted KeyValues, nested braces, and // comments.
        var matches = Regex.Matches(text, "\\G(?:\\s+|//[^\\r\\n]*|(?<brace>[{}])|\"(?<value>(?:\\\\.|[^\"\\\\])*)\")");
        var consumed = 0;
        var tokens = new List<(bool Brace, string Value)>();
        foreach (Match match in matches)
        {
            consumed += match.Length;
            if (match.Groups["brace"].Success) tokens.Add((true, match.Value));
            else if (match.Groups["value"].Success)
                tokens.Add((false, match.Groups["value"].Value.Replace("\\\\", "\\").Replace("\\\"", "\"")));
        }
        if (consumed != text.Length) throw new FormatException("Invalid Steam KeyValues syntax.");
        var index = 0;
        List<VdfEntry> Read(bool nested)
        {
            var result = new List<VdfEntry>();
            while (index < tokens.Count)
            {
                var key = tokens[index++];
                if (key.Brace && key.Value == "}" && nested) return result;
                if (key.Brace || index >= tokens.Count) throw new FormatException("Invalid Steam KeyValues entry.");
                var value = tokens[index++];
                if (value.Brace && value.Value != "{") throw new FormatException("Invalid Steam KeyValues block.");
                result.Add(value.Brace ? new(key.Value, null, Read(true)) : new(key.Value, value.Value, null));
            }
            if (nested) throw new FormatException("Unclosed Steam KeyValues block.");
            return result;
        }
        return Read(false);
    }
}
