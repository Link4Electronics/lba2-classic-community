# Host test for GpuRenderer_IsoWorldZO (LIB386/H/GPU/GPURENDERER.H).
# Header-only pure function: no engine sources to link, runs anywhere.
add_executable(test_iso_world_zo test_iso_world_zo.cpp)

target_include_directories(test_iso_world_zo PRIVATE
    ${CMAKE_SOURCE_DIR}/LIB386/H    # GPU/GPURENDERER.H, POLYGON/, SYSTEM/
    ${CMAKE_SOURCE_DIR}/tests       # test_harness.h
)
target_compile_definitions(test_iso_world_zo PRIVATE
    "TEST_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}/\""
)
set_target_properties(test_iso_world_zo PROPERTIES CXX_STANDARD 11)

add_test(NAME test_iso_world_zo COMMAND test_iso_world_zo)
register_host_test(test_iso_world_zo)
