add_executable(test_text test_text.c)
target_link_libraries(test_text PRIVATE embedded-react)
# test_text needs access to engine/text for text_renderer.h (a private engine header).
target_include_directories(test_text PRIVATE ${CMAKE_SOURCE_DIR}/engine/text)
add_test(NAME text COMMAND test_text)
