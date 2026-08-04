cmake_minimum_required(VERSION 3.16)
project(chip8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The interpreter itself, with no dependency on Raylib or the platform. Kept
# as its own target so the tests and the headless runner can link it without
# dragging a window system in.
add_library(chip8_core STATIC src/chip8.cpp)
target_include_directories(chip8_core PUBLIC src)

if(MSVC)
  target_compile_options(chip8_core PRIVATE /W4)
else()
  target_compile_options(chip8_core PRIVATE -Wall -Wextra)
endif()

# --- tests and the headless runner ------------------------------------------
# Both are plain executables with no Raylib dependency, so they build and run
# in CI where there is no GPU.

enable_testing()

add_executable(chip8_tests tests/test_chip8.cpp)
target_link_libraries(chip8_tests PRIVATE chip8_core)
add_test(NAME core COMMAND chip8_tests)

add_executable(render_rom tools/render_rom.cpp)
target_link_libraries(render_rom PRIVATE chip8_core)

# --- the Raylib frontend -----------------------------------------------------
# Off-switch rather than an always-on dependency, because FetchContent runs at
# configure time: without it, merely configuring the project pulls in Raylib
# and, on Linux, fails outright on a machine with no X11 development headers.
# The core and its tests genuinely do not need any of that, and CI builds them
# with this OFF.
option(CHIP8_BUILD_FRONTEND "Build the Raylib window and debugger" ON)

if(NOT CHIP8_BUILD_FRONTEND)
  message(STATUS "Raylib frontend disabled; building the core and tools only")
  return()
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Raylib builds its examples and its own tests by default, which roughly
# triples the build time and is never wanted here.
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

add_executable(chip8 src/main.cpp)
target_link_libraries(chip8 PRIVATE chip8_core raylib)

if(EMSCRIPTEN)
  # index.html rather than chip8.js, so the output drops straight into a
  # static host with no rewrite rules.
  set_target_properties(chip8 PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")

  # --preload-file bakes the ROMs into a .data file next to the wasm, so the
  # emulator has something to run with no upload and no network fetch.
  # _malloc and _free are needed because the page hands ROM bytes to the core
  # through the WASM heap; HEAPU8 is how it writes them there.
  target_link_options(chip8 PRIVATE
    -sUSE_GLFW=3
    -sALLOW_MEMORY_GROWTH=1
    -sFORCE_FILESYSTEM=1
    -sSTACK_SIZE=1048576
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_LoadRomFromMemory,_UiCommand
    -sEXPORTED_RUNTIME_METHODS=HEAPU8,ccall,cwrap
    # SHELL: on both, or CMake sees --preload-file twice, decides it is a
    # repeated flag and drops the second one - which leaves the path after it
    # sitting on the command line as though it were an input file.
    "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/roms@/roms"
    "SHELL:--preload-file ${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets"
    --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html
  )
  # Everything --preload-file bakes into index.data has to be a link
  # dependency, or the build is silently stale. Only the shell was listed here,
  # so reassembling a ROM left ninja with "no work to do" and the old bytes
  # still in index.data - the source changed, the site did not, and nothing
  # said so. Globbed at configure time, so a genuinely new file needs a
  # re-configure; CONFIGURE_DEPENDS makes CMake do that by itself.
  file(GLOB CHIP8_PRELOADED CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/roms/*.ch8
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/*)
  set_property(TARGET chip8 APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html
    ${CHIP8_PRELOADED})
endif()
