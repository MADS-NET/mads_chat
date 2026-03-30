cmake_minimum_required(VERSION 3.24)

if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

project(mads_chat LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

if(WIN32)
  execute_process(
    COMMAND mads -p
    OUTPUT_VARIABLE MADS_ROOT_FROM_TOOL
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
else()
  execute_process(
    COMMAND mads -p
    OUTPUT_VARIABLE MADS_ROOT_FROM_TOOL
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()

set(MADS_ROOT "${MADS_ROOT_FROM_TOOL}" CACHE PATH "Path to the MADS installation root")

if(NOT MADS_ROOT)
  message(FATAL_ERROR "MADS_ROOT is empty. Set -DMADS_ROOT=/path/to/mads or ensure `mads -p` works.")
endif()

set(MADS_INCLUDE_DIR "${MADS_ROOT}/include")

if(WIN32)
  set(MADS_LIBRARY "${MADS_ROOT}/lib/MadsCore.lib")
elseif(APPLE)
  set(MADS_LIBRARY "${MADS_ROOT}/lib/libMadsCore.dylib")
else()
  set(MADS_LIBRARY "${MADS_ROOT}/lib/libMadsCore.so")
endif()

if(NOT EXISTS "${MADS_INCLUDE_DIR}/agent.hpp")
  message(FATAL_ERROR "Could not find MADS headers under ${MADS_INCLUDE_DIR}")
endif()

if(NOT EXISTS "${MADS_LIBRARY}")
  message(FATAL_ERROR "Could not find MADS library at ${MADS_LIBRARY}")
endif()

FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.6
)

FetchContent_Declare(
  portable_file_dialogs
  GIT_REPOSITORY https://github.com/samhocevar/portable-file-dialogs.git
  GIT_TAG 0.1.0
)

FetchContent_Declare(
  imgui_color_text_edit
  GIT_REPOSITORY https://github.com/goossens/ImGuiColorTextEdit.git
  GIT_TAG master
)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw imgui)

FetchContent_GetProperties(portable_file_dialogs)
if(NOT portable_file_dialogs_POPULATED)
  FetchContent_Populate(portable_file_dialogs)
endif()

FetchContent_GetProperties(imgui_color_text_edit)
if(NOT imgui_color_text_edit_POPULATED)
  FetchContent_Populate(imgui_color_text_edit)
endif()

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui_lib
  PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

if(WIN32)

else()
  add_compile_options(-Wno-deprecated-literal-operator)
endif()

target_link_libraries(imgui_lib PUBLIC glfw)

find_package(OpenGL REQUIRED)
target_link_libraries(imgui_lib PUBLIC OpenGL::GL)

if(APPLE)
  target_compile_definitions(imgui_lib PUBLIC GL_SILENCE_DEPRECATION IMGUI_IMPL_OPENGL_LOADER_NONE)
else()
  target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_OPENGL_LOADER_NONE)
endif()

add_library(portable_file_dialogs INTERFACE)
target_include_directories(portable_file_dialogs INTERFACE ${portable_file_dialogs_SOURCE_DIR})

add_executable(mads_chat
  src/main.cpp
  ${imgui_color_text_edit_SOURCE_DIR}/TextEditor.cpp
)

target_include_directories(mads_chat
  PRIVATE
    ${MADS_INCLUDE_DIR}
    ${imgui_color_text_edit_SOURCE_DIR}
)

target_link_libraries(mads_chat
  PRIVATE
    imgui_lib
    portable_file_dialogs
    ${MADS_LIBRARY}
)

if(APPLE)
  target_link_libraries(mads_chat PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
elseif(WIN32)
  target_link_libraries(mads_chat PRIVATE comdlg32 ole32 shell32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(mads_chat PRIVATE Threads::Threads)
endif()

target_compile_definitions(mads_chat PRIVATE IMGUI_DEFINE_MATH_OPERATORS)
