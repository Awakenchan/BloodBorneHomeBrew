using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Layout;
using Avalonia.Threading;
using BloodborneLauncher.Core;

namespace BloodborneLauncher.Views
{
    /// <summary>Home: the game folder and its check, saves, language, a summary and the Play button.</summary>
    public sealed class PlayPage : LauncherPage
    {
        private readonly InfoBar _problem;
        private readonly SettingRow _engineCard;
        private readonly AppButton _profileButton;
        private readonly TextBlock _profileHint, _backupHint;
        private readonly AppButton _restoreButton;
        private readonly TextBlock _gameHint, _dlcHint, _userHint, _statGame, _statUpscaler, _statResolution, _statFps, _statVia;
        private readonly SymbolIcon _gameIcon;
        private readonly AppButton _play, _resetUser, _resetDlc;
        private readonly TextBlock _playStatus;
        private readonly DispatcherTimer _clock;

        public PlayPage() : base(L.T("Play"), L.T("Check the game folder, choose where saves go, and start Bloodborne."))
        {
            // ==================== Summary + Play ====================
            // A glass pane in the Modern layout, a card in Classic.
            var summary = new Border
            {
                Margin = new Thickness(0, 14, 0, 0),
                Padding = new Thickness(18, 16, 18, 16),
                CornerRadius = new CornerRadius(SettingRow.Classic ? 8 : 16),
                BorderThickness = new Thickness(1),
            };
            summary.SetResourceReference(Border.BackgroundProperty, "GlassPaneBrush");
            summary.SetResourceReference(Border.BorderBrushProperty, "GlassPaneStrokeBrush");
            var stack = new StackPanel();
            var stats = new UniformGrid { Rows = 1 };
            stats.Children.Add(Stat(L.T("Game"), out _statGame));
            stats.Children.Add(Stat(L.T("Upscaler"), out _statUpscaler));
            stats.Children.Add(Stat(L.T("Output"), out _statResolution));
            stats.Children.Add(Stat(L.T("Frame rate"), out _statFps));
            stats.Children.Add(Stat(L.T("Trophies"), out _statVia));
            stack.Children.Add(stats);

            var actions = new Grid { Margin = new Thickness(0, 16, 0, 0), ColumnDefinitions = new ColumnDefinitions("Auto,*") };
            _play = new AppButton
            {
                Appearance = ControlAppearance.Primary,
                Text = L.T("Play"),
                Icon = Icon(SymbolRegular.Play24),
                Padding = new Thickness(26, 10, 26, 10),
                FontWeight = Avalonia.Media.FontWeight.SemiBold,
            };
            _play.Click += async (_, _) => await MainWindow.Instance!.PlayOrStopAsync();
            actions.Children.Add(_play);
            _playStatus = Text("", "Muted");
            _playStatus.Margin = new Thickness(16, 0, 0, 0);
            _playStatus.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetColumn(_playStatus, 1);
            actions.Children.Add(_playStatus);
            stack.Children.Add(actions);
            summary.Child = stack;
            Body.Children.Add(summary);

            _problem = new InfoBar { IsClosable = false, IsOpen = false, Margin = new Thickness(0, 12, 0, 0) };
            Body.Children.Add(_problem);

            // A package from before this launcher's engine (it updated itself, the engine did not).
            _engineCard = Card(SymbolRegular.ArrowDownload24, L.T("Engine update available"),
                L.T("This package has an older engine than this launcher: the crash fixes since then are missing. ") +
                L.T("Updating downloads the latest package and keeps your settings, saves and mods."),
                SmallButton(L.T("Update engine"), SymbolRegular.ArrowDownload24, async (_, _) => await MainWindow.Instance!.UpdateEngineAsync(),
                    ControlAppearance.Primary), out _);
            _engineCard.Margin = new Thickness(0, 12, 0, 0);
            Body.Children.Add(_engineCard);

            // The community: help, news and people to play with. Hidden for good with the X
            // (Settings keeps the link).
            if (!S.Bool("discord_hidden"))
            {
                SettingRow? discord = null;
                discord = Card(SymbolRegular.PeopleCommunity24, L.T("Join the Discord"),
                    L.T("Help, news, co-op partners and beta feedback"), Row(
                        SmallButton(L.T("Join"), SymbolRegular.Open24, (_, _) => Ui.OpenLink(LauncherSettings.Discord), ControlAppearance.Primary),
                        IconButton(SymbolRegular.Dismiss24, L.T("Hide"), (_, _) =>
                        {
                            S.Set("discord_hidden", true);
                            Ws.SaveSoon();
                            Body.Children.Remove(discord!);
                        })), out _);
                discord.Margin = new Thickness(0, 12, 0, 0);
                Body.Children.Add(discord);
            }

            // ==================== Game ====================
            Group(L.T("Game"));
            var gameCard = Card(SymbolRegular.Folder24, L.T("Game folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) => ChooseGame()),
                IconButton(SymbolRegular.Open24, OpenTip, (_, _) => Paths.OpenFolder(GameEnvironment.GameDir(S)))), out _gameHint);
            _gameIcon = gameCard.Icon;
            gameCard.ShowIcon = true;
            Add(gameCard);
            ToggleRow(SymbolRegular.TextBulletListSquare24, L.T("Lantern menus in the game"),
                L.T("The PC menus Lantern Engine adds to the game: the extra pages in System (PC graphics, controls, camera), the title ") +
                L.T("screen's entries and the Altar of Despair's rebirth. Off: only the game's own menus, as on PS4; the launcher and the ") +
                L.T("overlay (Insert or L3 + R3) still change every setting. Takes effect at the next start"),
                () => Ini.Get("pc_menus", "1") != "0", v => Ini.Set("pc_menus", v ? "1" : "0"));

            // The Old Hunters on base-game dumps: the folder of the dumped add-ons (BB_DLC_DIR).
            _resetDlc = IconButton(SymbolRegular.Dismiss24, L.T("No DLC folder"), (_, _) =>
            {
                S.Set("dlc_dir", "");
                Ws.SaveSoon();
                Refresh();
            });
            ToggleRow(SymbolRegular.PuzzlePiece24, L.T("The Old Hunters (DLC)"),
                L.T("Off: the game starts without its DLC. The Game of the Year edition (CUSA03173) always has it"),
                () => S.Bool("dlc_enabled"), v =>
                {
                    S.Set("dlc_enabled", v);
                    Ws.SaveSoon();
                    Refresh();
                });
            Add(Card(SymbolRegular.FolderOpen24, L.T("DLC folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) => ChooseDlc()),
                IconButton(SymbolRegular.Open24, OpenTip, (_, _) => Paths.OpenFolder(GameEnvironment.DlcDir(S))),
                _resetDlc), out _dlcHint));

            // Steam: the game as a non-Steam game (Core/SteamShortcut.cs).
            Add(Card(SymbolRegular.Games24, L.T("Add to Steam"),
                L.T("Bloodborne as a non-Steam game in your Steam library: Steam Input, the overlay and Big Picture work with it"),
                SmallButton(L.T("Add to Steam"), SymbolRegular.Add24, (_, _) => AddToSteam()), out _));

            // Player profiles: each its own saves and trophies (a button with a menu, not a drop-down,
            // so a click can't pick an entry by itself).
            _profileButton = SmallButton("Main", SymbolRegular.Person24, (_, _) => ShowProfileMenu());
            Add(Card(SymbolRegular.People24, L.T("Player profile"),
                L.T("Each profile has its own save files and trophies, so several people can play their own runs on this PC"), Row(
                _profileButton,
                SmallButton(L.T("New…"), SymbolRegular.PersonAdd24, (_, _) => NewProfile()),
                IconButton(SymbolRegular.Rename24, L.T("Rename this profile"), (_, _) => RenameProfile()),
                IconButton(SymbolRegular.PersonDelete24, L.T("Remove this profile from the list (its saves stay on disk)"), (_, _) => RemoveProfile())),
                out _profileHint));

            _resetUser = IconButton(SymbolRegular.ArrowUndo24, L.T("Back to the default folder"), (_, _) =>
            {
                S.Set("user_dir", "");
                Ws.SaveSoon();
                Refresh();
            });
            Add(Card(SymbolRegular.Save24, L.T("Saves folder"), "", Row(
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) => ChooseUser()),
                IconButton(SymbolRegular.Open24, OpenTip, (_, _) => Paths.OpenFolder(GameEnvironment.SavesRoot(S))),
                _resetUser), out _userHint));

            // Save backups: made before each start when the saves changed (Workspace.StartGame).
            _restoreButton = SmallButton(L.T("Restore…"), SymbolRegular.History24, (_, _) => ShowBackupMenu());
            var backupToggle = new ToggleSwitch { IsChecked = S.Bool("save_backups"), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(8, 0, 0, 0) };
            backupToggle.Click += (_, _) =>
            {
                S.Set("save_backups", backupToggle.IsChecked == true);
                Ws.SaveSoon();
                RefreshBackups();
            };
            Add(Card(SymbolRegular.ArrowSync24, L.T("Save backups"),
                L.F($"Before every start and every {SaveBackups.PlayEvery.TotalMinutes:0} minutes while playing, when this profile's saves changed. The newest {SaveBackups.Keep} and {SaveBackups.PlayKeep} from play are kept"), Row(
                _restoreButton,
                IconButton(SymbolRegular.Open24, L.T("Open the backups folder"), (_, _) =>
                {
                    Directory.CreateDirectory(SaveBackups.Folder(S));
                    Paths.OpenFolder(SaveBackups.Folder(S));
                }),
                backupToggle), out _backupHint));
            Add(Card(SymbolRegular.ArrowImport24, L.T("Import saves"),
                L.T("From shadPS4, another Bloodborne port, or a PS4 save exported decrypted (Apollo Save Tool, Save Wizard). This profile's saves are backed up first"),
                SmallButton(L.T("Import…"), SymbolRegular.FolderArrowRight24, (_, _) => ImportSaves()), out _));

            ComboRow(SymbolRegular.LocalLanguage24, L.T("In-game menu language"), L.T("The port's settings menu (Insert or L3 + R3)"),
                Choices.MenuLanguages, () => Ini.Get("menu_language", "en"), v => Ini.Set("menu_language", v));

            _clock = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _clock.Tick += (_, _) => UpdateRunning();

            Ws.PortChanged += Refresh;
            Ws.GameStateChanged += () => { RefreshProfile(); RefreshBackups(); };
            Ws.Saved += RefreshSummary;
            Ws.TrophiesChanged += RefreshSummary;
        }

        /// <summary>"Open in Explorer" on Windows; the file manager elsewhere.</summary>
        internal static string OpenTip => OperatingSystem.IsWindows() ? L.T("Open in Explorer") : L.T("Open in the file manager");

        private static StackPanel Stat(string label, out TextBlock value)
        {
            var panel = new StackPanel { Margin = new Thickness(0, 0, 10, 0) };
            panel.Children.Add(Text(label, "StatLabel"));
            value = Text("—", "StatValue");
            panel.Children.Add(value);
            return panel;
        }

        public override void OnShown() => Refresh();

        /// <summary>The game folder's last check (null until the first one is done).</summary>
        private GameStatus? _status;
        private int _refreshes, _profileReads, _backupReads;

        /// <summary>Shows what is known at once, then checks the game folder (eboot.bin, param.sfo, an
        /// update folder beside it, a DLC dump nearby) on a worker thread: on a slow or sleeping
        /// disk that took seconds, with the whole launcher frozen.</summary>
        private void Refresh()
        {
            var gameDir = GameEnvironment.GameDir(S);
            var dlcChosen = S.Str("dlc_dir");
            SetHint(_gameHint, string.IsNullOrEmpty(gameDir) ? L.T("Not chosen") : gameDir);
            var custom = S.Str("user_dir").Length > 0;
            var root = GameEnvironment.SavesRoot(S);
            SetHint(_userHint, (custom ? root : L.F($"Default: {root}")) + L.T("  ·  every profile's saves live here"));
            _resetUser.IsEnabled = custom;
            _resetDlc.IsEnabled = dlcChosen.Length > 0;
            RefreshProfile();
            RefreshBackups();
            _engineCard.IsVisible = Updater.EngineOutdated;
            RefreshSummary();
            UpdateRunning();
            _ = CheckGameAsync(gameDir, dlcChosen);
        }

        private async Task CheckGameAsync(string gameDir, string dlcChosen)
        {
            var refresh = ++_refreshes;
            GameStatus status;
            string dlc;
            try
            {
                (status, dlc) = await Task.Run(() => (GameCheck.Inspect(gameDir),
                    dlcChosen.Length > 0 ? Paths.Expand(dlcChosen) : GameEnvironment.FoundDlc(gameDir) ?? ""));
            }
            catch (Exception e)
            {
                (status, dlc) = (new GameStatus(Health.Warning, L.T("The game folder could not be read: ") + e.Message, "", ""), "");
            }
            if (refresh != _refreshes) return; // a newer check is on its way
            ShowGame(status, dlc, chosen: dlcChosen.Length > 0);
        }

        private void ShowGame(GameStatus status, string dlc, bool chosen)
        {
            _status = status;
            _gameIcon.Symbol = status.Health == Health.Good ? SymbolRegular.CheckmarkCircle24 : SymbolRegular.Warning24;
            _gameIcon.SetResourceReference(TextBlock.ForegroundProperty, status.Health switch
            {
                Health.Good => "GoodTextBrush",
                Health.Warning => "WarningTextBrush",
                _ => "DangerTextBrush",
            });
            _gameIcon.SetTip(status.Summary);

            _problem.IsOpen = status.Health != Health.Good;
            _problem.Severity = status.Health == Health.Bad ? InfoBarSeverity.Error : InfoBarSeverity.Warning;
            _problem.Title = status.Health == Health.Bad ? L.T("The game folder needs attention") : L.T("Check the game");
            _problem.Message = status.Summary;

            SetHint(_dlcHint, dlc.Length > 0
                ? chosen ? dlc : L.F($"Found next to the game: {dlc}")
                : L.T("Only with a base-game dump: the folder of your dumped add-ons, one folder each (The Old Hunters: SPEXPANSIONDLC03). ") +
                  L.T("Found on its own when it is beside the game folder. The Game of the Year edition (CUSA03173) has it already"));

            _statGame.Text = status.Health == Health.Bad ? L.T("Not found") : $"{status.TitleId} {status.Version}".Trim();
            UpdateRunning();
        }

        private void RefreshSummary()
        {
            _statUpscaler.Text = Label(Choices.Upscalers, Ini.Get("upscaler"));
            var (outW, outH) = Choices.OutputSize(Ini.Get("output_res", "1920x1080"));
            _statResolution.Text = $"{outW} × {outH}";
            _statFps.Text = Label(Choices.FpsModes, GameEnvironment.Fps(S));
            _statVia.Text = $"{Ws.Trophies.Count} / {TrophyTracker.All.Length} · {Ws.Trophies.Completion * 100:0}%";
        }

        private static string Label(IReadOnlyList<Choice> choices, string value) =>
            choices.FirstOrDefault(c => c.Value == value)?.Label.Split(" (")[0] ?? value;

        public void UpdateRunning()
        {
            var game = Ws.Game;
            _play.Text = game.Running ? L.T("Stop") : L.T("Play");
            _play.Icon = Icon(game.Running ? SymbolRegular.Stop24 : SymbolRegular.Play24);
            _play.Appearance = game.Running ? ControlAppearance.Danger : ControlAppearance.Primary;

            if (game.Running)
            {
                var up = DateTime.Now - game.StartedAt;
                _playStatus.Text = L.F($"Running for {(int)up.TotalMinutes}:{up.Seconds:00}  ·  output on the Game output page");
                _clock.Start();
            }
            else
            {
                _clock.Stop();
                // The last check's answer (Refresh checks the folder again off the UI thread).
                _playStatus.Text = _status?.Health == Health.Bad
                    ? L.T("Choose the game folder first")
                    : Ws.Session is { Length.TotalSeconds: > 10 } last
                        ? L.T("Last session: ") + last.Summary
                        : L.T("Settings are saved as you change them");
            }
        }

        private void ChooseGame()
        {
            var path = PickFolder(L.T("Game folder (the one with eboot.bin)"), GameEnvironment.GameDir(S));
            if (path == null) return;
            S.Set("game_dir", path);
            if (!Paths.IsPortDir(Paths.PortDir))
                Ws.ResolvePort(); // the port may sit beside the new game folder
            Ws.SaveSoon();
            Ws.Log(L.T("Game folder: ") + path, LogLevel.Step);
            Refresh();
            MainWindow.Instance?.ApplyBackdrop(); // the game's own artwork
        }

        private void AddToSteam()
        {
            // Windows: Bloodborne.exe, which starts the game with the launcher's settings and waits for
            // it. Elsewhere the launcher itself with --play, which does the same.
            var windows = OperatingSystem.IsWindows();
            var exe = windows ? Path.Combine(Paths.PortDir, "Bloodborne.exe") : Environment.ProcessPath ?? "";
            if (!File.Exists(exe))
            {
                Prompt.Tell(L.T("Add to Steam"), L.T("Bloodborne.exe is missing from the Lantern Engine folder: reinstall the package."));
                return;
            }
            if (SteamShortcut.SteamDir() is null)
            {
                Prompt.Tell(L.T("Add to Steam"), windows ? L.T("Steam is not installed for this Windows user.") : L.T("Steam is not installed for this user."));
                return;
            }
            while (SteamShortcut.SteamRunning())
            {
                if (Prompt.Choose(L.T("Close Steam first"),
                        L.T("Steam rewrites its list of non-Steam games when it closes, so it has to be closed while Bloodborne is added. ") +
                        L.T("Exit Steam (Steam › Exit, or right-click its tray icon › Exit), then press Try again."),
                        L.T("Cancel"), L.T("Try again")) <= 0)
                    return;
            }
            try
            {
                SteamShortcut.Add(exe, windows ? "" : "--play");
                Ws.Log(L.T("Added Bloodborne to Steam as a non-Steam game"), LogLevel.Good);
                Prompt.Tell(L.T("Added to Steam"),
                    L.T("Bloodborne is in your Steam library after Steam starts again. It starts the game with this launcher's settings; ") +
                    L.T("Steam shows it as running until the game ends."));
            }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Add to Steam"), e.Message);
            }
        }

        private void ChooseDlc()
        {
            var path = PickFolder(L.T("DLC folder (your dumped add-ons, like SPEXPANSIONDLC03)"), GameEnvironment.DlcDir(S));
            if (path == null) return;
            S.Set("dlc_dir", path);
            Ws.SaveSoon();
            Ws.Log(L.T("DLC folder: ") + path, LogLevel.Step);
            Refresh();
        }

        // ---------------------------------------------------------------- player profiles

        /// <summary>The profile line; whether saves exist is looked up on a worker thread.</summary>
        private async void RefreshProfile()
        {
            var read = ++_profileReads;
            var profile = PlayerProfiles.Current(S);
            _profileButton.Text = profile;
            var dir = GameEnvironment.UserDir(S);
            var saves = Path.Combine(dir, "savedata");
            var played = SessionStats.Playtime(S, profile);
            bool found;
            try { found = await Task.Run(() => Directory.Exists(saves) && Directory.EnumerateFileSystemEntries(saves).Any()); }
            catch { found = false; }
            if (read != _profileReads) return;
            SetHint(_profileHint, dir + (found ? L.T("  ·  saves found") : L.T("  ·  no saves yet; the game creates them here")) +
                                  L.F($"  ·  {Ws.Trophies.Count} / {TrophyTracker.All.Length} trophies") +
                                  (played.TotalMinutes >= 1 ? L.F($"  ·  {SessionStats.Duration(played)} played") : ""));
        }

        // ---------------------------------------------------------------- save backups

        /// <summary>The backups line. Listing them adds up every backup's size, so it is done on a
        /// worker thread (it ran at each game start and stop, right when the launcher is busiest).</summary>
        private async void RefreshBackups()
        {
            var read = ++_backupReads;
            var folder = SaveBackups.Folder(S);
            var profile = PlayerProfiles.Current(S);
            List<SaveBackups.Backup> list;
            try { list = await Task.Run(() => SaveBackups.ListIn(folder)); }
            catch { list = new(); }
            if (read != _backupReads) return;
            _restoreButton.IsEnabled = list.Count > 0;
            SetHint(_backupHint, !S.Bool("save_backups") ? L.T("Off: no backups are made")
                : list.Count == 0 ? L.T("None yet: the first is made when you next press Play")
                : L.F($"{list.Count} for {profile}  ·  newest {list[0].When:d MMM, HH:mm}"));
        }

        private void ShowBackupMenu()
        {
            var menu = new ContextMenu { Placement = PlacementMode.BottomEdgeAlignedLeft };
            foreach (var backup in SaveBackups.List(S))
            {
                var item = new MenuItem
                {
                    Header = L.F($"{backup.When:d MMM yyyy, HH:mm}  ·  {backup.Bytes / 1048576.0:0.0} MB") +
                             (backup.MadeBeforeRestore ? L.T("  ·  before a restore") : backup.MadeDuringPlay ? L.T("  ·  while playing") : ""),
                };
                var chosen = backup;
                item.Click += (_, _) => Dispatcher.UIThread.Post(() => RestoreBackup(chosen));
                menu.Items.Add(item);
            }
            menu.Open(_restoreButton);
        }

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
                    var file = Ui.OpenFile(L.T("A save file: a .zip of the save, or one of its files (userdata0000)"),
                        L.T("Bloodborne saves (*.zip;userdata*)|*.zip;userdata*|All files|*.*"));
                    try { folder = file != null ? SaveImport.FolderOfFile(file) : null; }
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
            if (string.Equals(Path.GetFullPath(from), Path.GetFullPath(target),
                    OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
            {
                Prompt.Tell(L.T("Import saves"), L.T("Those are already this profile's saves."));
                return;
            }
            var replacing = SaveImport.HasSaves(target)
                ? L.T("\n\nThis profile's current saves are backed up first: Save backups › Restore… brings them back.")
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

        private void ShowProfileMenu()
        {
            var menu = new ContextMenu { Placement = PlacementMode.BottomEdgeAlignedLeft };
            var current = PlayerProfiles.Current(S);
            foreach (var profile in PlayerProfiles.All(S))
            {
                var name = profile;
                var item = new MenuItem
                {
                    Header = name,
                    ToggleType = MenuItemToggleType.CheckBox,
                    IsChecked = name.Equals(current, StringComparison.OrdinalIgnoreCase),
                };
                item.Click += (_, _) => Dispatcher.UIThread.Post(() => SwitchProfile(name));
                menu.Items.Add(item);
            }
            menu.Open(_profileButton);
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
            if (PlayerProfiles.IsMain(current)) { Prompt.Tell(L.T("Rename profile"), L.T("Main is the original saves folder and keeps its name.")); return; }
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
            if (PlayerProfiles.IsMain(current)) { Prompt.Tell(L.T("Remove profile"), L.T("Main is the original saves folder and can't be removed.")); return; }
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
