function(add_evm2llvm_fixture name)
  set(output_ll "${CMAKE_CURRENT_BINARY_DIR}/${name}.ll")
  set(output_bc "${CMAKE_CURRENT_BINARY_DIR}/${name}.bc")

  add_test(
    NAME evm2llvm.fixture.${name}.emit
    COMMAND evm2llvm
      --facts "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${name}"
      --output "${output_ll}"
  )

  add_test(
    NAME evm2llvm.fixture.${name}.llvm-as
    COMMAND "${NOTDEC_LLVM_INSTALL_DIR}/bin/llvm-as"
      "${output_ll}"
      -o "${output_bc}"
  )
  set_tests_properties(
    evm2llvm.fixture.${name}.llvm-as
    PROPERTIES DEPENDS evm2llvm.fixture.${name}.emit
  )

  add_test(
    NAME evm2llvm.fixture.${name}.opt-verify
    COMMAND "${NOTDEC_LLVM_INSTALL_DIR}/bin/opt"
      -passes=verify
      -disable-output
      "${output_bc}"
  )
  set_tests_properties(
    evm2llvm.fixture.${name}.opt-verify
    PROPERTIES DEPENDS evm2llvm.fixture.${name}.llvm-as
  )
endfunction()

add_evm2llvm_fixture(arithmetic)
add_evm2llvm_fixture(state)
add_evm2llvm_fixture(private_call)
add_evm2llvm_fixture(phi_branch)

if (NOTDEC_EVM2LLVM_ENABLE_GIGAHORSE_TESTS)
  set(EVM2LLVM_GIGAHORSE_LL "${CMAKE_CURRENT_BINARY_DIR}/gigahorse-arithmetic.ll")
  set(EVM2LLVM_GIGAHORSE_BC "${CMAKE_CURRENT_BINARY_DIR}/gigahorse-arithmetic.bc")

  add_test(
    NAME evm2llvm.gigahorse.arithmetic.emit
    COMMAND "${PROJECT_SOURCE_DIR}/scripts/notdec-evm2llvm.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bytecode/arithmetic/arithmetic.hex"
      --output "${EVM2LLVM_GIGAHORSE_LL}"
      --evm2llvm "$<TARGET_FILE:evm2llvm>"
      --timeout-secs 120
  )
  set_tests_properties(
    evm2llvm.gigahorse.arithmetic.emit
    PROPERTIES
      TIMEOUT 180
  )

  add_test(
    NAME evm2llvm.gigahorse.arithmetic.llvm-as
    COMMAND "${NOTDEC_LLVM_INSTALL_DIR}/bin/llvm-as"
      "${EVM2LLVM_GIGAHORSE_LL}"
      -o "${EVM2LLVM_GIGAHORSE_BC}"
  )
  set_tests_properties(
    evm2llvm.gigahorse.arithmetic.llvm-as
    PROPERTIES DEPENDS evm2llvm.gigahorse.arithmetic.emit
  )

  add_test(
    NAME evm2llvm.gigahorse.arithmetic.opt-verify
    COMMAND "${NOTDEC_LLVM_INSTALL_DIR}/bin/opt"
      -passes=verify
      -disable-output
      "${EVM2LLVM_GIGAHORSE_BC}"
  )
  set_tests_properties(
    evm2llvm.gigahorse.arithmetic.opt-verify
    PROPERTIES DEPENDS evm2llvm.gigahorse.arithmetic.llvm-as
  )
endif ()
