#pragma once

#include <memory>
#include <vector>
#include <string>
#include <functional>
#include <optional>
#include <deque>
#include "gc.h"

namespace lightjs {

// Forward declarations
struct Value;
struct Promise;
struct Function;

// Stream state enums
enum class ReadableStreamState {
  Readable,   // Stream is open and can be read
  Closed,     // Stream has been closed
  Errored     // Stream has encountered an error
};

enum class WritableStreamState {
  Writable,   // Stream is open and can be written to
  Closing,    // Stream is in the process of closing
  Closed,     // Stream has been closed
  Errored     // Stream has encountered an error
};

// Forward declarations for stream types
struct ReadableStream;
struct ReadableStreamDefaultReader;
struct ReadableStreamDefaultController;
struct WritableStream;
struct WritableStreamDefaultWriter;
struct WritableStreamDefaultController;
struct TransformStream;
struct TransformStreamDefaultController;

// Queued chunk for stream buffers
struct QueuedChunk {
  std::shared_ptr<Value> value;
  size_t size;

  QueuedChunk(std::shared_ptr<Value> v, size_t s = 1) : value(v), size(s) {}
};

// ReadableStreamDefaultController - controls a ReadableStream
struct ReadableStreamDefaultController : public GCObject {
  std::weak_ptr<ReadableStream> stream;  // Weak reference to avoid cycle
  std::deque<QueuedChunk> queue;
  double desiredSize;                     // For backpressure
  bool closeRequested;
  bool pullAgain;
  bool pulling;
  bool started;

  // Underlying source callbacks (stored as shared_ptr<Function>)
  std::shared_ptr<Function> pullCallback;
  std::shared_ptr<Function> cancelCallback;

  ReadableStreamDefaultController()
    : desiredSize(1.0), closeRequested(false), pullAgain(false),
      pulling(false), started(false) {}

  // Controller methods
  void enqueue(const Value& chunk);
  void close();
  void error(const Value& reason);

  // GCObject interface
  const char* typeName() const override { return "ReadableStreamDefaultController"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// ReadableStreamDefaultReader - reads from a ReadableStream
struct ReadableStreamDefaultReader : public GCObject {
  std::shared_ptr<ReadableStream> stream;
  std::shared_ptr<Promise> closedPromise;

  // Pending read requests
  struct ReadRequest {
    std::shared_ptr<Promise> promise;
  };
  std::vector<ReadRequest> readRequests;

  ReadableStreamDefaultReader() = default;
  explicit ReadableStreamDefaultReader(std::shared_ptr<ReadableStream> s);

  // Reader methods
  std::shared_ptr<Promise> read();      // Returns Promise<{value, done}>
  void releaseLock();
  std::shared_ptr<Promise> cancel(const Value& reason);

  // GCObject interface
  const char* typeName() const override { return "ReadableStreamDefaultReader"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// ReadableStream - WHATWG ReadableStream implementation
struct ReadableStream : public GCObject, public std::enable_shared_from_this<ReadableStream> {
  ReadableStreamState state;
  std::shared_ptr<Value> storedError;

  // Controller for this stream
  std::shared_ptr<ReadableStreamDefaultController> controller;

  // Reader attached to this stream (if any)
  std::weak_ptr<ReadableStreamDefaultReader> reader;
  bool locked;

  // For piping
  bool disturbed;

  ReadableStream()
    : state(ReadableStreamState::Readable), locked(false), disturbed(false) {}

  // Stream methods
  std::shared_ptr<ReadableStreamDefaultReader> getReader();
  std::shared_ptr<Promise> cancel(const Value& reason);

  // Piping methods
  std::shared_ptr<Promise> pipeTo(std::shared_ptr<WritableStream> destination,
                                   bool preventClose = false,
                                   bool preventAbort = false,
                                   bool preventCancel = false);
  std::shared_ptr<ReadableStream> pipeThrough(std::shared_ptr<TransformStream> transform,
                                               bool preventClose = false,
                                               bool preventAbort = false,
                                               bool preventCancel = false);

  // Tee creates two branches of the stream
  std::pair<std::shared_ptr<ReadableStream>, std::shared_ptr<ReadableStream>> tee();

  // GCObject interface
  const char* typeName() const override { return "ReadableStream"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// WritableStreamDefaultController - controls a WritableStream
struct WritableStreamDefaultController : public GCObject {
  std::weak_ptr<WritableStream> stream;
  std::deque<QueuedChunk> queue;
  double desiredSize;
  bool started;

  // Underlying sink callbacks
  std::shared_ptr<Function> writeCallback;
  std::shared_ptr<Function> closeCallback;
  std::shared_ptr<Function> abortCallback;

  WritableStreamDefaultController()
    : desiredSize(1.0), started(false) {}

  // Controller methods
  void error(const Value& reason);

  // GCObject interface
  const char* typeName() const override { return "WritableStreamDefaultController"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// WritableStreamDefaultWriter - writes to a WritableStream
struct WritableStreamDefaultWriter : public GCObject {
  std::shared_ptr<WritableStream> stream;
  std::shared_ptr<Promise> closedPromise;
  std::shared_ptr<Promise> readyPromise;  // For backpressure

  // Pending write requests
  struct WriteRequest {
    std::shared_ptr<Value> chunk;
    std::shared_ptr<Promise> promise;
  };
  std::vector<WriteRequest> writeRequests;

  WritableStreamDefaultWriter() = default;
  explicit WritableStreamDefaultWriter(std::shared_ptr<WritableStream> s);

  // Writer methods
  std::shared_ptr<Promise> write(const Value& chunk);
  std::shared_ptr<Promise> close();
  std::shared_ptr<Promise> abort(const Value& reason);
  void releaseLock();

  // Properties
  double desiredSize() const;

  // GCObject interface
  const char* typeName() const override { return "WritableStreamDefaultWriter"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// WritableStream - WHATWG WritableStream implementation
struct WritableStream : public GCObject, public std::enable_shared_from_this<WritableStream> {
  WritableStreamState state;
  std::shared_ptr<Value> storedError;

  // Controller for this stream
  std::shared_ptr<WritableStreamDefaultController> controller;

  // Writer attached to this stream (if any)
  std::weak_ptr<WritableStreamDefaultWriter> writer;
  bool locked;

  // Pending operations
  std::shared_ptr<Promise> pendingAbortRequest;

  // For close operation
  std::shared_ptr<Promise> closeRequest;
  std::shared_ptr<Promise> inFlightWriteRequest;
  std::shared_ptr<Promise> inFlightCloseRequest;

  WritableStream()
    : state(WritableStreamState::Writable), locked(false) {}

  // Stream methods
  std::shared_ptr<WritableStreamDefaultWriter> getWriter();
  std::shared_ptr<Promise> abort(const Value& reason);
  std::shared_ptr<Promise> close();

  // GCObject interface
  const char* typeName() const override { return "WritableStream"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// TransformStreamDefaultController - controls a TransformStream
struct TransformStreamDefaultController : public GCObject {
  std::weak_ptr<TransformStream> stream;
  std::shared_ptr<Function> transformCallback;
  std::shared_ptr<Function> flushCallback;

  TransformStreamDefaultController() = default;

  // Controller methods
  void enqueue(const Value& chunk);  // Enqueue to readable side
  void error(const Value& reason);
  void terminate();

  // Access to desiredSize from the readable side
  double desiredSize() const;

  // GCObject interface
  const char* typeName() const override { return "TransformStreamDefaultController"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// TransformStream - connects a ReadableStream and WritableStream with a transformer
struct TransformStream : public GCObject {
  std::shared_ptr<ReadableStream> readable;
  std::shared_ptr<WritableStream> writable;
  std::shared_ptr<TransformStreamDefaultController> controller;

  // Backpressure state
  bool backpressure;
  std::shared_ptr<Promise> backpressureChangePromise;

  TransformStream() : backpressure(false) {}

  // GCObject interface
  const char* typeName() const override { return "TransformStream"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Helper functions for stream creation
std::shared_ptr<ReadableStream> createReadableStream(
  std::shared_ptr<Function> start,
  std::shared_ptr<Function> pull = nullptr,
  std::shared_ptr<Function> cancel = nullptr,
  double highWaterMark = 1.0);

std::shared_ptr<WritableStream> createWritableStream(
  std::shared_ptr<Function> start,
  std::shared_ptr<Function> write = nullptr,
  std::shared_ptr<Function> close = nullptr,
  std::shared_ptr<Function> abort = nullptr,
  double highWaterMark = 1.0);

std::shared_ptr<TransformStream> createTransformStream(
  std::shared_ptr<Function> start,
  std::shared_ptr<Function> transform = nullptr,
  std::shared_ptr<Function> flush = nullptr);

}  // namespace lightjs
