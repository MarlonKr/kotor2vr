using System.Buffers.Binary;
using System.Globalization;

namespace Kotor2Vr.Launcher;

internal static class VrBridgeBootstrapPayload
{
    internal const int Size = 32;
    internal const ushort Major = 1;
    internal const ushort Minor = 0;
    internal const ulong Generation = 1;

    internal static byte[] Create(Guid sessionNonce) =>
        CreateFromNonceText(sessionNonce.ToString("N"));

    internal static byte[] CreateFromNonceText(string nonceText)
    {
        if (nonceText.Length != 32 ||
            !ulong.TryParse(
                nonceText.AsSpan(0, 16),
                NumberStyles.AllowHexSpecifier,
                CultureInfo.InvariantCulture,
                out var high) ||
            !ulong.TryParse(
                nonceText.AsSpan(16, 16),
                NumberStyles.AllowHexSpecifier,
                CultureInfo.InvariantCulture,
                out var low))
        {
            throw new ArgumentException(
                "VrBridge session nonce must contain exactly 32 hexadecimal characters.",
                nameof(nonceText));
        }
        if (high == 0 && low == 0)
        {
            throw new ArgumentException(
                "VrBridge session nonce must be nonzero.",
                nameof(nonceText));
        }

        var payload = new byte[Size];
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(0, 4), Size);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(4, 2), Major);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(6, 2), Minor);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(8, 8), low);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(16, 8), high);
        BinaryPrimitives.WriteUInt64LittleEndian(payload.AsSpan(24, 8), Generation);
        return payload;
    }
}
