#include "event_loop.h"
#include <thread>
#include <algorithm>

namespace lightjs {

EventLoop::EventLoop()
  : nextTimerId_(1), running_(false) {}

TimerId EventLoop::setTimeout(TimerCallback callback, int64_t delayMs) {
  TimerId id = nextTimerId_++;
  auto executeAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
  timerQueue_.emplace(id, executeAt, 0, callback);
  return id;
}

TimerId EventLoop::setInterval(TimerCallback callback, int64_t intervalMs) {
  TimerId id = nextTimerId_++;
  auto executeAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(intervalMs);
  timerQueue_.emplace(id, executeAt, intervalMs, callback);
  return id;
}

void EventLoop::clearTimer(TimerId id) {
  // Mark timer as cancelled - we can't remove from priority_queue efficiently
  // The timer will be skipped when it's popped
  // Note: This is a simplified implementation. A production implementation
  // might use a more sophisticated data structure.

  // We'll filter cancelled timers when processing
  // For now, we rebuild the queue without the cancelled timer
  std::vector<TimerEntry> entries;
  while (!timerQueue_.empty()) {
    auto entry = timerQueue_.top();
    timerQueue_.pop();
    if (entry.id != id) {
      entries.push_back(entry);
    }
  }

  for (auto& entry : entries) {
    timerQueue_.push(entry);
  }
}

void EventLoop::queueMicrotask(MicrotaskCallback callback) {
  microtaskQueue_.push(callback);
}

void EventLoop::processMicrotasks() {
  // Process all microtasks currently in the queue
  // Note: New microtasks queued during processing will be handled in the next iteration
  size_t count = microtaskQueue_.size();
  for (size_t i = 0; i < count; i++) {
    if (microtaskQueue_.empty()) break;

    auto callback = microtaskQueue_.front();
    microtaskQueue_.pop();

    try {
      callback();
    } catch (...) {
      // Microtask errors should not stop the event loop
      // In a real implementation, this would be reported
    }
  }
}

void EventLoop::processTimers() {
  auto now = std::chrono::steady_clock::now();

  // Process all timers that are ready
  while (!timerQueue_.empty()) {
    auto entry = timerQueue_.top();

    // If the next timer isn't ready yet, we're done
    if (entry.executeAt > now) {
      break;
    }

    timerQueue_.pop();

    // Skip cancelled timers
    if (entry.cancelled) {
      continue;
    }

    // Execute the timer callback
    try {
      entry.callback();
    } catch (...) {
      // Timer errors should not stop the event loop
    }

    // If it's an interval timer, reschedule it
    if (entry.intervalMs > 0) {
      entry.executeAt = now + std::chrono::milliseconds(entry.intervalMs);
      timerQueue_.push(entry);
    }
  }
}

bool EventLoop::runOnce() {
  running_ = true;

  // 1. Process all expired timers
  processTimers();

  // 2. Process all microtasks (this includes Promise callbacks)
  processMicrotasks();

  bool hasWork = hasPendingWork();
  if (!hasWork) {
    running_ = false;
  }
  return hasWork;
}

void EventLoop::run() {
  running_ = true;

  while (running_ && hasPendingWork()) {
    runOnce();

    // If there are timers but no immediate work, sleep until next timer
    if (!timerQueue_.empty() && microtaskQueue_.empty()) {
      auto now = std::chrono::steady_clock::now();
      auto nextTimer = timerQueue_.top().executeAt;

      if (nextTimer > now) {
        auto sleepDuration = std::chrono::duration_cast<std::chrono::milliseconds>(nextTimer - now);
        // Cap sleep at 100ms to allow checking for new work
        auto maxSleep = std::chrono::milliseconds(100);
        std::this_thread::sleep_for(std::min(sleepDuration, maxSleep));
      }
    } else if (!hasPendingWork()) {
      // No work at all, break
      break;
    }
  }

  running_ = false;
}

bool EventLoop::hasPendingWork() const {
  return !timerQueue_.empty() || !microtaskQueue_.empty();
}

void EventLoop::stop() {
  running_ = false;
}

size_t EventLoop::pendingTimerCount() const {
  return timerQueue_.size();
}

size_t EventLoop::pendingMicrotaskCount() const {
  return microtaskQueue_.size();
}

// Global event loop context
EventLoopContext& EventLoopContext::instance() {
  static EventLoopContext ctx;
  return ctx;
}

}
