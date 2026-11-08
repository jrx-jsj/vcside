# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(vcside VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# --- SDL3: system package if present, otherwise built from source -----------
option(VCSIDE_VENDORED_SDL "Always build SDL3 from source" OFF)
if(NOT VCSIDE_VENDORED_SDL)
  find_package(SDL3 CONFIG QUIET)
endif()
if(NOT SDL3_FOUND)
  message(STATUS "SDL3 not found: fetching it")
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26.tar.gz)
  FetchContent_MakeAvailable(SDL3)
endif()

find_package(OpenGL REQUIRED)

# --- Dear ImGui (docking) -------------------------------------------------
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b-docking
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(imgui PUBLIC SDL3::SDL3 OpenGL::GL)

# --- JSON -----------------------------------------------------------------
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

# --- VCSIDE -----------------------------------------------------------------
file(GLOB VCSIDE_SOURCES CONFIGURE_DEPENDS src/*.cpp src/editor/*.cpp)
add_executable(vcside WIN32 MACOSX_BUNDLE ${VCSIDE_SOURCES})
target_include_directories(vcside PRIVATE src)
target_link_libraries(vcside PRIVATE imgui nlohmann_json::nlohmann_json)
target_compile_definitions(vcside PRIVATE VCSIDE_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(vcside PRIVATE /W4 /utf-8)
else()
  target_compile_options(vcside PRIVATE -Wall -Wextra)
endif()
set_target_properties(vcside PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME VCSIDE
  MACOSX_BUNDLE_GUI_IDENTIFIER org.vcside.VCSIDE)

# The knowledge base travels next to the executable (inside the bundle on macOS).
if(APPLE)
  set(VCSIDE_DATA_DIR $<TARGET_BUNDLE_CONTENT_DIR:vcside>/Resources/data)
else()
  set(VCSIDE_DATA_DIR $<TARGET_FILE_DIR:vcside>/data)
endif()
add_custom_command(TARGET vcside POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data ${VCSIDE_DATA_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/licenses ${VCSIDE_DATA_DIR}/licenses
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/LICENSE ${VCSIDE_DATA_DIR}/licenses/VCSIDE-GPL-3.0.txt)

# --- Tests (no GUI) ---------------------------------------------------------
enable_testing()
add_executable(vcside_tests tests/tests.cpp src/kb.cpp src/cycles.cpp src/builder_parse.cpp src/editor/text_editor.cpp src/i18n.cpp src/space.cpp)
target_include_directories(vcside_tests PRIVATE src)
target_link_libraries(vcside_tests PRIVATE imgui nlohmann_json::nlohmann_json)
target_compile_definitions(vcside_tests PRIVATE VCSIDE_SOURCE_DATA="${CMAKE_SOURCE_DIR}/data")
add_test(NAME vcside_tests COMMAND vcside_tests)
