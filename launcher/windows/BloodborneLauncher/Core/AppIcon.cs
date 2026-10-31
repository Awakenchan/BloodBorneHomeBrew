using System.IO;
#if AVALONIA
using Avalonia;
using Avalonia.Media.Imaging;
using Window = Avalonia.Controls.Window;
#else
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
#endif

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Settings › Appearance › App icon (app_icon): the launcher window's and the game window's
    /// icon. The purple lantern is the main one and the orange lantern its variant;
    /// Your picture is one the player chose, made into an icon file. The exe file keeps the build's
    /// icon whatever is chosen.
    /// </summary>
    public static class AppIcon
    {
        public static readonly Choice[] Choices =
        {
            new(L.T("Purple lantern"), "auto"),
            new(L.T("Orange lantern"), "orange"),
            new(L.T("Your picture"), "custom"),
        };

        /// <summary>Your picture as an icon file, made when it is chosen (the picture itself may move).</summary>
        public static string CustomFile => Path.Combine(Paths.ConfigDir, "app-icon.ico");

        /// <summary>The sizes Your picture is drawn at, so small icons stay sharp.</summary>
        private static readonly int[] Sizes = { 16, 24, 32, 48, 64, 128, 256 };

        private static string Resource(string choice) => choice switch
        {
            "orange" => "lantern-orange.ico",
            _ => "app.ico",
        };

        public static string? Backdrop(string mode) => mode switch
        {
            "lantern" => "lantern-purple.png",
            "lantern-orange" => "lantern-orange.png",
            _ => null,
        };

        /// <summary>The chosen icon's .ico file, as the launcher carries it (Your picture: as made,
        /// or the build's own while none is made).</summary>
        public static byte[]? Bytes(string choice)
        {
            try
            {
                if (choice == "custom")
                    return File.Exists(CustomFile) ? File.ReadAllBytes(CustomFile) : Bytes("auto");
#if AVALONIA
                using var source = typeof(AppIcon).Assembly.GetManifestResourceStream(Resource(choice));
#else
                using var source = Application.GetResourceStream(new Uri($"pack://application:,,,/{Resource(choice)}"))?.Stream;
#endif
                if (source == null) return null;
                using var memory = new MemoryStream();
                source.CopyTo(memory);
                return memory.ToArray();
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

        /// <summary>The launcher window shows the chosen icon; the game window takes it at its next start.</summary>
        public static void Apply(Window? window, string choice)
        {
            if (window == null) return;
            try
            {
#if AVALONIA
                if (Bytes(choice) is { } bytes)
                    window.Icon = new Avalonia.Controls.WindowIcon(new MemoryStream(bytes));
#else
                if (choice == "custom" && File.Exists(CustomFile))
                    window.Icon = BitmapFrame.Create(new MemoryStream(File.ReadAllBytes(CustomFile)), BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
                else
                    window.Icon = BitmapFrame.Create(new Uri($"pack://application:,,,/{Resource(choice == "custom" ? "auto" : choice)}"));
#endif
            }
            catch (Exception)
            {
                // The window keeps the icon it has.
            }
        }

        /// <summary>The game window's icon file (engine\out\bloodborne.ico), when it differs.</summary>
        public static void SyncGameWindow(string package, string choice)
        {
            try
            {
                var target = Path.Combine(Paths.EngineDirOf(package), "out", "bloodborne.ico");
                if (!Directory.Exists(Path.GetDirectoryName(target))) return;
                if (Bytes(choice) is not { } bytes) return;
                if (!File.Exists(target) || !File.ReadAllBytes(target).AsSpan().SequenceEqual(bytes))
                    File.WriteAllBytes(target, bytes);
            }
            catch (Exception)
            {
                // The old icon stays: nothing else depends on it.
            }
        }

        private static string ShortcutMark => Path.Combine(Paths.ConfigDir, "shortcut-icon.txt");

        public static void SyncShortcuts(string package, string choice)
        {
            if (!OperatingSystem.IsWindows()) return;
            var bytes = choice == "auto" ? null : Bytes(choice);
            var drawn = Bytes(choice);
            var sign = choice + ":" + (drawn == null ? "" : Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(drawn))[..12]);
            Task.Run(() =>
            {
                try
                {
                    var mark = File.Exists(ShortcutMark) ? File.ReadAllText(ShortcutMark) : "";
                    if (mark == sign + "|" + package) return;
                    if (OperatingSystem.IsWindows()) RestyleShortcuts(package, bytes, sign);
                    File.WriteAllText(ShortcutMark, sign + "|" + package);
                }
                catch (Exception)
                {
                }
            });
        }

        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
        private static void RestyleShortcuts(string package, byte[]? bytes, string sign)
        {
            string? icon = null;
            if (bytes != null)
            {
                Directory.CreateDirectory(Paths.ConfigDir);
                icon = Path.Combine(Paths.ConfigDir, "shortcut-icon-" + sign[(sign.IndexOf(':') + 1)..] + ".ico");
                if (!File.Exists(icon)) File.WriteAllBytes(icon, bytes);
            }
            var exes = new[] { Environment.ProcessPath, Path.Combine(package, "Lantern Engine.exe"), Path.Combine(package, "Bloodborne.exe") }
                .Where(e => e != null && File.Exists(e)).Select(e => Path.GetFullPath(e!)).Distinct(StringComparer.OrdinalIgnoreCase);
            foreach (var exe in exes)
                LanternRename.Shortcuts.Restyle(LanternRename.Shortcuts.Pointing(exe), icon ?? exe);
            foreach (var old in Directory.EnumerateFiles(Paths.ConfigDir, "shortcut-icon-*.ico"))
                if (!string.Equals(old, icon, StringComparison.OrdinalIgnoreCase))
                    try { File.Delete(old); } catch (Exception) { }
            SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero);
        }

        [System.Runtime.InteropServices.DllImport("shell32.dll")]
        private static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);

        /// <summary>Makes a picture Your picture's icon file: an .ico as it is, any other picture fitted
        /// into a square at each size (PNG images inside the .ico). False when it cannot be read.</summary>
        public static bool MakeCustom(string picture)
        {
            try
            {
                var bytes = File.ReadAllBytes(picture);
                // An icon file already: 0, 1 (icon), then at least one image.
                if (bytes.Length > 6 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1 && bytes[3] == 0 && bytes[4] + bytes[5] > 0)
                {
                    Directory.CreateDirectory(Paths.ConfigDir);
                    File.WriteAllBytes(CustomFile, bytes);
                    return true;
                }
                var images = Sizes.Select(size => (size, Png: Draw(bytes, size))).ToList();
                Directory.CreateDirectory(Paths.ConfigDir);
                File.WriteAllBytes(CustomFile, Ico(images));
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        /// <summary>The picture fitted into a size × size square (centred, the rest transparent), as a PNG.</summary>
        private static byte[] Draw(byte[] picture, int size)
        {
#if AVALONIA
            using var source = new Bitmap(new MemoryStream(picture));
            var (width, height) = Fit(source.PixelSize.Width, source.PixelSize.Height, size);
            using var scaled = source.CreateScaledBitmap(new PixelSize(width, height), BitmapInterpolationMode.HighQuality);
            using var target = new RenderTargetBitmap(new PixelSize(size, size), new Vector(96, 96));
            using (var context = target.CreateDrawingContext())
                context.DrawImage(scaled, new Rect((size - width) / 2, (size - height) / 2, width, height));
            using var png = new MemoryStream();
            target.Save(png);
            return png.ToArray();
#else
            var source = BitmapFrame.Create(new MemoryStream(picture), BitmapCreateOptions.None, BitmapCacheOption.OnLoad);
            var (width, height) = Fit(source.PixelWidth, source.PixelHeight, size);
            var visual = new DrawingVisual();
            RenderOptions.SetBitmapScalingMode(visual, BitmapScalingMode.HighQuality);
            using (var context = visual.RenderOpen())
                context.DrawImage(source, new Rect((size - width) / 2, (size - height) / 2, width, height));
            var target = new RenderTargetBitmap(size, size, 96, 96, PixelFormats.Pbgra32);
            target.Render(visual);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(target));
            using var png = new MemoryStream();
            encoder.Save(png);
            return png.ToArray();
#endif
        }

        /// <summary>A width × height picture scaled to fit a size × size square, in whole pixels.</summary>
        private static (int Width, int Height) Fit(int width, int height, int size)
        {
            var scale = Math.Min((double)size / width, (double)size / height);
            return (Math.Clamp((int)Math.Round(width * scale), 1, size), Math.Clamp((int)Math.Round(height * scale), 1, size));
        }

        /// <summary>An .ico file of PNG images (Windows Vista and later read them at any size).</summary>
        private static byte[] Ico(IReadOnlyList<(int Size, byte[] Png)> images)
        {
            using var file = new MemoryStream();
            using var writer = new BinaryWriter(file);
            writer.Write((short)0);
            writer.Write((short)1); // icon
            writer.Write((short)images.Count);
            var offset = 6 + 16 * images.Count;
            foreach (var (size, png) in images)
            {
                writer.Write((byte)(size >= 256 ? 0 : size)); // 0 is 256
                writer.Write((byte)(size >= 256 ? 0 : size));
                writer.Write((byte)0); // no palette
                writer.Write((byte)0);
                writer.Write((short)1); // planes
                writer.Write((short)32); // bits per pixel
                writer.Write(png.Length);
                writer.Write(offset);
                offset += png.Length;
            }
            foreach (var (_, png) in images)
                writer.Write(png);
            writer.Flush();
            return file.ToArray();
        }
    }
}
