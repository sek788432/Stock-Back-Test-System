add_library(
  bte_engine
  STATIC
    Include/Bte/Engine/Backtest.h
    Include/Bte/Engine/Replay.h
    Private/BacktestTestHooks.h
    Private/Backtest.cpp
    Private/Replay.cpp)

add_library(bte::engine ALIAS bte_engine)

target_include_directories(
  bte_engine
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private")

target_link_libraries(
  bte_engine
  PUBLIC
    bte::core
    bte::data
    bte::results
    bte::strategy)

target_compile_features(bte_engine PUBLIC cxx_std_20)

if(BTE_BUILD_TESTS)
  target_compile_definitions(bte_engine PRIVATE BTE_ENABLE_TEST_HOOKS)
endif()
