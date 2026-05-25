add_executable(test_layout test_layout.c)
target_link_libraries(test_layout PRIVATE embedded-react)
add_test(NAME layout COMMAND test_layout)
