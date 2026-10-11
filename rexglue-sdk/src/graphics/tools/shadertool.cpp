/**
 * rex-shadertool: offline Xenos microcode -> SPIR-V translation.
 *
 *   rex-shadertool translate <ucode_dir> <out_dir>
 *
 * <ucode_dir> holds <hash>.vs.bin / <hash>.ps.bin files with big-endian
 * microcode (as written by tools/extract_shaders.py). Every shader is
 * analysed and translated with the translator's default modification for
 * its type; the SPIR-V goes to <out_dir>/<hash>.<vs|ps>.<modification>.spv
 * and a manifest (<out_dir>/manifest.json) records per-shader reflection:
 * float constants read, bool/loop use, vertex and texture fetch slots,
 * interpolators, colour targets, depth writes, kills, memexport, and any
 * failure. Failures are what decides whether FH2's whole shader set can go
 * through this translator.
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>

namespace fs = std::filesystem;
using namespace rex::graphics;

namespace {

int Translate(const fs::path& ucode_dir, const fs::path& out_dir) {
  fs::create_directories(out_dir);
  SpirvShaderTranslator translator(SpirvShaderTranslator::Features(true),
                                   /*native_2x_msaa_with_attachments=*/true,
                                   /*native_2x_msaa_no_attachments=*/true,
                                   /*edram_fragment_shader_interlock=*/false);
  rex::string::StringBuffer disasm;
  std::string manifest = "{\n";
  size_t total = 0, ok = 0, failed = 0;
  std::vector<fs::path> files;
  if (fs::is_directory(ucode_dir)) {
    for (const auto& entry : fs::directory_iterator(ucode_dir)) {
      if (entry.path().extension() == ".bin") files.push_back(entry.path());
    }
  } else {
    files.push_back(ucode_dir);  // single file (one process per shader)
  }
  std::sort(files.begin(), files.end());
  for (const fs::path& path : files) {
    // <hash>.<vs|ps>.bin
    std::string stem = path.stem().string();
    size_t dot = stem.find('.');
    if (dot == std::string::npos) continue;
    std::string hash_text = stem.substr(0, dot), kind = stem.substr(dot + 1);
    xenos::ShaderType type =
        kind == "vs" ? xenos::ShaderType::kVertex : xenos::ShaderType::kPixel;
    std::ifstream in(path, std::ios::binary);
    std::vector<uint32_t> dwords(fs::file_size(path) / 4);
    in.read(reinterpret_cast<char*>(dwords.data()), dwords.size() * 4);
    ++total;

    Shader shader(type, std::stoull(hash_text, nullptr, 16), dwords.data(), dwords.size(),
                  std::endian::big);
    disasm.Reset();
    shader.AnalyzeUcode(disasm);
    uint32_t dynamic_count = shader.GetDynamicAddressableRegisterCount(0);
    uint64_t modification =
        type == xenos::ShaderType::kVertex
            ? translator.GetDefaultVertexShaderModification(dynamic_count)
            : translator.GetDefaultPixelShaderModification(dynamic_count);
    Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
    bool translated = shader.is_ucode_analyzed() && translator.TranslateAnalyzedShader(*translation) &&
                      translation->is_valid() && !translation->translated_binary().empty();
    std::string spv_name = fmt::format("{}.{}.{:016X}.spv", hash_text, kind, modification);
    if (translated) {
      std::ofstream(out_dir / spv_name, std::ios::binary)
          .write(reinterpret_cast<const char*>(translation->translated_binary().data()),
                 translation->translated_binary().size());
      ++ok;
    } else {
      ++failed;
    }

    const auto& constants = shader.constant_register_map();
    uint32_t float_count = 0;
    for (uint64_t block : constants.float_bitmap) float_count += uint32_t(__builtin_popcountll(block));
    std::string vfetch, tfetch;
    for (const auto& binding : shader.vertex_bindings()) {
      vfetch += fmt::format("{}{}", vfetch.empty() ? "" : ",", binding.fetch_constant);
    }
    for (const auto& binding : shader.texture_bindings()) {
      tfetch += fmt::format("{}{}", tfetch.empty() ? "" : ",", binding.fetch_constant);
    }
    manifest += fmt::format(
        "{}  \"{}.{}\": {{\"ok\": {}, \"spv\": \"{}\", \"modification\": \"{:016X}\", "
        "\"dwords\": {}, \"float_constants\": {}, \"bool_bitmap\": [{},{},{},{},{},{},{},{}], "
        "\"loop_bitmap\": {}, \"vfetch\": [{}], \"tfetch\": [{}], \"writes_interpolators\": {}, "
        "\"color_targets\": {}, \"writes_depth\": {}, \"kills\": {}, \"memexport\": {}, "
        "\"dynamic_registers\": {}}}",
        ok + failed > 1 ? ",\n" : "", hash_text, kind, translated ? "true" : "false",
        translated ? spv_name : "", modification, dwords.size(), float_count,
        constants.bool_bitmap[0], constants.bool_bitmap[1], constants.bool_bitmap[2],
        constants.bool_bitmap[3], constants.bool_bitmap[4], constants.bool_bitmap[5],
        constants.bool_bitmap[6], constants.bool_bitmap[7], constants.loop_bitmap, vfetch, tfetch,
        shader.writes_interpolators(), shader.writes_color_targets(),
        shader.writes_depth() ? "true" : "false", shader.kills_pixels() ? "true" : "false",
        shader.memexport_eM_written() ? "true" : "false", dynamic_count);
    if (!translated) {
      std::fprintf(stderr, "FAILED %s.%s (%zu dwords, analyzed=%d)\n", hash_text.c_str(),
                   kind.c_str(), dwords.size(), int(shader.is_ucode_analyzed()));
    }
  }
  manifest += "\n}\n";
  std::ofstream(out_dir / (fs::is_directory(ucode_dir) ? std::string("manifest.json") : ucode_dir.stem().string() + ".json")) << manifest;
  std::printf("%zu shaders: %zu translated, %zu failed -> %s\n", total, ok, failed,
              out_dir.string().c_str());
  return failed ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && std::strcmp(argv[1], "translate") == 0) {
    return Translate(argv[2], argv[3]);
  }
  std::fprintf(stderr, "usage: rex-shadertool translate <ucode_dir> <out_dir>\n");
  return 2;
}
