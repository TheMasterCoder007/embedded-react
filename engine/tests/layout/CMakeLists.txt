add_executable(test_layout test_layout.c)
target_link_libraries(test_layout PRIVATE embedded-react)
add_test(NAME layout COMMAND test_layout)

add_executable(test_yoga_parity test_yoga_parity.c)
target_link_libraries(test_yoga_parity PRIVATE embedded-react)
add_test(NAME yoga_parity COMMAND test_yoga_parity)
