cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "Pinned vcpkg dependency toolchain")
  else()
    message(FATAL_ERROR
      "Pinned vcpkg manifest provenance is required. Set VCPKG_ROOT to the "
      "verified checkout documented in Docs/BUILD.md or pass its vcpkg "
      "CMAKE_TOOLCHAIN_FILE explicitly.")
  endif()
endif()

if(NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  message(FATAL_ERROR
    "Pinned vcpkg manifest provenance is required, but CMAKE_TOOLCHAIN_FILE "
    "does not exist: ${CMAKE_TOOLCHAIN_FILE}")
endif()

project(
  stockBacktester
  VERSION 0.1.0
  LANGUAGES CXX)

if(NOT VCPKG_TOOLCHAIN OR NOT VCPKG_MANIFEST_MODE OR
   NOT VCPKG_MANIFEST_INSTALL)
  message(FATAL_ERROR
    "Pinned vcpkg manifest provenance is required. The configured toolchain "
    "must enable vcpkg manifest installation for the root vcpkg.json.")
endif()

cmake_path(
  ABSOLUTE_PATH VCPKG_MANIFEST_DIR
  BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
  NORMALIZE
  OUTPUT_VARIABLE bte_vcpkg_manifest_directory)
if(NOT bte_vcpkg_manifest_directory STREQUAL CMAKE_SOURCE_DIR)
  message(FATAL_ERROR
    "Pinned vcpkg manifest provenance is required from ${CMAKE_SOURCE_DIR}, "
    "not ${bte_vcpkg_manifest_directory}.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BTE_BUILD_TESTS "Build GoogleTest-based unit tests" ON)
option(BTE_SANITIZERS "Enable ASan/UBSan on Clang/GNU builds" OFF)
option(BTE_THREAD_SANITIZER "Enable TSan on Clang/GNU builds" OFF)
option(BTE_BUILD_QT_APP "Build the Qt desktop app shell" OFF)
option(BTE_COVERAGE "Instrument tests for gcov-compatible coverage" OFF)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMake")
include(CompilerWarnings)
include(Sanitizers)
include(Coverage)
bte_enable_coverage()

find_package(SQLite3 3.53.4 EXACT REQUIRED)

if(NOT VCPKG_INSTALLED_DIR OR NOT VCPKG_TARGET_TRIPLET)
  message(FATAL_ERROR
    "SQLite3 dependency provenance violation: vcpkg did not identify its "
    "installed root and target triplet.")
endif()

file(
  REAL_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
  bte_vcpkg_triplet_root)
if(NOT IS_DIRECTORY "${bte_vcpkg_triplet_root}")
  message(FATAL_ERROR
    "SQLite3 dependency provenance violation: active vcpkg triplet tree "
    "does not exist: ${bte_vcpkg_triplet_root}")
endif()

function(bte_require_vcpkg_sqlite_path source resolved_path)
  if(NOT IS_ABSOLUTE "${resolved_path}")
    message(FATAL_ERROR
      "SQLite3 dependency provenance violation: ${source} did not resolve "
      "to an absolute path: ${resolved_path}")
  endif()

  file(REAL_PATH "${resolved_path}" bte_sqlite_path)
  cmake_path(
    IS_PREFIX bte_vcpkg_triplet_root "${bte_sqlite_path}"
    NORMALIZE bte_sqlite_path_is_from_vcpkg)
  if(NOT bte_sqlite_path_is_from_vcpkg)
    message(FATAL_ERROR
      "SQLite3 dependency provenance violation: ${source} resolved outside "
      "the active vcpkg triplet tree ${bte_vcpkg_triplet_root}: "
      "${bte_sqlite_path}")
  endif()
endfunction()

foreach(bte_sqlite_include IN LISTS SQLite3_INCLUDE_DIRS)
  bte_require_vcpkg_sqlite_path(
    "SQLite3_INCLUDE_DIRS" "${bte_sqlite_include}")
endforeach()
set(bte_sqlite_library_from_vcpkg FALSE)
foreach(bte_sqlite_library IN LISTS SQLite3_LIBRARIES)
  # FindSQLite3 may append platform linker dependencies such as `dl` to the
  # absolute SQLite artifact. Those names are not SQLite provenance evidence.
  if(IS_ABSOLUTE "${bte_sqlite_library}")
    bte_require_vcpkg_sqlite_path(
      "SQLite3_LIBRARIES" "${bte_sqlite_library}")
    set(bte_sqlite_library_from_vcpkg TRUE)
  endif()
endforeach()
if(NOT bte_sqlite_library_from_vcpkg)
  message(FATAL_ERROR
    "SQLite3 dependency provenance violation: SQLite3_LIBRARIES did not "
    "contain an absolute artifact from the active vcpkg triplet tree: "
    "${SQLite3_LIBRARIES}")
endif()

if(NOT TARGET SQLite::SQLite3)
  message(FATAL_ERROR
    "SQLite3 dependency provenance violation: FindSQLite3 did not provide "
    "SQLite::SQLite3.")
endif()

set(bte_sqlite_target_properties
  INTERFACE_INCLUDE_DIRECTORIES
  IMPORTED_LOCATION
  IMPORTED_IMPLIB)
get_target_property(
  bte_sqlite_imported_configurations
  SQLite::SQLite3 IMPORTED_CONFIGURATIONS)
foreach(bte_sqlite_configuration IN LISTS bte_sqlite_imported_configurations)
  string(TOUPPER "${bte_sqlite_configuration}" bte_sqlite_configuration)
  list(APPEND bte_sqlite_target_properties
    "IMPORTED_LOCATION_${bte_sqlite_configuration}"
    "IMPORTED_IMPLIB_${bte_sqlite_configuration}")
endforeach()

foreach(bte_sqlite_target_property IN LISTS bte_sqlite_target_properties)
  get_target_property(
    bte_sqlite_target_paths
    SQLite::SQLite3 "${bte_sqlite_target_property}")
  if(bte_sqlite_target_paths AND
     NOT bte_sqlite_target_paths MATCHES "-NOTFOUND$")
    foreach(bte_sqlite_target_path IN LISTS bte_sqlite_target_paths)
      bte_require_vcpkg_sqlite_path(
        "SQLite::SQLite3 ${bte_sqlite_target_property}"
        "${bte_sqlite_target_path}")
    endforeach()
  endif()
endforeach()

add_subdirectory(Src/Backend/Core)
add_subdirectory(Src/Backend/Data)
add_subdirectory(Src/Backend/Indicators)
add_subdirectory(Src/Backend/Strategy)
add_subdirectory(Src/Backend/Results)
add_subdirectory(Src/Backend/Engine)

if(BTE_BUILD_QT_APP)
  set(BTE_QT_COMPONENTS Charts Concurrent Core Widgets)
  if(BTE_BUILD_TESTS)
    list(APPEND BTE_QT_COMPONENTS Test)
  endif()
  find_package(Qt6 6.8 REQUIRED COMPONENTS ${BTE_QT_COMPONENTS})
  set(CMAKE_AUTOMOC ON)

  add_subdirectory(Src/Bindings)
  add_subdirectory(Src/Frontend)
  add_subdirectory(Src/App)
endif()

if(BTE_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        f8d7d77c06936315286eb55f8de22cd23c188571) # v1.14.0
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  set_target_properties(
    gtest
    gtest_main
    gmock
    gmock_main
    PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

  enable_testing()
  add_subdirectory(Tests)
endif()
