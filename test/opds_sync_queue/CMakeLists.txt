add_executable(OpdsSyncQueueTest
  OpdsSyncQueueTest.cpp
  ${REPO_ROOT}/src/activities/browser/OpdsSyncQueue.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsLibraryMatch.cpp
)

target_include_directories(OpdsSyncQueueTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/browser
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(OpdsSyncQueueTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsSyncQueueTest)
