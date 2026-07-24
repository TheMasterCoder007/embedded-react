add_executable(test_rrect test_rrect.c)
target_link_libraries(test_rrect PRIVATE embedded-react)
# test_rrect needs access to engine/rendering for rrect.h (a private engine header).
target_include_directories(test_rrect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rendering)
add_test(NAME rendering_rrect COMMAND test_rrect)

add_executable(test_image test_image.c)
target_link_libraries(test_image PRIVATE embedded-react)
add_test(NAME rendering_image COMMAND test_image)

add_executable(test_opacity test_opacity.c)
target_link_libraries(test_opacity PRIVATE embedded-react)
add_test(NAME rendering_opacity COMMAND test_opacity)

find_package(Threads REQUIRED)
add_executable(test_parallel test_parallel.c)
target_link_libraries(test_parallel PRIVATE embedded-react Threads::Threads)
# test_parallel needs engine/core for renderer_internal.h (er_parallel_for / worker contexts).
target_include_directories(test_parallel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_parallel COMMAND test_parallel)

add_executable(test_scratch_copy test_scratch_copy.c)
target_link_libraries(test_scratch_copy PRIVATE embedded-react)
# test_scratch_copy needs engine/core for renderer_internal.h (er_scratch_begin / er_blit_copy).
target_include_directories(test_scratch_copy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_scratch_copy COMMAND test_scratch_copy)

add_executable(test_transform test_transform.c)
target_link_libraries(test_transform PRIVATE embedded-react)
add_test(NAME rendering_transform COMMAND test_transform)

add_executable(test_transform_damage test_transform_damage.c)
target_link_libraries(test_transform_damage PRIVATE embedded-react)
add_test(NAME rendering_transform_damage COMMAND test_transform_damage)

add_executable(test_multibuffer test_multibuffer.c)
target_link_libraries(test_multibuffer PRIVATE embedded-react)
add_test(NAME rendering_multibuffer COMMAND test_multibuffer)

add_executable(test_shadow test_shadow.c)
target_link_libraries(test_shadow PRIVATE embedded-react)
add_test(NAME rendering_shadow COMMAND test_shadow)

add_executable(test_components test_components.c)
target_link_libraries(test_components PRIVATE embedded-react)
target_include_directories(test_components PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../scene)
add_test(NAME rendering_components COMMAND test_components)

add_executable(test_gradient test_gradient.c)
target_link_libraries(test_gradient PRIVATE embedded-react)
add_test(NAME rendering_gradient COMMAND test_gradient)

add_executable(test_vector test_vector.c)
target_link_libraries(test_vector PRIVATE embedded-react)
# test_vector needs engine/rendering for vector.h (a private engine header).
target_include_directories(test_vector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rendering)
add_test(NAME rendering_vector COMMAND test_vector)
