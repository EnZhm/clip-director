# Clip Director core (libobs/Qt-free) + doctest suite.
#   cmake -S tests -B build-tests && cmake --build build-tests --config Release
#   ctest --test-dir build-tests -C Release --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(cd_core_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
enable_testing()

set(CD_CORE ${CMAKE_CURRENT_SOURCE_DIR}/../src/core)
set(CD_NET ${CMAKE_CURRENT_SOURCE_DIR}/../src/net)

find_package(Threads REQUIRED)

add_library(
  cd_core
  STATIC
  ${CD_CORE}/picker.cpp
  ${CD_CORE}/scheduler.cpp
  ${CD_CORE}/levels.cpp
  ${CD_CORE}/osc.cpp
  ${CD_CORE}/scene_pool.cpp
  ${CD_CORE}/midi_map.cpp
  ${CD_NET}/osc-transport.cpp
)
target_include_directories(cd_core PUBLIC ${CD_CORE} ${CD_NET})
target_link_libraries(cd_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(cd_core PRIVATE /W4 /WX)
else()
  target_compile_options(cd_core PRIVATE -Wall -Wextra -Werror)
endif()
if(WIN32)
  target_link_libraries(cd_core PUBLIC ws2_32)
endif()

add_executable(
  cd_core_tests
  doctest_main.cpp
  test_picker.cpp
  test_scheduler.cpp
  test_levels.cpp
  test_cyber_set.cpp
  test_osc.cpp
  test_scene_pool.cpp
  test_scene_sched.cpp
  test_osc_transport.cpp
  test_midi_map.cpp
)
target_include_directories(cd_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/doctest)
target_link_libraries(cd_core_tests PRIVATE cd_core)
add_test(NAME cd_core_tests COMMAND cd_core_tests)

# P6 integration: Clip Director's scheduler driven by Palette Director's real Director on a WAV
# fixture. Needs a palette-director checkout next to this repo (or -DCD_PD_ROOT=...); skipped
# when it is missing (e.g. in CI).
set(CD_PD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../palette-director" CACHE PATH "palette-director checkout")
if(EXISTS "${CD_PD_ROOT}/src/director/director.cpp")
  set(PDD ${CD_PD_ROOT}/src/director)
  add_library(
    cd_pd_director
    STATIC
    ${PDD}/director.cpp
    ${PDD}/analysis.cpp
    ${PDD}/beat_tracker.cpp
    ${PDD}/section_state.cpp
    ${PDD}/scheduler.cpp
    ${CD_PD_ROOT}/tests/fixtures/gen_fixtures.cpp
  )
  target_include_directories(cd_pd_director PRIVATE ${CD_PD_ROOT}/src ${PDD} ${CD_PD_ROOT}/tests/fixtures)
  add_executable(cd_pd_integration doctest_main.cpp test_pd_stutter.cpp test_pd_scene_sync.cpp)
  target_include_directories(
    cd_pd_integration
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/doctest
      ${CD_CORE}
      ${CD_PD_ROOT}/tests
      ${CD_PD_ROOT}/tests/fixtures
      ${PDD}
  )
  target_link_libraries(cd_pd_integration PRIVATE cd_core cd_pd_director)
  add_test(NAME cd_pd_integration COMMAND cd_pd_integration)
  message(STATUS "P6 integration test against ${CD_PD_ROOT}")
else()
  message(STATUS "palette-director not found at ${CD_PD_ROOT}: P6 integration test skipped")
endif()
