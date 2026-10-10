#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context.h>

#include <filesystem>
#include <memory>

#include "forzahorizon2_init.h"

// Defined in forzahorizon2_hooks.cpp.
void InstallGuestPageFaultRecovery();

// Xbox Live Hive / Cloud sync offline handler
static void Hook_sub_82D9F3E0(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = 0;
}

// String tags protection
static void Hook_sub_824A5FC0(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = 0;
}

// Was registered over 0x82D93740 as a "Voice/Kinect thread exit". It is
// not: 0x82D93740 is the entry point of the 11-thread job worker pool that
// sub_82D94508 starts during boot (one worker per ExCreateThread at
// 0x82D94654). Stubbing it made every worker exit immediately, so the first
// job the main thread queued (NtSetEvent from 0x8248A47C) was never run and
// the main thread blocked forever waiting for its completion event. Kept
// unregistered for reference.
[[maybe_unused]] static void Hook_sub_82D93740(PPCContext& ctx, uint8_t* base) {
  (void)base;
  ctx.r3.u64 = 0;
}

// Diagnostic-only: not registered. See the note in OnPreLaunchModule() for
// why installing this at guest address 0 breaks kernel import resolution.
// Kept because the data it produced is what identified the two null-call
// sites worth chasing:
//   lr=0x8255D388  CGame::GetPartsHelper -- the SQL statement object comes
//                  back as 0, i.e. the query factory never wrote its out-param
//   lr=0x82D97D9C  inside 0x82D97D60, which walks an MSVC std::set of
//                  listeners (FUN_82fc77c8 beside it is the red-black tree
//                  increment: _Left+0 /_Parent+4 /_Right+8 /_Myval+0xC
//                  /_Isnil+0x11) and calls vtable[0] on each entry
[[maybe_unused]] static void Hook_NullIndirectCallGuard(PPCContext& ctx, uint8_t* base) {
  uint32_t obj = ctx.r3.u32;
  uint32_t vtable = 0;
  uint32_t first_word = 0;
  // Guest pointers live in the 32-bit guest arena; anything outside it is
  // already garbage and must not be dereferenced.
  if (obj != 0 && obj < 0xC0000000u) {
    vtable = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + obj));
    if (vtable != 0 && vtable < 0xC0000000u) {
      first_word = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + vtable));
    }
  }
  REXKRNL_WARN(
      "[NullCallGuard] null call: lr={:08X} r3(obj)={:08X} [obj]={:08X} "
      "[[obj]]={:08X} r4={:08X}",
      static_cast<uint32_t>(ctx.lr), obj, vtable, first_word, ctx.r4.u32);
  ctx.r3.u64 = 0;
}

class Forzahorizon2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Forzahorizon2App>(new Forzahorizon2App(ctx, "forzahorizon2",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    auto cwd = std::filesystem::current_path();
    std::error_code ec;

    // Set standard path roots directly on PathConfig. ReXGlue handles native early device mapping,
    // writable cache:\ mounting, and STFS ContentManager save handling for these paths.
    paths.user_data_root = cwd / "user";
    paths.cache_root     = cwd / "cache";

    // Remembered so OnPreLaunchModule() can pre-mount the save package at the
    // exact same root ContentManager resolves against.
    user_data_root_ = paths.user_data_root;
    
    // --game_data_root wins; the original development path is only a fallback
    // on Windows. Elsewhere the SDK reports the missing flag itself.
    if (paths.game_data_root.empty()) {
#ifdef _WIN32
      paths.game_data_root =
          "D:\\GAME RECOMP\\Forza Horizon 2 (Europe) (En,Ja,Fr,De,Es,It,Pt,Zh,Pl,Ru)";
#endif
    }
    if (!paths.game_data_root.empty()) {
      paths.game_data_root = std::filesystem::weakly_canonical(paths.game_data_root, ec);
    }
    paths.update_data_root = paths.game_data_root;

    std::filesystem::create_directories(paths.user_data_root, ec);
    std::filesystem::create_directories(paths.cache_root, ec);
  }

  // OnPreLaunchModule - register function hooks and extra symbolic aliases
  void OnPreLaunchModule() override {
    InstallGuestPageFaultRecovery();

    auto* ks = rex::system::kernel_state();
    if (ks && ks->file_system()) {
      // Direct disc drive aliases to game root (game: is already mounted by ReXGlue via PathConfig)
      ks->file_system()->RegisterSymbolicLink("d:", "game:");
      ks->file_system()->RegisterSymbolicLink("\\Device\\Image", "game:");

      // Pre-mount "B13EBABEBABEBABE:" (the signed-in XUID, which Forza uses as
      // its content root name) at the exact path ContentManager would use:
      //   <user_data_root>/<XUID>/<title id>/<content type>/<file name>
      //
      // Why this is required: the game opens its save database *before* the
      // XamContentCreateEx that would make ContentManager mount the package.
      // Those early opens ("B13EBABEBABEBABE:\PlayerDatabase" and friends)
      // otherwise fail with 0xC000000F (device not found), leaving the SQL
      // layer in sub_8255D2C0 -- which runs
      //   "SELECT Count(GarageId) AS NumParts FROM %sCareer_Garage"
      // (string literals read straight out of Forza_uncompressed.xex) --
      // holding an invalid statement object. Its vtable slot is then read as
      // garbage, producing either a call to guest address 0 or a jump to a
      // junk address, i.e. the "crashes on second launch" symptom.
      //
      // Because RegisterSymbolicLink() uses map::insert(), this registration
      // wins over the later ContentPackage one -- which is harmless here
      // precisely because both resolve to the same directory.
      //
      // Initialize() is what populates the device's root entry; without it
      // every ResolvePath()/GetChild() against it dereferences a null root.
      //
      // Only mount when the package directory already exists, i.e. on a
      // second or later launch. Creating it here would be actively harmful on
      // a first launch: ContentManager::CreateContent() bails out with
      // X_ERROR_ALREADY_EXISTS the moment the package path exists, so
      // pre-creating it makes the game believe it has a save it never wrote,
      // and it then reads an empty PlayerDatabase instead of initializing one.
      const std::string xuid_name = fmt::format("{:016X}", ks->user_profile()->xuid());
      auto save_dir = user_data_root_ / xuid_name / "4D530AA4" /
                      "00000001" / "ForzaProfile";
      if (!std::filesystem::exists(save_dir)) {
        REXKRNL_INFO(
            "[SaveMount] No existing save package; leaving the mount to "
            "ContentManager so first-launch content creation succeeds.");
      } else {
        auto save_device = std::make_unique<rex::filesystem::HostPathDevice>(
            "\\Device\\" + xuid_name, save_dir, /*read_only=*/false,
            /*allow_share_delete=*/true);
        if (save_device->Initialize()) {
          ks->file_system()->RegisterDevice(std::move(save_device));
          ks->file_system()->RegisterSymbolicLink(xuid_name + ":", "\\Device\\" + xuid_name);
          REXKRNL_INFO("[SaveMount] Pre-mounted {}: -> {}", xuid_name,
                       save_dir.string());
        } else {
          REXKRNL_WARN("[SaveMount] Failed to initialize save device at {}",
                       save_dir.string());
        }
      }
    }

    auto* fd = runtime()->function_dispatcher();
    if (fd) {
      fd->SetFunction(0x824A5FC0, Hook_sub_824A5FC0);
      fd->SetFunction(0x82D9F3E0, Hook_sub_82D9F3E0);
      fd->SetFunction(0x8245F9E0, Hook_sub_82D9F3E0);

      // NOTE: do not register a phantom module at code_base 0 to make
      // SetFunction(0x0, Hook_NullIndirectCallGuard) legal. It was tried and
      // it corrupts kernel import resolution.
      //
      // FunctionDispatcher::SetFunction() only accepts addresses inside a
      // registered module's [code_base, thunk_limit), and thunk_limit is
      // always code_base + code_size + kThunkReserveSize (64KB), so any
      // module based at 0 spans 0x0-0x10000+. AllocateThunk() looks the
      // caller up with FindModuleByAddress(caller_address), and for imports
      // resolved by ordinal that "address" is a small ordinal-sized value.
      // Those then land inside the phantom module instead of failing, so
      // XexGetProcedureAddress starts handing the title real-looking thunks
      // in a module that contains no code:
      //     Ordinal 642 -> ProcAddr 0x00010008   (XInputdFFGetDeviceInfo)
      //     Ordinal 643 -> ProcAddr 0x0001000c   (XInputdFFSetEffect)
      // Unpatched, those ordinals simply report NOT FOUND and the title
      // copes. With the phantom module it calls into the fake thunk pool --
      // which is what turned pressing START into an immediate crash.
    }
  }

 private:
  std::filesystem::path user_data_root_;
};
