// SPDX-License-Identifier: GPL-3.0-or-later
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace BloodborneLauncher.Core
{
    public static class AmdFsrDriver
    {
        public const string Switch = "LANTERN_FSR_AMD_DRIVER";
        private const string Provider = "amdvkffx64.dll";

        private static bool _looked;
        private static string? _found;

        public static string? ProviderPath()
        {
            if (_looked) return _found;
            _looked = true;
            try
            {
                _found = OperatingSystem.IsWindows() ? Find() : null;
            }
            catch
            {
                _found = null;
            }
            return _found;
        }

        public static string DriverVersion(string path)
        {
            try
            {
                return FileVersionInfo.GetVersionInfo(path).FileVersion ?? "";
            }
            catch
            {
                return "";
            }
        }

        [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool SetupGetInfDriverStoreLocationW(string fileName, IntPtr platform, string? locale,
            [Out] char[] buffer, int size, out int required);

        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
        private static string? Find()
        {
            var gpu = Hardware.Detect();
            if (gpu.Vendor != "AMD") return null;
            using var adapters = Registry.LocalMachine.OpenSubKey(
                @"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}");
            if (adapters == null) return null;
            var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
            foreach (var sub in adapters.GetSubKeyNames())
            {
                if (sub.Length != 4 || !sub.All(char.IsDigit)) continue;
                using var key = adapters.OpenSubKey(sub);
                if (key == null) continue;
                var id = (key.GetValue("MatchingDeviceId") as string ?? "").ToUpperInvariant();
                if (!id.Contains("VEN_1002")) continue;
                if (key.GetValue("DriverDesc") is not string desc || !desc.Trim().Equals(gpu.Gpu, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (key.GetValue("InfPath") is not string inf) continue;
                var buffer = new char[1024];
                if (!SetupGetInfDriverStoreLocationW(Path.Combine(windows, "INF", inf), IntPtr.Zero, null, buffer, buffer.Length, out _))
                    continue;
                var stored = new string(buffer).TrimEnd('\0');
                var dll = Path.Combine(Path.GetDirectoryName(stored) ?? "", Provider);
                if (File.Exists(dll)) return dll;
            }
            return null;
        }
    }
}
