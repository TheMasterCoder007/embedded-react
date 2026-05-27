add_executable(test_animation test_animation.c)
target_link_libraries(test_animation PRIVATE embedded-react)
# test_animation inspects internal node props to verify animation output.
target_include_directories(test_animation PRIVATE ${CMAKE_SOURCE_DIR}/engine/scene)
add_test(NAME animation COMMAND test_animation)
