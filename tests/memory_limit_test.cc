#include "../include/lightjs.h"
#include <iostream>
#include <cassert>

using namespace lightjs;

// Helper to run JavaScript and get result
std::pair<Value, bool> runScript(const char* script) {
  Lexer lexer(script);
  auto tokens = lexer.tokenize();

  Parser parser(tokens);
  auto program = parser.parse();

  if (!program) {
    return {Value(Undefined{}), false};
  }

  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  auto task = interpreter.evaluate(*program);
  LIGHTJS_RUN_TASK_VOID(task);

  return {interpreter.hasError() ? interpreter.getError() : task.result(),
          interpreter.hasError()};
}

void testMemoryLimitsConfiguration() {
  std::cout << "\n=== Memory Limits Configuration Test ===\n";

  auto& gc = GarbageCollector::instance();

  // Check system memory detection
  size_t sysMem = MemoryLimits::getSystemMemory();
  std::cout << "System memory: " << (sysMem / (1024 * 1024)) << " MB\n";
  assert(sysMem > 0 && "System memory detection should work");

  // Check default heap limit
  size_t defaultLimit = MemoryLimits::getDefaultHeapLimit();
  std::cout << "Default heap limit: " << (defaultLimit / (1024 * 1024)) << " MB\n";

  // Verify it's either 2GB or 4GB based on system memory
  bool is2GB = (defaultLimit == MemoryLimits::DEFAULT_HEAP_LIMIT);
  bool is4GB = (defaultLimit == MemoryLimits::EXTENDED_HEAP_LIMIT);
  assert((is2GB || is4GB) && "Default limit should be 2GB or 4GB");

  if (sysMem >= MemoryLimits::EXTENDED_LIMIT_THRESHOLD) {
    assert(is4GB && "Should be 4GB for systems with 16GB+ RAM");
    std::cout << "System has 16GB+ RAM, using extended limit\n";
  } else {
    assert(is2GB && "Should be 2GB for systems with <16GB RAM");
    std::cout << "System has <16GB RAM, using default limit\n";
  }

  // Test GC configuration
  size_t currentLimit = gc.getHeapLimit();
  std::cout << "Current GC heap limit: " << (currentLimit / (1024 * 1024)) << " MB\n";

  // Test setting custom limit
  gc.setHeapLimit(1024 * 1024 * 1024);  // 1GB
  assert(gc.getHeapLimit() == 1024ULL * 1024 * 1024);
  std::cout << "Set custom 1GB limit: OK\n";

  // Restore default
  gc.setHeapLimit(defaultLimit);

  std::cout << "Memory limits configuration test passed!\n";
}

void testMemoryTracking() {
  std::cout << "\n=== Memory Tracking Test ===\n";

  auto& gc = GarbageCollector::instance();
  gc.resetStats();

  // Get initial stats
  auto initialStats = gc.getStats();
  std::cout << "Initial allocated: " << initialStats.currentlyAllocated << " bytes\n";

  // Run a script that creates objects
  const char* script = R"(
    let arr = [];
    for (let i = 0; i < 100; i++) {
      arr.push({x: i, y: i * 2});
    }
    arr.length
  )";

  auto [result, hasError] = runScript(script);
  assert(!hasError && "Script should not error");
  assert(result.isNumber() && std::get<double>(result.data) == 100);

  // Check memory was tracked
  auto stats = gc.getStats();
  std::cout << "After allocations:\n";
  std::cout << "  Currently allocated: " << stats.currentlyAllocated << " bytes\n";
  std::cout << "  Total allocated: " << stats.totalAllocated << " bytes\n";
  std::cout << "  Peak allocated: " << stats.peakAllocated << " bytes\n";
  std::cout << "  Object count: " << stats.objectCount << "\n";

  assert(stats.totalAllocated > initialStats.totalAllocated &&
         "Total allocated should increase");

  std::cout << "Memory tracking test passed!\n";
}

void testHeapLimitEnforcement() {
  std::cout << "\n=== Heap Limit Enforcement Test ===\n";

  auto& gc = GarbageCollector::instance();

  // Set a small heap limit for testing (10MB)
  size_t originalLimit = gc.getHeapLimit();
  gc.setHeapLimit(10 * 1024 * 1024);  // 10MB

  std::cout << "Set heap limit to 10MB for testing\n";

  // Check heap limit works
  assert(gc.checkHeapLimit(1024) && "Small allocation should be OK");
  std::cout << "Small allocation check: OK\n";

  // Restore original limit
  gc.setHeapLimit(originalLimit);
  std::cout << "Restored original heap limit\n";

  std::cout << "Heap limit enforcement test passed!\n";
}

void testMemoryStatsOutput() {
  std::cout << "\n=== Memory Statistics Output Test ===\n";

  auto& gc = GarbageCollector::instance();
  auto stats = gc.getStats();

  std::cout << "GC Statistics:\n";
  std::cout << "  Total allocated: " << stats.totalAllocated << " bytes\n";
  std::cout << "  Total freed: " << stats.totalFreed << " bytes\n";
  std::cout << "  Currently allocated: " << stats.currentlyAllocated << " bytes\n";
  std::cout << "  Peak allocated: " << stats.peakAllocated << " bytes\n";
  std::cout << "  Object count: " << stats.objectCount << "\n";
  std::cout << "  Peak object count: " << stats.peakObjectCount << "\n";
  std::cout << "  Collections triggered: " << stats.collectionsTriggered << "\n";
  std::cout << "  Cycles detected: " << stats.cyclesDetected << "\n";
  std::cout << "  Heap limit exceeded: " << stats.heapLimitExceeded << "\n";
  std::cout << "  Total GC time: " << stats.totalGCTime.count() << " us\n";
  std::cout << "  Last GC time: " << stats.lastGCTime.count() << " us\n";

  std::cout << "\nMemory statistics output test passed!\n";
}

int main() {
  try {
    std::cout << "=== LightJS Memory Limit Tests ===" << std::endl;

    testMemoryLimitsConfiguration();
    testMemoryTracking();
    testHeapLimitEnforcement();
    testMemoryStatsOutput();

    std::cout << "\nAll memory limit tests passed!" << std::endl;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Unexpected exception: " << e.what() << std::endl;
    return 1;
  }
}
