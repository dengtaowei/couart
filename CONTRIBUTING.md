# Contributing

Build and run the tests before sending a PR:

```bash
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

C11, no GNU-only control-flow tricks beyond POSIX extras already enabled
in `CMakeLists.txt`. Match the surrounding style: short names, `couart_`
prefix, `-1` / `0` return codes.

Please keep board-specific bring-up scripts out of this repo. couart is
the session layer, not a firmware test harness.
