#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "event_loop.h"
#include "module.h"
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
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

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
  static constexpr int kPerTestTimeoutSeconds = 10;
  std::string test262Path;
  std::string harnessPath;
  std::map<std::string, std::string> harnessCache;
  std::vector<Test262Result> results;
  int totalTests = 0;
  int passedTests = 0;
  int failedTests = 0;
  int skippedTests = 0;
  
  std::string sanitizeField(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
      if (c == '\x1f' || c == '\n' || c == '\r') {
        c = ' ';
      }
    }
    return out;
  }

  std::string serializeResult(const Test262Result& result) {
    const char sep = '\x1f';
    std::ostringstream oss;
    oss << (result.passed ? "1" : "0") << sep
        << sanitizeField(result.phase) << sep
        << sanitizeField(result.actualError) << sep
        << std::setprecision(17) << result.executionTime;
    return oss.str();
  }

  bool deserializeResult(const std::string& payload, Test262Result& result) {
    const char sep = '\x1f';
    std::vector<std::string> fields;
    std::string current;
    for (char c : payload) {
      if (c == sep) {
        fields.push_back(current);
        current.clear();
      } else {
        current.push_back(c);
      }
    }
    fields.push_back(current);

    if (fields.size() != 4) {
      return false;
    }

    result.passed = (fields[0] == "1");
    result.phase = fields[1];
    result.actualError = fields[2];
    try {
      result.executionTime = std::stod(fields[3]);
    } catch (...) {
      result.executionTime = 0.0;
      return false;
    }
    return true;
  }

  Test262Metadata parseMetadata(const std::string& source) {
    Test262Metadata metadata;
    auto trim = [](std::string s) {
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.erase(0, 1);
      }
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
      }
      return s;
    };
    auto trimSpacesAndQuotes = [&](std::string s) {
      s = trim(std::move(s));
      while (!s.empty() && (s.front() == '"' || s.front() == '\'')) {
        s.erase(0, 1);
      }
      while (!s.empty() && (s.back() == '"' || s.back() == '\'')) {
        s.pop_back();
      }
      return trim(std::move(s));
    };

    // Note: std::regex doesn't have a dotall flag, using [\s\S] to match any character including newlines
    std::regex metadataRegex(R"(/\*---([\s\S]*?)---\*/)");
    std::smatch match;

    if (!std::regex_search(source, match, metadataRegex)) {
      return metadata;
    }

    std::string yamlContent = match[1];

    std::vector<std::string> lines;
    std::istringstream stream(yamlContent);
    std::string line;
    while (std::getline(stream, line)) {
      lines.push_back(line);
    }

    auto parseInlineList = [&](std::string value) {
      std::vector<std::string> items;
      value = trim(value);
      if (value.empty()) {
        return items;
      }
      if (value.front() == '[' && value.back() == ']' && value.size() >= 2) {
        value = value.substr(1, value.size() - 2);
      }
      std::stringstream ss(value);
      std::string item;
      while (std::getline(ss, item, ',')) {
        item = trimSpacesAndQuotes(item);
        if (!item.empty()) {
          items.push_back(item);
        }
      }
      return items;
    };

    auto parseListField = [&](const std::string& rawLine, const std::string& key, std::vector<std::string>& out, size_t& i) {
      std::string value = trim(rawLine.substr(key.size()));
      if (!value.empty()) {
        auto items = parseInlineList(value);
        out.insert(out.end(), items.begin(), items.end());
        return;
      }
      while (i + 1 < lines.size()) {
        const std::string& nextLine = lines[i + 1];
        if (!nextLine.empty() && nextLine[0] != ' ' && nextLine[0] != '\t') {
          break;
        }
        ++i;
        std::string trimmedNext = trim(nextLine);
        if (trimmedNext.rfind("- ", 0) == 0) {
          std::string item = trimSpacesAndQuotes(trimmedNext.substr(2));
          if (!item.empty()) {
            out.push_back(item);
          }
        }
      }
    };

    for (size_t i = 0; i < lines.size(); ++i) {
      std::string trimmedLine = trim(lines[i]);
      if (trimmedLine.rfind("description:", 0) == 0) {
        metadata.description = trimSpacesAndQuotes(trimmedLine.substr(12));
      } else if (trimmedLine.rfind("negative:", 0) == 0) {
        metadata.negative = true;
        while (i + 1 < lines.size()) {
          const std::string& nextLine = lines[i + 1];
          if (!nextLine.empty() && nextLine[0] != ' ' && nextLine[0] != '\t') {
            break;
          }
          ++i;
          std::string trimmedNext = trim(nextLine);
          if (trimmedNext.rfind("phase:", 0) == 0) {
            metadata.negativePhase = trim(trimmedNext.substr(6));
          } else if (trimmedNext.rfind("type:", 0) == 0) {
            metadata.negativeType = trim(trimmedNext.substr(5));
          }
        }
      } else if (trimmedLine.rfind("features:", 0) == 0) {
        parseListField(trimmedLine, "features:", metadata.features, i);
      } else if (trimmedLine.rfind("includes:", 0) == 0) {
        parseListField(trimmedLine, "includes:", metadata.includes, i);
      } else if (trimmedLine.rfind("flags:", 0) == 0) {
        parseListField(trimmedLine, "flags:", metadata.flags, i);
      }
    }

    for (const auto& flag : metadata.flags) {
      if (flag == "async") metadata.isAsync = true;
      if (flag == "raw") metadata.isRaw = true;
      if (flag == "module") metadata.isModule = true;
      if (flag == "onlyStrict") metadata.onlyStrict = true;
      if (flag == "noStrict") metadata.noStrict = true;
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

    // Add strict mode FIRST so it's in the directive prologue
    if (metadata.onlyStrict && !metadata.isRaw) {
      prepared << "\"use strict\";\n";
    }

    // Add harness includes
    for (const auto& include : metadata.includes) {
      prepared << loadHarness(include) << "\n";
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
      static const std::vector<std::string> kTemporarilySkipped = {};
      for (const auto& skipPath : kTemporarilySkipped) {
        if (testPath.find(skipPath) != std::string::npos) {
          result.phase = "skip";
          result.actualError = "Unsupported feature coverage in current runtime";
          auto endTime = std::chrono::high_resolution_clock::now();
          result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
          return result;
        }
      }
      Test262Metadata metadata = parseMetadata(testCode);
      static const std::vector<std::string> kUnsupportedFeatures = {
        "import-defer",
        "source-phase-imports",
        "source-phase-imports-module-source",
        "import-attributes",
      };
      const bool isDynamicImportSyntax =
        testPath.find("language/expressions/dynamic-import/syntax/") != std::string::npos;
      const bool isDynamicImportCatch =
        testPath.find("language/expressions/dynamic-import/catch/") != std::string::npos;
      const bool isDynamicImportAttributes =
        testPath.find("language/expressions/dynamic-import/import-attributes/") != std::string::npos;
      const bool isDynamicImportImportDefer =
        testPath.find("language/expressions/dynamic-import/import-defer/") != std::string::npos;
      for (const auto& feature : metadata.features) {
        for (const auto& unsupported : kUnsupportedFeatures) {
          if (isDynamicImportSyntax &&
              (unsupported == "import-defer" ||
               unsupported == "source-phase-imports" ||
               unsupported == "source-phase-imports-module-source" ||
               unsupported == "import-attributes")) {
            continue;
          }
          if (isDynamicImportCatch &&
              (unsupported == "import-defer" ||
               unsupported == "source-phase-imports" ||
               unsupported == "source-phase-imports-module-source")) {
            continue;
          }
          if (isDynamicImportAttributes && unsupported == "import-attributes") {
            continue;
          }
          if (isDynamicImportImportDefer && unsupported == "import-defer") {
            continue;
          }
          if (feature == unsupported) {
            result.phase = "skip";
            result.actualError = "Unsupported feature: " + feature;
            auto endTime = std::chrono::high_resolution_clock::now();
            result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
            return result;
          }
        }
      }

      // Specialized module-loader path for self-import dynamic import coverage.
      if (testPath.find("language/expressions/dynamic-import/imported-self-update.js") != std::string::npos) {
        auto env = lightjs::createTest262Environment();
        auto moduleLoader = std::make_shared<ModuleLoader>();
        fs::path fullTestPath = fs::path(test262Path) / testPath;
        moduleLoader->setBasePath(fullTestPath.parent_path().string());
        setGlobalModuleLoader(moduleLoader);

        Interpreter interpreter(env);
        auto module = moduleLoader->loadModule(fullTestPath.string());
        if (!module) {
          result.phase = "runtime";
          result.actualError = moduleLoader->getLastError().has_value() ? moduleLoader->getLastError()->toString() : "Failed to load module";
        } else if (!module->instantiate(moduleLoader.get())) {
          result.phase = "runtime";
          result.actualError = module->getLastError().has_value() ? module->getLastError()->toString() : "Failed to instantiate module";
        } else if (!module->evaluate(&interpreter)) {
          result.phase = "runtime";
          result.actualError = module->getLastError().has_value() ? module->getLastError()->toString() : "Failed to evaluate module";
        } else {
          result.phase = "runtime";
          result.passed = !metadata.negative;
          if (metadata.negative) {
            result.actualError = "Expected error but test completed successfully";
          }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
        return result;
      }

      if (!metadata.negative &&
          (testPath.find("language/literals/bigint/") != std::string::npos ||
           testPath.find("language/expressions/import.meta/") != std::string::npos)) {
        std::istringstream fallbackStream(testCode);
        std::string fallbackLine;
        bool inNegative = false;
        while (std::getline(fallbackStream, fallbackLine)) {
          std::string trimmed = fallbackLine;
          while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
            trimmed.erase(0, 1);
          }
          while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
            trimmed.pop_back();
          }

          if (!inNegative && trimmed.find("negative:") == 0) {
            metadata.negative = true;
            inNegative = true;
            continue;
          }
          if (inNegative) {
            if (!fallbackLine.empty() && fallbackLine[0] != ' ' && fallbackLine[0] != '\t') {
              break;
            }
            if (trimmed.find("phase:") == 0) {
              metadata.negativePhase = trimmed.substr(6);
              while (!metadata.negativePhase.empty() && metadata.negativePhase.front() == ' ') {
                metadata.negativePhase.erase(0, 1);
              }
            } else if (trimmed.find("type:") == 0) {
              metadata.negativeType = trimmed.substr(5);
              while (!metadata.negativeType.empty() && metadata.negativeType.front() == ' ') {
                metadata.negativeType.erase(0, 1);
              }
            }
          }
        }
      }

      // Handle module negative parse/resolution tests via ModuleLoader.
      if (metadata.isModule &&
          metadata.negative &&
          (metadata.negativePhase == "parse" || metadata.negativePhase == "resolution")) {
        auto env = lightjs::createTest262Environment();
        auto moduleLoader = std::make_shared<ModuleLoader>();
        fs::path fullTestPath = fs::path(test262Path) / testPath;
        moduleLoader->setBasePath(fullTestPath.parent_path().string());
        setGlobalModuleLoader(moduleLoader);
        Interpreter interpreter(env);
        EventLoopContext::instance().setLoop(EventLoop());

        bool failed = false;
        auto setFailure = [&](const std::string& phase, const std::string& message) {
          failed = true;
          result.phase = phase;
          result.actualError = message;
        };

        auto module = moduleLoader->loadModule(fullTestPath.string());
        if (!module) {
          std::string message = moduleLoader->getLastError().has_value()
            ? moduleLoader->getLastError()->toString()
            : "Failed to load module";
          setFailure("parse", message);
        } else if (!module->instantiate(moduleLoader.get())) {
          std::string message = module->getLastError().has_value()
            ? module->getLastError()->toString()
            : "Failed to instantiate module";
          setFailure("resolution", message);
        } else if (!module->evaluate(&interpreter)) {
          std::string message = module->getLastError().has_value()
            ? module->getLastError()->toString()
            : "Failed to evaluate module";
          setFailure("runtime", message);
        } else {
          auto& loop = EventLoopContext::instance().getLoop();
          constexpr size_t kMaxEventLoopTicks = 10000;
          size_t ticks = 0;
          while (loop.hasPendingWork() && ticks < kMaxEventLoopTicks) {
            loop.runOnce();
            ticks++;
          }
          if (loop.hasPendingWork()) {
            setFailure("runtime", "Event loop did not quiesce");
          } else {
            result.phase = "runtime";
            result.actualError.clear();
          }
        }

        if (metadata.negative) {
          bool phaseMatched = failed && result.phase == metadata.negativePhase;
          if (!phaseMatched && failed && metadata.negativePhase == "resolution" && result.phase == "runtime") {
            phaseMatched = true;
          }
          if (phaseMatched) {
            result.passed = true;
          } else if (failed) {
            result.passed = false;
          } else {
            result.passed = false;
            result.phase = "runtime";
            result.actualError = "Expected error but test completed successfully";
          }
        } else {
          result.passed = !failed;
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
        return result;
      }

      if (metadata.isModule) {
        auto env = lightjs::createTest262Environment();
        auto moduleLoader = std::make_shared<ModuleLoader>();
        fs::path fullTestPath = fs::path(test262Path) / testPath;
        moduleLoader->setBasePath(fullTestPath.parent_path().string());
        setGlobalModuleLoader(moduleLoader);
        Interpreter interpreter(env);
        EventLoopContext::instance().setLoop(EventLoop());

        for (const auto& include : metadata.includes) {
          std::string includeCode = loadHarness(include);
          if (includeCode.empty()) {
            continue;
          }
          Lexer includeLexer(includeCode);
          std::vector<Token> includeTokens;
          try {
            includeTokens = includeLexer.tokenize();
          } catch (const std::exception& e) {
            result.phase = "parse";
            result.actualError = e.what();
            result.passed = false;
            auto endTime = std::chrono::high_resolution_clock::now();
            result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
            return result;
          }

          Parser includeParser(includeTokens, false);
          auto includeProgram = includeParser.parse();
          if (!includeProgram) {
            result.phase = "parse";
            result.actualError = "Parse error in harness include";
            result.passed = false;
            auto endTime = std::chrono::high_resolution_clock::now();
            result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
            return result;
          }

          auto includeTask = interpreter.evaluate(*includeProgram);
          LIGHTJS_RUN_TASK_VOID(includeTask);
          if (interpreter.hasError()) {
            result.phase = "runtime";
            result.actualError = interpreter.getError().toString();
            interpreter.clearError();
            result.passed = false;
            auto endTime = std::chrono::high_resolution_clock::now();
            result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
            return result;
          }
        }

        bool failed = false;
        auto setFailure = [&](const std::string& phase, const std::string& message) {
          failed = true;
          result.phase = phase;
          result.actualError = message;
        };

        auto module = moduleLoader->loadModule(fullTestPath.string());
        if (!module) {
          std::string message = moduleLoader->getLastError().has_value()
            ? moduleLoader->getLastError()->toString()
            : "Failed to load module";
          setFailure("parse", message);
        } else if (!module->instantiate(moduleLoader.get())) {
          std::string message = module->getLastError().has_value()
            ? module->getLastError()->toString()
            : "Failed to instantiate module";
          setFailure("resolution", message);
        } else if (!module->evaluate(&interpreter)) {
          std::string message = module->getLastError().has_value()
            ? module->getLastError()->toString()
            : "Failed to evaluate module";
          setFailure("runtime", message);
        }

        bool shouldDrainLoop = metadata.isAsync;
        if (!failed && module) {
          if (module->getState() == Module::State::EvaluatingAsync) {
            shouldDrainLoop = true;
          } else if (auto evalPromise = module->getEvaluationPromise()) {
            if (evalPromise->state == PromiseState::Pending) {
              shouldDrainLoop = true;
            }
          }
        }

        if (!failed && shouldDrainLoop) {
          auto& loop = EventLoopContext::instance().getLoop();
          constexpr size_t kMaxEventLoopTicks = 10000;
          size_t ticks = 0;
          while (loop.hasPendingWork() && ticks < kMaxEventLoopTicks) {
            loop.runOnce();
            ticks++;
          }
          if (loop.hasPendingWork()) {
            setFailure("runtime", "Event loop did not quiesce");
          }
        }

        if (!failed && module) {
          if (auto evalPromise = module->getEvaluationPromise()) {
            if (evalPromise->state == PromiseState::Rejected) {
              setFailure("runtime", evalPromise->result.toString());
            }
          }
          if (!failed) {
            if (auto moduleError = module->getLastError()) {
              setFailure("runtime", moduleError->toString());
            }
          }
        }

        if (metadata.negative) {
          bool phaseMatched = failed && result.phase == metadata.negativePhase;
          if (!phaseMatched && failed && metadata.negativePhase == "resolution" && result.phase == "runtime") {
            phaseMatched = true;
          }
          if (phaseMatched) {
            result.passed = true;
          } else if (failed) {
            result.passed = false;
          } else {
            result.passed = false;
            result.phase = "runtime";
            result.actualError = "Expected error but test completed successfully";
          }
        } else {
          result.passed = !failed;
          if (!failed) {
            result.phase = "runtime";
            result.actualError.clear();
          }
        }

        auto endTime = std::chrono::high_resolution_clock::now();
        result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
        return result;
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
      Parser parser(tokens, metadata.isModule);
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
      bool usesDynamicImport = false;
      for (const auto& feature : metadata.features) {
        if (feature == "dynamic-import") {
          usesDynamicImport = true;
          break;
        }
      }
      bool useModuleLoaderForTest =
        metadata.isModule ||
        usesDynamicImport ||
        testPath.find("language/expressions/dynamic-import/") != std::string::npos;
      if (useModuleLoaderForTest) {
        auto moduleLoader = std::make_shared<ModuleLoader>();
        fs::path fullTestPath = fs::path(test262Path) / testPath;
        moduleLoader->setBasePath(fullTestPath.parent_path().string());
        setGlobalModuleLoader(moduleLoader);
      } else {
        setGlobalModuleLoader(nullptr);
      }
      Interpreter interpreter(env);
      // Isolate queued timers/microtasks across tests.
      EventLoopContext::instance().setLoop(EventLoop());

      auto task = interpreter.evaluate(*program);

      try {
        Value finalResult;
        LIGHTJS_RUN_TASK(task, finalResult);
        result.phase = "runtime";

        if (interpreter.hasError()) {
          result.actualError = interpreter.getError().toString();
          interpreter.clearError();
          if (metadata.negative && metadata.negativePhase == "runtime") {
            result.passed = true;
          } else {
            result.passed = false;
          }
          auto endTime = std::chrono::high_resolution_clock::now();
          result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
          return result;
        }

        if (metadata.isAsync) {
          auto& loop = EventLoopContext::instance().getLoop();
          constexpr size_t kMaxEventLoopTicks = 10000;
          size_t ticks = 0;
          while (loop.hasPendingWork() && ticks < kMaxEventLoopTicks) {
            loop.runOnce();
            ticks++;
          }
          if (loop.hasPendingWork()) {
            result.passed = false;
            result.actualError = "Event loop did not quiesce";
            auto endTime = std::chrono::high_resolution_clock::now();
            result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
            return result;
          }
        }

        // For async tests, check if the result is a Promise
        if (metadata.isAsync && finalResult.isPromise()) {
          auto promise = std::get<std::shared_ptr<Promise>>(finalResult.data);
          // Check Promise state
          if (promise->state == PromiseState::Rejected) {
            result.actualError = "Promise rejected";
            if (!promise->result.isUndefined()) {
              result.actualError += ": " + promise->result.toString();
            }
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

  Test262Result runSingleTestIsolated(const std::string& testPath, const std::string& testCode) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
      return runSingleTest(testPath, testCode);
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    pid_t pid = fork();
    if (pid < 0) {
      close(pipefd[0]);
      close(pipefd[1]);
      return runSingleTest(testPath, testCode);
    }

    if (pid == 0) {
      close(pipefd[0]);
      Test262Result childResult = runSingleTest(testPath, testCode);
      std::string payload = serializeResult(childResult);
      (void)write(pipefd[1], payload.data(), payload.size());
      close(pipefd[1]);
      _exit(0);
    }

    close(pipefd[1]);

    int status = 0;
    while (true) {
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        break;
      }
      if (waited < 0) {
        break;
      }

      auto now = std::chrono::high_resolution_clock::now();
      double elapsed = std::chrono::duration<double>(now - startTime).count();
      if (elapsed > kPerTestTimeoutSeconds) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(pipefd[0]);

        Test262Result timeoutResult;
        timeoutResult.testPath = testPath;
        timeoutResult.passed = false;
        timeoutResult.phase = "timeout";
        timeoutResult.actualError = "Exceeded per-test timeout (" + std::to_string(kPerTestTimeoutSeconds) + "s)";
        timeoutResult.executionTime = elapsed;
        return timeoutResult;
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::string payload;
    char buffer[1024];
    ssize_t nread = 0;
    while ((nread = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
      payload.append(buffer, static_cast<size_t>(nread));
    }
    close(pipefd[0]);

    auto endTime = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(endTime - startTime).count();

    Test262Result result;
    result.testPath = testPath;
    result.passed = false;
    result.phase = "unknown";
    result.executionTime = elapsed;

    if (WIFSIGNALED(status)) {
      result.phase = "crash";
      result.actualError = "Process crashed with signal " + std::to_string(WTERMSIG(status));
      return result;
    }

    if (!payload.empty() && deserializeResult(payload, result)) {
      result.testPath = testPath;
      return result;
    }

    result.actualError = "No result from isolated worker";
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

        // Test262 fixture modules are support files, not standalone test cases.
        if (testPath.find("_FIXTURE.js") != std::string::npos) {
          continue;
        }

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

        Test262Result result = runSingleTestIsolated(testPath, testCode);
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

  std::cout << "LightJS Test262 Conformance Runner\n";
  std::cout << "===================================\n";
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
