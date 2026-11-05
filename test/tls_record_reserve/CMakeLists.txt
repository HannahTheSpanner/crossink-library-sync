add_executable(TlsRecordReserveTest
  TlsRecordReserveTest.cpp
  ${REPO_ROOT}/src/network/TlsRecordReserve.cpp
)

target_include_directories(TlsRecordReserveTest PRIVATE
  stubs
  ${REPO_ROOT}/src
)

target_link_libraries(TlsRecordReserveTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TlsRecordReserveTest)
