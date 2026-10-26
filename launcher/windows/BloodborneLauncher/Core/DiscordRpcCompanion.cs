// Optional, Windows-only companion for the Bloodborne game process.
// The companion is launched only when the executable exists beside Lantern Engine.
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Threading.Tasks;

namespace BloodborneLauncher.Core
{
    internal static class DiscordRpcCompanion
    {
        /// <summary>
        /// Non-blocking: wait for the engine's real bb-probe.exe process, then
        /// start BloodborneRPC.exe with BB_GAME_PID so it can close itself.
        /// </summary>
        public static void StartForSession(
            Process launchProcess,
            string portDir,
            string binariesDir,
            bool builtInPresenceOff,
            Action<string> log)
        {
            // The C++ companion is Windows-only. Never interfere with game startup.
            if (!OperatingSystem.IsWindows() || !builtInPresenceOff)
                return;

            try
            {
                if (string.IsNullOrWhiteSpace(portDir) ||
                    string.IsNullOrWhiteSpace(binariesDir))
                    return;

                var rpcDir = Path.Combine(portDir, "discord_rpc");
                var rpcExe = Path.Combine(rpcDir, "BloodborneRPC.exe");
                var gameExe = Path.GetFullPath(Path.Combine(binariesDir, "bb-probe.exe"));

                // File presence acts as the opt-in for this first integration.
                if (!File.Exists(rpcExe) || !File.Exists(gameExe))
                    return;

                _ = Task.Run(() => WaitAndStartAsync(
                    launchProcess, rpcExe, rpcDir, gameExe, log));
            }
            catch (Exception ex)
            {
                log($"[Discord RPC] Could not initialize: {ex.Message}");
            }
        }

        private static async Task WaitAndStartAsync(
            Process launchProcess,
            string rpcExe,
            string rpcDir,
            string gameExe,
            Action<string> log)
        {
            try
            {
                // GameRunner starts bb-start.exe, not bb-probe.exe directly.
                // Never pass bb-start's PID to BloodborneRPC.
                while (!launchProcess.HasExited)
                {
                    var gamePid = FindGamePid(gameExe);
                    if (gamePid != 0)
                    {
                        var startInfo = new ProcessStartInfo
                        {
                            FileName = rpcExe,
                            WorkingDirectory = rpcDir,
                            UseShellExecute = false,
                            CreateNoWindow = true
                        };

                        startInfo.Environment["BB_GAME_PID"] =
                            gamePid.ToString(CultureInfo.InvariantCulture);

                        // Disposing the managed Process does NOT kill the child.
                        // The C++ companion watches BB_GAME_PID and exits cleanly.
                        using var rpc = Process.Start(startInfo);
                        if (rpc != null)
                            log($"[Discord RPC] Started (game PID {gamePid}).");
                        else
                            log("[Discord RPC] Process.Start returned null.");
                        return;
                    }

                    await Task.Delay(500).ConfigureAwait(false);
                }
            }
            catch (InvalidOperationException ex)
            {
                log($"[Discord RPC] Process error: {ex.Message}");
            }
            catch (Win32Exception ex)
            {
                log($"[Discord RPC] Windows error: {ex.Message}");
            }
            catch (Exception ex)
            {
                // RPC failure must never terminate or block the game.
                log($"[Discord RPC] Error: {ex.Message}");
            }
        }

        private static int FindGamePid(string expectedGameExe)
        {
            foreach (var candidate in Process.GetProcessesByName("bb-probe"))
            {
                using (candidate)
                {
                    try
                    {
                        if (candidate.HasExited)
                            continue;

                        var image = candidate.MainModule?.FileName;
                        if (image == null)
                            continue;

                        // Do not attach to another bb-probe from a different port.
                        if (string.Equals(Path.GetFullPath(image), expectedGameExe,
                                StringComparison.OrdinalIgnoreCase))
                            return candidate.Id;
                    }
                    catch (Win32Exception) { /* Process not readable; retry later. */ }
                    catch (InvalidOperationException) { /* Process exited. */ }
                }
            }
            return 0;
        }
    }
}
