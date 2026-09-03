#include <catch2/catch_test_macros.hpp>

#include <rex/codegen/binary_view.h>
#include <rex/codegen/config.h>
#include <rex/codegen/function_graph.h>

namespace rex::codegen {

TEST_CASE("A label must have emitted code before a branch stays local", "[codegen][graph]") {
  FunctionGraph graph;
  graph.addFunction(0x1000, 0x200, FunctionAuthority::CONFIG);
  graph.addBlockToFunction(0x1000, {0x1000, 0x20});

  graph.addFunction(0x1100, 0x20, FunctionAuthority::CONFIG);
  graph.addBlockToFunction(0x1100, {0x1100, 0x20});
  graph.addLabelToFunction(0x1000, 0x1100);

  REQUIRE(graph.classifyTarget(0x1100, 0x1004, false) == TargetKind::Function);
}

TEST_CASE("A dispatcher entry inside an emitted parent stays local", "[codegen][graph]") {
  FunctionGraph graph;
  graph.addFunction(0x2000, 0x100, FunctionAuthority::CONFIG);
  graph.addBlockToFunction(0x2000, {0x2000, 0x100});

  graph.addFunction(0x2080, 0x20, FunctionAuthority::CONFIG);
  graph.addBlockToFunction(0x2080, {0x2080, 0x20});
  graph.addLabelToFunction(0x2000, 0x2080);

  REQUIRE(graph.classifyTarget(0x2080, 0x2010, false) == TargetKind::InternalLabel);
}

TEST_CASE("A shared epilogue is local to its owner and callable from another function",
          "[codegen][graph]") {
  FunctionGraph graph;
  graph.addFunction(0x3000, 0x80, FunctionAuthority::CONFIG);
  graph.addBlockToFunction(0x3000, {0x3000, 0x80});
  graph.addLabelToFunction(0x3000, 0x3180);

  graph.addFunction(0x3100, 0x8C, FunctionAuthority::CONFIG);
  graph.addBlockToFunction(0x3100, {0x3100, 0x8C});
  graph.addLabelToFunction(0x3100, 0x3180);

  graph.addFunction(0x3180, 0x0C, FunctionAuthority::CONFIG);
  graph.addBlockToFunction(0x3180, {0x3180, 0x0C});

  REQUIRE(graph.classifyTarget(0x3180, 0x3040, false) == TargetKind::Function);
  REQUIRE(graph.classifyTarget(0x3180, 0x3140, false) == TargetKind::InternalLabel);
}

TEST_CASE("A continuation alias uses its parent's discovered body", "[codegen][graph]") {
  BinaryView binary;
  RecompilerConfig config;
  FunctionGraph graph;

  auto* parent = graph.addFunction(0x4000, 0x100, FunctionAuthority::PDATA);
  graph.addBlockToFunction(0x4000, {0x4000, 0x80});
  graph.addFunction(0x4040, 0x40, FunctionAuthority::CONFIG, "named_continuation");

  config.functions[0x4040].parent = 0x4000;
  config.functions[0x4080].parent = 0x4000;
  config.functions[0x4040].name = "named_continuation";
  config.functions[0x9000].parent = 0x8000;
  graph.registerChunk(0x4040, 0x40, 0x4000);
  graph.registerChunk(0x4080, 0x40, 0x4000);
  graph.registerChunk(0x9000, 0x40, 0x8000);

  EmitContext emitContext{binary, config, graph};
  const auto aliases = parent->parentBackedAliases(emitContext);

  REQUIRE(aliases.size() == 1);
  CHECK(aliases[0].first == 0x4040);
  CHECK(aliases[0].second == "named_continuation");
}

TEST_CASE("A verified parent-backed alias seals without duplicate blocks",
          "[codegen][graph]") {
  FunctionGraph graph;
  auto* parent = graph.addFunction(0x5000, 0x100, FunctionAuthority::PDATA);
  graph.addBlockToFunction(0x5000, {0x5000, 0x100});
  auto* alias = graph.addFunction(0x5040, 0x40, FunctionAuthority::CONFIG);

  REQUIRE(alias->canDiscover());
  alias->discoverAsParentBackedAlias();
  CHECK(alias->isParentBackedAlias());
  CHECK(alias->blocks().empty());
  REQUIRE(alias->canSeal());
  alias->seal();
  CHECK(alias->isSealed());
  CHECK(graph.isEntryPoint(0x5040));
  CHECK(graph.getFunctionContaining(0x5040) == parent);
  CHECK(graph.getFunctionContaining(0x5060) == parent);
}

}  // namespace rex::codegen
