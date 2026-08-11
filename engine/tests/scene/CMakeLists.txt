add_executable(test_node_pool test_node_pool.c)
target_link_libraries(test_node_pool PRIVATE embedded-react)
target_include_directories(test_node_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../scene)
add_test(NAME scene_node_pool COMMAND test_node_pool)

add_executable(test_damage_set test_damage_set.c)
target_link_libraries(test_damage_set PRIVATE embedded-react)
# test_damage_set needs engine/scene for er_damage_internal.h (a private engine header).
target_include_directories(test_damage_set PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../scene)
add_test(NAME scene_damage_set COMMAND test_damage_set)
