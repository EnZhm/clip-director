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
add_library(cd_core STATIC ${CD_CORE}/picker.cpp ${CD_CORE}/scheduler.cpp ${CD_CORE}/levels.cpp)
target_include_directories(cd_core PUBLIC ${CD_CORE})
if(MSVC)
  target_compile_options(cd_core PRIVATE /W4 /WX)
else()
  target_compile_options(cd_core PRIVATE -Wall -Wextra -Werror)
endif()

add_executable(
  cd_core_tests
  doctest_main.cpp
  test_picker.cpp
  test_scheduler.cpp
  test_levels.cpp
  test_cyber_set.cpp
)
target_include_directories(cd_core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/doctest)
target_link_libraries(cd_core_tests PRIVATE cd_core)
add_test(NAME cd_core_tests COMMAND cd_core_tests)
