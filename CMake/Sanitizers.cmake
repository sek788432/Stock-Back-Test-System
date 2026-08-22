# Optional ASan/UBSan or TSan instrumentation. The runtimes cannot be combined.
if(NOT BTE_SANITIZERS AND NOT BTE_THREAD_SANITIZER)
  return()
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(
    STATUS "Sanitizers ignored: compiler is ${CMAKE_CXX_COMPILER_ID}")
  return()
endif()

if(BTE_SANITIZERS AND BTE_THREAD_SANITIZER)
  message(FATAL_ERROR "ASan/UBSan and TSan cannot be enabled together")
endif()

if(BTE_THREAD_SANITIZER)
  message(STATUS "Enabling ThreadSanitizer")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g -O1)
  add_link_options(-fsanitize=thread)
else()
  message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()
