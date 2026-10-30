function(bte_enable_coverage)
  if(NOT BTE_COVERAGE)
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "BTE_COVERAGE requires a GNU-compatible compiler")
  endif()

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(BTE_COVERAGE_FORMAT "llvm-source-based" CACHE INTERNAL
        "Coverage data format selected for the active compiler" FORCE)
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping -O0 -g)
    add_link_options(-fprofile-instr-generate)
  else()
    set(BTE_COVERAGE_FORMAT "gcov" CACHE INTERNAL
        "Coverage data format selected for the active compiler" FORCE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
  endif()
endfunction()
