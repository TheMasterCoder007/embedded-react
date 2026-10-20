add_executable(test_layout test_layout.c)
target_link_libraries(test_layout PRIVATE embedded-react)
# test_layout measures text the way the renderer breaks it, through text_renderer.h (a private engine header).
target_include_directories(test_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../text)
add_test(NAME layout COMMAND test_layout)

add_executable(test_yoga_parity test_yoga_parity.c)
target_link_libraries(test_yoga_parity PRIVATE embedded-react)
add_test(NAME yoga_parity COMMAND test_yoga_parity)
