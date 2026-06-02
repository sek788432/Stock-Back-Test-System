add_library(
  bte_frontend
  STATIC
    Include/Bte/Frontend/IChartView.h
    Include/Bte/Frontend/QtChartsCandlestickView.h
    Include/Bte/Frontend/ReplayTab.h
    Include/Bte/Frontend/ReplaySessionVm.h
    Charts/QtChartsCandlestickView.cpp
    Replay/ReplaySessionVm.cpp
    Replay/ReplayTab.cpp)

add_library(bte::frontend ALIAS bte_frontend)

target_include_directories(
  bte_frontend
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Replay")

target_link_libraries(
  bte_frontend
  PUBLIC
    bte::core
    Qt6::Charts
    Qt6::Widgets)

target_compile_features(bte_frontend PUBLIC cxx_std_20)
