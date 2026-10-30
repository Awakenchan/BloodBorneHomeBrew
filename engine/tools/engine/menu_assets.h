// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace menu_assets {
using Bytes = std::vector<std::uint8_t>;

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr std::size_t kMostBytes = 64u << 20;

struct Dcx {
    Bytes header;
    Bytes data;
};
Dcx DcxRead(const Bytes& file);
Bytes DcxWrite(const Dcx& dcx);

struct BinderFile {
    std::uint64_t flags = 0;
    std::uint32_t id = 0;
    Bytes name;
    Bytes data;
};
struct Binder {
    Bytes header;
    std::size_t entry_bytes = 0;
    bool wide_names = false;
    std::vector<BinderFile> files;
};
Binder BinderRead(const Bytes& file);
Bytes BinderWrite(const Binder& binder);

struct Fmg {
    Bytes header;
    std::map<std::int32_t, std::optional<std::u16string>> text;
};
Fmg FmgRead(const Bytes& file);
Bytes FmgWrite(const Fmg& fmg);

std::u16string Utf16(const std::string& utf8);

struct PcMessage {
    std::string text, help;
};
// The language bundles that carry their own translation of the PC port's strings; every other
// bundle gets the English columns.
enum class Language { English, Chinese, Russian };
// The language of a msg/<folder> bundle: "zhocn" is Chinese, "rusru" is Russian.
Language LanguageOf(const std::string& folder);
std::map<std::int32_t, PcMessage> PcMessages(Language language);
std::map<std::int32_t, PcMessage> PcMessages(bool chinese = false);
enum class Kind { Menu, Question, Event };
Kind KindOf(std::int32_t id);

struct PlateTitles {
    std::optional<std::string> host, guest;
};
Bytes MenuMessages(const Bytes& dcx, Language language, const PlateTitles& titles = {});
Bytes MenuMessages(const Bytes& dcx, bool chinese = false);
Bytes OptionMovie(const Bytes& gfx);
Bytes AltarScripts(const Bytes& dcx);
}
