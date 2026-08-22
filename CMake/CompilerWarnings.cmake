# Baseline warning flags for stockBacktester targets (extend as the tree grows).
if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wdouble-promotion
    -Wformat=2
    -Wnon-virtual-dtor
    -Wnull-dereference
    -Wold-style-cast
    -Woverloaded-virtual
    -Wshadow
    -Wsign-conversion)
endif()
