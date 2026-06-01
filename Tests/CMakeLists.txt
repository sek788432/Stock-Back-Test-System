add_executable(
  bte_core_tests
  Unit/Core/UnitTest_Bar.cpp)

target_link_libraries(
  bte_core_tests
  PRIVATE
    bte::core
    GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(bte_core_tests)

if(BTE_BUILD_QT_APP)
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
endif()
