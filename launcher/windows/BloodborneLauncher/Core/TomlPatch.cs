using System.Xml.Linq;

namespace BloodborneLauncher.Core
{
    public static class TomlPatch
    {
        public static XElement AsXml(string text, string fallbackName)
        {
            var name = fallbackName;
            var note = "";
            var author = "";
            var group = "";
            var section = "";
            var lines = new List<XElement>();
            string? address = null, value = null;
            void Flush()
            {
                if (address != null && value != null)
                    lines.Add(new XElement("Line", new XAttribute("Type", "bytes"), new XAttribute("Address", address), new XAttribute("Value", value)));
                address = value = null;
            }
            foreach (var raw in text.Split('\n'))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line[0] == '#') continue;
                if (line[0] == '[')
                {
                    Flush();
                    section = line;
                    continue;
                }
                var equals = line.IndexOf('=');
                if (equals < 0) throw new FormatException(line);
                var key = line[..equals].Trim();
                var item = line[(equals + 1)..].Trim().Trim('"');
                if (section == "[patch]")
                {
                    if (key == "name") name = item;
                    else if (key == "description") note = item;
                    else if (key == "author") author = item;
                    else if (key == "group") group = item;
                }
                else if (section.StartsWith("[site.", StringComparison.Ordinal))
                {
                    if (key == "address") address = item;
                    else if (key == "bytes") value = item.Replace(" ", "");
                }
            }
            Flush();
            if (lines.Count == 0) throw new FormatException("no patch sites");
            if (group.Length > 0) note = (note.Length > 0 ? note + "\n" : "") + L.F($"Only one patch of the group {group} is used");
            return new XElement("Patch", new XElement("Metadata",
                new XAttribute("Name", name), new XAttribute("Note", note), new XAttribute("Author", author),
                new XAttribute("AppVer", "01.09"), new XAttribute("AppElf", "eboot.bin"), new XAttribute("isEnabled", "false"),
                new XElement("PatchList", lines)));
        }
    }
}
