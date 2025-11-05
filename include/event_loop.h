#pragma once

#include "value.h"
#include <functional>
#include <queue>
#include <vector>
#include <chrono>
#include <memory>
#include <mutex>

namespace tinyjs {

// Forward declarations
class Interpreter;
class Environment;

// Timer ID type
using TimerId = uint64_t;

// Callback type for timers
using TimerCallback = std::function<Value()>;

// Callback type for microtasks
using MicrotaskCallback = std::function<void()>;

// Timer entry in the timer queue
struct TimerEntry {
  TimerId id;
  std::chrono::steady_clock::time_point executeAt;
  int64_t intervalMs;  // 0 for setTimeout, > 0 for setInterval
  TimerCallback callback;
  bool cancelled;

  TimerEntry(TimerId tid, std::chrono::steady_clock::time_point exec,
             int64_t interval, TimerCallback cb)
    : id(tid), executeAt(exec), intervalMs(interval), callback(cb), cancelled(false) {}

  // Comparison for priority queue (earlier times have higher priority)
  bool operator>(const TimerEntry& other) const {
    return executeAt > other.executeAt;
  }
};

// Event loop manages timers and microtasks
class EventLoop {
public:
  EventLoop();
  ~EventLoop() = default;

  // Timer management
  TimerId setTimeout(TimerCallback callback, int64_t delayMs);
  TimerId setInterval(TimerCallback callback, int64_t intervalMs);
  void clearTimer(TimerId id);

  // Microtask management
  void queueMicrotask(MicrotaskCallback callback);

  // Event loop execution
  void run();                    // Run until all tasks complete
  bool runOnce();                // Process one iteration (timers + microtasks)
  bool hasPendingWork() const;   // Check if there are pending tasks
  void stop();                   // Stop the event loop

  // For testing/debugging
  size_t pendingTimerCount() const;
  size_t pendingMicrotaskCount() const;

private:
  TimerId nextTimerId_;
  std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<TimerEntry>> timerQueue_;
  std::queue<MicrotaskCallback> microtaskQueue_;
  bool running_;

  void processMicrotasks();
  void processTimers();
};

// Global event loop instance (one per interpreter context)
class EventLoopContext {
public:
  static EventLoopContext& instance();

  EventLoop& getLoop() { return loop_; }
  void setLoop(EventLoop loop) { loop_ = std::move(loop); }

private:
  EventLoopContext() = default;
  EventLoop loop_;
  std::mutex mutex_;
};

}
