add_executable(evm2llvm
  evm2llvm.cpp
)

target_link_libraries(evm2llvm
  PRIVATE
    notdec-evm2llvm-core
)
