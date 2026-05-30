add_executable(test_node_pool test_node_pool.c)
target_link_libraries(test_node_pool PRIVATE embedded-react)
target_include_directories(test_node_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../scene)
add_test(NAME scene_node_pool COMMAND test_node_pool)
