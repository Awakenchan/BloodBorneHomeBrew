using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Threading;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>Home: the game folder and its check, saves, language, a summary and the Play button.</summary>
    public sealed class PlayPage : LauncherPage
    {
        private readonly InfoBar _problem;
        private readonly SettingRow _engineCard;
        private readonly TextBlock _savesHint;
        private readonly TextBlock _gameHint, _dlcHint, _statGame, _statUpscaler, _statResolution, _statFps, _statVia;
        private readonly SymbolIcon _gameIcon;
        private readonly Button _play, _resetDlc;
        private readonly TextBlock _playStatus;
        private readonly DispatcherTimer _clock;

        public PlayPage() : base(L.T("Play"), L.T("Check the game folder and start Bloodborne."))
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

            var actions = new Grid { Margin = new Thickness(0, 16, 0, 0) };
            actions.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            actions.ColumnDefinitions.Add(new ColumnDefinition());
            _play = new Button
            {
                Appearance = ControlAppearance.Primary,
                Content = L.T("Play"),
                Icon = Icon(SymbolRegular.Play24),
                Padding = new Thickness(26, 10, 26, 10),
                FontWeight = FontWeights.SemiBold,
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

            _problem = new InfoBar
            {
                IsClosable = false,
                IsOpen = false,
                Margin = new Thickness(0, 12, 0, 0),
                // What is wrong and what to do, step by step.
                Content = SmallButton(L.T("Open the setup guide"), SymbolRegular.Lightbulb24, (_, _) => SetupGuide.Open()),
            };
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
                        SmallButton(L.T("Join"), SymbolRegular.Open24, (_, _) => OpenDiscord(), ControlAppearance.Primary),
                        IconButton(SymbolRegular.Dismiss24, L.T("Hide"), (_, _) =>
                        {
                            S.Set("discord_hidden", true);
                            Ws.SaveSoon();
                            Body.Children.Remove(discord);
                        })), out _);
                discord.Margin = new Thickness(0, 12, 0, 0);
                Body.Children.Add(discord);
            }

            // ==================== Game ====================
            Group(L.T("Game"));
            var gameCard = Card(SymbolRegular.Folder24, L.T("Game folder"), "", Row(
                SmallButton(L.T("Setup guide"), SymbolRegular.Lightbulb24, (_, _) => SetupGuide.Open()),
                SmallButton(L.T("Browse"), SymbolRegular.FolderOpen24, (_, _) => ChooseGame()),
                IconButton(SymbolRegular.Open24, L.T("Open in Explorer"), (_, _) => Paths.OpenFolder(GameEnvironment.GameDir(S)))), out _gameHint);
            _gameIcon = gameCard.Icon;
            gameCard.ShowIcon = true;
            Add(gameCard);
            ToggleRow(SymbolRegular.TextBulletListSquare24, L.T("Lantern menus in the game"),
                L.T("The PC menus Lantern Engine adds to the game: the extra pages in System (PC graphics, controls, camera), the title ") +
                L.T("screen's entries and the Altar of Despair's rebirth. Off: only the game's own menus, as on PS4; the launcher and the ") +
                L.T("overlay (Insert or L3 + R3) still change every setting. Takes effect at the next start"),
                () => Ini.Get("pc_menus", "1") != "0", v => Ini.Set("pc_menus", v ? "1" : "0"));
            ToggleRow(SymbolRegular.PersonEdit24, L.T("Hunter's Dream Mirror"),
                L.T("The Dream's mirror opens the appearance editor (the unused \"Put on Disguise\"). Takes effect at the next start"),
                () => Ini.Get("change_appearance", "0") != "0", v => Ini.Set("change_appearance", v ? "1" : "0"));
            ToggleRow(SymbolRegular.ArrowCounterclockwise24, L.T("Rebirth at the Altar"),
                L.T("The Altar of Despair offers Rebirth: your stats go back to your origin's, the Blood Echoes are refunded, and it can be undone. Takes effect at the next start"),
                () => Ini.Get("rebirth", "0") != "0", v => Ini.Set("rebirth", v ? "1" : "0"));
            ToggleRow(SymbolRegular.Video24, L.T("PC Camera Controls"),
                L.T("Camera field of view, distance and height in the game's System menu. Takes effect at the next start"),
                () => Ini.Get("camera_controls", "0") != "0", v => Ini.Set("camera_controls", v ? "1" : "0"));

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
                IconButton(SymbolRegular.Open24, L.T("Open in Explorer"), (_, _) => Paths.OpenFolder(GameEnvironment.DlcDir(S))),
                _resetDlc), out _dlcHint));

            // Steam: Bloodborne.exe as a non-Steam game (Core/SteamShortcut.cs).
            Add(Card(SymbolRegular.Games24, L.T("Add to Steam"),
                L.T("Bloodborne as a non-Steam game in your Steam library: Steam Input, the overlay and Big Picture work with it"),
                SmallButton(L.T("Add to Steam"), SymbolRegular.Add24, (_, _) => AddToSteam()), out _));

            Add(Card(SymbolRegular.Save24, L.T("Saves and profiles"), L.T("Who is playing, their backups, and importing saves"),
                SmallButton(L.T("Manage"), SymbolRegular.FolderOpen24, (_, _) => MainWindow.Instance?.Show("saves")), out _savesHint));

            ComboRow(SymbolRegular.LocalLanguage24, L.T("In-game menu language"), L.T("The port's settings menu (Insert or L3 + R3)"),
                Choices.MenuLanguages, () => Ini.Get("menu_language", "en"), v => Ini.Set("menu_language", v));

            _clock = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
            _clock.Tick += (_, _) => UpdateRunning();

            Ws.PortChanged += Refresh;
            Ws.GameStateChanged += RefreshSaves;
            Ws.Saved += RefreshSummary;
            Ws.TrophiesChanged += RefreshSummary;
        }

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
        private int _refreshes;

        /// <summary>Shows what is known at once, then checks the game folder (eboot.bin, param.sfo, an
        /// update folder beside it, a DLC dump nearby) on a worker thread: on a slow or sleeping
        /// disk that took seconds, with the whole launcher frozen.</summary>
        private void Refresh()
        {
            var gameDir = GameEnvironment.GameDir(S);
            var dlcChosen = S.Str("dlc_dir");
            SetHint(_gameHint, string.IsNullOrEmpty(gameDir) ? L.T("Not chosen") : gameDir);
            _resetDlc.IsEnabled = dlcChosen.Length > 0;
            RefreshSaves();
            _engineCard.Visibility = Updater.EngineOutdated ? Visibility.Visible : Visibility.Collapsed;
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
            _gameIcon.SetResourceReference(ForegroundProperty, status.Health switch
            {
                Health.Good => "GoodTextBrush",
                Health.Warning => "WarningTextBrush",
                _ => "DangerTextBrush",
            });
            _gameIcon.ToolTip = status.Summary;

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
            _play.Content = game.Running ? L.T("Stop") : L.T("Play");
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

        private void OpenDiscord()
        {
            try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(LauncherSettings.Discord) { UseShellExecute = true }); }
            catch (Exception e) { Ws.Log(L.T("Could not open the Discord link: ") + e.Message, LogLevel.Warn); }
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
        }

        private void AddToSteam()
        {
            var exe = Path.Combine(Paths.PortDir, "Bloodborne.exe");
            if (!File.Exists(exe))
            {
                Prompt.Tell(L.T("Add to Steam"), L.T("Bloodborne.exe is missing from the Lantern Engine folder: reinstall the package."));
                return;
            }
            if (SteamShortcut.SteamDir() is null)
            {
                Prompt.Tell(L.T("Add to Steam"), L.T("Steam is not installed for this Windows user."));
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
                SteamShortcut.Add(exe);
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

        private void RefreshSaves()
        {
            var profile = PlayerProfiles.Current(S);
            var played = SessionStats.Playtime(S, profile);
            SetHint(_savesHint, played.TotalMinutes >= 1 ? L.F($"{profile}  ·  {SessionStats.Duration(played)} played") : profile);
        }
    }
}
