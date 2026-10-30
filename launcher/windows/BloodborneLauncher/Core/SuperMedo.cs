using System.Net.Http;
using System.Net.Sockets;

namespace BloodborneLauncher.Core
{
    public static class SuperMedo
    {
        public const string Id = "supermedo";
        public const string Host = "game.supermedo.com";
        public const int Port = 31313;
        public const string WebApi = "http://game.supermedo.com:31315/";
        public const string GameApi = "https://game.supermedo.com/";
        public const string Register = "https://game.supermedo.com/account/register";

        public static bool Selected(IniFile ini) => ini.Get("online_server") == Id;

        public static void Apply(IniFile ini, LauncherSettings s)
        {
            var selected = Selected(ini);
            Set(ini, "online_host", selected ? Host : "");
            Set(ini, "online_port", selected ? Port.ToString() : "");
            Set(ini, "online_webapi", selected ? WebApi : "");
            Set(ini, "online_game_api", selected ? GameApi : "");
            Set(ini, "online_thd_account", "");
            Set(ini, "online_pool", !selected && s.HuntersDreamToken.Length > 0 && s.Str("hd_name").Length > 0 ? "huntersdream" : "");
        }

        public static string? Problem(IniFile ini, LauncherSettings s) =>
            ini.Get("online_username").Contains('@') ? L.T("The account name is the name registered on the server, not an email address.")
            : ini.Get("online_username").Trim().Length == 0 ? L.T("Enter your SuperMedo account name on the Online page first.")
            : s.OnlinePassword.Length == 0 ? L.T("Enter your SuperMedo account password on the Online page first.")
            : null;

        public static async Task<List<Online.Result>> CheckAsync()
        {
            var results = new List<Online.Result>();
            try
            {
                using var tcp = new TcpClient();
                using var cancel = new CancellationTokenSource(TimeSpan.FromSeconds(5));
                await tcp.ConnectAsync(Host, Port, cancel.Token);
                results.Add(new Online.Result(L.T("Co-op server"), $"{Host}:{Port}", true, ""));
            }
            catch (Exception e)
            {
                results.Add(new Online.Result(L.T("Co-op server"), $"{Host}:{Port}", false, e.GetBaseException().Message));
            }
            foreach (var (name, url) in new[] { (L.T("Web API"), WebApi), (L.T("Game server"), GameApi) })
            {
                try
                {
                    using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(7) };
                    using var response = await http.GetAsync(url);
                    results.Add(new Online.Result(name, url, true, $"HTTP {(int)response.StatusCode}"));
                }
                catch (Exception e)
                {
                    results.Add(new Online.Result(name, url, false, e.GetBaseException().Message));
                }
            }
            return results;
        }

        private static void Set(IniFile ini, string key, string value)
        {
            if (ini.Get(key) != value) ini.Set(key, value);
        }
    }
}
