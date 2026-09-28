# Host test for GpuPresent_DirtyBounds / GpuPresent_PlanUpload
# (LIB386/H/GPU/GPUPRESENT_DIRTY.H): the Log overlay upload rect.
add_executable(test_gpu_present_dirty test_gpu_present_dirty.cpp)

target_include_directories(test_gpu_present_dirty PRIVATE
    ${CMAKE_SOURCE_DIR}/LIB386/H    # GPU/, SVGA/, SYSTEM/
    ${CMAKE_SOURCE_DIR}/tests       # test_harness.h
)

set_target_properties(test_gpu_present_dirty PROPERTIES CXX_STANDARD 11)

add_test(NAME test_gpu_present_dirty COMMAND test_gpu_present_dirty)
register_host_test(test_gpu_present_dirty)
