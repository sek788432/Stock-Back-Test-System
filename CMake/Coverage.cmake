function(bte_enable_coverage)
  if(NOT BTE_COVERAGE)
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "BTE_COVERAGE requires a GNU-compatible compiler")
  endif()

  add_compile_options(--coverage -O0 -g)
  add_link_options(--coverage)
endfunction()
