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
  Unit/Data/UnitTest_CsvBarStream.cpp)

target_link_libraries(
  bte_data_tests
  PRIVATE
    bte::data
    GTest::gtest_main)

target_compile_definitions(
  bte_data_tests
  PRIVATE
    BTE_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Fixtures")

gtest_discover_tests(bte_data_tests DISCOVERY_TIMEOUT 30)

add_executable(
  bte_engine_tests
  Unit/Engine/UnitTest_Backtest.cpp
  Unit/Engine/UnitTest_Replay.cpp
  Unit/Engine/UnitTest_ResultSnapshotStore.cpp)

target_link_libraries(
  bte_engine_tests
  PRIVATE
    bte::engine
    GTest::gtest_main)

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
  PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

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
