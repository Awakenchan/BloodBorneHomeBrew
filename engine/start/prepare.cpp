// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare.h"
#include "elf.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_map>

namespace start {
namespace {

constexpr std::uint64_t kLimit = 512ull << 20;
const char* const kSupportedTitle = "CUSA03173";
const char* const kSupportedVersion = "01.09";
const char* const kSupportedImage = "071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a";
constexpr std::uint64_t kChunk = 1 << 16;
constexpr std::size_t kPatchedChunks = 32;

bool IsLoad(const Ph& p) {
    return p.type == 1 || p.type == 0x61000010;
}

std::optional<std::size_t> DifferingChunks(const Bytes& image, const fs::path& engine) {
    std::vector<std::string> expected;
    try {
        for (const std::string& line : Split(ReadText(engine / "data" / "image_109_hashes.txt"), '\n')) {
            const std::string word = Trim(line);
            if (word.size() == 16 && word[0] != '#') expected.push_back(word);
        }
    } catch (const Error&) {
        return std::nullopt;
    }
    if ((image.size() + kChunk - 1) / kChunk != expected.size()) return std::nullopt;
    std::size_t differing = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::size_t at = i * kChunk, n = std::min<std::size_t>(kChunk, image.size() - at);
        if (Sha256Hex(image.data() + at, n).substr(0, 16) != expected[i]) ++differing;
    }
    return differing;
}

bool checked_code_109 = false, game_check_skipped = false;

void CheckGame(const fs::path& game, const Bytes& image, const fs::path& engine) {
    checked_code_109 = false;
    game_check_skipped = Env("BB_SKIP_GAME_CHECK", "") == "1";
    if (game_check_skipped) return;
    std::string title = "?", version = "?";
    try {
        const auto sfo = ParseSfo(ReadFile(game / "sce_sys" / "param.sfo"));
        title = SfoText(sfo, "TITLE_ID");
        version = SfoText(sfo, "APP_VER");
    } catch (const Error&) {
    }
    if (Sha256Hex(image) == kSupportedImage) {
        checked_code_109 = true;
        return;
    }
    if (const auto n = DifferingChunks(image, engine); n && *n <= kPatchedChunks) {
        checked_code_109 = true;
        Log("Game check: eboot.bin is 1.09 with %zu of %zu 64 KiB pieces changed (a patched eboot, e.g. a 60 FPS "
            "package); starting anyway. If the game crashes, use the clean eboot.bin from the dumped 1.09 update.",
            *n, std::size_t((image.size() + kChunk - 1) / kChunk));
        return;
    }
    const std::string found = "Found " + title + " version " + version + " (sce_sys/param.sfo).";
    if (title != kSupportedTitle) {
        Log("Game check: %s Works best with Bloodborne CUSA03173 (update 1.09): this edition's executable differs, "
            "so it starts without the FPS, resolution and engine patches made for that one (30 FPS, the game's own "
            "settings). Expect missing features; crashes here are not supported yet.",
            found.c_str());
        return;
    }
    if (version != kSupportedVersion)
        throw GameCheckError(found + " Lantern Engine needs the 1.09 update merged into the game folder: copy everything "
                                     "from the dumped 1.09 update (e.g. CUSA03173-patch) into the game folder, "
                                     "replacing files (eboot.bin and sce_sys too).");
    throw GameCheckError(found + " param.sfo is from 1.09 but eboot.bin is not: copy eboot.bin from the dumped "
                                 "1.09 update into the game folder, replacing the old one.");
}

Executable ReadExecutable(const fs::path& path, const char* shown) {
    const Bytes data = ReadFile(path);
    try {
        if (data.size() < 64) throw Error(data.empty() ? "it is empty (0 bytes)" : "it is too small");
        return ParseSelf(data);
    } catch (const GameCheckError&) {
        throw;
    } catch (const Error& e) {
        throw GameCheckError(std::string(shown) + " is damaged (" + e.what() +
                             "). Copy it again from your game dump or the dumped 1.09 update (both if unsure), "
                             "and check that your antivirus has not quarantined or cut it.");
    }
}

struct Identity {
    std::string nid;
    std::string library;
    std::uint16_t library_version = 0;
    std::string module;
    std::uint16_t module_version = 0;
    bool operator==(const Identity&) const = default;
};
struct IdentityHash {
    std::size_t operator()(const Identity& id) const {
        return std::hash<std::string>{}(id.nid + '#' + id.library + '#' + id.module) ^
               (std::size_t(id.library_version) << 16 | id.module_version);
    }
};

struct Symbol {
    std::string name;
    std::uint8_t type, binding;
    std::uint16_t section;
    std::uint64_t value, size;
    std::optional<Identity> identity;
};
struct Relocation {
    std::uint64_t target;
    std::uint32_t kind, symbol;
    std::int64_t addend;
};

struct Module {
    Executable exe;
    std::map<std::uint64_t, std::uint64_t> tags;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dynamic;
    std::vector<Symbol> symbols;
    std::vector<Relocation> relocs;
    std::map<std::string, std::pair<std::string, std::uint16_t>> libraries, modules;
    Bytes blob;
    std::size_t strings_at = 0, strings_size = 0;

    std::string String(std::uint64_t offset) const {
        if (offset >= strings_size) throw Error("string out of bounds");
        const auto* begin = blob.data() + strings_at + offset;
        const auto* end = blob.data() + strings_at + strings_size;
        const auto* nul = std::find(begin, end, 0);
        if (nul == end) throw Error("unterminated string");
        return std::string(begin, nul);
    }
    std::optional<Identity> IdentityOf(const std::string& name) const {
        const auto parts = Split(name, '#');
        if (parts.size() != 3) return std::nullopt;
        const auto lib = libraries.find(parts[1]);
        const auto mod = modules.find(parts[2]);
        if (lib == libraries.end() || mod == modules.end()) throw Error("unknown library or module in " + name);
        return Identity{parts[0], lib->second.first, lib->second.second, mod->second.first, mod->second.second};
    }
    const Ph* Find(std::uint32_t type) const {
        for (const Ph& p : exe.ph)
            if (p.type == type) return &p;
        return nullptr;
    }
};

std::string EncodeId(std::uint64_t value) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string out(1, alphabet[value & 63]);
    value >>= 6;
    while (value) {
        out.insert(out.begin(), alphabet[value & 63]);
        value >>= 6;
    }
    return out;
}

Module ReadModule(Executable exe, bool stop_at_null) {
    Module m;
    m.exe = std::move(exe);
    const Ph* dp = m.Find(2);
    if (!dp) throw Error("no dynamic segment");
    for (std::uint64_t pos = dp->offset; pos < dp->offset + dp->filesz; pos += 16) {
        const std::uint8_t* e = Span(m.exe.elf, pos, 16);
        const std::uint64_t tag = Load<std::uint64_t>(e), value = Load<std::uint64_t>(e + 8);
        if (stop_at_null && !tag) break;
        m.dynamic.emplace_back(tag, value);
        m.tags[tag] = value;
    }
    const Ph* lp = m.Find(0x61000000);
    if (!lp) throw Error("no dynamic library data");
    const std::uint8_t* blob = Span(m.exe.elf, lp->offset, lp->filesz);
    m.blob.assign(blob, blob + lp->filesz);
    const auto tag = [&](std::uint64_t t) {
        const auto it = m.tags.find(t);
        if (it == m.tags.end()) throw Error("missing dynamic tag " + Hex(t));
        return it->second;
    };
    m.strings_at = tag(0x61000035);
    m.strings_size = tag(0x61000037);
    Span(m.blob, m.strings_at, m.strings_size);
    for (const auto& [t, value] : m.dynamic) {
        if (t == 0x61000013 || t == 0x61000015 || t == 0x6100000d || t == 0x6100000f) {
            auto& table = (t == 0x61000013 || t == 0x61000015) ? m.libraries : m.modules;
            const std::string key = EncodeId(value >> 48);
            std::pair<std::string, std::uint16_t> identity{m.String(value & 0xffffffff), std::uint16_t(value >> 32)};
            const auto it = table.find(key);
            if (it != table.end() && it->second != identity) throw Error("conflicting module/library IDs");
            table[key] = identity;
        }
    }
    const std::uint64_t syms_at = tag(0x61000039), syms_size = tag(0x6100003f);
    const std::uint8_t* syms = Span(m.blob, syms_at, syms_size);
    for (std::uint64_t pos = 0; pos + 24 <= syms_size; pos += 24) {
        const std::uint8_t* s = syms + pos;
        Symbol symbol;
        symbol.name = m.String(Load<std::uint32_t>(s));
        symbol.type = s[4] & 15;
        symbol.binding = s[4] >> 4;
        symbol.section = Load<std::uint16_t>(s + 6);
        symbol.value = Load<std::uint64_t>(s + 8);
        symbol.size = Load<std::uint64_t>(s + 16);
        if (!symbol.name.empty()) symbol.identity = m.IdentityOf(symbol.name);
        m.symbols.push_back(std::move(symbol));
    }
    for (const auto& [offset_tag, size_tag] : {std::pair{0x61000029ull, 0x6100002dull}, std::pair{0x6100002full, 0x61000031ull}}) {
        const std::uint64_t at = tag(offset_tag), size = tag(size_tag);
        const std::uint8_t* table = Span(m.blob, at, size);
        for (std::uint64_t pos = 0; pos + 24 <= size; pos += 24) {
            const std::uint64_t info = Load<std::uint64_t>(table + pos + 8);
            m.relocs.push_back({Load<std::uint64_t>(table + pos), std::uint32_t(info), std::uint32_t(info >> 32),
                                Load<std::int64_t>(table + pos + 16)});
        }
    }
    return m;
}

struct Segment {
    std::uint64_t address, size, flags;
};
struct Reloc {
    std::uint64_t target, kind;
    std::int64_t value, addend;
};

struct Boot {
    Bytes image;
    std::uint64_t entry = 0, flags = 0;
    std::vector<Segment> segments;
    std::vector<std::string> names;
    std::vector<Reloc> relocs;
};

std::string Nid(const std::string& name) {
    static const std::uint8_t salt[] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
                                        0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
    std::string input = name;
    input.append(reinterpret_cast<const char*>(salt), sizeof salt);
    const Bytes digest = Sha1(input.data(), input.size());
    std::uint8_t eight[8];
    for (int i = 0; i < 8; ++i) eight[i] = digest[7 - i];
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::uint32_t bits = 0;
    int count = 0;
    for (std::uint8_t byte : eight) {
        bits = bits << 8 | byte;
        count += 8;
        while (count >= 6) {
            out += b64[(bits >> (count - 6)) & 63];
            count -= 6;
        }
    }
    if (count) out += b64[(bits << (6 - count)) & 63];
    std::replace(out.begin(), out.end(), '/', '-');
    return out;
}

bool InitEnvIsRet(const fs::path& libc) {
    Module m = ReadModule(ReadExecutable(libc, "sce_module/libc.prx"), false);
    const std::string init_env = Nid("_init_env");
    bool is_ret = false;
    for (const Symbol& s : m.symbols) {
        if (Split(s.name, '#')[0] != init_env || !s.section || s.type != 2) continue;
        const Ph* p = nullptr;
        for (const Ph& q : m.exe.ph)
            if (q.type == 1 && (q.flags & 1) && q.vaddr <= s.value && s.value < q.vaddr + q.filesz) {
                p = &q;
                break;
            }
        if (!p) throw Error("libc _init_env outside its code");
        const std::uint8_t* code = Span(m.exe.elf, p->offset + s.value - p->vaddr, s.size);
        is_ret = s.size == 1 && code[0] == 0xc3;
    }
    return is_ret;
}

Boot PrepareBoot(const fs::path& game, Module& main, const fs::path& engine) {
    Boot boot;
    std::vector<const Ph*> loads;
    for (const Ph& p : main.exe.ph)
        if (IsLoad(p)) loads.push_back(&p);
    const auto mapped = [&](std::uint64_t address, std::uint64_t size = 8) {
        return std::any_of(loads.begin(), loads.end(),
                           [&](const Ph* p) { return p->vaddr <= address && address + size <= p->vaddr + p->memsz; });
    };
    std::uint64_t size = 0;
    for (const Ph* p : loads) size = std::max(size, p->vaddr + p->memsz);
    if (size > kLimit || !mapped(main.exe.entry, 1)) throw Error("invalid memory image");
    boot.image.assign(size, 0);
    for (const Ph* p : loads) {
        if (p->filesz > p->memsz) throw Error("segment file size exceeds memory size");
        const std::uint8_t* data = Span(main.exe.elf, p->offset, p->filesz);
        std::copy_n(data, p->filesz, boot.image.begin() + std::ptrdiff_t(p->vaddr));
    }
    CheckGame(game, boot.image, engine);
    boot.entry = main.exe.entry;
    for (const Ph* p : loads) boot.segments.push_back({p->vaddr, std::min((p->memsz + 0x3fffu) & ~std::uint64_t(0x3fff), size - p->vaddr), p->flags});
    std::map<std::uint32_t, std::size_t> imports;
    std::vector<std::uint32_t> import_order;
    for (const Relocation& r : main.relocs) {
        if (!mapped(r.target)) throw Error("relocation outside mapped segments: " + Hex(r.target));
        if (r.kind == 8) {
            boot.relocs.push_back({r.target, 0, r.addend, 0});
        } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
            const Symbol& symbol = main.symbols.at(r.symbol);
            if (symbol.section) {
                if (symbol.section == 0xfff1)
                    Store<std::uint64_t>(boot.image.data() + r.target, symbol.value + std::uint64_t(r.addend));
                else
                    boot.relocs.push_back({r.target, 0, std::int64_t(symbol.value) + r.addend, 0});
            } else {
                if (!imports.count(r.symbol)) {
                    imports[r.symbol] = import_order.size();
                    import_order.push_back(r.symbol);
                }
                const std::uint64_t kind = symbol.type == 1 ? 2 : 1;
                if ((kind == 1 && r.addend) || r.addend < 0 || r.addend >= 4096) throw Error("unsupported import addend");
                boot.relocs.push_back({r.target, kind, std::int64_t(imports[r.symbol]), r.addend});
            }
        } else {
            throw Error("unsupported relocation " + std::to_string(r.kind));
        }
    }
    for (const std::uint32_t index : import_order) boot.names.push_back(main.symbols[index].name);
    boot.flags = InitEnvIsRet(game / "sce_module" / "libc.prx") ? 1 : 0;
    std::string title = "?";
    try {
        title = SfoText(ParseSfo(ReadFile(game / "sce_sys" / "param.sfo")), "TITLE", "None");
    } catch (const Error&) {
    }
    std::size_t total_relocs = main.relocs.size(), needed = 0;
    for (const auto& [t, v] : main.dynamic) needed += t == 1;
    Log("%s | entry=%s | image=%s bytes", title.c_str(), Hex(boot.entry).c_str(),
        [&] {
            std::string n = std::to_string(size);
            for (int i = int(n.size()) - 3; i > 0; i -= 3) n.insert(std::size_t(i), ",");
            return n;
        }().c_str());
    Log("%zu imported symbols; %zu relocations; %zu required modules", boot.names.size(), total_relocs, needed);
    Log("libc _init_env verified RET: %s", boot.flags ? "True" : "False");
    return boot;
}

const Bytes kFsLoad = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00};
const Bytes kLeaDown = {0x48, 0x8d, 0x64, 0x24, 0x80};
const Bytes kLeaUp = {0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00};

int PatchFsLoads(Bytes& image, const std::vector<Ph>& ph, std::uint64_t base) {
    int patched = 0;
    for (const Ph& p : ph) {
        if (p.type != 1 || !(p.flags & 1)) continue;
        const std::uint64_t start = base + p.vaddr, end = std::min<std::uint64_t>(base + p.vaddr + p.filesz, image.size());
        auto from = image.begin() + std::ptrdiff_t(start);
        const auto stop = image.begin() + std::ptrdiff_t(end);
        while (from < stop) {
            const auto at = std::search(from, stop, kFsLoad.begin(), kFsLoad.end());
            if (at == stop) break;
            *at = 0x65;
            ++patched;
            from = at + std::ptrdiff_t(kFsLoad.size());
        }
    }
    return patched;
}

Bytes ProtectRedZones(Bytes& image, const nlohmann::json& sites, std::uint64_t base,
                      const std::vector<std::uint64_t>& skip, int& applied) {
    Bytes code;
    for (const auto& site : sites) {
        const std::uint64_t offset = site.at(0).get<std::uint64_t>();
        const Bytes original = FromHex(site.at(1).get<std::string>());
        const std::uint64_t end = offset + original.size();
        const auto next_reloc = std::upper_bound(skip.begin(), skip.end(), offset - 8);
        if (end > image.size() || !std::equal(original.begin(), original.end(), image.begin() + std::ptrdiff_t(offset)) ||
            (next_reloc != skip.end() && *next_reloc < end))
            continue;
        const std::uint64_t here = base + code.size();
        std::uint64_t pos = offset;
        for (const auto& length_json : site.at(2)) {
            const std::int64_t length = length_json.get<std::int64_t>();
            const std::uint64_t n = std::uint64_t(length < 0 ? -length : length);
            const auto body = image.begin() + std::ptrdiff_t(pos);
            if (length < 0) code.insert(code.end(), kLeaDown.begin(), kLeaDown.end());
            code.insert(code.end(), body, body + std::ptrdiff_t(n));
            if (length < 0) code.insert(code.end(), kLeaUp.begin(), kLeaUp.end());
            pos += n;
        }
        code.push_back(0xe9);
        Append<std::int32_t>(code, std::int32_t(std::int64_t(end) - std::int64_t(base + code.size() + 4)));
        while (code.size() % 16) code.push_back(0xcc);
        image[offset] = 0xe9;
        Store<std::int32_t>(image.data() + offset + 1, std::int32_t(std::int64_t(here) - std::int64_t(offset + 5)));
        std::fill(image.begin() + std::ptrdiff_t(offset + 5), image.begin() + std::ptrdiff_t(end), 0xcc);
        ++applied;
    }
    return code;
}

Bytes RewriteRedZoneFunctions(Bytes& image, const nlohmann::json& functions, std::uint64_t base,
                              const std::vector<std::uint64_t>& skip, int& applied) {
    Bytes code;
    for (const auto& f : functions) {
        const std::uint64_t start = f.at("offset").get<std::uint64_t>();
        const std::uint64_t size = f.at("size").get<std::uint64_t>();
        const std::uint64_t end = start + size;
        const auto next_reloc = std::upper_bound(skip.begin(), skip.end(), start - 8);
        if (size < 5 || end > image.size() || Sha256Hex(image.data() + start, size) != f.at("sha256").get<std::string>() ||
            (next_reloc != skip.end() && *next_reloc < end))
            continue;
        const std::uint64_t here = base + code.size();
        Bytes body;
        std::unordered_map<std::uint64_t, std::size_t> at;
        std::vector<std::pair<std::size_t, std::uint64_t>> jumps;
        std::uint64_t pos = 0;
        bool bad = false;
        for (const auto& item : f.at("insns")) {
            const std::uint64_t n = item.at(0).get<std::uint64_t>();
            const int kind = item.size() > 1 ? item.at(1).get<int>() : 0;
            if (pos + n > size) {
                bad = true;
                break;
            }
            Bytes insn(image.begin() + std::ptrdiff_t(start + pos), image.begin() + std::ptrdiff_t(start + pos + n));
            at[pos] = body.size();
            if (kind == 1) {
                const std::size_t modrm = item.at(2).get<std::size_t>(), disp = item.at(3).get<std::size_t>();
                const int width = item.at(4).get<int>();
                const std::int32_t old = width == 0 ? 0
                                         : width == 1 ? std::int32_t(std::int8_t(insn[disp]))
                                                      : Load<std::int32_t>(insn.data() + disp);
                const std::int32_t value = old + 0x80;
                if (width == 1 && value >= -128 && value <= 127) {
                    insn[disp] = std::uint8_t(value);
                } else if (width == 4) {
                    Store<std::int32_t>(insn.data() + disp, value);
                } else {
                    insn[modrm] = std::uint8_t((insn[modrm] & 0x3f) | 0x80);
                    Bytes disp32;
                    Append<std::int32_t>(disp32, value);
                    insn.erase(insn.begin() + std::ptrdiff_t(disp), insn.begin() + std::ptrdiff_t(disp + std::size_t(width)));
                    insn.insert(insn.begin() + std::ptrdiff_t(disp), disp32.begin(), disp32.end());
                }
                body.insert(body.end(), insn.begin(), insn.end());
            } else if (kind == 2) {
                const std::size_t disp = item.at(2).get<std::size_t>();
                const std::int64_t target = std::int64_t(start + pos + n) + Load<std::int32_t>(insn.data() + disp);
                Store<std::int32_t>(insn.data() + disp, std::int32_t(target - std::int64_t(here + body.size() + n)));
                body.insert(body.end(), insn.begin(), insn.end());
            } else if (kind == 3) {
                const int cc = item.at(2).get<int>();
                if (cc == 16) {
                    body.push_back(0xe9);
                } else {
                    body.push_back(0x0f);
                    body.push_back(std::uint8_t(0x80 + cc));
                }
                jumps.push_back({body.size(), item.at(3).get<std::uint64_t>()});
                body.insert(body.end(), 4, 0);
            } else if (kind == 4) {
                body.insert(body.end(), insn.begin(), insn.end());
                body.insert(body.end(), kLeaDown.begin(), kLeaDown.end());
            } else if (kind == 5) {
                body.insert(body.end(), kLeaUp.begin(), kLeaUp.end());
                body.insert(body.end(), insn.begin(), insn.end());
            } else if (kind == 0) {
                body.insert(body.end(), insn.begin(), insn.end());
            } else {
                bad = true;
                break;
            }
            pos += n;
        }
        for (const auto& jump : jumps) bad = bad || !at.count(jump.second);
        if (bad || pos != size) {
            Log("Red zone: the recipe for %s does not fit; left as it is", Hex(start).c_str());
            continue;
        }
        for (const auto& [where, target] : jumps)
            Store<std::int32_t>(body.data() + where, std::int32_t(std::int64_t(at[target]) - std::int64_t(where + 4)));
        code.insert(code.end(), body.begin(), body.end());
        while (code.size() % 16) code.push_back(0xcc);
        image[start] = 0xe9;
        Store<std::int32_t>(image.data() + start + 1, std::int32_t(std::int64_t(here) - std::int64_t(start + 5)));
        ++applied;
    }
    return code;
}

struct Export {
    std::uint64_t address, kind;
    std::string file;
};

void LinkModules(const fs::path& game, const fs::path& out, Module& main, Boot boot, const fs::path& engine) {
    Bytes& image = boot.image;
    const std::uint64_t size = image.size();
    std::vector<std::optional<Identity>> order;
    std::vector<std::size_t> index_of;
    std::unordered_map<Identity, std::size_t, IdentityHash> position;
    std::optional<std::size_t> none_position;
    const auto set_identity = [&](const std::optional<Identity>& id, std::size_t index) {
        if (!id) {
            if (none_position) {
                index_of[*none_position] = index;
            } else {
                none_position = order.size();
                order.push_back(id);
                index_of.push_back(index);
            }
            return;
        }
        const auto it = position.find(*id);
        if (it != position.end()) {
            index_of[it->second] = index;
        } else {
            position[*id] = order.size();
            order.push_back(id);
            index_of.push_back(index);
        }
    };
    for (std::size_t i = 0; i < boot.names.size(); ++i) set_identity(main.IdentityOf(boot.names[i]), i);
    std::map<std::pair<std::string, std::uint16_t>, std::string> main_libraries, main_modules;
    for (const auto& [key, id] : main.libraries) main_libraries[id] = key;
    for (const auto& [key, id] : main.modules) main_modules[id] = key;
    const auto imported = [&](const Symbol& symbol) -> std::size_t {
        if (!symbol.identity) throw Error("import without identity: " + symbol.name);
        const Identity& id = *symbol.identity;
        const auto it = position.find(id);
        if (it != position.end()) return index_of[it->second];
        const auto lib = main_libraries.find({id.library, id.library_version});
        const auto mod = main_modules.find({id.module, id.module_version});
        const std::string name = lib != main_libraries.end() && mod != main_modules.end()
                                     ? id.nid + "#" + lib->second + "#" + mod->second
                                     : id.nid + "#" + id.library;
        const std::size_t index = boot.names.size();
        set_identity(id, index);
        boot.names.push_back(name);
        return index;
    };

    std::unordered_map<Identity, Export, IdentityHash> exports;
    std::map<std::pair<std::string, std::string>, std::vector<Export>> by_nid;
    struct Entry {
        std::string file;
        std::uint64_t base, size, init, tls_address, tls_memsz, tls_filesz, tls_module;
    };
    std::vector<Entry> table;
    std::uint64_t base = (size + 65535) & ~65535ull;
    int fs_patched = PatchFsLoads(image, main.exe.ph, 0);
    std::uint64_t tls_module = 2;
    for (const char* filename : {"libc.prx", "libSceFios2.prx"}) {
        Module m = ReadModule(ReadExecutable(game / "sce_module" / filename, (std::string("sce_module/") + filename).c_str()), true);
        std::vector<const Ph*> loads;
        for (const Ph& p : m.exe.ph)
            if (IsLoad(p)) loads.push_back(&p);
        std::uint64_t modsize = 0;
        for (const Ph* p : loads) modsize = std::max(modsize, p->vaddr + p->memsz);
        if (base + modsize > kLimit) throw Error("linked image exceeds probe limit");
        if (image.size() < base + modsize) image.resize(base + modsize, 0);
        const auto mapped = [&](std::uint64_t address, std::uint64_t length = 8) {
            return std::any_of(loads.begin(), loads.end(), [&](const Ph* p) {
                return p->vaddr <= address && address + length <= p->vaddr + p->memsz;
            });
        };
        for (const Ph* p : loads) {
            if (p->filesz > p->memsz) throw Error(std::string(filename) + ": invalid segment size");
            std::copy_n(Span(m.exe.elf, p->offset, p->filesz), p->filesz, image.begin() + std::ptrdiff_t(base + p->vaddr));
            boot.segments.push_back({base + p->vaddr, p->memsz, p->flags});
        }
        const Ph* tls = m.Find(7);
        std::uint64_t module_id = 0;
        if (tls) {
            if (tls->filesz > tls->memsz || !mapped(tls->vaddr, tls->memsz))
                throw Error(std::string(filename) + ": unsupported TLS layout");
            module_id = tls_module++;
        }
        for (const Relocation& r : m.relocs) {
            if (!mapped(r.target)) throw Error(std::string(filename) + ": relocation outside image " + Hex(r.target));
            const std::uint64_t target = r.target + base;
            if (r.kind == 8) {
                boot.relocs.push_back({target, 0, std::int64_t(base) + r.addend, 0});
            } else if (r.kind == 16) {
                const Symbol& symbol = m.symbols.at(r.symbol);
                if (!symbol.name.empty() || symbol.type != 3 || r.addend || !module_id)
                    throw Error(std::string(filename) + ": unsupported external TLS relocation");
                Store<std::uint64_t>(image.data() + target, module_id);
            } else if (r.kind == 1 || r.kind == 6 || r.kind == 7) {
                const Symbol& symbol = m.symbols.at(r.symbol);
                if (symbol.section) {
                    if (symbol.section == 0xfff1)
                        Store<std::uint64_t>(image.data() + target, symbol.value + std::uint64_t(r.addend));
                    else
                        boot.relocs.push_back({target, 0, std::int64_t(base + symbol.value) + r.addend, 0});
                } else if (!symbol.identity && symbol.binding == 2) {
                    Store<std::uint64_t>(image.data() + target, 0);
                } else {
                    const std::uint64_t kind = symbol.type == 1 ? 2 : 1;
                    if ((kind == 1 && r.addend) || r.addend < 0 || r.addend >= 4096)
                        throw Error(std::string(filename) + ": unsupported import addend");
                    boot.relocs.push_back({target, kind, std::int64_t(imported(symbol)), r.addend});
                }
            } else {
                throw Error(std::string(filename) + ": unsupported relocation type " + std::to_string(r.kind));
            }
        }
        for (const Symbol& s : m.symbols) {
            if (!s.section || (s.binding != 1 && s.binding != 2) || !s.identity || (s.type != 1 && s.type != 2)) continue;
            if (!mapped(s.value, std::max<std::uint64_t>(1, s.size)))
                throw Error(std::string(filename) + ": export outside load segments");
            if (exports.count(*s.identity)) throw Error(std::string(filename) + ": ambiguous export");
            const Export entry{base + s.value, std::uint64_t(s.type == 1 ? 2 : 1), filename};
            exports[*s.identity] = entry;
            by_nid[{s.identity->nid, s.identity->library}].push_back(entry);
        }
        fs_patched += PatchFsLoads(image, m.exe.ph, base);
        const auto init = m.tags.find(12);
        table.push_back({filename, base, modsize, base + (init == m.tags.end() ? 0 : init->second),
                         tls ? base + tls->vaddr : 0, tls ? tls->memsz : 0, tls ? tls->filesz : 0, module_id});
        base = (base + modsize + 65535) & ~65535ull;
    }

    int red_zone_sites = 0;
    const fs::path sites_path = engine / "patches" / "redzone.json";
    const fs::path functions_path = engine / "patches" / "redzone_functions.json";
    std::error_code error;
    std::vector<std::uint64_t> skip;
    for (const Reloc& r : boot.relocs) skip.push_back(r.target);
    std::sort(skip.begin(), skip.end());
    Bytes code;
    if (Env("BB_RED_ZONE_FUNCTIONS", "") == "1" && fs::is_regular_file(functions_path, error)) {
        const auto functions = nlohmann::json::parse(ReadText(functions_path)).at("functions");
        int moved = 0;
        code = RewriteRedZoneFunctions(image, functions, base, skip, moved);
        Log("Red zone: %d of %zu functions run with their locals above rsp (BB_RED_ZONE_FUNCTIONS=1)", moved,
            functions.size());
    }
    if (fs::is_regular_file(sites_path, error)) {
        const auto sites = nlohmann::json::parse(ReadText(sites_path)).at("sites");
        const Bytes more = ProtectRedZones(image, sites, base + code.size(), skip, red_zone_sites);
        code.insert(code.end(), more.begin(), more.end());
    }
    if (!code.empty()) {
        image.resize(base, 0);
        image.insert(image.end(), code.begin(), code.end());
        boot.segments.push_back({base, code.size(), 5});
    }

    std::vector<std::array<std::uint64_t, 3>> bindings;
    std::size_t unresolved = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const auto& id = order[i];
        const Export* found = nullptr;
        if (id) {
            const auto it = exports.find(*id);
            if (it != exports.end()) found = &it->second;
            if (!found && id->library == "libSceLibcInternal") {
                const auto candidates = by_nid.find({id->nid, "libc"});
                if (candidates != by_nid.end() && candidates->second.size() == 1) found = &candidates->second[0];
            }
        }
        if (found) bindings.push_back({index_of[i], found->address, found->kind});
        else ++unresolved;
    }

    const Ph* main_tls = main.Find(7);
    const std::array<std::uint64_t, 4> main_tls_values =
        main_tls ? std::array<std::uint64_t, 4>{main_tls->vaddr, main_tls->filesz, main_tls->memsz, main_tls->align}
                 : std::array<std::uint64_t, 4>{0, 0, 0, 0};
    if (main_tls && (main_tls->filesz > main_tls->memsz || main_tls->memsz > (1u << 20) ||
                     main_tls->vaddr + main_tls->filesz > size))
        throw Error("unsupported eboot TLS layout");
    const Ph* procparam = main.Find(0x61000001);
    if (!procparam) throw Error("no process parameters");

    Bytes linked;
    linked.reserve(image.size() + boot.relocs.size() * 32 + boot.names.size() * 128 + 4096);
    Append(linked, "BBPROBE5", 8);
    for (std::uint64_t v : {std::uint64_t(image.size()), boot.entry, std::uint64_t(boot.segments.size()),
                            std::uint64_t(boot.relocs.size()), std::uint64_t(boot.names.size()), boot.flags})
        Append(linked, v);
    Append(linked, procparam->vaddr);
    for (std::uint64_t v : main_tls_values) Append(linked, v);
    Append<std::uint64_t>(linked, table.size());
    for (const Entry& t : table)
        for (std::uint64_t v : {t.base, t.size, t.init, t.tls_address, t.tls_memsz, t.tls_filesz, t.tls_module})
            Append(linked, v);
    Append<std::uint64_t>(linked, bindings.size());
    for (const auto& b : bindings)
        for (std::uint64_t v : b) Append(linked, v);
    for (const Segment& s : boot.segments) {
        Append(linked, s.address);
        Append(linked, s.size);
        Append(linked, s.flags);
    }
    for (const std::string& name : boot.names) {
        if (name.size() >= 128) throw Error("name too long");
        char padded[128] = {};
        std::memcpy(padded, name.data(), name.size());
        Append(linked, padded, sizeof padded);
    }
    for (const Reloc& r : boot.relocs) {
        Append(linked, r.target);
        Append(linked, r.kind);
        Append(linked, r.value);
        Append(linked, r.addend);
    }
    Append(linked, image.data(), image.size());
    WriteFile(out / "boot-linked.bin", linked);
    std::string summary;
    for (const Entry& t : table) summary += (summary.empty() ? "" : ", ") + t.file + "@" + Hex(t.base);
    Log("Linked modules: %s; %zu native bindings, %zu imports left to the host runtime, fs->gs patched=%d, "
        "red-zone-safe stores at %d sites",
        summary.c_str(), bindings.size(), unresolved, fs_patched, red_zone_sites);
}

void ContentProfile(const fs::path& game, const fs::path& out, const std::string& sku) {
    const auto sfo = ParseSfo(ReadFile(game / "sce_sys" / "param.sfo"));
    std::uint32_t params[4] = {};
    for (int i = 0; i < 4; ++i) {
        const auto it = sfo.find("USER_DEFINED_PARAM_" + std::to_string(i + 1));
        if (it == sfo.end()) continue;
        const auto* value = std::get_if<std::uint32_t>(&it->second);
        if (!value) throw Error("invalid user-defined parameter");
        params[i] = *value;
    }
    Bytes data;
    Append(data, "BBCONT01", 8);
    Append<std::uint32_t>(data, sku == "trial" ? 1 : 3);
    for (std::uint32_t v : params) Append(data, v);
    WriteFile(out / "content.bin", data);
    Log("AppContent profile: SKU=%s (probe setting), user params=[%u, %u, %u, %u], mounted add-ons=0", sku.c_str(),
        params[0], params[1], params[2], params[3]);
}

const char* const kPrepared[] = {"boot-linked.bin", "eboot.elf", "content.bin", "game-code.txt"};

std::string PrepareKey(const fs::path& game, const std::string& sku, const fs::path& engine) {
    Sha256Stream digest;
    std::vector<fs::path> paths{game / "eboot.bin", game / "sce_sys" / "param.sfo"};
    std::error_code error;
    std::vector<fs::path> modules;
    if (fs::is_directory(game / "sce_module", error))
        for (const auto& entry : fs::directory_iterator(game / "sce_module", error)) modules.push_back(entry.path());
    std::sort(modules.begin(), modules.end());
    paths.insert(paths.end(), modules.begin(), modules.end());
    wchar_t self[MAX_PATH * 4];
    paths.push_back(fs::path(std::wstring(self, GetModuleFileNameW(nullptr, self, DWORD(std::size(self))))));
    paths.push_back(engine / "data" / "image_109_hashes.txt");
    paths.push_back(engine / "patches" / "redzone.json");
    paths.push_back(engine / "patches" / "redzone_functions.json");
    for (const fs::path& path : paths) {
        const std::string name = Utf8(path.filename());
        digest.Update(name);
        digest.Update("\0", 1);
        if (fs::is_regular_file(path, error)) {
            const Bytes data = ReadFile(path);
            digest.Update(data.data(), data.size());
        }
        digest.Update("\0", 1);
    }
    digest.Update("sku=" + sku + ";check=" + Env("BB_SKIP_GAME_CHECK", "") +
                  ";redzone_functions=" + Env("BB_RED_ZONE_FUNCTIONS", ""));
    return digest.HexDigest();
}

}

void PrepareImage(const fs::path& game, const fs::path& out, const fs::path& engine) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    const std::string sku = Env("BB_CONTENT_SKU", "full");
    const fs::path stamp = out / "prepared.json";
    const std::string key = PrepareKey(game, sku, engine);
    std::error_code error;
    bool reusable = false;
    try {
        reusable = nlohmann::json::parse(ReadText(stamp)).value("key", "") == key;
    } catch (const std::exception&) {
    }
    if (reusable && std::all_of(std::begin(kPrepared), std::end(kPrepared),
                                [&](const char* name) { return fs::is_regular_file(out / name, error); })) {
        Log("Boot image unchanged: reused (%.1f s)", elapsed());
        return;
    }
    fs::remove(stamp, error);
    fs::create_directories(out);
    Module main = ReadModule(ReadExecutable(game / "eboot.bin", "eboot.bin"), true);
    WriteFile(out / "eboot.elf", main.exe.elf);
    Boot boot = PrepareBoot(game, main, engine);
    WriteText(out / "game-code.txt", checked_code_109 ? "1.09\n" : game_check_skipped ? "unchecked\n" : "other\n");
    LinkModules(game, out, main, std::move(boot), engine);
    ContentProfile(game, out, sku);
    WriteText(stamp, nlohmann::json{{"key", key}}.dump() + "\n");
    Log("Boot image prepared in %.1f s (reused on later launches)", elapsed());
}

}
