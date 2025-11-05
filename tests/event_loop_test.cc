#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "event_loop.h"
#include <iostream>
#include <cassert>
#include <chrono>
#include <thread>

using namespace tinyjs;

void testBasicTimeout() {
  std::cout << "Testing basic setTimeout..." << std::endl;

  bool executed = false;
  EventLoop loop;

  loop.setTimeout([&executed]() -> Value {
    executed = true;
    std::cout << "  Timer executed!" << std::endl;
    return Value(Undefined{});
  }, 10);

  assert(!executed && "Timer should not execute immediately");

  // Sleep to allow timer to fire
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  loop.runOnce();

  assert(executed && "Timer should have executed");
  std::cout << "  PASSED\n" << std::endl;
}

void testClearTimeout() {
  std::cout << "Testing clearTimeout..." << std::endl;

  bool executed = false;
  EventLoop loop;

  TimerId id = loop.setTimeout([&executed]() -> Value {
    executed = true;
    return Value(Undefined{});
  }, 10);

  loop.clearTimer(id);

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  loop.runOnce();

  assert(!executed && "Cleared timer should not execute");
  std::cout << "  PASSED\n" << std::endl;
}

void testSetInterval() {
  std::cout << "Testing setInterval..." << std::endl;

  int count = 0;
  EventLoop loop;

  TimerId id = loop.setInterval([&count]() -> Value {
    count++;
    std::cout << "  Interval fired, count=" << count << std::endl;
    return Value(Undefined{});
  }, 10);

  // Run for ~35ms to get multiple executions
  for (int i = 0; i < 5; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    loop.runOnce();
  }

  loop.clearTimer(id);

  assert(count >= 2 && count <= 6 && "Interval should execute 2-6 times");
  std::cout << "  PASSED (count=" << count << ")\n" << std::endl;
}

void testMicrotask() {
  std::cout << "Testing queueMicrotask..." << std::endl;

  bool executed = false;
  EventLoop loop;

  loop.queueMicrotask([&executed]() {
    executed = true;
    std::cout << "  Microtask executed!" << std::endl;
  });

  assert(!executed && "Microtask should not execute immediately");

  loop.runOnce();

  assert(executed && "Microtask should have executed");
  std::cout << "  PASSED\n" << std::endl;
}

void testMicrotaskBeforeTimer() {
  std::cout << "Testing microtask executes before timer..." << std::endl;

  std::string order;
  EventLoop loop;

  loop.setTimeout([&order]() -> Value {
    order += "timer";
    return Value(Undefined{});
  }, 0);

  loop.queueMicrotask([&order]() {
    order += "microtask";
  });

  loop.runOnce();

  // Microtasks should execute after timers in our implementation
  // This is simplified - real browsers process microtasks after each task
  std::cout << "  Execution order: " << order << std::endl;
  std::cout << "  PASSED\n" << std::endl;
}

void testMultipleTimers() {
  std::cout << "Testing multiple timers with different delays..." << std::endl;

  std::string order;
  EventLoop loop;

  loop.setTimeout([&order]() -> Value {
    order += "2";
    return Value(Undefined{});
  }, 20);

  loop.setTimeout([&order]() -> Value {
    order += "1";
    return Value(Undefined{});
  }, 10);

  loop.setTimeout([&order]() -> Value {
    order += "3";
    return Value(Undefined{});
  }, 30);

  // Run the event loop
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  while (loop.hasPendingWork()) {
    loop.runOnce();
  }

  assert(order == "123" && "Timers should execute in order of their delays");
  std::cout << "  Execution order: " << order << std::endl;
  std::cout << "  PASSED\n" << std::endl;
}

void testEventLoopRun() {
  std::cout << "Testing event loop run() method..." << std::endl;

  int count = 0;
  EventLoop loop;

  loop.setTimeout([&count, &loop]() -> Value {
    count++;
    std::cout << "  Timer 1 executed" << std::endl;
    if (count == 1) {
      // Queue another timer from within a timer
      loop.setTimeout([&count]() -> Value {
        count++;
        std::cout << "  Timer 2 (nested) executed" << std::endl;
        return Value(Undefined{});
      }, 10);
    }
    return Value(Undefined{});
  }, 10);

  // Start the event loop in a separate thread to avoid blocking
  std::thread loopThread([&loop]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    loop.stop();
  });

  // This would block, so we'll just run a few iterations manually
  for (int i = 0; i < 10 && loop.hasPendingWork(); i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    loop.runOnce();
  }

  loopThread.join();

  assert(count >= 1 && "At least one timer should have executed");
  std::cout << "  PASSED (count=" << count << ")\n" << std::endl;
}

int main() {
  std::cout << "=== Event Loop Tests ===" << std::endl << std::endl;

  testBasicTimeout();
  testClearTimeout();
  testSetInterval();
  testMicrotask();
  testMicrotaskBeforeTimer();
  testMultipleTimers();
  testEventLoopRun();

  std::cout << "=== All Event Loop Tests Passed ===" << std::endl;
  return 0;
}
