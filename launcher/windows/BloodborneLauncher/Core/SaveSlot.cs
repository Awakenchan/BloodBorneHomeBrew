// SPDX-License-Identifier: GPL-3.0-or-later
using System.Text;

namespace BloodborneLauncher.Core
{
    public enum SaveStat { Vitality, Endurance, Strength, Skill, Bloodtinge, Arcane }

    public enum SaveList { Inventory, KeyItems, Storage }

    public sealed record SaveItem(SaveList List, int Position, int Offset, uint Handle, uint ItemId, int Quantity, uint Packed)
    {
        public int Category => (int)(ItemId >> 28);
        public int Id => (int)(ItemId & 0xFFFFFFF);
    }

    public sealed record GearInstance(uint Handle, uint ItemId, int Offset, int Size);

    public sealed class SaveFormatException : Exception
    {
        public SaveFormatException(string message) : base(message) { }
    }

    public sealed class SaveSlot
    {
        public const int FileSize = 0x140000;
        public const int TableSlots = 0x1000;
        public const int InventoryCapacity = 0x7C0;
        public const int KeyCapacity = 0x40;
        public const int StorageCapacity = 0x840;
        public const int FlagPoolBytes = 150000;
        public const int FlagBlocks = 1200;
        public const int FlagsPerBlock = 1000;
        public const int MinTailAfterGrowth = 4096;
        public const int MaxEchoes = 999999999;
        public const int MaxInsight = 99;
        public const int MaxStat = 99;

        private const uint Version = 0x41;
        private const int TableStart = 0x54;
        private const int EmptySize = 8;
        private const int GemSize = 0x28;
        private const int GearSize = 0x3C;
        private const int Growth = GearSize - EmptySize;
        private const uint NoGemSlot = 0x80000000;
        private const uint WeaponHandle = 0x80800000;
        private const uint ArmorHandle = 0x90800000;
        private const uint GoodsHandle = 0xB0000000;
        private const uint GoodsItem = 0x40000000;
        private const uint ArmorItem = 0x10000000;
        private const int KeyIndexBase = 0;
        private const int InventoryIndexBase = KeyCapacity;
        private const int RecordSize = 16;
        private const int InventoryCountAt = 0x25C;
        private const int FaceSize = 0xF0;
        private const int LevelAt = 0x80;
        private const int EchoesAt = 0x84;
        private const int InsightAt = 0x74;
        private const int NameAt = 0x98;
        private const int NameBytes = 0x20;
        private const int TrailingBytes = 0x1027;

        private static readonly int[] StatAt = { 0x30, 0x38, 0x48, 0x50, 0x58, 0x60 };
        private static readonly int[] GrowingHeaderFields = { 4, 6, 7, 9, 11, 13, 15, 17, 19 };
        private static readonly byte[] FaceTag = Encoding.ASCII.GetBytes("FACE");
        private static readonly (int Area, int Block)[] FlagAreas =
        {
            (21, 0), (21, 1), (22, 0), (23, 0), (24, 0), (24, 1), (24, 2), (25, 0), (26, 0), (27, 0), (28, 0), (29, 0),
            (32, 0), (33, 0), (34, 0), (35, 0), (36, 0),
        };
        private static readonly int[] AreaFlagTypes = { 1, 5, 6, 7, 9 };
        private static readonly int[] GlobalFlagTypes = { 0, 1, 5, 6, 7 };

        private readonly byte[] _bytes;
        private readonly List<GearInstance> _table = new();
        private readonly List<SaveItem> _items = new();
        private int _player;
        private int _keyCountAt;
        private int _storageCountAt;

        private SaveSlot(byte[] bytes)
        {
            _bytes = bytes;
            Walk();
        }

        public static SaveSlot Parse(byte[] bytes)
        {
            if (bytes.Length != FileSize) throw new SaveFormatException(L.T("This save file has an unexpected size."));
            return new SaveSlot((byte[])bytes.Clone());
        }

        public static bool IsEmptySlot(byte[] bytes) => bytes.Length == FileSize && Array.TrueForAll(bytes[..64], b => b == 0);

        public byte[] ToBytes() => (byte[])_bytes.Clone();

        public IReadOnlyList<SaveItem> Items => _items;
        public IReadOnlyList<GearInstance> Gear => _table;
        public string Name
        {
            get
            {
                var text = Encoding.Unicode.GetString(_bytes, _player + NameAt, NameBytes);
                var end = text.IndexOf('\0');
                return end >= 0 ? text[..end] : text;
            }
        }
        public int Level => I32(_player + LevelAt);
        public int Echoes => I32(_player + EchoesAt);
        public int Insight => I32(_player + InsightAt);
        public uint MapId => U32(4);
        public long PlayMilliseconds => U32(8);
        public int FlagPool => H(13);
        public int LogicalEnd => H(19) + H(20) + TrailingBytes;
        public int GearRoom => Math.Max(0, (FileSize - LogicalEnd - MinTailAfterGrowth) / Growth);

        public int Stat(SaveStat stat) => I32(_player + StatAt[(int)stat]);

        public void SetStat(SaveStat stat, int value)
        {
            if (value < 1 || value > MaxStat) throw new ArgumentOutOfRangeException(nameof(value));
            var level = Level + value - Stat(stat);
            if (level < 1) throw new ArgumentOutOfRangeException(nameof(value));
            W32(_player + StatAt[(int)stat], value);
            W32(_player + LevelAt, level);
        }

        public void SetEchoes(int value)
        {
            if (value < 0 || value > MaxEchoes) throw new ArgumentOutOfRangeException(nameof(value));
            W32(_player + EchoesAt, value);
        }

        public void SetInsight(int value)
        {
            if (value < 0 || value > MaxInsight) throw new ArgumentOutOfRangeException(nameof(value));
            W32(_player + InsightAt, value);
        }

        public static int? FlagBlock(int flag)
        {
            if (flag < 0) return null;
            var block = flag / FlagsPerBlock;
            var type = block / 10000;
            var area = block / 100 % 100;
            var sub = block / 10 % 10;
            var zone = block % 10;
            if (area == 0 && sub == 0)
            {
                var global = Array.IndexOf(GlobalFlagTypes, type);
                return global < 0 ? null : global + zone * 5;
            }
            var typeIndex = Array.IndexOf(AreaFlagTypes, type);
            if (typeIndex < 0) return null;
            int areaIndex;
            if (area == 40 && sub == 0) areaIndex = 11;
            else if (area >= 41 && area <= 47 && sub == 0) areaIndex = area - 24;
            else
            {
                areaIndex = Array.IndexOf(FlagAreas, (area, sub));
                if (areaIndex < 0) return null;
            }
            return zone * 115 + 27 + 23 * (typeIndex + 1) + areaIndex;
        }

        public bool Flag(int flag)
        {
            var (at, mask) = FlagBit(flag);
            return (_bytes[at] & mask) != 0;
        }

        public void SetFlag(int flag, bool on)
        {
            var (at, mask) = FlagBit(flag);
            if (on) _bytes[at] |= mask;
            else _bytes[at] &= (byte)~mask;
        }

        private (int At, byte Mask) FlagBit(int flag)
        {
            var block = FlagBlock(flag) ?? throw new ArgumentOutOfRangeException(nameof(flag));
            var within = flag % FlagsPerBlock;
            return (FlagPool + block * (FlagPoolBytes / FlagBlocks) + (within >> 3), (byte)(1 << (7 - (within & 7))));
        }

        public void SetQuantity(SaveItem item, int quantity)
        {
            if (quantity < 1) throw new ArgumentOutOfRangeException(nameof(quantity));
            var current = _items.FirstOrDefault(i => i.Offset == item.Offset && i.ItemId == item.ItemId && i.List == item.List)
                          ?? throw new InvalidOperationException();
            W32(current.Offset + 8, quantity);
            _items[_items.IndexOf(current)] = current with { Quantity = quantity };
        }

        public void AddGoods(int id, int quantity, int sort, bool keyItem)
        {
            if (quantity < 1) throw new ArgumentOutOfRangeException(nameof(quantity));
            var item = GoodsItem | (uint)id;
            var list = keyItem ? SaveList.KeyItems : SaveList.Inventory;
            var existing = _items.FirstOrDefault(i => i.List == list && i.ItemId == item);
            if (existing != null)
            {
                SetQuantity(existing, existing.Quantity + quantity);
                return;
            }
            AddRecord(list, GoodsHandle | (uint)id, item, quantity, sort);
        }

        public void AddWeapon(int id, int durability, int sort, IReadOnlyList<uint> gemSlots)
        {
            AddGear(WeaponHandle, (uint)id, durability, sort, gemSlots);
        }

        public void AddArmor(int id, int durability, int sort)
        {
            AddGear(ArmorHandle, ArmorItem | (uint)id, durability, sort, Array.Empty<uint>());
        }

        private void AddGear(uint handleKind, uint item, int durability, int sort, IReadOnlyList<uint> gemSlots)
        {
            if (_items.Count(i => i.List == SaveList.Inventory) >= InventoryCapacity)
                throw new InvalidOperationException(L.T("The inventory is full."));
            if (GearRoom < 1)
                throw new InvalidOperationException(L.T("This save has no room left for more gear."));

            var gems = _table.Where(e => e.Handle >> 24 == 0xC0).Select(e => e.Handle & 0xFFFF).ToHashSet();
            uint index = 0;
            GearInstance? free = null;
            foreach (var entry in _table.Where(e => e.Handle >> 24 != 0xC0))
            {
                while (gems.Contains(index)) index++;
                if (entry.Handle == 0)
                {
                    free = entry;
                    break;
                }
                index++;
            }
            if (free == null) throw new InvalidOperationException(L.T("This save has no room left for more gear."));

                        var after = free.Offset + EmptySize;
            Buffer.BlockCopy(_bytes, after, _bytes, after + Growth, LogicalEnd - after);
            var handle = handleKind | index;
            W32U(free.Offset, handle);
            W32U(free.Offset + 4, item);
            W32(free.Offset + 8, durability);
            W32(free.Offset + 12, 0);
            W32(free.Offset + 16, 1);
            for (var slot = 0; slot < 5; slot++)
            {
                W32U(free.Offset + 20 + slot * 8, slot < gemSlots.Count ? gemSlots[slot] : NoGemSlot);
                W32U(free.Offset + 24 + slot * 8, 0);
            }
            foreach (var field in GrowingHeaderFields)
                W32U(field * 4, U32(field * 4) + Growth);
            Walk();
            AddRecord(SaveList.Inventory, handle, item, 1, sort);
        }

        private void AddRecord(SaveList list, uint handle, uint item, int quantity, int sort)
        {
            var (first, capacity, countAt, indexBase) = list switch
            {
                SaveList.KeyItems => (_keyCountAt + 4, KeyCapacity, _keyCountAt, KeyIndexBase),
                SaveList.Inventory => (_player + InventoryCountAt + 4, InventoryCapacity, _player + InventoryCountAt, InventoryIndexBase),
                _ => throw new ArgumentOutOfRangeException(nameof(list)),
            };
            for (var position = 0; position < capacity; position++)
            {
                var at = first + position * RecordSize;
                if (U32(at) != 0) continue;
                var packed = ((uint)sort << 12) | (uint)(position + indexBase);
                W32U(at, handle);
                W32U(at + 4, item);
                W32(at + 8, quantity);
                W32U(at + 12, packed);
                W32(countAt, I32(countAt) + 1);
                _items.Add(new SaveItem(list, position, at, handle, item, quantity, packed));
                return;
            }
            throw new InvalidOperationException(L.T("The inventory is full."));
        }

        private void Walk()
        {
            _table.Clear();
            _items.Clear();
            if (U32(0) != Version) throw new SaveFormatException(L.T("This save is from a game version the editor does not know."));
            CheckHeader();
            var at = TableStart;
            for (var slot = 0; slot < TableSlots; slot++)
            {
                Need(at + 8);
                var handle = U32(at);
                var item = U32(at + 4);
                int size;
                if (handle == 0 && item == 0xFFFFFFFF) size = EmptySize;
                else if (handle >> 24 == 0xC0) size = GemSize;
                else if (handle >> 24 is 0x80 or 0x90) size = GearSize;
                else throw new SaveFormatException(L.T("The save's item table has an entry the editor does not know."));
                _table.Add(new GearInstance(handle, item, at, size));
                at += size;
            }
            _player = at;

            ReadList(SaveList.Inventory, _player + InventoryCountAt, InventoryCapacity);
            _keyCountAt = _player + InventoryCountAt + 4 + InventoryCapacity * RecordSize;
            ReadList(SaveList.KeyItems, _keyCountAt, KeyCapacity);

            var face = IndexOf(FaceTag, _keyCountAt + 4 + KeyCapacity * RecordSize, H(9));
            if (face < 0) throw new SaveFormatException(L.T("The save's storage box could not be found."));
            _storageCountAt = face + FaceSize;
            ReadList(SaveList.Storage, _storageCountAt, StorageCapacity);
            if (_storageCountAt + 4 + StorageCapacity * RecordSize > H(9))
                throw new SaveFormatException(L.T("The save's storage box runs past its section."));
        }

        private void ReadList(SaveList list, int countAt, int capacity)
        {
            Need(countAt + 4 + capacity * RecordSize);
            var used = 0;
            for (var position = 0; position < capacity; position++)
            {
                var at = countAt + 4 + position * RecordSize;
                var handle = U32(at);
                if (handle == 0) continue;
                _items.Add(new SaveItem(list, position, at, handle, U32(at + 4), I32(at + 8), U32(at + 12)));
                used++;
            }
            if (used != I32(countAt)) throw new SaveFormatException(L.T("A list in this save does not match its count."));
        }

        private void CheckHeader()
        {
            var table = H(6);
            var fine = H(3) == TableStart && H(5) == TableStart &&
                       table > 0 && H(4) == table + 241 && H(7) == TableStart + table + 158 && H(8) == 0 &&
                       H(9) == TableStart + table && H(10) == 150 && H(11) == TableStart + table + 150 && H(12) == 8 &&
                       H(13) == TableStart + table + 241 && H(14) == FlagPoolBytes + 1 && H(15) == H(13) + H(14) &&
                       H(17) == H(15) + H(16) && H(18) == H(16) + 80 && H(19) == H(15) + H(18) && H(20) == 0x20004 &&
                       H(19) + H(20) + TrailingBytes <= FileSize;
            if (!fine) throw new SaveFormatException(L.T("The save's section table does not look as expected."));
        }

        private int IndexOf(byte[] tag, int from, int to)
        {
            for (var i = from; i + tag.Length <= to && i + tag.Length <= _bytes.Length; i++)
                if (_bytes.AsSpan(i, tag.Length).SequenceEqual(tag))
                    return i;
            return -1;
        }

        private void Need(int end)
        {
            if (end > _bytes.Length) throw new SaveFormatException(L.T("The save ends sooner than its tables say."));
        }

        private int H(int field) => I32(field * 4);
        private uint U32(int at) => BitConverter.ToUInt32(_bytes, at);
        private int I32(int at) => BitConverter.ToInt32(_bytes, at);
        private void W32(int at, int value) => BitConverter.TryWriteBytes(_bytes.AsSpan(at, 4), value);
        private void W32U(int at, uint value) => BitConverter.TryWriteBytes(_bytes.AsSpan(at, 4), value);
    }

    public static class SaveSlotSummary
    {
        public const int FileSize = 0x40000;
        private const int First = 0x1096;
        private const int Stride = 0x192;
        private const int NameBytes = 0x22;

        public static string Name(byte[] file, int slot)
        {
            var text = Encoding.Unicode.GetString(file, First + slot * Stride, NameBytes);
            var end = text.IndexOf('\0');
            return end >= 0 ? text[..end] : text;
        }

        public static int Level(byte[] file, int slot) => BitConverter.ToInt32(file, First + slot * Stride + NameBytes);

        public static byte[]? WithLevel(byte[] file, int slot, string name, int level)
        {
            if (file.Length != FileSize || Name(file, slot) != name) return null;
            var copy = (byte[])file.Clone();
            BitConverter.TryWriteBytes(copy.AsSpan(First + slot * Stride + NameBytes, 4), level);
            return copy;
        }
    }
}
