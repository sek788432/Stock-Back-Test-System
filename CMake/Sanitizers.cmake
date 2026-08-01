# Optional ASan/UBSan (Clang/GCC). Enabled when -DBTE_SANITIZERS=ON is passed.
if(NOT BTE_SANITIZERS)
  return()
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(
    STATUS "BTE_SANITIZERS=ON ignored: compiler is ${CMAKE_CXX_COMPILER_ID}")
  return()
endif()

message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
add_link_options(-fsanitize=address,undefined)
