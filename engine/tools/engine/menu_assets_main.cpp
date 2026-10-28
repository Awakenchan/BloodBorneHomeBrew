// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_assets.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::vector<fs::path> args;
#ifdef _WIN32
    (void)argc, (void)argv;
    int count = 0;
    wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!wide) return 2;
    for (int i = 0; i < count; ++i) args.emplace_back(wide[i]);
    LocalFree(wide);
#else
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    if (args.size() != 4 || (args[1] != "movie" && args[1] != "messages" && args[1] != "rebirth")) {
        std::fputs("Usage: bb-engine-assets movie|messages|rebirth SOURCE DEST\n", stderr);
        return 2;
    }
    std::error_code same;
    if (fs::equivalent(args[2], args[3], same)) {
        std::fputs("Engine menu assets: the destination is the source\n", stderr);
        return 1;
    }
    try {
        std::ifstream in(args[2], std::ios::binary | std::ios::ate);
        const auto size = in ? std::streamoff(in.tellg()) : -1;
        if (size < 0 || std::size_t(size) > menu_assets::kMostBytes) throw menu_assets::Error("the source is missing or too large");
        menu_assets::Bytes source(static_cast<std::size_t>(size));
        in.seekg(0);
        if (!in.read(reinterpret_cast<char*>(source.data()), size)) throw menu_assets::Error("the source cannot be read");
        const menu_assets::Bytes made =
            args[1] == "movie"     ? menu_assets::OptionMovie(source)
            : args[1] == "rebirth" ? menu_assets::AltarScripts(source)
                                   : menu_assets::MenuMessages(source, menu_assets::LanguageOf(args[2].parent_path().filename().string()));
        std::ofstream out(args[3], std::ios::binary | std::ios::trunc);
        if (!out.write(reinterpret_cast<const char*>(made.data()), std::streamsize(made.size())) || !out.flush())
            throw menu_assets::Error("the destination cannot be written");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Engine menu assets: %s\n", e.what());
    }
    return 1;
}
