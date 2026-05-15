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
add_evm2llvm_fixture(private_call_multi_return)
add_evm2llvm_fixture(phi_branch)

if (NOTDEC_EVM2LLVM_ENABLE_GIGAHORSE_TESTS)
  function(add_evm2llvm_gigahorse_test name bytecode)
    set(output_ll "${CMAKE_CURRENT_BINARY_DIR}/gigahorse-${name}.ll")
    set(output_bc "${CMAKE_CURRENT_BINARY_DIR}/gigahorse-${name}.bc")
    set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/gigahorse-${name}.work")

    add_test(
      NAME evm2llvm.gigahorse.${name}.emit
      COMMAND "${PROJECT_SOURCE_DIR}/scripts/notdec-evm2llvm.py"
        "${bytecode}"
        --output "${output_ll}"
        --evm2llvm "$<TARGET_FILE:evm2llvm>"
        --gigahorse-dir "${NOTDEC_EVM2LLVM_GIGAHORSE_ROOT}"
        --work-dir "${work_dir}"
        --timeout-secs 180
        --gigahorse-extra-arg=-i
        --gigahorse-extra-arg=--disable_inline
        --gigahorse-extra-arg=--disable_scalable_fallback
    )
    set_tests_properties(
      evm2llvm.gigahorse.${name}.emit
      PROPERTIES
        TIMEOUT 240
    )

    add_test(
      NAME evm2llvm.gigahorse.${name}.llvm-as
      COMMAND "${NOTDEC_LLVM_INSTALL_DIR}/bin/llvm-as"
        "${output_ll}"
        -o "${output_bc}"
    )
    set_tests_properties(
      evm2llvm.gigahorse.${name}.llvm-as
      PROPERTIES DEPENDS evm2llvm.gigahorse.${name}.emit
    )

    add_test(
      NAME evm2llvm.gigahorse.${name}.opt-verify
      COMMAND "${NOTDEC_LLVM_INSTALL_DIR}/bin/opt"
        -passes=verify
        -disable-output
        "${output_bc}"
    )
    set_tests_properties(
      evm2llvm.gigahorse.${name}.opt-verify
      PROPERTIES DEPENDS evm2llvm.gigahorse.${name}.llvm-as
    )
  endfunction()

  add_evm2llvm_gigahorse_test(
    arithmetic
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/bytecode/arithmetic/arithmetic.hex"
  )
  add_evm2llvm_gigahorse_test(
    simple-storage
    "${NOTDEC_EVM2LLVM_GIGAHORSE_ROOT}/tests/via-ir/simple-storage.hex"
  )
  add_evm2llvm_gigahorse_test(
    owner-guard
    "${NOTDEC_EVM2LLVM_GIGAHORSE_ROOT}/tests/guards/owner-guard.hex"
  )
  add_evm2llvm_gigahorse_test(
    map1
    "${NOTDEC_EVM2LLVM_GIGAHORSE_ROOT}/tests/storage/map1.hex"
  )

  if (NOTDEC_EVM2LLVM_ENABLE_GIGAHORSE_SLOW_TESTS)
    add_evm2llvm_gigahorse_test(
      long-running
      "${NOTDEC_EVM2LLVM_GIGAHORSE_ROOT}/examples/long_running.hex"
    )
  endif ()
endif ()
