include_directories(H)

# LIB386 debug traces are an ASM-vs-CPP equivalence check tool — every shim in
# LIB386_TRACE_CPP / LIB386_TRACE_ASM prints "[CPP][Func][N] ..." lines that the
# ASM equivalence tests diff against the matching "[ASM]" lines. The macros are
# defined inline as ((void)0) when ENABLE_LIB386_DEBUG_TRACES isn't set.
#
# This define was previously applied at directory scope (add_definitions when
# LBA2_BUILD_TESTS was on), which leaked into every target built in the LIB386
# subdirs — including the static libraries linked into the shipping engine
# binary. The engine then wrote ~22K [CPP] lines per tick to stdout on any
# build configured with -DLBA2_BUILD_TESTS=ON (e.g. the default `make build`
# after a CI-style configure). Scope of the leak: ~58% of sys time in
# projection_demo on a WSL2 setup.
#
# The fix: only the test executables that need the traces opt in, via
# target_compile_definitions(<test> PRIVATE ENABLE_LIB386_DEBUG_TRACES) inside
# the add_asm_cpp_test() helper. Engine targets stay clean.

add_subdirectory(3D)
add_subdirectory(AIL)
add_subdirectory(ANIM)
add_subdirectory(FILEIO)
add_subdirectory(OBJECT)
add_subdirectory(pol_work)
add_subdirectory(SVGA)
add_subdirectory(SYSTEM)

# The GL renderer core. Built unconditionally: when GPURENDERER is on it is
# the working backend, otherwise RENDER_GL.CPP compiles the stub set that
# keeps every unconditional caller (WINDOW, SDL, menu, config) linkable.
add_subdirectory(GL)

# Common GPU renderer code shared by both backends. Built unconditionally for
# the same reason as GL: its !USE_GPURENDERER half compiles the GpuRenderer_*
# stubs that keep every unconditional caller (menu, config, RENDER_SWITCH)
# linkable when the renderer is off. Its backend table is built from the two
# backend vtables, so both backends must link whenever it does — the
# SDL3GPU gate below keeps them together.
add_subdirectory(COMMON)

if(GPURENDERER)
  add_subdirectory(SDL3GPU)
else()
  # Link target only: there is no SDL3-GPU code in an OFF build. An interface
  # library satisfies the engine's unconditional `gs3` in its link line
  # without needing a stub TU.
  add_library(gs3 INTERFACE)
endif()

if(ENABLE_POLY_RECORDING OR LBA2_BUILD_TESTS)
  add_subdirectory(SNAPSHOT)
endif()

if(MVIDEO_BACKEND STREQUAL "smacker")
  add_subdirectory(libsmacker)
endif()

if(MVIDEO_BACKEND STREQUAL "smacker" OR LBA2_BUILD_TESTS)
  add_subdirectory(VIDEO_AUDIO_RESAMPLE)
endif()
