using System.Globalization;

namespace Kotor2Vr.Launcher;

internal sealed class TomlLite
{
    private readonly Dictionary<string, string> _values;

    private TomlLite(Dictionary<string, string> values)
    {
        _values = values;
    }

    public static TomlLite Load(string path)
    {
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        var section = string.Empty;
        var lineNumber = 0;

        foreach (var rawLine in File.ReadLines(path))
        {
            lineNumber++;
            var line = StripComment(rawLine).Trim();
            if (line.Length == 0)
            {
                continue;
            }

            if (line.StartsWith('[') && line.EndsWith(']'))
            {
                section = line[1..^1].Trim();
                if (section.Length == 0)
                {
                    throw new FormatException($"Empty TOML section at line {lineNumber}.");
                }
                continue;
            }

            var equals = line.IndexOf('=');
            if (equals <= 0)
            {
                throw new FormatException($"Expected key=value at line {lineNumber}.");
            }

            var key = line[..equals].Trim();
            var value = line[(equals + 1)..].Trim();
            var fullKey = section.Length == 0 ? key : $"{section}.{key}";
            if (!values.TryAdd(fullKey, value))
            {
                throw new FormatException($"Duplicate TOML key '{fullKey}' at line {lineNumber}.");
            }
        }

        return new TomlLite(values);
    }

    public string RequireString(string key)
    {
        var raw = RequireRaw(key);
        if (raw.Length < 2 || raw[0] != '"' || raw[^1] != '"')
        {
            throw new FormatException($"TOML key '{key}' must be a quoted string.");
        }

        return raw[1..^1]
            .Replace("\\\"", "\"", StringComparison.Ordinal)
            .Replace("\\\\", "\\", StringComparison.Ordinal);
    }

    public string GetString(string key, string fallback)
    {
        if (!_values.ContainsKey(key))
        {
            return fallback;
        }

        return RequireString(key);
    }

    public bool GetBoolean(string key, bool fallback)
    {
        if (!_values.TryGetValue(key, out var raw))
        {
            return fallback;
        }

        return raw.ToLowerInvariant() switch
        {
            "true" => true,
            "false" => false,
            _ => throw new FormatException($"TOML key '{key}' must be true or false.")
        };
    }

    public int GetInt32(string key, int fallback)
    {
        if (!_values.TryGetValue(key, out var raw))
        {
            return fallback;
        }

        if (!int.TryParse(raw, NumberStyles.Integer, CultureInfo.InvariantCulture, out var value))
        {
            throw new FormatException($"TOML key '{key}' must be an integer.");
        }
        return value;
    }

    private string RequireRaw(string key) =>
        _values.TryGetValue(key, out var value)
            ? value
            : throw new FormatException($"Missing required TOML key '{key}'.");

    private static string StripComment(string value)
    {
        var quoted = false;
        var escaped = false;
        for (var i = 0; i < value.Length; i++)
        {
            var current = value[i];
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (current == '\\' && quoted)
            {
                escaped = true;
                continue;
            }
            if (current == '"')
            {
                quoted = !quoted;
            }
            else if (current == '#' && !quoted)
            {
                return value[..i];
            }
        }
        return value;
    }
}
