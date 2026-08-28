add_executable(test_input test_input.c)
target_link_libraries(test_input PRIVATE embedded-react)
add_test(NAME input COMMAND test_input)

add_executable(test_transform_singular_hit test_transform_singular_hit.c)
target_link_libraries(test_transform_singular_hit PRIVATE embedded-react)
# test_transform_singular_hit needs engine/rendering (transform.h, er_transform_is_invertible /
# _source_fits — it asserts which side of the invert epsilon each state lands on) and engine/scene
# (er_node_internal.h, for the last_paint_untransformed flag whose stale window is the bug).
target_include_directories(test_transform_singular_hit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rendering ${CMAKE_CURRENT_SOURCE_DIR}/../../scene)
add_test(NAME input_transform_singular_hit COMMAND test_transform_singular_hit)
