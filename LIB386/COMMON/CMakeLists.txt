add_library(gpur GPURENDERER_COMMON.CPP)

target_include_directories(gpur PUBLIC ${CMAKE_SOURCE_DIR}/LIB386/H)
target_compile_definitions(gpur PRIVATE USE_GPURENDERER)
target_link_libraries(gpur PUBLIC sys)