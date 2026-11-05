find_package(nlohmann_json 3.12.0 CONFIG REQUIRED)

add_library(
  bte_python_strategy
  STATIC
    ManagedWorkerProcess.h
    ManagedWorkerProcess.cpp
    ManagedWorkerProcessTestHooks.h
    PythonWorkerStrategy.h
    PythonWorkerStrategy.cpp
    RuntimeProfile.h
    RuntimeProfile.cpp
    RuntimeProfileTestHooks.h
    StrategyProtocol.h
    StrategyProtocol.cpp
    worker_bootstrap.py)

add_library(bte::python_strategy ALIAS bte_python_strategy)

target_include_directories(
  bte_python_strategy
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(
  bte_python_strategy
  PUBLIC
    bte::core
    bte::strategy
    nlohmann_json::nlohmann_json)

target_compile_features(bte_python_strategy PUBLIC cxx_std_20)

if(BTE_BUILD_TESTS)
  target_compile_definitions(bte_python_strategy PRIVATE BTE_ENABLE_TEST_HOOKS)
endif()
