add_library(
  bte_result_replay_bindings
  STATIC
    Include/Bte/Bindings/ResultReplay.h
    Include/Bte/Bindings/ResultReplayRequests.h
    Replay/ResultReplay.cpp
    Replay/ResultReplayRequests.cpp
    Replay/ResultReplayRequestsTestHooks.h)

add_library(bte::result_replay_bindings ALIAS bte_result_replay_bindings)

target_include_directories(
  bte_result_replay_bindings
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Replay")

target_link_libraries(
  bte_result_replay_bindings
  PUBLIC
    bte::core
    bte::results
    Qt6::Core
  PRIVATE
    bte::data
    Qt6::Concurrent)

target_compile_features(bte_result_replay_bindings PUBLIC cxx_std_20)

add_library(
  bte_bindings
  STATIC
    Include/Bte/Bindings/BacktestSessionVm.h
    Include/Bte/Bindings/ReplayDataLoader.h
    Include/Bte/Bindings/ReplaySessionVm.h
    Backtest/BacktestSessionVm.cpp
    Replay/ReplayDataLoader.cpp
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
    bte::result_replay_bindings
    bte::results
    bte::strategy
    Qt6::Core
  PRIVATE
    bte::data
    bte::engine
    Qt6::Concurrent)

target_compile_definitions(
  bte_bindings
  PRIVATE
    BTE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_compile_features(bte_bindings PUBLIC cxx_std_20)

if(BTE_BUILD_TESTS)
  target_compile_definitions(
    bte_result_replay_bindings PRIVATE BTE_ENABLE_TEST_HOOKS)
  target_compile_definitions(bte_bindings PRIVATE BTE_ENABLE_TEST_HOOKS)
endif()
