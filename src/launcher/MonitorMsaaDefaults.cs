using System.Globalization;
using System.Text.RegularExpressions;

namespace Kotor2Vr.Launcher;

// A reversible INI-key override, not a saved graphics-quality preset.
internal static class MonitorMsaaDefaults
{
    internal const string EnvironmentVariable = "KOTOR2VR_MONITOR_MSAA_OFF";
    private const string SectionName = "Graphics Options";
    private static readonly Regex Headers = new(
        @"^[ \t]*\[(?<name>[^\]\r\n]+)\][ \t]*(?:[;#][^\r\n]*)?\r?$",
        RegexOptions.Multiline | RegexOptions.CultureInvariant);
    private static readonly Regex Key = new(
        @"^(?<prefix>[ \t]*Anti Aliasing[ \t]*=[ \t]*)(?<value>[^\r\n]*)(?<newline>\r?\n|$)",
        RegexOptions.Multiline | RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    private static readonly Regex Value = new(
        @"^(?<number>[0-9]+)(?<suffix>[ \t]*(?:[;#].*)?)$", RegexOptions.CultureInvariant);

    internal static bool ShouldApply(bool nativeStereo, string? setting) =>
        nativeStereo && setting == "1";

    internal static string Create(string original)
    {
        var (start, end, key) = Find(original);
        if (start < 0)
            throw new InvalidDataException("Game INI has no Graphics Options section; MSAA was not changed.");
        if (key is null)
        {
            var newline = original.Contains("\r\n", StringComparison.Ordinal) ? "\r\n" : "\n";
            var separator = end > 0 && original[end - 1] != '\n' ? newline : "";
            return original[..end] + separator + "Anti Aliasing=0" + newline + original[end..];
        }
        var value = Parse(key);
        if (value is null)
            throw new InvalidDataException("Game INI has an unsupported Anti Aliasing value; MSAA was not changed.");
        return ReplaceNumber(original, start, key, value, "0");
    }

    internal static string Restore(string current, string original)
    {
        var (start, _, key) = Find(current);
        var value = key is null ? null : Parse(key);
        // Deleted keys/sections and changed or unfamiliar values belong to the
        // user/game. Only an identifiable remaining session value is ours.
        if (key is null || value is null ||
            !int.TryParse(value.Groups["number"].Value, NumberStyles.None,
                CultureInfo.InvariantCulture, out var number) || number != 0) return current;
        var (_, _, originalKey) = Find(original);
        if (originalKey is null)
            return current.Remove(start + key.Index, key.Length);
        var originalValue = Parse(originalKey) ?? throw new InvalidDataException("Invalid original MSAA value.");
        return ReplaceNumber(current, start, key, value, originalValue.Groups["number"].Value);
    }

    private static Match? Parse(Match key)
    {
        var value = Value.Match(key.Groups["value"].Value);
        return value.Success && int.TryParse(value.Groups["number"].Value,
            NumberStyles.None, CultureInfo.InvariantCulture, out _) ? value : null;
    }

    private static string ReplaceNumber(string text, int start, Match key, Match value, string number)
    {
        var index = start + key.Groups["value"].Index + value.Groups["number"].Index;
        return text[..index] + number + text[(index + value.Groups["number"].Length)..];
    }

    private static (int Start, int End, Match? Key) Find(string text)
    {
        var headers = Headers.Matches(text);
        var found = -1;
        for (var i = 0; i < headers.Count; ++i)
            if (headers[i].Groups["name"].Value.Trim().Equals(SectionName, StringComparison.OrdinalIgnoreCase))
            {
                if (found >= 0) throw new InvalidDataException("Duplicate Graphics Options sections; MSAA restore is ambiguous.");
                found = i;
            }
        if (found < 0) return (-1, -1, null);
        var start = headers[found].Index;
        var end = found + 1 < headers.Count ? headers[found + 1].Index : text.Length;
        var matches = Key.Matches(text.Substring(start, end - start));
        if (matches.Count > 1) throw new InvalidDataException("Duplicate Anti Aliasing keys; MSAA restore is ambiguous.");
        return (start, end, matches.Count == 0 ? null : matches[0]);
    }
}
