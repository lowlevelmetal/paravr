# Port access, ISP protocol, Intel HEX, MCU table and saved settings.
add_library(paravr_core STATIC
    hex.cpp
    isp.cpp
    mcu.cpp
    port.cpp
    settings.cpp
)
target_include_directories(paravr_core PUBLIC ${PROJECT_SOURCE_DIR}/include)

# Command-line tool.
add_executable(paravr
    commands.cpp
    main.cpp
)
target_link_libraries(paravr PRIVATE paravr_core)
set_target_properties(paravr PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})

install(TARGETS paravr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
