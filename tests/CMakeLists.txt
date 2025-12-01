cmake_minimum_required(VERSION 3.10)

# ============================================================================
# SILO COMPREHENSIVE TESTS
# ============================================================================

# Test: Allocator + Tuple Integration (Real System)
add_executable(test_silo_allocator_tuple
    silo/test_allocator_tuple.cc
)

target_link_libraries(test_silo_allocator_tuple
    mako
    ${GTEST_LIBRARIES}
    ${GTEST_MAIN_LIBRARIES}
    pthread
    numa
)

target_include_directories(test_silo_allocator_tuple PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
)

# Test: Varint (Utility)
add_executable(test_silo_varint
    silo/test_varint.cc
)

target_link_libraries(test_silo_varint
    mako
    ${GTEST_LIBRARIES}
    ${GTEST_MAIN_LIBRARIES}
    pthread
)

target_include_directories(test_silo_varint PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
)

# Test: RCU + Thread
add_executable(test_silo_rcu_thread
    silo/test_rcu_thread.cc
)

target_link_libraries(test_silo_rcu_thread
    mako
    ${GTEST_LIBRARIES}
    ${GTEST_MAIN_LIBRARIES}
    pthread
    numa
)

target_include_directories(test_silo_rcu_thread PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
)

# ============================================================================
# STO COMPREHENSIVE TESTS  
# ============================================================================

# Test: TRcu (Transaction RCU)
add_executable(test_sto_transaction
    sto/test_transaction.cc
)

target_link_libraries(test_sto_transaction
    mako
    ${GTEST_LIBRARIES}
    ${GTEST_MAIN_LIBRARIES}
    pthread
)

target_include_directories(test_sto_transaction PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
)

# Test: Transaction Real (Integration)
add_executable(test_sto_transaction_real
    sto/test_transaction_real.cc
)

target_link_libraries(test_sto_transaction_real
    mako
    ${GTEST_LIBRARIES}
    ${GTEST_MAIN_LIBRARIES}
    pthread
)

target_include_directories(test_sto_transaction_real PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mako
)

# ============================================================================
# BENCHMARKS (requires Google Benchmark)
# ============================================================================

# Note: Benchmarks require Google Benchmark library
# Install with: sudo apt-get install libbenchmark-dev
# Uncomment when benchmark library is available:

# add_executable(bench_silo_varint silo/bench_varint.cc)
# target_link_libraries(bench_silo_varint mako gtest pthread benchmark benchmark_main)
# target_include_directories(bench_silo_varint PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/mako)

# add_executable(bench_sto_transaction sto/bench_transaction.cc)
# target_link_libraries(bench_sto_transaction mako gtest pthread benchmark benchmark_main)
# target_include_directories(bench_sto_transaction PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/src/mako ${CMAKE_SOURCE_DIR}/src/mako/benchmarks)

# ============================================================================
# CTEST REGISTRATION
# ============================================================================

enable_testing()

add_test(NAME SiloAllocatorTupleTests COMMAND test_silo_allocator_tuple)
add_test(NAME SiloVarintTests COMMAND test_silo_varint)
add_test(NAME SiloRCUThreadTests COMMAND test_silo_rcu_thread)
add_test(NAME STOTransactionTests COMMAND test_sto_transaction)
add_test(NAME STOTransactionRealTests COMMAND test_sto_transaction_real)

# ============================================================================
# CUSTOM TARGETS
# ============================================================================

# Run all unit tests
add_custom_target(run_all_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_silo_allocator_tuple test_silo_varint test_silo_rcu_thread test_sto_transaction test_sto_transaction_real
    COMMENT "Running all unit tests"
)

# Run only Silo tests
add_custom_target(run_silo_tests
    COMMAND ./test_silo_allocator_tuple
    COMMAND ./test_silo_varint
    COMMAND ./test_silo_rcu_thread
    DEPENDS test_silo_allocator_tuple test_silo_varint test_silo_rcu_thread
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running Silo tests"
)

# Run only STO tests
add_custom_target(run_sto_tests
    COMMAND ./test_sto_transaction
    COMMAND ./test_sto_transaction_real
    DEPENDS test_sto_transaction test_sto_transaction_real
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Running STO tests"
)
