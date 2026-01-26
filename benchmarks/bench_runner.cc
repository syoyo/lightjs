#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/interpreter.h"
#include "../include/environment.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <coroutine>

using namespace lightjs;
using namespace std::chrono;

struct BenchmarkResult {
  std::string name;
  double timeMs;
  size_t iterations;
  double opsPerSecond;
  bool success;
  std::string error;
};

class BenchmarkRunner {
public:
  BenchmarkRunner() : totalBenchmarks_(0), passedBenchmarks_(0) {}

  BenchmarkResult runBenchmark(const std::string& name, const std::string& code, size_t iterations = 1) {
    std::cout << "Running benchmark: " << name << " (" << iterations << " iterations)..." << std::flush;

    BenchmarkResult result;
    result.name = name;
    result.iterations = iterations;
    result.success = false;

    try {
      // Parse once
      Lexer lexer(code);
      auto tokens = lexer.tokenize();
      Parser parser(tokens);
      auto program = parser.parse();

      if (!program) {
        result.error = "Parse error";
        std::cout << " FAILED (parse error)\n";
        return result;
      }

      // Create environment
      auto env = Environment::createGlobal();
      Interpreter interpreter(env);

      // Warm-up run
      auto warmupTask = interpreter.evaluate(*program);
      while (!warmupTask.done()) {
        std::coroutine_handle<>::from_address(warmupTask.handle.address()).resume();
      }

      // Timed runs
      auto startTime = high_resolution_clock::now();

      for (size_t i = 0; i < iterations; ++i) {
        auto task = interpreter.evaluate(*program);
        while (!task.done()) {
          std::coroutine_handle<>::from_address(task.handle.address()).resume();
        }
      }

      auto endTime = high_resolution_clock::now();
      auto duration = duration_cast<microseconds>(endTime - startTime);

      result.timeMs = duration.count() / 1000.0;
      result.opsPerSecond = (iterations * 1000000.0) / duration.count();
      result.success = true;

      std::cout << " " << std::fixed << std::setprecision(2) << result.timeMs << "ms "
                << "(" << std::setprecision(0) << result.opsPerSecond << " ops/sec)\n";

      totalBenchmarks_++;
      passedBenchmarks_++;

    } catch (const std::exception& e) {
      result.error = e.what();
      std::cout << " FAILED: " << e.what() << "\n";
      totalBenchmarks_++;
    }

    results_.push_back(result);
    return result;
  }

  BenchmarkResult runBenchmarkFromFile(const std::string& name, const std::string& filepath, size_t iterations = 1) {
    std::ifstream file(filepath);
    if (!file) {
      BenchmarkResult result;
      result.name = name;
      result.success = false;
      result.error = "Cannot open file: " + filepath;
      std::cout << "Cannot open file: " << filepath << "\n";
      return result;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return runBenchmark(name, buffer.str(), iterations);
  }

  void printSummary() {
    std::cout << "\n========================================\n";
    std::cout << "Benchmark Summary\n";
    std::cout << "========================================\n";
    std::cout << "Total benchmarks: " << totalBenchmarks_ << "\n";
    std::cout << "Passed: " << passedBenchmarks_ << "\n";
    std::cout << "Failed: " << (totalBenchmarks_ - passedBenchmarks_) << "\n";
    std::cout << "\nResults:\n";
    std::cout << std::left << std::setw(30) << "Benchmark"
              << std::right << std::setw(12) << "Time (ms)"
              << std::setw(15) << "Ops/sec" << "\n";
    std::cout << std::string(57, '-') << "\n";

    for (const auto& result : results_) {
      if (result.success) {
        std::cout << std::left << std::setw(30) << result.name
                  << std::right << std::setw(12) << std::fixed << std::setprecision(2) << result.timeMs
                  << std::setw(15) << std::setprecision(0) << result.opsPerSecond << "\n";
      } else {
        std::cout << std::left << std::setw(30) << result.name
                  << std::right << std::setw(27) << "FAILED\n";
      }
    }
    std::cout << "\n";
  }

  void saveResults(const std::string& filename) {
    std::ofstream file(filename);
    if (!file) {
      std::cerr << "Cannot write to file: " << filename << "\n";
      return;
    }

    file << "benchmark,time_ms,ops_per_sec,iterations,success,error\n";
    for (const auto& result : results_) {
      file << result.name << ","
           << result.timeMs << ","
           << result.opsPerSecond << ","
           << result.iterations << ","
           << (result.success ? "true" : "false") << ","
           << result.error << "\n";
    }

    std::cout << "Results saved to: " << filename << "\n";
  }

private:
  std::vector<BenchmarkResult> results_;
  size_t totalBenchmarks_;
  size_t passedBenchmarks_;
};

int main(int argc, char* argv[]) {
  std::cout << "LightJS Benchmark Suite\n";
  std::cout << "=======================\n\n";

  BenchmarkRunner runner;

  // Arithmetic benchmark
  runner.runBenchmark("Arithmetic", R"(
    let sum = 0;
    for (let i = 0; i < 100000; i++) {
      sum = sum + i;
    }
    sum
  )", 10);

  // Function calls
  runner.runBenchmark("Function Calls", R"(
    function fibonacci(n) {
      if (n <= 1) return n;
      return fibonacci(n - 1) + fibonacci(n - 2);
    }
    fibonacci(20)
  )", 5);

  // Array operations
  runner.runBenchmark("Array Operations", R"(
    let arr = [];
    for (let i = 0; i < 1000; i++) {
      arr.push(i);
    }
    let sum = 0;
    for (let i = 0; i < arr.length; i++) {
      sum = sum + arr[i];
    }
    sum
  )", 100);

  // Object property access
  runner.runBenchmark("Object Access", R"(
    let obj = {a: 1, b: 2, c: 3, d: 4, e: 5};
    let sum = 0;
    for (let i = 0; i < 10000; i++) {
      sum = sum + obj.a + obj.b + obj.c + obj.d + obj.e;
    }
    sum
  )", 10);

  // String operations
  runner.runBenchmark("String Operations", R"(
    let str = "hello";
    let result = "";
    for (let i = 0; i < 1000; i++) {
      result = result + str;
    }
    result.length
  )", 10);

  // Array methods (map/filter/reduce)
  runner.runBenchmark("Array Methods", R"(
    let arr = [];
    for (let i = 0; i < 100; i++) {
      arr.push(i);
    }
    let doubled = arr.map(x => x * 2);
    let evens = doubled.filter(x => x % 4 === 0);
    let sum = evens.reduce((a, b) => a + b, 0);
    sum
  )", 100);

  // Closures
  runner.runBenchmark("Closures", R"(
    function makeCounter() {
      let count = 0;
      return function() {
        count = count + 1;
        return count;
      };
    }
    let counter = makeCounter();
    for (let i = 0; i < 10000; i++) {
      counter();
    }
  )", 10);

  // Class instantiation
  runner.runBenchmark("Class Creation", R"(
    class Point {
      constructor(x, y) {
        this.x = x;
        this.y = y;
      }
      distance() {
        return Math.sqrt(this.x * this.x + this.y * this.y);
      }
    }
    let sum = 0;
    for (let i = 0; i < 1000; i++) {
      let p = new Point(i, i + 1);
      sum = sum + p.distance();
    }
    sum
  )", 10);

  // Run external benchmarks if provided
  if (argc > 1) {
    for (int i = 1; i < argc; i++) {
      std::string filepath = argv[i];
      std::string name = filepath.substr(filepath.find_last_of("/\\") + 1);
      runner.runBenchmarkFromFile(name, filepath, 10);
    }
  }

  runner.printSummary();

  // Save results
  auto now = system_clock::now();
  auto timestamp = system_clock::to_time_t(now);
  std::stringstream filename;
  filename << "bench_results_" << timestamp << ".csv";
  runner.saveResults(filename.str());

  return 0;
}
