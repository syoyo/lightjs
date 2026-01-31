# Repository Guidelines

## Project Structure & Module Organization
Core headers live in `include/` (AST, parser, interpreter, runtime values) and their C++20 implementations sit in `src/`. Example programs that exercise new language features reside in `examples/`, while JavaScript snippets for manual verification live under `tests/` alongside small C++ harnesses. Conformance assets are split between the optional `test262/` runner sources and the downloaded suite in `test262_sample/`. Helper scripts (downloaders, tooling) live in `scripts/`, and reusable CMake modules are kept in `cmake/`.

## Build, Test, and Development Commands
Configure via `cmake -S . -B build -DLIGHTJS_BUILD_TESTS=ON`; add `-DUSE_SIMPLE_REGEX=ON` for the pure C++ lexer. Build everything with `cmake --build build -j$(nproc)`—artifacts such as `liblightjs.a` and the `lightjs` REPL land inside `build/`. Run the full CTest battery with `ctest --test-dir build --output-on-failure`, or invoke an individual binary like `build/tests/gc_test` while iterating. Test262 coverage requires fetching the suite (`scripts/download_test262.sh`) and calling `build/test262_runner ../test262 --test language/expressions` with focused paths to keep runs fast.

To run a single test:
- Run `ctest --test-dir build --output-on-failure -R gc_test` to run a specific test
- Or `build/tests/gc_test` to run an individual test binary directly

## Coding Style & Naming Conventions
Stick to modern C++20 with exceptions and RTTI disabled (the toolchain adds `-fno-rtti`, so keep polymorphism virtual-only). Indent with two spaces, braces stay on the same line, and prefer `auto` when type context is obvious. Filenames use `snake_case.cc` / `.h`, classes and structs are `PascalCase`, member functions `camelCase`, and constants follow `kPascalCase`. Encapsulate new runtime components under the `lightjs` namespace.

## Imports and Formatting
- Use #include for C++ standard library and system headers
- Use #include for project headers in the include directory
- Avoid including headers that are not needed (no forward declarations)
- Use standard C++20 features when available

## Error Handling
- All errors should be handled gracefully
- Use proper exception handling where appropriate
- Avoid silent failures
- Log errors appropriately

## Testing Guidelines
When adding runtime features, pair each change with at least one scenario in `tests/` plus a corresponding C++ harness (use the `add_lightjs_test` macro in `CMakeLists.txt`). Integration-heavy contributions should also stage a focused Test262 shard and document the exact folder you exercised. Name new tests after the behavior they guard (`test_template_literals.js`, `generator_forof_test.cc`) so failures map cleanly to spec areas. Aim to keep existing suites green under `ctest` and file TODOs before skipping anything.

## Cursor/Copilot Rules
If Cursor/Copilot is enabled for this repository, follow these rules:
- Maintain consistency with existing code style and patterns
- Use descriptive variable and function names that follow the codebase conventions
- Ensure all new code follows proper C++20 idioms and practices
- Maintain readability and documentation in code comments

## Commit & Pull Request Guidelines
Follow the short, imperative subject style seen in `git log` (e.g., `Improve library modularity and integration`). Group related work in a single commit when possible, describing what and why rather than how. PRs should include a synopsis of the change, configuration flags used, test evidence (`ctest`, specific Test262 shards), and links to any tracked issues. Add screenshots or REPL transcripts only when UI/REPL behavior changes.