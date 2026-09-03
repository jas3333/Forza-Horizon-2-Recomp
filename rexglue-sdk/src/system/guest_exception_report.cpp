#include <rex/system/guest_exception_report.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <fmt/format.h>

#include <rex/platform.h>
#include <rex/ppc/context.h>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#endif

namespace rex::system {

std::string SanitizeGuestExceptionText(std::string_view text, size_t limit) {
  std::string result;
  result.reserve(std::min(text.size(), limit));
  for (char value : text) {
    if (result.size() == limit) {
      break;
    }
    const unsigned char byte = static_cast<unsigned char>(value);
    result.push_back(std::iscntrl(byte) ? ' ' : value);
  }
  return result;
}

std::vector<std::string> CaptureGuestExceptionHostFrames(size_t skip, size_t limit) {
  std::vector<std::string> result;
#if REX_PLATFORM_WIN32
  constexpr USHORT kFrameCapacity = 32;
  void* frames[kFrameCapacity]{};
  const USHORT requested =
      static_cast<USHORT>(std::min(limit, static_cast<size_t>(kFrameCapacity)));
  const USHORT count =
      CaptureStackBackTrace(static_cast<DWORD>(skip + 1), requested, frames, nullptr);
  result.reserve(count);
  for (USHORT index = 0; index < count; ++index) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(frames[index]), &module)) {
      result.push_back(fmt::format("unknown+0x{:X}", reinterpret_cast<uintptr_t>(frames[index])));
      continue;
    }
    wchar_t path[MAX_PATH]{};
    const DWORD path_size = GetModuleFileNameW(module, path, MAX_PATH);
    const std::filesystem::path module_path(path_size == 0 ? L"unknown"
                                                           : std::wstring(path, path_size));
    const uintptr_t offset =
        reinterpret_cast<uintptr_t>(frames[index]) - reinterpret_cast<uintptr_t>(module);
    result.push_back(fmt::format("{}+0x{:X}", module_path.filename().string(), offset));
  }
#else
  (void)skip;
  (void)limit;
#endif
  return result;
}

std::string FormatGuestExceptionReport(const GuestExceptionReport& report) {
  std::string result = fmt::format(
      "[GUEST_EXCEPTION] kind={} type={} message=\"{}\" thread=0x{:X} "
      "handle=0x{:08X} function=0x{:08X} pc=0x{:08X} lr=0x{:08X} "
      "r1=0x{:08X} last_import=unavailable guest_memory_included=false",
      report.kind, report.type, SanitizeGuestExceptionText(report.message), report.thread_id,
      report.thread_handle, report.function, report.instruction, report.lr, report.stack_pointer);
  for (size_t index = 0; index < report.host_frames.size(); ++index) {
    result +=
        fmt::format("\n[GUEST_EXCEPTION] host_frame[{}]={}", index, report.host_frames[index]);
  }
  return result;
}

GuestExceptionReport MakeGuestExceptionReport(const PPCContext* context, uint32_t thread_id,
                                              uint32_t thread_handle, std::string_view kind,
                                              std::string_view type, std::string_view message) {
  GuestExceptionReport report;
  report.kind = SanitizeGuestExceptionText(kind, 48);
  report.type = SanitizeGuestExceptionText(type, 120);
  report.message = SanitizeGuestExceptionText(message);
  report.thread_id = thread_id;
  report.thread_handle = thread_handle;
  if (context != nullptr) {
    report.function = context->current_function;
    report.instruction = context->current_instruction;
    report.lr = static_cast<uint32_t>(context->lr);
    report.stack_pointer = context->r1.u32;
  }
  report.host_frames = CaptureGuestExceptionHostFrames(1);
  return report;
}

}  // namespace rex::system
