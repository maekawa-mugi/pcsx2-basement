cmake_minimum_required(VERSION 3.20)
project(avx512_validation LANGUAGES CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  message(FATAL_ERROR "This standalone generated-kernel validation uses the Linux x86-64 ABI.")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(PCSX2_ROOT "../.." ABSOLUTE)
file(GLOB EMITTER_SOURCES "${PCSX2_ROOT}/common/emitter/*.cpp")
file(GLOB GENERATOR_INPUTS "${PCSX2_ROOT}/pcsx2/x86/microVU*.inl")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generators.h" "${CMAKE_CURRENT_BINARY_DIR}/yuv-generators.h" "${CMAKE_CURRENT_BINARY_DIR}/oracle.h" "${CMAKE_CURRENT_BINARY_DIR}/oracle.cpp"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/mmi-generators.h"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py" "${CMAKE_CURRENT_BINARY_DIR}/generators.h" "${CMAKE_CURRENT_BINARY_DIR}/yuv-generators.h" "${CMAKE_CURRENT_BINARY_DIR}/oracle.h" "${CMAKE_CURRENT_BINARY_DIR}/oracle.cpp"
  DEPENDS generate.py ${GENERATOR_INPUTS} "${PCSX2_ROOT}/pcsx2/x86/iMMI.cpp" "${PCSX2_ROOT}/pcsx2/x86/microVU.h" "${PCSX2_ROOT}/pcsx2/x86/iFPU.cpp" "${PCSX2_ROOT}/pcsx2/PS2Float.h" "${PCSX2_ROOT}/pcsx2/PS2Float.inl" "${PCSX2_ROOT}/pcsx2/PS2Float.cpp" "${PCSX2_ROOT}/pcsx2/IPU/yuv2rgb.cpp")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/phase2-allocator-state.h" "${CMAKE_CURRENT_BINARY_DIR}/phase2-allocator-functions.h"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_phase2.py" "${CMAKE_CURRENT_BINARY_DIR}/phase2-allocator-state.h" "${CMAKE_CURRENT_BINARY_DIR}/phase2-allocator-functions.h"
  DEPENDS generate_phase2.py "${PCSX2_ROOT}/pcsx2/x86/iCore.h" "${PCSX2_ROOT}/pcsx2/x86/iCore.cpp"
    "${PCSX2_ROOT}/pcsx2/x86/ix86-32/iCore.cpp" "${PCSX2_ROOT}/pcsx2/x86/ix86-32/iR5900Templates.cpp" "${PCSX2_ROOT}/pcsx2/x86/iMMI.cpp")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/inline-generators.h"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_inline.py" "${CMAKE_CURRENT_BINARY_DIR}/inline-generators.h"
  DEPENDS generate_inline.py ${GENERATOR_INPUTS} "${PCSX2_ROOT}/pcsx2/x86/microVU.h" "${PCSX2_ROOT}/pcsx2/x86/microVU_Misc.h")
add_executable(avx512-validation main.cpp leaf_tests.cpp flag_tests.cpp phase2_allocator_tests.cpp inline_tests.cpp "${CMAKE_CURRENT_BINARY_DIR}/inline-generators.h"
  "${CMAKE_CURRENT_BINARY_DIR}/phase2-allocator-state.h" "${CMAKE_CURRENT_BINARY_DIR}/phase2-allocator-functions.h"
  "${CMAKE_CURRENT_BINARY_DIR}/generators.h" "${CMAKE_CURRENT_BINARY_DIR}/yuv-generators.h"
  "${CMAKE_CURRENT_BINARY_DIR}/oracle.cpp"
  "${PCSX2_ROOT}/pcsx2/x86/InterpreterAVX512.cpp"
  ${EMITTER_SOURCES} "${PCSX2_ROOT}/pcsx2/x86/microVU_SoftFloatTables.cpp"
  "${PCSX2_ROOT}/3rdparty/fmt/src/format.cc")
target_compile_features(avx512-validation PRIVATE cxx_std_20)
target_compile_definitions(avx512-validation PRIVATE PCSX2_DEVBUILD)
target_include_directories(avx512-validation PRIVATE "${PCSX2_ROOT}" "${PCSX2_ROOT}/3rdparty/fmt/include" "${CMAKE_CURRENT_BINARY_DIR}")
# Baseline stays AVX2. AVX-512 is emitted only after CPU/OS feature checks.
target_compile_options(avx512-validation PRIVATE -O2 -msse4.1 -mavx2 -mbmi -mbmi2 -fno-pie -ffunction-sections -fdata-sections)
target_link_options(avx512-validation PRIVATE -no-pie -Wl,--gc-sections)
target_link_libraries(avx512-validation PRIVATE "-l:libzstd.so.1")
enable_testing()
add_test(NAME avx512-kernels COMMAND avx512-validation)
set_tests_properties(avx512-kernels PROPERTIES SKIP_RETURN_CODE 77)
