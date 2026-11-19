// Portable by default. With portable.txt in the game's folder, the settings,
// saves, mods and logs go in that folder (recompui::file::get_app_folder_path);
// without it, in the user's folders (%LOCALAPPDATA%\snprecomp on Windows,
// ~/.config/snprecomp on Linux). A new install now gets portable.txt; one
// that already has a settings folder in the user's folders keeps using it.
// Players switch by adding or deleting portable.txt. Not on Android, whose
// app folder is fixed.
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "SDL.h"

#include "util/file.h"

#include "snp_command_line.h"

void snp::portable::use_game_folder() {
#if !defined(__ANDROID__)
    std::error_code ec;
    if (std::filesystem::is_directory("assets", ec)) {
        return;
    }
    char* base = SDL_GetBasePath();
    if (base == nullptr) {
        return;
    }
    std::filesystem::path exe_dir = std::filesystem::u8path(base);
    SDL_free(base);
    if (std::filesystem::is_directory(exe_dir / "assets", ec)) {
        std::filesystem::current_path(exe_dir, ec);
    }
#endif
}

void snp::portable::choose_default() {
#if !defined(__ANDROID__) && !defined(RECOMP_FLATPAK)
    std::error_code ec;
    if (std::filesystem::exists("portable.txt", ec)) {
        return;
    }
#if !defined(_WIN32)
    // A folder chosen on purpose.
    if (std::getenv("APP_FOLDER_PATH") != nullptr) {
        return;
    }
#endif
    // Without portable.txt this is the user-folder location.
    const std::filesystem::path user_folder = recompui::file::get_app_folder_path();
    if (user_folder.empty() || std::filesystem::exists(user_folder, ec)) {
        return;
    }
    std::ofstream file("portable.txt");
    if (file) {
        file << "This file keeps Sin and Punishment Recompiled's settings, saves and mods in this folder.\n"
                "Delete it to keep them in your user folder instead.\n";
    }
    // A folder the game can't write to (Program Files, say) keeps the user folder.
#endif
}
