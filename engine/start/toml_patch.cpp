// SPDX-License-Identifier: GPL-3.0-or-later
#include "toml_patch.h"

#include <cctype>

namespace start {
namespace {

constexpr std::size_t kLargestFile = 16 * 1024 * 1024;

std::string Clean(std::string_view text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string Hex(const std::string& value, int line) {
    std::string out;
    for (const char c : value) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) throw Error("line " + std::to_string(line) + ": not hexadecimal");
        out += c;
    }
    if (out.empty() || out.size() % 2) throw Error("line " + std::to_string(line) + ": not whole bytes");
    return out;
}

std::string Value(const std::string& raw, int line) {
    if (raw.size() >= 2 && raw.front() == '"') {
        const std::size_t close = raw.find('"', 1);
        if (close == std::string::npos) throw Error("line " + std::to_string(line) + ": unclosed string");
        const std::string rest = Clean(std::string_view(raw).substr(close + 1));
        if (!rest.empty() && rest.front() != '#') throw Error("line " + std::to_string(line) + ": text after the value");
        return raw.substr(1, close - 1);
    }
    const std::string number = Clean(std::string_view(raw).substr(0, raw.find('#')));
    if (number.empty() || number.find_first_not_of("0123456789") != std::string::npos)
        throw Error("line " + std::to_string(line) + ": only strings and whole numbers are read");
    return number;
}

void Attribute(XmlElement& element, const char* key, const std::string& value) {
    element.attributes.emplace_back(key, value);
}

std::unique_ptr<XmlElement> Element(const char* name) {
    auto element = std::make_unique<XmlElement>();
    element->name = name;
    return element;
}

}

std::unique_ptr<XmlElement> TomlPatchAsXml(std::string_view text, const std::string& fallback_name) {
    if (text.size() > kLargestFile) throw Error("the file is too large");
    std::string name = fallback_name, note, author, order = "0", group;
    auto list = Element("PatchList");
    struct Site {
        std::string address, expect, bytes;
    };
    std::vector<Site> sites;
    bool in_patch = false, in_site = false;
    int number = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = Clean(text.substr(at, end - at));
        at = end + 1;
        ++number;
        if (line.empty() || line.front() == '#') continue;
        if (line.front() == '[') {
            in_patch = line == "[patch]";
            in_site = line.rfind("[site.", 0) == 0 && line.back() == ']';
            if (!in_patch && !in_site) throw Error("line " + std::to_string(number) + ": unknown section");
            if (in_site) sites.emplace_back();
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) throw Error("line " + std::to_string(number) + ": expected key = value");
        const std::string key = Clean(std::string_view(line).substr(0, equals));
        const std::string value = Value(Clean(std::string_view(line).substr(equals + 1)), number);
        if (in_patch) {
            if (key == "name") name = value;
            else if (key == "description") note = value;
            else if (key == "author") author = value;
            else if (key == "order") order = value;
            else if (key == "group") group = value;
        } else if (in_site) {
            if (key == "address") sites.back().address = value;
            else if (key == "expect") sites.back().expect = Hex(value, number);
            else if (key == "bytes") sites.back().bytes = Hex(value, number);
        } else {
            throw Error("line " + std::to_string(number) + ": a value outside any section");
        }
    }
    if (sites.empty()) throw Error("no patch sites");
    for (const Site& site : sites) {
        if (site.address.empty() || site.bytes.empty()) throw Error("a site without an address or bytes");
        if (!site.expect.empty() && site.expect.size() != site.bytes.size()) throw Error("a site at " + site.address + " expects a different length than it writes");
        auto entry = Element("Line");
        Attribute(*entry, "Type", "bytes");
        Attribute(*entry, "Address", site.address);
        Attribute(*entry, "Value", site.bytes);
        if (!site.expect.empty()) Attribute(*entry, "Expect", site.expect);
        list->children.push_back(std::move(entry));
    }
    auto meta = Element("Metadata");
    Attribute(*meta, "Name", name);
    Attribute(*meta, "Note", note);
    Attribute(*meta, "Author", author);
    Attribute(*meta, "AppVer", "01.09");
    Attribute(*meta, "AppElf", "eboot.bin");
    Attribute(*meta, "isEnabled", "false");
    Attribute(*meta, "Order", order);
    Attribute(*meta, "Group", group);
    meta->children.push_back(std::move(list));
    auto root = Element("Patch");
    root->children.push_back(std::move(meta));
    return root;
}

}
