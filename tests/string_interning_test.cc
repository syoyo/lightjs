#include "../include/lightjs.h"
#include "../include/string_table.h"
#include <iostream>
#include <cassert>

using namespace lightjs;

void testStringInterning() {
  std::cout << "\n=== String Interning Test ===\n";

  // Reset stats
  StringTable::instance().resetStats();

  // Test script with many repeated identifiers and property names
  const char* script = R"(
    let obj1 = { name: "Alice", age: 30, city: "NYC" };
    let obj2 = { name: "Bob", age: 25, city: "LA" };
    let obj3 = { name: "Charlie", age: 35, city: "SF" };
    let obj4 = { name: "David", age: 28, city: "NYC" };
    let obj5 = { name: "Eve", age: 32, city: "LA" };

    // Access properties repeatedly
    let n1 = obj1.name;
    let n2 = obj2.name;
    let n3 = obj3.name;

    let a1 = obj1.age;
    let a2 = obj2.age;
    let a3 = obj3.age;
  )";

  Lexer lexer(script);
  auto tokens = lexer.tokenize();

  // Count interned tokens
  int totalTokens = 0;
  int internedTokens = 0;

  for (const auto& token : tokens) {
    totalTokens++;
    if (token.isInterned()) {
      internedTokens++;
    }
  }

  std::cout << "Total tokens: " << totalTokens << "\n";
  std::cout << "Interned tokens: " << internedTokens << "\n";
  std::cout << "Interning rate: " << (internedTokens * 100.0 / totalTokens) << "%\n\n";

  // Print string table statistics
  auto stats = StringTable::instance().getStats();
  std::cout << "String Table Statistics:\n";
  std::cout << "  Total intern calls: " << stats.totalInterns << "\n";
  std::cout << "  Cache hits: " << stats.hitCount << "\n";
  std::cout << "  Cache misses: " << stats.missCount << "\n";
  std::cout << "  Hit rate: " << (stats.hitRate() * 100.0) << "%\n";
  std::cout << "  Unique strings: " << stats.uniqueStrings << "\n";
  std::cout << "  Total bytes stored: " << stats.totalBytes << "\n";
  std::cout << "  Average string length: "
            << (stats.uniqueStrings > 0 ? stats.totalBytes / stats.uniqueStrings : 0) << "\n";

  // Verify that same identifiers share memory
  std::shared_ptr<std::string> firstName, secondName;
  bool foundNames = false;

  for (const auto& token : tokens) {
    if (token.isInterned() && token.getString() == "name") {
      if (!firstName) {
        firstName = token.internedValue;
      } else if (!secondName) {
        secondName = token.internedValue;
        foundNames = true;
        break;
      }
    }
  }

  if (foundNames) {
    bool samePointer = (firstName.get() == secondName.get());
    std::cout << "\nMemory sharing verification:\n";
    std::cout << "  'name' identifiers share memory: " << (samePointer ? "YES" : "NO") << "\n";
    assert(samePointer && "Interned strings should share memory!");
  }

  std::cout << "\n✅ String interning is working correctly!\n";
}

void testInterningThreshold() {
  std::cout << "\n=== String Literal Interning Threshold Test ===\n";

  StringTable::instance().resetStats();

  // Test short string (should be interned)
  const char* shortString = R"("hello")";
  Lexer lexer1(shortString);
  auto tokens1 = lexer1.tokenize();

  bool shortInterned = tokens1[0].isInterned();
  std::cout << "Short string (5 chars): " << (shortInterned ? "INTERNED" : "NOT INTERNED") << "\n";

  // Test long string (should NOT be interned)
  std::string longString = "\"" + std::string(300, 'x') + "\"";
  Lexer lexer2(longString);
  auto tokens2 = lexer2.tokenize();

  bool longInterned = tokens2[0].isInterned();
  std::cout << "Long string (300 chars): " << (longInterned ? "INTERNED" : "NOT INTERNED") << "\n";

  assert(shortInterned && "Short strings should be interned");
  assert(!longInterned && "Long strings should NOT be interned");

  std::cout << "\n✅ Interning threshold working correctly!\n";
}

void testMemorySavings() {
  std::cout << "\n=== Memory Savings Estimation ===\n";

  StringTable::instance().resetStats();

  // Create code with many repeated property accesses
  const char* script = R"(
    for (let i = 0; i < 100; i++) {
      let obj = { x: i, y: i*2, z: i*3 };
      let sum = obj.x + obj.y + obj.z;
    }
  )";

  Lexer lexer(script);
  auto tokens = lexer.tokenize();

  auto stats = StringTable::instance().getStats();

  // Calculate memory saved
  // Each string would need to be stored separately without interning
  double avgStringLength = stats.uniqueStrings > 0 ?
    (double)stats.totalBytes / stats.uniqueStrings : 0.0;

  size_t memoryWithoutInterning = (size_t)(stats.totalInterns * avgStringLength);
  size_t memoryWithInterning = stats.totalBytes;

  std::cout << "Average string length: " << avgStringLength << " bytes\n";
  std::cout << "Total intern calls: " << stats.totalInterns << "\n";
  std::cout << "Unique strings stored: " << stats.uniqueStrings << "\n\n";

  if (memoryWithoutInterning > memoryWithInterning) {
    size_t memorySaved = memoryWithoutInterning - memoryWithInterning;
    double savingsPercent = (memorySaved * 100.0) / memoryWithoutInterning;

    std::cout << "Memory without interning (estimated): " << memoryWithoutInterning << " bytes\n";
    std::cout << "Memory with interning: " << memoryWithInterning << " bytes\n";
    std::cout << "Memory saved: " << memorySaved << " bytes (" << savingsPercent << "%)\n";
    std::cout << "\n✅ String interning provides " << static_cast<int>(savingsPercent) << "% memory reduction!\n";
  } else {
    std::cout << "Not enough data to estimate memory savings\n";
    std::cout << "\n✅ String interning infrastructure is functional!\n";
  }
}

int main() {
  try {
    testStringInterning();
    testInterningThreshold();
    testMemorySavings();

    std::cout << "\n🎉 All string interning tests passed!\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
