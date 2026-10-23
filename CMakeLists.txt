cmake_minimum_required(VERSION 3.11)
project(minimalCoding LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "This project only supports Windows.")
endif()

set(CORE_SOURCES
  src/base/text.cpp
  src/base/color.cpp
  src/base/win_error.cpp
  src/base/hash.cpp
  src/base/terminal.cpp
  src/base/path.cpp
  src/io/config.cpp
  src/io/source.cpp
  src/io/hotkey.cpp
  src/process/accounting.cpp
  src/process/compiler.cpp
  src/process/runner_single.cpp
  src/monitor/monitor.cpp
  src/app/app.cpp
  src/app/modes.cpp
  src/app/builder.cpp
  src/app/build_cache.cpp
  src/app/prompt.cpp
  src/app/stale_exe.cpp
  src/app/interactive_ui.cpp
  src/app/console_bar.cpp
  src/app/line_edit.cpp
  src/app/status_line.cpp
  src/app/clipboard.cpp
)

add_library(coding_core STATIC ${CORE_SOURCES})

target_include_directories(coding_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/third_party
)

target_link_libraries(coding_core PUBLIC bcrypt)

add_executable(cg_s src/main/main_single.cpp)
add_executable(cg_i src/main/main_interactive.cpp)

add_executable(cg_t
  src/main/main_daemon.cpp
  src/app/daemon.cpp
)

foreach(app cg_s cg_i cg_t)
  target_link_libraries(${app} PRIVATE coding_core)
  set_target_properties(${app} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin
  )
endforeach()

if(MSVC)
  target_compile_options(coding_core PRIVATE /W4 /permissive-)
  target_compile_options(cg_t PRIVATE /W4 /permissive-)
else()
  target_compile_options(coding_core PRIVATE -Wall -Wextra)
  target_compile_options(cg_t PRIVATE -Wall -Wextra)
endif()