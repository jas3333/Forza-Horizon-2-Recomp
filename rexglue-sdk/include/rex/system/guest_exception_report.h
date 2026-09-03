#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;

namespace rex::system {

struct GuestExceptionReport {
  std::string kind;
  std::string type;
  std::string message;
  uint32_t thread_id = 0;
  uint32_t thread_handle = 0;
  uint32_t function = 0;
  uint32_t instruction = 0;
  uint32_t lr = 0;
  uint32_t stack_pointer = 0;
  std::vector<std::string> host_frames;
};

std::string SanitizeGuestExceptionText(std::string_view text, size_t limit = 240);
std::vector<std::string> CaptureGuestExceptionHostFrames(size_t skip = 0, size_t limit = 12);
std::string FormatGuestExceptionReport(const GuestExceptionReport& report);
GuestExceptionReport MakeGuestExceptionReport(const PPCContext* context, uint32_t thread_id,
                                              uint32_t thread_handle, std::string_view kind,
                                              std::string_view type, std::string_view message);

}  // namespace rex::system
