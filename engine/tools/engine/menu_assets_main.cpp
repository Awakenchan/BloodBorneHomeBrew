// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_assets.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <filesystem>
#include <fstream>
#include <iterator>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

std::optional<std::string> Variable(const char* name) {
#ifdef _WIN32
    std::wstring wide_name(name, name + std::strlen(name));
    const DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (!size) return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? std::nullopt : std::optional<std::string>(std::string());
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(wide_name.c_str(), value.data(), size));
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(std::max(bytes, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), out.data(), bytes, nullptr, nullptr);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::optional<std::string>(v) : std::nullopt;
#endif
}

std::optional<std::string> Title(const char* name) {
    auto v = Variable(name);
    if (!v || v->empty() || v->front() != '=') return std::nullopt;
    return v->substr(1);
}

menu_assets::PlateTitles PlateTitles() {
    return {Title("BB_PLATE_HOST_TITLE"), Title("BB_PLATE_GUEST_TITLE")};
}

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
    if (args.size() == 5 && args[1] == "wide-movies") {
        // Widescreen: every movie of LIST ("relative path<TAB>source" a line, UTF-8) whose
        // full-screen shapes need it, widened for the UI viewport WxH into DEST/relative path; a
        // line "relative path<TAB>what changed" on stdout for each, "!relative path<TAB>why" for
        // one that could not be read. Movies with nothing to widen are not written.
        unsigned width = 0, height = 0;
        if (std::sscanf(args[2].string().c_str(), "%ux%u", &width, &height) != 2 || width < 320 || height < 180 ||
            width > 32768 || height > 32768) {
            std::fputs("Engine menu assets: the UI size is not WxH\n", stderr);
            return 2;
        }
        std::ifstream list(args[3]);
        if (!list) {
            std::fputs("Engine menu assets: the list cannot be read\n", stderr);
            return 1;
        }
        for (std::string line; std::getline(list, line);) {
            while (!line.empty() && line.back() == '\r') line.pop_back();
            const auto tab = line.find('\t');
            if (tab == std::string::npos || !tab) continue;
            const std::string relative = line.substr(0, tab);
            try {
                std::ifstream in(fs::u8path(line.substr(tab + 1)), std::ios::binary);
                if (!in) throw menu_assets::Error("cannot be read");
                const menu_assets::Bytes source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (source.size() > menu_assets::kMostBytes) throw menu_assets::Error("is too large");
                std::string notes;
                // Full-screen pictures fill the screen (loading screens, menu backgrounds, the boss kill's
                // band); the title screen's art keeps its shape (title, title_dlc*, trial_title).
                const std::string stem = fs::u8path(relative).stem().string();
                const bool title = stem.rfind("title", 0) == 0 || stem == "trial_title";
                const menu_assets::Bytes made = menu_assets::WideMovie(source, int(width), int(height), notes, !title);
                if (made.empty()) {
                    // Shapes it found but left as they are (also drawn small): said, not written.
                    if (!notes.empty()) std::printf("!%s\t%s\n", relative.c_str(), notes.c_str());
                    continue;
                }
                const fs::path destination = args[4] / fs::u8path(relative);
                fs::create_directories(destination.parent_path());
                std::ofstream out(destination, std::ios::binary | std::ios::trunc);
                if (!out.write(reinterpret_cast<const char*>(made.data()), std::streamsize(made.size())) || !out.flush())
                    throw menu_assets::Error("cannot be written");
                std::printf("%s\t%s\n", relative.c_str(), notes.c_str());
            } catch (const std::exception& e) {
                std::printf("!%s\t%s\n", relative.c_str(), e.what());
            }
        }
        return 0;
    }
    if (args.size() != 4 || (args[1] != "movie" && args[1] != "messages" && args[1] != "rebirth")) {
        std::fputs("Usage: bb-engine-assets movie|messages|rebirth SOURCE DEST\n"
                   "       bb-engine-assets wide-movies WxH LIST.txt DEST   (LIST: relative path<TAB>movie a line)\n",
                   stderr);
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
                                   : menu_assets::MenuMessages(source, menu_assets::LanguageOf(args[2].parent_path().filename().string()),
                                                               PlateTitles());
        std::ofstream out(args[3], std::ios::binary | std::ios::trunc);
        if (!out.write(reinterpret_cast<const char*>(made.data()), std::streamsize(made.size())) || !out.flush())
            throw menu_assets::Error("the destination cannot be written");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Engine menu assets: %s\n", e.what());
    }
    return 1;
}
