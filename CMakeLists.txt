# forzahorizon2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(forzahorizon2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(FORZAHORIZON2_SOURCES
    src/main.cpp
    src/forzahorizon2_hooks.cpp
    src/forzahorizon2_hook_sub_823f4ee8.cpp
    src/forzahorizon2_hook_sub_82d881e8.cpp
    src/forzahorizon2_hook_sub_824a6170.cpp
    src/forzahorizon2_hook_sub_823f8718.cpp
    src/forzahorizon2_d3d_probe.cpp
)

if(WIN32)
    add_executable(forzahorizon2 WIN32 ${FORZAHORIZON2_SOURCES})
else()
    add_executable(forzahorizon2 ${FORZAHORIZON2_SOURCES})
endif()

rexglue_setup_target(forzahorizon2 GPU_PLUGINS xenos)

# The generated function tables are one giant initializer each (~475k
# entries). Optimizing them buys nothing and the register table alone took
# ~6.5 min at -O2, serially, at the end of every regeneration build.
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/forzahorizon2_register.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/default/forzahorizon2_init.cpp
    PROPERTIES COMPILE_OPTIONS "-O0" SKIP_PRECOMPILE_HEADERS ON)

# rex_app.cpp (added by rexglue_configure_target) needs imgui.h, but rexruntime
# links imgui PRIVATE -- that propagates the .lib to the final link but not
# the include directory. Link it directly so this consumer sees the header.
target_link_libraries(forzahorizon2 PRIVATE imgui)

# The executable's RUNPATH is $ORIGIN, but the in-tree SDK writes
# librexruntime.so into its own output directory. Put it next to the binary.
if(NOT WIN32)
    add_custom_command(TARGET forzahorizon2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:rexruntime> $<TARGET_FILE_DIR:forzahorizon2>)
endif()

# POST_BUILD copies only run when the executable relinks, so a change that
# only rebuilds the GPU plugin or the runtime left the old copies next to the
# binary (and that copy is the one loaded). Stage them on every build.
if(NOT WIN32)
    set(_fh2_stage_libs $<TARGET_FILE:rexruntime> $<TARGET_FILE:rexgpu-xenos>)
    set(_fh2_stage_deps rexruntime rexgpu-xenos)
    if(TARGET TracyClient)
        list(APPEND _fh2_stage_libs $<TARGET_FILE:TracyClient>)
        list(APPEND _fh2_stage_deps TracyClient)
    endif()
    add_custom_target(forzahorizon2_stage_libs ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_fh2_stage_libs} $<TARGET_FILE_DIR:forzahorizon2>
        DEPENDS ${_fh2_stage_deps}
        COMMENT "Staging runtime and GPU plugin next to forzahorizon2")
    add_dependencies(forzahorizon2 forzahorizon2_stage_libs)
endif()

# FH2 native renderer plugin (librexgpu-fh2.so, --gpu_plugin=fh2). Reuses the
# SDK's generic GPU layer (ring buffer, PM4 parsing, register file, shader
# analysis) and provides its own Vulkan backend in src/gfx.
if(NOT WIN32)
    set(_rex_gfx ${CMAKE_CURRENT_SOURCE_DIR}/rexglue-sdk/src/graphics)
    file(GLOB _fh2_gfx_vk ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/vk/*.cpp)
    add_library(rexgpu-fh2 SHARED
        src/gfx/plugin.cpp
        ${_fh2_gfx_vk}
        ${_rex_gfx}/flags.cpp
        ${_rex_gfx}/registers.cpp
        ${_rex_gfx}/register_file.cpp
        ${_rex_gfx}/graphics_system.cpp
        ${_rex_gfx}/command_processor.cpp
        ${_rex_gfx}/packet_disassembler.cpp
        ${_rex_gfx}/primitive_processor.cpp
        ${_rex_gfx}/shared_memory.cpp
        ${_rex_gfx}/pipeline/render_target/cache.cpp
        ${_rex_gfx}/pipeline/texture/cache.cpp
        ${_rex_gfx}/pipeline/shader/shader.cpp
        ${_rex_gfx}/pipeline/shader/interpreter.cpp
        ${_rex_gfx}/pipeline/shader/translator.cpp
        ${_rex_gfx}/pipeline/shader/translator_disasm.cpp
        ${_rex_gfx}/pipeline/shader/spirv_builder.cpp
        ${_rex_gfx}/pipeline/shader/spirv.cpp
        ${_rex_gfx}/pipeline/shader/spirv_translator.cpp
        ${_rex_gfx}/pipeline/shader/spirv_translator_alu.cpp
        ${_rex_gfx}/pipeline/shader/spirv_translator_fetch.cpp
        ${_rex_gfx}/pipeline/shader/spirv_translator_memexport.cpp
        ${_rex_gfx}/pipeline/shader/spirv_translator_rb.cpp
        ${_rex_gfx}/format/ucode.cpp
        ${_rex_gfx}/util/draw.cpp
        ${_rex_gfx}/util/draw_extent_estimator.cpp
    )
    set_target_properties(rexgpu-fh2 PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        LIBRARY_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:forzahorizon2>)
    target_include_directories(rexgpu-fh2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/vk
        ${_rex_gfx}
        ${CMAKE_CURRENT_SOURCE_DIR}/rexglue-sdk/thirdparty/renderdoc
        ${CMAKE_CURRENT_SOURCE_DIR}/rexglue-sdk)
    target_compile_options(rexgpu-fh2 PRIVATE -Wno-unused-variable)
    target_link_options(rexgpu-fh2 PRIVATE -Wl,--no-undefined)
    target_compile_definitions(rexgpu-fh2 PRIVATE REX_HAS_VULKAN=1)
    target_link_libraries(rexgpu-fh2 PRIVATE rexruntime xxHash::xxhash Vulkan::Headers
        GPUOpen::VulkanMemoryAllocator glslang::SPIRV glslang MachineIndependent
        GenericCodeGen OSDependent OGLCompiler)
    add_dependencies(forzahorizon2 rexgpu-fh2)
endif()
