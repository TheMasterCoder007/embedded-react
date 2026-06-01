add_executable(test_resources test_resources.c)
target_link_libraries(test_resources PRIVATE embedded-react)
# test_resources reaches into the font and image registry internals to verify replace/reuse.
target_include_directories(test_resources PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../font
        ${CMAKE_CURRENT_SOURCE_DIR}/../../rendering)
add_test(NAME resources COMMAND test_resources)
