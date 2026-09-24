cmake_minimum_required(VERSION 3.20)
project(cpu_playback LANGUAGES CXX)
set(SNSF9X_SOURCE "$ENV{SNSF9X_SOURCE}" CACHE PATH "External directory containing SNESSystem.cpp")
if(NOT EXISTS "${SNSF9X_SOURCE}/SNESSystem.cpp")
  message(FATAL_ERROR "Set SNSF9X_SOURCE to your local emulator source directory")
endif()
set(core "${SNSF9X_SOURCE}/snes9x")
# Generate just the instrumented translation unit in the ignored build directory.
file(READ "${core}/cpuexec.cpp" cpu)
set(anchor "\t\tCPU.PrevCycles = CPU.Cycles;")
string(FIND "${cpu}" "${anchor}" hook_position)
if(hook_position LESS 0)
  message(FATAL_ERROR "Unsupported CPU loop: observation anchor missing")
endif()
string(REPLACE "${anchor}" "\t\textern void PlaybackObserve();\n\t\tPlaybackObserve();\n${anchor}" cpu "${cpu}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/observed_cpuexec.cpp" "${cpu}")
set(sources cpu cpuops dma globals memmap ppu sa1 sdd1)
set(apu_sources apu SNES_SPC SNES_SPC_misc SNES_SPC_state SPC_DSP SPC_Filter)
add_executable(cpu_playback playback.cpp "${SNSF9X_SOURCE}/SNESSystem.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/observed_cpuexec.cpp")
foreach(source IN LISTS sources)
  target_sources(cpu_playback PRIVATE "${core}/${source}.cpp")
endforeach()
foreach(source IN LISTS apu_sources)
  target_sources(cpu_playback PRIVATE "${core}/apu/${source}.cpp")
endforeach()
target_include_directories(cpu_playback PRIVATE "${SNSF9X_SOURCE}" "${core}")
target_compile_features(cpu_playback PRIVATE cxx_std_14)
target_compile_definitions(cpu_playback PRIVATE HAVE_STDINT_H HAVE_INTTYPES_H HAVE_STDBOOL_H
  __WIN32__ WIN32 _CRT_SECURE_NO_WARNINGS __x86_64__)
# This legacy port.h recognizes _M_IX86 but not MSVC's _M_X64. All supported
# builds here target x64 Windows. Expose the recognized x64 spelling so both CPU
# and APU agree on little endian (LSB_FIRST alone also leaves MSB_FIRST set).
