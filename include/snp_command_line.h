#pragma once

#include <string>
#include <vector>

// Command line options (src/main/command_line.cpp, README's "Command line").
namespace snp::command_line {
    // Applies the options this game handles (settings, --rom, --game's short
    // names; --help and --list-settings print and exit) and returns the
    // arguments to give the runtime. Call after recompui::config::finalize()
    // and after the games are registered.
    std::vector<std::string> process(int argc, char** argv);
}

// Portable by default (src/main/command_line.cpp's sibling, src/main/portable.cpp).
namespace snp::portable {
    // Desktop: when started from elsewhere, works from the folder with
    // assets/ in it (the exe's), so portable.txt, saves and logs are found
    // there. Call first thing in main.
    void use_game_folder();
    // Desktop: a new install (no portable.txt, and no settings folder in the
    // user's folders yet) gets portable.txt, so it keeps everything in the
    // game's folder. Existing installs are left as they are. Call after the
    // program ID is set, before the config path is registered.
    void choose_default();
}
