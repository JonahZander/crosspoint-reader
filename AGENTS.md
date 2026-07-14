# Tactics release verification

Run these commands from the repository root before publishing tactics changes:

```sh
./bin/clang-format-fix
git diff --exit-code

pio check -e default
pio run -e default
pio run -e gh_release

cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/test
ctest --test-dir build/test --output-on-failure -j
```

For a desktop smoke test, build and run the simulator:

```sh
cmake --build build/test --target TacticsSim
build/test/tactics_sim/TacticsSim data/tactics/deep_road/11_the_gates.yaml
```

Hardware QA still needs an Xteink X3 or X4. Flash a development build with
`pio run -e default --target upload`, then verify Home > Tactics, a campaign
load, save/resume, and a complete battle.
