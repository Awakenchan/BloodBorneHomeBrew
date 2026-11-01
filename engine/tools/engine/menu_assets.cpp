// SPDX-License-Identifier: GPL-2.0-or-later
#include "menu_assets.h"
#include "rebirth_script.h"
#include <zlib.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <tuple>

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
    // A class name only with HasClassName. The SWF format also has one with HasImage and
    // HasCharacter, but the game's movies set HasImage on placements of a character with no name
    // after it (nowloading.gfx's spinner): read so, every placement after it was misplaced.
    if (t.code == kPlace3 && (more & 0x08)) {
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

// Widescreen (WideMovie): a placement's matrix as numbers, x' = a*x + c*y + tx, y' = b*x + d*y + ty
// in twips, as the SWF matrix's ScaleX, RotateSkew0, RotateSkew1, ScaleY and translation.
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};
Affine Of(const Matrix& m) {
    Affine f;
    if (m.scaled) f.a = Fixed(m.sx, m.scale_bits), f.d = Fixed(m.sy, m.scale_bits);
    if (m.rotated) f.b = Fixed(m.r0, m.rotate_bits), f.c = Fixed(m.r1, m.rotate_bits);
    f.tx = double(m.tx), f.ty = double(m.ty);
    return f;
}
// `inner` applied first, then `outer`.
Affine Then(const Affine& outer, const Affine& inner) {
    return {outer.a * inner.a + outer.c * inner.b, outer.b * inner.a + outer.d * inner.b,
            outer.a * inner.c + outer.c * inner.d, outer.b * inner.c + outer.d * inner.d,
            outer.a * inner.tx + outer.c * inner.ty + outer.tx, outer.b * inner.tx + outer.d * inner.ty + outer.ty};
}
Matrix ToMatrix(const Affine& f) {
    const auto fixed = [](double v) { return std::int64_t(std::llround(v * 65536.0)); };
    const auto bits = [](std::int64_t v, int n) { return std::uint32_t(std::uint64_t(v) & ((std::uint64_t(1) << n) - 1)); };
    Matrix m;
    const std::int64_t sx = fixed(f.a), sy = fixed(f.d), r0 = fixed(f.b), r1 = fixed(f.c);
    if (sx != 65536 || sy != 65536) {
        m.scaled = true;
        m.scale_bits = std::max(BitsFor(sx), BitsFor(sy));
        m.sx = bits(sx, m.scale_bits), m.sy = bits(sy, m.scale_bits);
    }
    if (r0 || r1) {
        m.rotated = true;
        m.rotate_bits = std::max(BitsFor(r0), BitsFor(r1));
        m.r0 = bits(r0, m.rotate_bits), m.r1 = bits(r1, m.rotate_bits);
    }
    m.tx = std::llround(f.tx), m.ty = std::llround(f.ty);
    return m;
}

// A shape's bounds (twips) and whether it fills anything; per bitmap fill, how many twips one
// texel spans across and down (20: one stage pixel).
struct ShapeInfo {
    double x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    bool filled = false;
    std::vector<std::pair<double, double>> texels;
};
std::optional<ShapeInfo> ReadShape(const Tag& t) {
    if (t.code != 2 && t.code != 22 && t.code != 32 && t.code != 83) return std::nullopt;
    const Bytes& b = t.body;
    ShapeInfo s;
    try {
        BitReader r{b, 2};
        const int n = int(r.Unsigned(5));
        s.x0 = double(r.Signed(n)), s.x1 = double(r.Signed(n)), s.y0 = double(r.Signed(n)), s.y1 = double(r.Signed(n));
        std::size_t at = r.End();
        if (t.code == 83) {
            BitReader edges{b, at};
            const int e = int(edges.Unsigned(5));
            edges.Unsigned(4 * e);
            at = edges.End() + 1;
        }
        const bool rgba = t.code >= 32;
        std::size_t count = Le(b, at++, 1);
        if (count == 0xff && t.code != 2) count = U16(b, at), at += 2;
        for (; count; --count) {
            const std::uint8_t type = std::uint8_t(Le(b, at++, 1));
            if (type == 0x00) {
                at += rgba ? 4 : 3;
            } else if (type == 0x10 || type == 0x12 || type == 0x13) {
                BitReader m{b, at};
                ReadMatrix(m);
                at = m.End();
                at += (Le(b, at, 1) & 15) * (rgba ? 5 : 4) + 1 + (type == 0x13 ? 2 : 0);
            } else if (type >= 0x40 && type <= 0x43) {
                BitReader m{b, at + 2};
                const Affine f = Of(ReadMatrix(m));
                at = m.End();
                s.texels.emplace_back(std::hypot(f.a, f.b), std::hypot(f.c, f.d));
            } else {
                return std::nullopt;
            }
            s.filled = true;
        }
    } catch (const Error&) {
        return std::nullopt;
    }
    return s;
}

// Widescreen: the movie's full-screen shapes (a dim, a fade) are drawn for the 1920x1080 stage, and
// Scaleform shows the stage whole and centred in the wider UI viewport: they stopped at the 16:9
// edges. Each colour or soft, magnified texture (the death screen's glow) placed over the whole
// stage is stretched about its centre until it covers what the viewport shows of the movie. A
// picture (a bitmap shown at less than two pixels a texel) is scaled the same both ways, cropped top
// and bottom rather than stretched, except on the title screen, whose art keeps its shape with
// black beside it. The scale goes into every placement that puts the shape on
// its timeline (initial and animated), one per depth, the largest any position over the stage
// needs: a zoom over the stage keeps its motion.
struct Widening {
    using Key = std::tuple<std::uint16_t, std::uint16_t, std::uint16_t>;  // container, depth, shape
    struct Need {
        double kx = 1, ky = 1;
        bool picture = false;
        // A band: over the stage's whole width (or a strip over its whole height at its side),
        // widened across only; a strip about its inner edge (this x, in the shape's twips).
        bool band = false;
        std::optional<double> anchor_x;
    };
    std::map<std::uint16_t, ShapeInfo> shapes;
    std::map<std::uint16_t, Sprite> sprites;
    double vx0 = 0, vx1 = 0, vy0 = 0, vy1 = 0;
    bool wider = false, taller = false;  // the viewport shows more than the stage across, down
    std::map<Key, Need> needs;
    std::set<std::uint16_t> left_pictures;  // bands of a picture, left as they are
    // A placement is written once in its sprite's timeline, and every instance of the sprite has
    // it: one also seen drawn small (under half the stage both ways: a panel's background) is left
    // as it is, or that panel would stretch past its frame.
    std::set<Key> drawn_small;
    // Each sprite is walked under at most kPositions of the different matrices its timeline
    // places it with (an animated parent places it with a new one every frame, and nested ones
    // multiply): enough to see where it rests and how far it moves.
    static constexpr std::size_t kPositions = 8;
    // Over the whole stage (or its whole width or height), give or take 4 pixels: some are authored
    // a pixel short.
    static constexpr double kSlack = 4 * 20;
    std::set<std::tuple<std::uint16_t, std::int64_t, std::int64_t, std::int64_t, std::int64_t>> walked;
    std::map<std::uint16_t, std::size_t> positions;
    std::size_t visits = 0;

    void Consider(std::uint16_t container, std::uint16_t depth, std::uint16_t id, const Affine& m) {
        const ShapeInfo& s = shapes.at(id);
        if (!s.filled || std::abs(m.b) > 1e-4 || std::abs(m.c) > 1e-4 || std::abs(m.a) < 1e-6 || std::abs(m.d) < 1e-6) return;
        const double xa = m.a * s.x0 + m.tx, xb = m.a * s.x1 + m.tx, ya = m.d * s.y0 + m.ty, yb = m.d * s.y1 + m.ty;
        const double x0 = std::min(xa, xb), x1 = std::max(xa, xb), y0 = std::min(ya, yb), y1 = std::max(ya, yb);
        const bool across = x0 <= kSlack && x1 >= 1920 * 20 - kSlack, down = y0 <= kSlack && y1 >= 1080 * 20 - kSlack;
        if (x1 - x0 < 960 * 20 && y1 - y0 < 540 * 20) drawn_small.insert({container, depth, id});
        if (!across || !down) return Band(container, depth, id, m, x0, x1, across, down);
        const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        if (cx <= vx0 || cx >= vx1 || cy <= vy0 || cy >= vy1) return;
        Need& n = needs[{container, depth, id}];
        // Only across a side the viewport shows past: one authored a pixel short of the stage's edge
        // stays as it is where the stage's edge is the screen's.
        if (wider) n.kx = std::max({n.kx, (cx - vx0) / (cx - x0), (vx1 - cx) / (x1 - cx)});
        if (taller) n.ky = std::max({n.ky, (cy - vy0) / (cy - y0), (vy1 - cy) / (y1 - cy)});
        for (const auto& [across, down] : s.texels)
            n.picture |= across * std::abs(m.a) < 2 * 20 && down * std::abs(m.d) < 2 * 20;
    }

    // A band over the stage's whole width and part of its height (the dialogue subtitles' dark
    // band, an area name's): stretched across about its own centre to the viewport's width, its
    // height and place kept. A strip over the stage's whole height at its left or right side:
    // stretched outwards from its inner edge to the screen's. A centred column (a framed panel)
    // stays. Only colours, gradients and soft, magnified textures: a picture would be stretched.
    void Band(std::uint16_t container, std::uint16_t depth, std::uint16_t id, const Affine& m, double x0, double x1,
              bool across, bool down) {
        if (!wider || (!across && !down)) return;
        const bool left = x0 <= kSlack, right = x1 >= 1920 * 20 - kSlack;
        if (!across && !left && !right) return;
        const ShapeInfo& s = shapes.at(id);
        for (const auto& [texel_x, texel_y] : s.texels)
            if (texel_x * std::abs(m.a) < 2 * 20 || texel_y * std::abs(m.d) < 2 * 20) {
                left_pictures.insert(id);
                return;
            }
        Need& n = needs[{container, depth, id}];
        n.band = true;
        if (across) {
            const double cx = (x0 + x1) / 2;
            if (cx > vx0 && cx < vx1) n.kx = std::max({n.kx, (cx - vx0) / (cx - x0), (vx1 - cx) / (x1 - cx)});
            return;
        }
        // The inner edge in the shape's own twips (its matrix may mirror it).
        const double inner = left ? x1 : x0;
        if (!n.anchor_x) n.anchor_x = (inner - m.tx) / m.a;
        n.kx = std::max(n.kx, left ? (x1 - vx0) / (x1 - x0) : (vx1 - x0) / (x1 - x0));
    }

    // The placements of a timeline in order: each with the character it places (a move without one
    // keeps the depth's), its matrix in the parent (a move without one keeps the depth's), and
    // whether it only moves the character already there. Walk and Apply both read them so.
    template <typename Tags, typename Visit>
    static void Placements(Tags& tags, Visit&& visit) {
        std::map<std::uint16_t, std::pair<std::uint16_t, Affine>> at;
        for (auto& t : tags) {
            if (t.code == 5 || t.code == 28) {
                at.erase(U16(t.body, t.code == 5 ? 2 : 0));
                continue;
            }
            if (!IsPlace(t)) continue;
            const Place p = ReadPlace(t);
            const auto was = at.find(p.depth);
            if ((!p.character && !p.matrix) || (!p.character && was == at.end())) continue;
            const std::uint16_t id = p.character ? *p.character : was->second.first;
            const bool kept = !p.matrix && (p.flags & 1) && was != at.end();
            const Affine local = p.matrix ? Of(*p.matrix) : kept ? was->second.second : Affine{};
            const bool same = kept && was->second.first == id;
            at[p.depth] = {id, local};
            visit(t, p, id, local, same);
        }
    }

    void Walk(std::uint16_t container, const std::vector<Tag>& tags, const Affine& parent, int level) {
        if (level > 16 || ++visits > 100000) return;
        Placements(tags, [&](const Tag&, const Place& p, std::uint16_t id, const Affine& local, bool) {
            const Affine m = Then(parent, local);
            if (shapes.count(id)) {
                Consider(container, p.depth, id, m);
            } else if (const auto s = sprites.find(id); s != sprites.end()) {
                const auto round = [](double v) { return std::int64_t(std::llround(v * 64)); };
                if (positions[id] < kPositions &&
                    walked.emplace(id, round(m.a), round(m.d), std::llround(m.tx), std::llround(m.ty)).second) {
                    ++positions[id];
                    Walk(id, s->second.tags, m, level + 1);
                }
            }
        });
    }

    // The placements of the shapes found, scaled. True when a tag changed.
    bool Apply(std::uint16_t container, std::vector<Tag>& tags) {
        bool changed = false;
        Placements(tags, [&](Tag& t, const Place& p, std::uint16_t id, const Affine& local, bool same) {
            const auto need = needs.find({container, p.depth, id});
            if (need == needs.end() || same) return;
            double kx = need->second.kx, ky = need->second.ky;
            if (need->second.picture) kx = ky = std::max(kx, ky);
            if (kx < 1.0005 && ky < 1.0005) return;
            const ShapeInfo& s = shapes.at(id);
            const double lx = need->second.anchor_x.value_or((s.x0 + s.x1) / 2), ly = (s.y0 + s.y1) / 2;
            const Matrix scaled = ToMatrix(Then(local, Affine{kx, 0, 0, ky, lx * (1 - kx), ly * (1 - ky)}));
            Bytes body(t.body.begin(), t.body.begin() + std::ptrdiff_t(p.matrix_at));
            body[0] |= 0x04;
            Append(body, WriteMatrix(scaled));
            body.insert(body.end(), t.body.begin() + std::ptrdiff_t(p.matrix_end), t.body.end());
            t = Changed(t.code, std::move(body));
            changed = true;
        });
        return changed;
    }
};

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

Bytes WideMovie(const Bytes& gfx, int width, int height, std::string& notes, bool pictures) {
    if (!Has(gfx, 0, "GFX", 3)) throw Error("not an uncompressed GFX movie");
    Movie m = ReadMovie(gfx);
    Widening w;
    // What the viewport shows of the movie, in twips: the stage shown whole and centred, scaled by
    // the smaller of the two ratios (ui_composition.h StageScale), plus a pixel for filtering on a
    // side that shows more than the stage (none on one that shows the stage's edge: at 16:9 nothing
    // changes).
    const double scale = std::min(width / 1920.0, height / 1080.0);
    const double shown_x = width / scale / 2, shown_y = height / scale / 2;
    w.wider = shown_x > 960.5, w.taller = shown_y > 540.5;
    const double half_x = w.wider ? shown_x + 1 : 960, half_y = w.taller ? shown_y + 1 : 540;
    w.vx0 = (960 - half_x) * 20, w.vx1 = (960 + half_x) * 20, w.vy0 = (540 - half_y) * 20, w.vy1 = (540 + half_y) * 20;
    for (const Tag& t : m.tags) {
        if (const auto s = ReadShape(t)) w.shapes[U16(t.body, 0)] = *s;
        else if (t.code == kSprite) w.sprites[U16(t.body, 0)] = ReadSprite(t);
    }
    w.Walk(0, m.tags, Affine{}, 0);
    for (auto need = w.needs.begin(); need != w.needs.end();) {
        if (pictures || !need->second.picture || (need->second.kx < 1.0005 && need->second.ky < 1.0005)) {
            ++need;
            continue;
        }
        const auto [container, depth, id] = need->first;
        notes += (notes.empty() ? "" : ", ") + std::string("shape ") + std::to_string(id) + " (picture) left as it is" +
                 (container ? " in sprite " + std::to_string(container) : std::string(" on the root")) + " depth " +
                 std::to_string(depth);
        need = w.needs.erase(need);
    }
    for (const auto& key : w.drawn_small) {
        const auto need = w.needs.find(key);
        if (need == w.needs.end() || (need->second.kx < 1.0005 && need->second.ky < 1.0005)) continue;
        const auto [container, depth, id] = key;
        notes += (notes.empty() ? "" : ", ") + std::string("shape ") + std::to_string(id) + " left as it is (also drawn small)" +
                 (container ? " in sprite " + std::to_string(container) : std::string(" on the root")) + " depth " +
                 std::to_string(depth);
        w.needs.erase(need);
    }
    for (const auto& [key, need] : w.needs) {
        const auto [container, depth, id] = key;
        if (need.kx < 1.0005 && need.ky < 1.0005) continue;
        char line[160];
        if (need.picture)
            std::snprintf(line, sizeof line, "shape %u (picture) x%.3f", unsigned(id), std::max(need.kx, need.ky));
        else if (need.band)
            std::snprintf(line, sizeof line, "shape %u (%s %s) x%.3f across", unsigned(id),
                          w.shapes[id].texels.empty() ? "colour" : "soft texture",
                          need.anchor_x ? "strip at a side" : "band", need.kx);
        else
            std::snprintf(line, sizeof line, "shape %u (%s) x%.3f by x%.3f", unsigned(id),
                          w.shapes[id].texels.empty() ? "colour" : "soft texture", need.kx, need.ky);
        notes += (notes.empty() ? "" : ", ") + std::string(line) +
                 (container ? " in sprite " + std::to_string(container) : std::string(" on the root")) + " depth " +
                 std::to_string(depth);
    }
    for (const std::uint16_t id : w.left_pictures)
        notes += (notes.empty() ? "" : ", ") + std::string("shape ") + std::to_string(id) +
                 " (a band of a picture) left as it is";
    bool changed = w.Apply(0, m.tags);
    for (Tag& t : m.tags) {
        if (t.code != kSprite) continue;
        Sprite s = w.sprites.at(U16(t.body, 0));
        if (w.Apply(s.id, s.tags)) t = WriteSprite(s), changed = true;
    }
    return changed ? WriteMovie(m) : Bytes{};
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
