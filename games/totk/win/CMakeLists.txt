# Windows / x86-64 port of the TotK runtime (branch `windows`). Builds on Windows (MSYS2 CLANG64 or clang-cl) and on
# Linux, where the tests run. The recompiler is an external dependency (ScarletNX's recompiler library, GPL-3.0):
# install it and pass -DCMAKE_PREFIX_PATH=<its install prefix>. Without it only the platform layer builds.
cmake_minimum_required(VERSION 3.20)
project(totk_win C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(WIN_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(BRIDGE_DIR ${WIN_DIR}/bridge)

# the platform layer: no recompiler, no game, no third-party code
add_library(totkwin_platform STATIC runtime/guest_mem.c runtime/module_loader.c)
target_include_directories(totkwin_platform PUBLIC runtime ${BRIDGE_DIR})
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(totkwin_platform PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(platform_test tests/platform_test.c)
target_link_libraries(platform_test PRIVATE totkwin_platform)

# gen_bridge(<target> <protos header> <include name>): generates bridge_nvn.c for those prototypes into the target
function(gen_bridge target protos include)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/${target}_bridge)
  add_custom_command(OUTPUT ${out}/bridge_nvn.c
    COMMAND ${Python3_EXECUTABLE} -I ${BRIDGE_DIR}/gen_bridge.py --nvn-header ${protos} --protos ${protos}
            --include ${include} --out-dir ${out}
    DEPENDS ${BRIDGE_DIR}/gen_bridge.py ${protos})
  target_sources(${target} PRIVATE ${out}/bridge_nvn.c)
endfunction()

find_package(ScarletNXRecompiler QUIET)
if(ScarletNXRecompiler_FOUND)
  add_library(totkwin_recomp STATIC runtime/recomp_host.c)
  target_link_libraries(totkwin_recomp PUBLIC totkwin_platform ScarletNX::recompiler)
  target_compile_options(totkwin_recomp PUBLIC "SHELL:-include ${WIN_DIR}/runtime/guest_cpu.h")

  add_executable(loader_test tests/loader_test.c)
  target_include_directories(loader_test PRIVATE tests)
  target_link_libraries(loader_test PRIVATE totkwin_recomp)
  gen_bridge(loader_test ${WIN_DIR}/tests/test_protos.h test_protos.h)

  # ---- totk-win: the upstream runtime (games/totk/runtime) on the recompiler ----
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "totk-win needs Clang (gen_host_bridge.py reads Clang's AST)")
  endif()
  set(TOTK_DIR ${WIN_DIR}/..)
  set(RUNTIME_DIR ${TOTK_DIR}/runtime)
  option(TOTK_WIN_RUNTIME_CONFIG "read romfs_config.h and shared_fonts_config.h at start (TOTK_WIN_CONFIG_DIR at run
         time) instead of compiling them in: one prebuilt program for every dump" OFF)
  set(TOTK_WIN_CONFIG_DIR "${WIN_DIR}/tests/config" CACHE PATH
      "romfs_config.h and shared_fonts_config.h made from the player's files (the tests' placeholders by default)")
  if(TOTK_WIN_RUNTIME_CONFIG)
    set(TOTK_WIN_CONFIG_DIR "${WIN_DIR}/compat/runtime_config" CACHE PATH "" FORCE)
  endif()
  set(TOTK_WIN_DEFINES "" CACHE STRING "game version defines, as the Mac build passes them (e.g. TOTK_GAME_V143=1)")
  set(UPSTREAM_FLAGS "SHELL:-include ${WIN_DIR}/compat/win_compat.h" -I${WIN_DIR}/compat/include
      -I${TOTK_DIR}/third_party/d2r_gfx/gfx/nvn/include -I${RUNTIME_DIR} -I${TOTK_WIN_CONFIG_DIR})
  foreach(d ${TOTK_WIN_DEFINES})
    list(APPEND UPSTREAM_FLAGS -D${d})
  endforeach()
  set(UPSTREAM_GEN_FLAGS ${UPSTREAM_FLAGS})
  list(REMOVE_AT UPSTREAM_GEN_FLAGS 0)   # the -include pair is spelled out for the generator
  if(NOT WIN32)
    list(APPEND UPSTREAM_FLAGS -Wno-format)   # upstream prints uint64_t with %llx: right on Darwin and Windows, a warning on Linux
  endif()

  # host functions of the upstream files: adapters generated from Clang's AST, one wrapper TU per file
  set(HOST_FILES services.c account.c applet_host.c time_host.c input.c hid_feedback.c fs_host.c romfs.c
      shared_font.c swkbd_host.c)
  set(HB_DIR ${CMAKE_CURRENT_BINARY_DIR}/host_bridge)
  set(HB_OUT ${HB_DIR}/hb_index.c)
  set(HB_DEPS)
  foreach(f ${HOST_FILES})
    get_filename_component(stem ${f} NAME_WE)
    list(APPEND HB_OUT ${HB_DIR}/hb_${stem}.c)
    list(APPEND HB_DEPS ${RUNTIME_DIR}/${f})
  endforeach()
  add_custom_command(OUTPUT ${HB_OUT}
    COMMAND ${Python3_EXECUTABLE} -I ${BRIDGE_DIR}/gen_host_bridge.py --out-dir ${HB_DIR} --runtime-dir ${RUNTIME_DIR}
            --cc ${CMAKE_C_COMPILER} ${HOST_FILES} -- -std=c11 -include ${WIN_DIR}/compat/win_compat.h
            ${UPSTREAM_GEN_FLAGS}
    DEPENDS ${BRIDGE_DIR}/gen_host_bridge.py ${HB_DEPS} ${WIN_DIR}/compat/win_compat.h
    COMMENT "Generating host-function adapters")

  add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/nvn_names.inc
    COMMAND ${Python3_EXECUTABLE} -I ${WIN_DIR}/tools/names_inc.py ${TOTK_DIR}/third_party/d2r_gfx/gfx/nvn/names.txt
            ${CMAKE_CURRENT_BINARY_DIR}/nvn_names.inc
    DEPENDS ${WIN_DIR}/tools/names_inc.py ${TOTK_DIR}/third_party/d2r_gfx/gfx/nvn/names.txt)

  set(UPSTREAM_PLAIN threads.c ipc.c audio_renderer.c audio_effects.c audio_reverb.c audio_mixer.c motion_fusion.c
      autoload.c world_survey.c game_patches.c)
  list(TRANSFORM UPSTREAM_PLAIN PREPEND ${RUNTIME_DIR}/)
  add_library(totkwin_upstream STATIC ${UPSTREAM_PLAIN} ${HB_OUT} ${WIN_DIR}/compat/compat.c ${WIN_DIR}/compat/compat_audio.c)
  target_compile_options(totkwin_upstream PRIVATE ${UPSTREAM_FLAGS})
  target_include_directories(totkwin_upstream PRIVATE runtime ${BRIDGE_DIR})
  target_link_libraries(totkwin_upstream PUBLIC totkwin_recomp)
  if(TOTK_WIN_RUNTIME_CONFIG)
    target_sources(totkwin_upstream PRIVATE runtime/game_config.c)
  endif()

  add_executable(totk-win runtime/host_runtime.c runtime/thread_enter.c runtime/platform_m0.c runtime/totk_win_main.c
                 ${CMAKE_CURRENT_BINARY_DIR}/nvn_names.inc)
  target_compile_options(totk-win PRIVATE ${UPSTREAM_FLAGS})
  target_include_directories(totk-win PRIVATE runtime ${BRIDGE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
  find_package(Threads REQUIRED)
  target_link_libraries(totk-win PRIVATE totkwin_upstream Threads::Threads m)
  if(WIN32)
    target_link_libraries(totk-win PRIVATE bcrypt)
    # keep the CRT heap and other host allocations bottom-up and low: guest-visible host memory must stay below 2^39
    target_link_options(totk-win PRIVATE -Wl,--disable-high-entropy-va)
  endif()
else()
  message(STATUS "ScarletNX recompiler library not found: building the platform layer only")
endif()
