set(CMAKE_DISABLE_SOURCE_CHANGES ON)

add_custom_command(OUTPUT "a<" COMMAND b)
add_custom_command(OUTPUT "a>" COMMAND c)
add_custom_command(OUTPUT "$<CONFIG>/$<ANGLE-R>" COMMAND d)
add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/e COMMAND f)
add_custom_command(OUTPUT "$<TARGET_PROPERTY:prop>" COMMAND g)
add_custom_command(OUTPUT "$<OUTPUT_CONFIG:h>" COMMAND h)
add_custom_command(OUTPUT "$<COMMAND_CONFIG:i>" COMMAND i)
