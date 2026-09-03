/**
 * @file        tests/unit/codegen/resume_entry_test.cpp
 * @brief       Synthetic coverage for interior-PC AOT resume entries
 */

#include <array>
#include <map>

#include <catch2/catch_test_macros.hpp>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/codegen_context.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>
#include <rex/codegen/test_support.h>

TEST_CASE("FunctionNode: linked-call return PC emits an interior resume entry",
          "[codegen][resume]") {
  constexpr uint32_t kBase = 0x82010000u;
  std::array<uint32_t, 4> words = {
      __builtin_bswap32(0x4800000Du),  // bl kBase + 12
      __builtin_bswap32(0x60000000u),  // nop (saved LR resumes here)
      __builtin_bswap32(0x60000001u),  // ori with bit 0 set is not a linked branch
      __builtin_bswap32(0x4E800020u),  // blr
  };

  rex::codegen::TestModule module;
  module.Load(kBase, reinterpret_cast<const uint8_t*>(words.data()), sizeof(words));
  auto binary = rex::codegen::BinaryView::fromModule(module);
  rex::codegen::RecompilerConfig config;
  auto ctx = rex::codegen::CodegenContext::Create(std::move(binary), std::move(config));

  rex::codegen::AnalyzeTestBinary(
      ctx, "resume", {{kBase, "test_owner"}, {kBase + 12, "test_callee"}}, kBase,
      reinterpret_cast<const uint8_t*>(words.data()), sizeof(words));

  const auto* owner = ctx.graph.getFunction(kBase);
  REQUIRE(owner != nullptr);
  CHECK(owner->resumableReturnAddresses(ctx.binary()) == std::vector<uint32_t>{kBase + 4});

  rex::codegen::EmitContext emit{ctx.binary(), ctx.Config(), ctx.graph, kBase + 12, nullptr};
  const std::string output = owner->emitCpp(emit);
  CHECK(output.find("case 0x82010004: goto loc_82010004;") != std::string::npos);
  CHECK(output.find("loc_82010004:") != std::string::npos);
  CHECK(output.find("ctx.dispatch_address = 0;") != std::string::npos);
}
