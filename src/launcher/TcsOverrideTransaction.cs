using System.ComponentModel;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Kotor2Vr.Launcher;

internal enum OwnerLiveness
{
    Alive,
    DefinitelyDead,
    Unknown
}

internal readonly record struct OwnerIdentity(int ProcessId, long StartTimeUtcTicks);

internal interface IProcessLifetime
{
    OwnerIdentity Current { get; }
    OwnerLiveness Inspect(OwnerIdentity owner);
}

internal sealed class SystemProcessLifetime : IProcessLifetime
{
    public OwnerIdentity Current { get; }

    public SystemProcessLifetime()
    {
        using var current = Process.GetCurrentProcess();
        Current = new OwnerIdentity(
            current.Id,
            current.StartTime.ToUniversalTime().Ticks);
    }

    public OwnerLiveness Inspect(OwnerIdentity owner)
    {
        try
        {
            using var process = Process.GetProcessById(owner.ProcessId);
            if (process.HasExited)
            {
                return OwnerLiveness.DefinitelyDead;
            }

            var actualStart = process.StartTime.ToUniversalTime().Ticks;
            return actualStart == owner.StartTimeUtcTicks
                ? OwnerLiveness.Alive
                : OwnerLiveness.DefinitelyDead;
        }
        catch (ArgumentException)
        {
            return OwnerLiveness.DefinitelyDead;
        }
        catch (InvalidOperationException)
        {
            return OwnerLiveness.DefinitelyDead;
        }
        catch (Win32Exception)
        {
            return OwnerLiveness.Unknown;
        }
        catch (NotSupportedException)
        {
            return OwnerLiveness.Unknown;
        }
    }
}

internal sealed record RecoveryReport(
    int Restored,
    int StillOwned,
    int Uncertain,
    int Invalid)
{
    public bool HasBlockingProblems => Uncertain != 0 || Invalid != 0;
}

internal sealed class TcsOverrideTransaction : IDisposable
{
    private const int CurrentSchemaVersion = 2;
    // Older launchers must not treat a game-INI merge as a legacy full restore.
    private const int MonitorMsaaSchemaVersion = 3;
    // The broader pattern also surfaces journals from the pre-ownership schema as
    // invalid instead of silently ignoring a potentially disabled user config.
    private const string JournalPattern = "tcs-override*.json";
    private static readonly Regex EnabledPattern = new(
        @"^(?<prefix>\s*enabled\s*=\s*)(?<value>[01])(?<suffix>\s*(?:[#;].*)?)$",
        RegexOptions.Multiline | RegexOptions.IgnoreCase |
        RegexOptions.CultureInvariant);

    private readonly string _stateDirectory;
    private readonly IProcessLifetime _processLifetime;
    private FileStream? _ownershipLock;
    private string? _journalPath;
    private Guid _sessionNonce;
    private bool _active;
    private bool _disposed;

    private sealed record Journal(
        int SchemaVersion,
        Guid SessionNonce,
        int OwnerProcessId,
        long OwnerStartTimeUtcTicks,
        string TargetKey,
        string TargetPath,
        string BackupPath,
        string OriginalSha256,
        string OverriddenSha256,
        DateTimeOffset CreatedUtc,
        string? MonitorPresetPath = null,
        bool MonitorMsaaOff = false);

    public TcsOverrideTransaction(
        string? stateDirectory = null,
        IProcessLifetime? processLifetime = null)
    {
        _stateDirectory = Path.GetFullPath(stateDirectory ?? DefaultStateDirectory);
        _processLifetime = processLifetime ?? new SystemProcessLifetime();
    }

    public static string DefaultStateDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "Kotor2VR");

    public bool IsActive => _active;
    public Guid SessionNonce => _sessionNonce;

    public static RecoveryReport RecoverAbandonedTransactions(
        TextWriter? log = null,
        string? stateDirectory = null,
        IProcessLifetime? processLifetime = null)
    {
        var state = Path.GetFullPath(stateDirectory ?? DefaultStateDirectory);
        var lifetime = processLifetime ?? new SystemProcessLifetime();
        if (!Directory.Exists(state))
        {
            return new RecoveryReport(0, 0, 0, 0);
        }

        var restored = 0;
        var stillOwned = 0;
        var uncertain = 0;
        var invalid = 0;

        foreach (var journalPath in Directory.EnumerateFiles(
                     state, JournalPattern, SearchOption.TopDirectoryOnly))
        {
            Journal? initial;
            try
            {
                initial = ReadAndValidateJournal(journalPath, state);
            }
            catch (Exception exception)
            {
                invalid++;
                log?.WriteLine(
                    $"Cannot recover invalid TCS journal '{journalPath}': {exception.Message}");
                continue;
            }

            var lockPath = GetLockPath(state, initial.TargetKey);
            using var ownershipLock = TryAcquireLock(lockPath);
            if (ownershipLock is null)
            {
                stillOwned++;
                log?.WriteLine(
                    $"TCS override is still owned by another launcher (PID {initial.OwnerProcessId}).");
                continue;
            }

            Journal journal;
            try
            {
                journal = ReadAndValidateJournal(journalPath, state);
                if (journal.SessionNonce != initial.SessionNonce)
                {
                    uncertain++;
                    log?.WriteLine(
                        $"TCS journal changed while recovery acquired ownership: {journalPath}");
                    continue;
                }
            }
            catch (Exception exception)
            {
                invalid++;
                log?.WriteLine(
                    $"Cannot recover changed TCS journal '{journalPath}': {exception.Message}");
                continue;
            }

            var liveness = lifetime.Inspect(new OwnerIdentity(
                journal.OwnerProcessId,
                journal.OwnerStartTimeUtcTicks));
            if (liveness == OwnerLiveness.Alive)
            {
                stillOwned++;
                log?.WriteLine(
                    $"TCS override owner PID {journal.OwnerProcessId} is still alive; not restoring.");
                continue;
            }
            if (liveness != OwnerLiveness.DefinitelyDead)
            {
                uncertain++;
                log?.WriteLine(
                    $"Could not prove that TCS override owner PID {journal.OwnerProcessId} is dead; not restoring.");
                continue;
            }

            try
            {
                RestoreJournal(journalPath, journal, state, log);
                restored++;
            }
            catch (Exception exception)
            {
                invalid++;
                log?.WriteLine(
                    $"TCS recovery failed for '{journal.TargetPath}': {exception.Message}");
            }
        }

        return new RecoveryReport(restored, stillOwned, uncertain, invalid);
    }

    public void BeginDisable(string targetPath) => Begin(targetPath, monitorDefaultOff: false);

    public void BeginMonitorDefaultOff(string reshadeConfigPath) =>
        Begin(reshadeConfigPath, monitorDefaultOff: true);

    public void BeginMonitorMsaaOff(string gameIniPath) =>
        Begin(gameIniPath, monitorDefaultOff: false, monitorMsaaOff: true);

    private void Begin(string targetPath, bool monitorDefaultOff, bool monitorMsaaOff = false)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_active)
        {
            throw new InvalidOperationException("A TCS override transaction is already active.");
        }

        var target = Path.GetFullPath(targetPath);
        if (!File.Exists(target))
        {
            throw new FileNotFoundException(monitorMsaaOff ? "Game INI not found." : monitorDefaultOff
                ? "Monitor ReShade config not found." : "Legacy DLSS config not found.", target);
        }

        Directory.CreateDirectory(_stateDirectory);
        var targetKey = GetTargetKey(target);
        var lockPath = GetLockPath(_stateDirectory, targetKey);
        _ownershipLock = TryAcquireLock(lockPath) ?? throw new InvalidOperationException(
            $"Another Kotor2VR launcher owns the TCS override for '{target}'.");

        _journalPath = GetJournalPath(_stateDirectory, targetKey);
        try
        {
            ResolveExistingJournalBeforeBegin(_journalPath);

            var originalBytes = File.ReadAllBytes(target);
            var text = DecodeText(originalBytes, out var encoding, out var hasBom);
            _sessionNonce = Guid.NewGuid();
            string? monitorPresetPath = null;
            string overriddenText;
            if (monitorMsaaOff)
            {
                overriddenText = MonitorMsaaDefaults.Create(text);
            }
            else if (monitorDefaultOff)
            {
                monitorPresetPath = Path.Combine(_stateDirectory,
                    $"monitor-dlss.{_sessionNonce:N}.ini");
                overriddenText = MonitorDlssDefaults.Create(text, target, monitorPresetPath);
            }
            else
            {
                var match = EnabledPattern.Match(text);
                if (!match.Success)
                    throw new InvalidDataException(
                        "Legacy DLSS config has no supported enabled=0/1 setting.");
                overriddenText = EnabledPattern.Replace(
                    text,
                    matchValue => matchValue.Groups["prefix"].Value + "0" +
                                  matchValue.Groups["suffix"].Value,
                    1);
            }
            var overriddenBytes = EncodeText(overriddenText, encoding, hasBom);

            var backupPath = Path.Combine(
                _stateDirectory,
                $"tcs-override.{targetKey}.{_sessionNonce:N}.backup");
            DurableCreate(backupPath, originalBytes);

            var owner = _processLifetime.Current;
            var journal = new Journal(
                monitorMsaaOff ? MonitorMsaaSchemaVersion : CurrentSchemaVersion,
                _sessionNonce,
                owner.ProcessId,
                owner.StartTimeUtcTicks,
                targetKey,
                target,
                backupPath,
                Sha256(originalBytes),
                Sha256(overriddenBytes),
                DateTimeOffset.UtcNow,
                monitorPresetPath,
                monitorMsaaOff);

            try
            {
                WriteJournal(_journalPath, journal);
            }
            catch
            {
                TryDelete(backupPath);
                throw;
            }

            try
            {
                AtomicWrite(target, overriddenBytes);
                _active = true;
            }
            catch (Exception primary)
            {
                try
                {
                    RestoreJournal(_journalPath, journal, _stateDirectory, null);
                }
                catch (Exception cleanup)
                {
                    Console.Error.WriteLine(
                        $"TCS rollback also failed: {cleanup.Message}");
                }
                throw new IOException(
                    $"Could not apply the session defaults: {primary.Message}",
                    primary);
            }
        }
        catch
        {
            ReleaseOwnership();
            throw;
        }
    }

    public bool Restore(TextWriter? log = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!_active || _journalPath is null)
        {
            return false;
        }

        var journal = ReadAndValidateJournal(_journalPath, _stateDirectory);
        if (journal.SessionNonce != _sessionNonce)
        {
            throw new IOException(
                "TCS journal ownership changed; refusing to restore another session.");
        }

        RestoreJournal(_journalPath, journal, _stateDirectory, log);
        _active = false;
        ReleaseOwnership();
        return true;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        try
        {
            if (_active)
            {
                Restore(Console.Error);
            }
        }
        catch (Exception exception)
        {
            // IDisposable commonly runs while another exception is unwinding. Cleanup
            // failures are recorded, but never replace the primary launch failure.
            Console.Error.WriteLine(
                $"ERROR: automatic TCS restore failed: {exception.Message}");
        }
        finally
        {
            _active = false;
            ReleaseOwnership();
            _disposed = true;
        }
    }

    internal void AbandonForTesting()
    {
        _active = false;
        ReleaseOwnership();
        _disposed = true;
    }

    private void ResolveExistingJournalBeforeBegin(string journalPath)
    {
        if (!File.Exists(journalPath))
        {
            return;
        }

        var journal = ReadAndValidateJournal(journalPath, _stateDirectory);
        var liveness = _processLifetime.Inspect(new OwnerIdentity(
            journal.OwnerProcessId,
            journal.OwnerStartTimeUtcTicks));
        if (liveness != OwnerLiveness.DefinitelyDead)
        {
            throw new InvalidOperationException(
                "A previous TCS override exists and its owner is alive or cannot be proven dead.");
        }

        RestoreJournal(journalPath, journal, _stateDirectory, Console.Error);
    }

    private static Journal ReadAndValidateJournal(
        string journalPath,
        string stateDirectory)
    {
        var bytes = File.ReadAllBytes(journalPath);
        var journal = JsonSerializer.Deserialize<Journal>(bytes)
            ?? throw new InvalidDataException("TCS override journal is empty.");
        if (journal.SchemaVersion != CurrentSchemaVersion && journal.SchemaVersion != MonitorMsaaSchemaVersion)
        {
            throw new InvalidDataException(
                $"Unsupported TCS journal schema {journal.SchemaVersion}.");
        }
        if (journal.MonitorMsaaOff != (journal.SchemaVersion == MonitorMsaaSchemaVersion) ||
            (journal.MonitorMsaaOff && journal.MonitorPresetPath is not null))
            throw new InvalidDataException("Session journal schema and override type do not match.");
        if (journal.SessionNonce == Guid.Empty || journal.OwnerProcessId <= 0 ||
            journal.OwnerStartTimeUtcTicks <= 0)
        {
            throw new InvalidDataException("TCS override journal has invalid ownership data.");
        }

        var target = Path.GetFullPath(journal.TargetPath);
        var expectedKey = GetTargetKey(target);
        if (!expectedKey.Equals(journal.TargetKey, StringComparison.Ordinal) ||
            !Path.GetFullPath(journalPath).Equals(
                GetJournalPath(stateDirectory, expectedKey),
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("TCS journal target key does not match its path.");
        }
        if (!IsChildPath(stateDirectory, journal.BackupPath))
        {
            throw new InvalidDataException("TCS journal backup is outside the state directory.");
        }
        if (!IsSha256(journal.OriginalSha256) || !IsSha256(journal.OverriddenSha256))
        {
            throw new InvalidDataException("TCS journal contains an invalid SHA-256 value.");
        }
        if (journal.MonitorPresetPath is not null &&
            !IsChildPath(stateDirectory, journal.MonitorPresetPath))
            throw new InvalidDataException("Monitor preset is outside the state directory.");
        return journal with
        {
            TargetPath = target,
            BackupPath = Path.GetFullPath(journal.BackupPath),
            MonitorPresetPath = journal.MonitorPresetPath is null
                ? null : Path.GetFullPath(journal.MonitorPresetPath)
        };
    }

    private static void RestoreJournal(
        string journalPath,
        Journal journal,
        string stateDirectory,
        TextWriter? log)
    {
        if (!File.Exists(journal.BackupPath))
        {
            throw new FileNotFoundException(
                "TCS backup referenced by the journal is missing.",
                journal.BackupPath);
        }

        var originalBytes = File.ReadAllBytes(journal.BackupPath);
        if (!Sha256(originalBytes).Equals(
                journal.OriginalSha256,
                StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "TCS backup checksum does not match the journal.");
        }

        var mustWrite = true;
        var restoredBytes = originalBytes;
        if (File.Exists(journal.TargetPath))
        {
            var currentBytes = File.ReadAllBytes(journal.TargetPath);
            var currentHash = Sha256(currentBytes);
            if (currentHash.Equals(
                    journal.OriginalSha256,
                    StringComparison.OrdinalIgnoreCase))
            {
                mustWrite = false;
            }
            else if (!currentHash.Equals(
                         journal.OverriddenSha256,
                         StringComparison.OrdinalIgnoreCase))
            {
                if (journal.MonitorMsaaOff)
                {
                    var currentText = DecodeText(currentBytes, out var encoding, out var hasBom);
                    var originalText = DecodeText(originalBytes, out _, out _);
                    restoredBytes = EncodeText(MonitorMsaaDefaults.Restore(currentText,
                        originalText), encoding, hasBom);
                }
                else if (journal.MonitorPresetPath is not null)
                {
                    var currentText = DecodeText(currentBytes, out var encoding, out var hasBom);
                    var originalText = DecodeText(originalBytes, out _, out _);
                    restoredBytes = EncodeText(MonitorDlssDefaults.Restore(currentText,
                        originalText, journal.TargetPath, journal.MonitorPresetPath), encoding, hasBom);
                }
                else
                {
                    var conflict = Path.Combine(
                        stateDirectory,
                        $"tcs-override.restore-conflict.{DateTimeOffset.UtcNow:yyyyMMddHHmmssfff}.{Guid.NewGuid():N}.cfg");
                    DurableCreate(conflict, currentBytes);
                    throw new IOException(
                        $"The DLSS config changed during the VR session. A durable copy was saved to '{conflict}'; " +
                        "the original was not overwritten.");
                }
            }
        }

        if (mustWrite)
        {
            AtomicWrite(journal.TargetPath, restoredBytes);
        }

        File.Delete(journalPath);
        TryDelete(journal.BackupPath);
        log?.WriteLine(journal.MonitorMsaaOff
            ? $"Completed monitor MSAA session restore (preserving game/user edits): {journal.TargetPath}"
            : journal.MonitorPresetPath is null
            ? $"Restored legacy DLSS config: {journal.TargetPath}"
            : $"Restored monitor ReShade startup settings: {journal.TargetPath}");
    }

    private static void WriteJournal(string path, Journal journal)
    {
        var bytes = JsonSerializer.SerializeToUtf8Bytes(
            journal,
            new JsonSerializerOptions { WriteIndented = true });
        AtomicWrite(path, bytes);
    }

    private static void DurableCreate(string path, byte[] content)
    {
        if (File.Exists(path))
        {
            throw new IOException($"Refusing to overwrite existing transaction file: {path}");
        }
        var directory = Path.GetDirectoryName(Path.GetFullPath(path))
            ?? throw new InvalidOperationException("Transaction path has no parent directory.");
        Directory.CreateDirectory(directory);
        var temporary = Path.Combine(
            directory,
            $".{Path.GetFileName(path)}.{Guid.NewGuid():N}.tmp");
        try
        {
            WriteThrough(temporary, content);
            File.Move(temporary, path);
        }
        finally
        {
            TryDelete(temporary);
        }
    }

    private static void AtomicWrite(string path, byte[] content)
    {
        var fullPath = Path.GetFullPath(path);
        var directory = Path.GetDirectoryName(fullPath)
            ?? throw new InvalidOperationException("Target path has no parent directory.");
        Directory.CreateDirectory(directory);
        var temporary = Path.Combine(
            directory,
            $".{Path.GetFileName(path)}.{Guid.NewGuid():N}.tmp");
        try
        {
            WriteThrough(temporary, content);
            if (File.Exists(fullPath))
            {
                File.Replace(temporary, fullPath, null, ignoreMetadataErrors: true);
            }
            else
            {
                File.Move(temporary, fullPath);
            }
        }
        finally
        {
            TryDelete(temporary);
        }
    }

    private static void WriteThrough(string path, byte[] content)
    {
        using var stream = new FileStream(
            path,
            FileMode.CreateNew,
            FileAccess.Write,
            FileShare.None,
            bufferSize: 4096,
            FileOptions.WriteThrough);
        stream.Write(content);
        stream.Flush(flushToDisk: true);
    }

    private static FileStream? TryAcquireLock(string path)
    {
        try
        {
            Directory.CreateDirectory(
                Path.GetDirectoryName(path) ?? throw new InvalidOperationException(
                    "Lock path has no parent directory."));
            return new FileStream(
                path,
                FileMode.OpenOrCreate,
                FileAccess.ReadWrite,
                FileShare.None,
                bufferSize: 1,
                FileOptions.WriteThrough);
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }
    }

    private void ReleaseOwnership()
    {
        _ownershipLock?.Dispose();
        _ownershipLock = null;
    }

    private static string GetTargetKey(string targetPath)
    {
        var normalized = Path.GetFullPath(targetPath).ToUpperInvariant();
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(normalized)));
    }

    private static string GetJournalPath(string stateDirectory, string targetKey) =>
        Path.Combine(stateDirectory, $"tcs-override.{targetKey}.journal.json");

    private static string GetLockPath(string stateDirectory, string targetKey) =>
        Path.Combine(stateDirectory, $"tcs-override.{targetKey}.lock");

    private static bool IsChildPath(string parent, string candidate)
    {
        var relative = Path.GetRelativePath(
            Path.GetFullPath(parent),
            Path.GetFullPath(candidate));
        return relative.Length > 0 &&
               !relative.Equals("..", StringComparison.Ordinal) &&
               !relative.StartsWith($"..{Path.DirectorySeparatorChar}", StringComparison.Ordinal) &&
               !Path.IsPathRooted(relative);
    }

    private static bool IsSha256(string value) =>
        value.Length == 64 && value.All(Uri.IsHexDigit);

    private static string Sha256(byte[] content) =>
        Convert.ToHexString(SHA256.HashData(content));

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static string DecodeText(
        byte[] bytes,
        out Encoding encoding,
        out bool hasBom)
    {
        hasBom = bytes.Length >= 3 && bytes[0] == 0xEF &&
                 bytes[1] == 0xBB && bytes[2] == 0xBF;
        encoding = new UTF8Encoding(
            encoderShouldEmitUTF8Identifier: hasBom,
            throwOnInvalidBytes: true);
        var offset = hasBom ? 3 : 0;
        return encoding.GetString(bytes, offset, bytes.Length - offset);
    }

    private static byte[] EncodeText(
        string text,
        Encoding encoding,
        bool hasBom)
    {
        var body = encoding.GetBytes(text);
        if (!hasBom)
        {
            return body;
        }
        var preamble = encoding.GetPreamble();
        return [.. preamble, .. body];
    }
}
