add_executable(
  stockBacktester
    MainWindow.cpp
    main.cpp)

target_include_directories(
  stockBacktester
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  stockBacktester
  PRIVATE
    bte::frontend
    Qt6::Widgets)

target_compile_features(stockBacktester PRIVATE cxx_std_20)
