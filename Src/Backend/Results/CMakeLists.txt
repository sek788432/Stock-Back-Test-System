add_library(
  bte_results
  STATIC
    Include/Bte/Results/ResultStore.h
    Private/ResultStore.cpp)

add_library(bte::results ALIAS bte_results)

target_include_directories(
  bte_results
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private")

target_link_libraries(
  bte_results
  PUBLIC
    bte::core
    bte::data
  PRIVATE
    SQLite::SQLite3)

target_compile_features(bte_results PUBLIC cxx_std_20)
