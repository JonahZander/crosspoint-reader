# Interactive/scriptable host simulator for the tactics mini-game — not a
# test, but built with the suite so `cmake --build` keeps it compiling.
add_executable(TacticsSim
  TacticsSim.cpp
  SimCanvas.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsLevel.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsGame.cpp
  ${REPO_ROOT}/lib/Tactics/TacticsAi.cpp
)

target_include_directories(TacticsSim PRIVATE
  ${REPO_ROOT}/lib/Tactics
)

target_link_libraries(TacticsSim PRIVATE crosspoint_test_common)
