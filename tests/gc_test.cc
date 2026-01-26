#include <iostream>
#include <string>
#include <vector>
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "gc.h"

using namespace lightjs;

int main() {
    std::cout << "TinyJS Garbage Collection Test\n";
    std::cout << "==============================\n\n";

    // Create a script that allocates memory
    std::string code = R"(
        // Create objects that will be garbage collected
        function createData() {
            let obj = {
                data: "Some data",
                nested: {
                    array: [1, 2, 3, 4, 5],
                    more: "More data"
                }
            };
            return obj;
        }

        // Create multiple objects
        let results = [];
        for (let i = 0; i < 10; i = i + 1) {
            results[i] = createData();
        }

        // Create some circular references
        let a = { name: "A" };
        let b = { name: "B" };
        a.ref = b;
        b.ref = a;

        // Create orphaned objects (should be collected)
        for (let i = 0; i < 100; i = i + 1) {
            let temp = {
                id: i,
                data: "Temporary data that will be garbage collected"
            };
        }

        console.log("Created test objects");
    )";

    // Get GC instance
    auto& gc = GarbageCollector::instance();

    // Reset stats
    gc.resetStats();

    // Configure GC
    gc.setThreshold(1024); // Trigger GC after 1KB of allocations
    gc.setAutoCollect(true);

    std::cout << "Initial GC stats:\n";
    auto stats = gc.getStats();
    std::cout << "  Objects allocated: " << stats.currentlyAllocated << "\n";
    std::cout << "  Peak allocated: " << stats.peakAllocated << "\n";
    std::cout << "  Collections triggered: " << stats.collectionsTriggered << "\n\n";

    // Parse and execute the code
    Lexer lexer(code);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    auto program = parser.parse();

    if (!program) {
        std::cerr << "Parse error!\n";
        return 1;
    }

    auto env = Environment::createGlobal();
    Interpreter interpreter(env);

    std::cout << "Executing script...\n\n";

    auto task = interpreter.evaluate(*program);
    while (!task.done()) {
        std::coroutine_handle<>::from_address(task.handle.address()).resume();
    }

    std::cout << "\nGC stats after execution:\n";
    stats = gc.getStats();
    std::cout << "  Objects allocated: " << stats.currentlyAllocated << "\n";
    std::cout << "  Peak allocated: " << stats.peakAllocated << "\n";
    std::cout << "  Total allocated: " << stats.totalAllocated << "\n";
    std::cout << "  Total freed: " << stats.totalFreed << "\n";
    std::cout << "  Collections triggered: " << stats.collectionsTriggered << "\n";
    std::cout << "  Cycles detected: " << stats.cyclesDetected << "\n";
    std::cout << "  Last GC time: " << stats.lastGCTime.count() << " microseconds\n";
    std::cout << "  Total GC time: " << stats.totalGCTime.count() << " microseconds\n";

    // Manually trigger a collection
    std::cout << "\nManually triggering garbage collection...\n";
    gc.collect();

    std::cout << "\nGC stats after manual collection:\n";
    stats = gc.getStats();
    std::cout << "  Objects allocated: " << stats.currentlyAllocated << "\n";
    std::cout << "  Total freed: " << stats.totalFreed << "\n";
    std::cout << "  Collections triggered: " << stats.collectionsTriggered << "\n";
    std::cout << "  Cycles detected: " << stats.cyclesDetected << "\n";

    // Clear the environment (should trigger more cleanup)
    env.reset();

    std::cout << "\nGC stats after clearing environment:\n";
    stats = gc.getStats();
    std::cout << "  Objects allocated: " << stats.currentlyAllocated << "\n";
    std::cout << "  Total freed: " << stats.totalFreed << "\n";

    std::cout << "\nGarbage collection test complete!\n";

    return 0;
}