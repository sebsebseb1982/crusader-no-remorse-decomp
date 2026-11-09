cmake_minimum_required(VERSION 3.21)
project(crusader_port LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# SDL2: use the system package when available (Linux distros), otherwise build it.
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz)
  FetchContent_MakeAvailable(SDL2)
endif()
if(TARGET SDL2::SDL2-static)
  set(SDL2_TARGET SDL2::SDL2-static)
else()
  set(SDL2_TARGET SDL2::SDL2)
endif()

# Game data formats, no platform dependency.
add_library(crusader_core
  src/core/flex.cpp
  src/core/palette.cpp
  src/core/shape.cpp
  src/core/xform.cpp
  src/core/weapon_overlay.cpp
  src/core/font.cpp
  src/core/game_config.cpp
  src/core/data_info.cpp
  src/core/iso_image.cpp
  src/core/dtable.cpp
  src/core/language.cpp
  src/core/typeflags.cpp
  src/core/trig.cpp
  src/core/random.cpp
  src/core/anim_dat.cpp
  src/core/sound_flex.cpp
  src/core/amf_module.cpp
  src/core/png_writer.cpp
  src/core/avi_movie.cpp
  src/core/movie_subtitles.cpp)
target_include_directories(crusader_core PUBLIC src)

# Engine core: process kernel (KERNEL.C / PROCESS.C), no platform dependency.
add_library(crusader_engine
  src/engine/kernel.cpp
  src/engine/process.cpp
  src/engine/palette_fader.cpp
  src/engine/usecode/usecode_file.cpp
  src/engine/usecode/uc_heap.cpp
  src/engine/usecode/uc_process.cpp
  src/engine/usecode/uc_machine.cpp
  src/engine/usecode/item_intrinsics.cpp
  src/engine/usecode/world_intrinsics.cpp
  src/engine/usecode/npc_intrinsics.cpp
  src/engine/usecode/audio_intrinsics.cpp
  src/engine/usecode/movie_intrinsics.cpp
  src/engine/usecode/intrinsic_numbering.cpp
  src/engine/audio/asylum_mixer.cpp
  src/engine/audio/asylum_music.cpp
  src/engine/audio/music.cpp
  src/engine/audio/sound_manager.cpp
  src/engine/audio/sound_sources.cpp
  src/engine/world/world.cpp
  src/engine/world/item_search.cpp
  src/engine/world/actor_anim.cpp
  src/engine/world/collision.cpp
  src/engine/world/avatar_mover.cpp
  src/engine/world/gravity.cpp
  src/engine/world/combat_control.cpp
  src/engine/world/projectile.cpp
  src/engine/world/attack.cpp
  src/engine/world/activity.cpp
  src/engine/world/actor_create.cpp
  src/engine/world/pathfind.cpp
  src/engine/world/player_commands.cpp
  src/engine/world/cheats.cpp
  src/engine/world/item_anim.cpp
  src/engine/world/item_damage.cpp
  src/engine/world/chargers.cpp
  src/engine/world/inventory.cpp
  src/engine/world/item_selection.cpp
  src/engine/world/map_change.cpp
  src/engine/world/teleport.cpp
  src/engine/world/sprite_process.cpp
  src/engine/world/aim_mode.cpp
  src/engine/world/world_mouse.cpp
  src/engine/world/palette_cycle.cpp
  src/engine/world/pickup_list.cpp
  src/engine/input/keyboard.cpp
  src/engine/input/mouse_queue.cpp
  src/engine/ui/status_bar.cpp
  src/engine/ui/pickup_area.cpp
  src/engine/ui/computer_gump.cpp
  src/engine/ui/keypad_gump.cpp
  src/engine/ui/weasel_gump.cpp
  src/engine/ui/mouse_cursor.cpp
  src/engine/ui/main_menu.cpp
  src/engine/ui/load_screen.cpp
  src/engine/ui/options_screen.cpp
  src/engine/ui/difficulty_screen.cpp
  src/engine/ui/message_gump.cpp
  src/engine/ui/shape_dialog.cpp
  src/engine/ui/help_screen.cpp
  src/engine/ui/credits.cpp
  src/engine/ui/gamepad_screen.cpp
  src/engine/ui/enhancements_screen.cpp
  src/engine/ui/movie_player.cpp
  src/engine/world/auto_firer.cpp
  src/engine/world/render.cpp)
target_link_libraries(crusader_engine PUBLIC crusader_core)

# Platform layer (replaces SUPERVGA/VPORT/BASEPAL hardware access).
add_library(crusader_platform
  src/platform/sdl_presenter.cpp
  src/platform/sdl_input.cpp
  src/platform/sdl_gamepad.cpp
  src/platform/sdl_audio.cpp)
target_link_libraries(crusader_platform PUBLIC crusader_engine ${SDL2_TARGET})

# The game without platform code (testable headless), and its executable.
add_library(crusader_game src/game/game.cpp)
target_link_libraries(crusader_game PUBLIC crusader_engine)

if(WIN32)
  # Embed the application icon (Explorer, taskbar, window title bar).
  add_executable(crusader src/game/main.cpp src/game/res/crusader.rc)
else()
  add_executable(crusader src/game/main.cpp)
endif()
target_link_libraries(crusader PRIVATE crusader_game crusader_platform)
# Set by the release pipeline (semantic version, without the leading "v").
set(CRUSADER_VERSION "0.0.0-dev" CACHE STRING "Version reported by crusader --version")
configure_file(src/game/version.hpp.in ${CMAKE_BINARY_DIR}/generated/crusader_version.hpp @ONLY)
target_include_directories(crusader PRIVATE ${CMAKE_BINARY_DIR}/generated)

add_executable(shapeview src/tools/shapeview.cpp)
target_link_libraries(shapeview PRIVATE crusader_platform)

add_executable(mapview src/tools/mapview.cpp)
target_link_libraries(mapview PRIVATE crusader_engine crusader_platform)

add_executable(sortcheck src/tools/sortcheck.cpp)
target_link_libraries(sortcheck PRIVATE crusader_engine)

add_executable(ucsmoke src/tools/ucsmoke.cpp)
target_link_libraries(ucsmoke PRIVATE crusader_engine)

add_executable(diffcheck src/tools/diffcheck.cpp)
target_link_libraries(diffcheck PRIVATE crusader_engine)

add_executable(useprobe src/tools/useprobe.cpp)
target_link_libraries(useprobe PRIVATE crusader_game)

add_executable(shapedump src/tools/shapedump.cpp)
target_link_libraries(shapedump PRIVATE crusader_core)

# Level editor: the model (FIXED.DAT, GLOB.FLX, egg parameters), testable
# without a window, and the editor itself (Dear ImGui over SDL2).
add_library(crusader_editor_core
  src/editor/level.cpp
  src/editor/item_semantics.cpp
  src/editor/project.cpp)
target_link_libraries(crusader_editor_core PUBLIC crusader_core)

option(CRUSADER_BUILD_EDITOR "Build the level editor (fetches Dear ImGui)" ON)
if(CRUSADER_BUILD_EDITOR)
  include(FetchContent)
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.91.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui)
  add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
  target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(imgui PUBLIC ${SDL2_TARGET})
  # Third-party code: no warnings from it.
  if(MSVC)
    target_compile_options(imgui PRIVATE /W0)
  else()
    target_compile_options(imgui PRIVATE -w)
  endif()

  if(WIN32)
    add_executable(crusader-editor src/editor/editor_main.cpp src/editor/editor_app.cpp src/game/res/crusader.rc)
  else()
    add_executable(crusader-editor src/editor/editor_main.cpp src/editor/editor_app.cpp)
  endif()
  target_link_libraries(crusader-editor PRIVATE crusader_editor_core crusader_engine imgui)
endif()

enable_testing()
# tests/: no game data needed, run by the GitHub CI.
# e2e/: end-to-end tests on the original game data and on saved games, none of
# which is in the repository. They are registered only with
# CRUSADER_GAME_DATA_TESTS=ON, the default when the game data is found.
set(CRUSADER_DATA_DIR "${CMAKE_SOURCE_DIR}/data" CACHE PATH "Original game data used by the e2e tests")
set(CRUSADER_TEST_SAVES_DIR "${CMAKE_SOURCE_DIR}/private/e2e/saves" CACHE PATH "Saved games used by the e2e tests")
if(EXISTS "${CRUSADER_DATA_DIR}/STATIC")
    set(CRUSADER_HAVE_GAME_DATA ON)
else()
    set(CRUSADER_HAVE_GAME_DATA OFF)
endif()
option(CRUSADER_GAME_DATA_TESTS "Register the end-to-end tests (e2e/) that need the game data" ${CRUSADER_HAVE_GAME_DATA})

add_executable(core_tests tests/core_tests.cpp)
target_link_libraries(core_tests PRIVATE crusader_core)
add_test(NAME core_tests COMMAND core_tests)

add_executable(kernel_tests tests/kernel_tests.cpp)
target_link_libraries(kernel_tests PRIVATE crusader_engine)
add_test(NAME kernel_tests COMMAND kernel_tests)

add_executable(input_tests tests/input_tests.cpp)
target_link_libraries(input_tests PRIVATE crusader_platform)
add_test(NAME input_tests COMMAND input_tests)

add_executable(usecode_tests tests/usecode_tests.cpp)
target_link_libraries(usecode_tests PRIVATE crusader_engine)
add_test(NAME usecode_tests COMMAND usecode_tests)

add_executable(world_tests tests/world_tests.cpp)
target_link_libraries(world_tests PRIVATE crusader_engine)
add_test(NAME world_tests COMMAND world_tests)

add_executable(ui_tests tests/ui_tests.cpp)
target_link_libraries(ui_tests PRIVATE crusader_engine)
add_test(NAME ui_tests COMMAND ui_tests)

add_executable(editor_tests tests/editor_tests.cpp)
target_link_libraries(editor_tests PRIVATE crusader_editor_core)
add_test(NAME editor_tests COMMAND editor_tests)

add_executable(audio_tests tests/audio_tests.cpp)
target_link_libraries(audio_tests PRIVATE crusader_engine)
add_test(NAME audio_tests COMMAND audio_tests)

add_executable(core_data_tests e2e/core_data_tests.cpp)
target_link_libraries(core_data_tests PRIVATE crusader_core)

add_executable(world_data_tests e2e/world_data_tests.cpp)
target_link_libraries(world_data_tests PRIVATE crusader_engine)
target_include_directories(world_data_tests PRIVATE e2e)
target_compile_definitions(world_data_tests PRIVATE CRUSADER_TEST_SAVES="${CRUSADER_TEST_SAVES_DIR}")

add_executable(audio_data_tests e2e/audio_data_tests.cpp)
target_link_libraries(audio_data_tests PRIVATE crusader_engine)

add_executable(editor_data_tests e2e/editor_data_tests.cpp)
target_link_libraries(editor_data_tests PRIVATE crusader_editor_core)

add_executable(game_tests e2e/game_tests.cpp)
target_link_libraries(game_tests PRIVATE crusader_game crusader_editor_core)
target_include_directories(game_tests PRIVATE e2e)
target_compile_definitions(game_tests PRIVATE CRUSADER_TEST_SAVES="${CRUSADER_TEST_SAVES_DIR}")

if(CRUSADER_GAME_DATA_TESTS)
    foreach(t core_data_tests world_data_tests audio_data_tests editor_data_tests game_tests)
        add_test(NAME ${t} COMMAND ${t} ${CRUSADER_DATA_DIR})
    endforeach()
endif()

# The US data (v1.23), when found: the same game through the US usecode and texts.
set(CRUSADER_US_DATA_DIR "${CMAKE_SOURCE_DIR}/data_us" CACHE PATH "US game data used by us_data_tests")
add_executable(us_data_tests e2e/us_data_tests.cpp)
target_link_libraries(us_data_tests PRIVATE crusader_game)
if(EXISTS "${CRUSADER_US_DATA_DIR}/STATIC")
    add_test(NAME us_data_tests COMMAND us_data_tests ${CRUSADER_US_DATA_DIR})
endif()

# Optional private workspace (decompilation material and analysis tools, not in
# this repository): it adds its own checks against the original executable.
if(EXISTS ${CMAKE_SOURCE_DIR}/private/CMakeLists.txt)
    add_subdirectory(private)
endif()
