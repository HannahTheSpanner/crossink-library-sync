# Counts every heap request the parser and expat make by interposing glibc's
# malloc family, so it builds only where __libc_malloc exists (see the
# parent CMakeLists.txt).
#
# Built against the firmware's own expat (lib/expat) with platformio.ini's
# expat flags, so the buffer growth measured is the device's.
add_library(opds_device_expat STATIC
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)
target_include_directories(opds_device_expat PUBLIC ${REPO_ROOT}/lib/expat)
target_compile_definitions(opds_device_expat PUBLIC XML_GE=0 XML_CONTEXT_BYTES=1024)
target_compile_options(opds_device_expat PRIVATE -w)

add_executable(OpdsParserMemoryTest
  OpdsParserMemoryTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp
)

target_include_directories(OpdsParserMemoryTest BEFORE PRIVATE
  ${REPO_ROOT}/lib/expat
  ${REPO_ROOT}/test/opds_parser/stubs
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(OpdsParserMemoryTest PRIVATE
  crosspoint_test_common
  opds_device_expat
  GTest::gtest_main
)

gtest_discover_tests(OpdsParserMemoryTest)
