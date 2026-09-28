# Vertex record layout: 16 bytes per vertex on disk, converted per vertex
# (Struc_Point's in-memory stride is wider). Links the real recorder so the
# writer's payload layout is what is being asserted.
add_executable(test_polyrec_points
    test_polyrec_points.cpp
    ${CMAKE_SOURCE_DIR}/LIB386/SNAPSHOT/POLY_RECORDING.CPP
)

target_include_directories(test_polyrec_points PRIVATE
    ${CMAKE_SOURCE_DIR}/LIB386/H
)

set_target_properties(test_polyrec_points PROPERTIES CXX_STANDARD 11)

add_test(NAME test_polyrec_points COMMAND test_polyrec_points)

register_host_test(test_polyrec_points)
