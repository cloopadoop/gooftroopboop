add_subdirectory(main)

if(ENABLE_UI_QT)
  message(STATUS "Building UI (Qt)")
  add_subdirectory(ui/qt)
else()
  add_subdirectory(engine)
endif()

if(ENABLE_CLI)
  message(STATUS "Building CLI application")
  add_subdirectory(ui/cli)
endif()
