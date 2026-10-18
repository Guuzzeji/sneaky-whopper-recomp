cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Burger King: Sneak King - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(sneakking_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Web port: compact guest memory, one block with every alias folded onto its
# store and register access through explicit MMIO calls -- the layout
# WebAssembly needs, buildable natively (xbox_memory_layout.h). The lifted code
# must be generated with --mmio-range for the XDK D3D/DSOUND/XPP sections.
option(RECOMP_COMPACT_MEM "Compact guest memory (the WebAssembly layout)" OFF)
option(RECOMP_MMIO_CENSUS "Log lifted functions that touch MMIO" OFF)
if(RECOMP_MMIO_CENSUS)
    add_compile_definitions(RECOMP_MMIO_CENSUS)
endif()
if(RECOMP_COMPACT_MEM)
    add_compile_definitions(XBOX_COMPACT_MEM)
endif()

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# WebAssembly (emcmake): the guest's threads are real threads, so every
# object is built for shared memory.
if(EMSCRIPTEN)
    add_compile_options(-pthread)
    add_link_options(-pthread)
endif()

# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# The renderer: d3d9 (d3d9_hle.c, Direct3D 9, with the F1 overlay) or gl
# (gl_hle.c, OpenGL ES 3 / WebGL2: the web port's, testable here on GL 4.3).
set(RECOMP_RENDERER "d3d9" CACHE STRING "Renderer back end: d3d9 or gl")
set_property(CACHE RECOMP_RENDERER PROPERTY STRINGS d3d9 gl)

# Core game sources
if(EMSCRIPTEN)
    set(RECOMP_RENDERER "gl" CACHE STRING "" FORCE)
    set(GAME_SOURCES src/main_web.c src/web_host.c src/recomp_manual.c)
else()
    set(GAME_SOURCES src/main.c src/recomp_manual.c)
endif()
if(RECOMP_RENDERER STREQUAL "gl")
    list(APPEND GAME_SOURCES src/gl_hle.c)
else()
    list(APPEND GAME_SOURCES
        src/d3d9_hle.c
        # The F1/F2 debug overlay, on the vendored Dear ImGui.
        src/overlay.cpp
        third_party/imgui/imgui.cpp
        third_party/imgui/imgui_draw.cpp
        third_party/imgui/imgui_tables.cpp
        third_party/imgui/imgui_widgets.cpp
        third_party/imgui/imgui_impl_dx9.cpp
        third_party/imgui/imgui_impl_win32.cpp
    )
endif()

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

if(EMSCRIPTEN)
    add_executable(${PROJECT_NAME}
        ${GAME_SOURCES}
        ${RECOMP_GEN_SOURCES}
    )
else()
add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)
endif()

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
    # d3d9_hle.c: the toolkit's swizzle helpers.
    ${XBOXRECOMP_DIR}/src/d3d
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
)

# Windows SDK libraries needed at link time
if(EMSCRIPTEN)
    # The browser build: see docs/WEB_PORT_RESEARCH.md. main() runs on a
    # pthread so the guest can block; the canvas moves to it as an
    # OffscreenCanvas; game files come through WasmFS (main_web.c).
    set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX ".js")
    target_link_options(${PROJECT_NAME} PRIVATE
        -sPROXY_TO_PTHREAD
        -sPTHREAD_POOL_SIZE=32
        -sINITIAL_MEMORY=1536MB
        -sSTACK_SIZE=16MB
        -sDEFAULT_PTHREAD_STACK_SIZE=4MB
        -sWASMFS
        -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
        -sOFFSCREENCANVAS_SUPPORT
        -sENVIRONMENT=web,worker
        -sEXPORTED_RUNTIME_METHODS=ENV
        -sEXIT_RUNTIME=0
        --profiling-funcs
    )
endif()
if(RECOMP_RENDERER STREQUAL "gl" AND WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE opengl32)
endif()
if(WIN32)
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d9
    xinput
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
    # d3d9_hle.c compiles the title's vertex programs at run time.
    d3dcompiler
    # overlay.cpp: the thread table and working set.
    psapi
)
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # Names host RVAs for the RECOMP_SAMPLE_SECS profiler; Release has no PDB.
        /MAP
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
