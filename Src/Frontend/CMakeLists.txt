add_library(
  bte_frontend
  STATIC
    Replay/ReplayTab.cpp)

add_library(bte::frontend ALIAS bte_frontend)

target_include_directories(
  bte_frontend
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/Include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/Replay")

target_link_libraries(
  bte_frontend
  PUBLIC
    Qt6::Widgets)

target_compile_features(bte_frontend PUBLIC cxx_std_20)
