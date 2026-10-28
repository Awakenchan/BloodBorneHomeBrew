using System.Windows;
using System.Windows.Controls;
using BloodborneLauncher.Core;
using Wpf.Ui.Controls;
using Button = Wpf.Ui.Controls.Button;

namespace BloodborneLauncher.Views
{
    public sealed class OnlinePage : LauncherPage
    {
        private readonly ToggleSwitch _online;
        private readonly StackPanel _results = new();
        private readonly Button _test;

        public OnlinePage() : base(L.T("Online"),
            L.T("1. Turn on Play online. 2. Connect your Hunter's Dream account below (with Discord, or without it). ") +
            L.T("3. In the game, choose online at the title screen: The Hunter's Dream's welcome message means you are online. ") +
            L.T("Everyone needs game version 1.09."))
        {
            foreach (var key in new[] { "online_host", "online_port", "online_webapi", "online_game_api", "online_thd_account" })
                if (Ini.Get(key).Length > 0) Ini.Set(key, "");
            Ini.Set("online_pool", S.HuntersDreamToken.Length > 0 && S.Str("hd_name").Length > 0 ? "huntersdream" : "");

            if (HuntersDream.OnlinePaused)
                Body.Children.Add(Text(L.T("Online play is paused in this version. The game starts offline whatever is set here; ") +
                                       L.T("your settings are kept for when it returns."), "Faint"));

            Group(L.T("Play"));
            _online = ToggleRow(SymbolRegular.Globe24, L.T("Play online"), L.T("Off: the game stays offline"),
                () => Ini.Get("online_enabled") == "1", v => Ini.Set("online_enabled", v ? "1" : "0"));

            Group(L.T("Seamless co-op"));
            ToggleRow(SymbolRegular.PeopleTeam24, L.T("Seamless co-op"),
                L.T("Play the campaign together in your own encrypted room: no Hunter's Dream, no ports to open"),
                () => Ini.Get("seamless_enabled") == "1", v => Ini.Set("seamless_enabled", v ? "1" : "0"));
            _role = ComboRow(SymbolRegular.Crown24, L.T("Role"), L.T("One player hosts, the others join the host's world"),
                Choices.SeamlessRoles, () => Ini.Get("seamless_role", "guest"), v => Ini.Set("seamless_role", v));
            IniText(SymbolRegular.Person24, L.T("Your name"), L.T("Shown to the others (up to 16 characters, no spaces)"), "Hunter", "seamless_player", 220);
            _room = TextRow(SymbolRegular.Door24, L.T("Room name"), L.T("Everyone types the same room name"), "",
                () => Ini.Get("seamless_room"), v => Ini.Set("seamless_room", v.Trim()), width: 220);
            _password = TextRow(SymbolRegular.LockClosed24, L.T("Room password"), L.T("Everyone types the same password; the room's traffic is encrypted with it"),
                "", () => S.SeamlessPassword, v => { S.SeamlessPassword = v; Ws.SaveNow(); }, width: 220);
            var invite = new StackPanel { Orientation = Orientation.Horizontal };
            invite.Children.Add(SmallButton(L.T("Copy invite code"), SymbolRegular.Copy24, (_, _) => CopyInvite()));
            var paste = SmallButton(L.T("Paste invite code"), SymbolRegular.ClipboardPaste24, (_, _) => PasteInvite());
            paste.Margin = new Thickness(8, 0, 0, 0);
            invite.Children.Add(paste);
            Add(Card(SymbolRegular.Link24, L.T("Invite code"),
                L.T("The host copies it and sends it to friends; they paste it here. It holds the room name and password"), invite, out _));

            Group(L.T("Server"));
            ComboRow(SymbolRegular.Server24, L.T("Server"), L.T("Messages, bloodstains, ghosts, co-op and invasions"),
                Choices.OnlineServers, () => "huntersdream", _ => { });
            Add(Card(SymbolRegular.Globe24, L.T("The Hunter's Dream"), L.T("Players online, the map and your hunter's profile"),
                SmallButton(L.T("Open website"), SymbolRegular.Open24, (_, _) =>
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(LauncherSettings.OnlineCommunity) { UseShellExecute = true })),
                out _));

            Group(L.T("The Hunter's Dream account"));
            _hdButton = SmallButton(L.T("Connect"), SymbolRegular.Person24, async (_, _) => await LinkAsync());
            Add(Card(SymbolRegular.PersonAccounts24, L.T("Hunter's Dream account"), null, _hdButton, out _hdHint));
            ShowLink();

            Group(L.T("Connection"));
            IniText(SymbolRegular.Router24, L.T("Local P2P port"), L.T("Empty: 3658, the PS4's"), "3658", "online_p2p_port", 120);
            ComboRow(SymbolRegular.Timer24, L.T("Summon sign timeout"), L.T("How long a sign you touched waits for its host before giving up"),
                Choices.SignTimeout, () => Ini.Get("online_sign_timeout", "30"), v => Ini.Set("online_sign_timeout", v));
            _test = SmallButton(L.T("Test"), SymbolRegular.PlayCircle24, async (_, _) => await TestAsync());
            Add(Card(SymbolRegular.Pulse24, L.T("Test the connection"), L.T("Checks The Hunter's Dream: game server, co-op sessions and the relay summons use"), _test, out _));
            Body.Children.Add(_results);
        }

        private readonly System.Windows.Controls.TextBox _room, _password;
        private readonly FrameworkElement _role;
        private const string InvitePrefix = "LANTERN-ROOM:";

        private static string RandomText(int length)
        {
            const string letters = "abcdefghjkmnpqrstuvwxyz23456789";
            var bytes = System.Security.Cryptography.RandomNumberGenerator.GetBytes(length);
            return new string(bytes.Select(b => letters[b % letters.Length]).ToArray());
        }

        private void CopyInvite()
        {
            if (Ini.Get("seamless_room").Length == 0) _room.Text = "room-" + RandomText(8);
            if (S.SeamlessPassword.Length == 0) _password.Text = RandomText(16);
            Ini.Set("seamless_room", _room.Text.Trim());
            S.SeamlessPassword = _password.Text;
            Ws.SaveNow();
            var parts = new List<string> { Ini.Get("seamless_room"), S.SeamlessPassword };
            if (Ini.Get("seamless_server").Length > 0) parts.Add(Ini.Get("seamless_server"));
            var code = InvitePrefix + Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(string.Join('\n', parts)))
                .TrimEnd('=').Replace('+', '-').Replace('/', '_');
            if (ClipboardText.Set(code)) Ws.Log(L.T("Invite code copied: send it to your friends"), LogLevel.Good);
        }

        private void PasteInvite()
        {
            var text = System.Windows.Clipboard.ContainsText() ? System.Windows.Clipboard.GetText().Trim() : "";
            var start = text.IndexOf(InvitePrefix, StringComparison.OrdinalIgnoreCase);
            string[] parts = Array.Empty<string>();
            if (start >= 0)
            {
                var body = new string(text[(start + InvitePrefix.Length)..].TakeWhile(c => char.IsLetterOrDigit(c) || c == '-' || c == '_').ToArray())
                    .Replace('-', '+').Replace('_', '/');
                body = body.PadRight(body.Length + (4 - body.Length % 4) % 4, '=');
                try { parts = System.Text.Encoding.UTF8.GetString(Convert.FromBase64String(body)).Split('\n'); }
                catch (FormatException) { }
            }
            if (parts.Length < 2 || parts[0].Length == 0)
            {
                Prompt.Tell(L.T("Invite code"), L.T("The clipboard holds no invite code. Copy the whole code the host sent you, then paste again."));
                return;
            }
            _room.Text = parts[0];
            _password.Text = parts[1];
            Ini.Set("seamless_room", parts[0]);
            S.SeamlessPassword = parts[1];
            Ini.Set("seamless_server", parts.Length > 2 ? parts[2] : "");
            Ini.Set("seamless_role", "guest");
            if (_role is Segmented segmented) segmented.Select(0, notify: false);
            else if (_role is System.Windows.Controls.ComboBox box) box.SelectedIndex = 0;
            Ws.SaveNow();
            Ws.Log(L.F($"Invite code pasted: room {parts[0]}"), LogLevel.Good);
        }

        private readonly Button _hdButton;
        private readonly System.Windows.Controls.TextBlock _hdHint;
        private CancellationTokenSource? _linking;

        private void ShowLink()
        {
            var name = S.Str("hd_name");
            var linked = name.Length > 0 && S.HuntersDreamToken.Length > 0;
            var refused = linked ? S.HuntersDreamRefused : "";
            SetHint(_hdHint, !linked ? L.T("Not connected. Connect with Discord, or create an account here without it")
                : refused.Length > 0 ? L.F($"The Hunter's Dream refused the sign-in as {name} ({refused}). Sign in again: until then nobody can summon or invite you")
                : L.F($"Connected as {name}"));
            _hdHint.SetResourceReference(System.Windows.Controls.TextBlock.ForegroundProperty,
                refused.Length > 0 ? "WarningTextBrush" : "TextFillColorTertiaryBrush");
            if (_linking is null) _hdButton.Content = !linked ? L.T("Connect") : refused.Length > 0 ? L.T("Sign in again") : L.T("Disconnect");
        }

        private async Task AccountWithoutDiscordAsync(bool recover)
        {
            var fields = recover
                ? new[] { new Prompt.Field(L.T("Account name")), new Prompt.Field(L.T("Recovery code")) }
                : new[] { new Prompt.Field(L.T("Account name"), "", L.T("Your online name (letters, digits, - and _)")) };
            var answer = Prompt.Ask(recover ? L.T("Recover the account") : L.T("Create an account"),
                recover ? L.T("The name and the recovery code you wrote down. Other PCs signed in to this account are signed out.")
                        : L.T("An account held by this PC. You get a recovery code: write it down, it is the only way back in on another PC or after signing out."),
                fields);
            if (answer is null || answer[0].Trim().Length == 0) return;
            try
            {
                var (name, token, code) = recover
                    ? await HuntersDream.RecoverAsync(answer[0].Trim(), answer[1].Trim(), CancellationToken.None)
                    : await HuntersDream.CreateAccountAsync(answer[0].Trim(), CancellationToken.None);
                S.HuntersDreamToken = token;
                S.Set("hd_name", name);
                Ini.Set("online_pool", "huntersdream");
                Ws.SaveNow();
                Ws.Log(L.F($"The Hunter's Dream: signed in as {name}"), LogLevel.Good);
                if (code.Length > 0)
                {
                    ClipboardText.Set(code);
                    Prompt.Tell(L.T("Your recovery code"), L.F($"Signed in as {name}.\n\nRecovery code (copied to the clipboard):\n\n{code}\n\n") +
                        L.T("Write it down now: it is shown only once, and it is the only way back into this account."));
                }
            }
            catch (Exception e)
            {
                Prompt.Tell(recover ? L.T("Recovering failed") : L.T("Creating the account failed"), e.Message);
            }
            finally
            {
                ShowLink();
            }
        }

        private async Task LinkAsync()
        {
            if (_linking is { } running)
            {
                running.Cancel();
                return;
            }
            if (S.HuntersDreamToken is { Length: > 0 } token && S.HuntersDreamRefused.Length == 0)
            {
                await HuntersDream.LogoutAsync(token);
                S.HuntersDreamToken = "";
                S.Set("hd_name", "");
                Ini.Set("online_pool", "");
                Ws.SaveNow();
                Ws.Log(L.T("The Hunter's Dream: disconnected"), LogLevel.Info);
                ShowLink();
                return;
            }
            var how = Prompt.Choose(L.T("The Hunter's Dream account"),
                L.T("Sign in with Discord, or use an account held by this PC (a name and a recovery code, no Discord needed)."),
                L.T("Cancel"), "Discord", L.T("Without Discord…"), L.T("Recover with a code…"));
            if (how <= 0) return;
            if (how >= 2)
            {
                await AccountWithoutDiscordAsync(recover: how == 3);
                return;
            }
            _linking = new CancellationTokenSource();
            _hdButton.Content = L.T("Cancel");
            try
            {
                var link = await HuntersDream.StartLinkAsync(_linking.Token);
                ClipboardText.Set(link.UserCode);
                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(link.Page) { UseShellExecute = true });
                SetHint(_hdHint, L.F($"Enter the code {link.UserCode} on the page that opened (it is on the clipboard), then approve"));
                _hdHint.SetResourceReference(System.Windows.Controls.TextBlock.ForegroundProperty, "TextFillColorTertiaryBrush");
                var (name, linked) = await HuntersDream.WaitLinkAsync(link, _linking.Token);
                S.HuntersDreamToken = linked;
                S.Set("hd_name", name);
                Ini.Set("online_pool", "huntersdream");
                Ws.SaveNow();
                Ws.Log(L.F($"The Hunter's Dream: connected as {name}"), LogLevel.Good);
            }
            catch (OperationCanceledException) { }
            catch (Exception e)
            {
                Prompt.Tell(L.T("Connecting failed"), e.Message);
            }
            finally
            {
                _linking.Dispose();
                _linking = null;
                ShowLink();
            }
        }

        private void IniText(SymbolRegular icon, string title, string? hint, string placeholder, string key, double width = 300) =>
            TextRow(icon, title, hint, placeholder, () => Ini.Get(key), v => Ini.Set(key, v.Trim()), width: width);

        public override void OnShown()
        {
            _online.IsChecked = Ini.Get("online_enabled") == "1";
            if (_linking is null) ShowLink();
        }

        private async Task TestAsync()
        {
            _results.Children.Clear();
            _test.IsEnabled = false;
            _test.Content = L.T("Testing…");
            try
            {
                var results = await Online.CheckAsync(Online.GameServer(Ini.Get("online_game_api")));
                foreach (var r in results)
                {
                    var said = r.Ok ? (r.Detail.Length > 0 ? L.F($"Reachable: {r.Detail}") : L.T("Reachable")) : L.F($"Not reachable ({r.Detail})");
                    var card = Card(r.Ok ? SymbolRegular.CheckmarkCircle24 : SymbolRegular.ErrorCircle24,
                        $"{r.Name}: {r.Address}", said, null, out _);
                    card.ShowIcon = true;
                    card.Icon.SetResourceReference(ForegroundProperty, r.Ok ? "GoodTextBrush" : "DangerTextBrush");
                    SetHint(card.Hint, said);
                    card.Margin = new Thickness(24, 0, 0, 4);
                    _results.Children.Add(card);
                    Ws.Log($"{r.Name} {r.Address}: {(r.Ok ? "reachable" : "not reachable " + r.Detail)}", r.Ok ? LogLevel.Good : LogLevel.Warn);
                }
            }
            finally
            {
                _test.IsEnabled = true;
                _test.Content = L.T("Test");
            }
        }
    }
}
