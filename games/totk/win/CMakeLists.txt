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
  target_compile_options(totkwin_recomp PUBLIC -include ${WIN_DIR}/runtime/guest_cpu.h)

  add_executable(loader_test tests/loader_test.c)
  target_include_directories(loader_test PRIVATE tests)
  target_link_libraries(loader_test PRIVATE totkwin_recomp)
  gen_bridge(loader_test ${WIN_DIR}/tests/test_protos.h test_protos.h)
else()
  message(STATUS "ScarletNX recompiler library not found: building the platform layer only")
endif()
