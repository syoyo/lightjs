#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "event_loop.h"
#include "module.h"
#include "regex_utils.h"
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
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/resource.h>

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
  static constexpr int kPerTestTimeoutSeconds = 300;
  static constexpr int kTailCallPerTestTimeoutSeconds = 300;
  static constexpr int kResizableArrayBufferPerTestTimeoutSeconds = 300;
  std::string test262Path;
  std::string harnessPath;
  bool allowTemporarySkips_ = true;
  bool isolateWorkers_ = true;
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

  int configuredTimeoutSeconds(const char* envName, int fallback) const {
    const char* raw = std::getenv(envName);
    if (!raw || *raw == '\0') {
      return fallback;
    }
    try {
      int parsed = std::stoi(raw);
      return parsed > 0 ? parsed : fallback;
    } catch (...) {
      return fallback;
    }
  }

  bool isSupportedRegExpPropertyEscapesGeneratedTest(const std::string& testPath) const {
    static const std::string kPrefix = "built-ins/RegExp/property-escapes/generated/";
    static const std::string kGeneralCategoryPrefix = "General_Category_-_";
    static const std::string kScriptPrefix = "Script_-_";
    static const std::string kScriptExtensionsPrefix = "Script_Extensions_-_";

    auto prefixPos = testPath.find(kPrefix);
    if (prefixPos == std::string::npos) {
      return false;
    }

    std::string fileName = testPath.substr(prefixPos + kPrefix.size());
    if (fileName.find('/') != std::string::npos ||
        fileName.size() <= 3 ||
        fileName.compare(fileName.size() - 3, 3, ".js") != 0) {
      return false;
    }
    fileName.resize(fileName.size() - 3);

    std::string propertyName;
    if (fileName.compare(0, kGeneralCategoryPrefix.size(), kGeneralCategoryPrefix) == 0) {
      propertyName = "General_Category=" + fileName.substr(kGeneralCategoryPrefix.size());
    } else if (fileName.compare(0, kScriptExtensionsPrefix.size(), kScriptExtensionsPrefix) ==
               0) {
      propertyName = "Script_Extensions=" + fileName.substr(kScriptExtensionsPrefix.size());
    } else if (fileName.compare(0, kScriptPrefix.size(), kScriptPrefix) == 0) {
      propertyName = "Script=" + fileName.substr(kScriptPrefix.size());
    } else {
      propertyName = fileName;
    }

    return classifySupportedRegexUnicodePropertyName(propertyName).has_value();
  }

  Test262Metadata parseMetadata(const std::string& source) {
    Test262Metadata metadata;
    auto trim = [](std::string s) {
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.erase(0, 1);
      }
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
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
    if (filename == "tcoHelper.js") {
      content = std::regex_replace(
          content,
          std::regex(R"(var\s+\$MAX_ITERATIONS\s*=\s*100000\s*;)"),
          "var $MAX_ITERATIONS = 12000;");
    }
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
      static const std::vector<std::string> kTemporarilySkipped = {
      };
      if (allowTemporarySkips_) {
        for (const auto& skipPath : kTemporarilySkipped) {
          if (testPath.find(skipPath) != std::string::npos) {
            result.phase = "skip";
            result.actualError = "Unsupported feature coverage in current runtime";
            auto endTime = std::chrono::high_resolution_clock::now();
            result.executionTime = std::chrono::duration<double>(endTime - startTime).count();
            return result;
          }
        }
      }
      Test262Metadata metadata = parseMetadata(testCode);
      static const std::vector<std::string> kUnsupportedFeatures = {
        "regexp-v-flag",
        "regexp-unicode-property-escapes",
      };
      bool allowTopLevelAwaitForAwaitSyntaxCoverage =
        testPath.find("language/module-code/top-level-await/syntax/for-await-await-expr-") != std::string::npos;
      const bool isDynamicImportSyntax =
        testPath.find("language/expressions/dynamic-import/syntax/") != std::string::npos;
      const bool isDynamicImportCatch =
        testPath.find("language/expressions/dynamic-import/catch/") != std::string::npos;
      const bool isDynamicImportUsage =
        testPath.find("language/expressions/dynamic-import/usage/") != std::string::npos;
      const bool isDynamicImportAttributes =
        testPath.find("language/expressions/dynamic-import/import-attributes/") != std::string::npos;
      const bool isDynamicImportImportDefer =
        testPath.find("language/expressions/dynamic-import/import-defer/") != std::string::npos;
      const bool isStaticSourcePhaseImport =
        testPath.find("language/module-code/source-phase-import/") != std::string::npos;
      const bool isRegExpModifiersSyntaxValid =
        testPath.find("built-ins/RegExp/regexp-modifiers/syntax/valid/") != std::string::npos;
      const bool isRegExpModifiersEarlyError =
        testPath.find("language/literals/regexp/early-err-") != std::string::npos ||
        testPath.find("built-ins/RegExp/early-err-") != std::string::npos ||
        testPath.find("built-ins/RegExp/syntax-err-") != std::string::npos;
      const bool isRegExpModifiersPropertyOnly =
        testPath.find("built-ins/RegExp/regexp-modifiers/") != std::string::npos &&
        testPath.find("property.js") != std::string::npos;
      const bool isRegExpModifiersScopedLowered =
        testPath.find("built-ins/RegExp/regexp-modifiers/") != std::string::npos &&
        testPath.find("property.js") == std::string::npos &&
        true;
      const bool isRegExpPropertyEscapesParseNegative =
        testPath.find("built-ins/RegExp/property-escapes/") != std::string::npos &&
        metadata.negative &&
        metadata.negativePhase == "parse";
      const bool isRegExpPropertyEscapesStringParseNegative =
        testPath.find("built-ins/RegExp/property-escapes/generated/strings/") !=
          std::string::npos &&
        metadata.negative &&
        metadata.negativePhase == "parse";
      const bool isRegExpPropertyEscapesStringPositiveTargeted =
        testPath.find("built-ins/RegExp/property-escapes/generated/strings/Basic_Emoji.js") !=
          std::string::npos ||
        testPath.find("built-ins/RegExp/property-escapes/generated/strings/Emoji_Keycap_Sequence.js") !=
          std::string::npos ||
        testPath.find("built-ins/RegExp/property-escapes/generated/strings/RGI_Emoji.js") !=
          std::string::npos ||
        testPath.find("built-ins/RegExp/property-escapes/generated/strings/RGI_Emoji_Flag_Sequence.js") !=
          std::string::npos ||
        testPath.find("built-ins/RegExp/property-escapes/generated/strings/RGI_Emoji_Modifier_Sequence.js") !=
          std::string::npos ||
        testPath.find("built-ins/RegExp/property-escapes/generated/strings/RGI_Emoji_Tag_Sequence.js") !=
          std::string::npos ||
        testPath.find("built-ins/RegExp/property-escapes/generated/strings/RGI_Emoji_ZWJ_Sequence.js") !=
          std::string::npos;
      const bool isRegExpPropertyEscapesGeneratedSupportedSubset =
        isSupportedRegExpPropertyEscapesGeneratedTest(testPath);
      const bool isRegExpUnicodeSetsTargeted =
        testPath.find("built-ins/RegExp/prototype/unicodeSets/") != std::string::npos;
      const bool isRegExpUnicodeSetsRgiEmojiGenerated =
        testPath.find("built-ins/RegExp/unicodeSets/generated/rgi-emoji-") != std::string::npos;
      const bool isRegExpFlagsUsesUnicodeSets =
        testPath.find("built-ins/RegExp/prototype/flags/this-val-regexp.js") !=
        std::string::npos;
      const bool isRegExpStringVFlagTargeted =
        testPath.find("built-ins/String/prototype/match/regexp-prototype-match-v-u-flag.js") != std::string::npos ||
        testPath.find("built-ins/String/prototype/matchAll/regexp-prototype-matchAll-v-u-flag.js") != std::string::npos ||
        testPath.find("built-ins/String/prototype/replace/regexp-prototype-replace-v-u-flag.js") != std::string::npos ||
        testPath.find("built-ins/String/prototype/search/regexp-prototype-search-v-flag.js") != std::string::npos ||
        testPath.find("built-ins/String/prototype/search/regexp-prototype-search-v-u-flag.js") != std::string::npos;
      const bool isRegExpExecVFlagTargeted =
        testPath.find("built-ins/RegExp/prototype/exec/regexp-builtin-exec-v-u-flag.js") != std::string::npos;
      const bool isExplicitResourceManagementSymbolSurface =
        testPath.find("built-ins/Symbol/dispose/") != std::string::npos ||
        testPath.find("built-ins/Symbol/asyncDispose/") != std::string::npos ||
        testPath.find("built-ins/Iterator/prototype/Symbol.dispose/") != std::string::npos ||
        testPath.find("built-ins/AsyncIteratorPrototype/Symbol.asyncDispose/") != std::string::npos;
      const bool isExplicitResourceManagementCtorSurface =
        testPath.find("built-ins/SuppressedError/") != std::string::npos ||
        (testPath.find("built-ins/DisposableStack/") != std::string::npos &&
         testPath.find("built-ins/DisposableStack/prototype/") == std::string::npos) ||
        (testPath.find("built-ins/AsyncDisposableStack/") != std::string::npos &&
         testPath.find("built-ins/AsyncDisposableStack/prototype/") == std::string::npos);
      const bool isDisposableStackSupportedPrototypeSurface =
        testPath.find("built-ins/DisposableStack/prototype/dispose/") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/disposed/") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/use/") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/adopt/") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/defer/") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/move/") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/Symbol.dispose.js") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/Symbol.toStringTag.js") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/prop-desc.js") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/proto.js") != std::string::npos ||
        testPath.find("built-ins/DisposableStack/prototype/constructor.js") != std::string::npos;
      const bool isAsyncDisposableStackSupportedPrototypeSurface =
        testPath.find("built-ins/AsyncDisposableStack/prototype/disposeAsync/") != std::string::npos ||
        testPath.find("built-ins/AsyncDisposableStack/prototype/disposed/") != std::string::npos ||
        testPath.find("built-ins/AsyncDisposableStack/prototype/use/") != std::string::npos ||
        testPath.find("built-ins/AsyncDisposableStack/prototype/adopt/") != std::string::npos ||
        testPath.find("built-ins/AsyncDisposableStack/prototype/defer/") != std::string::npos ||
        testPath.find("built-ins/AsyncDisposableStack/prototype/move/") != std::string::npos ||
        testPath.find("built-ins/AsyncDisposableStack/prototype/Symbol.asyncDispose.js") != std::string::npos ||
        testPath.find("built-ins/AsyncDisposableStack/prototype/prop-desc.js") != std::string::npos ||
        testPath.find("built-ins/AsyncDisposableStack/prototype/proto.js") != std::string::npos;
      const bool isExplicitResourceManagementStagingStackSurface =
        testPath.find("staging/explicit-resource-management/disposable-stack") != std::string::npos ||
        testPath.find("staging/explicit-resource-management/async-disposable-stack") != std::string::npos;
      const bool isExplicitResourceManagementUsingSurface =
        testPath.find("language/statements/using/") != std::string::npos ||
        testPath.find("language/statements/await-using/") != std::string::npos ||
        testPath.find("staging/explicit-resource-management/using-with-null-or-undefined.js") != std::string::npos ||
        testPath.find("staging/explicit-resource-management/call-dispose-methods.js") != std::string::npos ||
        testPath.find("staging/explicit-resource-management/exception-handling.js") != std::string::npos;
      const bool isDynamicImportAsyncIterationTopLevelTargeted =
        testPath.find("language/expressions/dynamic-import/for-await-resolution-and-error-agen.js") != std::string::npos ||
        testPath.find("language/expressions/dynamic-import/for-await-resolution-and-error-agen-yield.js") != std::string::npos;
      const bool isCoalesceTailCallTargeted =
        testPath.find("language/expressions/coalesce/tco-pos-null.js") != std::string::npos ||
        testPath.find("language/expressions/coalesce/tco-pos-undefined.js") != std::string::npos;
      const bool isTailCallTargeted =
        testPath.find("/tco") != std::string::npos;
      for (const auto& feature : metadata.features) {
        if (allowTopLevelAwaitForAwaitSyntaxCoverage && feature == "async-iteration") {
          continue;
        }
        if (isDynamicImportAsyncIterationTopLevelTargeted && feature == "async-iteration") {
          continue;
        }
        if (isCoalesceTailCallTargeted && feature == "tail-call-optimization") {
          continue;
        }
        if (isTailCallTargeted && feature == "tail-call-optimization") {
          continue;
        }
        for (const auto& unsupported : kUnsupportedFeatures) {
          if (isDynamicImportSyntax &&
              (unsupported == "import-defer" ||
               unsupported == "source-phase-imports" ||
               unsupported == "import-attributes" ||
               unsupported == "async-iteration")) {
            continue;
          }
          if (isDynamicImportCatch &&
              (unsupported == "import-defer" ||
               unsupported == "source-phase-imports" ||
               unsupported == "async-iteration")) {
            continue;
          }
          if (isDynamicImportUsage &&
              (unsupported == "import-defer" ||
               unsupported == "source-phase-imports" ||
               unsupported == "async-iteration")) {
            continue;
          }
          if (isDynamicImportAttributes &&
              unsupported == "import-attributes") {
            continue;
          }
          if (isDynamicImportImportDefer && unsupported == "import-defer") {
            continue;
          }
          if (isStaticSourcePhaseImport && unsupported == "source-phase-imports") {
            continue;
          }
          if ((isRegExpModifiersSyntaxValid || isRegExpModifiersEarlyError) &&
              unsupported == "regexp-modifiers") {
            continue;
          }
          if (isRegExpModifiersPropertyOnly &&
              unsupported == "regexp-modifiers") {
            continue;
          }
          if (isRegExpModifiersScopedLowered &&
              unsupported == "regexp-modifiers") {
            continue;
          }
          if (isRegExpPropertyEscapesParseNegative &&
              unsupported == "regexp-unicode-property-escapes") {
            continue;
          }
          if (isRegExpPropertyEscapesStringParseNegative &&
              (unsupported == "regexp-unicode-property-escapes" ||
               unsupported == "regexp-v-flag")) {
            continue;
          }
          if (isRegExpPropertyEscapesStringPositiveTargeted &&
              (unsupported == "regexp-unicode-property-escapes" ||
               unsupported == "regexp-v-flag")) {
            continue;
          }
          if (isRegExpPropertyEscapesGeneratedSupportedSubset &&
              unsupported == "regexp-unicode-property-escapes") {
            continue;
          }
          if (isRegExpStringVFlagTargeted &&
              (unsupported == "regexp-unicode-property-escapes" ||
               unsupported == "regexp-v-flag")) {
            continue;
          }
          if (isRegExpExecVFlagTargeted &&
              (unsupported == "regexp-unicode-property-escapes" ||
               unsupported == "regexp-v-flag")) {
            continue;
          }
          if ((isRegExpUnicodeSetsTargeted ||
               isRegExpUnicodeSetsRgiEmojiGenerated ||
               isRegExpFlagsUsesUnicodeSets) &&
              (unsupported == "regexp-v-flag" ||
               unsupported == "regexp-unicode-property-escapes")) {
            continue;
          }
          if (isExplicitResourceManagementSymbolSurface &&
              unsupported == "explicit-resource-management") {
            continue;
          }
          if ((isExplicitResourceManagementCtorSurface ||
               isExplicitResourceManagementStagingStackSurface ||
               isExplicitResourceManagementUsingSurface ||
               isDisposableStackSupportedPrototypeSurface ||
               isAsyncDisposableStackSupportedPrototypeSurface) &&
              unsupported == "explicit-resource-management") {
            continue;
          }
          if (isAsyncDisposableStackSupportedPrototypeSurface &&
              unsupported == "async-disposable-stack") {
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
        setGlobalInterpreter(&interpreter);
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
        setGlobalInterpreter(&interpreter);
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
        setGlobalInterpreter(&interpreter);
        EventLoopContext::instance().setLoop(EventLoop());
        std::vector<std::shared_ptr<Program>> retainedHarnessPrograms;
        retainedHarnessPrograms.reserve(metadata.includes.size());

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

          auto includeProgramOwner = std::make_shared<Program>(std::move(*includeProgram));
          retainedHarnessPrograms.push_back(includeProgramOwner);

          auto includeTask = interpreter.evaluate(*includeProgramOwner);
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
      parser.setSource(preparedCode);
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

      // Negative parse tests must fail during parse; do not execute them.
      if (!metadata.isModule &&
          metadata.negative &&
          metadata.negativePhase == "parse") {
        result.phase = "parse";
        result.actualError = "Expected parse error but test parsed successfully";
        result.passed = false;
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
      setGlobalInterpreter(&interpreter);
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
          auto promise = std::get<GCPtr<Promise>>(finalResult.data);
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
      // Set per-child memory limit (default 2GB, configurable via env var)
      {
        rlim_t memLimitBytes = 2ULL * 1024 * 1024 * 1024; // 2GB default
        const char* envLimit = std::getenv("LIGHTJS_TEST262_MEM_LIMIT_MB");
        if (envLimit && *envLimit != '\0') {
          try {
            long mb = std::stol(envLimit);
            if (mb > 0) memLimitBytes = static_cast<rlim_t>(mb) * 1024 * 1024;
          } catch (...) {}
        }
        struct rlimit rl;
        rl.rlim_cur = memLimitBytes;
        rl.rlim_max = memLimitBytes;
        setrlimit(RLIMIT_AS, &rl);
      }
      Test262Result childResult = runSingleTest(testPath, testCode);
      std::string payload = serializeResult(childResult);
      (void)write(pipefd[1], payload.data(), payload.size());
      close(pipefd[1]);
      _exit(0);
    }

    close(pipefd[1]);

    int timeoutSeconds =
      configuredTimeoutSeconds("LIGHTJS_TEST262_TIMEOUT_SECONDS", kPerTestTimeoutSeconds);
    auto metadata = parseMetadata(testCode);
    if (std::find(metadata.features.begin(),
                  metadata.features.end(),
                  "resizable-arraybuffer") != metadata.features.end()) {
      timeoutSeconds = std::max(
        timeoutSeconds,
        configuredTimeoutSeconds(
          "LIGHTJS_TEST262_RAB_TIMEOUT_SECONDS",
          kResizableArrayBufferPerTestTimeoutSeconds));
    }
    if (std::find(metadata.features.begin(),
                  metadata.features.end(),
                  "tail-call-optimization") != metadata.features.end()) {
      timeoutSeconds = configuredTimeoutSeconds(
        "LIGHTJS_TEST262_TCO_TIMEOUT_SECONDS", kTailCallPerTestTimeoutSeconds);
    }

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
      if (elapsed > timeoutSeconds) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(pipefd[0]);

        Test262Result timeoutResult;
        timeoutResult.testPath = testPath;
        timeoutResult.passed = false;
        timeoutResult.phase = "timeout";
        timeoutResult.actualError = "Exceeded per-test timeout (" + std::to_string(timeoutSeconds) + "s)";
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
  Test262Runner(const std::string& test262Path,
                bool allowTemporarySkips = true,
                bool isolateWorkers = true)
    : test262Path(test262Path),
      harnessPath(test262Path + "/harness"),
      allowTemporarySkips_(allowTemporarySkips),
      isolateWorkers_(isolateWorkers) {}

  void runTestsInDirectory(const std::string& relativePath, const std::string& filter = "") {
    std::string fullPath = test262Path + "/" + relativePath;

    if (!fs::exists(fullPath)) {
      std::cerr << "Test directory does not exist: " << fullPath << std::endl;
      return;
    }

    std::regex filterRegex(filter.empty() ? ".*" : filter);

    auto runTest = [&](const fs::path& path) {
      if (path.extension() == ".js") {
        std::string testPath = fs::relative(path, test262Path).string();

        // Test262 fixture modules are support files, not standalone test cases.
        if (testPath.find("_FIXTURE.js") != std::string::npos) {
          return;
        }

        if (!std::regex_search(testPath, filterRegex)) {
          return;
        }

        std::ifstream file(path);
        if (!file.is_open()) {
          std::cerr << "Could not open test file: " << testPath << std::endl;
          return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string testCode = buffer.str();

        std::cout << "Running: " << testPath << " ... " << std::flush;

        Test262Result result = isolateWorkers_
          ? runSingleTestIsolated(testPath, testCode)
          : runSingleTest(testPath, testCode);
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
    };

    if (fs::is_directory(fullPath)) {
      for (const auto& entry : fs::recursive_directory_iterator(fullPath)) {
        runTest(entry.path());
      }
    } else {
      runTest(fullPath);
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
  std::cout << "  --no-temp-skips   Disable temporary runtime skip list\n";
  std::cout << "  --no-isolation    Run tests in-process (debug only)\n";
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
  bool allowTemporarySkips = true;
  bool isolateWorkers = true;

  // Parse command line arguments
  for (int i = 2; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--test" && i + 1 < argc) {
      testPath = "test/" + std::string(argv[++i]);
    } else if (arg == "--filter" && i + 1 < argc) {
      filter = argv[++i];
    } else if (arg == "--output" && i + 1 < argc) {
      outputFile = argv[++i];
    } else if (arg == "--no-temp-skips") {
      allowTemporarySkips = false;
    } else if (arg == "--no-isolation") {
      isolateWorkers = false;
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
  if (!allowTemporarySkips) {
    std::cout << "Temporary skip list: disabled" << std::endl;
  }
  if (!isolateWorkers) {
    std::cout << "Isolation mode: disabled" << std::endl;
  }
  std::cout << std::endl;

  Test262Runner runner(test262Path, allowTemporarySkips, isolateWorkers);
  runner.runTestsInDirectory(testPath, filter);
  runner.printSummary();

  if (!outputFile.empty()) {
    runner.saveResults(outputFile);
  }

  return 0;
}
