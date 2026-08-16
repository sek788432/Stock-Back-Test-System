add_library(
  bte_indicators
  STATIC
    Include/Bte/Indicators/StreamingIndicator.h
    Private/StreamingIndicator.cpp)

add_library(bte::indicators ALIAS bte_indicators)

target_include_directories(
  bte_indicators
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private")

target_link_libraries(
  bte_indicators
  PUBLIC
    bte::core)

target_compile_features(bte_indicators PUBLIC cxx_std_20)
