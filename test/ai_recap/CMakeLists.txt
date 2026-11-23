add_executable(AiRecapSourceTest
  AiRecapSourceTest.cpp
  ${REPO_ROOT}/src/ai/AiRecapSource.cpp
  ${REPO_ROOT}/src/ai/AiRecapJob.cpp
)
target_include_directories(AiRecapSourceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)
target_link_libraries(AiRecapSourceTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AiRecapSourceTest)
