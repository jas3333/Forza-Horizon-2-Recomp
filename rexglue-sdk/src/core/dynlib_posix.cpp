#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <dlfcn.h>

namespace rex::platform {

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& path, SymbolResolution mode) {
  Close();
  int flags = (mode == SymbolResolution::kImmediate) ? RTLD_NOW : RTLD_LAZY;
  // Recompiled DLL modules are registered by bare target name, which
  // LoadLibraryW resolves against the executable's folder with an implied
  // .dll. dlopen does neither, so spell out the platform file name here.
  std::filesystem::path resolved = path;
  if (!path.has_parent_path() && !path.has_extension()) {
#if REX_PLATFORM_MAC
    constexpr const char* kSuffix = ".dylib";
#else
    constexpr const char* kSuffix = ".so";
#endif
    resolved = rex::filesystem::GetExecutableFolder() / ("lib" + path.string() + kSuffix);
  }
  handle_ = dlopen(resolved.c_str(), flags);
  if (!handle_) {
    REXLOG_ERROR("dlopen({}) failed: {}", resolved.string(), dlerror());
  }
  return handle_ != nullptr;
}

void DynamicLibrary::Close() {
  if (handle_) {
    dlclose(handle_);
    handle_ = nullptr;
  }
}

void* DynamicLibrary::GetRawSymbol(const char* name) const {
  if (!handle_)
    return nullptr;
  return dlsym(handle_, name);
}

}  // namespace rex::platform
