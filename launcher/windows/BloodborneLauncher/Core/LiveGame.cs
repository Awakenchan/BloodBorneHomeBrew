using System.Globalization;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    public sealed record LiveGameInfo(uint Map, int Boss, int Level, long Echoes, int Insight)
    {
        public string Area => LiveGame.AreaName(Map);
        public string BossName => Boss > 0 ? LiveGame.BossName(Boss) : "";
    }

    public static partial class LiveGame
    {
        public const string Prefix = "Game status:";

        private static LiveGameInfo? _current;

        public static LiveGameInfo? Current => _current;

        public static event Action? Changed;

        public static readonly LiveGameInfo Example = new(0x18000000, -1, 52, 12480, 9);

        [GeneratedRegex(@"^Game status: map ([0-9a-f]{8}), boss (-?\d+), level (\d+), echoes (\d+), insight (\d+)$")]
        private static partial Regex Line();

        public static bool Take(string line)
        {
            if (!line.StartsWith(Prefix, StringComparison.Ordinal)) return false;
            var m = Line().Match(line);
            LiveGameInfo? next = null;
            if (m.Success)
                next = new LiveGameInfo(uint.Parse(m.Groups[1].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture),
                    int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture), int.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture),
                    long.Parse(m.Groups[4].Value, CultureInfo.InvariantCulture), int.Parse(m.Groups[5].Value, CultureInfo.InvariantCulture));
            if (next != _current)
            {
                _current = next;
                Changed?.Invoke();
            }
            return true;
        }

        public static void Clear()
        {
            if (_current == null) return;
            _current = null;
            Changed?.Invoke();
        }

        public static string AreaName(uint map)
        {
            int area = (int)(map >> 24), block = (int)(map >> 16) & 0xff;
            return (area, block) switch
            {
                (21, 1) => "Abandoned Old Workshop",
                (21, _) => "Hunter's Dream",
                (22, _) => "Hemwick Charnel Lane",
                (23, _) => "Old Yharnam",
                (24, 1) => "Central Yharnam",
                (24, 2) => "Upper Cathedral Ward",
                (24, _) => "Cathedral Ward",
                (25, _) => "Forsaken Cainhurst Castle",
                (26, _) => "Nightmare of Mensis",
                (27, _) => "Forbidden Woods",
                (28, _) => "Yahar'gul, Unseen Village",
                (29, _) => "Chalice Dungeon",
                (32, _) => "Byrgenwerth",
                (33, _) => "Nightmare Frontier",
                (34, _) => "Hunter's Nightmare",
                (35, _) => "Research Hall",
                (36, _) => "Fishing Hamlet",
                _ => "",
            };
        }

        public static string BossName(int id) => Bosses.TryGetValue(id, out var name) ? name : "a boss";

        private static readonly Dictionary<int, string> Bosses = new()
        {
            [1000] = "Madman's Escort", [1010] = "Forgotten Madman", [6400] = "Old Hunter Henryk",
            [6500] = "Valtr, Master of the League", [6505] = "Valtr, Beast Eater", [6530] = "Yamamura the Wanderer",
            [6540] = "Younger Madaras Twin", [6550] = "Mensis Scholar Damian", [6600] = "Henriett",
            [6640] = "Defector Antal", [7040] = "Tomb Prospector Olek", [7045] = "Tomb Prospector Gremia",
            [7060] = "Beastclaw Jozef", [7160] = "Queen Killer", [7300] = "Vitus", [7310] = "Madman Wallar",
            [101000] = "Degenerate Beast", [106000] = "Brainsucker", [127000] = "Maneater Boar",
            [209000] = "Blood-starved Beast", [209010] = "Blood-starved Beast", [210000] = "The Witch of Hemwick",
            [212010] = "Shadow of Yharnam", [212020] = "Shadow of Yharnam", [212030] = "Shadow of Yharnam",
            [216000] = "Keeper of the Old Lords", [218000] = "Loran Silverbeast", [232000] = "Martyr Logarius",
            [251000] = "Ebrietas, Daughter of the Cosmos", [251010] = "Ebrietas, Daughter of the Cosmos",
            [257000] = "Celestial Emissary", [257010] = "Celestial Emissary", [271000] = "Father Gascoigne",
            [272000] = "Father Gascoigne", [304000] = "Merciless Watcher", [304001] = "Watcher Chieftain",
            [304002] = "Merciless Watcher", [304003] = "Merciless Watcher", [305000] = "Pthumerian Descendant",
            [305001] = "Pthumerian Descendant", [305002] = "Pthumerian Elder", [306000] = "Pthumerian Bride",
            [306001] = "Yharnam, Pthumerian Queen", [313000] = "Undead Giant", [403000] = "Living Failures",
            [450000] = "Laurence, the First Vicar", [451000] = "Ludwig the Accursed", [451005] = "Ludwig, the Holy Blade",
            [452000] = "Lady Maria of the Astral Clocktower", [453000] = "Orphan of Kos", [454000] = "Orphan of Kos",
            [500000] = "Cleric Beast", [501000] = "Watchdog of the Old Lords", [502000] = "Vicar Amelia",
            [504000] = "Loran Darkbeast", [504010] = "Abhorrent Beast", [507000] = "The One Reborn",
            [508000] = "Darkbeast Paarl", [508010] = "Loran Darkbeast", [509000] = "Bloodletting Beast",
            [509010] = "Bloodletting Beast", [510000] = "Rom, the Vacuous Spider", [510010] = "Rom, the Vacuous Spider",
            [511000] = "Great One Beast", [512000] = "Amygdala", [512010] = "Amygdala", [540000] = "Moon Presence",
            [551000] = "Mergo's Wet Nurse", [551010] = "Mergo's Wet Nurse", [750000] = "Beast-possessed Soul",
            [802000] = "Slug Princess", [803000] = "Heroine Doll", [804000] = "Gehrman, the First Hunter",
            [805000] = "Gehrman, the First Hunter", [899000] = "Micolash, Host of the Nightmare",
            [899010] = "Vileblood Hunter Alfred", [900020] = "The Vileblood", [900030] = "The Executioners",
            [900040] = "Hunter of Hunters", [901040] = "Nameless Tomb Prospector", [901060] = "Izzy's Admirer",
            [901070] = "Nameless Madman", [901110] = "Bone Ash Hunter Carla", [901150] = "Drifting Vileblood Leo",
            [901160] = "Ardeo Man", [901170] = "Defeated Hunter", [901180] = "Choir Intelligencer Edgar",
        };
    }
}
