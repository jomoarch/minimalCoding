add_library(linedit_core STATIC
  src/session.cpp
  src/editor.cpp
  src/bar.cpp
  src/status_line.cpp
)

target_include_directories(linedit_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(linedit_core PRIVATE
  ${CMAKE_SOURCE_DIR}/include
)

if(MSVC)
  target_compile_options(linedit_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(linedit_core PRIVATE -Wall -Wextra)
endif()
