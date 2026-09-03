#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/system/guest_exception_report.h>

using namespace rex::system;

TEST_CASE("guest exception report contains bounded guest metadata", "[system][guest_exception]") {
  PPCContext context{};
  context.current_function = 0x82345678;
  context.current_instruction = 0x823456A0;
  context.lr = 0x82400000;
  context.r1.u32 = 0x70001000;

  auto report = MakeGuestExceptionReport(&context, 7, 0xF8000044, "std_exception", "runtime_error",
                                         "first line\nsecond line");
  report.host_frames = {"fm2.exe+0x1234", "rexruntime.dll+0x5678"};
  const std::string text = FormatGuestExceptionReport(report);

  REQUIRE(text.find("function=0x82345678") != std::string::npos);
  REQUIRE(text.find("pc=0x823456A0") != std::string::npos);
  REQUIRE(text.find("lr=0x82400000") != std::string::npos);
  REQUIRE(text.find("r1=0x70001000") != std::string::npos);
  REQUIRE(text.find("message=\"first line second line\"") != std::string::npos);
  REQUIRE(text.find("guest_memory_included=false") != std::string::npos);
  REQUIRE(text.find("host_frame[1]=rexruntime.dll+0x5678") != std::string::npos);
}

TEST_CASE("guest exception text is bounded", "[system][guest_exception]") {
  REQUIRE(SanitizeGuestExceptionText("abcdef", 4) == "abcd");
}
