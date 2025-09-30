#!/bin/bash

# Test262 download script for TinyJS
# Downloads and sets up the official ECMAScript test suite

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "$SCRIPT_DIR/.." && pwd )"
TEST262_DIR="$PROJECT_ROOT/test262"

echo "==================================="
echo "Test262 Download Script for TinyJS"
echo "==================================="
echo

# Check if test262 directory already exists
if [ -d "$TEST262_DIR" ]; then
    echo "Test262 directory already exists at: $TEST262_DIR"
    read -p "Do you want to update it? (y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Skipping download."
        exit 0
    fi
    echo "Updating test262..."
    cd "$TEST262_DIR"
    git pull
else
    echo "Downloading test262 to: $TEST262_DIR"
    echo

    # Clone the test262 repository
    git clone --depth 1 https://github.com/tc39/test262.git "$TEST262_DIR"
fi

echo
echo "Test262 has been successfully downloaded/updated!"
echo

# Create a README for the test262 directory
cat > "$TEST262_DIR/README_TINYJS.md" << 'EOF'
# Test262 for TinyJS

This directory contains the official ECMAScript test suite (test262).

## Running Tests

From the build directory:

```bash
# Run all language tests
./test262_runner ../test262

# Run specific test directory
./test262_runner ../test262 --test language/expressions

# Run with filter
./test262_runner ../test262 --filter "array.*push"

# Save results to file
./test262_runner ../test262 --output results.csv
```

## Test Categories

- `test/language/` - Core language features
- `test/built-ins/` - Built-in objects and functions
- `test/intl402/` - Internationalization (not supported yet)
- `test/annexB/` - Legacy features

## Current Support Status

TinyJS is a work in progress. Many test262 tests will fail or be skipped:
- Async/await tests are skipped
- Module tests are skipped
- Some advanced features may not be implemented yet

## Harness Files

The `harness/` directory contains helper functions used by test262 tests.
These are automatically loaded by the test runner when needed.
EOF

echo "Created README_TINYJS.md in test262 directory"
echo
echo "Next steps:"
echo "1. Build TinyJS with test262 support:"
echo "   cd $PROJECT_ROOT/build"
echo "   cmake .."
echo "   make"
echo
echo "2. Run test262 tests:"
echo "   ./test262_runner ../test262 --test language/types"
echo
echo "Done!"