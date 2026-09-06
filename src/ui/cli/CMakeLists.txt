add_executable(
  GTBoop-cli
  CLIGTBRoot.cpp
  main_cli.cpp
)

target_include_directories(GTBoop-cli PUBLIC "${PROJECT_BINARY_DIR}/src")

target_link_libraries(GTBoop-cli PRIVATE GTBoopcore)
