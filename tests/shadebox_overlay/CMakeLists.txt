# Host test for ShadeBoxBlkOverlay (new dual-path helper in FLOW_A.CPP).
# No ASM original — pure overlay-darken expectations, runs in host_quick.
add_executable(test_shadebox_overlay
    test_shadebox_overlay.cpp
    ${CMAKE_SOURCE_DIR}/SOURCES/FLOW_A.CPP
)

target_include_directories(test_shadebox_overlay PRIVATE
    ${CMAKE_SOURCE_DIR}/SOURCES     # FLOW.H
    ${CMAKE_SOURCE_DIR}/LIB386/H    # SVGA/, POLYGON/, SYSTEM/
    ${CMAKE_SOURCE_DIR}/tests       # test_harness.h
)
target_compile_definitions(test_shadebox_overlay PRIVATE
    "TEST_SOURCE_ROOT=\"${CMAKE_SOURCE_DIR}/\""
)
set_target_properties(test_shadebox_overlay PROPERTIES CXX_STANDARD 11)

add_test(NAME test_shadebox_overlay COMMAND test_shadebox_overlay)
register_host_test(test_shadebox_overlay)
