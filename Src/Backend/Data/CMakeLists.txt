add_library(
  bte_data
  STATIC
    Include/Bte/Data/BarStream.h
    Include/Bte/Data/ReleaseSnapshot.h
    Include/Bte/Data/SegmentRetention.h
    Private/CsvBarStream.cpp
    Private/ReleaseSnapshot.cpp
    Private/ReleaseSnapshotTestHooks.h
    Private/SegmentRetention.cpp
    Private/SegmentRetentionTestHooks.h)

add_library(bte::data ALIAS bte_data)

target_include_directories(
  bte_data
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private")

target_link_libraries(
  bte_data
  PUBLIC
    bte::core
  PRIVATE
    SQLite::SQLite3)

target_compile_features(bte_data PUBLIC cxx_std_20)

add_executable(
  bte_snapshot_builder
  Private/SnapshotBuilderMain.cpp)

target_link_libraries(
  bte_snapshot_builder
  PRIVATE
    bte::data)

target_compile_features(bte_snapshot_builder PRIVATE cxx_std_20)
