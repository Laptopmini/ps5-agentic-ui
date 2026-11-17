cmake_minimum_required(VERSION 3.24)
project(a2ui VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(GNUInstallDirs)

# The core library: the A2UI renderer (model/view/session) and the haptics model, with no platform
# dependency — a consumer supplies a Canvas (see a2ui/view.hpp) and the device bindings. Its public
# API uses nlohmann/json, vendored here and installed alongside. The PS5 build links these sources
# with its own canvas via payload/console/Makefile; this target is for other adopters:
#   find_package(a2ui 1.0 CONFIG REQUIRED)        after `cmake --install`, or
#   FetchContent_Declare(a2ui GIT_REPOSITORY … GIT_TAG v1.0.0 SOURCE_SUBDIR renderer)
# then target_link_libraries(app PRIVATE a2ui::a2ui).
add_library(a2ui src/model.cpp src/view.cpp src/session.cpp src/haptics.cpp vendor/qrcodegen/qrcodegen.cpp)
add_library(a2ui::a2ui ALIAS a2ui)
set_target_properties(a2ui PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
target_include_directories(a2ui PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor>   # nlohmann/json.hpp + internal qrcodegen
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_options(a2ui PRIVATE -Wall -Wextra -Wpedantic)

# Everything below builds the host app, the console-side helpers and the tests. It's on when this
# is the top-level project and off when a2ui is pulled into another one (add_subdirectory,
# FetchContent), which then gets only the `a2ui` target and needs no SDL2, libcurl or SQLite.
option(A2UI_BUILD_DEV "Build the a2ui host app, console helpers and tests" ${PROJECT_IS_TOP_LEVEL})
if(A2UI_BUILD_DEV)
  # Key setup from a phone or computer (POSIX sockets, TweetNaCl).
  add_library(pairing STATIC src/pairing.cpp vendor/tweetnacl/tweetnacl.c)
  target_include_directories(pairing PUBLIC src vendor)
  target_link_libraries(pairing PUBLIC a2ui)

  # What's on the console (ps5-system), read on the console; tested here against stand-in files.
  find_package(SQLite3 REQUIRED)
  add_library(ps5system STATIC src/ps5_system.cpp src/ps5_voice.cpp src/ps5_whisper.cpp)
  target_include_directories(ps5system PUBLIC src vendor)
  # FindSQLite3 names its target SQLite::SQLite3; CMake 4.3 added SQLite3::SQLite3 and deprecates the old one.
  if(TARGET SQLite3::SQLite3)
    target_link_libraries(ps5system PUBLIC SQLite3::SQLite3)
  else()
    target_link_libraries(ps5system PUBLIC SQLite::SQLite3)
  endif()
  target_compile_options(ps5system PRIVATE -Wall -Wextra)

  enable_testing()
  add_executable(a2ui-tests tests/test_main.cpp)
  target_link_libraries(a2ui-tests PRIVATE pairing ps5system)  # a2ui comes in via pairing (PUBLIC)
  add_test(NAME a2ui-tests COMMAND a2ui-tests)

  # QuickJS (core only), for the agent runtime in-process (JsAgent). The PS5 build links the same
  # version from the pacbrew bundle.
  add_library(quickjs STATIC vendor/quickjs/quickjs.c vendor/quickjs/dtoa.c vendor/quickjs/libregexp.c vendor/quickjs/libunicode.c vendor/quickjs/cutils.c)
  target_compile_definitions(quickjs PRIVATE _GNU_SOURCE CONFIG_VERSION="2026-06-04")
  target_compile_options(quickjs PRIVATE -fwrapv -w)
  target_include_directories(quickjs PUBLIC vendor)

  # Host app: SDL2 window and headless screenshots on the Mac.
  find_package(SDL2 CONFIG)
  find_package(CURL)
  if(SDL2_FOUND AND CURL_FOUND)
    add_executable(a2ui-host src/main.cpp src/canvas_geometry.cpp src/canvas_sdl.cpp src/canvas_gl.cpp src/agent_client.cpp src/channel_client.cpp src/js_agent.cpp)
    target_link_libraries(a2ui-host PRIVATE pairing quickjs SDL2::SDL2 CURL::libcurl)  # a2ui via pairing (PUBLIC)
    target_compile_options(a2ui-host PRIVATE -Wall -Wextra)
    target_compile_definitions(a2ui-host PRIVATE A2UI_DEFAULT_FONT="${CMAKE_CURRENT_SOURCE_DIR}/vendor/fonts/NotoSans.ttf"
                               A2UI_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets" A2UI_VENDOR="${CMAKE_CURRENT_SOURCE_DIR}/vendor")
    # Smoke test: every fixture (real model output) renders headless and Cross does something.
    file(GLOB fixtures CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/*.jsonl)
    foreach(fixture ${fixtures})
      get_filename_component(name ${fixture} NAME_WE)
      add_test(NAME render-${name} COMMAND a2ui-host ${fixture} --keys cross --screenshot ${CMAKE_CURRENT_BINARY_DIR}/${name}.bmp)
      set_tests_properties(render-${name} PROPERTIES FAIL_REGULAR_EXPRESSION "line [0-9]+:|unknown|cannot")
    endforeach()
  else()
    message(STATUS "SDL2 or libcurl not found: building the library and tests only")
  endif()
endif()

# ── Install + package: make a2ui consumable via find_package(a2ui) ───────────────────────────────
# `cmake --install` lays down the static lib, the public headers (a2ui/ + the vendored
# nlohmann/json.hpp its API needs), and a CMake package so another project can do
# `find_package(a2ui 1.0 CONFIG)` + `target_link_libraries(app PRIVATE a2ui::a2ui)`.
include(CMakePackageConfigHelpers)

install(TARGETS a2ui EXPORT a2uiTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/a2ui DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES vendor/nlohmann/json.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nlohmann)

install(EXPORT a2uiTargets
  FILE a2uiTargets.cmake
  NAMESPACE a2ui::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a2ui)

configure_package_config_file(cmake/a2uiConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/a2uiConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a2ui)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/a2uiConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/a2uiConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/a2uiConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/a2ui)
