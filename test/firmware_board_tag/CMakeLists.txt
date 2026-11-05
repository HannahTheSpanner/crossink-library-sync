add_executable(FirmwareBoardTagTest
  FirmwareBoardTagTest.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)

target_include_directories(FirmwareBoardTagTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
)

target_compile_definitions(FirmwareBoardTagTest PRIVATE
  FREEINK_DEVICE_STICKY=1
)

target_link_libraries(FirmwareBoardTagTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareBoardTagTest)

add_executable(PiccoFirmwareBoardTagTest
  FirmwareBoardTagTest.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)
target_include_directories(PiccoFirmwareBoardTagTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
)
target_compile_definitions(PiccoFirmwareBoardTagTest PRIVATE FREEINK_DEVICE_PICCO=1)
target_link_libraries(PiccoFirmwareBoardTagTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PiccoFirmwareBoardTagTest TEST_PREFIX "Picco.")

foreach(board_variant IN ITEMS METALIO_EINK4 EEGO_A4)
  set(target ${board_variant}FirmwareBoardTagTest)
  add_executable(${target}
    FirmwareBoardTagTest.cpp
    ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
  )
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${REPO_ROOT}/src/network
  )
  target_compile_definitions(${target} PRIVATE FREEINK_DEVICE_${board_variant}=1)
  target_link_libraries(${target} PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(${target} TEST_PREFIX "${board_variant}.")
endforeach()
