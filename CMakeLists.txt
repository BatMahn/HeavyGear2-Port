# Heavy Gear II port — CMake build for macOS, Linux and Windows (release packages).
#   cmake -B build-cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-cmake
# SDL3 and zlib are taken from the system when found, otherwise downloaded and built
# (HG2_FETCH_DEPS=ON forces the download, which is what the release workflow does).
cmake_minimum_required(VERSION 3.21)
project(hg2port VERSION 0.24.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(HG2_FETCH_DEPS "Download and build SDL3 and zlib instead of using installed ones" OFF)

include(FetchContent)
if(NOT HG2_FETCH_DEPS)
  find_package(SDL3 CONFIG QUIET)
  find_package(ZLIB QUIET)
endif()
if(NOT TARGET SDL3::SDL3)
  FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.26/SDL3-3.2.26.tar.gz)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)  # static, linked in: nothing of SDL goes in the package
  FetchContent_MakeAvailable(SDL3)
endif()
if(NOT ZLIB_FOUND)
  FetchContent_Declare(zlib URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz)
  set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  # zlib's install rules use absolute paths (/usr/local/lib) that ignore `cmake --install --prefix`; it is linked
  # statically, so skip them
  set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(zlib)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
  target_include_directories(zlibstatic INTERFACE ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
endif()

set(CORE_SRC
  src/core/fs.cpp src/core/zfs.cpp src/assets/assets.cpp src/assets/mission.cpp src/script/vm.cpp src/script/system.cpp
  src/assets/gear.cpp src/assets/weapon.cpp)
set(GAME_SRC
  src/render/renderer.cpp src/game/scene.cpp src/game/player.cpp src/game/combat.cpp src/game/ai.cpp
  src/audio/audio.cpp src/game/hud.cpp src/game/particles.cpp src/game/fountains.cpp src/game/session.cpp
  src/shell/ldl.cpp src/shell/ui.cpp src/shell/gamedata.cpp src/shell/avi.cpp src/shell/movie.cpp src/shell/shell.cpp src/net/net.cpp src/main.cpp)

# Intel Indeo 5 video decoder (from FFmpeg, LGPL-2.1-or-later; see third_party/indeo5/COPYING.LGPLv2.1)
add_library(indeo5 STATIC third_party/indeo5/indeo5.c third_party/indeo5/ivi.c third_party/indeo5/ivi_dsp.c third_party/indeo5/ffshim.c)
set_target_properties(indeo5 PROPERTIES C_STANDARD 11)
if(MSVC)
  target_compile_options(indeo5 PRIVATE /W0)
else()
  target_compile_options(indeo5 PRIVATE -w)
endif()

add_library(hg2core STATIC ${CORE_SRC})
target_include_directories(hg2core PUBLIC src)
target_link_libraries(hg2core PUBLIC SDL3::SDL3 ZLIB::ZLIB)

if(APPLE)
  add_executable(hg2 MACOSX_BUNDLE ${GAME_SRC})
  set_target_properties(hg2 PROPERTIES
    OUTPUT_NAME "Heavy Gear II"
    MACOSX_BUNDLE_BUNDLE_NAME "Heavy Gear II"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.batmahn.hg2port"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
elseif(WIN32)
  add_executable(hg2 WIN32 ${GAME_SRC})
else()
  add_executable(hg2 ${GAME_SRC})
endif()
target_link_libraries(hg2 PRIVATE hg2core indeo5)
if(WIN32)
  target_link_libraries(hg2 PRIVATE ws2_32)
endif()

add_executable(hg2extract tools/hg2extract.cpp)
target_link_libraries(hg2extract PRIVATE hg2core)

if(MSVC)
  target_compile_options(hg2core PRIVATE /W3 /utf-8)
  target_compile_options(hg2 PRIVATE /W3 /utf-8)
  target_compile_definitions(hg2core PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  target_compile_options(hg2core PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  target_compile_options(hg2 PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

install(TARGETS hg2 hg2extract BUNDLE DESTINATION . RUNTIME DESTINATION .)
install(FILES README.md DESTINATION .)
