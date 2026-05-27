add_executable(test_input test_input.c)
target_link_libraries(test_input PRIVATE embedded-react)
add_test(NAME input COMMAND test_input)
