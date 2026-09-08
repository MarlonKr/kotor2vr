using System.Text;
using System.Text.RegularExpressions;

namespace Kotor2Vr.Launcher;

// ReShade 6.8 has no persisted global effects-enabled setting. A session preset
// starts its techniques disabled and gives them the existing effects hotkey.
// The feed remains enabled, so F6 can still run the original monitor pipeline.
internal static class MonitorDlssDefaults
{
    private const string DisabledHotkey = "0,0,0,0";

    public static string Create(string config, string configPath, string sessionPresetPath)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(configPath))!;
        var preset = Get(config, "GENERAL", "StartupPresetPath");
        if (string.IsNullOrWhiteSpace(preset))
            preset = Get(config, "GENERAL", "PresetPath");
        var presetPath = ResolvePath(string.IsNullOrWhiteSpace(preset)
            ? ".\\ReShadePreset.ini" : preset, directory);
        var presetText = File.ReadAllText(presetPath);
        var hotkey = Get(config, "INPUT", "KeyEffects") ?? "117,0,0,0";
        if (!Regex.IsMatch(hotkey, @"^\d+\s*,\s*[01]\s*,\s*[01]\s*,\s*[01]$"))
            throw new InvalidDataException("Unsupported ReShade effects hotkey.");
        if (hotkey.Split(',')[0].Trim() == "0")
            hotkey = "117,0,0,0";

        var techniques = Get(presetText, "", "Techniques") ?? throw new InvalidDataException(
            "ReShade preset has no Techniques setting; monitor defaults were not changed.");
        foreach (var technique in techniques.Split(',', StringSplitOptions.TrimEntries |
                     StringSplitOptions.RemoveEmptyEntries).Distinct(StringComparer.Ordinal))
            presetText = Set(presetText, "", "Key" + technique, hotkey);
        presetText = Set(presetText, "", "Techniques", "");

        // A new file per launch keeps the flat preset byte-for-byte unchanged,
        // even when ReShade auto-saves F6 toggles or overlay edits during VR.
        using (var stream = new FileStream(sessionPresetPath, FileMode.CreateNew,
                   FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
        {
            stream.Write(Encoding.UTF8.GetBytes(presetText));
            stream.Flush(flushToDisk: true);
        }
        config = Set(config, "GENERAL", "StartupPresetPath", sessionPresetPath);
        config = Set(config, "GENERAL", "PresetPath", sessionPresetPath);
        return Set(config, "INPUT", "KeyEffects", DisabledHotkey);
    }

    public static string Restore(string current, string original, string configPath,
        string sessionPresetPath)
    {
        var directory = Path.GetDirectoryName(Path.GetFullPath(configPath))!;
        foreach (var key in new[] { "StartupPresetPath", "PresetPath" })
        {
            var value = Get(current, "GENERAL", key);
            if (!string.IsNullOrWhiteSpace(value) &&
                ResolvePath(value, directory).Equals(sessionPresetPath,
                    StringComparison.OrdinalIgnoreCase))
                current = Set(current, "GENERAL", key, Get(original, "GENERAL", key));
        }
        if (Get(current, "INPUT", "KeyEffects")?.Replace(" ", "", StringComparison.Ordinal)
            == DisabledHotkey)
            current = Set(current, "INPUT", "KeyEffects", Get(original, "INPUT", "KeyEffects"));
        // ReShade rewrites window layout and other settings on exit. Restore only
        // our three entries; preserve all other edits and deliberate user remaps.
        return current;
    }

    private static string ResolvePath(string value, string directory) => Path.GetFullPath(
        Environment.ExpandEnvironmentVariables(value.Trim().Trim('"')), directory);

    internal static string? Get(string text, string section, string key)
    {
        var (start, length) = Section(text, section);
        var match = KeyPattern(key).Match(text.Substring(start, length));
        return match.Success ? match.Groups["value"].Value.Trim() : null;
    }

    internal static string Set(string text, string section, string key, string? value)
    {
        var newline = text.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
        var (start, length) = Section(text, section);
        var body = text.Substring(start, length);
        var pattern = KeyPattern(key);
        if (pattern.IsMatch(body))
            body = pattern.Replace(body, match => value is null ? "" :
                match.Groups["prefix"].Value + value + match.Groups["newline"].Value);
        else if (value is not null)
        {
            if (body.Length > 0 && !body.EndsWith('\n')) body += newline;
            if (section.Length > 0 && length == 0)
                body += $"[{section}]{newline}";
            body += $"{key}={value}{newline}";
        }
        if (start > 0 && text[start - 1] != '\n' && body.Length > 0)
            body = newline + body;
        return text[..start] + body + text[(start + length)..];
    }

    private static Regex KeyPattern(string key) => new(
        @"^(?<prefix>[ \t]*" + Regex.Escape(key) + @"[ \t]*=[ \t]*)(?<value>[^\r\n]*)(?<newline>\r?\n|$)",
        RegexOptions.Multiline | RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    private static (int Start, int Length) Section(string text, string section)
    {
        var headers = Regex.Matches(text, @"^[ \t]*\[(?<name>[^\]\r\n]+)\][ \t]*\r?$",
            RegexOptions.Multiline | RegexOptions.CultureInvariant);
        if (section.Length == 0)
            return (0, headers.Count == 0 ? text.Length : headers[0].Index);
        for (var i = 0; i < headers.Count; i++)
        {
            if (!headers[i].Groups["name"].Value.Trim().Equals(section,
                    StringComparison.OrdinalIgnoreCase)) continue;
            var start = headers[i].Index;
            var end = i + 1 < headers.Count ? headers[i + 1].Index : text.Length;
            return (start, end - start);
        }
        return (text.Length, 0);
    }
}
