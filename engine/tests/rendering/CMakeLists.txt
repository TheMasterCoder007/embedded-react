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

add_executable(test_transform test_transform.c)
target_link_libraries(test_transform PRIVATE embedded-react)
add_test(NAME rendering_transform COMMAND test_transform)

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
