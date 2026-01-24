# Repository Guidelines

## Project Structure & Module Organization
Core headers live in `include/` (AST, parser, interpreter, runtime values) and their C++20 implementations sit in `src/`. Example programs that exercise new language features reside in `examples/`, while JavaScript snippets for manual verification live under `tests/` alongside small C++ harnesses. Conformance assets are split between the optional `test262/` runner sources and the downloaded suite in `test262_sample/`. Helper scripts (downloaders, tooling) live in `scripts/`, and reusable CMake modules are kept in `cmake/`.

## Build, Test, and Development Commands
Configure via `cmake -S . -B build -DTINYJS_BUILD_TESTS=ON`; add `-DUSE_SIMPLE_REGEX=ON` for the pure C++ lexer. Build everything with `cmake --build build -j$(nproc)`—artifacts such as `libtinyjs.a` and the `tinyjs` REPL land inside `build/`. Run the full CTest battery with `ctest --test-dir build --output-on-failure`, or invoke an individual binary like `build/tests/gc_test` while iterating. Test262 coverage requires fetching the suite (`scripts/download_test262.sh`) and calling `build/test262_runner ../test262 --test language/expressions` with focused paths to keep runs fast.

## Coding Style & Naming Conventions
Stick to modern C++20 with exceptions and RTTI disabled (the toolchain adds `-fno-rtti`, so keep polymorphism virtual-only). Indent with two spaces, braces stay on the same line, and prefer `auto` when type context is obvious. Filenames use `snake_case.cc` / `.h`, classes and structs are `PascalCase`, member functions `camelCase`, and constants follow `kPascalCase`. Encapsulate new runtime components under the `tinyjs` namespace. Run `cmake --build build --target tinyjs` before opening a PR to ensure headers stay warning-free on all supported compilers.

## Testing Guidelines
When adding runtime features, pair each change with at least one scenario in `tests/` plus a corresponding C++ harness (use the `add_tinyjs_test` macro in `CMakeLists.txt`). Integration-heavy contributions should also stage a focused Test262 shard and document the exact folder you exercised. Name new tests after the behavior they guard (`test_template_literals.js`, `generator_forof_test.cc`) so failures map cleanly to spec areas. Aim to keep existing suites green under `ctest` and file TODOs before skipping anything.

## Commit & Pull Request Guidelines
Follow the short, imperative subject style seen in `git log` (e.g., `Improve library modularity and integration`). Group related work in a single commit when possible, describing what and why rather than how. PRs should include a synopsis of the change, configuration flags used, test evidence (`ctest`, specific Test262 shards), and links to any tracked issues. Add screenshots or REPL transcripts only when UI/REPL behavior changes.
