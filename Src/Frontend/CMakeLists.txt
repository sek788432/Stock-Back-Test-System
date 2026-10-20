add_library(
  bte_frontend
  STATIC
    Include/Bte/Frontend/BacktestTab.h
    Include/Bte/Frontend/IChartView.h
    Include/Bte/Frontend/QtChartsCandlestickView.h
    Include/Bte/Frontend/ReplayBarViewport.h
    Include/Bte/Frontend/ReplayTab.h
    Include/Bte/Frontend/ResultsTab.h
    Charts/ReplayBarViewport.cpp
    Charts/QtChartsCandlestickView.cpp
    Backtest/BacktestTab.cpp
    Replay/ReplayTab.cpp
    Replay/ReplayTabSections.cpp
    Replay/ReplayTabStyle.cpp
    Results/ResultsTab.cpp
    Results/ResultRecoveryNoticeLabel.h)

add_library(bte::frontend ALIAS bte_frontend)

target_include_directories(
  bte_frontend
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Backtest"
    "${CMAKE_CURRENT_SOURCE_DIR}/Replay"
    "${CMAKE_CURRENT_SOURCE_DIR}/Results")

target_link_libraries(
  bte_frontend
  PUBLIC
    bte::core
    bte::bindings
    Qt6::Charts
    Qt6::Concurrent
    Qt6::Widgets)

target_compile_features(bte_frontend PUBLIC cxx_std_20)
