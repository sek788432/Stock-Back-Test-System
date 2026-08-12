add_library(
  bte_bindings
  STATIC
    Include/Bte/Bindings/BacktestSessionVm.h
    Include/Bte/Bindings/ReplayDataLoader.h
    Include/Bte/Bindings/ReplaySessionVm.h
    Backtest/BacktestSessionVm.cpp
    Replay/ReplayDataLoader.cpp
    Include/Bte/Bindings/ReplaySessionVm.h
    Replay/ReplaySessionVm.cpp)

add_library(bte::bindings ALIAS bte_bindings)

target_include_directories(
  bte_bindings
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Backtest"
    "${CMAKE_CURRENT_SOURCE_DIR}/Replay")

target_link_libraries(
  bte_bindings
  PUBLIC
    bte::core
    bte::strategy
    Qt6::Core
  PRIVATE
    bte::data
    bte::engine)

target_compile_definitions(
  bte_bindings
  PRIVATE
    BTE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_compile_features(bte_bindings PUBLIC cxx_std_20)
