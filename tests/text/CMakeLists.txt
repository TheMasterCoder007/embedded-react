add_executable(test_text test_text.c)
target_link_libraries(test_text PRIVATE embedded-react)
target_include_directories(test_text PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME text COMMAND test_text)
