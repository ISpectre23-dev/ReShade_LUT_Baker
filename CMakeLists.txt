cmake_minimum_required(VERSION 3.23)

project(ReShadeLUTBaker VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "ReShade LUT Baker builds as a Windows ReShade add-on.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Only the x64 ReShade add-on is supported.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(RESHADE_SDK_ROOT "" CACHE PATH "Path to a ReShade source checkout")
set(IMGUI_ROOT "" CACHE PATH "Path to the Dear ImGui checkout used by ReShade")

# These revisions are the ReShade 6.8.0/API 20 sources audited for this release.
set(RESHADE_SDK_COMMIT "18deaa52de0c425a78b329e9cb3c497281cd00ec" CACHE STRING "Pinned ReShade 6.8.0 SDK revision")
set(RESHADE_IMGUI_COMMIT "3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c" CACHE STRING "Pinned ReShade Dear ImGui commit")

include(FetchContent)

if(NOT RESHADE_SDK_ROOT)
  FetchContent_Declare(
    reshade_sdk
    GIT_REPOSITORY https://github.com/crosire/reshade.git
    GIT_TAG ${RESHADE_SDK_COMMIT}
    GIT_SHALLOW FALSE
    GIT_PROGRESS TRUE
    GIT_SUBMODULES ""
    GIT_SUBMODULES_RECURSE FALSE
    # Populate headers only; never add ReShade's own build to this project.
    SOURCE_SUBDIR "_lut_baker_sdk_headers_only"
  )
  FetchContent_MakeAvailable(reshade_sdk)
  set(RESHADE_SDK_ROOT "${reshade_sdk_SOURCE_DIR}")
endif()

if(NOT IMGUI_ROOT)
  if(EXISTS "${RESHADE_SDK_ROOT}/deps/imgui/imgui.h")
    set(IMGUI_ROOT "${RESHADE_SDK_ROOT}/deps/imgui")
  else()
    FetchContent_Declare(
      reshade_imgui
      GIT_REPOSITORY https://github.com/ocornut/imgui.git
      GIT_TAG ${RESHADE_IMGUI_COMMIT}
      # A shallow clone cannot reliably check out an arbitrary pinned commit.
      GIT_SHALLOW FALSE
      GIT_PROGRESS TRUE
      SOURCE_SUBDIR "_lut_baker_imgui_headers_only"
    )
    FetchContent_MakeAvailable(reshade_imgui)
    set(IMGUI_ROOT "${reshade_imgui_SOURCE_DIR}")
  endif()
endif()

if(NOT EXISTS "${RESHADE_SDK_ROOT}/include/reshade.hpp")
  message(FATAL_ERROR "RESHADE_SDK_ROOT does not contain include/reshade.hpp")
endif()

if(NOT EXISTS "${IMGUI_ROOT}/imgui.h")
  message(FATAL_ERROR "IMGUI_ROOT does not contain imgui.h")
endif()

add_library(reshade_lut_core STATIC
  src/cube_lut.cpp
  src/cube_lut.hpp
)
target_compile_features(reshade_lut_core PUBLIC cxx_std_17)
target_include_directories(reshade_lut_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(reshade_lut_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(ReShadeLUTBaker SHARED
  src/addon.cpp
  src/version.hpp
)
target_include_directories(ReShadeLUTBaker PRIVATE
  "${RESHADE_SDK_ROOT}/include"
  "${IMGUI_ROOT}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ReShadeLUTBaker PRIVATE reshade_lut_core shell32)
target_compile_definitions(ReShadeLUTBaker PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  UNICODE
  _UNICODE
)

if(MSVC)
  target_compile_options(reshade_lut_core PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
  target_compile_options(ReShadeLUTBaker PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
endif()

set_target_properties(ReShadeLUTBaker PROPERTIES
  PREFIX ""
  SUFFIX ".addon64"
  OUTPUT_NAME "ReShadeLUTBaker"
)

include(CTest)
if(BUILD_TESTING)
  add_executable(cube_core_tests tests/cube_core_tests.cpp)
  target_link_libraries(cube_core_tests PRIVATE reshade_lut_core)
  if(MSVC)
    target_compile_options(cube_core_tests PRIVATE /W4 /permissive- /EHsc /Zc:__cplusplus)
  endif()
  add_test(NAME cube_core_tests COMMAND cube_core_tests)

  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME cube_python_tests
      COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_*.py"
    )
  endif()
endif()
