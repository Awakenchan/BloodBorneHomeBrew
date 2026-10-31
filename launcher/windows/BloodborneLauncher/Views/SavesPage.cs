using System.IO;
using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    public sealed class SavesPage : LauncherPage
    {
        private readonly StackPanel _profiles = new();
        private readonly StackPanel _backups = new();
        private readonly TextBlock _backupHint, _userHint;
        private readonly Button _resetUser;
        private int _profileReads, _backupReads;

        public SavesPage() : base(L.T("Saves"), L.T("Who is playing, their backups, and where saves are kept."))
        {
            Group(L.T("Who is playing"));
            Body.Children.Add(_profiles);
            Add(Card(SymbolRegular.PersonAdd24, L.T("New profile"),
                L.T("Each profile has its own save files and trophies, so several people can play their own runs on this PC"),
                SmallButton(L.T("New…"), SymbolRegular.PersonAdd24, (_, _) => NewProfile()), out _));

            Group(L.T("Backups"));
            var backupToggle = new ToggleSwitch { IsChecked = S.Bool("save_backups"), VerticalAlignment = VerticalAlignment.Center };
            backupToggle.Click += (_, _) =>
            {
                S.Set("save_backups", backupToggle.IsChecked == true);
                Ws.SaveSoon();
                RefreshBackups();
            };
            Add(Card(SymbolRegular.ArrowSync24, L.T("Back up my saves automatically"),
                L.F($"Before every start and every {SaveBackups.PlayEvery.TotalMinutes:0} minutes while playing, when this profile's saves changed. The newest {SaveBackups.Keep} and {SaveBackups.PlayKeep} from play are kept"),
                backupToggle, out _backupHint));
            Body.Children.Add(_backups);

            Group(L.T("Other"));
            Add(Card(SymbolRegular.ArrowImport24, L.T("Import saves"),
                L.T("From shadPS4, another Bloodborne port, or a PS4 save exported decrypted (Apollo Save Tool, Save Wizard). This profile's saves are backed up first"),
                SmallButton(L.T("Import…"), SymbolRegular.FolderArrowRight24, (_, _) => ImportSaves()), out _));
            _resetUser = IconButton(SymbolRegular.ArrowUndo24, L.T("Back to the default folder"), (_, _) =>
            {
                S.Set("user_dir", "");
                Ws.SaveSoon();
                Refresh();
            });
            Add(Card(SymbolRegular.Save24, L.T("Saves folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) => ChooseUser()),
                IconButton(SymbolRegular.Open24, L.T("Open in Explorer"), (_, _) => Paths.OpenFolder(GameEnvironment.SavesRoot(S))),
                _resetUser), out _userHint));

            Ws.PortChanged += Refresh;
            Ws.GameStateChanged += Refresh;
        }

        public override void OnShown() => Refresh();

        private void Refresh()
        {
            var custom = S.Str("user_dir").Length > 0;
            var root = GameEnvironment.SavesRoot(S);
            SetHint(_userHint, custom ? root : L.F($"Default: {root}"));
            _resetUser.IsEnabled = custom;
            RefreshProfiles();
            RefreshBackups();
        }

        private async void RefreshProfiles()
        {
            var read = ++_profileReads;
            var names = PlayerProfiles.All(S);
            var savesRoot = GameEnvironment.SavesRoot(S);
            var folders = names.ToDictionary(n => n, n => (
                Saves: Path.Combine(PlayerProfiles.Folder(S, n), "savedata"),
                Backups: SaveBackups.FolderOf(S, n),
                Copies: SaveGuard.ProfileCopies(savesRoot, n)));
            Dictionary<string, (bool Found, int Backups)> info;
            try
            {
                info = await Task.Run(() => folders.ToDictionary(f => f.Key, f => (
                    Directory.Exists(f.Value.Saves) && Directory.EnumerateFileSystemEntries(f.Value.Saves).Any(),
                    SaveBackups.ListWithSafetyCopies(f.Value.Backups, f.Value.Copies).Count)));
            }
            catch
            {
                info = names.ToDictionary(n => n, _ => (false, 0));
            }
            if (read != _profileReads) return;
            var current = PlayerProfiles.Current(S);
            _profiles.Children.Clear();
            foreach (var name in names)
            {
                var playing = name.Equals(current, StringComparison.OrdinalIgnoreCase);
                var played = SessionStats.Playtime(S, name);
                var parts = new List<string> { info[name].Found ? L.T("Has saves") : L.T("No saves yet") };
                if (info[name].Backups > 0) parts.Add(L.F($"{info[name].Backups} backups"));
                if (played.TotalMinutes >= 1) parts.Add(L.F($"{SessionStats.Duration(played)} played"));
                if (playing) parts.Add(L.F($"{Ws.Trophies.Count} / {TrophyTracker.All.Length} trophies"));

                UIElement control;
                if (playing)
                {
                    var tag = Text(L.T("Playing"), "Muted");
                    tag.VerticalAlignment = VerticalAlignment.Center;
                    tag.Margin = new Thickness(0, 0, 8, 0);
                    control = PlayerProfiles.IsMain(name)
                        ? tag
                        : Row(tag,
                            IconButton(SymbolRegular.Rename24, L.T("Rename this profile"), (_, _) => RenameProfile()),
                            IconButton(SymbolRegular.PersonDelete24, L.T("Remove this profile from the list (its saves stay on disk)"), (_, _) => RemoveProfile()));
                }
                else
                {
                    var chosen = name;
                    control = SmallButton(L.T("Switch"), SymbolRegular.Person24, (_, _) => SwitchProfile(chosen));
                }
                var row = Card(SymbolRegular.Person24, name, null, control, out var hint);
                SetHint(hint, string.Join("  ·  ", parts));
                _profiles.Children.Add(row);
            }
        }

        private async void RefreshBackups()
        {
            var read = ++_backupReads;
            var folder = SaveBackups.Folder(S);
            var copies = SaveGuard.ProfileCopies(GameEnvironment.SavesRoot(S), PlayerProfiles.Current(S));
            List<SaveBackups.Backup> list;
            try { list = await Task.Run(() => SaveBackups.ListWithSafetyCopies(folder, copies)); }
            catch { list = new(); }
            if (read != _backupReads) return;
            SetHint(_backupHint, !S.Bool("save_backups") ? L.T("Off: no backups are made")
                : list.Count == 0 ? L.T("None yet: the first is made when you next press Play")
                : L.F($"{list.Count} for {PlayerProfiles.Current(S)}"));

            _backups.Children.Clear();
            foreach (var backup in list)
            {
                var chosen = backup;
                var row = Card(SymbolRegular.History24, FriendlyTime(backup.When), null,
                    SmallButton(L.T("Restore"), SymbolRegular.ArrowUndo24, (_, _) => RestoreBackup(chosen)), out var hint);
                SetHint(hint, Kind(backup) + L.F($"  ·  {backup.Bytes / 1048576.0:0.0} MB"));
                _backups.Children.Add(row);
            }
        }

        private static string FriendlyTime(DateTime when)
        {
            var today = DateTime.Today;
            var day = when.Date == today ? L.T("Today")
                : when.Date == today.AddDays(-1) ? L.T("Yesterday")
                : when.Year == today.Year ? when.ToString("d MMM")
                : when.ToString("d MMM yyyy");
            return $"{day}, {when:HH:mm}";
        }

        private static string Kind(SaveBackups.Backup backup) =>
            backup.Label != null ? L.F($"Named {backup.Label}")
            : backup.SafetyCopy ? L.T("Safety copy")
            : backup.MadeBeforeRestore ? L.T("Before a restore") : backup.MadeDuringPlay ? L.T("While playing") : L.T("Before playing");

        private void ImportSaves()
        {
            if (Ws.Game.Running)
            {
                Prompt.Tell(L.T("Import saves"), L.T("Close the game first: it has the saves open."));
                return;
            }
            string? from = null;
            if (SaveImport.Known(S).FirstOrDefault() is { } found)
            {
                var choice = Prompt.Choose(L.T("Import saves"),
                    L.F($"Bloodborne saves found ({found.From}), last saved {found.Saved:d MMM yyyy, HH:mm}:\n{found.Dir}"),
                    L.T("Cancel"), L.T("Choose others…"), L.T("Import these"));
                if (choice <= 0) return;
                if (choice == 2) from = found.Dir;
            }
            if (from == null)
            {
                var how = Prompt.Choose(L.T("Import saves"),
                    L.T("Choose the folder with your saves (another port's whole folder is fine), or a save file: ") +
                    L.T("a .zip of the save, or one of its files (userdata0000)."),
                    L.T("Cancel"), L.T("A folder…"), L.T("A file or .zip…"));
                if (how <= 0) return;
                string? folder;
                if (how == 1)
                {
                    folder = PickFolder(L.T("The folder with your saves (another port's whole folder is fine)"), "");
                }
                else
                {
                    var dialog = new Microsoft.Win32.OpenFileDialog
                    {
                        Title = L.T("A save file: a .zip of the save, or one of its files (userdata0000)"),
                        Filter = L.T("Bloodborne saves (*.zip;userdata*)|*.zip;userdata*|All files|*.*"),
                    };
                    try { folder = dialog.ShowDialog() == true ? SaveImport.FolderOfFile(dialog.FileName) : null; }
                    catch (Exception e)
                    {
                        Prompt.Tell(L.T("Import saves"), L.T("Could not open that file: ") + e.Message);
                        return;
                    }
                }
                if (folder == null) return;
                if (SaveImport.Find(folder, L.T("chosen folder")).FirstOrDefault() is not { } inside)
                {
                    Prompt.Tell(L.T("Import saves"), SaveImport.HasEncryptedPs4Save(folder)
                        ? L.T("That is a PS4 save as the console copies it to USB: it is encrypted with that PS4's own keys, ") +
                          L.T("which only the console has, so no PC program can open it.\n\n") +
                          L.T("Export it decrypted instead: on a jailbroken PS4 with Apollo Save Tool (Copy save game → decrypted, to USB), ") +
                          L.T("or with Save Wizard for PS4. Then import that folder here.")
                        : L.T("No Bloodborne saves in that folder or the folders under it (a SPRJ0005 folder with userdata files)."));
                    return;
                }
                from = inside.Dir;
            }
            var target = SaveImport.Target(S);
            if (string.Equals(Path.GetFullPath(from), Path.GetFullPath(target), StringComparison.OrdinalIgnoreCase))
            {
                Prompt.Tell(L.T("Import saves"), L.T("Those are already this profile's saves."));
                return;
            }
            var replacing = SaveImport.HasSaves(target)
                ? L.T("\n\nThis profile's current saves are backed up first: the Backups list on this page brings them back.")
                : "";
            if (!Prompt.Confirm(L.T("Import saves"), L.F($"Copy the saves from\n{from}\ninto {PlayerProfiles.Current(S)}'s saves?{replacing}"), L.T("Import")))
                return;
            try
            {
                SaveImport.Import(S, from);
                Ws.Log(L.F($"Saves imported from {from}."), LogLevel.Good);
                Prompt.Tell(L.T("Saves imported"), L.T("Start the game and choose Continue or Load Game."));
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not import the saves: ") + e.Message, LogLevel.Bad);
                Prompt.Tell(L.T("Import saves"), L.T("Could not import: ") + e.Message);
            }
            Refresh();
        }

        private void RestoreBackup(SaveBackups.Backup backup)
        {
            if (Ws.Game.Running)
            {
                Prompt.Tell(L.T("Restore saves"), L.T("Close the game first: it has the saves open."));
                return;
            }
            if (!Prompt.Confirm(L.T("Restore saves"),
                    L.F($"{PlayerProfiles.Current(S)}'s saves go back to {backup.When:d MMM yyyy, HH:mm}. ") +
                    L.T("What is there now is backed up first, so you can undo this from the same list."), L.T("Restore")))
                return;
            try
            {
                SaveBackups.Restore(S, backup);
                Ws.Log(L.F($"Saves restored from {backup.When:d MMM yyyy, HH:mm}."), LogLevel.Good);
            }
            catch (Exception e)
            {
                Ws.Log(L.T("Could not restore the saves: ") + e.Message, LogLevel.Bad);
                Prompt.Tell(L.T("Restore saves"), L.T("Could not restore: ") + e.Message);
            }
            Refresh();
        }

        private bool GameBlocksProfiles()
        {
            if (!Ws.Game.Running) return false;
            Prompt.Tell(L.T("Player profile"), L.T("Close the game before changing profiles: it is using this profile's saves."));
            return true;
        }

        private void SwitchProfile(string name)
        {
            if (name.Equals(PlayerProfiles.Current(S), StringComparison.OrdinalIgnoreCase) || GameBlocksProfiles()) return;
            S.Set("player_profile", PlayerProfiles.IsMain(name) ? "" : name);
            Directory.CreateDirectory(GameEnvironment.UserDir(S));
            Ws.Trophies.SetUserDir(GameEnvironment.UserDir(S));
            Ws.SaveSoon();
            Ws.Log(L.F($"Player profile: {name}."), LogLevel.Step);
            Refresh();
        }

        private void NewProfile()
        {
            if (GameBlocksProfiles()) return;
            var values = Prompt.Ask(L.T("New player profile"), L.T("Its own save files and trophies. The game starts a new character the first time."),
                new Prompt.Field(L.T("Name"), "", L.T("e.g. Alex")));
            if (values == null) return;
            var name = PlayerProfiles.Validate(S, values[0], out var error);
            if (name == null) { Prompt.Tell(L.T("New player profile"), error); return; }
            PlayerProfiles.Add(S, name);
            SwitchProfile(name);
        }

        private void RenameProfile()
        {
            var current = PlayerProfiles.Current(S);
            if (GameBlocksProfiles()) return;
            var values = Prompt.Ask(L.T("Rename profile"), null, new Prompt.Field(L.T("Name"), current));
            if (values == null) return;
            var name = PlayerProfiles.Validate(S, values[0], out var error, renaming: current);
            if (name == null) { Prompt.Tell(L.T("Rename profile"), error); return; }
            try
            {
                PlayerProfiles.Rename(S, current, name);
            }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Rename profile"), L.T("Could not rename its folder: ") + e.Message);
                return;
            }
            Ws.Trophies.SetUserDir(GameEnvironment.UserDir(S));
            Ws.SaveSoon();
            Refresh();
        }

        private void RemoveProfile()
        {
            var current = PlayerProfiles.Current(S);
            if (GameBlocksProfiles()) return;
            if (!Prompt.Confirm(L.T("Remove profile"),
                    L.F($"{current} is taken off the list and the launcher switches to Main. Its saves and trophies are not deleted: ") +
                    L.F($"they stay in {GameEnvironment.UserDir(S)}, and adding a profile with the same name brings them back."), L.T("Remove")))
                return;
            PlayerProfiles.Remove(S, current);
            S.Set("player_profile", "");
            Ws.Trophies.SetUserDir(GameEnvironment.UserDir(S));
            Ws.SaveSoon();
            Refresh();
        }

        private void ChooseUser()
        {
            if (GameBlocksProfiles()) return;
            var path = PickFolder(L.T("Saves folder"), GameEnvironment.SavesRoot(S));
            if (path == null) return;
            S.Set("user_dir", path);
            Ws.SaveSoon();
            Refresh();
        }
    }
}
