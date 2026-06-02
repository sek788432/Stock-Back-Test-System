add_executable(
  bte_core_tests
  Unit/Core/UnitTest_Bar.cpp
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

if(BTE_BUILD_QT_APP)
  add_executable(
    bte_frontend_vm_tests
    Frontend/UnitTest_ReplaySessionVm.cpp)

  set_target_properties(bte_frontend_vm_tests PROPERTIES AUTOMOC ON)

  target_link_libraries(
    bte_frontend_vm_tests
    PRIVATE
      bte::frontend
      Qt6::Test
      Qt6::Widgets)

  add_test(
    NAME bte_frontend_vm_tests
    COMMAND bte_frontend_vm_tests -platform offscreen)

  add_executable(
    bte_frontend_tests
    Frontend/UnitTest_ReplayTab.cpp)

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

  add_executable(
    bte_frontend_chart_tests
    Frontend/UnitTest_QtChartsCandlestickView.cpp)

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
endif()
