add_executable(TacticsTest
  TacticsTest.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsLevel.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsGame.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsAi.cpp
)

target_include_directories(TacticsTest PRIVATE
  ${REPO_ROOT}/lib/Tactics
)

target_link_libraries(TacticsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TacticsTest)
