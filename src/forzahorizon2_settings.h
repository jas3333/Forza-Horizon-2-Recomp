#pragma once

#include <filesystem>

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

// Pushes the config's FH2 renderer options to the renderer. Call before the
// GPU plugin starts drawing (OnPreLaunchModule).
void ApplyFh2SettingsAtStartup();

// Registers the F1 settings menu. Changes are saved to config_path.
void InstallFh2Settings(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                        std::filesystem::path config_path);
