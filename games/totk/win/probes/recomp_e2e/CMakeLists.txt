cmake_minimum_required(VERSION 3.20)
project(recomp_e2e C)
# The recompiler is an external GPL-3.0 dependency: build and install ScarletNX's recompiler library, then pass
# -DCMAKE_PREFIX_PATH=<its install prefix>. Nothing of it is in this repository.
find_package(ScarletNXRecompiler REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(BRIDGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../bridge)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gen/bridge_nvn.c
  COMMAND ${Python3_EXECUTABLE} -I ${BRIDGE_DIR}/gen_bridge.py --nvn-header ${CMAKE_CURRENT_SOURCE_DIR}/bridge_protos.h
          --protos ${CMAKE_CURRENT_SOURCE_DIR}/bridge_protos.h --include bridge_protos.h --out-dir ${CMAKE_CURRENT_BINARY_DIR}/gen
  DEPENDS ${BRIDGE_DIR}/gen_bridge.py bridge_protos.h)
# native reference: the same guest.c for the host, SSE baseline (4 float lanes, as NEON)
add_library(ref OBJECT guest.c)
target_compile_options(ref PRIVATE -O2 -march=x86-64-v2)
target_compile_definitions(ref PRIVATE guest_calls=ref_calls guest_fp=ref_fp guest_int=ref_int)
add_executable(recomp_e2e host.c ${CMAKE_CURRENT_BINARY_DIR}/gen/bridge_nvn.c $<TARGET_OBJECTS:ref>)
target_include_directories(recomp_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${BRIDGE_DIR})
target_compile_options(recomp_e2e PRIVATE -O2 -include ${CMAKE_CURRENT_SOURCE_DIR}/snx_guestcpu.h)
target_link_libraries(recomp_e2e PRIVATE ScarletNX::recompiler)
