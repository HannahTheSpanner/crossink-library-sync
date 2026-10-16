add_executable(OpdsLibraryMatchTest
  OpdsLibraryMatchTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsLibraryMatch.cpp
)

target_include_directories(OpdsLibraryMatchTest PRIVATE
  ${REPO_ROOT}/lib/OpdsParser
)

target_link_libraries(OpdsLibraryMatchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OpdsLibraryMatchTest)
