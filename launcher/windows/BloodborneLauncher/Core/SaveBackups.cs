using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Copies of the current profile's savedata, made before each start when it changed since the
    /// last copy; the newest ten are kept, in launcher-backups\&lt;profile&gt;\&lt;time&gt; in the saves
    /// folder. Restoring first backs up what is there now.
    /// </summary>
    public static class SaveBackups
    {
        public const int Keep = 10;
        public const int PlayKeep = 6;
        public static readonly TimeSpan PlayEvery = TimeSpan.FromMinutes(10);
        private static readonly TimeSpan Quiet = TimeSpan.FromSeconds(30);
        private const string DuringPlay = "_play";
        private const string Stamp = "yyyy-MM-dd_HH-mm-ss";
        private const string BeforeRestore = "_before-restore";
        private const string FingerprintFile = "fingerprint.txt";

        public sealed record Backup(string Dir, DateTime When, long Bytes, bool MadeBeforeRestore, bool MadeDuringPlay = false,
            string? Label = null, bool SafetyCopy = false);

        public static string SaveData(LauncherSettings s) => Path.Combine(GameEnvironment.UserDir(s), "savedata");

        public static string Folder(LauncherSettings s) => FolderOf(s, PlayerProfiles.Current(s));

        public static string FolderOf(LauncherSettings s, string profile) =>
            Path.Combine(GameEnvironment.SavesRoot(s), "launcher-backups", profile);

        public static List<Backup> ListWithSafetyCopies(string folder, string vaultFolder)
        {
            var backups = ListIn(folder);
            var prints = backups.Select(b => ReadFingerprint(b.Dir)).Where(p => p != null).ToHashSet();
            var copies = ListIn(vaultFolder)
                .Where(c => ReadFingerprint(c.Dir) is not { } print || !prints.Contains(print))
                .Select(c => c with { SafetyCopy = true });
            return backups.Concat(copies).OrderByDescending(b => b.When).ToList();
        }

        /// <summary>This profile's backups, newest first.</summary>
        public static List<Backup> List(LauncherSettings s) => ListIn(Folder(s));

        /// <summary>The backups in a profile's backup folder, newest first. Reads the disk only (each
        /// backup's size too), so it can run on a worker thread.</summary>
        public static List<Backup> ListIn(string folder)
        {
            if (!Directory.Exists(folder)) return new();
            var list = new List<Backup>();
            foreach (var dir in Directory.GetDirectories(folder))
            {
                var name = Path.GetFileName(dir);
                var before = name.EndsWith(BeforeRestore, StringComparison.Ordinal);
                if (before) name = name[..^BeforeRestore.Length];
                var play = name.EndsWith(DuringPlay, StringComparison.Ordinal);
                if (play) name = name[..^DuringPlay.Length];
                string? label = null;
                if (name.Length > Stamp.Length + 1 && name[^(Stamp.Length + 1)] == '_')
                {
                    label = name[..^(Stamp.Length + 1)];
                    name = name[^Stamp.Length..];
                }
                if (!DateTime.TryParseExact(name, Stamp, CultureInfo.InvariantCulture, DateTimeStyles.None, out var when))
                    continue;
                var saved = Size(Path.Combine(dir, "savedata"));
                if (saved > 0)
                    list.Add(new Backup(dir, when, saved, before, play, label));
            }
            return list.OrderByDescending(b => b.When).ToList();
        }

        /// <summary>Copies the savedata unless the newest backup already holds the same files.
        /// Returns the new backup's folder, or null when there was nothing new to copy.</summary>
        public static string? Create(LauncherSettings s, bool beforeRestore = false)
        {
            var source = SaveData(s);
            if (!Directory.Exists(source) || !Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories).Any())
                return null;
            var print = Fingerprint(source);
            if (!beforeRestore && List(s).FirstOrDefault() is { } newest && ReadFingerprint(newest.Dir) == print)
                return null;

            var target = Path.Combine(Folder(s), DateTime.Now.ToString(Stamp, CultureInfo.InvariantCulture) + (beforeRestore ? BeforeRestore : ""));
            if (Directory.Exists(target)) return null; // one a second is plenty
            Copy(source, Path.Combine(target, "savedata"));
            File.WriteAllText(Path.Combine(target, FingerprintFile), print);
            Prune(s);
            return target;
        }

        public static string? CreateDuringPlay(LauncherSettings s)
        {
            var source = SaveData(s);
            if (!Directory.Exists(source)) return null;
            if (Directory.EnumerateFileSystemEntries(source, "*", SearchOption.AllDirectories).Any(Writing)) return null;
            var files = Directory.GetFiles(source, "*", SearchOption.AllDirectories).Where(f => !Staging(source, f)).ToList();
            if (files.Count == 0) return null;
            if (DateTime.UtcNow - files.Max(File.GetLastWriteTimeUtc) < Quiet) return null;
            var print = Fingerprint(source);
            if (List(s).FirstOrDefault() is { } newest && ReadFingerprint(newest.Dir) == print) return null;

            var target = Path.Combine(Folder(s), DateTime.Now.ToString(Stamp, CultureInfo.InvariantCulture) + DuringPlay);
            if (Directory.Exists(target)) return null;
            var staging = target + ".copying";
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            Copy(source, Path.Combine(staging, "savedata"));
            if (Fingerprint(source) != print)
            {
                Directory.Delete(staging, recursive: true);
                return null;
            }
            File.WriteAllText(Path.Combine(staging, FingerprintFile), print);
            Directory.Move(staging, target);
            Prune(s);
            return target;
        }

        private static void Prune(LauncherSettings s)
        {
            var all = List(s).Where(b => b.Label == null).ToList();
            foreach (var old in all.Where(b => !b.MadeDuringPlay).Skip(Keep).Concat(all.Where(b => b.MadeDuringPlay).Skip(PlayKeep)))
            {
                try { Directory.Delete(old.Dir, recursive: true); } catch { }
            }
        }

        private static bool Writing(string path)
        {
            var name = Path.GetFileName(path);
            return name.EndsWith(".lantern-work", StringComparison.OrdinalIgnoreCase) ||
                   name.EndsWith(".lantern-commit", StringComparison.OrdinalIgnoreCase);
        }

        /// <summary>Puts a backup's savedata back, after backing up the current one.</summary>
        public static void Restore(LauncherSettings s, Backup backup)
        {
            var from = Path.Combine(backup.Dir, "savedata");
            if (!Directory.Exists(from)) throw new DirectoryNotFoundException(L.T("This backup has no savedata folder."));
            if (!Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories).Any())
                throw new IOException(L.T("This backup is empty, so your saves were left as they are."));
            Create(s, beforeRestore: true);

            var target = SaveData(s);
            // Copied next to it first, so a failed copy leaves the current saves in place.
            var staging = target + ".restoring";
            if (Directory.Exists(staging)) Directory.Delete(staging, recursive: true);
            Copy(from, staging);
            if (Directory.Exists(target)) Directory.Delete(target, recursive: true);
            Directory.Move(staging, target);
        }

        /// <summary>A save import's staging folder (SaveImport): never part of the saves. One left by
        /// an older import held a whole Downloads folder, copied into every backup.</summary>
        private static bool Staging(string from, string path) =>
            Path.GetRelativePath(from, path).Split(Path.DirectorySeparatorChar)
                .Any(part => part.EndsWith(".importing", StringComparison.OrdinalIgnoreCase) || Writing(part));

        internal static void Copy(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (var dir in Directory.GetDirectories(from, "*", SearchOption.AllDirectories).Where(d => !Staging(from, d)))
                Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, dir)));
            foreach (var file in Directory.GetFiles(from, "*", SearchOption.AllDirectories).Where(f => !Staging(from, f)))
                File.Copy(file, Path.Combine(to, Path.GetRelativePath(from, file)), overwrite: true);
        }

        /// <summary>Names, sizes and times of the files: equal when nothing was saved since.</summary>
        internal static string Fingerprint(string dir)
        {
            var text = new StringBuilder();
            foreach (var file in Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Where(f => !Staging(dir, f)).OrderBy(f => f, StringComparer.Ordinal))
            {
                var info = new FileInfo(file);
                text.Append(Path.GetRelativePath(dir, file)).Append('|').Append(info.Length).Append('|')
                    .Append(info.LastWriteTimeUtc.Ticks).Append('\n');
            }
            return Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(text.ToString())));
        }

        private static string? ReadFingerprint(string backup)
        {
            try { return File.ReadAllText(Path.Combine(backup, FingerprintFile)).Trim(); }
            catch { return null; }
        }

        private static long Size(string dir)
        {
            try { return Directory.GetFiles(dir, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length); }
            catch { return 0; }
        }
    }
}
