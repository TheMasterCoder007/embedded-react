add_executable(test_rrect test_rrect.c)
target_link_libraries(test_rrect PRIVATE embedded-react)
# test_rrect needs access to engine/rendering for rrect.h (a private engine header).
target_include_directories(test_rrect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rendering)
add_test(NAME rendering_rrect COMMAND test_rrect)

add_executable(test_image test_image.c)
target_link_libraries(test_image PRIVATE embedded-react)
add_test(NAME rendering_image COMMAND test_image)
