add_executable(test_animation test_animation.c)
target_link_libraries(test_animation PRIVATE embedded-react)
# test_animation inspects internal node props to verify animation output.
target_include_directories(test_animation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../scene)
add_test(NAME animation COMMAND test_animation)

add_executable(test_interpolate test_interpolate.c)
target_link_libraries(test_interpolate PRIVATE embedded-react)
add_test(NAME animation_interpolate COMMAND test_interpolate)

add_executable(test_layout_anim test_layout_anim.c)
target_link_libraries(test_layout_anim PRIVATE embedded-react)
target_include_directories(test_layout_anim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../scene)
add_test(NAME animation_layout_anim COMMAND test_layout_anim)
