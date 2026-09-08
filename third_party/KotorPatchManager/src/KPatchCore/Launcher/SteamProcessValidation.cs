using System.Buffers.Binary;
using System.Runtime.CompilerServices;

[assembly: InternalsVisibleTo("kotor2vr-launcher")]

namespace KPatchCore.Launcher;

/// <summary>
/// Pure validation primitives shared by the live Steam process selector and its
/// offline tests. These methods do not enumerate, open, start, or mutate processes.
/// </summary>
internal static class SteamProcessValidation
{
    internal const int DosHeaderSize = 64;
    internal const int NtHeaderPrefixSize = 26;
    internal const string ProbeBootstrapExportName = "K2VR_ProbeBootstrap";
    internal const string RenderTraceBootstrapExportName = "K2VR_RenderTraceBootstrap";
    internal const string RenderDoublePassBootstrapExportName =
        "K2VR_RenderDoublePassBootstrap";
    internal const string VrBridgeBootstrapExportName = "K2VR_VrBridgeBootstrap";
    internal const string GameImageSmokeBootstrapExportName =
        "K2VR_GameImageSmokeBootstrap";

    private const ushort ImageFileMachineI386 = 0x014c;
    private const ushort Pe32Magic = 0x010b;
    private const ushort MinimumPe32OptionalHeaderSize = 96;
    private const int MaximumPeHeaderOffset = 1024 * 1024;

    internal static bool IsEligibleIdentity(
        int processId,
        DateTime processStartTimeUtc,
        string mainModulePath,
        string expectedGamePath,
        DateTime launchNotBeforeUtc,
        IReadOnlySet<int> preexistingProcessIds,
        out string failure)
    {
        if (preexistingProcessIds.Contains(processId))
        {
            failure = "PID existed before this Steam launch";
            return false;
        }

        var normalizedStartTimeUtc = processStartTimeUtc.ToUniversalTime();
        var normalizedBoundaryUtc = launchNotBeforeUtc.ToUniversalTime();
        if (normalizedStartTimeUtc < normalizedBoundaryUtc)
        {
            failure =
                $"start time {normalizedStartTimeUtc:O} predates launch boundary " +
                $"{normalizedBoundaryUtc:O}";
            return false;
        }

        try
        {
            var actualPath = CanonicalizeExecutablePath(mainModulePath);
            var expectedPath = CanonicalizeExecutablePath(expectedGamePath);
            if (!actualPath.Equals(expectedPath, StringComparison.OrdinalIgnoreCase))
            {
                failure =
                    $"main-module path mismatch (expected '{expectedPath}', " +
                    $"found '{actualPath}')";
                return false;
            }
        }
        catch (Exception ex)
        {
            failure = $"main-module path could not be canonicalized: {ex.Message}";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    internal static bool TryGetPeHeaderOffset(
        ReadOnlySpan<byte> dosHeader,
        out int peOffset,
        out string failure)
    {
        peOffset = 0;
        if (dosHeader.Length < DosHeaderSize)
        {
            failure = "DOS header is truncated";
            return false;
        }
        if (dosHeader[0] != (byte)'M' || dosHeader[1] != (byte)'Z')
        {
            failure = "DOS MZ signature is invalid";
            return false;
        }

        peOffset = BinaryPrimitives.ReadInt32LittleEndian(dosHeader.Slice(0x3c, 4));
        if (peOffset < DosHeaderSize || peOffset > MaximumPeHeaderOffset)
        {
            failure = $"PE header offset 0x{peOffset:X} is outside the accepted range";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    internal static bool IsPe32NtHeader(
        ReadOnlySpan<byte> ntHeaderPrefix,
        out string failure)
    {
        if (ntHeaderPrefix.Length < NtHeaderPrefixSize)
        {
            failure = "NT/COFF header prefix is truncated";
            return false;
        }

        var signature = BinaryPrimitives.ReadUInt32LittleEndian(ntHeaderPrefix.Slice(0, 4));
        if (signature != 0x00004550)
        {
            failure = "NT PE signature is invalid";
            return false;
        }

        var machine = BinaryPrimitives.ReadUInt16LittleEndian(ntHeaderPrefix.Slice(4, 2));
        if (machine != ImageFileMachineI386)
        {
            failure = $"COFF machine 0x{machine:X4} is not I386";
            return false;
        }

        var optionalHeaderSize =
            BinaryPrimitives.ReadUInt16LittleEndian(ntHeaderPrefix.Slice(20, 2));
        if (optionalHeaderSize < MinimumPe32OptionalHeaderSize)
        {
            failure =
                $"COFF optional header size {optionalHeaderSize} is too small for PE32";
            return false;
        }

        var optionalHeaderMagic =
            BinaryPrimitives.ReadUInt16LittleEndian(ntHeaderPrefix.Slice(24, 2));
        if (optionalHeaderMagic != Pe32Magic)
        {
            failure = $"optional-header magic 0x{optionalHeaderMagic:X4} is not PE32";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    internal static string CanonicalizeExecutablePath(string path)
    {
        return Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
    }

    internal static bool TryGetPe32ImageIdentity(
        ReadOnlySpan<byte> imageHeaders,
        out Pe32ImageIdentity identity,
        out string failure)
    {
        identity = default;
        if (!TryGetPeHeaderOffset(imageHeaders, out var peOffset, out failure) ||
            !HasFileRange(imageHeaders, (ulong)peOffset, NtHeaderPrefixSize) ||
            !IsPe32NtHeader(
                imageHeaders.Slice(peOffset, NtHeaderPrefixSize),
                out failure))
        {
            if (string.IsNullOrEmpty(failure))
            {
                failure = "NT/COFF header prefix is outside the supplied image headers";
            }
            return false;
        }

        var sectionCount = BinaryPrimitives.ReadUInt16LittleEndian(
            imageHeaders.Slice(peOffset + 6, 2));
        var timeDateStamp = BinaryPrimitives.ReadUInt32LittleEndian(
            imageHeaders.Slice(peOffset + 8, 4));
        var optionalHeaderSize = BinaryPrimitives.ReadUInt16LittleEndian(
            imageHeaders.Slice(peOffset + 20, 2));
        var optionalHeaderOffset = checked(peOffset + 24);
        if (!HasFileRange(
                imageHeaders,
                (ulong)optionalHeaderOffset,
                optionalHeaderSize))
        {
            failure = "PE32 optional header is truncated";
            return false;
        }

        var optionalHeader = imageHeaders.Slice(
            optionalHeaderOffset,
            optionalHeaderSize);
        var dataDirectoryCount = BinaryPrimitives.ReadUInt32LittleEndian(
            optionalHeader.Slice(92, 4));
        if (dataDirectoryCount == 0 || optionalHeader.Length < 104)
        {
            failure = "PE32 optional header has no complete export directory entry";
            return false;
        }

        identity = new Pe32ImageIdentity(
            sectionCount,
            optionalHeaderSize,
            timeDateStamp,
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(16, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(28, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(56, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(60, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(64, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(96, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(100, 4)));

        if (identity.SectionCount == 0 ||
            identity.SizeOfImage == 0 ||
            identity.SizeOfHeaders == 0 ||
            identity.SizeOfHeaders > identity.SizeOfImage ||
            identity.ExportDirectoryRva == 0 ||
            identity.ExportDirectorySize < 40 ||
            (ulong)identity.ExportDirectoryRva + identity.ExportDirectorySize >
                identity.SizeOfImage)
        {
            identity = default;
            failure = "PE32 identity contains invalid image/header/export bounds";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    internal static bool TryReadPe32FileBytesAtRva(
        ReadOnlySpan<byte> image,
        uint rva,
        int byteCount,
        out byte[] bytes,
        out string failure)
    {
        bytes = [];
        if (byteCount <= 0)
        {
            failure = "requested RVA byte count must be positive";
            return false;
        }
        if (!TryReadPe32FileLayout(image, out var layout, out failure) ||
            !TryMapRvaToFileOffset(
                image,
                layout,
                rva,
                byteCount,
                out var fileOffset))
        {
            if (string.IsNullOrEmpty(failure))
            {
                failure = "requested RVA range is not backed by file data";
            }
            return false;
        }

        bytes = image.Slice(fileOffset, byteCount).ToArray();
        failure = string.Empty;
        return true;
    }

    internal static bool TryFindPe32ExportRva(
        ReadOnlySpan<byte> image,
        string exportName,
        out uint functionRva,
        out string failure)
    {
        functionRva = 0;
        if (string.IsNullOrEmpty(exportName) ||
            exportName.Any(character => character == '\0' || character > 0x7f))
        {
            failure = "export name must be non-empty ASCII without embedded nulls";
            return false;
        }

        if (!TryReadPe32FileLayout(image, out var layout, out failure))
        {
            return false;
        }
        if (layout.ExportDirectoryRva == 0 || layout.ExportDirectorySize < 40)
        {
            failure = "PE32 image has no complete export directory";
            return false;
        }
        var exportDirectoryEnd =
            (ulong)layout.ExportDirectoryRva + layout.ExportDirectorySize;
        if (layout.ExportDirectoryRva >= layout.SizeOfImage ||
            exportDirectoryEnd > layout.SizeOfImage)
        {
            failure = "export directory lies outside the PE32 image";
            return false;
        }

        if (!TryMapRvaToFileOffset(
                image,
                layout,
                layout.ExportDirectoryRva,
                40,
                out var exportDirectoryOffset))
        {
            failure = "export directory RVA is not backed by file data";
            return false;
        }

        var exportDirectory = image.Slice(exportDirectoryOffset, 40);
        var functionCount = BinaryPrimitives.ReadUInt32LittleEndian(exportDirectory.Slice(20, 4));
        var nameCount = BinaryPrimitives.ReadUInt32LittleEndian(exportDirectory.Slice(24, 4));
        var functionTableRva = BinaryPrimitives.ReadUInt32LittleEndian(exportDirectory.Slice(28, 4));
        var nameTableRva = BinaryPrimitives.ReadUInt32LittleEndian(exportDirectory.Slice(32, 4));
        var ordinalTableRva = BinaryPrimitives.ReadUInt32LittleEndian(exportDirectory.Slice(36, 4));

        if (functionCount == 0 || nameCount == 0)
        {
            failure = $"export '{exportName}' is not present";
            return false;
        }
        if (functionTableRva == 0 || nameTableRva == 0 || ordinalTableRva == 0)
        {
            failure = "export directory contains a null table RVA";
            return false;
        }
        if (!TryGetTableByteCount(functionCount, sizeof(uint), out var functionTableBytes) ||
            !TryGetTableByteCount(nameCount, sizeof(uint), out var nameTableBytes) ||
            !TryGetTableByteCount(nameCount, sizeof(ushort), out var ordinalTableBytes))
        {
            failure = "export table count exceeds safe parser limits";
            return false;
        }
        if (!TryMapRvaToFileOffset(
                image,
                layout,
                functionTableRva,
                functionTableBytes,
                out var functionTableOffset) ||
            !TryMapRvaToFileOffset(
                image,
                layout,
                nameTableRva,
                nameTableBytes,
                out var nameTableOffset) ||
            !TryMapRvaToFileOffset(
                image,
                layout,
                ordinalTableRva,
                ordinalTableBytes,
                out var ordinalTableOffset))
        {
            failure = "one or more export tables are not backed by file data";
            return false;
        }

        for (var index = 0U; index < nameCount; index++)
        {
            var indexAsInt = checked((int)index);
            var nameRva = BinaryPrimitives.ReadUInt32LittleEndian(
                image.Slice(nameTableOffset + indexAsInt * sizeof(uint), sizeof(uint)));
            if (!TryCompareNullTerminatedAscii(
                    image,
                    layout,
                    nameRva,
                    exportName,
                    out var nameMatches))
            {
                failure = "export name is not a bounded null-terminated file string";
                return false;
            }
            if (!nameMatches)
            {
                continue;
            }

            var ordinal = BinaryPrimitives.ReadUInt16LittleEndian(
                image.Slice(ordinalTableOffset + indexAsInt * sizeof(ushort), sizeof(ushort)));
            if (ordinal >= functionCount)
            {
                failure = $"export '{exportName}' has an out-of-range ordinal";
                return false;
            }

            functionRva = BinaryPrimitives.ReadUInt32LittleEndian(
                image.Slice(
                    functionTableOffset + ordinal * sizeof(uint),
                    sizeof(uint)));
            if (functionRva == 0 || functionRva >= layout.SizeOfImage)
            {
                failure = $"export '{exportName}' has an invalid function RVA";
                functionRva = 0;
                return false;
            }

            if (functionRva >= layout.ExportDirectoryRva &&
                (ulong)functionRva < exportDirectoryEnd)
            {
                failure = $"export '{exportName}' is forwarded, not executable code";
                functionRva = 0;
                return false;
            }
            if (!IsExecutableSectionRva(image, layout, functionRva))
            {
                failure = $"export '{exportName}' does not point into an executable section";
                functionRva = 0;
                return false;
            }

            failure = string.Empty;
            return true;
        }

        failure = $"export '{exportName}' is not present";
        return false;
    }

    internal static bool TryAddRemoteModuleRva(
        uint remoteModuleBase,
        uint functionRva,
        out uint remoteFunctionAddress,
        out string failure)
    {
        remoteFunctionAddress = 0;
        if (remoteModuleBase == 0 || functionRva == 0)
        {
            failure = "remote module base and export RVA must both be nonzero";
            return false;
        }

        var sum = (ulong)remoteModuleBase + functionRva;
        if (sum > uint.MaxValue)
        {
            failure = "remote module base plus export RVA overflows the x86 address space";
            return false;
        }

        remoteFunctionAddress = (uint)sum;
        failure = string.Empty;
        return true;
    }

    internal static bool HasFreshProbeBootstrapWitness(
        string appendedLogText,
        int targetProcessId,
        out string failure)
    {
        if (targetProcessId <= 0)
        {
            failure = "target process id is invalid";
            return false;
        }

        var entryMarker =
            $"bootstrap-entry source=explicit-export pid={targetProcessId} tid=";
        var completionMarker =
            $"bootstrap-complete pid={targetProcessId} sampler=started hooks=disabled";
        var entryOffset = appendedLogText.IndexOf(entryMarker, StringComparison.Ordinal);
        if (entryOffset < 0)
        {
            failure = "fresh explicit-export bootstrap entry is missing";
            return false;
        }

        var completionOffset = appendedLogText.IndexOf(
            completionMarker,
            entryOffset + entryMarker.Length,
            StringComparison.Ordinal);
        if (completionOffset < 0)
        {
            failure = "fresh sampler-started bootstrap completion is missing";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    internal static bool HasFreshRenderTraceBootstrapWitness(
        string appendedLogText,
        int targetProcessId,
        out string failure)
    {
        if (targetProcessId <= 0)
        {
            failure = "target process id is invalid";
            return false;
        }

        // The injector has independently observed a zero bootstrap exit code.
        // The fresh writer-session header binds that success to this PID and the
        // fully prepared three-hook trace stream without depending on a second log.
        var sessionMarker =
            $"{{\"type\":\"session\",\"pid\":{targetProcessId}," +
            "\"qpc_frequency\":";
        var sessionOffset = appendedLogText.IndexOf(
            sessionMarker,
            StringComparison.Ordinal);
        if (sessionOffset < 0 ||
            appendedLogText.IndexOf(
                "\"hooks\":3,\"duration_ms\":600000,\"double_pass\":false}",
                sessionOffset + sessionMarker.Length,
                StringComparison.Ordinal) < 0)
        {
            failure = "fresh three-hook render-trace session header is missing";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    internal static bool HasFreshRenderDoublePassBootstrapWitness(
        string appendedLogText,
        int targetProcessId,
        out string failure)
    {
        if (targetProcessId <= 0)
        {
            failure = "target process id is invalid";
            return false;
        }

        var sessionMarker =
            $"{{\"type\":\"session\",\"pid\":{targetProcessId}," +
            "\"qpc_frequency\":";
        var sessionOffset = appendedLogText.IndexOf(
            sessionMarker,
            StringComparison.Ordinal);
        var modeOffset = sessionOffset < 0
            ? -1
            : appendedLogText.IndexOf(
                "\"hooks\":3,\"duration_ms\":600000,\"double_pass\":true}",
                sessionOffset + sessionMarker.Length,
                StringComparison.Ordinal);
        var armedOffset = modeOffset < 0
            ? -1
            : appendedLogText.IndexOf(
                "{\"type\":\"double_pass_armed\"",
                modeOffset,
                StringComparison.Ordinal);
        if (sessionOffset < 0 || modeOffset < 0 || armedOffset < 0)
        {
            failure = "fresh F8-armed render double-pass session witness is missing";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    internal static bool HasFreshVrBridgeBootstrapWitness(
        string appendedLogText,
        int targetProcessId,
        out string failure)
    {
        if (targetProcessId <= 0)
        {
            failure = "target process id is invalid";
            return false;
        }

        var entryMarker = $"bootstrap pid={targetProcessId} tid=";
        var entryOffset = appendedLogText.IndexOf(
            entryMarker,
            StringComparison.Ordinal);
        if (entryOffset < 0)
        {
            failure = "fresh target-PID VrBridge bootstrap entry is missing";
            return false;
        }

        const string completionMarker =
            "bootstrap complete worker=1 camera_writes=0 render_writes=0";
        var completionOffset = appendedLogText.IndexOf(
            completionMarker,
            entryOffset + entryMarker.Length,
            StringComparison.Ordinal);
        if (completionOffset < 0)
        {
            failure = "fresh VrBridge bootstrap completion is missing";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    internal static bool HasFreshGameImageSmokeBootstrapWitness(
        string appendedLogText,
        int targetProcessId,
        out string failure)
    {
        if (targetProcessId <= 0)
        {
            failure = "target process id is invalid";
            return false;
        }

        var entryMarker = $"game-image-smoke-entry pid={targetProcessId} tid=";
        var entryOffset = appendedLogText.IndexOf(
            entryMarker,
            StringComparison.Ordinal);
        if (entryOffset < 0)
        {
            failure = "fresh target-PID game-image smoke entry is missing";
            return false;
        }

        var completionMarker =
            $"game-image-smoke-complete pid={targetProcessId} hooks=3 key=F7";
        var completionOffset = appendedLogText.IndexOf(
            completionMarker,
            entryOffset + entryMarker.Length,
            StringComparison.Ordinal);
        if (completionOffset < 0)
        {
            failure = "fresh F7-armed game-image smoke completion is missing";
            return false;
        }

        failure = string.Empty;
        return true;
    }

    private static bool TryReadPe32FileLayout(
        ReadOnlySpan<byte> image,
        out Pe32FileLayout layout,
        out string failure)
    {
        layout = default;
        if (!TryGetPeHeaderOffset(image, out var peOffset, out failure) ||
            !HasFileRange(image, (ulong)peOffset, NtHeaderPrefixSize) ||
            !IsPe32NtHeader(image.Slice(peOffset, NtHeaderPrefixSize), out failure))
        {
            if (string.IsNullOrEmpty(failure))
            {
                failure = "NT/COFF header prefix is outside the file";
            }
            return false;
        }

        var sectionCount = BinaryPrimitives.ReadUInt16LittleEndian(image.Slice(peOffset + 6, 2));
        var optionalHeaderSize =
            BinaryPrimitives.ReadUInt16LittleEndian(image.Slice(peOffset + 20, 2));
        const int requiredOptionalHeaderBytes = 104;
        if (sectionCount == 0 || sectionCount > 96)
        {
            failure = "PE32 section count is outside the accepted range";
            return false;
        }
        if (optionalHeaderSize < requiredOptionalHeaderBytes)
        {
            failure = "PE32 optional header does not contain an export data directory";
            return false;
        }

        var optionalHeaderOffset = checked(peOffset + 24);
        if (!HasFileRange(image, (ulong)optionalHeaderOffset, optionalHeaderSize))
        {
            failure = "PE32 optional header is truncated";
            return false;
        }

        var optionalHeader = image.Slice(optionalHeaderOffset, optionalHeaderSize);
        var sizeOfImage = BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(56, 4));
        var sizeOfHeaders = BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(60, 4));
        var dataDirectoryCount =
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(92, 4));
        if (sizeOfImage == 0 || sizeOfHeaders == 0 || dataDirectoryCount == 0)
        {
            failure = "PE32 image/header size or data-directory count is invalid";
            return false;
        }

        var exportDirectoryRva =
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(96, 4));
        var exportDirectorySize =
            BinaryPrimitives.ReadUInt32LittleEndian(optionalHeader.Slice(100, 4));
        var sectionTableOffset = checked(optionalHeaderOffset + optionalHeaderSize);
        var sectionTableBytes = checked(sectionCount * 40);
        if (!HasFileRange(image, (ulong)sectionTableOffset, (ulong)sectionTableBytes))
        {
            failure = "PE32 section table is truncated";
            return false;
        }

        layout = new Pe32FileLayout(
            sizeOfImage,
            sizeOfHeaders,
            exportDirectoryRva,
            exportDirectorySize,
            sectionTableOffset,
            sectionCount);
        failure = string.Empty;
        return true;
    }

    private static bool TryGetTableByteCount(uint count, int elementSize, out int byteCount)
    {
        const uint maximumEntries = 1_000_000;
        if (count > maximumEntries || (ulong)count * (uint)elementSize > int.MaxValue)
        {
            byteCount = 0;
            return false;
        }

        byteCount = checked((int)count * elementSize);
        return true;
    }

    private static bool TryMapRvaToFileOffset(
        ReadOnlySpan<byte> image,
        Pe32FileLayout layout,
        uint rva,
        int requiredBytes,
        out int fileOffset)
    {
        fileOffset = 0;
        if (requiredBytes < 0)
        {
            return false;
        }

        if (rva < layout.SizeOfHeaders)
        {
            if (HasFileRange(image, rva, (ulong)requiredBytes))
            {
                fileOffset = checked((int)rva);
                return true;
            }
            return false;
        }

        for (var index = 0; index < layout.SectionCount; index++)
        {
            var section = image.Slice(layout.SectionTableOffset + index * 40, 40);
            var virtualSize = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(8, 4));
            var virtualAddress = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(12, 4));
            var rawSize = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(16, 4));
            var rawOffset = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(20, 4));
            var mappedSize = Math.Max(virtualSize, rawSize);
            var requestedEnd = (ulong)rva + (uint)requiredBytes;
            var sectionEnd = (ulong)virtualAddress + mappedSize;
            if (rva < virtualAddress || requestedEnd > sectionEnd)
            {
                continue;
            }

            var sectionDelta = rva - virtualAddress;
            if ((ulong)sectionDelta + (uint)requiredBytes > rawSize)
            {
                return false;
            }

            var offset = (ulong)rawOffset + sectionDelta;
            if (!HasFileRange(image, offset, (ulong)requiredBytes) || offset > int.MaxValue)
            {
                return false;
            }

            fileOffset = (int)offset;
            return true;
        }

        return false;
    }

    private static bool TryCompareNullTerminatedAscii(
        ReadOnlySpan<byte> image,
        Pe32FileLayout layout,
        uint stringRva,
        string expected,
        out bool matches)
    {
        const int maximumExportNameBytes = 512;
        matches = true;
        for (var index = 0; index < maximumExportNameBytes; index++)
        {
            var characterRva = (ulong)stringRva + (uint)index;
            if (characterRva > uint.MaxValue ||
                !TryMapRvaToFileOffset(
                    image,
                    layout,
                    (uint)characterRva,
                    1,
                    out var characterOffset))
            {
                matches = false;
                return false;
            }

            var value = image[characterOffset];
            if (value == 0)
            {
                matches &= index == expected.Length;
                return true;
            }

            if (index >= expected.Length || value != (byte)expected[index])
            {
                matches = false;
            }
        }

        matches = false;
        return false;
    }

    private static bool IsExecutableSectionRva(
        ReadOnlySpan<byte> image,
        Pe32FileLayout layout,
        uint rva)
    {
        const uint imageScnMemExecute = 0x20000000;
        for (var index = 0; index < layout.SectionCount; index++)
        {
            var section = image.Slice(layout.SectionTableOffset + index * 40, 40);
            var virtualSize = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(8, 4));
            var virtualAddress = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(12, 4));
            var rawSize = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(16, 4));
            var characteristics = BinaryPrimitives.ReadUInt32LittleEndian(section.Slice(36, 4));
            var sectionEnd = (ulong)virtualAddress + Math.Max(virtualSize, rawSize);
            if (rva >= virtualAddress && (ulong)rva < sectionEnd)
            {
                return (characteristics & imageScnMemExecute) != 0;
            }
        }

        return false;
    }

    private static bool HasFileRange(
        ReadOnlySpan<byte> image,
        ulong offset,
        ulong size)
    {
        return offset <= (ulong)image.Length && size <= (ulong)image.Length - offset;
    }

    private readonly record struct Pe32FileLayout(
        uint SizeOfImage,
        uint SizeOfHeaders,
        uint ExportDirectoryRva,
        uint ExportDirectorySize,
        int SectionTableOffset,
        int SectionCount);

    internal readonly record struct Pe32ImageIdentity(
        ushort SectionCount,
        ushort OptionalHeaderSize,
        uint TimeDateStamp,
        uint AddressOfEntryPoint,
        uint PreferredImageBase,
        uint SizeOfImage,
        uint SizeOfHeaders,
        uint Checksum,
        uint ExportDirectoryRva,
        uint ExportDirectorySize);
}
