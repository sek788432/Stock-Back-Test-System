cmake_minimum_required(VERSION 3.24)

project(
  stockBacktester
  VERSION 0.1.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BTE_BUILD_TESTS "Build GoogleTest-based unit tests" ON)
option(BTE_SANITIZERS "Enable ASan/UBSan on Clang/GNU builds" OFF)
option(BTE_BUILD_QT_APP "Build the Qt desktop app shell" OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Cmake")
include(CompilerWarnings)
include(Sanitizers)

add_subdirectory(Src/Backend/Core)
add_subdirectory(Src/Backend/Data)
add_subdirectory(Src/Backend/Engine)

if(BTE_BUILD_QT_APP)
  set(BTE_QT_COMPONENTS Charts Core Widgets)
  if(BTE_BUILD_TESTS)
    list(APPEND BTE_QT_COMPONENTS Test)
  endif()
  find_package(Qt6 6.8 REQUIRED COMPONENTS ${BTE_QT_COMPONENTS})
  set(CMAKE_AUTOMOC ON)

  add_subdirectory(Src/Bindings)
  add_subdirectory(Src/Frontend)
  add_subdirectory(Src/App)
endif()

if(BTE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  set_target_properties(
    gtest
    gtest_main
    gmock
    gmock_main
    PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

  enable_testing()
  add_subdirectory(Tests)
endif()
