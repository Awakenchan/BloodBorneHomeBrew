// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;

namespace BloodborneLauncher.Core
{
    public sealed class SaveEditorSession
    {
        public SavePlace Place { get; }
        public SaveCharacter Character { get; }
        public SaveSlot Slot { get; private set; }
        public int Changes { get; private set; }
        private byte[] _original;

        private SaveEditorSession(SavePlace place, SaveCharacter character, byte[] bytes)
        {
            Place = place;
            Character = character;
            _original = bytes;
            Slot = SaveSlot.Parse(bytes);
        }

        public static SaveEditorSession Open(SavePlace place, SaveCharacter character) =>
            new(place, character, File.ReadAllBytes(character.File));

        public void Edit(Action<SaveSlot> edit)
        {
            edit(Slot);
            Changes++;
        }

        public string Commit()
        {
            var backup = SaveStore.Commit(Place, Character, _original, Slot);
            Reload();
            return backup;
        }

        public void Discard()
        {
            Slot = SaveSlot.Parse(_original);
            Changes = 0;
        }

        public void Reload()
        {
            _original = File.ReadAllBytes(Character.File);
            Slot = SaveSlot.Parse(_original);
            Changes = 0;
        }

        public static List<SavePlace> Places(LauncherSettings s)
        {
            var places = new List<SavePlace>();
            var id = SaveImport.GameId(s);
            foreach (var profile in PlayerProfiles.All(s))
            {
                var saveData = Path.Combine(PlayerProfiles.Folder(s, profile), "savedata");
                var folder = Path.Combine(saveData, "1", id, SaveImport.SaveDir);
                places.Add(new SavePlace(profile, saveData, folder, Path.Combine(GameEnvironment.SavesRoot(s), "launcher-backups", profile, "save-editor")));
            }
            return places;
        }
    }
}
