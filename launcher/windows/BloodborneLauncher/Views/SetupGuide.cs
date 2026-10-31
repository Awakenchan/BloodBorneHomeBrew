using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Effects;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;
using TextBlock = System.Windows.Controls.TextBlock;

namespace BloodborneLauncher.Views
{
    /// <summary>
    /// The setup guide: a few steps over the launcher that check the game folder (Core/GameSetup.cs)
    /// and say in plain words what each part is for and what to do when something is missing. It opens
    /// on the first start when the game folder is not ready, from the Play page, and with --setup.
    /// Next never blocks: the last step lists what is still open.
    /// </summary>
    public sealed class SetupGuide : Window
    {
        private enum Step { Welcome, Folder, Version, Files, Dlc, Style, Ready }

        private static Workspace Ws => Workspace.Current;
        private static LauncherSettings S => Ws.Settings;

        private Step _step;
        private SetupReport? _report;
        private int _checks;
        private CancellationTokenSource? _work;
        private List<string>? _searchResults;
        /// <summary>The play style picked on its step (Hardware.Styles), and this PC once detected.</summary>
        private string _style = S.Str("play_style") is { Length: > 0 } style ? style : Hardware.StyleBalanced;
        private Hardware? _hardware;

        private readonly List<(Button Button, SymbolIcon Icon)> _stepRows = new();
        private readonly TextBlock _title, _intro, _where, _status;
        private readonly StackPanel _results, _extra;
        private readonly WrapPanel _actions;
        private readonly Button _back, _next, _close;
        private readonly ProgressBar _progress;
        private readonly ScrollViewer _scroll;

        /// <summary>Opens the guide (modal) and refreshes the Play page when it closes.</summary>
        public static void Open(bool firstRun = false)
        {
            S.Set("setup_shown", true);
            Ws.SaveSoon();
            new SetupGuide().ShowDialog();
            MainWindow.Instance?.Show("play");
            if (firstRun && !S.Bool("hardware_offered") && Paths.IsPortDir(Paths.PortDir))
                MainWindow.Instance?.OfferBestSettings(firstRun: true);
        }

        private SetupGuide()
        {
            var owner = Application.Current.MainWindow is { IsLoaded: true, IsVisible: true } main && main.WindowState != WindowState.Minimized ? main : null;
            Owner = owner;
            TextOptions.SetTextFormattingMode(this, TextFormattingMode.Display);
            TextOptions.SetTextHintingMode(this, TextHintingMode.Fixed);
            UseLayoutRounding = true;
            WindowStyle = WindowStyle.None;
            ResizeMode = ResizeMode.NoResize;
            AllowsTransparency = true;
            Background = Brushes.Transparent;
            ShowInTaskbar = owner == null;
            Title = L.T("Setup guide");
            SetResourceReference(FontFamilyProperty, "UiFont");
            SetResourceReference(ForegroundProperty, "TextFillColorPrimaryBrush");

            // Left: the steps, each a button to jump to it, with its check's state.
            var steps = new StackPanel { Margin = new Thickness(0, 0, 18, 0) };
            var heading = LauncherPage.Text(L.T("Setup guide"), "PageTitle");
            heading.Margin = new Thickness(8, 0, 0, 14);
            steps.Children.Add(heading);
            foreach (var (step, name) in new[]
                     {
                         (Step.Welcome, L.T("Before you start")), (Step.Folder, L.T("Game folder")), (Step.Version, L.T("Game version")),
                         (Step.Files, L.T("Files check")), (Step.Dlc, L.T("The Old Hunters")), (Step.Style, L.T("How it should run")),
                         (Step.Ready, L.T("Ready")),
                     })
            {
                var icon = new SymbolIcon { Symbol = SymbolRegular.Circle24, FontSize = 16, Margin = new Thickness(0, 0, 10, 0) };
                var label = new TextBlock { Text = name, VerticalAlignment = VerticalAlignment.Center, TextTrimming = TextTrimming.CharacterEllipsis };
                var row = new Button
                {
                    Content = new StackPanel { Orientation = Orientation.Horizontal, Children = { icon, label } },
                    Appearance = ControlAppearance.Transparent,
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    HorizontalContentAlignment = HorizontalAlignment.Left,
                    Padding = new Thickness(8, 7, 8, 7),
                    Margin = new Thickness(0, 0, 0, 2),
                };
                var target = step;
                row.Click += (_, _) => Go(target);
                _stepRows.Add((row, icon));
                steps.Children.Add(row);
            }

            // Right: the step's title, what it is for, the check's lines and the step's buttons.
            var content = new StackPanel();
            _title = LauncherPage.Text("", "PageTitle");
            content.Children.Add(_title);
            _intro = Wrapped(13, "TextFillColorSecondaryBrush");
            _intro.Margin = new Thickness(0, 8, 0, 0);
            content.Children.Add(_intro);
            _where = Wrapped(12, "TextFillColorPrimaryBrush");
            _where.SetResourceReference(TextBlock.FontFamilyProperty, "MonoFont");
            _where.Margin = new Thickness(0, 12, 0, 0);
            content.Children.Add(_where);
            _results = new StackPanel { Margin = new Thickness(0, 10, 0, 0) };
            content.Children.Add(_results);
            _extra = new StackPanel();
            content.Children.Add(_extra);
            _actions = new WrapPanel { Margin = new Thickness(-6, 12, 0, 0) };
            content.Children.Add(_actions);
            _progress = new ProgressBar { Height = 4, Minimum = 0, Maximum = 1, Margin = new Thickness(0, 12, 0, 0), Visibility = Visibility.Collapsed };
            content.Children.Add(_progress);
            _status = Wrapped(12, "TextFillColorSecondaryBrush");
            _status.Margin = new Thickness(0, 8, 0, 0);
            content.Children.Add(_status);
            _scroll = new ScrollViewer
            {
                Content = content,
                VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
                Padding = new Thickness(0, 0, 8, 0),
            };

            // Bottom: Close on the left, Back and Next on the right.
            _close = new Button { Content = L.T("Close"), Appearance = ControlAppearance.Transparent, MinWidth = 90 };
            _close.Click += (_, _) => Close();
            _back = new Button { Content = L.T("Back"), MinWidth = 100, Margin = new Thickness(0, 0, 8, 0) };
            _back.Click += (_, _) => Go(_step - 1);
            _next = new Button { Appearance = ControlAppearance.Primary, MinWidth = 120, IsDefault = true };
            _next.Click += (_, _) =>
            {
                if (_step == Step.Ready) Close();
                else Go(_step + 1);
            };
            var bar = new DockPanel { Margin = new Thickness(0, 16, 0, 0), LastChildFill = false };
            DockPanel.SetDock(_close, Dock.Left);
            bar.Children.Add(_close);
            var right = new StackPanel { Orientation = Orientation.Horizontal, Children = { _back, _next } };
            DockPanel.SetDock(right, Dock.Right);
            bar.Children.Add(right);

            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(200) });
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.RowDefinitions.Add(new RowDefinition());
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var divider = new Border { BorderThickness = new Thickness(0, 0, 1, 0), Margin = new Thickness(0, 0, 18, 0) };
            divider.SetResourceReference(Border.BorderBrushProperty, "GlassDividerBrush");
            divider.Child = steps;
            steps.Margin = new Thickness(0, 0, 12, 0);
            grid.Children.Add(divider);
            Grid.SetColumn(_scroll, 1);
            grid.Children.Add(_scroll);
            Grid.SetRow(bar, 1);
            Grid.SetColumnSpan(bar, 2);
            grid.Children.Add(bar);

            var card = new Border
            {
                Width = 820,
                Height = 580,
                Padding = new Thickness(22, 20, 22, 18),
                CornerRadius = new CornerRadius(16),
                BorderThickness = new Thickness(1),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Child = grid,
                Effect = new DropShadowEffect { BlurRadius = 40, ShadowDepth = 8, Direction = 270, Opacity = 0.55, Color = Colors.Black },
            };
            card.SetResourceReference(Border.BackgroundProperty, "DialogBackgroundBrush");
            card.SetResourceReference(Border.BorderBrushProperty, "DialogStrokeBrush");

            if (owner != null)
            {
                // Over the launcher, dimmed, like Prompt; smaller when the launcher is.
                WindowStartupLocation = WindowStartupLocation.Manual;
                var source = PresentationSource.FromVisual(owner);
                var toDip = source?.CompositionTarget?.TransformFromDevice ?? Matrix.Identity;
                var topLeft = toDip.Transform(owner.PointToScreen(new Point(0, 0)));
                Left = topLeft.X;
                Top = topLeft.Y;
                Width = owner.ActualWidth;
                Height = owner.ActualHeight;
                card.Width = Math.Min(card.Width, Math.Max(560, owner.ActualWidth - 40));
                card.Height = Math.Min(card.Height, Math.Max(420, owner.ActualHeight - 40));
                var scrim = new Border { CornerRadius = new CornerRadius(8), Child = card };
                scrim.SetResourceReference(Border.BackgroundProperty, "DialogScrimBrush");
                Content = scrim;
            }
            else
            {
                WindowStartupLocation = WindowStartupLocation.CenterScreen;
                SizeToContent = SizeToContent.WidthAndHeight;
                card.Margin = new Thickness(24);
                card.MouseLeftButtonDown += (_, e) => { if (e.OriginalSource is Border or Grid) DragMove(); };
                Content = card;
            }

            PreviewKeyDown += (_, e) => { if (e.Key == Key.Escape) Close(); };
            Closing += (_, e) =>
            {
                if (_progress.Visibility == Visibility.Visible && _work != null)
                {
                    // Copying the update: stopping leaves every file whole (each is copied to a
                    // temporary name first), but the game half updated until it is copied again.
                    if (!Prompt.Confirm(L.T("Stop copying?"), L.T("The update is not fully copied yet. Stopping leaves the game half updated until you copy it again."), L.T("Stop")))
                    {
                        e.Cancel = true;
                        return;
                    }
                }
                _work?.Cancel();
            };
            Loaded += async (_, _) =>
            {
                Go(Step.Welcome);
                await CheckAsync();
            };
        }

        private static TextBlock Wrapped(double size, string brush)
        {
            var text = new TextBlock { TextWrapping = TextWrapping.Wrap, FontSize = size };
            text.SetResourceReference(TextBlock.ForegroundProperty, brush);
            return text;
        }

        // ---------------------------------------------------------------- steps

        private void Go(Step step)
        {
            if (step < Step.Welcome || step > Step.Ready) return;
            if (_step != step) _searchResults = null;
            _step = step;
            Render();
            _scroll.ScrollToTop();
            if (step == Step.Style && _hardware == null) _ = DetectAsync();
        }

        private async Task DetectAsync()
        {
            SetStatus(L.T("Looking at this PC…"));
            var hardware = await Task.Run(Hardware.Detect);
            if (!IsLoaded) return;
            _hardware = hardware;
            // The play style is the first start's "settings for this PC": not offered again after the guide.
            S.Set("hardware_offered", true);
            S.Set("engine_preset_asked", true);
            Ws.SaveSoon();
            SetStatus("");
            if (_step == Step.Style) Render();
        }

        /// <summary>Draws the current step from the last check.</summary>
        private void Render()
        {
            var r = _report;
            var game = GameEnvironment.GameDir(S);
            _results.Children.Clear();
            _extra.Children.Clear();
            _actions.Children.Clear();
            _where.Visibility = Visibility.Collapsed;
            var busy = _work != null;

            switch (_step)
            {
                case Step.Welcome:
                    _title.Text = L.T("Set up your game");
                    _intro.Text = L.T("This guide checks your Bloodborne files one step at a time, says what each part is for, and what to do when something is missing. It takes a minute or two.") + "\n\n" +
                                  L.T("What you need:") + "\n" +
                                  L.T("•  Bloodborne dumped from your own PS4: a folder with eboot.bin, sce_sys and dvdroot_ps4 inside. Lantern Engine does not come with the game.") + "\n" +
                                  L.T("•  The 1.09 update, dumped from your PS4 too (unless your dump has it already).") + "\n" +
                                  L.T("•  About 40 GB free, on an SSD if you can: the game loads its world from the disk as you play.") + "\n\n" +
                                  L.T("Nothing is downloaded, and the launcher only changes your game files when you press a button that says so.");
                    break;

                case Step.Folder:
                    _title.Text = L.T("1. Game folder");
                    _intro.Text = L.T("The game folder is the one that holds eboot.bin (the game's program), sce_sys (its details, such as which edition and version it is) and dvdroot_ps4 (the world, sounds and pictures). Dumps usually name it CUSA03173. The launcher plays the game from where it is: nothing is moved or copied.");
                    ShowWhere(game.Length > 0 ? game : L.T("No folder chosen yet"));
                    Lines(SetupStep.Folder);
                    _actions.Children.Add(ActionButton(L.T("Choose folder…"), SymbolRegular.FolderOpen24, ChooseGame, primary: r is not { HasEboot: true }));
                    _actions.Children.Add(ActionButton(busy && _searchResults != null ? L.T("Stop searching") : L.T("Find it for me"), SymbolRegular.Search24, Search));
                    if (r?.GameBelow is { } below)
                        _actions.Children.Add(ActionButton(L.T("Use this folder"), SymbolRegular.Checkmark24, () => UseGame(below), primary: true));
                    if (Directory.Exists(game))
                        _actions.Children.Add(ActionButton(L.T("Open in Explorer"), SymbolRegular.Open24, () => Paths.OpenFolder(game)));
                    if (_searchResults is { Count: > 0 } results)
                    {
                        _extra.Children.Add(Caption(L.T("Bloodborne folders on this PC:")));
                        foreach (var found in results)
                            _extra.Children.Add(FoundRow(found));
                    }
                    break;

                case Step.Version:
                    _title.Text = L.T("2. Game version");
                    _intro.Text = L.T("Bloodborne comes in several editions (each region has its own code, such as CUSA03173) and was updated after release. Lantern Engine is made for version 1.09, the last update: its frame rate, resolution and engine patches only fit that version.") + "\n\n" +
                                  L.T("The update is dumped from the PS4 apart from the game, as a folder of its own with its own eboot.bin and sce_sys (often named CUSA03173-patch).");
                    if (!NeedsGame()) break;
                    Lines(SetupStep.Version);
                    if (r!.Effective != GameCheck.SupportedVersion && GameCheck.SupportedTitles.Contains(r.Title))
                    {
                        _extra.Children.Add(Caption(L.T("Two ways to add the update:") + "\n" +
                            L.T("•  Copy it into the game folder: press \"Choose update folder\" and the launcher does it. Its files replace the older ones, as installing it on a PS4 does.") + "\n" +
                            L.F($"•  Or put the update folder next to the game folder, named like it with -UPDATE at the end ({Path.GetFileName(r.GameDir)}-UPDATE). The engine lays it over the game at every start and the game folder stays as it is.")));
                        _actions.Children.Add(ActionButton(L.T("Choose update folder…"), SymbolRegular.FolderArrowRight24, ChooseUpdate, primary: true));
                    }
                    _actions.Children.Add(ActionButton(L.T("Open in Explorer"), SymbolRegular.Open24, () => Paths.OpenFolder(Directory.GetParent(r.GameDir)?.FullName ?? r.GameDir)));
                    break;

                case Step.Files:
                    _title.Text = L.T("3. Files check");
                    _intro.Text = L.T("A copy can stop half way, an antivirus can take eboot.bin away, and OneDrive can keep files online only. This step looks for the files the engine reads before the game starts.") + "\n\n" +
                                  L.T("When you press Play, the engine also checks that eboot.bin is exactly the 1.09 one, and if it is not, the Game output page says what to do.");
                    if (!NeedsGame()) break;
                    Lines(SetupStep.Files);
                    _actions.Children.Add(ActionButton(L.T("Check again"), SymbolRegular.ArrowSync24, () => _ = CheckAsync()));
                    _actions.Children.Add(ActionButton(L.T("Open in Explorer"), SymbolRegular.Open24, () => Paths.OpenFolder(r!.GameDir)));
                    break;

                case Step.Dlc:
                    _title.Text = L.T("4. The Old Hunters");
                    _intro.Text = L.T("The Old Hunters is Bloodborne's expansion. The Game of the Year edition (CUSA03173) has it built in. Other editions need the add-on dumped from the PS4 as well: a folder with its own sce_sys, named like SPEXPANSIONDLC03. This step is optional: the game plays without it.");
                    if (!NeedsGame()) break;
                    Lines(SetupStep.Dlc);
                    if (r!.Title != "CUSA03173")
                        _actions.Children.Add(ActionButton(L.T("Choose add-ons folder…"), SymbolRegular.FolderOpen24, ChooseDlc));
                    if (S.Str("dlc_dir").Length > 0)
                        _actions.Children.Add(ActionButton(L.T("Clear"), SymbolRegular.Dismiss24, () =>
                        {
                            S.Set("dlc_dir", "");
                            Ws.SaveSoon();
                            _ = CheckAsync();
                        }));
                    break;

                case Step.Style:
                    _title.Text = L.T("5. How it should run");
                    _intro.Text = L.T("Pick what matters most to you. Each choice is sized to this PC (its graphics card, screen and processor), and only changes the launcher's settings, never the game files. You can change any of it later on the Graphics and Display pages.");
                    if (_hardware != null) ShowWhere(_hardware.Summary);
                    foreach (var choice in Hardware.Styles)
                        _results.Children.Add(StyleCard(choice));
                    if (Paths.IsPortDir(Paths.PortDir))
                        _actions.Children.Add(ActionButton(L.T("Apply"), SymbolRegular.Wand24, ApplyStyle, primary: true));
                    else
                        Line(Health.Warning, L.T("The engine folder was not found, so these settings cannot be saved yet. Reinstall the package, or choose the engine folder in Settings."));
                    break;

                case Step.Ready:
                    var worst = r?.Worst() ?? Health.Bad;
                    _title.Text = worst == Health.Good ? L.T("Ready to play") : L.T("Almost there");
                    _intro.Text = worst switch
                    {
                        Health.Good => L.T("Everything the engine needs is in place. Press Finish, then Play."),
                        Health.Warning => L.T("The game can start, but read the notes below first."),
                        _ => L.T("These still need fixing before the game can start. You can finish now and come back any time with \"Setup guide\" on the Play page."),
                    };
                    if (r == null || !r.HasEboot)
                        Line(Health.Bad, r?.Items.FirstOrDefault()?.Text ?? L.T("No game folder chosen yet."));
                    else
                        foreach (var item in r.Items.Where(i => i.Health != Health.Good && !i.Optional))
                            Line(item.Health, item.Text);
                    _extra.Children.Add(Caption(L.T("Next, if you like:") + "\n" +
                        L.T("•  Have a character from shadPS4 or another port? Saves page › Import saves.") + "\n" +
                        L.T("•  A controller works as it is. Keyboard and mouse keys are on the Controls page.")));
                    break;
            }

            _back.Visibility = _step == Step.Welcome ? Visibility.Hidden : Visibility.Visible;
            _next.Content = _step switch
            {
                Step.Welcome => L.T("Start"),
                Step.Ready => L.T("Finish"),
                _ => L.T("Next"),
            };
            foreach (var child in _actions.Children.OfType<Button>())
                child.IsEnabled = !busy || child.Content is string text && text == L.T("Stop searching");
            _back.IsEnabled = _next.IsEnabled = _progress.Visibility != Visibility.Visible;
            ShowStepStates();
        }

        /// <summary>The later steps need a game folder with eboot.bin: until then, they say so.</summary>
        private bool NeedsGame()
        {
            if (_report is { HasEboot: true }) return true;
            Line(Health.Bad, _report == null ? L.T("Checking…") : L.T("Choose the game folder first (step 1)."));
            if (_report != null)
                _actions.Children.Add(ActionButton(L.T("Go to step 1"), SymbolRegular.ArrowLeft24, () => Go(Step.Folder), primary: true));
            return false;
        }

        private void ShowStepStates()
        {
            for (var i = 0; i < _stepRows.Count; i++)
            {
                var step = (Step)i;
                var (row, icon) = _stepRows[i];
                Health? health = step switch
                {
                    Step.Folder when _report != null => _report.Worst(SetupStep.Folder),
                    Step.Version when _report is { HasEboot: true } => _report.Worst(SetupStep.Version),
                    Step.Files when _report is { HasEboot: true } => _report.Worst(SetupStep.Files),
                    Step.Dlc when _report is { HasEboot: true } => _report.Worst(SetupStep.Dlc),
                    _ => null,
                };
                icon.Symbol = health switch
                {
                    Health.Good => SymbolRegular.CheckmarkCircle24,
                    Health.Warning => SymbolRegular.Warning24,
                    Health.Bad => SymbolRegular.ErrorCircle24,
                    _ => step == Step.Ready ? SymbolRegular.Flag24 : SymbolRegular.Circle24,
                };
                icon.SetResourceReference(ForegroundProperty, health switch
                {
                    Health.Good => "GoodTextBrush",
                    Health.Warning => "WarningTextBrush",
                    Health.Bad => "DangerTextBrush",
                    _ => "TextFillColorSecondaryBrush",
                });
                row.SetResourceReference(BackgroundProperty, step == _step ? "SidebarItemSelectedBrush" : "ControlFillColorTransparentBrush");
                row.FontWeight = step == _step ? FontWeights.SemiBold : FontWeights.Normal;
            }
        }

        // ---------------------------------------------------------------- building blocks

        private void ShowWhere(string text)
        {
            _where.Text = text;
            _where.Visibility = Visibility.Visible;
        }

        private void Lines(SetupStep step)
        {
            if (_report == null)
            {
                Line(null, L.T("Checking…"));
                return;
            }
            foreach (var item in _report.Items.Where(i => i.Step == step))
                Line(item.Optional && item.Health == Health.Good ? null : item.Health, item.Text);
        }

        /// <summary>One check line: a state icon (null: information) and its text.</summary>
        private void Line(Health? health, string text)
        {
            var icon = new SymbolIcon
            {
                Symbol = health switch
                {
                    Health.Good => SymbolRegular.CheckmarkCircle24,
                    Health.Warning => SymbolRegular.Warning24,
                    Health.Bad => SymbolRegular.ErrorCircle24,
                    _ => SymbolRegular.Info24,
                },
                FontSize = 16,
                Margin = new Thickness(0, 1, 10, 0),
                VerticalAlignment = VerticalAlignment.Top,
            };
            icon.SetResourceReference(ForegroundProperty, health switch
            {
                Health.Good => "GoodTextBrush",
                Health.Warning => "WarningTextBrush",
                Health.Bad => "DangerTextBrush",
                _ => "TextFillColorSecondaryBrush",
            });
            var line = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, FontSize = 13 };
            var row = new Grid { Margin = new Thickness(0, 6, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.Children.Add(icon);
            Grid.SetColumn(line, 1);
            row.Children.Add(line);
            _results.Children.Add(row);
        }

        private static TextBlock Caption(string text)
        {
            var caption = Wrapped(13, "TextFillColorSecondaryBrush");
            caption.Text = text;
            caption.Margin = new Thickness(0, 14, 0, 0);
            return caption;
        }

        private Button ActionButton(string text, SymbolRegular icon, System.Action click, bool primary = false)
        {
            var button = LauncherPage.SmallButton(text, icon, (_, _) => click(), primary ? ControlAppearance.Primary : ControlAppearance.Secondary);
            button.Margin = new Thickness(6, 0, 0, 6);
            return button;
        }

        /// <summary>A folder the search found: its path, its edition and version, and Use.</summary>
        private UIElement FoundRow(string dir)
        {
            var sfo = GameCheck.ReadSfo(Path.Combine(dir, "sce_sys", "param.sfo"));
            var version = GameCheck.UpdateFolder(dir) is { } update
                ? GameCheck.ReadSfo(Path.Combine(update, "sce_sys", "param.sfo")).GetValueOrDefault("APP_VER", "?")
                : sfo.GetValueOrDefault("APP_VER", "?");
            var text = new TextBlock
            {
                Text = $"{dir}\n{sfo.GetValueOrDefault("TITLE_ID", "?")}  ·  {version}",
                TextWrapping = TextWrapping.Wrap,
                FontSize = 12,
                VerticalAlignment = VerticalAlignment.Center,
            };
            text.SetResourceReference(TextBlock.FontFamilyProperty, "MonoFont");
            var use = ActionButton(L.T("Use"), SymbolRegular.Checkmark24, () => UseGame(dir), primary: true);
            use.Margin = new Thickness(10, 0, 0, 0);
            var row = new Grid { Margin = new Thickness(0, 8, 0, 0) };
            row.ColumnDefinitions.Add(new ColumnDefinition());
            row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            row.Children.Add(text);
            Grid.SetColumn(use, 1);
            row.Children.Add(use);
            return row;
        }

        /// <summary>One play style: its name and what it does, the picked one marked; a click picks it.</summary>
        private UIElement StyleCard(Choice choice)
        {
            var picked = choice.Value == _style;
            var icon = new SymbolIcon
            {
                Symbol = picked ? SymbolRegular.CheckmarkCircle24 : SymbolRegular.Circle24,
                FontSize = 18,
                Margin = new Thickness(0, 1, 12, 0),
                VerticalAlignment = VerticalAlignment.Top,
            };
            icon.SetResourceReference(ForegroundProperty, picked ? "AccentTextFillColorPrimaryBrush" : "TextFillColorSecondaryBrush");
            var text = new StackPanel();
            text.Children.Add(new TextBlock { Text = choice.Label, FontSize = 14, FontWeight = FontWeights.SemiBold });
            var note = Wrapped(12.5, "TextFillColorSecondaryBrush");
            note.Text = choice.Note;
            note.Margin = new Thickness(0, 3, 0, 0);
            text.Children.Add(note);
            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition());
            grid.Children.Add(icon);
            Grid.SetColumn(text, 1);
            grid.Children.Add(text);
            var card = new Border
            {
                Child = grid,
                Padding = new Thickness(14, 10, 14, 10),
                Margin = new Thickness(0, 8, 0, 0),
                CornerRadius = new CornerRadius(10),
                BorderThickness = new Thickness(1),
                Cursor = Cursors.Hand,
            };
            card.SetResourceReference(Border.BackgroundProperty, picked ? "SidebarItemSelectedBrush" : "CardBackgroundFillColorDefaultBrush");
            card.SetResourceReference(Border.BorderBrushProperty, picked ? "AccentTextFillColorPrimaryBrush" : "CardStrokeColorDefaultBrush");
            card.MouseLeftButtonUp += (_, _) =>
            {
                _style = choice.Value;
                SetStatus("");
                Render();
            };
            return card;
        }

        /// <summary>Shows what the picked style changes on this PC, and applies it on the player's OK.</summary>
        private void ApplyStyle()
        {
            var hardware = _hardware ?? Hardware.Detect();
            _hardware = hardware;
            var name = Hardware.Styles.First(c => c.Value == _style).Label;
            var changes = hardware.Recommend(S, Ws.Ini, _style);
            S.Set("play_style", _style);
            Ws.SaveSoon();
            if (changes.Count == 0)
            {
                SetStatus(L.F($"Your settings already match {name}."));
                return;
            }
            var list = string.Join("\n", changes.Select(c => $"{c.Label}: {c.From} → {c.To}"));
            if (!Prompt.Confirm(name, L.F($"{hardware.Summary}\n\n{list}\n\nYou can change any of them later."), L.T("Apply")))
                return;
            foreach (var change in changes) change.Apply();
            Ws.SaveSoon();
            Ws.Log(L.F($"{name} applied: ") + string.Join(", ", changes.Select(c => $"{c.Label.ToLowerInvariant()} {c.To}")) + ".", LogLevel.Good);
            MainWindow.Instance?.ReloadPages();
            SetStatus(L.F($"{name} is set. Press Next."));
        }

        // ---------------------------------------------------------------- checks and actions

        /// <summary>Checks the game folder again on a worker thread (a sleeping disk took seconds).</summary>
        private async Task CheckAsync()
        {
            var check = ++_checks;
            var game = GameEnvironment.GameDir(S);
            var dlc = S.Str("dlc_dir") is { Length: > 0 } chosen ? Paths.Expand(chosen) : "";
            var enabled = S.Bool("dlc_enabled");
            _report = null;
            Render();
            SetStatus(L.T("Checking…"));
            SetupReport report;
            try
            {
                report = await Task.Run(() => GameSetup.Check(game, dlc, enabled));
            }
            catch (Exception e)
            {
                report = new SetupReport { GameDir = game };
                report.Items.Add(new SetupItem(SetupStep.Folder, Health.Bad, L.T("The game folder could not be read: ") + e.Message));
            }
            if (check != _checks || !IsLoaded) return;
            _report = report;
            SetStatus("");
            Render();
        }

        private void SetStatus(string text)
        {
            _status.Text = text;
            _status.Visibility = text.Length == 0 ? Visibility.Collapsed : Visibility.Visible;
        }

        private string? PickFolder(string title, string start)
        {
            var dialog = new Microsoft.Win32.OpenFolderDialog { Title = title };
            try
            {
                if (Directory.Exists(start)) dialog.InitialDirectory = Path.GetFullPath(start);
                return dialog.ShowDialog(this) == true ? dialog.FolderName : null;
            }
            catch (ArgumentException)
            {
                dialog = new Microsoft.Win32.OpenFolderDialog { Title = title };
                return dialog.ShowDialog(this) == true ? dialog.FolderName : null;
            }
        }

        private void ChooseGame()
        {
            var path = PickFolder(L.T("Game folder (the one with eboot.bin)"), GameEnvironment.GameDir(S));
            if (path == null) return;
            // The folder above the game was chosen (E:\Games for E:\Games\CUSA03173): the game in it.
            if (!File.Exists(Path.Combine(path, "eboot.bin")) && GameSetup.GameBelow(path) is { } below)
            {
                path = below;
                Ws.Log(L.T("The game was one level further in: using ") + below, LogLevel.Info);
            }
            UseGame(path);
        }

        private void UseGame(string path)
        {
            S.Set("game_dir", path);
            if (!Paths.IsPortDir(Paths.PortDir))
                Ws.ResolvePort(); // the engine may sit beside the new game folder
            Ws.SaveSoon();
            Ws.Log(L.T("Game folder: ") + path, LogLevel.Step);
            _searchResults = null;
            _ = CheckAsync();
        }

        private async void Search()
        {
            if (_work != null)
            {
                _work.Cancel();
                return;
            }
            _work = new CancellationTokenSource();
            _searchResults = new List<string>();
            var token = _work.Token;
            var count = 0;
            Render();
            SetStatus(L.T("Looking for Bloodborne on this PC (up to 20 seconds)…"));
            List<string> found;
            try
            {
                found = await Task.Run(() => GameSetup.Search(TimeSpan.FromSeconds(20),
                    _ => Dispatcher.BeginInvoke(() => SetStatus(L.F($"Looking for Bloodborne on this PC (up to 20 seconds)… {Interlocked.Increment(ref count)} found"))),
                    token));
            }
            catch (Exception e)
            {
                found = new List<string>();
                Ws.Log(L.T("Game search: ") + e.Message, LogLevel.Warn);
            }
            _work.Dispose();
            _work = null;
            if (!IsLoaded) return;
            _searchResults = found;
            if (found.Count == 0)
            {
                SetStatus(L.T("No Bloodborne folder found. If you have not copied your dump to this PC yet, do that first; otherwise choose it with \"Choose folder\"."));
                Render();
            }
            else if (found.Count == 1 && GameEnvironment.GameDir(S) is var current &&
                     !string.Equals(Path.TrimEndingDirectorySeparator(current), found[0], StringComparison.OrdinalIgnoreCase))
            {
                SetStatus(L.F($"Found it: {found[0]}"));
                UseGame(found[0]);
                _searchResults = found;
            }
            else
            {
                SetStatus(found.Count == 1 ? L.T("Found one: it is already the game folder.") : L.F($"Found {found.Count} folders: press Use beside the one to play (the best one is first)."));
                Render();
            }
        }

        private async void ChooseUpdate()
        {
            if (_report is not { HasEboot: true } r) return;
            var update = PickFolder(L.T("The dumped 1.09 update (the folder with its own eboot.bin)"), Directory.GetParent(r.GameDir)?.FullName ?? r.GameDir);
            if (update == null) return;
            if (GameSetup.CheckUpdate(update, r.GameDir, r.Title) is { } problem)
            {
                Prompt.Tell(L.T("Not the 1.09 update"), problem);
                return;
            }

            long size;
            try { size = Directory.EnumerateFiles(update, "*", SearchOption.AllDirectories).Sum(f => new FileInfo(f).Length); }
            catch (Exception e) { Prompt.Tell(L.T("Add the 1.09 update"), L.T("The update folder could not be read: ") + e.Message); return; }
            var gb = size / (double)(1L << 30);
            try
            {
                var free = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(r.GameDir))!).AvailableFreeSpace;
                if (free < size + (256L << 20))
                {
                    Prompt.Tell(L.T("Add the 1.09 update"), L.F($"The update needs {gb:0.0} GB free on the game's drive, and it has {free / (double)(1L << 30):0.0} GB. Make room, or put the update folder next to the game named {Path.GetFileName(r.GameDir)}-UPDATE instead."));
                    return;
                }
            }
            catch (Exception) { }
            if (!Prompt.Confirm(L.T("Add the 1.09 update"),
                    L.F($"Copy the update ({gb:0.0} GB) into {r.GameDir}?") + "\n\n" +
                    L.T("Its files replace the older ones there (eboot.bin, sce_sys and part of dvdroot_ps4), as installing the update on a PS4 does. The update folder itself stays as it is. This can take a few minutes."),
                    L.T("Copy into the game folder")))
                return;

            _work = new CancellationTokenSource();
            var token = _work.Token;
            _progress.Value = 0;
            _progress.Visibility = Visibility.Visible;
            Render();
            SetStatus(L.T("Copying the update…"));
            var progress = new Progress<double>(p =>
            {
                _progress.Value = p;
                SetStatus(L.F($"Copying the update… {p * 100:0}%"));
            });
            string? failed = null;
            try
            {
                await Task.Run(() => GameSetup.MergeUpdate(update, r.GameDir, progress, token));
                Ws.Log(L.T("The 1.09 update was copied into the game folder: ") + r.GameDir, LogLevel.Good);
            }
            catch (OperationCanceledException)
            {
                failed = L.T("Stopped. Copy the update again to finish it.");
            }
            catch (Exception e)
            {
                failed = L.T("Copying the update failed: ") + e.Message;
                Ws.Log(failed, LogLevel.Warn);
            }
            _work.Dispose();
            _work = null;
            if (!IsLoaded) return;
            _progress.Visibility = Visibility.Collapsed;
            await CheckAsync();
            SetStatus(failed ?? L.T("The update is in the game folder."));
        }

        private void ChooseDlc()
        {
            var game = GameEnvironment.GameDir(S);
            var path = PickFolder(L.T("Add-ons folder (holds SPEXPANSIONDLC03 or similar)"), Directory.GetParent(game)?.FullName ?? game);
            if (path == null) return;
            S.Set("dlc_dir", path);
            Ws.SaveSoon();
            Ws.Log(L.T("DLC folder: ") + path, LogLevel.Step);
            _ = CheckAsync();
        }
    }
}
