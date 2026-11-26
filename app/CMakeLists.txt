cmake_minimum_required(VERSION 3.21)
project(LibreTap VERSION 0.1.0 LANGUAGES CXX)

# LibreTap: record what old vendor software says to its instrument (passive sniffers) together with the
# instrument PC's screen, and look at both on one timeline. Session format: ../docs/FORMAT.md.
# Layout:  src/core  library (sessions, projects, sources, recorder, matching, control socket; Qt Core/Gui/
#                    SerialPort/Network)
#          src/cli   LibreTapCLI (headless recording, control of a running recorder, JSON output)
#          src/gui   LibreTap (Qt Widgets)
#          tests     QtTest unit tests (ctest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui SerialPort Network)

add_compile_definitions(LIBRETAP_VERSION="${PROJECT_VERSION}")
if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
else()
  add_compile_options(-Wall -Wextra)
endif()

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS src/core/*.cpp src/core/*.h)
add_library(libretap_core STATIC ${CORE_SOURCES})
target_include_directories(libretap_core PUBLIC src)
target_link_libraries(libretap_core PUBLIC Qt6::Core Qt6::Gui Qt6::SerialPort Qt6::Network)

add_executable(libretap-cli src/cli/main.cpp)
target_link_libraries(libretap-cli PRIVATE libretap_core)
set_target_properties(libretap-cli PROPERTIES OUTPUT_NAME LibreTapCLI)

option(LIBRETAP_GUI "Build the graphical program" ON)
if(LIBRETAP_GUI)
  find_package(Qt6 REQUIRED COMPONENTS Widgets)
  file(GLOB GUI_SOURCES CONFIGURE_DEPENDS src/gui/*.cpp src/gui/*.h)
  list(FILTER GUI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")
  add_library(libretap_gui STATIC ${GUI_SOURCES})   # a library so the GUI test can use it
  target_link_libraries(libretap_gui PUBLIC libretap_core Qt6::Widgets)
  target_compile_definitions(libretap_gui PRIVATE LIBRETAP_SOURCE_EXAMPLES="${CMAKE_CURRENT_SOURCE_DIR}/examples")
  qt_add_executable(libretap-gui WIN32 MACOSX_BUNDLE src/gui/main.cpp)
  qt_add_resources(libretap-gui icon PREFIX "/" BASE resources FILES resources/icon.png)
  target_link_libraries(libretap-gui PRIVATE libretap_gui)
  set_target_properties(libretap-gui PROPERTIES OUTPUT_NAME LibreTap)
endif()

include(cmake/Install.cmake)

enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Test)
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(src ${TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  if(name STREQUAL "test_gui" AND NOT LIBRETAP_GUI)
    continue()
  endif()
  if((name STREQUAL "test_serialtap" OR name STREQUAL "test_endtoend") AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    continue()   # pseudo-terminals stand in for the adapters (Linux openpty)
  endif()
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE libretap_core Qt6::Test)
  if(name STREQUAL "test_gui")
    target_link_libraries(${name} PRIVATE libretap_gui)
  endif()
  if(name STREQUAL "test_serialtap" OR name STREQUAL "test_endtoend" OR (name STREQUAL "test_usb" AND CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    target_link_libraries(${name} PRIVATE util)
  endif()
  target_compile_definitions(${name} PRIVATE CLI_PATH="$<TARGET_FILE:libretap-cli>" TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
  add_test(NAME ${name} COMMAND ${name})
  # never a real camera in tests (the developer's own webcam); a private control socket per test run
  set_tests_properties(${name} PROPERTIES ENVIRONMENT
    "LIBRETAP_NO_SYSTEM_CAMERAS=1;QT_QPA_PLATFORM=offscreen;LIBRETAP_CONTROL=libretap-test-${name}")
endforeach()

# The Windows Tick under Wine, end to end with the serial tap (slow; needs Wine, xvfb-run and Tick's Windows build)
option(LIBRETAP_WINE_TEST "Register the Wine end-to-end test (tests/scripts/e2e-wine.sh)" OFF)
if(LIBRETAP_WINE_TEST)
  add_test(NAME e2e_wine COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/e2e-wine.sh ${CMAKE_BINARY_DIR})
  set_tests_properties(e2e_wine PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()
