# The smallest consumer of the a2ui library: no SDL, no agent, no PS5.
#
#   cmake -S renderer/examples/hello -B build/hello && cmake --build build/hello && build/hello/hello
#
# By default it builds a2ui from this repository. To build against an installed package instead
# (cmake --install), pass -DHELLO_USE_INSTALLED=ON -DCMAKE_PREFIX_PATH=<prefix>. In your own
# project, use FetchContent:
#
#   include(FetchContent)
#   FetchContent_Declare(a2ui
#     GIT_REPOSITORY https://github.com/Laptopmini/ps5-agentic-ui.git
#     GIT_TAG        v1.0.0
#     SOURCE_SUBDIR  renderer)
#   FetchContent_MakeAvailable(a2ui)
cmake_minimum_required(VERSION 3.24)
project(a2ui_hello LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HELLO_USE_INSTALLED "Use an installed a2ui package instead of this repository's sources" OFF)
if(HELLO_USE_INSTALLED)
  find_package(a2ui 1.0 CONFIG REQUIRED)
else()
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. a2ui)
endif()

add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE a2ui::a2ui)
