# Host test for GpuBlend_Walk (LIB386/H/GPU/GPUBLEND_WALK.H), golden
# against the real ScaleSpriteTransp linked from svg.
add_executable(test_gpu_blend_walk test_gpu_blend_walk.cpp)

target_include_directories(test_gpu_blend_walk PRIVATE
    ${CMAKE_SOURCE_DIR}/LIB386/H    # GPU/, SVGA/, SYSTEM/
    ${CMAKE_SOURCE_DIR}/tests       # test_harness.h
)

target_link_libraries(test_gpu_blend_walk PRIVATE svg poly sys SDL3::SDL3)

set_target_properties(test_gpu_blend_walk PROPERTIES CXX_STANDARD 11)

add_test(NAME test_gpu_blend_walk COMMAND test_gpu_blend_walk)
register_host_test(test_gpu_blend_walk)
