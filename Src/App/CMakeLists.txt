if(APPLE)
  add_executable(
    stockBacktester
    MACOSX_BUNDLE
      MainWindow.cpp
      main.cpp)
else()
  add_executable(
    stockBacktester
    WIN32
      MainWindow.cpp
      main.cpp)
endif()

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

if(APPLE)
  set(bte_bundle_resources "$<TARGET_BUNDLE_DIR:stockBacktester>/Contents/Resources")
  set(bte_worker_bootstrap
      "${PROJECT_SOURCE_DIR}/Src/Bindings/Python/worker_bootstrap.py")
  set(bte_bundle_link_dependencies "${bte_worker_bootstrap}")
  add_custom_command(
    TARGET stockBacktester
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${bte_bundle_resources}/Python"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${bte_worker_bootstrap}"
            "${bte_bundle_resources}/Python/worker_bootstrap.py"
    VERBATIM)

  if(BTE_BUNDLE_MANAGED_PYTHON_RUNTIME)
    set(bte_runtime_manifest
        "${BTE_PYTHON_RUNTIME_PROFILE_OUTPUT_DIR}/manifest.json")
    set(bte_runtime_sbom
        "${BTE_PYTHON_RUNTIME_PROFILE_OUTPUT_DIR}/sbom.spdx.json")
    add_custom_command(
      OUTPUT "${bte_runtime_manifest}" "${bte_runtime_sbom}"
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/Tools/BuildPythonRuntime.py"
        --artifacts-dir "${BTE_PYTHON_RUNTIME_ARTIFACTS_DIR}"
        --output-dir "${BTE_PYTHON_RUNTIME_PROFILE_OUTPUT_DIR}"
      DEPENDS
        "${PROJECT_SOURCE_DIR}/Tools/BuildPythonRuntime.py"
        "${PROJECT_SOURCE_DIR}/Tools/PythonRuntimeArtifacts.json"
        "${PROJECT_SOURCE_DIR}/Src/Bindings/Python/stockbt/__init__.py"
        "${PROJECT_SOURCE_DIR}/LICENSE"
        ${bte_python_runtime_artifact_files}
      VERBATIM)
    add_custom_target(
      bte_python_runtime_profile
      DEPENDS "${bte_runtime_manifest}" "${bte_runtime_sbom}")
    add_dependencies(stockBacktester bte_python_runtime_profile)
    list(APPEND bte_bundle_link_dependencies "${bte_runtime_manifest}")

    add_custom_command(
      TARGET stockBacktester
      POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E rm -rf
              "${bte_bundle_resources}/RuntimeProfiles/${BTE_PYTHON_RUNTIME_PROFILE_ID}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${bte_bundle_resources}/RuntimeProfiles"
      COMMAND "${CMAKE_COMMAND}" -E copy_directory
              "${BTE_PYTHON_RUNTIME_PROFILE_OUTPUT_DIR}"
              "${bte_bundle_resources}/RuntimeProfiles/${BTE_PYTHON_RUNTIME_PROFILE_ID}"
      VERBATIM)
  else()
    add_custom_target(
      bte_python_runtime_profile_cleanup
      COMMAND "${CMAKE_COMMAND}" -E rm -rf
              "$<TARGET_BUNDLE_DIR:stockBacktester>/Contents/Resources/RuntimeProfiles/${BTE_PYTHON_RUNTIME_PROFILE_ID}"
      VERBATIM)
    add_dependencies(stockBacktester bte_python_runtime_profile_cleanup)
  endif()

  set_property(
    TARGET stockBacktester
    APPEND
    PROPERTY LINK_DEPENDS ${bte_bundle_link_dependencies})
  install(TARGETS stockBacktester BUNDLE DESTINATION .)
endif()
