set(EVM2LLVM_ARITHMETIC_LL "${CMAKE_CURRENT_BINARY_DIR}/arithmetic.ll")
set(EVM2LLVM_ARITHMETIC_BC "${CMAKE_CURRENT_BINARY_DIR}/arithmetic.bc")

add_test(
  NAME evm2llvm.fixture.arithmetic.emit
  COMMAND evm2llvm
    --facts "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/arithmetic"
    --output "${EVM2LLVM_ARITHMETIC_LL}"
)

add_test(
  NAME evm2llvm.fixture.arithmetic.llvm-as
  COMMAND "${NOTDEC_LLVM_INSTALL_DIR}/bin/llvm-as"
    "${EVM2LLVM_ARITHMETIC_LL}"
    -o "${EVM2LLVM_ARITHMETIC_BC}"
)
set_tests_properties(
  evm2llvm.fixture.arithmetic.llvm-as
  PROPERTIES DEPENDS evm2llvm.fixture.arithmetic.emit
)

add_test(
  NAME evm2llvm.fixture.arithmetic.opt-verify
  COMMAND "${NOTDEC_LLVM_INSTALL_DIR}/bin/opt"
    -passes=verify
    -disable-output
    "${EVM2LLVM_ARITHMETIC_BC}"
)
set_tests_properties(
  evm2llvm.fixture.arithmetic.opt-verify
  PROPERTIES DEPENDS evm2llvm.fixture.arithmetic.llvm-as
)
