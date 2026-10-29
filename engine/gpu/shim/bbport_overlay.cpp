// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <cctype>
#include <cfloat>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include "bbport_menu_i18n.h"
#include "bbport_settings.h"
#include "bbport_usage.h"
#include "engine/cheats.h"
#include "engine/graphics.h"
#include "engine/online_fixes.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_optiscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

#ifdef _WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_font_ttf[] = {
#embed BB_FONT_PATH
};
#pragma GCC diagnostic pop
static const unsigned char* const bb_font_ttf_end = bb_font_ttf + sizeof(bb_font_ttf);
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_ttf\n"
    ".global bb_font_ttf\n"
    "bb_font_ttf:\n"
    ".incbin \"" BB_FONT_PATH "\"\n"
    ".hidden bb_font_ttf_end\n"
    ".global bb_font_ttf_end\n"
    "bb_font_ttf_end:\n"
    ".previous\n");
extern "C" const unsigned char bb_font_ttf[];
extern "C" const unsigned char bb_font_ttf_end[];
#endif

#ifdef _WIN32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
alignas(16) static const unsigned char bb_font_cjk[] = {
#embed BB_CJK_FONT_PATH
};
#pragma GCC diagnostic pop
static const unsigned char* const bb_font_cjk_end = bb_font_cjk + sizeof(bb_font_cjk);
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden bb_font_cjk\n"
    ".global bb_font_cjk\n"
    "bb_font_cjk:\n"
    ".incbin \"" BB_CJK_FONT_PATH "\"\n"
    ".hidden bb_font_cjk_end\n"
    ".global bb_font_cjk_end\n"
    "bb_font_cjk_end:\n"
    ".previous\n");
extern "C" const unsigned char bb_font_cjk[];
extern "C" const unsigned char bb_font_cjk_end[];
#endif

extern "C" void runtime_restart(void);

namespace Vulkan {
extern std::atomic<u64> g_bb_compile_ns_total;
extern std::atomic<u32> g_bb_compiles_total;
}

namespace BbOverlay {

namespace {

std::mutex imgui_mutex;
bool initialized = false;
std::atomic<bool> menu_open{false};
bool l3_down = false, r3_down = false;
bool dirty = false;
float base_scale = 1.0f;

std::mutex prompt_mutex;
std::atomic<bool> prompt_active{false};
std::string prompt_title, prompt_text;

std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

float PixelDensity(SDL_WindowID id);

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
    if (value) {
        if (SDL_Window* window = SDL_GetMouseFocus()) {
            float x = 0.0f, y = 0.0f;
            SDL_GetMouseState(&x, &y);
            const float density = PixelDensity(SDL_GetWindowID(window));
            ImGui::GetIO().AddMousePosEvent(x * density, y * density);
        }
    }
    if (!value && dirty) {
        dirty = false;
        BbSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    if (key >= SDLK_A && key <= SDLK_Z) {
        return ImGuiKey(ImGuiKey_A + int(key - SDLK_A));
    }
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

bool Slider(const char* label, std::atomic<float>& value, float lo, float hi,
            const char* format = "%.2f", ImGuiSliderFlags flags = ImGuiSliderFlags_None) {
    float v = value;
    const bool changed = ImGui::SliderFloat(label, &v, lo, hi, format, flags);
    Store(value, v, changed);
    return changed;
}

int custom_vials = 20, custom_bullets = 20;

void Cheat(const char* label, std::atomic<int>& value, BbEngine::Cheats::Which which, int& custom) {
    ImGui::PushID(label);
    const char* modes[] = {BbSettings::MenuText("Off", "Выкл."),
                           BbSettings::MenuText("Infinite", "Бесконечно"),
                           BbSettings::MenuText("Custom", "Своё количество")};
    const int current = value;
    int mode = current == BbSettings::CheatOff ? 0 : current == BbSettings::CheatInfinite ? 1 : 2;
    if (current > 0) {
        custom = current;
    }
    if (ImGui::Combo(label, &mode, modes, 3)) {
        Store(value, mode == 0 ? BbSettings::CheatOff : mode == 1 ? BbSettings::CheatInfinite : custom, true);
    }
    if (mode == 2) {
        int amount = std::clamp(custom, 1, BbSettings::CheatMaxAmount);
        const bool changed = ImGui::SliderInt(BbSettings::MenuText("Amount", "Количество"), &amount, 1,
                                              BbSettings::CheatMaxAmount, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (changed) {
            custom = std::clamp(amount, 1, BbSettings::CheatMaxAmount);
            Store(value, custom, true);
        }
    }
    if (const char* why = BbEngine::Cheats::Unavailable(which)) {
        ImGui::TextDisabled("%s: %s", BbSettings::MenuText("Unavailable", "Недоступно"), BbSettings::MenuText(why));
    }
    ImGui::PopID();
}

ImVec2 last_hint{-1.0f, -1.0f};

void Hint(const char* text) {
    const ImVec2 setting_at = ImGui::GetItemRectMin();
    const bool setting = (setting_at.x != last_hint.x || setting_at.y != last_hint.y) &&
                         ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    last_hint = ImGui::GetItemRectMin();
    if (ImGui::BeginItemTooltip() || (setting && ImGui::BeginTooltip())) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void CheatUnavailable(BbEngine::Cheats::Which which) {
    if (const char* why = BbEngine::Cheats::Unavailable(which)) {
        ImGui::TextDisabled("%s: %s", BbSettings::MenuText("Unavailable", "Недоступно"), BbSettings::MenuText(why));
    }
}

void CheatToggle(const char* label, std::atomic<bool>& value, BbEngine::Cheats::Which which, const char* hint) {
    ImGui::PushID(label);
    Checkbox(label, value);
    Hint(hint);
    CheatUnavailable(which);
    ImGui::PopID();
}

template <std::size_t N>
void CheatLevels(const char* label, std::atomic<int>& value, const int (&levels)[N], const char* const (&names)[N],
                 BbEngine::Cheats::Which which, const char* hint) {
    ImGui::PushID(label);
    const int current = value;
    int index = 0;
    for (std::size_t i = 0; i < N; ++i) {
        if (levels[i] == current) {
            index = int(i);
        }
    }
    if (ImGui::Combo(label, &index, names, int(N))) {
        Store(value, levels[index], true);
    }
    Hint(hint);
    CheatUnavailable(which);
    ImGui::PopID();
}

void CheatStats() {
    const bool off = BbEngine::Cheats::Unavailable(BbEngine::Cheats::Echoes) != nullptr;
    ImGui::BeginDisabled(off);
    ImGui::TextUnformatted(BbSettings::MenuText("Blood Echoes", "Отголоски крови"));
    for (int amount : {1000, 10000, 100000}) {
        char label[32];
        std::snprintf(label, sizeof(label), "+%d,%03d", amount / 1000, 0);
        ImGui::SameLine();
        if (ImGui::Button(label)) {
            BbEngine::Cheats::AddEchoes(amount);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(BbSettings::MenuText("+1 Insight", "+1 озарение"))) {
        BbEngine::Cheats::AddInsight(1);
    }
    ImGui::EndDisabled();
    Hint(BbSettings::MenuText(
        "Added to the character in the world, up to the game's limits (999,999,999 Blood Echoes, 99 Insight). "
        "They are saved with your character: turning cheats off does not take them back.",
        "Добавляются персонажу в игре, до пределов игры (999 999 999 отголосков, 99 озарения). Они "
        "сохраняются вместе с персонажем: выключение читов их не отнимает."));
    CheatUnavailable(BbEngine::Cheats::Echoes);
}

char search_text[96] = "";
std::string search_query;
const char* tab_title = "";
const char* section_title = nullptr;
bool section_matches = false;
int shown_items = 0;

void AppendFolded(std::string& out, char32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ||
        (c >= 0x391 && c <= 0x3AB && c != 0x3A2) || (c >= 0x410 && c <= 0x42F)) {
        c += 32;
    } else if (c >= 0x400 && c <= 0x40F) {
        c += 80;
    } else if (c == 0x130 || c == 0x131) {
        c = 'i';
    } else if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) {
        c |= 1;
    } else if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
        c += c & 1;
    } else if (c == 0x178) {
        c = 0xFF;
    }
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

std::string Fold(const char* text) {
    std::string out;
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p && !(p[0] == '#' && p[1] == '#')) {
        char32_t c = *p++;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : extra == 1 ? 0x1F : 0x7F;
        for (; extra > 0 && (*p & 0xC0) == 0x80; --extra) {
            c = (c << 6) | (*p++ & 0x3F);
        }
        AppendFolded(out, c);
    }
    const auto first = out.find_first_not_of(' ');
    const auto last = out.find_last_not_of(' ');
    return first == std::string::npos ? std::string{} : out.substr(first, last - first + 1);
}

bool Holds(const char* text) {
    return text && Fold(text).find(search_query) != std::string::npos;
}

bool Matches(const char* english, const char* russian) {
    if (Holds(english) || (russian && Holds(russian))) {
        return true;
    }
    for (int l = 0; l < BbSettings::MenuLanguageCount; ++l) {
        const char* text = BbSettings::MenuTranslation(BbSettings::MenuLanguage(l), english);
        if (text != english && Holds(text)) {
            return true;
        }
    }
    return false;
}

bool Searching() {
    return !search_query.empty();
}

void Section(const char* english, const char* russian = nullptr) {
    section_title = russian ? BbSettings::MenuText(english, russian) : BbSettings::MenuText(english);
    section_matches = Searching() && Matches(english, russian);
}

bool WholeSection() {
    return !Searching() || section_matches;
}

bool Shown(const char* english, const char* russian = nullptr) {
    if (!WholeSection() && !Matches(english, russian)) {
        return false;
    }
    if (section_title) {
        if (Searching()) {
            const std::string title = std::string(tab_title) + "  >  " + section_title;
            ImGui::SeparatorText(title.c_str());
        } else {
            ImGui::SeparatorText(section_title);
        }
        section_title = nullptr;
    }
    ++shown_items;
    return true;
}

const char* Item(const char* english, const char* russian = nullptr) {
    if (!Shown(english, russian)) {
        return nullptr;
    }
    return russian ? BbSettings::MenuText(english, russian) : BbSettings::MenuText(english);
}

void HdrSection() {
    auto& s = BbSettings::Get();
    Section("HDR output", "Вывод HDR");
    if (const char* label = Item("HDR output (restart required)", "Вывод HDR (после перезапуска)")) {
        Checkbox(label, s.hdr);
        Hint(BbSettings::MenuText(
            "Shows the game's SDR picture on an HDR display without washed-out colors, with optional "
            "highlight expansion. Windows HDR must be on (Settings > Display > Use HDR). Turning it "
            "on or off applies after restarting the game.",
            "Показывает SDR-картинку игры на HDR-дисплее без блёклых цветов, с необязательным "
            "расширением светлых участков. В Windows должен быть включён HDR (Параметры > Дисплей > "
            "Использовать HDR). Включение и выключение применяются после перезапуска игры."));
    }
    const int output = s.hdr_output.load();
    if (output == BbSettings::HdrOutputOff) {
        if (s.startup_hdr && WholeSection() && Shown("HDR output", "Вывод HDR")) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s",
                               BbSettings::MenuText(
                                   "Not active: the display offers no HDR format (is Windows HDR on?)",
                                   "Не активен: дисплей не предлагает HDR-формат (включён ли HDR "
                                   "в Windows?)"));
            ImGui::PopTextWrapPos();
        }
        return;
    }
    if (WholeSection() && Shown("HDR output", "Вывод HDR")) {
        ImGui::TextDisabled("%s", output == BbSettings::HdrOutputScRgb
                                      ? BbSettings::MenuText("Active: scRGB (FP16)", "Активен: scRGB (FP16)")
                                      : BbSettings::MenuText("Active: HDR10 (PQ, BT.2020)",
                                                             "Активен: HDR10 (PQ, BT.2020)"));
    }
    int mode = std::clamp(s.hdr_mode.load(), 0, BbSettings::HdrModeCount - 1);
    if (const char* label = Item("HDR mode", "Режим HDR")) {
        const char* modes[BbSettings::HdrModeCount] = {
            BbSettings::MenuText("Native (from the game's HDR scene)", "Нативный (из HDR-сцены игры)"),
            BbSettings::MenuText("Expand (from the SDR picture)", "Расширение (из SDR-картинки)"),
        };
        const bool mode_changed = ImGui::Combo(label, &mode, modes, BbSettings::HdrModeCount);
        Store(s.hdr_mode, mode, mode_changed);
        Hint(BbSettings::MenuText(
            "Native: the brightness of lamps, fire, the moon and other highlights comes from the "
            "game's own HDR scene, before its tonemap clipped it to SDR white. Midtones, colours and "
            "the UI stay as in the SDR picture. Menus, loading screens and movies (no scene) use "
            "Expand. Expand: the SDR picture's brightest parts are stretched towards the peak.",
            "Нативный: яркость ламп, огня, луны и других светлых мест берётся из собственной "
            "HDR-сцены игры, до того как тонмаппинг обрезал её до белого SDR. Средние тона, цвета и "
            "интерфейс остаются как в SDR-картинке. В меню, на загрузках и в видеороликах (без сцены) "
            "работает «Расширение». Расширение: самые яркие места SDR-картинки растягиваются к пику."));
    }
    if (const char* label = Item("Paper white (nits)", "Белый SDR (нит)")) {
        Slider(label, s.hdr_paper_white, BbSettings::HdrPaperWhiteMin, BbSettings::HdrPaperWhiteMax, "%.0f",
               ImGuiSliderFlags_AlwaysClamp);
    }
    const float display_peak = s.hdr_display_peak.load();
    const bool from_display = s.hdr_peak_auto.load() && display_peak >= BbSettings::HdrPeakMin;
    if (const char* label = Item("Peak brightness from the display", "Пиковая яркость от дисплея")) {
        Checkbox(label, s.hdr_peak_auto);
        if (from_display) {
            char reported[96];
            std::snprintf(reported, sizeof(reported), "%s: %.0f",
                          BbSettings::MenuText("The display reports (nits)", "Дисплей сообщает (нит)"),
                          display_peak);
            ImGui::TextDisabled("%s", reported);
        } else if (s.hdr_peak_auto.load()) {
            ImGui::TextDisabled("%s", BbSettings::MenuText("The display reports no peak: the value below is used",
                                                           "Дисплей не сообщает пик: используется значение ниже"));
        }
    }
    if (const char* label = Item("Peak brightness (nits)", "Пиковая яркость (нит)")) {
        ImGui::BeginDisabled(from_display);
        Slider(label, s.hdr_peak, BbSettings::HdrPeakMin, BbSettings::HdrPeakMax, "%.0f",
               ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        ImGui::EndDisabled();
    }
    if (const char* label = Item("Highlight boost", "Усиление светлых участков")) {
        Slider(label, s.hdr_boost, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        Hint(BbSettings::MenuText(
            "Paper white: how bright the game's white (and this menu) is; about 200 suits a dim room. "
            "Peak: your display's peak brightness. From the display: the value Windows reports for it "
            "(the Windows HDR Calibration app's once it has run); turn it off to set it yourself. "
            "Highlight boost: 0 shows the SDR picture as it is; higher values let the brightest parts "
            "(lamps, fire, the moon) rise towards the peak. In Native mode it is how much of the "
            "scene's own highlight range is shown: 0.25 about half, 1 all of it (up to the peak). "
            "Changes apply live. Ctrl+click a slider to type an exact value.",
            "Белый SDR: яркость белого в игре (и этого меню); около 200 подходит для неяркой комнаты. "
            "Пиковая яркость: максимум вашего дисплея. «От дисплея» — значение, которое сообщает Windows "
            "(после приложения «Калибровка HDR» — его значение); выключите, чтобы задать вручную. "
            "Усиление светлых участков: 0 — SDR-картинка как есть; больше — "
            "самые яркие места (лампы, огонь, луна) поднимаются к пику. В нативном режиме — какая "
            "доля собственного диапазона светлых мест сцены показывается: 0.25 — около половины, "
            "1 — весь (до пика). Меняются сразу. Ctrl+клик по ползунку — ввести точное значение."));
    }
    if (mode == BbSettings::HdrModeNative) {
        if (const char* label = Item("UI brightness", "Яркость интерфейса")) {
            Slider(label, s.hdr_ui_brightness, BbSettings::HdrUiBrightnessMin, BbSettings::HdrUiBrightnessMax,
                   "%.2f", ImGuiSliderFlags_AlwaysClamp);
        }
        if (const char* label = Item("UI colour", "Цвета интерфейса")) {
            Slider(label, s.hdr_ui_color, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            Hint(BbSettings::MenuText(
                "Native mode only: the game's HUD, menus, subtitles and loading screens. UI brightness: their "
                "white, times paper white. UI colour: 0 shows their exact sRGB colours, 1 the wider colours a "
                "wide-gamut monitor shows SDR with (the usual look; exact sRGB can look washed out next to "
                "it). Live.",
                "Только в нативном режиме: интерфейс игры, меню, субтитры и экраны загрузки. Яркость "
                "интерфейса: его белый, умноженный на белый SDR. Цвета интерфейса: 0 — точные цвета sRGB, "
                "1 — более насыщенные, как на мониторе с широким охватом в SDR (привычный вид; точный sRGB "
                "рядом с ним может казаться блёклым). Меняются сразу."));
        }
    }
    if (const char* label = Item("Compare: SDR | HDR", "Сравнение: SDR | HDR")) {
        Checkbox(label, s.hdr_compare);
        Hint(BbSettings::MenuText(
            "The left half of the screen as an SDR display shows the game (at paper white), the right "
            "half in HDR, to judge the settings above. Not saved.",
            "Левая половина экрана — как игру показывает SDR-дисплей (при белом SDR), правая — в HDR, "
            "чтобы оценить настройки выше. Не сохраняется."));
    }
}

void PictureSection() {
    auto& s = BbSettings::Get();
    Section("Picture", "Изображение");
    const bool live = BbEngine::Graphics::Enabled();
    if (!live && WholeSection() && Shown("Picture", "Изображение")) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("Unavailable: the game's code differs from version 1.09.",
                                                       "Недоступно: код игры отличается от версии 1.09."));
    }
    ImGui::BeginDisabled(!live);
    if (const char* label = Item("Picture tuning", "Настройка изображения")) {
        Checkbox(label, s.graphics_controls);
        Hint(BbSettings::MenuText(
            "Changes apply live, Picture tuning too, to the game's own colour, fog and lighting of each area "
            "(the HUD and menus keep theirs). Moving one turns Picture tuning on; off: the game's own picture. 1.00 and Hue 0 are the game's. Fog 0 clears the haze over distant streets. The "
            "same settings are in the game's System menu (PC Graphics, PC Effects, PC Colour) and the "
            "launcher. Ctrl+click a slider to type any value, beyond its ends too (brightness, saturation and fog "
            "up to 5, contrast 0.5-1.5, hue -180-180, bloom up to 10, shadow distance 0.25-8).",
            "Меняются сразу, как и сама настройка: собственные цвет, туман и освещение каждой области "
            "(интерфейс и меню не меняются). Выкл. — картинка игры. 1.00 и оттенок 0 — как в игре. Туман 0 убирает дымку над дальними улицами. Те же "
            "настройки есть в системном меню игры (PC Graphics, PC Effects, PC Colour) и в лаунчере. "
            "Ctrl+клик по ползунку — ввести любое значение, в том числе за его краями (яркость, "
            "насыщенность и туман до 5, контраст 0.5-1.5, оттенок -180-180, bloom до 10, дальность теней "
            "0.25-8)."));
    }
    const bool vignette = s.graphics_vignette;
    bool moved = false;
    const auto slider = [&](const char* english, const char* russian, std::atomic<float>& value, float lo,
                            float hi, const char* format = "%.2f") {
        if (const char* label = Item(english, russian)) {
            moved |= Slider(label, value, lo, hi, format);
        }
    };
    slider("Brightness", "Яркость", s.graphics_brightness, 0.0f, 3.0f);
    slider("Contrast", "Контраст", s.graphics_contrast, 0.8f, 1.2f, "%.3f");
    slider("Saturation", "Насыщенность", s.graphics_saturation, 0.0f, 3.0f);
    slider("Hue", "Оттенок", s.graphics_hue, -180.0f, 180.0f, "%.0f");
    slider("Fog", "Туман", s.graphics_fog, 0.0f, 3.0f);
    slider("Bloom", nullptr, s.graphics_bloom, 0.0f, 3.0f);
    if (const char* label = Item("Vignette", "Виньетка")) {
        Checkbox(label, s.graphics_vignette);
        moved |= vignette != s.graphics_vignette;
    }
    slider("Ambient occlusion strength", "Сила затенения (AO)", s.graphics_ao_strength, 0.0f, 3.0f);
    slider("Shadow distance", "Дальность теней", s.graphics_shadow_scale, 0.5f, 4.0f);
    if (moved && !s.graphics_controls) {
        Store(s.graphics_controls, true, true);
    }
    if (const char* label = Item("Game's own picture", "Как в игре")) {
        if (ImGui::Button(label)) {
            Store(s.graphics_brightness, 1.0f, true);
            Store(s.graphics_contrast, 1.0f, true);
            Store(s.graphics_saturation, 1.0f, true);
            Store(s.graphics_hue, 0.0f, true);
            Store(s.graphics_fog, 1.0f, true);
            Store(s.graphics_bloom, 1.0f, true);
            Store(s.graphics_vignette, true, true);
            Store(s.graphics_ao_strength, 1.0f, true);
            Store(s.graphics_shadow_scale, 1.0f, true);
        }
    }
    ImGui::EndDisabled();
}

void EffectsSection() {
    auto& s = BbSettings::Get();
    Section("Game effects (restart required)", "Эффекты игры (после перезапуска)");
    if (const char* label = Item("Model detail", "Детализация моделей")) {
        const char* lods[] = {BbSettings::MenuText("Highest (-2)", "Максимальная (-2)"),
                              BbSettings::MenuText("Game default", "Как в игре"),
                              BbSettings::MenuText("Lower (1)", "Ниже (1)"),
                              BbSettings::MenuText("Lowest (2)", "Минимальная (2)")};
        static constexpr int lod_values[] = {-2, 0, 1, 2};
        int lod_index = 1;
        for (int i = 0; i < 4; ++i) {
            if (lod_values[i] == s.model_lod) lod_index = i;
        }
        if (ImGui::BeginCombo(label, lods[lod_index])) {
            for (int i = 0; i < 4; ++i) {
                if (ImGui::Selectable(lods[i], i == lod_index)) {
                    Store(s.model_lod, lod_values[i], true);
                }
            }
            ImGui::EndCombo();
        }
    }
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        const auto& effect = BbSettings::Effects[e];
        const char* label = Item(effect.label, effect.label_ru);
        if (!label) {
            continue;
        }
        Checkbox(label, s.effects[e]);
        if (std::strcmp(effect.key, "effect_dynamic_shadows") == 0) {
            Hint(BbSettings::MenuText(
                "Effects are enabled and disabled by game patches at startup (patches/Bloodborne.xml). "
                "Motion blur and shadows from dynamic lights place a significant load on the GPU.",
                "Эффекты включаются и выключаются патчами игры при запуске (patches/Bloodborne.xml). "
                "Размытие в движении и тени от динамических источников заметно нагружают GPU."));
        } else if (std::strcmp(effect.key, "debug_menu") == 0) {
            Hint(BbSettings::MenuText(
                "Free camera: hold Cross and press L3 (keyboard: Space + Z). "
                "Debug menu: left touchpad / Tab. Requires DbgFont14h.ccm and DbgFont14h.tpf "
                "in dvdroot_ps4/font from Nexus mod #253. Right touchpad: Backspace.",
                "Свободная камера: удерживайте Cross и нажимайте L3 (клавиатура: Space + Z). "
                "Debug menu: левый touchpad / Tab. Нужны DbgFont14h.ccm и DbgFont14h.tpf "
                "в dvdroot_ps4/font из мода Nexus #253. Правый touchpad: Backspace."));
        }
    }
}

void UpscalerSection() {
    auto& s = BbSettings::Get();
    Section("Temporal upscaler", "Временной апскейлер");
    if (const char* label = Item("Upscaler", "Апскейлер")) {
        const char* upscalers[] = {
            BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "FSR 4 (INT8)", "FSR 4.1.1 (INT8)",
            BbSettings::MenuText("TAA (native anti-aliasing)", "TAA (нативное сглаживание)"), "DLSS (NVIDIA)"};
        static const char* later[] = {"XeSS"};
        const int upscaler = s.upscaler;
        if (ImGui::BeginCombo(label, upscalers[upscaler])) {
            for (int i = 0; i < BbSettings::UpscalerCount; ++i) {
                const bool supported = i == BbSettings::UpscalerFsr4     ? s.fsr4_supported.load()
                                       : i == BbSettings::UpscalerFsr411 ? s.fsr411_supported.load()
                                       : i == BbSettings::UpscalerDlss   ? s.dlss_supported.load()
                                                                         : true;
                ImGui::BeginDisabled(!supported);
                if (ImGui::Selectable(upscalers[i], i == upscaler)) {
                    Store(s.upscaler, i, true);
                }
                ImGui::EndDisabled();
                if (!supported) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", BbSettings::MenuText("— not supported by this GPU",
                                                                   "— не поддерживается видеокартой"));
                }
            }
            for (const char* name : later) {
                ImGui::BeginDisabled();
                ImGui::Selectable(name, false);
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::TextDisabled("%s", BbSettings::MenuText("— in development", "— в работе"));
            }
            ImGui::EndCombo();
        }
        if (s.upscaler == BbSettings::UpscalerFsr411) {
            Hint(BbSettings::MenuText(
                "FSR 4.1.1 in INT8 mode: the model from AMD's 4.1.1 DLL, reproduced in Vulkan "
                "(output matches the DLL). One model for Native through Performance and another "
                "for Ultra Performance. Assets: tools/fsr4cap/build_assets.sh (requires the DLL "
                "and Proton).",
                "FSR 4.1.1 в режиме INT8: модель из DLL AMD 4.1.1, воспроизведённая в Vulkan "
                "(результат совпадает с DLL). Одна модель для Native..Performance и отдельная "
                "для Ultra Performance. Ассеты: tools/fsr4cap/build_assets.sh (нужны DLL и "
                "Proton)."));
        } else if (s.upscaler == BbSettings::UpscalerFsr4) {
            Hint(BbSettings::MenuText(
                "FSR 4 in INT8 mode (v07 model from AMD FidelityFX SDK sources). Higher quality "
                "than FSR 3.1, but the pass is more demanding. Changing the preset rebuilds the "
                "model (a brief pause). Assets: tools/fetch_fsr4_assets.sh.",
                "FSR 4 в режиме INT8 (модель v07 из исходников AMD FidelityFX SDK). Качество выше, "
                "чем у FSR 3.1, но проход тяжелее. Смена пресета пересобирает модель (короткая "
                "пауза). Ассеты: tools/fetch_fsr4_assets.sh."));
        }
        if (const char* problem = s.fsr4_problem.load()) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), BbSettings::MenuText("Upscaler unavailable: %s"),
                               BbSettings::MenuText(problem));
            if (!BbSettings::IsFsr4(s.upscaler))
                ImGui::TextUnformatted(
                    BbSettings::MenuText("The mode selected above is active. You can select it again to retry."));
            ImGui::PopTextWrapPos();
        }
    }
    if (BbSettings::IsFsr4(s.upscaler)) {
        if (const char* label = Item("FSR 4: auto exposure", "FSR 4: авто-экспозиция")) {
            Checkbox(label, s.fsr4_auto_exposure);
        }
        if (const char* label = Item("FSR 4: invert jitter sign", "FSR 4: обратный знак jitter")) {
            Checkbox(label, s.fsr4_invert_jitter);
            Hint(BbSettings::MenuText(
                "For diagnosing ghosting: the FSR 4 network normalizes color by exposure and uses "
                "it to decide when to discard previous frames. Changes apply immediately, without a "
                "restart.",
                "Проверка при гостинге: сеть FSR 4 нормирует цвет по экспозиции и по ней решает, "
                "когда отбросить прошлые кадры. Меняются сразу, без перезапуска."));
        }
    }
    const char* frame_gens[] = {BbSettings::MenuText("Off", "Выкл"), "FSR 3.1", "DLSS (NVIDIA)"};
    static_assert(std::size(frame_gens) == BbSettings::FrameGenCount);
    const int frame_gen = std::clamp(s.frame_gen.load(), 0, BbSettings::FrameGenCount - 1);
    const bool dlss_loaded = s.streamline_loaded.load();
    const bool dlss_fg = s.dlss_fg_supported.load();
    if (const char* label = Item("Frame generation", "Генерация кадров")) {
        if (ImGui::BeginCombo(label, frame_gens[frame_gen])) {
            for (int i = 0; i < BbSettings::FrameGenCount; ++i) {
                const bool unavailable = i == BbSettings::FrameGenDlss && dlss_loaded && !dlss_fg;
                ImGui::BeginDisabled(unavailable);
                if (ImGui::Selectable(frame_gens[i], i == frame_gen)) {
                    Store(s.frame_gen, i, true);
                }
                ImGui::EndDisabled();
                if (unavailable) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", BbSettings::MenuText("— not supported by this GPU",
                                                                   "— не поддерживается видеокартой"));
                }
            }
            ImGui::EndCombo();
        }
        if (frame_gen == BbSettings::FrameGenDlss) {
            Hint(BbSettings::MenuText(
                "NVIDIA DLSS Frame Generation: generated frames from the game's depth, motion vectors "
                "and the frame without its UI, paced by the driver, with NVIDIA Reflex. RTX 40 (one "
                "frame) or RTX 50 (up to five); Hardware-accelerated GPU scheduling on. Loads when the "
                "game starts with it selected. Off in menus, movies and with HDR output.",
                "NVIDIA DLSS Frame Generation: кадры из глубины, векторов движения и кадра без "
                "интерфейса игры, подачу ведёт драйвер, с NVIDIA Reflex. RTX 40 (один кадр) или RTX 50 "
                "(до пяти); должно быть включено аппаратное планирование GPU. Загружается, если игра "
                "запущена с этим выбором. Не работает в меню, роликах и с HDR."));
        } else {
            Hint(BbSettings::MenuText(
                "Shows an extra frame between the game's own (2x smoother motion). Adds a little input "
                "lag. Needs an upscaler; off in menus, movies and with HDR output.",
                "Показывает дополнительный кадр между кадрами игры (движение в 2 раза плавнее). Немного "
                "увеличивает задержку ввода. Нужен апскейлер; не работает в меню, роликах и с HDR."));
        }
        if (frame_gen == BbSettings::FrameGenDlss) {
            ImGui::PushTextWrapPos();
            if (!dlss_loaded) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                                   BbSettings::MenuText("Restart the game to load DLSS Frame Generation.",
                                                        "Перезапустите игру, чтобы загрузить DLSS Frame Generation."));
            }
            if (const char* problem = s.dlss_fg_problem.load()) {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                                   BbSettings::MenuText("DLSS Frame Generation unavailable: %s",
                                                        "DLSS Frame Generation недоступна: %s"),
                                   BbSettings::MenuText(problem));
            }
            ImGui::PopTextWrapPos();
        }
    }
    if (frame_gen == BbSettings::FrameGenDlss || Searching()) {
        if (const char* label = Item("DLSS: generated frames", "DLSS: сгенерированных кадров")) {
            const int max_frames = dlss_fg ? std::max(1, s.dlss_fg_max_frames.load()) : BbSettings::FrameGenFramesMax;
            int frames = std::clamp(s.frame_gen_frames.load(), 1, max_frames);
            char shown[16];
            std::snprintf(shown, sizeof(shown), "%d (%dx)", frames, frames + 1);
            if (ImGui::SliderInt(label, &frames, 1, max_frames, shown, ImGuiSliderFlags_AlwaysClamp)) {
                Store(s.frame_gen_frames, frames, true);
            }
            Hint(BbSettings::MenuText(
                "Generated frames per frame the game renders: 1 doubles the frames shown, 3 is 4x. More "
                "than one needs an RTX 50 GPU (DLSS multi frame generation). Live.",
                "Сгенерированных кадров на каждый кадр игры: 1 удваивает число кадров, 3 — это 4x. "
                "Больше одного — только RTX 50 (DLSS Multi Frame Generation). Меняется сразу."));
        }
        const bool dynamic_ok = s.dlss_fg_dynamic_supported.load();
        if (const char* label = Item("DLSS: dynamic multi frame generation",
                                     "DLSS: динамическая генерация кадров")) {
            ImGui::BeginDisabled(!dynamic_ok && !Searching());
            Checkbox(label, s.frame_gen_dynamic);
            ImGui::EndDisabled();
            Hint(dynamic_ok
                     ? BbSettings::MenuText(
                           "The driver picks 2x, 3x or 4x moment by moment to reach the target (up to "
                           "the generated frames above): fewer generated frames when the game is fast, "
                           "less lag and fewer artifacts. Live.",
                           "Драйвер сам выбирает 2x, 3x или 4x, чтобы достичь цели (не больше числа "
                           "кадров выше): меньше сгенерированных кадров, когда игра быстрая, меньше "
                           "задержка и артефактов. Меняется сразу.")
                     : BbSettings::MenuText("Needs an RTX 50 GPU, a recent driver and DLSS Frame Generation loaded.",
                                            "Нужна RTX 50, свежий драйвер и загруженная DLSS Frame Generation."));
        }
        if (const char* label = Item("DLSS: dynamic target FPS", "DLSS: целевой FPS")) {
            ImGui::BeginDisabled(!s.frame_gen_dynamic.load());
            int target = s.frame_gen_target_fps.load();
            char shown[32];
            if (target == 0) {
                std::snprintf(shown, sizeof(shown), "%s", BbSettings::MenuText("Refresh rate", "Частота экрана"));
            } else {
                std::snprintf(shown, sizeof(shown), "%d FPS", target);
            }
            if (ImGui::SliderInt(label, &target, 0, 360, shown, ImGuiSliderFlags_AlwaysClamp)) {
                Store(s.frame_gen_target_fps, target, true);
            }
            ImGui::EndDisabled();
        }
    }
    if (const char* label = Item("Frame generation: game at a share of the refresh rate",
                                 "Генерация кадров: игра на доле частоты экрана")) {
        ImGui::BeginDisabled(frame_gen == BbSettings::FrameGenOff);
        Checkbox(label, s.frame_gen_cap);
        ImGui::EndDisabled();
        Hint(BbSettings::MenuText(
            "Every generated frame reaches the screen evenly: the game at the refresh rate divided by "
            "the frames shown per game frame (60 + 60 at 120 Hz with one generated frame). Off: the "
            "game runs faster, but the screen skips some generated frames.",
            "Каждый сгенерированный кадр равномерно попадает на экран: игра на частоте экрана, "
            "делённой на число показанных кадров (60 + 60 при 120 Гц с одним сгенерированным). Выкл: "
            "игра быстрее, но часть сгенерированных кадров экран пропускает."));
    }
    if (dlss_loaded || Searching()) {
        if (const char* label = Item("NVIDIA Reflex")) {
            const char* modes[] = {BbSettings::MenuText("Off", "Выкл"), BbSettings::MenuText("On", "Вкл"),
                                   BbSettings::MenuText("On + Boost", "Вкл + Boost")};
            static_assert(std::size(modes) == BbSettings::ReflexCount);
            int mode = std::clamp(s.reflex.load(), 0, BbSettings::ReflexCount - 1);
            if (ImGui::Combo(label, &mode, modes, BbSettings::ReflexCount)) {
                Store(s.reflex, mode, true);
            }
            Hint(BbSettings::MenuText(
                "Lower input lag: the GPU's queue is kept short. On (at least) whenever DLSS Frame "
                "Generation runs; Boost also keeps the GPU clocks up. Only with DLSS Frame Generation "
                "loaded.",
                "Меньше задержка ввода: очередь GPU остаётся короткой. Включается всегда, когда "
                "работает DLSS Frame Generation; Boost ещё держит частоты GPU. Только когда DLSS Frame "
                "Generation загружена."));
        }
    }
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    if (const char* label = Item("Preset", "Пресет")) {
        ImGui::BeginDisabled(taa);
        const int preset = taa ? BbSettings::NativeAA : s.preset.load();
        char preset_label[64];
        std::snprintf(preset_label, sizeof(preset_label), "%s (x%.1f)",
                      BbSettings::MenuText(BbSettings::PresetName(preset)), BbSettings::PresetScale(preset));
        if (ImGui::BeginCombo(label, preset_label)) {
            for (int i = 0; i < BbSettings::PresetCount; ++i) {
                char choice[64];
                const float scale = BbSettings::PresetScale(i);
                const int output = s.output_res;
                std::snprintf(
                    choice, sizeof(choice),
                    BbSettings::MenuText("%s (x%.1f, render %dx%d)", "%s (x%.1f, рендер %dx%d)"),
                    BbSettings::MenuText(BbSettings::PresetName(i)), scale,
                    int(std::lround(BbSettings::OutputWidth(output) / scale / 4) * 4),
                    int(std::lround(BbSettings::OutputHeight(output) / scale / 4) * 4));
                if (ImGui::Selectable(choice, i == preset)) {
                    Store(s.preset, i, true);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (taa) {
            ImGui::TextWrapped("%s", BbSettings::MenuText(
                                         "TAA anti-aliases the scene at the output resolution, without an FSR "
                                         "model or upscaling. The saved preset returns when an upscaler is selected."));
        }
        if (!taa && s.preset == BbSettings::PresetCustom) {
            if (const char* scale_label = Item("Render scale (Custom)", "Масштаб рендера (Custom)")) {
                Slider(scale_label, s.render_scale, BbSettings::RenderScaleMin, BbSettings::RenderScaleMax, "x%.2f");
                Hint(BbSettings::MenuText(
                    "Output / render size for the Custom preset: 1.00 renders at the output (DLAA, Native "
                    "AA), 1.30 is between Native and Quality, 3.00 is Ultra Performance. FSR 4 and DLSS run "
                    "the model of the nearest preset. Ctrl+click to type a value.",
                    "Отношение вывода к рендеру для пресета Custom: 1.00 — рендер в разрешении вывода "
                    "(DLAA, Native AA), 1.30 — между Native и Quality, 3.00 — Ultra Performance. FSR 4 и "
                    "DLSS берут модель ближайшего пресета. Ctrl+клик — ввести значение."));
            }
        }
        ImGui::Text(BbSettings::MenuText("Active scene render: %d x %d"), s.active_render_width.load(),
                    s.active_render_height.load());
        if (BbSettings::FixedRenderSession()) {
            ImGui::Text(BbSettings::MenuText("Preset at startup: %s"),
                        BbSettings::MenuText(BbSettings::PresetName(s.startup_preset)));
            if (const char* automatic = std::getenv("BB_AUTO_RENDER_RES");
                automatic && automatic[0] == '1') {
                Hint(BbSettings::MenuText(
                    "With an output other than 1080p the whole game renders at the preset resolution "
                    "(patched at startup): fastest on the Steam Deck and weaker GPUs. Preset or output "
                    "changes apply after a restart. \"Live resolution changes\" below allows changing "
                    "without a restart (post-processing then stays at 1080p, slower)."));
            } else {
                Hint(BbSettings::MenuText(
                    "BB_RENDER_RES fixes the scene size at startup. Remove this explicit variable "
                    "to change resolution and presets without restarting the game."));
            }
        } else {
            Hint(BbSettings::MenuText(
                "Native AA: the upscaler works as anti-aliasing. The other presets lower the scene "
                "render resolution relative to the output. The UI is drawn at the output resolution. "
                "The preset applies from the next frame without restarting the game."));
        }
    }
    if (const char* label = Item("Sharpening (RCAS)")) {
        Checkbox(label, s.sharpen);
    }
    if (const char* label = Item("Sharpness")) {
        ImGui::BeginDisabled(!s.sharpen);
        Slider(label, s.sharpness, 0.0f, 2.0f);
        ImGui::EndDisabled();
        Hint(BbSettings::MenuText(
            "Up to 1: the upscaler's own sharpening (RCAS). Above 1 another RCAS pass is added. "
            "DLSS has no sharpening of its own: an RCAS pass does all of it. "
            "Ctrl+click the slider to type an exact value."));
    }
    if (const char* label = Item("Texture mip bias", "Mip bias текстур")) {
        Slider(label, s.mip_bias, BbSettings::MipBiasMin, BbSettings::MipBiasMax, "%+.2f");
        Hint(BbSettings::MenuText(
            "Added to the texture detail the upscaler already asks for (the render-to-output ratio, "
            "and DLSS's extra level): below 0 sharper textures (more shimmer), above 0 softer. 0: "
            "the upscaler's own. Live.",
            "Добавляется к детализации текстур, которую уже задаёт апскейлер (соотношение рендера и "
            "вывода и лишний уровень DLSS): меньше 0 — чётче (больше мерцания), больше 0 — мягче. 0 — "
            "как у апскейлера. Меняется сразу."));
    }
    if (const char* label = Item("Sub-pixel jitter")) {
        Checkbox(label, s.jitter);
        Hint(BbSettings::MenuText(
            "Each frame the scene shifts by a fraction of a pixel, and the upscaler gathers more "
            "detail from several frames. Without it you only get history-based anti-aliasing."));
    }
    ImGui::EndDisabled();
}

void DlssSection() {
    auto& s = BbSettings::Get();
    if (s.upscaler != BbSettings::UpscalerDlss && !Searching()) {
        return;
    }
    Section("DLSS");
    if (const char* label = Item("DLSS model", "Модель DLSS")) {
        static constexpr int presets[] = {BbSettings::DlssPresetAuto, 11, 10, 13, 12, 5, 6,
                                          BbSettings::DlssPresetDefault};
        const char* names[] = {
            BbSettings::MenuText("Auto: K, M for Performance, L for Ultra Performance",
                                 "Авто: K, M для Performance, L для Ultra Performance"),
            BbSettings::MenuText("K: transformer (default)", "K: трансформер (по умолчанию)"),
            BbSettings::MenuText("J: transformer", "J: трансформер"),
            BbSettings::MenuText("M: transformer, 2nd generation", "M: трансформер 2-го поколения"),
            BbSettings::MenuText("L: transformer, 2nd generation (Ultra Performance)",
                                 "L: трансформер 2-го поколения (Ultra Performance)"),
            BbSettings::MenuText("E: CNN", "E: CNN"),
            BbSettings::MenuText("F: CNN (Ultra Performance, DLAA)", "F: CNN (Ultra Performance, DLAA)"),
            BbSettings::MenuText("The DLL's default", "По умолчанию DLL"),
        };
        static_assert(std::size(presets) == std::size(names));
        const int current = s.dlss_preset;
        int index = -1;
        for (int i = 0; i < int(std::size(presets)); ++i) {
            if (presets[i] == current) index = i;
        }
        char other[48];
        std::snprintf(other, sizeof(other), "%s %c", BbSettings::MenuText("Preset", "Пресет"),
                      char('A' + std::clamp(current, 1, BbSettings::DlssPresetMax) - 1));
        if (ImGui::BeginCombo(label, index >= 0 ? names[index] : other)) {
            for (int i = 0; i < int(std::size(presets)); ++i) {
                if (ImGui::Selectable(names[i], i == index)) {
                    Store(s.dlss_preset, presets[i], true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "Which NVIDIA model DLSS runs; it changes at once (a brief pause). K keeps thin and moving "
            "objects steadier; E is a little faster on RTX 20 and 30 cards but softer in motion. M and L "
            "need a DLSS 4.5 nvngx_dlss.dll (310.5 or newer). A preset the DLL refuses runs its default. "
            "The launcher's Graphics page has the same setting.",
            "Какую модель NVIDIA запускает DLSS; меняется сразу (короткая пауза). K устойчивее на тонких "
            "и движущихся объектах; E чуть быстрее на RTX 20 и 30, но мягче в движении. Для M и L нужен "
            "nvngx_dlss.dll из DLSS 4.5 (310.5 или новее). Пресет, который DLL не принимает, заменяется "
            "её пресетом по умолчанию. Та же настройка есть на странице «Графика» лаунчера."));
        const int applied = s.dlss_applied_preset;
        if (applied > 0) {
            ImGui::TextDisabled(BbSettings::MenuText("Running: preset %c", "Работает: пресет %c"),
                                char('A' + applied - 1));
        } else if (applied == 0) {
            ImGui::TextDisabled("%s", BbSettings::MenuText("Running: the DLL's default preset",
                                                           "Работает: пресет DLL по умолчанию"));
        }
        if (applied == 0 && current != BbSettings::DlssPresetDefault) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                               BbSettings::MenuText("This DLSS version refuses preset %c: its default runs",
                                                    "Эта версия DLSS не принимает пресет %c: работает её пресет"),
                               char('A' + std::clamp(current, 1, BbSettings::DlssPresetMax) - 1));
            ImGui::PopTextWrapPos();
        }
    }
    if (const char* label = Item("DLSS: auto exposure", "DLSS: авто-экспозиция")) {
        Checkbox(label, s.dlss_auto_exposure);
        Hint(BbSettings::MenuText(
            "DLSS works out the frame's exposure itself, for the game's tonemapped frame too (the HDR "
            "scene at 1080p always has it). Can steady dark scenes that ghost. Live (a brief pause).",
            "DLSS сам определяет экспозицию кадра, и для готового кадра игры (HDR-сцена при 1080p — "
            "всегда). Может успокоить тёмные сцены с гостингом. Меняется сразу (короткая пауза)."));
    }
    if (const char* label = Item("DLSS: sharper textures (mip bias -1)", "DLSS: чётче текстуры (mip bias -1)")) {
        Checkbox(label, s.dlss_mip_bias);
        Hint(BbSettings::MenuText(
            "Textures one mip level sharper than the render resolution alone gives, as NVIDIA's DLSS "
            "guide asks. Off: only the render-to-output ratio. Live.",
            "Текстуры на один mip-уровень чётче, чем даёт само разрешение рендера, как советует "
            "руководство NVIDIA по DLSS. Выкл.: только соотношение рендера и вывода. Меняется сразу."));
    }
}

void Fsr3Section() {
    auto& s = BbSettings::Get();
    if (s.upscaler != BbSettings::UpscalerFsr3 && !Searching()) {
        return;
    }
    Section("FSR 3.1 tuning", "Настройка FSR 3.1");
    struct Tuning {
        const char* en;
        const char* ru;
        float velocity, reactiveness, shading, accumulation, disocclusion;
    };
    static constexpr Tuning presets[] = {
        {"Default (AMD's)", "По умолчанию (AMD)", 1.0f, 1.0f, 1.0f, 0.333f, -0.333f},
        {"Steady bright pixels", "Стабильные яркие пиксели", 0.5f, 1.0f, 1.0f, 0.333f, -0.333f},
        {"Less ghosting", "Меньше шлейфов", 1.0f, 1.5f, 1.5f, 0.25f, -0.333f},
        {"Less flicker", "Меньше мерцания", 1.0f, 1.0f, 1.0f, 0.4f, -0.1f},
    };
    if (const char* label = Item("Preset", "Пресет")) {
        int index = -1;
        for (int i = 0; i < int(std::size(presets)); ++i) {
            const auto& p = presets[i];
            const auto near = [](float a, float b) { return std::abs(a - b) < 0.0005f; };
            if (near(s.fsr3_velocity_factor, p.velocity) && near(s.fsr3_reactiveness_scale, p.reactiveness) &&
                near(s.fsr3_shading_change_scale, p.shading) && near(s.fsr3_accumulation, p.accumulation) &&
                near(s.fsr3_min_disocclusion, p.disocclusion)) {
                index = i;
            }
        }
        if (ImGui::BeginCombo(label, index >= 0 ? BbSettings::MenuText(presets[index].en, presets[index].ru)
                                                : BbSettings::MenuText("Custom", "Своё"))) {
            for (int i = 0; i < int(std::size(presets)); ++i) {
                if (ImGui::Selectable(BbSettings::MenuText(presets[i].en, presets[i].ru), i == index)) {
                    Store(s.fsr3_velocity_factor, presets[i].velocity, true);
                    Store(s.fsr3_reactiveness_scale, presets[i].reactiveness, true);
                    Store(s.fsr3_shading_change_scale, presets[i].shading, true);
                    Store(s.fsr3_accumulation, presets[i].accumulation, true);
                    Store(s.fsr3_min_disocclusion, presets[i].disocclusion, true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "FSR 3.1's own tuning, as OptiScaler sets it. Steady bright pixels: lamps and sparks shimmer "
            "less in motion. Less ghosting: fewer trails behind moving things, thin details flicker a "
            "little more. Less flicker: thin swaying things (grass, hair) flicker less, a little more "
            "ghosting.",
            "Собственная настройка FSR 3.1, как в OptiScaler. Стабильные яркие пиксели: фонари и искры "
            "меньше мерцают в движении. Меньше шлейфов: меньше следов за движущимся, тонкие детали "
            "мерцают чуть больше. Меньше мерцания: тонкое (трава, волосы) мерцает меньше, шлейфов "
            "чуть больше."));
    }
    if (const char* label = Item("Velocity factor", "Учёт скорости")) {
        Slider(label, s.fsr3_velocity_factor, 0.0f, 1.0f, "%.2f");
    }
    if (const char* label = Item("Reactiveness", "Реактивность")) {
        Slider(label, s.fsr3_reactiveness_scale, 0.0f, 4.0f, "%.2f");
    }
    if (const char* label = Item("Shading change", "Изменение освещения")) {
        Slider(label, s.fsr3_shading_change_scale, 0.0f, 4.0f, "%.2f");
    }
    if (const char* label = Item("Accumulation per frame", "Накопление за кадр")) {
        Slider(label, s.fsr3_accumulation, 0.0f, 1.0f, "%.3f");
    }
    if (const char* label = Item("Disocclusion accumulation", "Накопление при раскрытии")) {
        Slider(label, s.fsr3_min_disocclusion, -1.0f, 1.0f, "%.3f");
    }
}

void MaskSection() {
    auto& s = BbSettings::Get();
    const bool upscaler_on = s.upscaler != BbSettings::UpscalerOff;
    const bool taa = s.upscaler == BbSettings::UpscalerTaa;
    ImGui::BeginDisabled(!upscaler_on);
    Section("Reactive mask");
    ImGui::BeginDisabled(taa);
    if (const char* label = Item("Enable mask")) {
        Checkbox(label, s.reactive);
        Hint(BbSettings::MenuText(
            "Marks transparent effects (particles, haze) so the upscaler relies less on past "
            "frames. Fewer trails behind effects, but shimmer returns underneath them."));
        const char* use = s.upscaler == BbSettings::UpscalerFsr3
                              ? BbSettings::MenuText("FSR 3.1 uses it as its reactive mask.",
                                                     "FSR 3.1 берёт её как свою реактивную маску.")
                          : s.upscaler == BbSettings::UpscalerDlss
                              ? BbSettings::MenuText("DLSS uses it to trust the current frame more (its bias mask).",
                                                     "DLSS по ней больше доверяет текущему кадру (маска смещения).")
                          : BbSettings::IsFsr4(s.upscaler)
                              ? BbSettings::MenuText("FSR 4 has no mask input: the marked pixels are blended with "
                                                     "this frame after it.",
                                                     "У FSR 4 нет входа для маски: отмеченные пиксели смешиваются "
                                                     "с текущим кадром после него.")
                              : BbSettings::MenuText("TAA does not use it.", "TAA её не использует.");
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("%s", use);
        ImGui::PopTextWrapPos();
    }
    ImGui::BeginDisabled(!s.reactive);
    if (const char* label = Item("Scale")) {
        Slider(label, s.reactive_scale, 0.0f, 4.0f);
    }
    if (const char* label = Item("Threshold")) {
        Slider(label, s.reactive_threshold, 0.0f, 1.0f);
    }
    if (const char* label = Item("Maximum")) {
        Slider(label, s.reactive_max, 0.0f, 1.0f);
    }
    if (const char* label = Item("Show mask (debug)")) {
        bool show_mask = s.debug_view == BbSettings::DebugReactive;
        if (ImGui::Checkbox(label, &show_mask)) {
            s.debug_view = show_mask ? BbSettings::DebugReactive : BbSettings::DebugNone;
        }
        Hint(BbSettings::MenuText(
            "Red: how far each pixel stops relying on past frames (Maximum = full red); the rest of "
            "the picture is darkened. Scale makes changes count more, Threshold drops faint ones "
            "(thin mist). Not saved.",
            "Красный: насколько пиксель перестаёт полагаться на прошлые кадры (Maximum — полностью "
            "красный); остальная картинка затемнена. Scale усиливает изменения, Threshold отбрасывает "
            "слабые (лёгкий туман). Не сохраняется."));
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    Section("Motion vectors", "Векторы движения");
    if (const char* label = Item("Character motion vectors", "Векторы движения персонажей")) {
        Checkbox(label, s.object_motion);
        Hint(BbSettings::MenuText(
            "Accurate vectors for animated objects: clothing and weapons break up less "
            "during movement. The static scene does not get an additional pass. "
            "Changes apply after restarting the game.",
            "Точные векторы для анимированных объектов: одежда и оружие меньше рассыпаются "
            "при движении. Статичная сцена не получает дополнительный проход. "
            "Изменение применяется после перезапуска игры."));
    }
    if (const char* label = Item("Show motion vectors (debug)", "Показать векторы движения (отладка)")) {
        bool show_motion = s.debug_view == BbSettings::DebugMotion;
        if (ImGui::Checkbox(label, &show_motion)) {
            s.debug_view = show_motion ? BbSettings::DebugMotion : BbSettings::DebugNone;
        }
        Hint(BbSettings::MenuText(
            "Red/green: horizontal/vertical motion (8 pixels = full brightness). "
            "Blue: the pixel received an accurate object vector, not just camera motion. "
            "The upscaler treats a moving object with no blue or red/green as "
            "stationary, causing trails.",
            "Красный/зелёный: движение по горизонтали/вертикали (8 пикселей = полная яркость). "
            "Синий: пиксель получил точный вектор объекта, а не только движение камеры. "
            "Движущийся предмет без синего и без красного/зелёного апскейлер считает "
            "неподвижным, отсюда шлейф."));
    }
    ImGui::EndDisabled();
}

void OptiScalerSection() {
    auto& s = BbSettings::Get();
    const auto& opti = Vulkan::OptiScaler::Detect();
    Section("OptiScaler");
    if (WholeSection() && Shown("OptiScaler")) {
        ImGui::PushTextWrapPos();
        if (!opti.loaded) {
            ImGui::TextDisabled("%s", BbSettings::MenuText(
                                          "Not loaded. OptiScaler is a DLL put next to the engine (often as winmm.dll "
                                          "or dxgi.dll) with its OptiScaler.ini; it takes over DLSS. The switches below "
                                          "apply when it is there.",
                                          "Не загружен. OptiScaler — это DLL рядом с движком (обычно winmm.dll или "
                                          "dxgi.dll) с файлом OptiScaler.ini; он перехватывает DLSS. Переключатели ниже "
                                          "действуют, когда он есть."));
        } else {
            ImGui::Text(BbSettings::MenuText("Loaded: %s as %s", "Загружен: %s как %s"),
                        opti.version.empty() ? "OptiScaler" : ("v" + opti.version).c_str(), opti.module.c_str());
            ImGui::Text("%s",
                        Vulkan::OptiScaler::Compat()
                            ? BbSettings::MenuText("Compatibility mode in this run: on", "Режим совместимости сейчас: вкл.")
                            : BbSettings::MenuText("Compatibility mode in this run: off", "Режим совместимости сейчас: выкл."));
            if (!opti.frame_generation.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                                   BbSettings::MenuText(
                                       "Its own frame generation is on (%s). If the GPU is lost, set Frame "
                                       "Generation to none in OptiScaler's menu and use Frame generation above.",
                                       "Включена его собственная генерация кадров (%s). Если GPU теряется, "
                                       "выберите none для Frame Generation в меню OptiScaler и используйте "
                                       "генерацию кадров выше."),
                                   opti.frame_generation.c_str());
            }
        }
        ImGui::PopTextWrapPos();
    }
    if (const char* label = Item("Compatibility mode (restart required)", "Режим совместимости (после перезапуска)")) {
        Checkbox(label, s.optiscaler_compat);
        Hint(BbSettings::MenuText(
            "Keeps the engine on the paths OptiScaler's hooks handle: DLSS calls wait for the GPU queue, "
            "the Vulkan device is created where OptiScaler can prepare it, and one thread sends the GPU "
            "work while its frame generation is on (a little FPS). Off: the engine runs as without "
            "OptiScaler. OptiScaler's own settings (its upscaler, its frame generation) are in its own "
            "overlay and OptiScaler.ini. Applies after restarting the game.",
            "Держит движок на путях, которые выдерживают хуки OptiScaler: вызовы DLSS ждут очередь GPU, "
            "устройство Vulkan создаётся там, где OptiScaler может его подготовить, а работу GPU "
            "отправляет один поток, пока включена его генерация кадров (немного FPS). Выкл.: движок "
            "работает как без OptiScaler. Собственные настройки OptiScaler (его апскейлер, его генерация "
            "кадров) — в его оверлее и OptiScaler.ini. Применяется после перезапуска игры."));
        if (const char* env = std::getenv("BB_OPTISCALER_COMPAT"); env && env[0] == '0') {
            ImGui::TextDisabled("%s", BbSettings::MenuText(
                                          "Off for this run: the launcher's Engine > OptiScaler compatibility is off.",
                                          "Выключен в этом запуске: в лаунчере выключено Engine > OptiScaler "
                                          "compatibility."));
        }
    }
    ImGui::BeginDisabled(!s.optiscaler_compat);
    ImGui::Indent();
    if (const char* label = Item("DLSS calls wait for the GPU queue", "Вызовы DLSS ждут очередь GPU")) {
        Checkbox(label, s.optiscaler_ngx_lock);
    }
    if (const char* label = Item("Vulkan device on a thread of its own", "Устройство Vulkan в отдельном потоке")) {
        Checkbox(label, s.optiscaler_clean_thread);
    }
    if (const char* label = Item("One GPU submit thread", "Один поток отправки GPU")) {
        const char* modes[] = {BbSettings::MenuText("With its frame generation only", "Только с его генерацией кадров"),
                               BbSettings::MenuText("Always", "Всегда"), BbSettings::MenuText("Off", "Выкл.")};
        const int current = s.optiscaler_sync_submit;
        int index = current == BbSettings::OptiSubmitOn ? 1 : current == BbSettings::OptiSubmitOff ? 2 : 0;
        if (ImGui::Combo(label, &index, modes, 3)) {
            Store(s.optiscaler_sync_submit,
                  index == 1 ? int(BbSettings::OptiSubmitOn)
                             : index == 2 ? int(BbSettings::OptiSubmitOff) : int(BbSettings::OptiSubmitAuto),
                  true);
        }
        Hint(BbSettings::MenuText(
            "The parts of the compatibility mode, each applied after a restart. Turn one off only to "
            "find which one a crash with OptiScaler comes from, and post your log either way.",
            "Части режима совместимости, каждая применяется после перезапуска. Выключайте их по одной, "
            "только чтобы найти, из-за какой падает игра с OptiScaler, и в любом случае пришлите лог."));
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
}

void OutputSection() {
    auto& s = BbSettings::Get();
    Section("Output resolution", "Разрешение вывода");
    if (const char* label = Item("Output resolution", "Разрешение вывода")) {
        const char* outputs[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160",
            "2560 x 1080", "3440 x 1440", "3840 x 1600", "3840 x 1080", "5120 x 1440", "5120 x 2160",
            "7680 x 2160", BbSettings::MenuText("Display (auto)"), "Custom"};
        static_assert(std::size(outputs) == BbSettings::OutputCount);
        static char custom[48];
        std::snprintf(custom, sizeof custom, "Custom (%d x %d)", s.custom_output_w, s.custom_output_h);
        outputs[BbSettings::OutputCustom] = custom;
        const int output = s.output_res;
        if (ImGui::BeginCombo(label, outputs[output])) {
            for (int i = 0; i < BbSettings::OutputCount; ++i) {
                if (ImGui::Selectable(outputs[i], i == output)) {
                    Store(s.output_res, i, true);
                }
            }
            ImGui::EndCombo();
        }
        if (BbSettings::FixedRenderSession()) {
            Hint(BbSettings::MenuText(
                "Size of the final frame and UI. The preset sets the scene size relative to "
                "the output: 4K Performance = 1920x1080. Applies after restarting the game.",
                "Размер готового кадра и интерфейса. Пресет задаёт размер сцены относительно "
                "вывода: 4K Performance = 1920x1080. Применяется после перезапуска игры."));
        } else {
            Hint(BbSettings::MenuText(
                "The final frame and UI size changes at the next frame boundary. "
                "The preset sets the scene size relative to the output: 4K Performance = 1920x1080. "
                "Changing the size resets FSR history and may cause a brief pause.",
                "Размер готового кадра и интерфейса меняется на границе следующего кадра. "
                "Пресет задаёт размер сцены относительно вывода: 4K Performance = 1920x1080. "
                "Смена размера сбрасывает историю FSR и может вызвать короткую паузу."));
        }
    }
    if (const char* label = Item("Live resolution changes", "Смена разрешения на лету")) {
        const char* live_modes[] = {BbSettings::MenuText("Auto (based on GPU)", "Авто (по видеокарте)"),
                                    BbSettings::MenuText("Off (faster)", "Выключена (быстрее)"),
                                    BbSettings::MenuText("On", "Включена")};
        const int live = s.live_resolution + 1;
        if (ImGui::BeginCombo(label, live_modes[live])) {
            for (int i = 0; i < 3; ++i) {
                if (ImGui::Selectable(live_modes[i], i == live)) {
                    Store(s.live_resolution, i - 1, true);
                }
            }
            ImGui::EndCombo();
        }
        Hint(BbSettings::MenuText(
            "On: output resolution and preset change without restarting, but game post-processing "
            "stays at 1080p, which is noticeably slower on Steam Deck and older GPUs. "
            "Off: everything renders at the preset resolution, and changes require a restart. Auto "
            "enables this on powerful discrete GPUs. Applies after restarting the game.",
            "Включена: разрешение вывода и пресет меняются без перезапуска, но постобработка игры "
            "остаётся в 1080p — на Steam Deck и старых видеокартах это заметно медленнее. "
            "Выключена: всё рисуется в разрешении пресета, смена — через перезапуск. Авто включает "
            "её на мощных дискретных видеокартах. Применяется после перезапуска игры."));
    }
}

void CameraSection() {
    auto& s = BbSettings::Get();
    Section("PC Camera");
    if (WholeSection() && Shown("PC Camera")) {
        ImGui::TextDisabled("%s", BbSettings::MenuText("Enable PC Camera in the launcher before starting; changes here are live."));
    }
    if (const char* label = Item("Camera FOV scale")) {
        Slider(label, s.camera_fov_scale, 1.0f, 1.5f);
    }
    if (const char* label = Item("Camera distance scale")) {
        Slider(label, s.camera_distance_scale, 0.5f, 1.5f);
    }
    if (const char* label = Item("Camera height scale")) {
        Slider(label, s.camera_height_scale, 0.5f, 1.5f);
    }
}

void CheatsSection() {
    auto& s = BbSettings::Get();
    Section("Cheats", "Читы");
    if (const char* label = Item("Blood Vials", "Флаконы крови")) {
        Cheat(label, s.cheat_vials, BbEngine::Cheats::Vials, custom_vials);
    }
    if (const char* label = Item("Quicksilver Bullets", "Ртутные пули")) {
        Cheat(label, s.cheat_bullets, BbEngine::Cheats::Bullets, custom_bullets);
        Hint(BbSettings::MenuText(
            "Changes apply live. Infinite: kept full at the normal carry limit, even from none. Custom: the count is "
            "refilled to the amount and never lowered. Off restores the game's item data and keeps the "
            "counts you have. While a cheat is on, the counts it gives are saved with your character. "
            "Do not play online with cheats on.",
            "Применяются сразу. Бесконечно: запас всегда полный, даже если предметов не было. Своё "
            "количество: запас пополняется до него и никогда не уменьшается. Выкл. возвращает данные "
            "предметов игры, а имеющееся количество остаётся. Пока чит включён, полученные предметы "
            "сохраняются вместе с персонажем. Не играйте по сети с включёнными читами."));
    }
    if (const char* label = Item("Infinite stamina", "Бесконечная выносливость")) {
        CheatToggle(label, s.cheat_stamina, BbEngine::Cheats::Stamina,
                    BbSettings::MenuText("Your stamina stays full (enemies are not affected). Off: nothing to restore.",
                                         "Ваша выносливость всегда полная (на врагов не действует). Выкл.: "
                                         "восстанавливать нечего."));
    }
    if (const char* label = Item("Stealth", "Скрытность")) {
        CheatToggle(label, s.cheat_stealth, BbEngine::Cheats::Stealth,
                    BbSettings::MenuText("Enemies neither see nor hear you: the game's debug flags Player Hide and "
                                         "Player Silence. Off puts the flags back as they were.",
                                         "Враги вас не видят и не слышат: отладочные флаги игры Player Hide и Player "
                                         "Silence. Выкл. возвращает флаги как были."));
    }
    if (const char* label = Item("Firearm range", "Дальность огнестрела")) {
        static const char* const ranges[] = {"Off", "150%", "200%", "300%"};
        const char* const names[] = {BbSettings::MenuText(ranges[0], "Выкл."), ranges[1], ranges[2], ranges[3]};
        CheatLevels(label, s.cheat_gun_range, BbSettings::CheatGunRanges, names, BbEngine::Cheats::GunRange,
                    BbSettings::MenuText(
                        "Bullets of the pistols, rifles, blunderbusses, cannons, Gatling Gun and the trick "
                        "weapons' guns fly farther. Not Rosmarinus, the Flamesprayer, arcane or hunter tools. "
                        "Off restores the game's bullet data.",
                        "Пули пистолетов, ружей, мушкетонов, пушек, пулемёта и огнестрела трансформируемого "
                        "оружия летят дальше. Не Розмарин, огнемёт, тайные и охотничьи инструменты. Выкл. "
                        "возвращает данные пуль игры."));
    }
    if (const char* label = Item("Keep Blood Echoes on death", "Не терять отголоски при смерти")) {
        CheatToggle(label, s.cheat_keep_echoes, BbEngine::Cheats::KeepEchoes,
                    BbSettings::MenuText("Dying keeps your Blood Echoes; the bloodstain holds none. Off restores the "
                                         "game's code. Echoes you kept stay in your save.",
                                         "При смерти отголоски остаются у вас, в пятне крови их нет. Выкл. "
                                         "возвращает код игры. Сохранённые отголоски остаются в сохранении."));
    }
    if (const char* label = Item("Blood Echo multiplier", "Множитель отголосков")) {
        static const char* const multipliers[] = {"x1", "x2", "x5", "x10"};
        const char* const names[] = {BbSettings::MenuText("x1 (off)", "x1 (выкл.)"), multipliers[1], multipliers[2],
                                     multipliers[3]};
        CheatLevels(label, s.cheat_echo_multiplier, BbSettings::CheatEchoMultipliers, names,
                    BbEngine::Cheats::EchoMultiplier,
                    BbSettings::MenuText(
                        "Blood Echoes from enemies and bosses times this. An enemy already loaded may give the "
                        "normal amount until the area loads again. x1 restores the game's data; echoes gained "
                        "stay in your save.",
                        "Отголоски за врагов и боссов умножаются. Уже загруженный враг может дать обычное "
                        "количество, пока область не загрузится заново. x1 возвращает данные игры; полученные "
                        "отголоски остаются в сохранении."));
    }
    if (const char* label = Item("No weapon wear", "Оружие не изнашивается")) {
        CheatToggle(label, s.cheat_no_durability, BbEngine::Cheats::NoDurability,
                    BbSettings::MenuText("Equipped weapons are kept at full durability. Off stops it; durability is "
                                         "saved as it is.",
                                         "Экипированное оружие всегда с полной прочностью. Выкл. прекращает это; "
                                         "прочность сохраняется как есть."));
    }
    if (const char* label = Item("Infinite consumables", "Бесконечные расходники")) {
        CheatToggle(label, s.cheat_consumables, BbEngine::Cheats::Consumables,
                    BbSettings::MenuText(
                        "Molotovs, knives, pebbles, oil urns, antidotes, sedatives, elixirs, papers, Numbing Mist, "
                        "Shining Coins and similar tools are not used up. Not Blood Vials or Quicksilver Bullets "
                        "(above), key items, materials, or Insight and Blood Echo items. Off restores the item data.",
                        "Коктейли Молотова, ножи, камешки, масло, антидоты, успокоительное, эликсиры, бумага, "
                        "онемляющий туман, блестящие монеты и подобные инструменты не расходуются. Не флаконы крови и "
                        "ртутные пули (выше), не ключевые предметы, материалы, предметы озарения и отголосков. Выкл. "
                        "возвращает данные предметов."));
    }
    if (Shown("Blood Echoes", "Отголоски крови")) {
        CheatStats();
    }
}

void MouseSection();

void FpsSection() {
    auto& s = BbSettings::Get();
    Section("FPS counter", "Счётчик FPS");
    if (const char* label = Item("FPS counter in corner", "Счётчик FPS в углу")) {
        Checkbox(label, s.show_fps);
    }
    if (const char* label = Item("Note while shaders build", "Надпись при сборке шейдеров")) {
        Checkbox(label, s.show_compile_note);
        Hint(BbSettings::MenuText(
            "Shows \"Building shaders\" in a corner while the game builds shaders for a place it "
            "shows for the first time. A stutter with this note is shader building and is gone on "
            "the next visit; one without it is loading.",
            "Показывает «Сборка шейдеров» в углу, пока игра собирает шейдеры для места, которое "
            "показывает впервые. Рывок с этой надписью — сборка шейдеров, при следующем посещении "
            "его не будет; без неё — загрузка."));
    }
    if (!s.show_fps && !Searching()) {
        MouseSection();
        return;
    }
    ImGui::Indent();
    ImGui::PushID("fps_counter");
    if (const char* label = Item("Frame rate", "Частота кадров")) {
        Checkbox(label, s.show_fps_rate);
    }
    if (const char* label = Item("Frame time", "Время кадра")) {
        Checkbox(label, s.show_fps_frame_time);
    }
    if (const char* label = Item("Upscaler", "Апскейлер")) {
        Checkbox(label, s.show_fps_upscaler);
    }
    if (const char* label = Item("CPU usage", "Загрузка ЦП")) {
        Checkbox(label, s.show_fps_cpu);
    }
    if (const char* label = Item("GPU usage", "Загрузка ГП")) {
        Checkbox(label, s.show_fps_gpu);
    }
    if (const char* label = Item("Position", "Положение")) {
        const char* positions[] = {BbSettings::MenuText("Top right", "Сверху справа"),
                                   BbSettings::MenuText("Top left", "Сверху слева"),
                                   BbSettings::MenuText("Bottom right", "Снизу справа"),
                                   BbSettings::MenuText("Bottom left", "Снизу слева"),
                                   BbSettings::MenuText("Top centre", "Сверху по центру")};
        static_assert(std::size(positions) == BbSettings::FpsPositionCount);
        int position = std::clamp(s.fps_position.load(), 0, BbSettings::FpsPositionCount - 1);
        if (ImGui::Combo(label, &position, positions, BbSettings::FpsPositionCount)) {
            Store(s.fps_position, position, true);
        }
    }
    if (const char* label = Item("Size", "Размер")) {
        float scale = s.fps_scale;
        if (ImGui::SliderFloat(label, &scale, 0.75f, 2.0f, "%.2fx")) {
            Store(s.fps_scale, scale, true);
        }
    }
    const auto edit_colour = [](const char* label, std::atomic<unsigned>& rgb) {
        const unsigned v = rgb;
        float c[3] = {float((v >> 16) & 0xFF) / 255.0f, float((v >> 8) & 0xFF) / 255.0f,
                      float(v & 0xFF) / 255.0f};
        if (ImGui::ColorEdit3(label, c, ImGuiColorEditFlags_NoInputs)) {
            const auto byte = [](float f) { return unsigned(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
            Store(rgb, (byte(c[0]) << 16) | (byte(c[1]) << 8) | byte(c[2]), true);
        }
    };
    if (const char* label = Item("Text colour", "Цвет текста")) {
        edit_colour(label, s.fps_text_color);
    }
    if (const char* label = Item("Text opacity", "Непрозрачность текста")) {
        float text_opacity = s.fps_text_opacity;
        if (ImGui::SliderFloat(label, &text_opacity, 0.1f, 1.0f, "%.2f")) {
            Store(s.fps_text_opacity, text_opacity, true);
        }
    }
    if (const char* label = Item("Background colour", "Цвет фона")) {
        edit_colour(label, s.fps_bg_color);
    }
    if (const char* label = Item("Background opacity", "Непрозрачность фона")) {
        float opacity = s.fps_bg_opacity;
        if (ImGui::SliderFloat(label, &opacity, 0.0f, 1.0f, "%.2f")) {
            Store(s.fps_bg_opacity, opacity, true);
        }
    }
    if (const char* label = Item("Border", "Рамка")) {
        Checkbox(label, s.fps_border);
    }
    ImGui::PopID();
    ImGui::Unindent();
    MouseSection();
}

void MouseSection() {
    auto& s = BbSettings::Get();
    Section("Mouse", "Мышь");
    if (const char* label = Item("Hide mouse cursor", "Скрывать курсор мыши")) {
        Checkbox(label, s.hide_cursor);
        Hint(BbSettings::MenuText("Hidden 2 seconds after the mouse stops moving; moving it brings it back.",
                                  "Скрывается через 2 секунды после остановки мыши; движение возвращает его."));
    }
}

enum Tab : int { TabUpscaling, TabDisplay, TabPicture, TabGameplay, TabInterface, TabCount };

const char* TabTitle(int tab) {
    switch (tab) {
    case TabUpscaling: return BbSettings::MenuText("Upscaling", "Апскейлинг");
    case TabDisplay: return BbSettings::MenuText("Display", "Экран");
    case TabPicture: return BbSettings::MenuText("Picture", "Изображение");
    case TabGameplay: return BbSettings::MenuText("Gameplay", "Игра");
    default: return BbSettings::MenuText("Interface", "Интерфейс");
    }
}

void DrawTab(int tab) {
    tab_title = TabTitle(tab);
    ImGui::PushID(tab);
    switch (tab) {
    case TabUpscaling:
        UpscalerSection();
        DlssSection();
        Fsr3Section();
        MaskSection();
        OptiScalerSection();
        break;
    case TabDisplay:
        OutputSection();
        HdrSection();
        break;
    case TabPicture:
        PictureSection();
        EffectsSection();
        break;
    case TabGameplay:
        CameraSection();
        CheatsSection();
        break;
    default:
        FpsSection();
        break;
    }
    section_title = nullptr;
    ImGui::PopID();
}

int current_tab = TabUpscaling;
int pending_tab = -1;

void Menu() {
    auto& s = BbSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 pos(viewport->WorkPos.x + 40.0f * base_scale, viewport->WorkPos.y + 40.0f * base_scale);
    if (s.menu_x >= 0.0f && s.menu_y >= 0.0f) {
        const float margin = 80.0f * base_scale;
        pos.x = viewport->WorkPos.x +
                std::clamp(s.menu_x * viewport->WorkSize.x, 0.0f, std::max(viewport->WorkSize.x - margin, 0.0f));
        pos.y = viewport->WorkPos.y +
                std::clamp(s.menu_y * viewport->WorkSize.y, 0.0f, std::max(viewport->WorkSize.y - margin, 0.0f));
    }
    static int last_frame = -2;
    static float window_top = 0.0f;
    const float min_height = 240.0f * base_scale;
    const float bottom = viewport->WorkPos.y + viewport->WorkSize.y - 8.0f * base_scale;
    pos.y = std::min(pos.y, std::max(viewport->WorkPos.y, bottom - min_height));
    const bool appearing = ImGui::GetFrameCount() != last_frame + 1;
    const float top = appearing ? pos.y : window_top;
    const float max_height = std::max(min_height, bottom - top);
    ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f * base_scale, min_height), ImVec2(FLT_MAX, max_height));
    ImGui::SetNextWindowPos(pos, ImGuiCond_Appearing);
    ImGui::SetNextWindowSize(ImVec2(640.0f * base_scale, std::min(max_height, 820.0f * base_scale)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    if (!ImGui::Begin(
            BbSettings::MenuText("Bloodborne - Graphics  (Insert / L3+R3)###bbport_settings",
                                 "Bloodborne — настройки  (Insert / L3+R3)###bbport_settings"),
            &keep_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    last_frame = ImGui::GetFrameCount();
    window_top = ImGui::GetWindowPos().y;
    if (!ImGui::IsWindowAppearing() && viewport->WorkSize.x > 0.0f && viewport->WorkSize.y > 0.0f) {
        const ImVec2 at = ImGui::GetWindowPos();
        const float fx = (at.x - viewport->WorkPos.x) / viewport->WorkSize.x;
        const float fy = (at.y - viewport->WorkPos.y) / viewport->WorkSize.y;
        if (std::abs(at.x - pos.x) >= 1.0f || std::abs(at.y - pos.y) >= 1.0f) {
            s.menu_x = fx;
            s.menu_y = fy;
            dirty = true;
        }
    }

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteGlobal)) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##search", BbSettings::MenuText("Search settings (Ctrl+F)", "Поиск настроек (Ctrl+F)"),
                             search_text, sizeof(search_text), ImGuiInputTextFlags_EscapeClearsAll);
    search_query = Fold(search_text);

    static const char* languages[] = {"English",  "Русский",  "Español", "Português (Brasil)",
                                      "Français", "Deutsch",  "Italiano", "Polski", "Türkçe", "简体中文"};
    static_assert(std::size(languages) == BbSettings::MenuLanguageCount);
    int language = int(s.menu_language.load());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    if (ImGui::Combo(BbSettings::MenuText("Language"), &language, languages, BbSettings::MenuLanguageCount)) {
        s.menu_language =
            BbSettings::MenuLanguage(std::clamp(language, 0, BbSettings::MenuLanguageCount - 1));
        BbSettings::Save();
    }
    ImGui::SameLine();
    ImGui::Text(BbSettings::MenuText("%.0f FPS  (%.1f ms)", "%.0f FPS  (%.1f мс)"),
                frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f, frame_ms_avg);

    if (!Searching()) {
        const bool previous = ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false) ||
                              ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageUp, ImGuiInputFlags_RouteGlobal);
        const bool next = ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false) ||
                          ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageDown, ImGuiInputFlags_RouteGlobal);
        if (previous != next) {
            pending_tab = (current_tab + (next ? 1 : TabCount - 1)) % TabCount;
        }
    }
    static float footer_height = 0.0f;
    const ImVec2 page(0.0f, -std::max(footer_height, ImGui::GetFrameHeightWithSpacing() * 2.0f));
    shown_items = 0;
    if (!Searching()) {
        if (ImGui::BeginTabBar("##tabs")) {
            for (int tab = 0; tab < TabCount; ++tab) {
                const std::string label = std::string(TabTitle(tab)) + "###tab" + std::to_string(tab);
                const ImGuiTabItemFlags flags = tab == pending_tab ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
                    current_tab = tab;
                    if (ImGui::BeginChild("##page", page, ImGuiChildFlags_NavFlattened)) {
                        DrawTab(tab);
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        pending_tab = -1;
    } else {
        if (ImGui::BeginChild("##results", page, ImGuiChildFlags_NavFlattened)) {
            for (int tab = 0; tab < TabCount; ++tab) {
                DrawTab(tab);
            }
            if (shown_items == 0) {
                ImGui::TextDisabled(BbSettings::MenuText("Nothing matches \"%s\"", "Ничего не найдено по «%s»"),
                                    search_text);
            }
        }
        ImGui::EndChild();
    }

    const float footer_top = ImGui::GetCursorPosY();
    bool restart =
        s.object_motion != s.startup_object_motion || s.model_lod != s.startup_model_lod ||
        s.live_resolution != s.startup_live_resolution || s.hdr != s.startup_hdr ||
        BbSettings::ResolutionNeedsRestart();
    for (int e = 0; e < BbSettings::EffectCount; ++e) {
        restart |= s.effects[e] != s.startup_effects[e];
    }
    restart |= Vulkan::OptiScaler::Detect().loaded &&
               (s.optiscaler_compat != s.startup_optiscaler.compat ||
                s.optiscaler_ngx_lock != s.startup_optiscaler.ngx_lock ||
                s.optiscaler_clean_thread != s.startup_optiscaler.clean_thread ||
                s.optiscaler_sync_submit != s.startup_optiscaler.sync_submit);
    if (restart) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s",
                           BbSettings::MenuText("Changes apply after restarting the game",
                                                "Изменения применятся после перезапуска игры"));
        ImGui::SameLine();
        if (ImGui::Button(
                BbSettings::MenuText("Apply and restart game", "Применить и перезапустить игру"))) {
            BbSettings::Save();
            runtime_restart();
        }
    }
    if (ImGui::Button(BbSettings::MenuText("Close", "Закрыть"))) {
        keep_open = false;
    }
    static bool quit_armed = false;
    ImGui::SameLine();
    const std::string quit_label =
        std::string(quit_armed ? BbSettings::MenuText("Press again to quit", "Нажмите ещё раз")
                               : BbSettings::MenuText("Quit game", "Выйти из игры")) + "###quit";
    if (ImGui::Button(quit_label.c_str())) {
        if (quit_armed) {
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
        }
        quit_armed = !quit_armed;
    }
    if (quit_armed && !ImGui::IsItemHovered() && !ImGui::IsItemFocused()) {
        quit_armed = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", BbSettings::MenuText("Settings are saved to lantern.ini",
                                             "Настройки сохраняются в lantern.ini"));
    ImGui::PushTextWrapPos();
    ImGui::TextDisabled("%s", BbSettings::MenuText(
                                  "Tabs: L2 / R2 or Ctrl+PgUp / Ctrl+PgDn. Search: Ctrl+F, Esc clears it. "
                                  "A setting's (?) also shows while it is selected.",
                                  "Вкладки: L2 / R2 или Ctrl+PgUp / Ctrl+PgDn. Поиск: Ctrl+F, Esc очищает. "
                                  "Подсказка (?) видна и когда настройка выбрана."));
    ImGui::PopTextWrapPos();
    footer_height = ImGui::GetCursorPosY() - footer_top + ImGui::GetStyle().ItemSpacing.y;
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

ImVec2 fps_box_min{}, fps_box_max{};

std::mutex compile_note_mutex;
bool compile_note_primed = false;
u64 compile_note_seen_ns = 0, compile_note_burst_ns = 0;
u32 compile_note_seen = 0, compile_note_burst = 0;
std::chrono::steady_clock::time_point compile_note_last{};

bool CompileNoteActive() {
    if (!BbSettings::Get().show_compile_note) {
        return false;
    }
    const u64 ns = Vulkan::g_bb_compile_ns_total.load(std::memory_order_relaxed);
    const u32 count = Vulkan::g_bb_compiles_total.load(std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    std::scoped_lock lock{compile_note_mutex};
    if (!compile_note_primed) {
        compile_note_primed = true;
        compile_note_seen_ns = ns;
        compile_note_seen = count;
        return false;
    }
    if (ns != compile_note_seen_ns) {
        if (now - compile_note_last > std::chrono::seconds(2)) {
            compile_note_burst_ns = 0;
            compile_note_burst = 0;
        }
        compile_note_burst_ns += ns - compile_note_seen_ns;
        compile_note_burst += count - compile_note_seen;
        compile_note_seen_ns = ns;
        compile_note_seen = count;
        compile_note_last = now;
    }
    return compile_note_burst_ns >= 30'000'000ull &&
           now - compile_note_last < std::chrono::milliseconds(1500);
}

void CompileNote() {
    u32 burst;
    {
        std::scoped_lock lock{compile_note_mutex};
        burst = compile_note_burst;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const bool beside_counter = fps_box_max.x > fps_box_min.x;
    const bool bottom = beside_counter && fps_box_min.y > viewport->WorkPos.y + viewport->WorkSize.y * 0.5f;
    const float x = beside_counter ? fps_box_max.x : viewport->WorkPos.x + viewport->WorkSize.x - pad;
    const float y = !beside_counter ? viewport->WorkPos.y + pad
                    : bottom        ? fps_box_min.y - pad * 0.5f
                                    : fps_box_max.y + pad * 0.5f;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always, ImVec2(1.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##compile_note", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::Text("%s (%u)", BbSettings::MenuText("Building shaders", "Сборка шейдеров"), burst);
    ImGui::End();
}

void FpsCounter() {
    const auto& s = BbSettings::Get();
    char text[128];
    int n = 0;
    if (s.show_fps_rate) {
        n += std::snprintf(text + n, sizeof(text) - n, "%.0f FPS",
                           frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f);
        if (s.frame_gen_active) {
            n += std::snprintf(text + n, sizeof(text) - n, " (game %.0f, frame gen 2x)",
                               s.frame_gen_game_fps.load());
        }
    }
    if (s.show_fps_frame_time) {
        n += std::snprintf(text + n, sizeof(text) - n, "%s%.1f ms", n ? "  " : "", frame_ms_avg);
    }
    if (s.show_fps_upscaler) {
        n += std::snprintf(text + n, sizeof(text) - n, "%s%s", n ? "  " : "",
                           BbSettings::UpscalerName(s.upscaler));
    }
    if (s.show_fps_cpu || s.show_fps_gpu) {
        const BbUsage::Sample use = BbUsage::Get(s.show_fps_cpu, s.show_fps_gpu);
        if (s.show_fps_cpu && use.cpu >= 0.0f) {
            n += std::snprintf(text + n, sizeof(text) - n, "%sCPU %.0f%%", n ? "  " : "", use.cpu);
        }
        if (s.show_fps_gpu && use.gpu >= 0.0f) {
            n += std::snprintf(text + n, sizeof(text) - n, "%sGPU %.0f%%", n ? "  " : "", use.gpu);
        }
    }
    if (n == 0) {
        return;
    }
    static char shown[sizeof(text)] = "";
    static auto shown_at = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (shown[0] == '\0' || now - shown_at >= std::chrono::milliseconds(500)) {
        std::memcpy(shown, text, sizeof(text));
        shown_at = now;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int position = std::clamp(s.fps_position.load(), 0, BbSettings::FpsPositionCount - 1);
    const bool right = position == BbSettings::FpsTopRight || position == BbSettings::FpsBottomRight;
    const bool bottom = position == BbSettings::FpsBottomRight || position == BbSettings::FpsBottomLeft;
    const bool centre = position == BbSettings::FpsTopCentre;
    const float x = centre  ? viewport->WorkPos.x + viewport->WorkSize.x * 0.5f
                    : right ? viewport->WorkPos.x + viewport->WorkSize.x - pad
                            : viewport->WorkPos.x + pad;
    const float y = bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad;
    ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always,
                            ImVec2(centre ? 0.5f : right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    static float min_width = 0.0f;
    static size_t min_width_key = 0;
    const size_t width_key = size_t(s.show_fps_rate) | size_t(s.show_fps_frame_time) << 1 |
                             size_t(s.show_fps_upscaler) << 2 | size_t(s.show_fps_cpu) << 3 |
                             size_t(s.show_fps_gpu) << 4 | size_t(s.frame_gen_active) << 5 |
                             size_t(s.upscaler) << 6 | size_t(s.fps_scale * 100.0f) << 12;
    if (width_key != min_width_key) {
        min_width = 0.0f;
        min_width_key = width_key;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(min_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    const auto colour = [](unsigned rgb, float alpha) {
        return ImVec4(float((rgb >> 16) & 0xFF) / 255.0f, float((rgb >> 8) & 0xFF) / 255.0f,
                      float(rgb & 0xFF) / 255.0f, alpha);
    };
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colour(s.fps_bg_color, s.fps_bg_opacity));
    ImGui::PushStyleColor(ImGuiCol_Text, colour(s.fps_text_color, std::clamp(s.fps_text_opacity.load(), 0.1f, 1.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, s.fps_border ? 1.0f : 0.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(std::clamp(s.fps_scale.load(), 0.75f, 2.0f));
    ImGui::TextUnformatted(shown);
    min_width = std::max(min_width, ImGui::GetWindowWidth());
    fps_box_min = ImGui::GetWindowPos();
    fps_box_max = ImVec2(fps_box_min.x + ImGui::GetWindowWidth(), fps_box_min.y + ImGui::GetWindowHeight());
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

void GameBar(ImDrawList* draw, ImVec2 at, ImVec2 size, float fill, ImU32 light, ImU32 dark, float dim, float k) {
    const auto faded = [dim](ImU32 c) {
        const ImU32 a = ImU32(float((c >> IM_COL32_A_SHIFT) & 0xFF) * dim);
        return (c & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
    };
    const ImVec2 end(at.x + size.x, at.y + size.y);
    draw->AddRectFilled(ImVec2(at.x - k, at.y - k), ImVec2(end.x + k, end.y + k), IM_COL32(0, 0, 0, 170));
    draw->AddRectFilled(at, end, IM_COL32(24, 14, 12, 210));
    if (fill > 0.0f) {
        const ImVec2 fill_end(at.x + size.x * fill, end.y);
        draw->AddRectFilledMultiColor(at, fill_end, faded(light), faded(light), faded(dark), faded(dark));
        draw->AddLine(ImVec2(at.x, at.y + 0.5f * k), ImVec2(fill_end.x, at.y + 0.5f * k), faded(IM_COL32(255, 220, 200, 70)),
                      k);
    }
    draw->AddRect(ImVec2(at.x - k, at.y - k), ImVec2(end.x + k, end.y + k), IM_COL32(168, 156, 136, int(200 * dim)), 0.0f,
                  0, k);
    draw->AddRectFilled(ImVec2(end.x, at.y - 2.0f * k), ImVec2(end.x + 2.0f * k, end.y + 2.0f * k),
                        IM_COL32(200, 190, 170, int(220 * dim)));
}

void PartyHud(const std::vector<BbEngine::OnlineFixes::PartyRow>& rows) {
    static const ImU32 accents[] = {IM_COL32(196, 160, 92, 255), IM_COL32(92, 160, 170, 255),
                                    IM_COL32(176, 78, 64, 255), IM_COL32(140, 110, 180, 255)};
    const float k = base_scale;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float left = viewport->WorkPos.x + 40.0f * k;
    const float card_w = 330.0f * k, card_h = 62.0f * k, gap = 8.0f * k;
    float top = viewport->WorkPos.y + 240.0f * k;
    ImFont* font = ImGui::GetFont();
    const float name_size = ImGui::GetFontSize() * 1.05f;
    const float small_size = ImGui::GetFontSize() * 0.8f;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        const ImU32 accent = accents[i % std::size(accents)];
        const float dim = row.down ? 0.45f : 1.0f;
        const ImVec2 a(left, top), b(left + card_w, top + card_h);
        draw->AddRectFilledMultiColor(a, b, IM_COL32(8, 12, 22, 150), IM_COL32(8, 12, 22, 0), IM_COL32(8, 12, 22, 0),
                                      IM_COL32(8, 12, 22, 150));
        draw->AddRectFilled(a, ImVec2(left + 2.0f * k, top + card_h), accent);
        const ImVec2 badge(left + 32.0f * k, top + card_h * 0.5f);
        const float radius = 20.0f * k;
        draw->AddCircleFilled(badge, radius, IM_COL32(18, 22, 34, 220), 32);
        draw->AddCircle(badge, radius, accent, 32, 1.5f * k);
        char initial[8] = {row.name.empty() ? '?' : char(std::toupper(static_cast<unsigned char>(row.name[0]))), 0};
        const ImVec2 initial_size = font->CalcTextSizeA(name_size * 1.1f, FLT_MAX, 0.0f, initial);
        draw->AddText(font, name_size * 1.1f, ImVec2(badge.x - initial_size.x * 0.5f, badge.y - initial_size.y * 0.5f),
                      IM_COL32(230, 222, 205, int(255 * dim)), initial);
        const float text_x = left + 64.0f * k;
        const float bar_w = 240.0f * k;
        draw->AddText(font, name_size, ImVec2(text_x, top + 8.0f * k), IM_COL32(232, 224, 206, int(255 * dim)),
                      row.name.c_str());
        char where[32] = "";
        if (row.down) {
            std::snprintf(where, sizeof where, "%s", BbSettings::MenuText("down", "повержен"));
        } else if (row.distance >= 0.0f) {
            std::snprintf(where, sizeof where, "%.0f m", row.distance);
        }
        const float arrow_room = row.distance >= 0.0f && !row.down ? 16.0f * k : 0.0f;
        if (where[0]) {
            const ImVec2 size = font->CalcTextSizeA(small_size, FLT_MAX, 0.0f, where);
            draw->AddText(font, small_size, ImVec2(text_x + bar_w - arrow_room - size.x, top + 11.0f * k),
                          row.down ? IM_COL32(200, 90, 70, 255) : IM_COL32(200, 196, 186, 220), where);
        }
        if (arrow_room > 0.0f) {
            const ImVec2 c(text_x + bar_w - 6.0f * k, top + 11.0f * k + small_size * 0.5f);
            const ImVec2 d(std::sin(row.bearing), -std::cos(row.bearing));
            const ImVec2 p(-d.y, d.x);
            const float tip = 6.0f * k, back = 4.0f * k, wide = 4.0f * k;
            draw->AddTriangleFilled(ImVec2(c.x + d.x * tip, c.y + d.y * tip),
                                    ImVec2(c.x - d.x * back + p.x * wide, c.y - d.y * back + p.y * wide),
                                    ImVec2(c.x - d.x * back - p.x * wide, c.y - d.y * back - p.y * wide),
                                    IM_COL32(220, 214, 200, 230));
        }
        const float hp_y = top + 34.0f * k, hp_h = 7.0f * k;
        GameBar(draw, ImVec2(text_x, hp_y), ImVec2(bar_w, hp_h), row.health, IM_COL32(196, 46, 40, 255),
                IM_COL32(112, 16, 16, 255), dim, k);
        const float sp_y = hp_y + hp_h + 4.0f * k, sp_h = 5.0f * k;
        GameBar(draw, ImVec2(text_x, sp_y), ImVec2(bar_w * 0.8f, sp_h), row.stamina, IM_COL32(112, 172, 88, 255),
                IM_COL32(46, 98, 42, 255), dim, k);
        top += card_h + gap;
    }
}

void TextPrompt() {
    std::string title, text;
    {
        std::scoped_lock lock{prompt_mutex};
        title = prompt_title;
        text = prompt_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##textprompt", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();
    ImGui::Text("%s_", text.c_str());
    ImGui::Separator();
    ImGui::TextUnformatted(BbSettings::MenuText("Keyboard: type, Backspace = delete, Enter = OK, Esc = cancel"));
    ImGui::End();
}

}

void SetTextPrompt(bool active, const std::string& prompt, const std::string& text) {
    {
        std::scoped_lock lock{prompt_mutex};
        prompt_title = prompt;
        prompt_text = text;
    }
    prompt_active = active;
}

enum class OverlayKeys { Both, Insert, Off };
static OverlayKeys MenuKeys() {
    static const OverlayKeys keys = [] {
        const char* env = std::getenv("BB_OVERLAY_KEYS");
        const std::string value = env ? env : "";
        return value == "off" ? OverlayKeys::Off : value == "insert" ? OverlayKeys::Insert : OverlayKeys::Both;
    }();
    return keys;
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigDebugHighlightIdConflicts = false;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "lantern";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f;

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_ttf),
                                   int(bb_font_ttf_end - bb_font_ttf), 18.0f, &font_config);
    ImFontConfig cjk_config;
    cjk_config.FontDataOwnedByAtlas = false;
    cjk_config.MergeMode = true;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(bb_font_cjk),
                                   int(bb_font_cjk_end - bb_font_cjk), 18.0f, &cjk_config);

    const vk::Instance vk_instance = instance.GetInstance();
    ImGui_ImplVulkan_LoadFunctions(
        instance.ApiVersion(),
        [](const char* name, void* user) {
            return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(
                *static_cast<const vk::Instance*>(user), name);
        },
        const_cast<vk::Instance*>(&vk_instance));

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = instance.ApiVersion();
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    info.DescriptorPoolSize = 16;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    initialized = true;
    std::printf("Overlay: menu ready (%s)\n", MenuKeys() == OverlayKeys::Both     ? "Insert or L3+R3"
                                             : MenuKeys() == OverlayKeys::Insert ? "Insert; L3+R3 off"
                                                                                 : "shortcuts off");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (down && !event.key.repeat &&
            ((event.key.key == SDLK_INSERT && (is_open || MenuKeys() != OverlayKeys::Off)) ||
             (is_open && event.key.key == SDLK_ESCAPE && !io.WantTextInput))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        if (button == SDL_GAMEPAD_BUTTON_LEFT_STICK) {
            l3_down = down;
        } else if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
        }
        if (down && l3_down && r3_down && (is_open || MenuKeys() == OverlayKeys::Both)) {
            SetOpen(!is_open);
            return true;
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!is_open) {
            return false;
        }
        const u8 axis = event.gaxis.axis;
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            const float value = std::clamp(float(event.gaxis.value) / 32767.0f, 0.0f, 1.0f);
            io.AddKeyAnalogEvent(axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? ImGuiKey_GamepadL2 : ImGuiKey_GamepadR2,
                                 value > 0.5f, value);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: {
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

bool Visible() {
    return initialized && (menu_open || prompt_active || BbSettings::Get().show_fps || CompileNoteActive() ||
                           !BbEngine::OnlineFixes::PartyRows().empty());
}

bool MenuOpen() {
    return menu_open;
}

bool CapturesInput() {
    return menu_open || prompt_active;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    if (menu_open) {
        Menu();
    }
    fps_box_min = fps_box_max = ImVec2{};
    if (BbSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (!menu_open && CompileNoteActive()) {
        CompileNote();
    }
    if (prompt_active && !menu_open) {
        TextPrompt();
    }
    if (!menu_open) {
        const auto party = BbEngine::OnlineFixes::PartyRows();
        if (!party.empty()) PartyHud(party);
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

}
