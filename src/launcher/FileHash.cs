using System.Security.Cryptography;

namespace Kotor2Vr.Launcher;

internal static class FileHash
{
    public static string Sha256(string path)
    {
        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        return Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(stream));
    }
}

