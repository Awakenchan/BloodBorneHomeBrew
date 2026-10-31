// SPDX-License-Identifier: GPL-2.0-or-later
#include "menu_assets.h"
#include "rebirth_script.h"
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>

namespace menu_assets {
namespace {
std::uint64_t Le(const Bytes& b, std::size_t at, int n) {
    if (at > b.size() || std::size_t(n) > b.size() - at) throw Error("a file ends early");
    std::uint64_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = v << 8 | b[at + i];
    return v;
}
std::uint16_t U16(const Bytes& b, std::size_t at) { return std::uint16_t(Le(b, at, 2)); }
std::uint32_t U32(const Bytes& b, std::size_t at) { return std::uint32_t(Le(b, at, 4)); }
std::uint64_t U64(const Bytes& b, std::size_t at) { return Le(b, at, 8); }
std::uint32_t Be32(const Bytes& b, std::size_t at) {
    const std::uint32_t v = U32(b, at);
    return (v >> 24) | (v >> 8 & 0xff00) | (v << 8 & 0xff0000) | (v << 24);
}
void SetLe(Bytes& b, std::size_t at, std::uint64_t v, int n) {
    for (int i = 0; i < n; ++i) b[at + i] = std::uint8_t(v >> (8 * i));
}
void SetBe32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = std::uint8_t(v >> (24 - 8 * i));
}
void PutLe(Bytes& b, std::uint64_t v, int n) {
    b.resize(b.size() + n);
    SetLe(b, b.size() - n, v, n);
}
void Append(Bytes& b, const Bytes& more) { b.insert(b.end(), more.begin(), more.end()); }
bool Has(const Bytes& b, std::size_t at, const char* magic, std::size_t n) {
    return at <= b.size() && n <= b.size() - at && std::memcmp(b.data() + at, magic, n) == 0;
}
std::size_t WideEnd(const Bytes& b, std::size_t from) {
    for (std::size_t at = from; at + 1 < b.size(); at += 2)
        if (!b[at] && !b[at + 1]) return at;
    throw Error("a string runs past the end");
}

enum Code { kEnd = 0, kShowFrame = 1, kPlace2 = 26, kSprite = 39, kPlace3 = 70, kSymbolClass = 76 };
const std::set<int> kDefines = {2, 6, 7, 10, 11, 14, 20, 21, 22, 32, 33, 34, 35, 36, 37, 39,
                                46, 48, 60, 75, 83, 84, 90, 91, 1001, 1003, 1005, 1006, 1007, 1008, 1009, 1010};

struct Tag {
    int code = 0;
    Bytes body;
    Bytes raw;
};

std::vector<Tag> ReadTags(const Bytes& b, std::size_t at, std::size_t end) {
    std::vector<Tag> out;
    while (at < end) {
        const std::size_t head = at;
        const std::uint16_t code_length = U16(b, at);
        at += 2;
        std::size_t length = code_length & 0x3f;
        if (length == 0x3f) {
            length = U32(b, at);
            at += 4;
        }
        if (length > end - std::min(at, end)) throw Error("a movie tag runs past its end");
        Tag t;
        t.code = code_length >> 6;
        t.body.assign(b.begin() + at, b.begin() + at + length);
        t.raw.assign(b.begin() + head, b.begin() + at + length);
        at += length;
        out.push_back(std::move(t));
        if (out.back().code == kEnd) break;
    }
    return out;
}

void WriteTag(Bytes& out, const Tag& t) {
    if (!t.raw.empty()) return Append(out, t.raw);
    if (t.body.size() < 0x3f) {
        PutLe(out, std::uint32_t(t.code << 6 | t.body.size()), 2);
    } else {
        PutLe(out, std::uint32_t(t.code << 6 | 0x3f), 2);
        PutLe(out, t.body.size(), 4);
    }
    Append(out, t.body);
}
Tag Changed(int code, Bytes body) { return Tag{code, std::move(body), {}}; }

struct BitReader {
    const Bytes& b;
    std::size_t at;
    int bit = 0;
    std::uint32_t Unsigned(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            if (at >= b.size()) throw Error("a bit field runs past the end");
            v = v << 1 | (b[at] >> (7 - bit) & 1);
            if (++bit == 8) bit = 0, ++at;
        }
        return v;
    }
    std::int64_t Signed(int n) {
        const std::uint32_t v = Unsigned(n);
        return n && (v >> (n - 1) & 1) ? std::int64_t(v) - (std::int64_t(1) << n) : std::int64_t(v);
    }
    std::size_t End() { return bit ? at + 1 : at; }
};
struct BitWriter {
    Bytes out;
    int bit = 0;
    void Put(std::uint64_t v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (!bit) out.push_back(0);
            out.back() |= std::uint8_t((v >> i & 1) << (7 - bit));
            bit = (bit + 1) % 8;
        }
    }
};
int BitsFor(std::int64_t v) {
    int n = 1;
    while (v < -(std::int64_t(1) << (n - 1)) || v >= (std::int64_t(1) << (n - 1))) ++n;
    return n;
}

struct Matrix {
    bool scaled = false, rotated = false;
    int scale_bits = 0, rotate_bits = 0;
    std::uint32_t sx = 0, sy = 0, r0 = 0, r1 = 0;
    std::int64_t tx = 0, ty = 0;
};
Matrix ReadMatrix(BitReader& r) {
    Matrix m;
    if ((m.scaled = r.Unsigned(1))) {
        m.scale_bits = int(r.Unsigned(5));
        m.sx = r.Unsigned(m.scale_bits);
        m.sy = r.Unsigned(m.scale_bits);
    }
    if ((m.rotated = r.Unsigned(1))) {
        m.rotate_bits = int(r.Unsigned(5));
        m.r0 = r.Unsigned(m.rotate_bits);
        m.r1 = r.Unsigned(m.rotate_bits);
    }
    const int n = int(r.Unsigned(5));
    m.tx = r.Signed(n);
    m.ty = r.Signed(n);
    return m;
}
Bytes WriteMatrix(const Matrix& m) {
    BitWriter w;
    w.Put(m.scaled, 1);
    if (m.scaled) w.Put(m.scale_bits, 5), w.Put(m.sx, m.scale_bits), w.Put(m.sy, m.scale_bits);
    w.Put(m.rotated, 1);
    if (m.rotated) w.Put(m.rotate_bits, 5), w.Put(m.r0, m.rotate_bits), w.Put(m.r1, m.rotate_bits);
    const int n = std::max(BitsFor(m.tx), BitsFor(m.ty));
    w.Put(n, 5);
    w.Put(std::uint64_t(m.tx) & ((std::uint64_t(1) << n) - 1), n);
    w.Put(std::uint64_t(m.ty) & ((std::uint64_t(1) << n) - 1), n);
    return w.out;
}
double Fixed(std::uint32_t v, int bits) {
    return double(bits && (v >> (bits - 1) & 1) ? std::int64_t(v) - (std::int64_t(1) << bits) : std::int64_t(v)) / 65536.0;
}

struct Place {
    int code = 0;
    std::uint8_t flags = 0;
    std::uint16_t depth = 0;
    std::size_t character_at = 0, matrix_at = 0, matrix_end = 0, name_at = 0, name_end = 0;
    std::optional<std::uint16_t> character;
    std::optional<Matrix> matrix;
    std::optional<std::string> name;
};
bool IsPlace(const Tag& t) { return t.code == kPlace2 || t.code == kPlace3; }
Place ReadPlace(const Tag& t) {
    const Bytes& b = t.body;
    Place p;
    p.code = t.code;
    p.flags = std::uint8_t(Le(b, 0, 1));
    std::size_t at = 1;
    std::uint8_t more = 0;
    if (t.code == kPlace3) more = std::uint8_t(Le(b, at++, 1));
    p.depth = U16(b, at);
    at += 2;
    if (t.code == kPlace3 && ((more & 0x08) || ((more & 0x10) && (p.flags & 0x02)))) {
        while (at < b.size() && b[at]) ++at;
        ++at;
    }
    p.character_at = at;
    if (p.flags & 0x02) {
        p.character = U16(b, at);
        at += 2;
    }
    p.matrix_at = p.matrix_end = at;
    if (p.flags & 0x04) {
        BitReader r{b, at};
        p.matrix = ReadMatrix(r);
        at = p.matrix_end = r.End();
    }
    if (p.flags & 0x08) {
        BitReader r{b, at};
        const bool add = r.Unsigned(1), mult = r.Unsigned(1);
        const int n = int(r.Unsigned(4));
        if (mult) r.Unsigned(4 * n);
        if (add) r.Unsigned(4 * n);
        at = r.End();
    }
    if (p.flags & 0x10) at += 2;
    p.name_at = p.name_end = at;
    if (p.flags & 0x20) {
        std::size_t end = at;
        while (end < b.size() && b[end]) ++end;
        if (end >= b.size()) throw Error("an unterminated instance name");
        p.name.emplace(b.begin() + at, b.begin() + end);
        p.name_end = end + 1;
    }
    return p;
}
Bytes Rewrite(const Tag& t, const Place& p, std::optional<std::uint16_t> depth, std::optional<std::uint16_t> character,
              const std::optional<Matrix>& matrix, const std::optional<std::string>& name) {
    const Bytes& b = t.body;
    Bytes out(b.begin(), b.begin() + p.character_at);
    if (depth) SetLe(out, t.code == kPlace3 ? 2 : 1, *depth, 2);
    if (p.character) PutLe(out, character.value_or(*p.character), 2);
    if (p.matrix) Append(out, matrix ? WriteMatrix(*matrix) : Bytes(b.begin() + p.matrix_at, b.begin() + p.matrix_end));
    out.insert(out.end(), b.begin() + p.matrix_end, b.begin() + p.name_at);
    if (p.name) {
        const std::string& n = name.value_or(*p.name);
        out.insert(out.end(), n.begin(), n.end());
        out.push_back(0);
    }
    out.insert(out.end(), b.begin() + p.name_end, b.end());
    return out;
}

struct Movie {
    Bytes header;
    std::vector<Tag> tags;
};
Movie ReadMovie(const Bytes& b) {
    if (!Has(b, 0, "GFX", 3) || b.size() < 9) throw Error("optionsetting.gfx is not an uncompressed GFX movie");
    const std::size_t start = 8 + (5 + 4 * (b[8] >> 3) + 7) / 8 + 4;
    if (start > b.size()) throw Error("optionsetting.gfx ends in its header");
    return Movie{Bytes(b.begin(), b.begin() + start), ReadTags(b, start, b.size())};
}
Bytes WriteMovie(const Movie& m) {
    Bytes out = m.header;
    for (const Tag& t : m.tags) WriteTag(out, t);
    SetLe(out, 4, out.size(), 4);
    return out;
}

struct Sprite {
    std::uint16_t id = 0, frames = 0;
    std::vector<Tag> tags;
};
Sprite ReadSprite(const Tag& t) { return Sprite{U16(t.body, 0), U16(t.body, 2), ReadTags(t.body, 4, t.body.size())}; }
Tag WriteSprite(const Sprite& s) {
    Bytes body;
    PutLe(body, s.id, 2);
    PutLe(body, s.frames, 2);
    for (const Tag& t : s.tags) WriteTag(body, t);
    return Changed(kSprite, std::move(body));
}

std::size_t FindSprite(const Movie& m, std::uint16_t id) {
    for (std::size_t i = 0; i < m.tags.size(); ++i)
        if (m.tags[i].code == kSprite && U16(m.tags[i].body, 0) == id) return i;
    throw Error("no sprite " + std::to_string(id) + " in the movie");
}
std::optional<std::size_t> Placed(const Movie& m, const std::string& name) {
    for (std::size_t i = 0; i < m.tags.size(); ++i)
        if (IsPlace(m.tags[i]) && ReadPlace(m.tags[i]).name == name) return i;
    return std::nullopt;
}
std::size_t FindPlaced(const Movie& m, const std::string& name) {
    if (const auto at = Placed(m, name)) return *at;
    throw Error("the movie places no " + name);
}
std::uint16_t HighestCharacter(const Movie& m) {
    std::uint16_t most = 0;
    for (const Tag& t : m.tags)
        if (kDefines.count(t.code) && t.body.size() >= 2) most = std::max(most, U16(t.body, 0));
    return most;
}
int RowIndex(const std::string& name) {
    if (name.size() < 8 || name.compare(0, 5, "Item_") || name.compare(name.size() - 2, 2, "_0")) return -1;
    const std::string n = name.substr(5, name.size() - 7);
    if (n.empty() || n.size() > 2 || !std::all_of(n.begin(), n.end(), ::isdigit)) return -1;
    return std::stoi(n);
}
std::string RowName(int index) { return "Item_" + std::to_string(index) + "_0"; }

struct Section {
    const char* name;
    std::int32_t title;
    int rows;
    bool defaults;
};
constexpr Section kSections[] = {
    {"PCGraphics", 117020, 6, true}, {"PCControls", 118020, 8, false}, {"PCKeys", 119020, 8, false},
    {"PCEffects", 120020, 5, true},  {"PCCamera", 121020, 3, true},    {"PCEnhance", 124020, 3, true},
    {"PCColour", 125020, 5, true},   {"PCLighting", 126020, 8, true},  {"PCDetail", 127020, 8, true},
    {"PCLooks", 128020, 1, false},
};
constexpr int kDonorRows = 6, kDonorDefaults = 6;
constexpr std::uint16_t kFirstSectionDepth = 383;

struct Donor {
    std::size_t tag = 0;
    Sprite sprite;
    Matrix placed;
    std::optional<std::string> class_name;
    std::uint16_t title = 0;
    Sprite title_sprite;
    std::string title_field;
    std::uint16_t row_depth[kDonorRows] = {}, defaults_depth = 0;
    std::int64_t pitch = 0;
    std::set<std::uint16_t> depths;
};

std::map<std::uint16_t, std::string> SymbolClasses(const Movie& m) {
    std::map<std::uint16_t, std::string> out;
    for (const Tag& t : m.tags) {
        if (t.code != kSymbolClass) continue;
        std::size_t at = 2;
        for (std::uint16_t n = U16(t.body, 0); n; --n) {
            const std::uint16_t id = U16(t.body, at);
            std::size_t end = at + 2;
            while (end < t.body.size() && t.body[end]) ++end;
            if (end >= t.body.size()) throw Error("an unterminated class name");
            out[id].assign(t.body.begin() + at + 2, t.body.begin() + end);
            at = end + 1;
        }
    }
    return out;
}

Donor ReadDonor(const Movie& m) {
    Donor d;
    const Place placed = ReadPlace(m.tags[FindPlaced(m, "ControllSetting")]);
    if (!placed.character || !placed.matrix) throw Error("ControllSetting is placed without a character or position");
    d.placed = *placed.matrix;
    d.tag = FindSprite(m, *placed.character);
    d.sprite = ReadSprite(m.tags[d.tag]);
    if (auto c = SymbolClasses(m); c.count(d.sprite.id)) d.class_name = c[d.sprite.id];
    bool rows[kDonorRows] = {}, defaults = false;
    std::int64_t row0 = 0, row1 = 0;
    for (const Tag& t : d.sprite.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        d.depths.insert(p.depth);
        if (p.name && p.matrix) {
            const int row = RowIndex(*p.name);
            if (row >= 0 && row < kDonorRows) {
                rows[row] = true;
                d.row_depth[row] = p.depth;
                if (row == 0) row0 = p.matrix->ty;
                if (row == 1) row1 = p.matrix->ty;
            } else if (row == kDonorDefaults) {
                defaults = true;
                d.defaults_depth = p.depth;
            }
        }
        if (p.character && !d.title) {
            for (std::size_t i = 0; i < m.tags.size(); ++i) {
                if (m.tags[i].code != kSprite || U16(m.tags[i].body, 0) != *p.character) continue;
                const Sprite child = ReadSprite(m.tags[i]);
                for (const Tag& c : child.tags) {
                    if (c.code != kPlace2) continue;
                    const Place field = ReadPlace(c);
                    if (field.name && field.name->rfind("StaticText_", 0) == 0) {
                        d.title = child.id;
                        d.title_sprite = child;
                        d.title_field = *field.name;
                    }
                }
            }
        }
    }
    if (!std::all_of(rows, rows + kDonorRows, [](bool r) { return r; }) || !defaults || !d.title)
        throw Error("ControllSetting is not laid out as 1.09's (six rows, Defaults, a title)");
    d.pitch = row1 - row0;
    return d;
}

Sprite SectionSprite(const Donor& d, const Section& s, std::uint16_t id, std::uint16_t title) {
    const int kept = std::min(s.rows, kDonorRows);
    std::set<std::uint16_t> dropped;
    for (int r = kept; r < kDonorRows; ++r) dropped.insert(d.row_depth[r]);
    if (!s.defaults) dropped.insert(d.defaults_depth);
    std::vector<std::uint16_t> added;
    for (std::uint16_t depth = d.row_depth[kDonorRows - 1] + 1; int(added.size()) < s.rows - kDonorRows; ++depth)
        if (!d.depths.count(depth)) added.push_back(depth);
    const std::int64_t defaults_dy = d.pitch * (s.rows - kDonorDefaults);

    Sprite out{id, d.sprite.frames, {}};
    for (const Tag& t : d.sprite.tags) {
        if (!IsPlace(t)) {
            out.tags.push_back(t);
            continue;
        }
        const Place p = ReadPlace(t);
        if (dropped.count(p.depth)) continue;
        if (p.character == d.title) {
            out.tags.push_back(Changed(t.code, Rewrite(t, p, {}, title, {}, {})));
        } else if (p.depth == d.defaults_depth && defaults_dy) {
            std::optional<Matrix> m = p.matrix;
            if (m) m->ty += defaults_dy;
            const auto name = p.name ? std::optional<std::string>(RowName(s.rows)) : std::nullopt;
            out.tags.push_back(Changed(t.code, Rewrite(t, p, {}, {}, m, name)));
        } else {
            out.tags.push_back(t);
        }
        if (p.depth == d.row_depth[kDonorRows - 1] && t.code == kPlace2) {
            for (std::size_t k = 0; k < added.size(); ++k) {
                std::optional<Matrix> m = p.matrix;
                if (m) m->ty += d.pitch * std::int64_t(k + 1);
                const auto name = p.name ? std::optional<std::string>(RowName(kDonorRows + int(k))) : std::nullopt;
                out.tags.push_back(Changed(kPlace2, Rewrite(t, p, added[k], {}, m, name)));
            }
        }
    }
    return out;
}

Sprite TitleSprite(const Donor& d, const Section& s, std::uint16_t id) {
    const std::string field = "StaticText_" + std::to_string(s.title);
    Sprite out{id, d.title_sprite.frames, {}};
    for (const Tag& t : d.title_sprite.tags) {
        if (t.code == kPlace2 && ReadPlace(t).name == d.title_field) {
            const Place p = ReadPlace(t);
            out.tags.push_back(Changed(kPlace2, Rewrite(t, p, {}, {}, {}, field)));
        } else {
            out.tags.push_back(t);
        }
    }
    return out;
}

Tag HiddenPlacement(std::uint16_t id, std::uint16_t depth, const Matrix& at, const std::string& name) {
    Bytes b{0x02 | 0x04 | 0x20, 0x20};
    PutLe(b, depth, 2);
    PutLe(b, id, 2);
    Append(b, WriteMatrix(at));
    b.insert(b.end(), name.begin(), name.end());
    b.push_back(0);
    b.push_back(0);
    return Changed(kPlace3, std::move(b));
}

void AddClasses(Movie& m, const std::vector<std::pair<std::uint16_t, std::string>>& classes) {
    for (Tag& t : m.tags) {
        if (t.code != kSymbolClass) continue;
        Bytes body = t.body;
        SetLe(body, 0, U16(body, 0) + classes.size(), 2);
        for (const auto& [id, name] : classes) {
            PutLe(body, id, 2);
            body.insert(body.end(), name.begin(), name.end());
            body.push_back(0);
        }
        t = Changed(kSymbolClass, std::move(body));
        return;
    }
    throw Error("the movie has no SymbolClass");
}

constexpr double kScrollX = 1290, kScrollY = 354.5;
void AddScrollBar(Movie& m) {
    const Place top = ReadPlace(m.tags[FindPlaced(m, "Top")]);
    if (!top.character) throw Error("Top is placed without a character");
    std::optional<std::uint16_t> bar;
    for (const Tag& t : m.tags) {
        if (t.code != kSprite || bar) continue;
        for (const Tag& c : ReadSprite(t).tags)
            if (IsPlace(c) && !bar) {
                const Place p = ReadPlace(c);
                if (p.name == "ScrollBarV" && p.character) bar = p.character;
            }
    }
    if (!bar) throw Error("the movie has no ScrollBarV to place");
    Matrix list = top.matrix.value_or(Matrix{});
    if (list.rotated) throw Error("Top is rotated");
    const double sx = list.scaled ? Fixed(list.sx, list.scale_bits) : 1, sy = list.scaled ? Fixed(list.sy, list.scale_bits) : 1;
    const std::size_t at = FindSprite(m, *top.character);
    Sprite s = ReadSprite(m.tags[at]);
    std::uint16_t depth = 0;
    for (const Tag& t : s.tags) {
        if (!IsPlace(t)) continue;
        const Place p = ReadPlace(t);
        if (p.name == "ScrollBarV") throw Error("Top has a ScrollBarV already");
        depth = std::max(depth, p.depth);
    }
    const auto first_frame = std::find_if(s.tags.begin(), s.tags.end(), [](const Tag& t) { return t.code == kShowFrame; });
    if (first_frame == s.tags.end()) throw Error("Top has no frame");
    Matrix place;
    place.tx = std::llrint((kScrollX * 20 - double(list.tx)) / sx);
    place.ty = std::llrint((kScrollY * 20 - double(list.ty)) / sy);
    Bytes b{0x02 | 0x04 | 0x20};
    PutLe(b, depth + 1, 2);
    PutLe(b, *bar, 2);
    Append(b, WriteMatrix(place));
    const std::string name = "ScrollBarV";
    b.insert(b.end(), name.begin(), name.end());
    b.push_back(0);
    s.tags.insert(first_frame, Changed(kPlace2, std::move(b)));
    m.tags[at] = WriteSprite(s);
}

const char kTsv[] =
#include "pc_option_messages.inc"
    ;

constexpr std::int32_t kHostRole = 107000, kGuestRole = 107100;
constexpr std::uint32_t kMenuGroup = 200, kHelpGroup = 201, kQuestionGroup = 78, kScreenQuestionGroup = 204, kEventGroup = 30;

std::string Unescape(std::string s) {
    for (std::size_t at = 0; (at = s.find("\\n", at)) != std::string::npos; ++at) s.replace(at, 2, "\n");
    return s;
}
}

std::u16string Utf16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto lead = static_cast<unsigned char>(s[i++]);
        const int more = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : lead >= 0xc0 ? 1 : 0;
        std::uint32_t c = lead & (0x7f >> more);
        for (int k = 0; k < more && i < s.size(); ++k) c = c << 6 | (static_cast<unsigned char>(s[i++]) & 0x3f);
        if (c >= 0x10000) {
            out.push_back(char16_t(0xd800 + ((c - 0x10000) >> 10)));
            out.push_back(char16_t(0xdc00 + ((c - 0x10000) & 0x3ff)));
        } else {
            out.push_back(char16_t(c));
        }
    }
    return out;
}

Kind KindOf(std::int32_t id) {
    if (id >= 920000 && id <= 929999) return Kind::Question;
    if (id >= 14000332 && id <= 14000399) return Kind::Event;
    return Kind::Menu;
}

Language LanguageOf(const std::string& folder) {
    return folder == "zhocn" ? Language::Chinese : folder == "rusru" ? Language::Russian : Language::English;
}

std::map<std::int32_t, PcMessage> PcMessages(bool chinese) {
    return PcMessages(chinese ? Language::Chinese : Language::English);
}

std::map<std::int32_t, PcMessage> PcMessages(Language language) {
    // Columns: 0 id, 1-2 English text and help, 3-4 Simplified Chinese, 5-6 Russian.
    const std::size_t translated_text = language == Language::Chinese ? 3 : language == Language::Russian ? 5 : 1;
    std::map<std::int32_t, PcMessage> out;
    const std::string all(kTsv);
    for (std::size_t at = 0; at < all.size();) {
        std::size_t end = all.find('\n', at);
        if (end == std::string::npos) end = all.size();
        std::string line = all.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        std::vector<std::string> column;
        for (std::size_t from = 0;;) {
            const std::size_t tab = line.find('\t', from);
            column.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
            if (tab == std::string::npos) break;
            from = tab + 1;
        }
        if (column.size() < 2) throw Error("pc_option_messages.tsv: a line without text: " + line);
        auto pick = [&](std::size_t english, std::size_t translated) {
            const std::size_t c = translated != english && column.size() > translated && !column[translated].empty() ? translated : english;
            return c < column.size() ? Unescape(column[c]) : std::string();
        };
        const std::int32_t id = std::stoi(column[0]);
        if (out.count(id)) throw Error("pc_option_messages.tsv: id " + column[0] + " twice");
        out[id] = PcMessage{pick(1, translated_text), pick(2, translated_text + 1)};
    }
    return out;
}

Dcx DcxRead(const Bytes& b) {
    if (!Has(b, 0, "DCX\0", 4) || !Has(b, 0x18, "DCS\0", 4) || !Has(b, 0x24, "DCP\0DFLT", 8) || !Has(b, 0x44, "DCA\0", 4))
        throw Error("not a DFLT DCX");
    const std::size_t start = 0x44 + std::size_t(Be32(b, 0x48));
    const std::size_t size = Be32(b, 0x1c), packed = Be32(b, 0x20);
    if (start > b.size() || packed > b.size() - start) throw Error("a DCX shorter than its header says");
    if (size > kMostBytes) throw Error("a DCX of more than 64 MiB");
    Dcx d{Bytes(b.begin(), b.begin() + start), Bytes(size)};
    z_stream z{};
    if (inflateInit(&z) != Z_OK) throw Error("zlib");
    z.next_in = const_cast<Bytef*>(b.data() + start);
    z.avail_in = uInt(b.size() - start);
    z.next_out = d.data.data();
    z.avail_out = uInt(size);
    const int r = inflate(&z, Z_FINISH);
    const bool whole = r == Z_STREAM_END && z.avail_out == 0;
    inflateEnd(&z);
    if (!whole) throw Error("a DCX whose data does not inflate to its size");
    return d;
}

Bytes DcxWrite(const Dcx& d) {
    uLongf packed = compressBound(uLong(d.data.size()));
    Bytes z(packed);
    if (compress2(z.data(), &packed, d.data.data(), uLong(d.data.size()), 9) != Z_OK) throw Error("zlib");
    Bytes out = d.header;
    SetBe32(out, 0x1c, std::uint32_t(d.data.size()));
    SetBe32(out, 0x20, std::uint32_t(packed));
    out.insert(out.end(), z.begin(), z.begin() + packed);
    return out;
}

Binder BinderRead(const Bytes& b) {
    if (!Has(b, 0, "BND4", 4) || b.size() < 0x40) throw Error("not a BND4");
    Binder r;
    r.header.assign(b.begin(), b.begin() + 0x40);
    r.entry_bytes = std::size_t(U64(b, 0x20));
    r.wide_names = b[0x30] != 0;
    if (r.entry_bytes < 0x24 || r.entry_bytes > 0x100 || U64(b, 0x38)) throw Error("a BND4 laid out unlike the game's");
    const std::uint32_t count = U32(b, 0x0c);
    if (count > 4096) throw Error("a BND4 of too many files");
    for (std::uint32_t k = 0; k < count; ++k) {
        const std::size_t e = 0x40 + k * r.entry_bytes;
        BinderFile f;
        f.flags = U64(b, e);
        const std::uint64_t size = U64(b, e + 0x08), at = U32(b, e + 0x18);
        f.id = U32(b, e + 0x1c);
        const std::size_t name = U32(b, e + 0x20);
        if (at > b.size() || size > b.size() - at || name >= b.size()) throw Error("a BND4 entry past the end");
        std::size_t end = name;
        if (r.wide_names) end = WideEnd(b, name);
        else
            while (end < b.size() && b[end]) ++end;
        f.name.assign(b.begin() + name, b.begin() + end);
        f.data.assign(b.begin() + at, b.begin() + at + size);
        r.files.push_back(std::move(f));
    }
    return r;
}

Bytes BinderWrite(const Binder& r) {
    Bytes out = r.header;
    SetLe(out, 0x0c, r.files.size(), 4);
    const std::size_t names = 0x40 + r.files.size() * r.entry_bytes;
    out.resize(names, 0);
    std::vector<std::size_t> name_at;
    for (const BinderFile& f : r.files) {
        name_at.push_back(out.size());
        Append(out, f.name);
        out.resize(out.size() + (r.wide_names ? 2 : 1), 0);
    }
    SetLe(out, 0x28, out.size(), 8);
    for (std::size_t k = 0; k < r.files.size(); ++k) {
        const BinderFile& f = r.files[k];
        out.resize((out.size() + 15) & ~std::size_t(15), 0);
        const std::size_t e = 0x40 + k * r.entry_bytes;
        SetLe(out, e, f.flags, 8);
        SetLe(out, e + 0x08, f.data.size(), 8);
        SetLe(out, e + 0x10, f.data.size(), 8);
        SetLe(out, e + 0x18, out.size(), 4);
        SetLe(out, e + 0x1c, f.id, 4);
        SetLe(out, e + 0x20, name_at[k], 4);
        Append(out, f.data);
    }
    return out;
}

Fmg FmgRead(const Bytes& b) {
    if (b.size() < 0x28 || b[2] != 2) throw Error("an FMG that is not version 2");
    Fmg f;
    f.header.assign(b.begin(), b.begin() + 0x28);
    const std::uint32_t groups = U32(b, 0x0c), strings = U32(b, 0x10);
    const std::uint64_t offsets = U64(b, 0x18);
    for (std::uint32_t g = 0; g < groups; ++g) {
        const std::size_t at = 0x28 + std::size_t(g) * 16;
        const std::uint32_t index = U32(b, at);
        const auto first = std::int32_t(U32(b, at + 4)), last = std::int32_t(U32(b, at + 8));
        if (last < first || std::uint64_t(index) + std::uint64_t(last - first) >= strings) throw Error("an FMG group past its strings");
        for (std::int32_t id = first; id <= last; ++id) {
            const std::uint64_t text = U64(b, offsets + 8 * (index + std::uint64_t(id - first)));
            if (!text) {
                f.text[id] = std::nullopt;
                continue;
            }
            if (text >= b.size()) throw Error("an FMG string past the end");
            const std::size_t end = WideEnd(b, text);
            std::u16string s((end - text) / 2, u'\0');
            for (std::size_t i = 0; i < s.size(); ++i) s[i] = char16_t(U16(b, text + 2 * i));
            f.text[id] = std::move(s);
        }
    }
    return f;
}

Bytes FmgWrite(const Fmg& f) {
    struct Run {
        std::uint32_t index;
        std::int32_t first, last;
    };
    std::vector<Run> runs;
    std::uint32_t index = 0;
    for (const auto& [id, text] : f.text) {
        if (runs.empty() || runs.back().last + 1 != id) runs.push_back({index, id, id});
        else runs.back().last = id;
        ++index;
    }
    const std::size_t offsets = 0x28 + runs.size() * 16, pool = offsets + f.text.size() * 8;
    Bytes out = f.header;
    out.resize(pool, 0);
    std::map<std::u16string, std::size_t> placed;
    std::size_t k = 0;
    for (const auto& [id, text] : f.text) {
        std::size_t at = 0;
        if (text) {
            auto [it, fresh] = placed.emplace(*text, out.size());
            if (fresh) {
                for (const char16_t c : *text) PutLe(out, c, 2);
                PutLe(out, 0, 2);
            }
            at = it->second;
        }
        SetLe(out, offsets + 8 * k++, at, 8);
    }
    for (std::size_t g = 0; g < runs.size(); ++g) {
        SetLe(out, 0x28 + g * 16, runs[g].index, 4);
        SetLe(out, 0x28 + g * 16 + 4, std::uint32_t(runs[g].first), 4);
        SetLe(out, 0x28 + g * 16 + 8, std::uint32_t(runs[g].last), 4);
        SetLe(out, 0x28 + g * 16 + 12, 0, 4);
    }
    SetLe(out, 0x04, out.size(), 4);
    SetLe(out, 0x0c, runs.size(), 4);
    SetLe(out, 0x10, f.text.size(), 4);
    SetLe(out, 0x18, offsets, 8);
    return out;
}

Bytes MenuMessages(const Bytes& file, bool chinese) {
    return MenuMessages(file, chinese ? Language::Chinese : Language::English);
}

Bytes MenuMessages(const Bytes& file, Language language, const PlateTitles& titles) {
    const bool packed = Has(file, 0, "DCX\0", 4);
    Dcx dcx = packed ? DcxRead(file) : Dcx{{}, file};
    Binder bundle = BinderRead(dcx.data);
    const auto messages = PcMessages(language);
    struct Group {
        std::uint32_t id;
        Kind kind;
        bool help;
        bool found = false;
    };
    Group groups[] = {{kMenuGroup, Kind::Menu, false}, {kHelpGroup, Kind::Menu, true}, {kQuestionGroup, Kind::Question, false},
                      {kScreenQuestionGroup, Kind::Question, false}, {kEventGroup, Kind::Event, false}};
    for (BinderFile& file_in : bundle.files) {
        Group* g = nullptr;
        for (Group& candidate : groups)
            if (candidate.id == file_in.id) g = &candidate;
        if (!g) continue;
        Fmg fmg = FmgRead(file_in.data);
        for (const auto& [id, m] : messages) {
            if (KindOf(id) != g->kind) continue;
            if (fmg.text.count(id))
                throw Error("message " + std::to_string(id) + " is already in group " + std::to_string(g->id));
            const std::string& s = g->help ? m.help : m.text;
            fmg.text[id] = s.empty() ? std::nullopt : std::optional<std::u16string>(Utf16(s));
        }
        if (g->kind == Kind::Event) {
            if (titles.host && fmg.text.count(kHostRole)) fmg.text[kHostRole] = Utf16(*titles.host);
            if (titles.guest && fmg.text.count(kGuestRole)) fmg.text[kGuestRole] = Utf16(*titles.guest);
        }
        file_in.data = FmgWrite(fmg);
        g->found = true;
    }
    for (const Group& g : groups)
        if (!g.found) throw Error("the message bundle has no group " + std::to_string(g.id));
    dcx.data = BinderWrite(bundle);
    return packed ? DcxWrite(dcx) : dcx.data;
}

Bytes OptionMovie(const Bytes& gfx) {
    Movie m = ReadMovie(gfx);
    for (const Section& s : kSections)
        if (Placed(m, s.name)) throw Error(std::string("the movie already has ") + s.name);
    const Donor d = ReadDonor(m);
    std::uint16_t next = HighestCharacter(m);
    std::vector<Tag> sprites, placements;
    std::vector<std::pair<std::uint16_t, std::string>> classes;
    std::uint16_t depth = kFirstSectionDepth;
    for (const Section& s : kSections) {
        if (std::to_string(s.title).size() + 11 != d.title_field.size() || s.rows < 1 || s.rows > 8)
            throw Error(std::string("section ") + s.name + " does not fit the donor");
        const std::uint16_t id = ++next, title = ++next;
        sprites.push_back(WriteSprite(TitleSprite(d, s, title)));
        sprites.push_back(WriteSprite(SectionSprite(d, s, id, title)));
        placements.push_back(HiddenPlacement(id, depth++, d.placed, s.name));
        if (d.class_name) classes.emplace_back(id, *d.class_name);
    }
    if (depth > 500) throw Error("the sections' depths reach Top's");
    m.tags.insert(m.tags.begin() + std::ptrdiff_t(d.tag) + 1, sprites.begin(), sprites.end());
    const std::size_t top = FindPlaced(m, "Top");
    m.tags.insert(m.tags.begin() + std::ptrdiff_t(top) + 1, placements.begin(), placements.end());
    if (!classes.empty()) AddClasses(m, classes);
    AddScrollBar(m);
    return WriteMovie(m);
}

Bytes AltarScripts(const Bytes& file) {
    Dcx dcx = DcxRead(file);
    Binder scripts = BinderRead(dcx.data);
    bool found = false;
    for (BinderFile& f : scripts.files) {
        std::string name;
        for (std::size_t i = 0; i < f.name.size(); i += scripts.wide_names ? 2 : 1)
            name.push_back(scripts.wide_names && i + 1 < f.name.size() && f.name[i + 1] ? '?' : char(f.name[i]));
        if (name.size() < 11 || name.compare(name.size() - 11, 11, "t242307.esd")) continue;
        std::string why;
        if (!rebirth_script(f.data, &why)) throw Error(why);
        found = true;
    }
    if (!found) throw Error("the altar's scripts have no t242307.esd");
    dcx.data = BinderWrite(scripts);
    return DcxWrite(dcx);
}
}
