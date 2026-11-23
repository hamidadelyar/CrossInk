add_executable(AiRecapProtocolTest
  AiRecapProtocolTest.cpp
  ${REPO_ROOT}/src/ai/AiRecapProtocol.cpp
  ${REPO_ROOT}/src/ai/AiRecapSettings.cpp
)
target_compile_definitions(AiRecapProtocolTest PRIVATE SIMULATOR)
target_include_directories(AiRecapProtocolTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/ai
)
target_link_libraries(AiRecapProtocolTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(AiRecapProtocolTest)
