enable_language(C)

# Simulated AVR target behind /dev/parport0, injected into paravr with LD_PRELOAD.
add_library(avrsim MODULE avrsim/avrsim.c)
target_link_libraries(avrsim PRIVATE ${CMAKE_DL_LIBS})

add_executable(unit_tests unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE paravr_core)
add_test(NAME unit COMMAND unit_tests)

add_executable(cli_tests cli_tests.cpp)
target_link_libraries(cli_tests PRIVATE paravr_core)
add_dependencies(cli_tests paravr avrsim)
add_test(NAME cli
    COMMAND cli_tests $<TARGET_FILE:paravr> $<TARGET_FILE:avrsim> ${CMAKE_CURRENT_BINARY_DIR}/cli_work)
