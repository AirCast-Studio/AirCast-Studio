# Builds two ways: on its own against the system libobs (tests + per-user install), or inside the
# AirCast Studio source tree (add_subdirectory from its plugins/CMakeLists.txt), using in-tree libobs.
cmake_minimum_required(VERSION 3.28)
project(aircast-obs-plugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # everything ends up inside aircast.so

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)

# Media core: RTP, SDP filter, decoder. No OBS, so it is unit-tested on its own.
add_library(aircast-core STATIC src/rtp_video.cpp src/sdp_filter.cpp src/decoder.cpp)
target_include_directories(aircast-core PUBLIC src)
target_link_libraries(aircast-core PUBLIC PkgConfig::FFMPEG)
target_compile_options(aircast-core PRIVATE -Wall -Wextra)

if(PROJECT_IS_TOP_LEVEL)
  include(CTest)
endif()
if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
  foreach(t rtp_video sdp_filter decoder)
    add_executable(test_${t} tests/test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE aircast-core)
    add_test(NAME ${t} COMMAND test_${t})
    set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
  endforeach()
endif()

# The OBS plugin itself.
option(AIRCAST_BUILD_PLUGIN "Build the OBS plugin (needs libobs + libdatachannel)" ON)
if(AIRCAST_BUILD_PLUGIN)
  include(FetchContent)
  set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)  # AirCast Studio turns it on; third-party code isn't ours to fix
  set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(libdatachannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG v0.24.7
    GIT_SHALLOW ON
    GIT_SUBMODULES_RECURSE ON
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(libdatachannel)

  if(NOT TARGET OBS::libobs)
    find_package(libobs REQUIRED)
  endif()

  add_library(aircast-peer STATIC src/peer.cpp)
  target_link_libraries(aircast-peer PUBLIC aircast-core LibDataChannel::LibDataChannelStatic)
  target_compile_options(aircast-peer PRIVATE -Wall -Wextra)

  if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    add_executable(test_peer tests/test_peer.cpp)
    target_link_libraries(test_peer PRIVATE aircast-peer)
    add_test(NAME peer COMMAND test_peer)
    set_tests_properties(peer PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
  endif()

  add_library(aircast MODULE src/plugin-main.cpp src/camera_source.cpp)
  target_link_libraries(aircast PRIVATE aircast-peer OBS::libobs)
  target_compile_options(aircast PRIVATE -Wall -Wextra)
  # Keep libdatachannel's symbols private so they can't clash with another copy in OBS.
  set_target_properties(aircast PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
  target_link_options(aircast PRIVATE -Wl,--exclude-libs,ALL)

  if(COMMAND set_target_properties_obs)  # inside AirCast Studio: installs with the app's own plugins
    target_include_directories(aircast PRIVATE "${CMAKE_SOURCE_DIR}/plugins/obs-websocket/lib")  # header only, no target
    set_target_properties_obs(aircast PROPERTIES FOLDER plugins PREFIX "")
  else()
    install(TARGETS aircast LIBRARY DESTINATION "$ENV{HOME}/.config/obs-studio/plugins/aircast/bin/64bit")
  endif()
endif()
