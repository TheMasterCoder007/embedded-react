add_executable(test_rrect test_rrect.c)
target_link_libraries(test_rrect PRIVATE embedded-react)
# test_rrect needs access to engine/rendering for rrect.h (a private engine header).
target_include_directories(test_rrect PRIVATE ${CMAKE_SOURCE_DIR}/engine/rendering)
add_test(NAME rendering_rrect COMMAND test_rrect)
