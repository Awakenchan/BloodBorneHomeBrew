using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace BloodborneLauncher.Core
{
    public static class TestedSettings
    {
        public sealed record Setting(string Key, string Title, string Tested, string Default, bool Launcher = false);

        private const string AskedKey = "tested_settings_asked";

        public static readonly Setting[] All =
        {
            new("effect_dynamic_shadows", L.T("Shadows from dynamic lights"), "0", "1"),
            new("reflex", L.T("NVIDIA Reflex"), "on", "on"),
            new("reactive", L.T("Reactive mask"), "0", "0"),
            new("dlss_preset", L.T("DLSS preset"), "k", "k"),
            new("dlss_auto_exposure", L.T("DLSS auto exposure"), "0", "0"),
            new("exp_vram_cleanup", L.T("VRAM cleanup"), "1", "1", true),
            new("exp_frame_pacing", L.T("Precise frame pacing"), "1", "1", true),
            new("exp_vram_budget", L.T("Use more VRAM for textures"), "1", "1", true),
            new("exp_thread_priority", L.T("Graphics threads first"), "1", "1", true),
            new("exp_sleep_spin", L.T("Shorter sleep spin"), "1", "1", true),
            new("exp_async_pipelines", L.T("Background shader compiling"), "1", "1", true),
            new("exp_fast_draws", L.T("Faster draws"), "1", "1", true),
            new("exp_compile_skip", L.T("Skip draws while shaders compile"), "1", "1", true),
            new("exp_fast_loading", L.T("Faster loading"), "1", "1", true),
            new("exp_hot_pages", L.T("Hot memory pages"), "1", "1", true),
            new("exp_guest_in_place", L.T("PC memory model"), "1", "1", true),
            new("exp_native_calls", L.T("Direct draw calls"), "1", "1", true),
            new("exp_occlusion", L.T("Occlusion culling"), "0", "0", true),
            new("legacy_shaders", L.T("Legacy shader compiler"), "0", "0", true),
        };

        public static string Folder => Path.Combine(Paths.ConfigDir, "my-settings");

        private static string Current(Setting setting, IniFile ini, LauncherSettings s) =>
            setting.Launcher ? s.Bool(setting.Key) ? "1" : "0" : ini.Get(setting.Key, setting.Default);

        public static List<Setting> Differences(IniFile ini, LauncherSettings s) =>
            All.Where(setting => Current(setting, ini, s) != setting.Tested).ToList();

        public static bool ShouldAsk(IniFile ini, LauncherSettings s) =>
            Updater.CurrentBeta > 0 && s.Str(AskedKey) != Updater.CurrentLabel &&
            s.Str(AskedKey) != Updater.Label(Updater.Current, Updater.CurrentBeta / 100 * 100) && Differences(ini, s).Count > 0;

        public static void MarkAsked(LauncherSettings s) => s.Set(AskedKey, Updater.CurrentLabel);

        public static string Describe(Setting setting, IniFile ini, LauncherSettings s) =>
            L.F($"•  {setting.Title}: yours {Shown(Current(setting, ini, s))}, tested {Shown(setting.Tested)}");

        private static string Shown(string value) => value switch
        {
            "1" or "on" => L.T("On"),
            "0" or "off" => L.T("Off"),
            _ => value.Length > 0 ? char.ToUpperInvariant(value[0]) + value[1..] : value,
        };

        public static string? SavedFile()
        {
            if (!Directory.Exists(Folder)) return null;
            return Directory.GetFiles(Folder, "before-*.json")
                .OrderByDescending(File.GetLastWriteTimeUtc)
                .FirstOrDefault();
        }

        public static string Use(IniFile ini, LauncherSettings s)
        {
            var differences = Differences(ini, s);
            var file = Save(differences, ini, s);
            foreach (var setting in differences)
            {
                if (setting.Launcher)
                    s.Set(setting.Key, setting.Tested == "1");
                else
                    ini.Set(setting.Key, setting.Tested);
            }
            return file;
        }

        private static string Save(List<Setting> settings, IniFile ini, LauncherSettings s)
        {
            Directory.CreateDirectory(Folder);
            var name = new string(Updater.CurrentLabel.Select(c => char.IsLetterOrDigit(c) || c == '.' ? c : '-').ToArray());
            var file = Path.Combine(Folder, $"before-{name}.json");
            var iniValues = new JsonObject();
            var launcherValues = new JsonObject();
            if (File.Exists(file) && JsonNode.Parse(File.ReadAllText(file)) is JsonObject saved)
            {
                if (saved["ini"] is JsonObject oldIni) iniValues = (JsonObject)oldIni.DeepClone();
                if (saved["launcher"] is JsonObject oldLauncher) launcherValues = (JsonObject)oldLauncher.DeepClone();
            }
            foreach (var setting in settings)
            {
                if (setting.Launcher)
                {
                    if (!launcherValues.ContainsKey(setting.Key)) launcherValues[setting.Key] = s.Bool(setting.Key);
                }
                else if (!iniValues.ContainsKey(setting.Key))
                {
                    iniValues[setting.Key] = ini.Raw(setting.Key);
                }
            }
            var root = new JsonObject { ["from"] = Updater.CurrentLabel, ["ini"] = iniValues, ["launcher"] = launcherValues };
            var staging = file + ".saving";
            File.WriteAllText(staging, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
            File.Move(staging, file, overwrite: true);
            return file;
        }

        public static int Restore(IniFile ini, LauncherSettings s)
        {
            var file = SavedFile() ?? throw new FileNotFoundException(L.T("No saved settings were found."));
            if (JsonNode.Parse(File.ReadAllText(file)) is not JsonObject root)
                throw new InvalidDataException(L.T("The saved settings are damaged."));
            var restored = 0;
            foreach (var setting in All)
            {
                if (setting.Launcher)
                {
                    if (root["launcher"] is JsonObject values && values[setting.Key] is JsonValue value && value.TryGetValue<bool>(out var on))
                    {
                        s.Set(setting.Key, on);
                        restored++;
                    }
                }
                else if (root["ini"] is JsonObject values && values.ContainsKey(setting.Key))
                {
                    ini.Set(setting.Key, values[setting.Key] is JsonValue value && value.TryGetValue<string>(out var text) ? text : null);
                    restored++;
                }
            }
            return restored;
        }
    }
}
