add_executable(
  bte_core_tests
  Unit/Core/UnitTest_Bar.cpp
  Unit/Core/UnitTest_Cancellation.cpp
  Unit/Core/UnitTest_Result.cpp
  Unit/Core/UnitTest_Time.cpp)

target_link_libraries(
  bte_core_tests
  PRIVATE
    bte::core
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(bte_core_tests DISCOVERY_TIMEOUT 30)

add_executable(
  bte_data_tests
  Unit/Data/UnitTest_CsvBarStream.cpp
  Unit/Data/UnitTest_ReleaseSnapshot.cpp)

target_link_libraries(
  bte_data_tests
  PRIVATE
    bte::data
    GTest::gtest_main
    SQLite::SQLite3)

target_compile_definitions(
  bte_data_tests
  PRIVATE
    BTE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Fixtures")

target_include_directories(
  bte_data_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/Src/Backend/Data/Private")

gtest_discover_tests(bte_data_tests DISCOVERY_TIMEOUT 30)

add_executable(
  bte_results_tests
  Unit/Results/UnitTest_ResultStore.cpp)

target_link_libraries(
  bte_results_tests
  PRIVATE
    bte::results
    GTest::gtest_main
    SQLite::SQLite3)

target_include_directories(
  bte_results_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/Src/Backend/Results/Private"
    "${PROJECT_SOURCE_DIR}/Src/Backend/Data/Private")

gtest_discover_tests(bte_results_tests DISCOVERY_TIMEOUT 30)

add_executable(
  bte_indicators_tests
  Unit/Indicators/UnitTest_StreamingIndicator.cpp)

target_link_libraries(
  bte_indicators_tests
  PRIVATE
    bte::indicators
    GTest::gtest_main)

gtest_discover_tests(bte_indicators_tests DISCOVERY_TIMEOUT 30)

add_executable(
  bte_strategy_tests
  Unit/Strategy/UnitTest_SelectableStrategy.cpp)

target_link_libraries(
  bte_strategy_tests
  PRIVATE
    bte::strategy
    GTest::gtest_main)

target_include_directories(
  bte_strategy_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/Src/Backend/Strategy/Private")

gtest_discover_tests(bte_strategy_tests DISCOVERY_TIMEOUT 30)

add_executable(
  bte_engine_tests
  Unit/Engine/UnitTest_Backtest.cpp
  Unit/Engine/UnitTest_RecordedBacktest.cpp
  Unit/Engine/UnitTest_Replay.cpp)

target_link_libraries(
  bte_engine_tests
  PRIVATE
    bte::engine
    GTest::gtest_main)

target_include_directories(
  bte_engine_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/Src/Backend/Results/Private")

gtest_discover_tests(bte_engine_tests DISCOVERY_TIMEOUT 30)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
  NAME bte_project_standards_tests
  COMMAND
    ${Python3_EXECUTABLE} -m unittest discover
    -s ${CMAKE_CURRENT_SOURCE_DIR}/Unit/Tools
    -p UnitTest_*.py)
set_tests_properties(
  bte_project_standards_tests
  PROPERTIES ENVIRONMENT
    "PYTHONDONTWRITEBYTECODE=1;BTE_SNAPSHOT_BUILDER=$<TARGET_FILE:bte_snapshot_builder>")

if(BTE_BUILD_QT_APP)
  add_executable(
    bte_frontend_tests
    Unit/Frontend/UnitTest_ReplayTab.cpp)

  set_target_properties(bte_frontend_tests PROPERTIES AUTOMOC ON)

  target_link_libraries(
    bte_frontend_tests
    PRIVATE
      bte::frontend
      Qt6::Test
      Qt6::Widgets)

  add_test(
    NAME bte_frontend_tests
    COMMAND bte_frontend_tests -platform offscreen)
  set_tests_properties(
    bte_frontend_tests
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_executable(
    bte_result_replay_frontend_tests
    Unit/Frontend/UnitTest_ResultReplayTab.cpp)

  set_target_properties(
    bte_result_replay_frontend_tests
    PROPERTIES AUTOMOC ON)

  target_link_libraries(
    bte_result_replay_frontend_tests
    PRIVATE
      bte::frontend
      Qt6::Test
      Qt6::Widgets)

  add_test(
    NAME bte_result_replay_frontend_tests
    COMMAND bte_result_replay_frontend_tests -platform offscreen)
  set_tests_properties(
    bte_result_replay_frontend_tests
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_executable(
    bte_backtest_frontend_tests
    Unit/Frontend/UnitTest_BacktestTab.cpp)

  set_target_properties(bte_backtest_frontend_tests PROPERTIES AUTOMOC ON)

  target_link_libraries(
    bte_backtest_frontend_tests
    PRIVATE
      bte::frontend
      Qt6::Test
      Qt6::Widgets)

  add_test(
    NAME bte_backtest_frontend_tests
    COMMAND bte_backtest_frontend_tests -platform offscreen)
  set_tests_properties(
    bte_backtest_frontend_tests
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_executable(
    bte_app_shell_tests
    Unit/Frontend/UnitTest_MainWindow.cpp
    ${CMAKE_SOURCE_DIR}/Src/App/MainWindow.cpp)

  set_target_properties(bte_app_shell_tests PROPERTIES AUTOMOC ON)

  target_include_directories(
    bte_app_shell_tests
    PRIVATE
      ${CMAKE_SOURCE_DIR}/Src/App)

  target_link_libraries(
    bte_app_shell_tests
    PRIVATE
      bte::frontend
      Qt6::Test
      Qt6::Widgets)

  add_test(
    NAME bte_app_shell_tests
    COMMAND bte_app_shell_tests -platform offscreen)
  set_tests_properties(
    bte_app_shell_tests
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_test(
    NAME bte_app_startup_smoke
    COMMAND stockBacktester --smoke-test -platform offscreen)
  set_tests_properties(
    bte_app_startup_smoke
    PROPERTIES
      TIMEOUT 5
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_executable(
    bte_frontend_chart_tests
    Unit/Frontend/UnitTest_QtChartsCandlestickView.cpp)

  set_target_properties(bte_frontend_chart_tests PROPERTIES AUTOMOC ON)

  target_link_libraries(
    bte_frontend_chart_tests
    PRIVATE
      bte::frontend
      Qt6::Test
      Qt6::Widgets)

  add_test(
    NAME bte_frontend_chart_tests
    COMMAND bte_frontend_chart_tests -platform offscreen)
  set_tests_properties(
    bte_frontend_chart_tests
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_executable(
    bte_frontend_vm_tests
    Unit/Frontend/UnitTest_ReplaySessionVm.cpp)

  set_target_properties(bte_frontend_vm_tests PROPERTIES AUTOMOC ON)

  target_link_libraries(
    bte_frontend_vm_tests
    PRIVATE
      bte::bindings
      Qt6::Test)

  add_test(
    NAME bte_frontend_vm_tests
    COMMAND bte_frontend_vm_tests)
  set_tests_properties(
    bte_frontend_vm_tests
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_executable(
    bte_result_replay_tests
    Unit/Bindings/UnitTest_ResultReplay.cpp)

  target_link_libraries(
    bte_result_replay_tests
    PRIVATE
      GTest::gtest_main
      bte::bindings
      bte::data)

  target_include_directories(
    bte_result_replay_tests
    PRIVATE
      "${PROJECT_SOURCE_DIR}/Src/Backend/Results/Private")

  gtest_discover_tests(bte_result_replay_tests DISCOVERY_TIMEOUT 30)

  add_executable(
    bte_replay_fixture_tests
    Unit/Replay/UnitTest_ReplayFixture.cpp)

  target_compile_definitions(
    bte_replay_fixture_tests
    PRIVATE
      BTE_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

  target_link_libraries(
    bte_replay_fixture_tests
    PRIVATE
      GTest::gtest_main
      bte::bindings
      bte::data
      bte::results)

  gtest_discover_tests(bte_replay_fixture_tests DISCOVERY_TIMEOUT 30)

  add_executable(
    bte_backtest_vm_tests
    Unit/Frontend/UnitTest_BacktestSessionVm.cpp)

  set_target_properties(bte_backtest_vm_tests PROPERTIES AUTOMOC ON)

  target_link_libraries(
    bte_backtest_vm_tests
    PRIVATE
      bte::bindings
      Qt6::Test)

  add_test(
    NAME bte_backtest_vm_tests
    COMMAND bte_backtest_vm_tests)
  set_tests_properties(
    bte_backtest_vm_tests
    PROPERTIES
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

  add_executable(
    bte_frontend_data_loader_tests
    Unit/Frontend/UnitTest_ReplayDataLoader.cpp)

  set_target_properties(bte_frontend_data_loader_tests PROPERTIES AUTOMOC ON)

  target_link_libraries(
    bte_frontend_data_loader_tests
    PRIVATE
      bte::bindings
      Qt6::Test)

  add_test(
    NAME bte_frontend_data_loader_tests
    COMMAND bte_frontend_data_loader_tests)
endif()
