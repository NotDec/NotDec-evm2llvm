add_library(notdec-evm2llvm-core STATIC
  EvmRuntimeDecls.cpp
  FactLoader.cpp
  InstructionLowerer.cpp
  LlvmLowerer.cpp
  TacProgram.cpp
)

target_include_directories(notdec-evm2llvm-core
  PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(notdec-evm2llvm-core
  PUBLIC
    notdec_evm2llvm_llvm_deps
)
