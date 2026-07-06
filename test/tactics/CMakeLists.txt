add_executable(TacticsTest
  TacticsTest.cpp
  CampaignPlaytest.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsLevel.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsGame.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsAi.cpp
)

target_include_directories(TacticsTest PRIVATE
  ${REPO_ROOT}/lib/Tactics
)

# Campaign playtest tests load the real level files shipped on the SD card.
target_compile_definitions(TacticsTest PRIVATE
  CAMPAIGN_DIR="${REPO_ROOT}/data/tactics"
)

target_link_libraries(TacticsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TacticsTest)
