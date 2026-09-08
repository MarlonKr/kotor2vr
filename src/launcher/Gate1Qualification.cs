using System.Text.Json;

namespace Kotor2Vr.Launcher;

internal static class Gate1Qualification
{
    private const int SchemaVersion = 1;

    public static void RequirePassed(LauncherConfig config)
    {
        if (!File.Exists(config.Gate1QualificationArtifact))
        {
            throw new InvalidOperationException(
                "Native VR launch is intentionally locked: Gate 1 has not been qualified. " +
                $"Missing artifact: {config.Gate1QualificationArtifact}. " +
                "Use the explicit read-only 'probe' command for reverse-engineering runs; " +
                "do not create a qualification artifact until same-tick stereo has passed its in-game gate.");
        }

        using var document = JsonDocument.Parse(
            File.ReadAllBytes(config.Gate1QualificationArtifact));
        var root = document.RootElement;
        Require(root.ValueKind == JsonValueKind.Object, "artifact root must be an object");
        Require(GetInt32(root, "schemaVersion") == SchemaVersion, "unsupported schemaVersion");
        Require(GetString(root, "gate").Equals("gate1", StringComparison.OrdinalIgnoreCase),
            "gate must be 'gate1'");
        Require(GetString(root, "result").Equals("pass", StringComparison.OrdinalIgnoreCase),
            "result must be 'pass'");
        Require(GetBoolean(root, "sameTickStereoVerified"),
            "sameTickStereoVerified must be true");
        Require(GetBoolean(root, "reentrantWorldPassVerified"),
            "reentrantWorldPassVerified must be true");

        var gameHash = GetString(root, "gameSha256");
        Require(gameHash.Equals(config.ExpectedSha256, StringComparison.OrdinalIgnoreCase),
            "gameSha256 does not match the configured exact build");

        Require(File.Exists(config.GameModule), "qualified game module is missing");
        var moduleHash = GetString(root, "gameModuleSha256");
        Require(moduleHash.Equals(FileHash.Sha256(config.GameModule), StringComparison.OrdinalIgnoreCase),
            "gameModuleSha256 does not match the current DLL");

        Require(File.Exists(config.HostExecutable), "qualified host executable is missing");
        var hostHash = GetString(root, "hostSha256");
        Require(hostHash.Equals(FileHash.Sha256(config.HostExecutable), StringComparison.OrdinalIgnoreCase),
            "hostSha256 does not match the current host");

        var runId = GetString(root, "qualificationRunId");
        Require(Guid.TryParse(runId, out var parsedRunId) && parsedRunId != Guid.Empty,
            "qualificationRunId must be a non-empty GUID");
        var qualifiedUtcText = GetString(root, "qualifiedUtc");
        Require(DateTimeOffset.TryParse(qualifiedUtcText, out var qualifiedUtc),
            "qualifiedUtc must be an ISO-8601 timestamp");
        Require(qualifiedUtc <= DateTimeOffset.UtcNow.AddMinutes(5),
            "qualifiedUtc is unexpectedly in the future");
    }

    private static string GetString(JsonElement root, string name)
    {
        Require(root.TryGetProperty(name, out var value) &&
                value.ValueKind == JsonValueKind.String,
            $"{name} must be a string");
        return value.GetString()!;
    }

    private static int GetInt32(JsonElement root, string name)
    {
        if (!root.TryGetProperty(name, out var value) ||
            value.ValueKind != JsonValueKind.Number ||
            !value.TryGetInt32(out var result))
        {
            throw new InvalidDataException(
                $"Invalid Gate 1 qualification artifact: {name} must be an integer.");
        }
        return result;
    }

    private static bool GetBoolean(JsonElement root, string name)
    {
        Require(root.TryGetProperty(name, out var value) &&
                (value.ValueKind == JsonValueKind.True ||
                 value.ValueKind == JsonValueKind.False),
            $"{name} must be a boolean");
        return value.GetBoolean();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidDataException(
                $"Invalid Gate 1 qualification artifact: {message}.");
        }
    }
}
