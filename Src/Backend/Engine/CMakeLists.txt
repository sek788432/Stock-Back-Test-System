add_library(
  bte_engine
  STATIC
    Include/Bte/Engine/Replay.h
    Private/Replay.cpp)
    Include/Bte/Engine/ResultSnapshotStore.h
    Private/Replay.cpp
    Private/ResultSnapshotStore.cpp)

add_library(bte::engine ALIAS bte_engine)

target_include_directories(
  bte_engine
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Private")

target_link_libraries(
  bte_engine
  PUBLIC
    bte::core
    bte::data)

target_compile_features(bte_engine PUBLIC cxx_std_20)
