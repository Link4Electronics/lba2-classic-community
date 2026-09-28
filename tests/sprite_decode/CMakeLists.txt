# Host test for GpuSprite_Decode* (LIB386/H/GPU/GPUSPRITE_DECODE.H).
# Header-only pure decode: no engine sources to link, runs anywhere.
add_executable(test_sprite_decode test_sprite_decode.cpp)

target_include_directories(test_sprite_decode PRIVATE
    ${CMAKE_SOURCE_DIR}/LIB386/H    # GPU/, SYSTEM/
    ${CMAKE_SOURCE_DIR}/tests       # test_harness.h
)
set_target_properties(test_sprite_decode PROPERTIES CXX_STANDARD 11)

add_test(NAME test_sprite_decode COMMAND test_sprite_decode)
register_host_test(test_sprite_decode)
