#pragma once

// The menu's Restart button (src/main/restart.cpp).
namespace snp::restart {
    // Gives the frontend a restart callback (Windows and Linux; not Android).
    // Call before recomp::start with main's arguments.
    void install(int argc, char** argv);
    // If Restart was chosen, starts the game again. Call right before the
    // process ends; it does nothing after the first call.
    void relaunch_if_requested();
}
