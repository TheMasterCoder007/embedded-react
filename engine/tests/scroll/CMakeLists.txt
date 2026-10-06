add_executable(test_scroll test_scroll.c)
target_link_libraries(test_scroll PRIVATE embedded-react)
add_test(NAME scroll COMMAND test_scroll)

add_executable(test_scroll_copy test_scroll_copy.c)
target_link_libraries(test_scroll_copy PRIVATE embedded-react)
add_test(NAME scroll_copy COMMAND test_scroll_copy)
