cmake_minimum_required(VERSION 3.13.4)
project(notdec-evm2llvm
  LANGUAGES CXX
  VERSION 0.0.1
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(FindLLVM)

include_directories("${PROJECT_SOURCE_DIR}/include")
include_directories("${PROJECT_SOURCE_DIR}/include/notdec-evm2llvm")

option(NOTDEC_EVM2LLVM_ENABLE_TESTS "Enable evm2llvm tests." ON)

add_subdirectory(lib)
add_subdirectory(tools)

if (NOTDEC_EVM2LLVM_ENABLE_TESTS)
  enable_testing()
  add_subdirectory(test)
endif ()
