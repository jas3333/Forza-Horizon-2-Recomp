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
)

if(WIN32)
    add_executable(forzahorizon2 WIN32 ${FORZAHORIZON2_SOURCES})
else()
    add_executable(forzahorizon2 ${FORZAHORIZON2_SOURCES})
endif()

rexglue_setup_target(forzahorizon2 GPU_PLUGINS xenos)

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
