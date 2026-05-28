add_executable(test_scroll test_scroll.c)
target_link_libraries(test_scroll PRIVATE embedded-react)
add_test(NAME scroll COMMAND test_scroll)
