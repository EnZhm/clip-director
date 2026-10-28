cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)
option(CD_TEST_HOOKS "Sandbox test hooks (dock snapshots); never in releases" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_FRONTEND_API)
endif()

# winmm: MIDI input (W4 MIDI Director). Windows only; elsewhere cd-midi-input.cpp is a stub.
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE winmm)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_QT)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

if(CD_TEST_HOOKS)
  message(WARNING "CD_TEST_HOOKS is ON: sandbox test hooks are compiled in (do not ship this build)")
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE CD_TEST_HOOKS)
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/obs/cd-source.cpp
    src/obs/cd-library.cpp
    src/obs/cd-syn-hotkeys.cpp
    src/core/picker.cpp
    src/core/scheduler.cpp
    src/core/levels.cpp
    src/core/osc.cpp
    src/core/scene_pool.cpp
    src/net/osc-transport.cpp
    src/obs/cd-hotkeys.cpp
    src/obs/cd-midi-input.cpp
    src/core/midi_map.cpp
    src/dock/cd-dock.cpp
    src/dock/midi-director-dock.cpp
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src src/core src/net)

# OSC transport threads; on Windows the UDP sockets come from Winsock
find_package(Threads REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
