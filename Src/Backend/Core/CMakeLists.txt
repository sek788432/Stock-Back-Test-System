add_library(
  bte_core
  STATIC
    Include/Bte/Core/Bar.h
    Include/Bte/Core/Cancellation.h
    Include/Bte/Core/Result.h
    Include/Bte/Core/Time.h
    Private/Bar.cpp
    Private/Result.cpp
    Private/Time.cpp)

add_library(bte::core ALIAS bte_core)

target_include_directories(
  bte_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private")

target_compile_features(bte_core PUBLIC cxx_std_20)
