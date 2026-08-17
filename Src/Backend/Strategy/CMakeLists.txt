add_library(
  bte_strategy
  STATIC
    Include/Bte/Strategy/SelectableStrategy.h
    Private/SelectableStrategyDetail.h
    Private/SelectableStrategy.cpp)

add_library(bte::strategy ALIAS bte_strategy)

target_include_directories(
  bte_strategy
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private")

target_link_libraries(
  bte_strategy
  PUBLIC
    bte::core
    bte::indicators)

target_compile_features(bte_strategy PUBLIC cxx_std_20)
