namespace Kotor2Vr.Launcher;

internal sealed record LauncherConfig(
    string ConfigPath,
    string GameExecutable,
    string ExpectedSha256,
    int SteamAppId,
    string PreflightScript,
    string HostExecutable,
    string GameModule,
    string Gate1QualificationArtifact,
    bool DlssEnabled,
    string LegacyDlssConfig)
{
    public static LauncherConfig Load(string configPath)
    {
        var fullConfig = Path.GetFullPath(configPath);
        var root = Path.GetDirectoryName(fullConfig)
            ?? throw new InvalidOperationException("The config file has no parent directory.");
        var toml = TomlLite.Load(fullConfig);

        string Resolve(string path)
        {
            var expanded = Environment.ExpandEnvironmentVariables(path);
            return Path.GetFullPath(Path.IsPathRooted(expanded)
                ? expanded
                : Path.Combine(root, expanded));
        }

        return new LauncherConfig(
            fullConfig,
            Resolve(Environment.GetEnvironmentVariable("KOTOR2VR_GAME_EXECUTABLE")
                ?? toml.RequireString("game.executable")),
            toml.RequireString("game.expected_sha256").ToUpperInvariant(),
            toml.GetInt32("game.steam_app_id", 208580),
            Resolve(toml.GetString("paths.preflight_script", "preflight-not-included.ps1")),
            Resolve(toml.RequireString("paths.host_executable")),
            Resolve(toml.RequireString("paths.game_module")),
            Resolve(toml.GetString(
                "paths.gate1_qualification_artifact",
                "qualification/gate1-qualified.json")),
            toml.GetBoolean("dlss5.enabled", false),
            Resolve(toml.GetString("dlss5.legacy_config", "legacy-dlss-not-used.ini")));
    }
}
