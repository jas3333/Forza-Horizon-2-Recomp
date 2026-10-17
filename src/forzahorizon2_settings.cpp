// FH2 settings menu (F1). The options a player wants, in one window, on top of
// the cvar system: every change applies at once (or is marked "restart") and
// is saved straight away to the config next to the saves (play/forzahorizon2.toml).
// F4 still opens the SDK's raw list of every cvar.
#include "forzahorizon2_settings.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>

// FH2 renderer options kept in the config. The renderer reads them as FH2_*
// switches: from the environment at startup (an explicitly set environment
// variable wins over the config), live through Fh2SetLiveEnv afterwards.
REXCVAR_DEFINE_BOOL(fh2_motion_blur, false, "FH2/Graphics",
                    "Camera motion blur (the original game has it on)");

namespace {

// Live renderer switch in the fh2 GPU plugin (fh2_rtlog.cpp). Null with
// another GPU plugin.
using SetLiveEnvFn = void (*)(const char*, int, int);
SetLiveEnvFn LookupSetLiveEnv() {
  void* plugin = dlopen("librexgpu-fh2.so", RTLD_NOW | RTLD_NOLOAD);
  return reinterpret_cast<SetLiveEnvFn>(plugin ? dlsym(plugin, "Fh2SetLiveEnv")
                                               : dlsym(RTLD_DEFAULT, "Fh2SetLiveEnv"));
}

void SetRendererSwitch(const char* name, int value, bool clear = false) {
  if (SetLiveEnvFn fn = LookupSetLiveEnv()) {
    fn(name, value, clear ? 1 : 0);
  }
}

// Renderer shortcuts that trade exactness for speed. Listed under
// Troubleshooting to find which one causes a visual problem; switched live and
// never saved (a broken combination must not survive a restart).
struct RendererSwitch {
  const char* name;
  int default_value;
  int checked_value, unchecked_value;
  const char* label;
  const char* help;
};
constexpr RendererSwitch kRendererSwitches[] = {
    {"FH2_FULL_FRAME_STRIPS", 2, 2, 1, "Strip draw dedup",
     "Draws repeated in every predicated-tiling strip are drawn once over the whole frame. "
     "Off: each strip draws its own copy (slower, exact)."},
    {"FH2_DIRECT_RESOLVE", 1, 1, 0, "Direct resolves",
     "Resolves go straight from the render target into the texture that samples them. "
     "Off: through emulated EDRAM and guest memory, like the console."},
    {"FH2_RESOLVE_SKIP_MEMORY", 1, 1, 0, "Skip guest memory on direct resolves",
     "Direct resolves don't write guest memory (nothing else should read it)."},
    {"FH2_CLEAR_INSTEAD_OF_TRANSFER", 1, 1, 0, "Depth clears instead of copies",
     "Rows only ever cleared get a depth clear instead of being copied back."},
    {"FH2_DEFER_TRANSFERS", 1, 1, 0, "Deferred render target copies",
     "Render target copies are made only when something reads them."},
    {"FH2_DEFER_STENCIL", 1, 1, 0, "Deferred stencil copies",
     "Stencil isn't copied for passes that overwrite it."},
    {"FH2_DIRECT_PARTIAL_DEPTH", 1, 1, 0, "Partial depth resolves cover the texture",
     "A depth resolve from the corner of a texture is treated as covering all of it."},
    {"FH2_DIRECT_CUBE", 1, 1, 0, "Direct cube map resolves",
     "Environment cube faces are resolved directly (reflections)."},
    {"FH2_DIRECT_SAMPLE_AS_PIXEL", 1, 1, 0, "Shadow map samples as pixels",
     "4x shadow maps resolved as a double-size 1x view read each sample as a pixel."},
    {"FH2_QUAD_CLEAR_1X", 1, 1, 0, "Redirect 4x depth clears to 1x",
     "Clear-only draws into a 4x depth target go to its 1x twin."},
    {"FH2_VFETCH_CACHE", 1, 1, 0, "Vertex fetch range cache",
     "Vertex buffer ranges already checked since the last invalidation are skipped."},
    {"FH2_TRANSFER_ALL", 0, 1, 0, "Copy reinterpreted render targets",
     "On: also copies EDRAM between render targets of different formats (more exact, slower)."},
};
int g_switch_values[std::size(kRendererSwitches)];
bool g_switch_values_initialized = false;

void InitSwitchValues() {
  if (g_switch_values_initialized) return;
  g_switch_values_initialized = true;
  for (size_t i = 0; i < std::size(kRendererSwitches); ++i) {
    const char* env = std::getenv(kRendererSwitches[i].name);
    g_switch_values[i] = env ? std::atoi(env) : kRendererSwitches[i].default_value;
  }
}

std::filesystem::path g_config_path;
// A changed option that only applies after a restart.
std::atomic<bool> g_restart_needed{false};

// Writes the values that came from the config file or this menu -- not those
// from the command line or the environment, which are per-run (a windowed
// test run must not make the next normal run windowed).
void SaveFh2Config() {
  if (g_config_path.empty()) return;
  std::string content = "# FH2 settings (edited by the in-game menu, F1)\n";
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.type == rex::cvar::FlagType::Command ||
        (entry.source != rex::cvar::Source::kConfig &&
         entry.source != rex::cvar::Source::kRuntime)) {
      continue;
    }
    std::string value = entry.getter();
    if (value == entry.default_value) continue;
    if (entry.type == rex::cvar::FlagType::String) {
      std::string quoted = "\"";
      for (char c : value) {
        if (c == '"' || c == '\\') quoted += '\\';
        quoted += c;
      }
      value = quoted + "\"";
    }
    content += entry.name + " = " + value + "\n";
  }
  std::ofstream file(g_config_path, std::ios::trunc);
  if (!file) {
    REXLOG_WARN("[SETTINGS] can't write {}", g_config_path.string());
    return;
  }
  file << content;
}

const rex::cvar::FlagEntry* FindFlag(std::string_view name) {
  for (const auto& entry : rex::cvar::GetRegistry()) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

void SetFlag(const char* name, const std::string& value) {
  const rex::cvar::FlagEntry* entry = FindFlag(name);
  if (!entry || entry->getter() == value) return;
  if (!rex::cvar::SetFlagByName(name, value)) {
    REXLOG_WARN("[SETTINGS] {} = {} rejected", name, value);
    return;
  }
  if (entry->lifecycle != rex::cvar::Lifecycle::kHotReload) {
    g_restart_needed = true;
  }
  SaveFh2Config();
  REXLOG_INFO("[SETTINGS] {} = {}", name, value);
}

std::string GetFlag(const char* name) {
  const rex::cvar::FlagEntry* entry = FindFlag(name);
  return entry ? entry->getter() : std::string();
}

void RestartBadge(const char* name) {
  const rex::cvar::FlagEntry* entry = FindFlag(name);
  if (entry && entry->lifecycle != rex::cvar::Lifecycle::kHotReload) {
    ImGui::SameLine();
    ImGui::TextDisabled("(restart)");
  }
}

void HelpMarker(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

bool BoolFlagCheckbox(const char* label, const char* name) {
  bool value = GetFlag(name) == "true";
  bool changed = ImGui::Checkbox(label, &value);
  if (changed) SetFlag(name, value ? "true" : "false");
  RestartBadge(name);
  return changed;
}

// Combo over (label, value) pairs of one cvar.
struct ComboOption {
  const char* label;
  const char* value;
};
template <size_t N>
void FlagCombo(const char* label, const char* name, const ComboOption (&options)[N],
               void (*on_change)(const char* value) = nullptr) {
  std::string current = GetFlag(name);
  int index = -1;
  for (size_t i = 0; i < N; ++i) {
    if (current == options[i].value) index = int(i);
  }
  ImGui::SetNextItemWidth(220.0f);
  if (ImGui::BeginCombo(label, index >= 0 ? options[index].label : current.c_str())) {
    for (size_t i = 0; i < N; ++i) {
      if (ImGui::Selectable(options[i].label, int(i) == index)) {
        if (on_change) {
          on_change(options[i].value);
        } else {
          SetFlag(name, options[i].value);
        }
      }
    }
    ImGui::EndCombo();
  }
  RestartBadge(name);
}

class Fh2SettingsDialog : public rex::ui::ImGuiDialog {
 public:
  Fh2SettingsDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window)
      : rex::ui::ImGuiDialog(drawer), window_(window) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (!ImGui::Begin("Forza Horizon 2 Settings (F1)", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::End();
      return;
    }
    if (ImGui::BeginTabBar("##fh2tabs")) {
      if (ImGui::BeginTabItem("Display")) {
        DrawDisplay();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Graphics")) {
        DrawGraphics();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Controls")) {
        DrawControls();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Troubleshooting")) {
        DrawTroubleshooting();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (g_restart_needed) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                         "Some changes take effect after restarting the game.");
    }
    ImGui::TextDisabled("Saved automatically to %s", g_config_path.filename().string().c_str());
    ImGui::End();
  }

 private:
  void DrawDisplay() {
    if (window_) {
      bool fullscreen = window_->IsFullscreen();
      if (ImGui::Checkbox("Fullscreen", &fullscreen)) {
        // The SDK's change callback on "fullscreen" switches the window.
        SetFlag("fullscreen", fullscreen ? "true" : "false");
      }
    }
    BoolFlagCheckbox("Vertical sync", "vsync");
    HelpMarker("The 60 fps mode needs vertical sync on.");

    static constexpr ComboOption kFrameRates[] = {{"60 fps", "120"}, {"30 fps (original)", "0"}};
    FlagCombo("Frame rate", "vblank_frequency_override", kFrameRates);
    HelpMarker("FH2 runs at 30 fps on the console. 60 fps doubles the guest vblank rate.");

    // Both axes together.
    static constexpr ComboOption kScales[] = {{"1x (1280x720)", "1"},
                                              {"2x (2560x1440)", "2"},
                                              {"3x (3840x2160)", "3"},
                                              {"4x (5120x2880)", "4"}};
    FlagCombo("Internal resolution", "draw_resolution_scale_x", kScales, [](const char* value) {
      SetFlag("draw_resolution_scale_x", value);
      SetFlag("draw_resolution_scale_y", value);
    });
    HelpMarker("Rendering resolution. Higher is sharper and costs GPU time.");

    static constexpr ComboOption kEffects[] = {
        {"Bilinear", "bilinear"}, {"AMD CAS (sharpen)", "cas"}, {"AMD FSR", "fsr"}};
    FlagCombo("Output filter", "present_effect", kEffects);
    HelpMarker("How the frame is scaled to the window. CAS sharpens; FSR upscales when the "
               "internal resolution is below the window size.");
  }

  void DrawGraphics() {
    static constexpr ComboOption kAniso[] = {{"Off", "0"}, {"2x", "2"}, {"4x", "3"},
                                             {"8x", "4"},  {"16x", "5"}, {"Game default", "-1"}};
    FlagCombo("Anisotropic filtering", "anisotropic_override", kAniso);
    HelpMarker("Keeps road and ground textures sharp at shallow angles.");

    BoolFlagCheckbox("Motion blur", "fh2_motion_blur");
    HelpMarker("Camera motion blur from the original game. Bloom, tone mapping and colour "
               "grading stay either way.");

    BoolFlagCheckbox("Real occlusion queries", "occlusion_query_enable");
    HelpMarker("Off (recommended): the game's visibility tests always answer \"visible\". "
               "On: real GPU queries, which make buildings and windows pop in.");
  }

  void DrawControls() {
    BoolFlagCheckbox("Keyboard and mouse controls", "mnk_mode");
    HelpMarker("Emulates a controller with the keyboard. Rebind keys with F4 > Input > Keybinds.");
    ImGui::Spacing();
    ImGui::TextDisabled("Keys: F1 this menu, F3 frame stats, F4 all settings, F12 screenshot,");
    ImGui::TextDisabled("F11 report a visual glitch (screenshot + renderer log).");
  }

  void DrawTroubleshooting() {
    InitSwitchValues();
    ImGui::TextWrapped(
        "Renderer shortcuts that trade exactness for speed. If something looks wrong (for "
        "example seeing through cars), switch these one at a time and watch whether it goes "
        "away. Changes apply on the next frame and are not saved.");
    ImGui::Spacing();
    if (!LookupSetLiveEnv()) {
      ImGui::TextDisabled("Not available: the game isn't using the fh2 renderer.");
      return;
    }
    for (size_t i = 0; i < std::size(kRendererSwitches); ++i) {
      const RendererSwitch& sw = kRendererSwitches[i];
      bool checked = g_switch_values[i] == sw.checked_value;
      if (ImGui::Checkbox(sw.label, &checked)) {
        g_switch_values[i] = checked ? sw.checked_value : sw.unchecked_value;
        SetRendererSwitch(sw.name, g_switch_values[i]);
        REXLOG_INFO("[SETTINGS] renderer {} = {}", sw.name, g_switch_values[i]);
      }
      HelpMarker(sw.help);
      if (g_switch_values[i] != sw.default_value) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "changed");
      }
    }
    ImGui::Spacing();
    // Every shortcut off: the exact (slower) path everywhere -- the first step
    // of finding which one causes a glitch.
    if (ImGui::Button("Turn all off (most exact)")) {
      for (size_t i = 0; i < std::size(kRendererSwitches); ++i) {
        const RendererSwitch& sw = kRendererSwitches[i];
        // Off by default (copy reinterpreted targets): exact is the checked one.
        g_switch_values[i] = sw.default_value == 0 ? sw.checked_value : sw.unchecked_value;
        SetRendererSwitch(sw.name, g_switch_values[i]);
      }
      REXLOG_INFO("[SETTINGS] renderer: all shortcuts off");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all to defaults")) {
      for (size_t i = 0; i < std::size(kRendererSwitches); ++i) {
        g_switch_values[i] = kRendererSwitches[i].default_value;
        SetRendererSwitch(kRendererSwitches[i].name, g_switch_values[i]);
      }
      REXLOG_INFO("[SETTINGS] renderer: all shortcuts back to defaults");
    }
  }

  rex::ui::Window* window_;
};

std::unique_ptr<Fh2SettingsDialog> g_dialog;

}  // namespace

void ApplyFh2SettingsAtStartup() {
  // Before the renderer reads its switches: the config's renderer options as
  // environment variables, unless the environment already sets them.
  setenv("FH2_MOTION_BLUR", REXCVAR_GET(fh2_motion_blur) ? "1" : "0", 0);
  rex::cvar::RegisterChangeCallback("fh2_motion_blur", [](std::string_view, std::string_view v) {
    SetRendererSwitch("FH2_MOTION_BLUR", v == "true" ? 1 : 0);
  });
}

void InstallFh2Settings(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                        std::filesystem::path config_path) {
  g_config_path = std::move(config_path);
  // Glitch report: the renderer's frame log of the next frames ([RTLOG] in the
  // log: render target copies, clears, resolves, dedups) plus a screenshot,
  // in one key press while racing.
  rex::ui::RegisterBind("bind_fh2_glitch", "F11", "FH2: report a visual glitch", [] {
    void* plugin = dlopen("librexgpu-fh2.so", RTLD_NOW | RTLD_NOLOAD);
    auto capture =
        reinterpret_cast<void (*)(int)>(plugin ? dlsym(plugin, "Fh2CaptureFrames") : nullptr);
    if (capture) {
      capture(2);
    }
    REXLOG_INFO("[GLITCH] reported (frame log of the next 2 frames follows)");
    // The screenshot bind, from another thread: binds run under the bind
    // table's lock (the debug pipe presses F12 the same way).
    std::thread([] {
      rex::ui::KeyEvent e(nullptr, rex::ui::VirtualKey::kF12, 1, false, false, false, false, false);
      rex::ui::ProcessKeyEvent(e);
    }).detach();
  });
  rex::ui::RegisterBind("bind_fh2_settings", "F1", "FH2: settings menu", [drawer, window] {
    if (g_dialog) {
      g_dialog.reset();
    } else {
      g_dialog = std::make_unique<Fh2SettingsDialog>(drawer, window);
    }
  });
}
