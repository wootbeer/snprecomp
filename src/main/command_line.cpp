// Command line options (README's "Command line"), handled before the runtime
// starts. `--help` lists them; `--list-settings` lists every setting.
//
//   --game <jp|en|id>      straight into the game (the runtime's own --game,
//                          with jp and en for the two ROMs)
//   --rom <file>           loads that ROM, as Load ROM does, and starts it
//   --set <tab.setting=value>  changes a setting and saves it, as the menu
//                          does (repeatable); the tab can be left out when
//                          the setting's name is only on one tab
//   --fullscreen, --windowed   the Graphics tab's window mode
//   --list-settings, --help    print and exit
//
// Everything else (the runtime's --game-mode) is passed on.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

#include "SDL.h"

#include "librecomp/config.hpp"
#include "librecomp/game.hpp"
#include "recompui/config.h"

#include "snp_command_line.h"

extern std::vector<recomp::GameEntry> supported_games;

namespace {
    using recomp::config::Config;
    using recomp::config::ConfigOption;
    using recomp::config::ConfigOptionType;
    using recomp::config::ConfigValueVariant;

    // The settings tabs, in the menu's order (Android's Touch tab included there).
    std::vector<std::string> config_ids() {
        std::vector<std::string> ids = { "general", "graphics", "sound", "enhancements", "cheats" };
#if defined(__ANDROID__)
        ids.push_back("touch_controls");
#endif
        return ids;
    }

    Config* find_config(const std::string& id) {
        try {
            return &recompui::config::get_config(id);
        }
        catch (...) {
            return nullptr;
        }
    }

    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    // Letters and digits only, lower case: "Game's 2-Player Mode" and "games_2_player_mode" match.
    std::string simple(const std::string& s) {
        std::string out;
        for (unsigned char c : s) {
            if (std::isalnum(c)) {
                out += (char)std::tolower(c);
            }
        }
        return out;
    }

    // Windows release builds have no console: print to the one they were started from, if any.
    void attach_console() {
#if defined(_WIN32)
        static bool attached = false;
        if (!attached && AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
            std::printf("\n");
        }
        attached = true;
#endif
    }

    std::string value_text(const ConfigOption& option, const ConfigValueVariant& value) {
        switch (option.type) {
            case ConfigOptionType::Bool:
                return std::holds_alternative<bool>(value) && std::get<bool>(value) ? "on" : "off";
            case ConfigOptionType::Enum: {
                if (!std::holds_alternative<uint32_t>(value)) return "?";
                const auto& e = std::get<recomp::config::ConfigOptionEnum>(option.variant);
                auto it = e.find_option_from_value(std::get<uint32_t>(value));
                return it != e.options.end() ? it->key : std::to_string(std::get<uint32_t>(value));
            }
            case ConfigOptionType::Number: {
                if (!std::holds_alternative<double>(value)) return "?";
                char text[32];
                std::snprintf(text, sizeof(text), "%g", std::get<double>(value));
                return text;
            }
            case ConfigOptionType::String:
                return std::holds_alternative<std::string>(value) ? std::get<std::string>(value) : "?";
            default:
                return "?";
        }
    }

    std::string allowed_text(const ConfigOption& option) {
        switch (option.type) {
            case ConfigOptionType::Bool:
                return "on or off";
            case ConfigOptionType::Enum: {
                std::string text;
                for (const auto& o : std::get<recomp::config::ConfigOptionEnum>(option.variant).options) {
                    text += (text.empty() ? "" : ", ") + o.key;
                }
                return text;
            }
            case ConfigOptionType::Number: {
                const auto& n = std::get<recomp::config::ConfigOptionNumber>(option.variant);
                char text[64];
                std::snprintf(text, sizeof(text), "%g to %g%s", n.min, n.max, n.percent ? " (percent)" : "");
                return text;
            }
            case ConfigOptionType::String:
                return "text";
            default:
                return "";
        }
    }

    // The value for an option from its text, or false with the reason.
    bool parse_value(const ConfigOption& option, const std::string& text, ConfigValueVariant& out, std::string& error) {
        const std::string t = lower(text);
        switch (option.type) {
            case ConfigOptionType::Bool:
                if (t == "on" || t == "true" || t == "yes" || t == "1") { out = true; return true; }
                if (t == "off" || t == "false" || t == "no" || t == "0") { out = false; return true; }
                error = "use on or off";
                return false;
            case ConfigOptionType::Enum: {
                const auto& e = std::get<recomp::config::ConfigOptionEnum>(option.variant);
                for (const auto& o : e.options) {
                    if (simple(o.key) == simple(text) || simple(o.name) == simple(text)) {
                        out = o.value;
                        return true;
                    }
                }
                error = "use one of: " + allowed_text(option);
                return false;
            }
            case ConfigOptionType::Number: {
                std::string number = text;
                if (!number.empty() && number.back() == '%') number.pop_back();
                char* end = nullptr;
                double v = std::strtod(number.c_str(), &end);
                if (number.empty() || end == nullptr || *end != '\0' || !std::isfinite(v)) {
                    error = "use a number, " + allowed_text(option);
                    return false;
                }
                const auto& n = std::get<recomp::config::ConfigOptionNumber>(option.variant);
                if (n.max > n.min) {
                    v = std::clamp(v, n.min, n.max);
                }
                out = v;
                return true;
            }
            case ConfigOptionType::String:
                out = text;
                return true;
            default:
                error = "this setting can't be set from the command line";
                return false;
        }
    }

    struct Found {
        Config* config = nullptr;
        std::string tab;
        const ConfigOption* option = nullptr;
    };

    // "tab.setting" or a setting's id alone, if only one tab has it. Hidden settings aren't found.
    bool find_setting(const std::string& name, Found& found, std::string& error) {
        std::string tab, id = name;
        if (auto dot = name.find('.'); dot != std::string::npos) {
            tab = lower(name.substr(0, dot));
            id = name.substr(dot + 1);
        }
        std::vector<Found> matches;
        for (const std::string& config_id : config_ids()) {
            if (!tab.empty() && tab != config_id) {
                continue;
            }
            Config* config = find_config(config_id);
            if (config == nullptr) {
                continue;
            }
            for (const ConfigOption& option : config->get_config_schema().options) {
                if (!option.hidden && lower(option.id) == lower(id)) {
                    matches.push_back({ config, config_id, &option });
                }
            }
        }
        if (matches.empty()) {
            error = "no setting \"" + name + "\" (--list-settings lists them)";
            return false;
        }
        if (matches.size() > 1) {
            error = "\"" + name + "\" is on more than one tab; write it as";
            for (const Found& m : matches) error += " " + m.tab + "." + m.option->id;
            return false;
        }
        found = matches.front();
        return true;
    }

    void print_help() {
        attach_console();
        std::printf(
            "Sin and Punishment Recompiled\n"
            "\n"
            "  --game <jp|en|snp>       Start the game straight away: jp the Japanese ROM, en the\n"
            "                           translation, snp whichever is loaded. Also the full IDs\n"
            "                           snp.n64.jp.1.0 and snp.n64.en.fan.\n"
            "  --rom <file>             Load that ROM (as Load ROM does) and start it.\n"
            "  --set <tab.setting=value>\n"
            "                           Change a setting and save it, as the menu does. Repeat for\n"
            "                           more. The tab can be left out when only one tab has the\n"
            "                           setting. Example: --set graphics.wm_option=fullscreen\n"
            "  --fullscreen, --windowed The Graphics tab's window mode.\n"
            "  --list-settings          List every setting, its values and its current value.\n"
            "  --help                   This list.\n"
            "\n"
            "Settings and saves go in the game's folder when portable.txt is there (the default\n"
            "for new installs); delete it to use the user folder instead.\n");
        std::fflush(stdout);
    }

    void print_settings() {
        attach_console();
        for (const std::string& config_id : config_ids()) {
            Config* config = find_config(config_id);
            if (config == nullptr) {
                continue;
            }
            std::printf("[%s]\n", config_id.c_str());
            for (const ConfigOption& option : config->get_config_schema().options) {
                if (option.hidden) {
                    continue;
                }
                std::printf("  %s.%s = %s\n      %s%s%s\n", config_id.c_str(), option.id.c_str(),
                    value_text(option, config->get_option_value(option.id)).c_str(),
                    option.name.c_str(), option.name.empty() ? "" : ": ", allowed_text(option).c_str());
            }
        }
        std::fflush(stdout);
    }

    std::string game_alias(const std::string& name) {
        const std::string n = lower(name);
        if (n == "jp" || n == "japanese" || n == "jpn") return "snp.n64.jp.1.0";
        if (n == "en" || n == "english" || n == "translation") return "snp.n64.en.fan";
        return name;
    }

    // Loads a ROM as the launcher's Load ROM does: the game ID it was stored for, or empty.
    std::string load_rom(const std::string& path, std::string& error) {
        bool right_game_wrong_version = false;
        for (const recomp::GameEntry& game : supported_games) {
            switch (recomp::select_rom(std::filesystem::u8path(path), game.game_id)) {
                case recomp::RomValidationError::Good:
                    return std::string(game.game_id.begin(), game.game_id.end());
                case recomp::RomValidationError::FailedToOpen:
                    error = "couldn't open the ROM " + path;
                    return "";
                case recomp::RomValidationError::NotARom:
                    error = path + " isn't a ROM";
                    return "";
                case recomp::RomValidationError::IncorrectVersion:
                    right_game_wrong_version = true;
                    break;
                default:
                    break;
            }
        }
        error = right_game_wrong_version
            ? path + " is Sin and Punishment, but not the Japanese release's code"
            : path + " isn't Sin and Punishment";
        return "";
    }
}

std::vector<std::string> snp::command_line::process(int argc, char** argv) {
    std::vector<std::string> passed = { argc > 0 ? argv[0] : "SnPRecompiled" };
    std::vector<std::string> errors;
    std::set<Config*> changed;
    std::string game;
    bool list = false;

    auto set = [&](const std::string& assignment) {
        auto eq = assignment.find('=');
        if (eq == std::string::npos) {
            errors.push_back("--set " + assignment + ": write it as tab.setting=value");
            return;
        }
        Found found;
        std::string error;
        ConfigValueVariant value;
        if (!find_setting(assignment.substr(0, eq), found, error) ||
            !parse_value(*found.option, assignment.substr(eq + 1), value, error)) {
            errors.push_back("--set " + assignment + ": " + error);
            return;
        }
        found.config->set_option_value(found.option->id, value);
        changed.insert(found.config);
    };

    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 < argc) return argv[++i];
            errors.push_back(arg + " needs " + what);
            return nullptr;
        };
        if (arg == "--help" || arg == "-h" || arg == "/?") {
            print_help();
            std::exit(EXIT_SUCCESS);
        }
        else if (arg == "--list-settings") {
            list = true; // once the --set options are in
        }
        else if (arg == "--set") {
            if (const char* a = next("tab.setting=value")) set(a);
        }
        else if (arg.rfind("--set=", 0) == 0) {
            set(arg.substr(6));
        }
        else if (arg == "--fullscreen") {
            set("graphics.wm_option=fullscreen");
        }
        else if (arg == "--windowed") {
            set("graphics.wm_option=windowed");
        }
        else if (arg == "--game") {
            if (const char* g = next("a game (jp, en or snp)")) game = game_alias(g);
        }
        else if (arg == "--rom") {
            if (const char* path = next("a ROM file")) {
                std::string error;
                std::string id = load_rom(path, error);
                if (id.empty()) {
                    errors.push_back("--rom: " + error);
                }
                else {
                    game = id;
                }
            }
        }
        else {
            passed.push_back(arg);
        }
    }

    for (Config* config : changed) {
        config->save_config();
    }
    if (!game.empty()) {
        passed.push_back("--game");
        passed.push_back(game);
    }

    if (list) {
        for (const std::string& e : errors) {
            attach_console();
            std::fprintf(stderr, "%s\n", e.c_str());
        }
        print_settings();
        std::exit(EXIT_SUCCESS);
    }
    if (!errors.empty()) {
        attach_console();
        std::string message = "Some command line options weren't used:\n";
        for (const std::string& e : errors) {
            std::fprintf(stderr, "%s\n", e.c_str());
            message += "\n" + e;
        }
        message += "\n\n--help lists the options, --list-settings the settings.";
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Sin and Punishment Recompiled", message.c_str(), nullptr);
    }
    return passed;
}
