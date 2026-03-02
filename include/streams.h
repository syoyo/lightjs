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
  ReadableStream* stream = nullptr;  // Weak reference to avoid cycle (raw ptr, owned by stream)
  std::deque<QueuedChunk> queue;
  double desiredSize;                     // For backpressure
  bool closeRequested;
  bool pullAgain;
  bool pulling;
  bool started;

  // Underlying source callbacks (stored as shared_ptr<Function>)
  GCPtr<Function> pullCallback;
  GCPtr<Function> cancelCallback;

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
  GCPtr<ReadableStream> stream;
  GCPtr<Promise> closedPromise;

  // Pending read requests
  struct ReadRequest {
    GCPtr<Promise> promise;
  };
  std::vector<ReadRequest> readRequests;

  ReadableStreamDefaultReader() = default;
  explicit ReadableStreamDefaultReader(GCPtr<ReadableStream> s);

  // Reader methods
  GCPtr<Promise> read();      // Returns Promise<{value, done}>
  void releaseLock();
  GCPtr<Promise> cancel(const Value& reason);

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
  ReadableStreamDefaultReader* reader = nullptr;
  bool locked;

  // For piping
  bool disturbed;

  ReadableStream()
    : state(ReadableStreamState::Readable), locked(false), disturbed(false) {}

  // Stream methods
  GCPtr<ReadableStreamDefaultReader> getReader();
  GCPtr<Promise> cancel(const Value& reason);

  // Piping methods
  GCPtr<Promise> pipeTo(GCPtr<WritableStream> destination,
                                   bool preventClose = false,
                                   bool preventAbort = false,
                                   bool preventCancel = false);
  GCPtr<ReadableStream> pipeThrough(GCPtr<TransformStream> transform,
                                               bool preventClose = false,
                                               bool preventAbort = false,
                                               bool preventCancel = false);

  // Tee creates two branches of the stream
  std::pair<GCPtr<ReadableStream>, GCPtr<ReadableStream>> tee();

  // GCObject interface
  const char* typeName() const override { return "ReadableStream"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// WritableStreamDefaultController - controls a WritableStream
struct WritableStreamDefaultController : public GCObject {
  WritableStream* stream = nullptr;
  std::deque<QueuedChunk> queue;
  double desiredSize;
  bool started;

  // Underlying sink callbacks
  GCPtr<Function> writeCallback;
  GCPtr<Function> closeCallback;
  GCPtr<Function> abortCallback;

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
  GCPtr<WritableStream> stream;
  GCPtr<Promise> closedPromise;
  GCPtr<Promise> readyPromise;  // For backpressure

  // Pending write requests
  struct WriteRequest {
    std::shared_ptr<Value> chunk;
    GCPtr<Promise> promise;
  };
  std::vector<WriteRequest> writeRequests;

  WritableStreamDefaultWriter() = default;
  explicit WritableStreamDefaultWriter(GCPtr<WritableStream> s);

  // Writer methods
  GCPtr<Promise> write(const Value& chunk);
  GCPtr<Promise> close();
  GCPtr<Promise> abort(const Value& reason);
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
  WritableStreamDefaultWriter* writer = nullptr;
  bool locked;

  // Pending operations
  GCPtr<Promise> pendingAbortRequest;

  // For close operation
  GCPtr<Promise> closeRequest;
  GCPtr<Promise> inFlightWriteRequest;
  GCPtr<Promise> inFlightCloseRequest;

  WritableStream()
    : state(WritableStreamState::Writable), locked(false) {}

  // Stream methods
  GCPtr<WritableStreamDefaultWriter> getWriter();
  GCPtr<Promise> abort(const Value& reason);
  GCPtr<Promise> close();

  // GCObject interface
  const char* typeName() const override { return "WritableStream"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// TransformStreamDefaultController - controls a TransformStream
struct TransformStreamDefaultController : public GCObject {
  TransformStream* stream = nullptr;
  GCPtr<Function> transformCallback;
  GCPtr<Function> flushCallback;

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
  GCPtr<ReadableStream> readable;
  GCPtr<WritableStream> writable;
  std::shared_ptr<TransformStreamDefaultController> controller;

  // Backpressure state
  bool backpressure;
  GCPtr<Promise> backpressureChangePromise;

  TransformStream() : backpressure(false) {}

  // GCObject interface
  const char* typeName() const override { return "TransformStream"; }
  void getReferences(std::vector<GCObject*>& refs) const override;
};

// Helper functions for stream creation
GCPtr<ReadableStream> createReadableStream(
  GCPtr<Function> start = {},
    GCPtr<Function> pull = {},
    GCPtr<Function> cancel = {},
  double highWaterMark = 1.0);

GCPtr<WritableStream> createWritableStream(
  GCPtr<Function> start,
  GCPtr<Function> write = {},
  GCPtr<Function> close = {},
  GCPtr<Function> abort = {},
  double highWaterMark = 1.0);

GCPtr<TransformStream> createTransformStream(
  GCPtr<Function> start,
  GCPtr<Function> transform = {},
  GCPtr<Function> flush = {});

}  // namespace lightjs
