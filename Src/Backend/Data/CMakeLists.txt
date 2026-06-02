add_library(
  bte_data
  STATIC
    Include/Bte/Data/BarStream.h
    Private/CsvBarStream.cpp)

add_library(bte::data ALIAS bte_data)

target_include_directories(
  bte_data
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private")

target_link_libraries(
  bte_data
  PUBLIC
    bte::core)

target_compile_features(bte_data PUBLIC cxx_std_20)
