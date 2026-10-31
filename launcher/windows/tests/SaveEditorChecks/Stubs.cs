// SPDX-License-Identifier: GPL-3.0-or-later
global using L = BloodborneLauncher.Core.Localization;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace BloodborneLauncher.Core
{
    public static class Localization
    {
        public static string T(string text) => text;
        public static string F(FormattableString text) => text.ToString(CultureInfo.InvariantCulture);
    }

    public static class SaveBackups
    {
        internal static void Copy(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (var dir in Directory.GetDirectories(from, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, dir)));
            foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories))
                File.Copy(file, Path.Combine(to, Path.GetRelativePath(from, file)), overwrite: true);
        }

        internal static string Fingerprint(string dir) =>
            Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(string.Join("|", Directory.GetFiles(dir, "*", SearchOption.AllDirectories).OrderBy(f => f, StringComparer.Ordinal)))));
    }
}
