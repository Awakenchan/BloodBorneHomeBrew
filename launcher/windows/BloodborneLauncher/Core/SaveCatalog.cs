// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.IO.Compression;
using System.Text;

namespace BloodborneLauncher.Core
{
    public sealed record CatalogGoods(int Id, string Name, int MaxHeld, int MaxStored, int Sort, bool KeyItem, bool Dlc);
    public sealed record CatalogWeapon(int Id, string Name, int Durability, int Sort, IReadOnlyList<uint> GemSlots, bool Dlc);
    public sealed record CatalogArmor(int Id, string Name, int Durability, int Sort, bool Dlc);
    public sealed record CatalogLamp(int Flag, string Name, string Area, bool Dlc);

    public sealed class SaveCatalog
    {
        private const int ItemNames = 10;
        private const int WeaponNames = 11;
        private const int ArmorNames = 12;
        private const int NpcNames = 18;
        private const int PlaceNames = 19;
        private const int EventText = 30;
        private const int GestureFirst = 800;
        private const int GestureLast = 899;
        private const int UnusedGemsFirst = 1600;
        private const int UnusedGemsLast = 1699;
        private const int FirstRealWeapon = 2000000;
        private const uint NoGemSlot = 0x80000000;

        private static readonly (string Code, string Folder)[] Folders =
        {
            ("en", "engus"), ("de", "deude"), ("es", "spaes"), ("fr", "frafr"), ("it", "itait"), ("ja", "jpnjp"),
            ("ko", "korkr"), ("pl", "polpl"), ("pt-BR", "porbr"), ("ru", "rusru"), ("tr", "turtr"), ("zh-CN", "zhocn"),
        };

        private readonly Dictionary<int, Dictionary<int, string>> _text;

        public IReadOnlyList<CatalogGoods> Goods { get; }
        public IReadOnlyList<CatalogWeapon> Weapons { get; }
        public IReadOnlyList<CatalogArmor> Armor { get; }
        public IReadOnlyList<CatalogLamp> Lamps { get; }
        public IReadOnlyDictionary<int, CatalogGoods> GoodsById { get; }

        private SaveCatalog(Dictionary<int, Dictionary<int, string>> text, Dictionary<string, byte[]> parameters)
        {
            _text = text;
            var goods = ReadGoods(parameters["EquipParamGoods"]);
            Goods = goods;
            GoodsById = goods.ToDictionary(g => g.Id);
            Weapons = ReadWeapons(parameters["EquipParamWeapon"], parameters["WeaponGenParam"]);
            Armor = ReadArmor(parameters["EquipParamProtector"]);
            Lamps = SaveWorld.Lamps.Select(l => new CatalogLamp(l.Flag, LampName(l.Flag), PlaceName(l.Flag), SaveWorld.IsDlcLamp(l.Flag))).ToList();
        }

        public static SaveCatalog? TryLoad(string gameDir, string language, out string? problem)
        {
            problem = null;
            try
            {
                var root = Path.Combine(gameDir, "dvdroot_ps4");
                var folder = Folders.FirstOrDefault(f => f.Code == language).Folder ?? "engus";
                var msg = Path.Combine(root, "msg", folder);
                if (!Directory.Exists(msg)) msg = Path.Combine(root, "msg", "engus");
                var text = new Dictionary<int, Dictionary<int, string>>();
                foreach (var file in new[] { "item.msgbnd.dcx", "menu.msgbnd.dcx" })
                    foreach (var (id, _, data) in ReadBinder(Unpack(File.ReadAllBytes(Path.Combine(msg, file)))))
                        text[id] = ReadText(data);
                var parameters = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
                foreach (var (_, name, data) in ReadBinder(Unpack(File.ReadAllBytes(Path.Combine(root, "param", "gameparam", "gameparam.parambnd.dcx")))))
                    parameters[Path.GetFileNameWithoutExtension(name.Replace('\\', '/').Split('/').Last())] = data;
                return new SaveCatalog(text, parameters);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or KeyNotFoundException
                                          or ArgumentException or IndexOutOfRangeException or FormatException)
            {
                problem = e.Message;
                return null;
            }
        }

        public string ItemName(uint item)
        {
            var id = (int)(item & 0xFFFFFFF);
            return (item >> 28) switch
            {
                0 => Text(WeaponNames, id) ?? L.F($"Weapon {id}"),
                1 => Text(ArmorNames, id) ?? L.F($"Armor {id}"),
                4 => Text(ItemNames, id) ?? L.F($"Item {id}"),
                8 => L.T("Blood gem or rune"),
                _ => L.F($"Item {id}"),
            };
        }

        public string NpcName(int id, string fallback) => Text(NpcNames, id) ?? fallback;

        private string LampName(int flag)
        {
            var block = flag / 1000 % 10000;
            return Text(EventText, 70000000 + block / 10 * 10 + block % 10 + (flag % 100)) ?? L.F($"Lamp {flag}");
        }

        private string PlaceName(int flag)
        {
            var block = flag / 1000 % 10000;
            return Text(PlaceNames, block / 10 * 10) ?? Text(PlaceNames, block / 100 * 100) ?? "";
        }

        private string? Text(int table, int id)
        {
            if (!_text.TryGetValue(table, out var strings) || !strings.TryGetValue(id, out var value)) return null;
            value = value.Trim();
            return Usable(value) ? value : null;
        }

        private static bool Usable(string name) =>
            name.Length > 0 && !name.StartsWith('*') && !IsPlaceholder(name) && !name.Contains("NOT NEEDED", StringComparison.OrdinalIgnoreCase) &&
            !name.Contains("(Not needed)", StringComparison.OrdinalIgnoreCase);

        private static bool IsPlaceholder(string name) =>
            name.Length > 2 && name.StartsWith("en", StringComparison.Ordinal) && name.Skip(2).All(char.IsAsciiDigit);

        private List<CatalogGoods> ReadGoods(byte[] table)
        {
            var list = new List<CatalogGoods>();
            foreach (var (id, row) in ReadRows(table))
            {
                if (id is >= GestureFirst and <= GestureLast or >= UnusedGemsFirst and <= UnusedGemsLast) continue;
                var name = Text(ItemNames, id);
                if (name == null) continue;
                var max = BitConverter.ToInt16(table, row + 54);
                if (max <= 0) continue;
                list.Add(new CatalogGoods(id, name, max, BitConverter.ToInt16(table, row + 73), BitConverter.ToInt32(table, row + 28), table[row + 58] == 1, SaveWorld.IsDlcGoods(id)));
            }
            return list.OrderBy(g => g.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        private List<CatalogWeapon> ReadWeapons(byte[] table, byte[] gen)
        {
            var slots = ReadRows(gen).ToDictionary(r => r.Id, r => r.Row);
            var list = new List<CatalogWeapon>();
            foreach (var (id, row) in ReadRows(table))
            {
                if (id < FirstRealWeapon || !slots.TryGetValue(id, out var genRow)) continue;
                var name = Text(WeaponNames, id);
                if (name == null) continue;
                var gems = new List<uint>();
                var fixedGem = false;
                for (var slot = 0; slot < 5; slot++)
                {
                    var shape = BitConverter.ToUInt32(gen, genRow + 4 + slot * 8);
                    if (BitConverter.ToInt32(gen, genRow + 8 + slot * 8) >= 0) fixedGem = true;
                    gems.Add((shape & NoGemSlot) != 0 ? NoGemSlot : shape);
                }
                if (fixedGem) continue;
                var sort = BitConverter.ToInt32(table, row + 4);
                list.Add(new CatalogWeapon(id, name, BitConverter.ToUInt16(table, row + 190), sort < 0 ? -1 : sort * 100 - id % 10000, gems, SaveWorld.IsDlcWeapon(id)));
            }
            return list.OrderBy(w => w.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        private List<CatalogArmor> ReadArmor(byte[] table)
        {
            var list = new List<CatalogArmor>();
            foreach (var (id, row) in ReadRows(table))
            {
                var name = Text(ArmorNames, id);
                if (name == null) continue;
                var sort = BitConverter.ToInt32(table, row);
                list.Add(new CatalogArmor(id, name, 0, sort < 0 ? -1 : sort * 100, SaveWorld.IsDlcArmor(id)));
            }
            return list.OrderBy(a => a.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        private static List<(int Id, int Row)> ReadRows(byte[] table)
        {
            int count = BitConverter.ToUInt16(table, 0x0A);
            var rows = new List<(int, int)>(count);
            for (var i = 0; i < count; i++)
                rows.Add(((int)BitConverter.ToInt64(table, 0x40 + i * 0x18), (int)BitConverter.ToInt64(table, 0x48 + i * 0x18)));
            return rows;
        }

        private static byte[] Unpack(byte[] file)
        {
            if (file.Length < 0x50 || file[0] != 'D' || file[1] != 'C' || file[2] != 'X') throw new InvalidDataException("not DCX");
            using var stream = new ZLibStream(new MemoryStream(file, 0x4C, file.Length - 0x4C), CompressionMode.Decompress);
            using var output = new MemoryStream();
            stream.CopyTo(output);
            return output.ToArray();
        }

        private static IEnumerable<(int Id, string Name, byte[] Data)> ReadBinder(byte[] b)
        {
            if (b[0] != 'B' || b[1] != 'N' || b[2] != 'D' || b[3] != '4') throw new InvalidDataException("not BND4");
            var count = BitConverter.ToInt32(b, 0x0C);
            var header = (int)BitConverter.ToInt64(b, 0x10);
            var size = (int)BitConverter.ToInt64(b, 0x20);
            var unicode = b[0x28] != 0;
            for (var i = 0; i < count; i++)
            {
                var at = header + i * size;
                var length = (int)BitConverter.ToInt64(b, at + 8);
                var data = BitConverter.ToInt32(b, at + 24);
                var id = BitConverter.ToInt32(b, at + 28);
                var name = BitConverter.ToInt32(b, at + 32);
                var end = name;
                string text;
                if (unicode)
                {
                    while (b[end] != 0 || b[end + 1] != 0) end += 2;
                    text = Encoding.Unicode.GetString(b, name, end - name);
                }
                else
                {
                    while (b[end] != 0) end++;
                    text = Encoding.ASCII.GetString(b, name, end - name);
                }
                yield return (id, text, b.AsSpan(data, length).ToArray());
            }
        }

        private static Dictionary<int, string> ReadText(byte[] b)
        {
            var groups = BitConverter.ToInt32(b, 0x0C);
            var strings = (int)BitConverter.ToInt64(b, 0x18);
            var result = new Dictionary<int, string>();
            for (var g = 0; g < groups; g++)
            {
                var index = BitConverter.ToInt32(b, 0x28 + g * 16);
                var first = BitConverter.ToInt32(b, 0x2C + g * 16);
                var last = BitConverter.ToInt32(b, 0x30 + g * 16);
                for (var k = 0; k <= last - first; k++)
                {
                    var at = (int)BitConverter.ToInt64(b, strings + (index + k) * 8);
                    if (at == 0) continue;
                    var end = at;
                    while (b[end] != 0 || b[end + 1] != 0) end += 2;
                    result[first + k] = Encoding.Unicode.GetString(b, at, end - at);
                }
            }
            return result;
        }
    }
}
