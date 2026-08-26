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
add_executable(test_opacity_equiv test_opacity_equiv.c)
target_link_libraries(test_opacity_equiv PRIVATE embedded-react)
# test_opacity_equiv needs engine/core for renderer_internal.h (er_force_full_repaint, the reference frame).
target_include_directories(test_opacity_equiv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_opacity_equiv COMMAND test_opacity_equiv)

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

add_executable(test_reflow_damage test_reflow_damage.c)
target_link_libraries(test_reflow_damage PRIVATE embedded-react)
# test_reflow_damage needs engine/core for renderer_internal.h (er_force_full_repaint).
target_include_directories(test_reflow_damage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_reflow_damage COMMAND test_reflow_damage)

add_executable(test_occlusion test_occlusion.c)
target_link_libraries(test_occlusion PRIVATE embedded-react)
# test_occlusion needs engine/core for renderer_internal.h (er_force_full_repaint).
target_include_directories(test_occlusion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_occlusion COMMAND test_occlusion)

add_executable(test_transform test_transform.c)
target_link_libraries(test_transform PRIVATE embedded-react)
add_test(NAME rendering_transform COMMAND test_transform)

add_executable(test_transform_damage test_transform_damage.c)
target_link_libraries(test_transform_damage PRIVATE embedded-react)
add_test(NAME rendering_transform_damage COMMAND test_transform_damage)

add_executable(test_transform_shadow_damage test_transform_shadow_damage.c)
target_link_libraries(test_transform_shadow_damage PRIVATE embedded-react)
# test_transform_shadow_damage needs engine/core for renderer_internal.h (er_force_full_repaint, the
# reference frame it compares each damage-clipped frame against).
target_include_directories(test_transform_shadow_damage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_transform_shadow_damage COMMAND test_transform_shadow_damage)

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

add_executable(test_damage_rects test_damage_rects.c)
target_link_libraries(test_damage_rects PRIVATE embedded-react)
# test_damage_rects needs engine/core for renderer_internal.h (er_force_full_repaint, the reference frame).
target_include_directories(test_damage_rects PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_damage_rects COMMAND test_damage_rects)

add_executable(test_modal_damage test_modal_damage.c)
target_link_libraries(test_modal_damage PRIVATE embedded-react)
add_test(NAME rendering_modal_damage COMMAND test_modal_damage)

add_executable(test_perf test_perf.c)
target_link_libraries(test_perf PRIVATE embedded-react)
add_test(NAME rendering_perf COMMAND test_perf)

add_executable(test_vector test_vector.c)
target_link_libraries(test_vector PRIVATE embedded-react)
# test_vector needs engine/rendering for vector.h (a private engine header).
target_include_directories(test_vector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rendering)
add_test(NAME rendering_vector COMMAND test_vector)

add_executable(test_display_none test_display_none.c)
target_link_libraries(test_display_none PRIVATE embedded-react)
add_test(NAME rendering_display_none COMMAND test_display_none)

add_executable(test_arc test_arc.c)
target_link_libraries(test_arc PRIVATE embedded-react)
# test_arc needs engine/rendering (arc.h), engine/scene (er_node_internal.h) and engine/core (er_force_full_repaint).
target_include_directories(test_arc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../rendering ${CMAKE_CURRENT_SOURCE_DIR}/../../scene ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_arc COMMAND test_arc)

add_executable(test_overflow_damage test_overflow_damage.c)
target_link_libraries(test_overflow_damage PRIVATE embedded-react)
# test_overflow_damage needs engine/core for renderer_internal.h (er_force_full_repaint, the reference frame).
target_include_directories(test_overflow_damage PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core)
add_test(NAME rendering_overflow_damage COMMAND test_overflow_damage)
