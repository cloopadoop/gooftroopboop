# The Tauri sidecar and model tests need Qt Core, not the legacy widget UI.
find_package(Qt6 REQUIRED COMPONENTS Core)
set(MODEL_DIR "${PROJECT_SOURCE_DIR}/src/ui/qt/workarea")
set(MODEL_FILES "${MODEL_DIR}/CapcomPianoRollModel.cpp" "${MODEL_DIR}/CapcomPianoRollModel.h")
file(GLOB SNES_SPC_SOURCES "${PROJECT_SOURCE_DIR}/lib/snes_spc/*.cpp")
add_executable(gtb-engine EngineMain.cpp ${SNES_SPC_SOURCES} ${MODEL_FILES})
add_executable(gtb-model-test "${PROJECT_SOURCE_DIR}/src/ui/qt/tests/ModelEditTest.cpp" ${MODEL_FILES})
foreach(target IN ITEMS gtb-engine gtb-model-test)
  target_include_directories(${target} PRIVATE
    "${PROJECT_SOURCE_DIR}/src/ui/qt" "${MODEL_DIR}"
    "${PROJECT_SOURCE_DIR}/lib" "${PROJECT_SOURCE_DIR}/lib/snes_spc")
  target_link_libraries(${target} PRIVATE GTBoopcore Qt6::Core)
  set_target_properties(${target} PROPERTIES AUTOMOC ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<$<CONFIG:Debug>:>")
endforeach()
