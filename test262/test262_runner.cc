#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "test262_harness.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <regex>
#include <vector>
#include <map>
#include <iomanip>

namespace fs = std::filesystem;
using namespace lightjs;

struct Test262Result {
  std::string testPath;
  bool passed;
  std::string expectedError;
  std::string actualError;
  std::string phase;
  double executionTime;
};

struct Test262Metadata {
  std::string description;
  std::vector<std::string> features;
  std::vector<std::string> includes;
  std::vector<std::string> flags;
  bool negative = false;
  std::string negativePhase;
  std::string negativeType;
  bool isAsync = false;
  bool isRaw = false;
  bool isModule = false;
  bool onlyStrict = false;
  bool noStrict = false;
};

class Test262Runner {
private:
  std::string test262Path;
  std::string harnessPath;
  std::map<std::string, std::string> harnessCache;
  std::vector<Test262Result> results;
  int totalTests = 0;
  int passedTests = 0;
  int failedTests = 0;
  int skippedTests = 0;

  Test262Metadata parseMetadata(const std::string& source) {
    Test262Metadata metadata;

    // Note: std::regex doesn't have a dotall flag, using [\s\S] to match any character including newlines
    std::regex metadataRegex(R"(/\*---([\s\S]*?)---\*/)");
    std::smatch match;

    if (!std::regex_search(source, match, metadataRegex)) {
      return metadata;
    }

    std::string yamlContent = match[1];
    std::istringstream stream(yamlContent);
    std::string line;

    while (std::getline(stream, line)) {
      if (line.find("description:") == 0) {
        metadata.description = line.substr(12);
        while (metadata.description.front() == ' ' || metadata.description.front() == '"')
          metadata.description.erase(0, 1);
        while (metadata.description.back() == ' ' || metadata.description.back() == '"')
          metadata.description.pop_back();
      }
      else if (line.find("negative:") == 0) {
        metadata.negative = true;
        while (std::getline(stream, line)) {
          if (line.find("phase:") != std::string::npos) {
            metadata.negativePhase = line.substr(line.find("phase:") + 6);
            while (metadata.negativePhase.front() == ' ')
              metadata.negativePhase.erase(0, 1);
          }
          else if (line.find("type:") != std::string::npos) {
            metadata.negativeType = line.substr(line.find("type:") + 5);
            while (metadata.negativeType.front() == ' ')
              metadata.negativeType.erase(0, 1);
          }
          if (line[0] != ' ' && line[0] != '\t') break;
        }
      }
      else if (line.find("features:") == 0) {
        while (std::getline(stream, line) && (line[0] == ' ' || line[0] == '\t')) {
          if (line.find("- ") != std::string::npos) {
            std::string feature = line.substr(line.find("- ") + 2);
            metadata.features.push_back(feature);
          }
        }
      }
      else if (line.find("includes:") == 0) {
        while (std::getline(stream, line) && (line[0] == ' ' || line[0] == '\t')) {
          if (line.find("- ") != std::string::npos) {
            std::string include = line.substr(line.find("- ") + 2);
            metadata.includes.push_back(include);
          }
        }
      }
      else if (line.find("flags:") == 0) {
        while (std::getline(stream, line) && (line[0] == ' ' || line[0] == '\t')) {
          if (line.find("- ") != std::string::npos) {
            std::string flag = line.substr(line.find("- ") + 2);
            metadata.flags.push_back(flag);
            if (flag == "async") metadata.isAsync = true;
            if (flag == "raw") metadata.isRaw = true;
            if (flag == "module") metadata.isModule = true;
            if (flag == "onlyStrict") metadata.onlyStrict = true;
            if (flag == "noStrict") metadata.noStrict = true;
          }
        }
      }
    }

    return metadata;
  }

  std::string loadHarness(const std::string& filename) {
    if (harnessCache.find(filename) != harnessCache.end()) {
      return harnessCache[filename];
    }

    std::string path = harnessPath + "/" + filename;
    std::ifstream file(path);
    if (!file.is_open()) {
      std::cerr << "Warning: Could not load harness file: " << path << std::endl;
      return "";
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    harnessCache[filename] = content;
    return content;
  }

  std::string prepareTestCode(const std::string& testCode, const Test262Metadata& metadata) {
    std::stringstream prepared;

    // Add harness includes
    for (const auto& include : metadata.includes) {
      prepared << loadHarness(include) << "\n";
    }

    // Add strict mode if needed
    if (metadata.onlyStrict && !metadata.isRaw) {
      prepared << "\"use strict\";\n";
    }

    // Add the test code
    prepared << testCode;

    return prepared.str();
  }

  Test262Result runSingleTest(const std::string& testPath, const std::string& testCode) {
    Test262Result result;
    result.testPath = testPath;
    result.passed = false;

    auto startTime = std::chrono::high_resolution_clock::now();

    try {
      Test262Metadata metadata = parseMetadata(testCode);

      // Handle module tests
      if (metadata.isModule) {
        // Module tests require special handling with ModuleLoader
        // For now, we'll parse and evaluate as a module
        result.phase = "runtime";
      }

      std::string preparedCode = prepareTestCode(testCode, metadata);

      // Lexical analysis
      Lexer lexer(preparedCode);
      std::vector<Token> tokens;
      try {
        tokens = lexer.tokenize();
      } catch (const std::exception& e) {
        result.phase = "parse";
        result.actualError = e.what();
        if (metadata.negative && metadata.negativePhase == "parse") {
          result.passed = true;
        }
        auto endTime = std::chrono::high_resolution_clock::now();
        result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
        return result;
      }

      // Parsing
      Parser parser(tokens);
      auto program = parser.parse();

      if (!program) {
        result.phase = "parse";
        result.actualError = "Parse error";
        if (metadata.negative && metadata.negativePhase == "parse") {
          result.passed = true;
        }
        auto endTime = std::chrono::high_resolution_clock::now();
        result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
        return result;
      }

      // Execution
      auto env = lightjs::createTest262Environment();
      Interpreter interpreter(env);

      auto task = interpreter.evaluate(*program);

      try {
        while (!task.done()) {
          std::coroutine_handle<>::from_address(task.handle.address()).resume();
        }

        Value finalResult = task.result();
        result.phase = "runtime";

        // For async tests, check if the result is a Promise
        if (metadata.isAsync && finalResult.isPromise()) {
          auto promise = std::get<std::shared_ptr<Promise>>(finalResult.data);
          // Check Promise state
          if (promise->state == PromiseState::Rejected) {
            result.actualError = "Promise rejected";
            if (metadata.negative && metadata.negativePhase == "runtime") {
              result.passed = true;
            } else {
              result.passed = false;
            }
          } else if (promise->state == PromiseState::Fulfilled) {
            if (metadata.negative) {
              // Should have thrown but didn't
              result.passed = false;
              result.actualError = "Expected error but test completed successfully";
            } else {
              result.passed = true;
            }
          } else {
            // Pending promise
            result.passed = false;
            result.actualError = "Promise still pending";
          }
        } else {
          if (metadata.negative) {
            // Should have thrown but didn't
            result.passed = false;
            result.actualError = "Expected error but test completed successfully";
          } else {
            result.passed = true;
          }
        }
      } catch (const std::exception& e) {
        result.phase = "runtime";
        result.actualError = e.what();
        if (metadata.negative && metadata.negativePhase == "runtime") {
          result.passed = true;
        }
      }

    } catch (const std::exception& e) {
      result.actualError = e.what();
      result.phase = "unknown";
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    result.executionTime = std::chrono::duration<double>(endTime - startTime).count();

    return result;
  }


public:
  Test262Runner(const std::string& test262Path)
    : test262Path(test262Path), harnessPath(test262Path + "/harness") {}

  void runTestsInDirectory(const std::string& relativePath, const std::string& filter = "") {
    std::string fullPath = test262Path + "/" + relativePath;

    if (!fs::exists(fullPath)) {
      std::cerr << "Test directory does not exist: " << fullPath << std::endl;
      return;
    }

    std::regex filterRegex(filter.empty() ? ".*" : filter);

    for (const auto& entry : fs::recursive_directory_iterator(fullPath)) {
      if (entry.path().extension() == ".js") {
        std::string testPath = fs::relative(entry.path(), test262Path).string();

        if (!std::regex_search(testPath, filterRegex)) {
          continue;
        }

        std::ifstream file(entry.path());
        if (!file.is_open()) {
          std::cerr << "Could not open test file: " << testPath << std::endl;
          continue;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string testCode = buffer.str();

        std::cout << "Running: " << testPath << " ... " << std::flush;

        Test262Result result = runSingleTest(testPath, testCode);
        results.push_back(result);
        totalTests++;

        if (result.phase == "skip") {
          std::cout << "SKIP (" << result.actualError << ")" << std::endl;
          skippedTests++;
        } else if (result.passed) {
          std::cout << "PASS (" << std::fixed << std::setprecision(3)
                    << result.executionTime << "s)" << std::endl;
          passedTests++;
        } else {
          std::cout << "FAIL [" << result.phase << "] " << result.actualError << std::endl;
          failedTests++;
        }
      }
    }
  }

  void printSummary() {
    std::cout << "\n" << std::string(60, '=') << std::endl;
    std::cout << "Test262 Conformance Results" << std::endl;
    std::cout << std::string(60, '=') << std::endl;

    std::cout << "Total tests:   " << std::setw(6) << totalTests << std::endl;
    std::cout << "Passed tests:  " << std::setw(6) << passedTests
              << " (" << std::fixed << std::setprecision(1)
              << (totalTests > 0 ? 100.0 * passedTests / totalTests : 0.0) << "%)" << std::endl;
    std::cout << "Failed tests:  " << std::setw(6) << failedTests
              << " (" << std::fixed << std::setprecision(1)
              << (totalTests > 0 ? 100.0 * failedTests / totalTests : 0.0) << "%)" << std::endl;
    std::cout << "Skipped tests: " << std::setw(6) << skippedTests
              << " (" << std::fixed << std::setprecision(1)
              << (totalTests > 0 ? 100.0 * skippedTests / totalTests : 0.0) << "%)" << std::endl;

    if (failedTests > 0) {
      std::cout << "\nFailed tests:" << std::endl;
      for (const auto& result : results) {
        if (!result.passed && result.phase != "skip") {
          std::cout << "  - " << result.testPath << " [" << result.phase << "]" << std::endl;
        }
      }
    }
  }

  void saveResults(const std::string& outputFile) {
    std::ofstream out(outputFile);
    out << "Test262 Conformance Test Results\n";
    out << "=================================\n\n";
    out << "Total: " << totalTests << "\n";
    out << "Passed: " << passedTests << "\n";
    out << "Failed: " << failedTests << "\n";
    out << "Skipped: " << skippedTests << "\n\n";

    out << "Test,Result,Phase,Time(s),Error\n";
    for (const auto& result : results) {
      out << result.testPath << ",";
      out << (result.passed ? "PASS" : (result.phase == "skip" ? "SKIP" : "FAIL")) << ",";
      out << result.phase << ",";
      out << std::fixed << std::setprecision(4) << result.executionTime << ",";
      out << result.actualError << "\n";
    }

    std::cout << "Results saved to: " << outputFile << std::endl;
  }
};

void printUsage(const char* programName) {
  std::cout << "Usage: " << programName << " <test262-path> [options]\n";
  std::cout << "\nOptions:\n";
  std::cout << "  --test <path>     Run specific test or directory (relative to test/)\n";
  std::cout << "  --filter <regex>  Filter tests by regex pattern\n";
  std::cout << "  --output <file>   Save results to file\n";
  std::cout << "\nExamples:\n";
  std::cout << "  " << programName << " ./test262 --test language/expressions\n";
  std::cout << "  " << programName << " ./test262 --filter \"array.*push\"\n";
  std::cout << "  " << programName << " ./test262 --test language/types --output results.csv\n";
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(argv[0]);
    return 1;
  }

  std::string test262Path = argv[1];
  std::string testPath = "test/language";  // Default test path
  std::string filter = "";
  std::string outputFile = "";

  // Parse command line arguments
  for (int i = 2; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--test" && i + 1 < argc) {
      testPath = "test/" + std::string(argv[++i]);
    } else if (arg == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (arg == "--output" && i + 1 < argc) {
      outputFile = argv[++i];
    } else if (arg == "--help") {
      printUsage(argv[0]);
      return 0;
    }
  }

  if (!fs::exists(test262Path)) {
    std::cerr << "Error: test262 directory not found at: " << test262Path << std::endl;
    std::cerr << "Please download test262 from: https://github.com/tc39/test262" << std::endl;
    return 1;
  }

  std::cout << "TinyJS Test262 Conformance Runner\n";
  std::cout << "==================================\n";
  std::cout << "Test262 path: " << test262Path << std::endl;
  std::cout << "Running tests in: " << testPath << std::endl;
  if (!filter.empty()) {
    std::cout << "Filter: " << filter << std::endl;
  }
  std::cout << std::endl;

  Test262Runner runner(test262Path);
  runner.runTestsInDirectory(testPath, filter);
  runner.printSummary();

  if (!outputFile.empty()) {
    runner.saveResults(outputFile);
  }

  return 0;
}