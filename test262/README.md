# Test262 Support for LightJS

This directory contains the test262 conformance testing framework for LightJS.

## Overview

Test262 is the official ECMAScript conformance test suite. This framework allows LightJS to run test262 tests to validate its JavaScript implementation compliance.

## Components

- `test262_runner.cc` - Main test runner executable
- `test262_harness.cc/h` - Test262 harness implementation with helper functions
- `../scripts/download_test262.sh` - Script to download the official test262 suite

## Building

The test262 runner is built automatically with the main project:

```bash
cd build
cmake ..
make
```

## Downloading Test262 Suite

To download the official test262 suite:

```bash
../scripts/download_test262.sh
```

This will clone the test262 repository to the `test262` directory in the project root.

## Running Tests

### Basic Usage

```bash
# Run all language tests
./test262_runner ../test262

# Run specific test directory
./test262_runner ../test262 --test language/expressions

# Run with filter (regex)
./test262_runner ../test262 --filter "array.*push"

# Save results to file
./test262_runner ../test262 --output results.csv
```

### Command Line Options

- `--test <path>` - Run tests in specific directory (relative to test/)
- `--filter <regex>` - Filter tests by regex pattern
- `--output <file>` - Save results to CSV file
- `--help` - Show usage information

## Test262 Harness Functions

The framework implements key test262 harness functions:

### Assertion Functions
- `assert(condition, message)` - Basic assertion
- `assert.sameValue(actual, expected, message)` - Strict equality check
- `assert.notSameValue(actual, expected, message)` - Strict inequality check
- `assert.throws(errorConstructor, function)` - Verify exception is thrown
- `assert.compareArray(array1, array2)` - Deep array comparison

### Test262 Globals
- `$262` - Test262 specific object with:
  - `$262.createRealm()` - Create new realm
  - `$262.evalScript()` - Evaluate script
  - `$262.detachArrayBuffer()` - Detach ArrayBuffer
  - `$262.gc()` - Trigger garbage collection (no-op)
  - `$262.agent` - Agent API for parallel testing

### Other Helpers
- `$ERROR(message)` - Throw Test262Error
- `$DONE(error)` - Complete async test
- `Test262Error` - Test-specific error constructor
- `isConstructor(value)` - Check if value is constructor
- `fnGlobalObject()` - Get global object
- `verifyProperty(obj, prop, descriptor)` - Verify property descriptor
- `buildString(...)` - Build test strings

## Test Metadata

Test262 tests include YAML metadata in comments:

```javascript
/*---
description: Test description
features: [Array.prototype.push]
flags: [onlyStrict]
negative:
  phase: parse
  type: SyntaxError
includes: [propertyHelper.js]
---*/
```

The runner parses this metadata to:
- Skip unsupported features (async, modules)
- Handle negative tests (expected failures)
- Load required harness files
- Apply test flags (strict mode, etc.)

## Current Limitations

- Async/await tests are skipped (marked as unsupported)
- Module tests are skipped (ES6 modules not yet implemented)
- Some advanced features may not be fully implemented
- RegExp implementation is simplified

## Test Results

The runner reports:
- **PASS** - Test executed successfully
- **FAIL** - Test failed (with error details)
- **SKIP** - Test skipped (unsupported feature)

Results include execution time and can be exported to CSV for analysis.

## Development

To add support for new test262 features:

1. Implement missing language features in LightJS core
2. Add required harness functions to `test262_harness.cc`
3. Update metadata parsing if needed
4. Test with relevant test262 suite sections

## Example Output

```
LightJS Test262 Conformance Runner
==================================
Test262 path: ../test262
Running tests in: test/language/expressions

Running: test/language/expressions/addition/S11.6.1_A1.js ... PASS (0.002s)
Running: test/language/expressions/addition/S11.6.1_A2.js ... FAIL [runtime] Expected 7 but got 6
Running: test/language/expressions/addition/S11.6.1_A3.js ... SKIP (Async tests not yet supported)

============================================================
Test262 Conformance Results
============================================================
Total tests:      150
Passed tests:     120 (80.0%)
Failed tests:      20 (13.3%)
Skipped tests:     10 (6.7%)
```