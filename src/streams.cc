#include "streams.h"
#include "value.h"
#include "streams.h"
#include "wasm_js.h"

namespace lightjs {

// ===========================================================================
// ReadableStreamDefaultController implementation
// ===========================================================================

void ReadableStreamDefaultController::enqueue(const Value& chunk) {
  if (closeRequested) {
    return;  // Cannot enqueue after close is requested
  }

  auto* streamPtr = stream;
  if (!streamPtr || streamPtr->state != ReadableStreamState::Readable) {
    return;  // Stream not readable
  }

  // Add chunk to queue
  queue.push_back(QueuedChunk(std::make_shared<Value>(chunk)));

  // Update desired size (simple backpressure)
  desiredSize -= 1.0;

  // Try to fulfill pending read requests
  auto* readerPtr = streamPtr->reader;
  if (readerPtr && !readerPtr->readRequests.empty()) {
    // Dequeue and fulfill the oldest read request
    auto& request = readerPtr->readRequests.front();
    if (!queue.empty()) {
      auto queuedChunk = queue.front();
      queue.pop_front();
      desiredSize += 1.0;

      // Create result object {value, done}
      auto resultObj = GarbageCollector::makeGC<Object>();
      resultObj->properties["value"] = *queuedChunk.value;
      resultObj->properties["done"] = false;

      // Resolve the read promise
      request.promise->resolve(Value(resultObj));
      readerPtr->readRequests.erase(readerPtr->readRequests.begin());
    }
  }
}

void ReadableStreamDefaultController::close() {
  if (closeRequested) {
    return;  // Already closing
  }

  closeRequested = true;

  auto* streamPtr = stream;
  if (!streamPtr) {
    return;
  }

  // If queue is empty, close the stream immediately
  if (queue.empty()) {
    streamPtr->state = ReadableStreamState::Closed;

    // Resolve any pending read requests with done=true
    auto* readerPtr = streamPtr->reader;
    if (readerPtr) {
      for (auto& request : readerPtr->readRequests) {
        auto resultObj = GarbageCollector::makeGC<Object>();
        resultObj->properties["value"] = Undefined{};
        resultObj->properties["done"] = true;
        request.promise->resolve(Value(resultObj));
      }
      readerPtr->readRequests.clear();

      // Resolve closed promise
      if (readerPtr->closedPromise) {
        readerPtr->closedPromise->resolve(Undefined{});
      }
    }
  }
}

void ReadableStreamDefaultController::error(const Value& reason) {
  auto* streamPtr = stream;
  if (!streamPtr) {
    return;
  }

  if (streamPtr->state != ReadableStreamState::Readable) {
    return;  // Already errored or closed
  }

  // Clear queue and set error state
  queue.clear();
  streamPtr->state = ReadableStreamState::Errored;
  streamPtr->storedError = std::make_shared<Value>(reason);

  // Reject any pending read requests
  auto* readerPtr = streamPtr->reader;
  if (readerPtr) {
    for (auto& request : readerPtr->readRequests) {
      request.promise->reject(reason);
    }
    readerPtr->readRequests.clear();

    // Reject closed promise
    if (readerPtr->closedPromise) {
      readerPtr->closedPromise->reject(reason);
    }
  }
}

void ReadableStreamDefaultController::getReferences(std::vector<GCObject*>& refs) const {
  if (pullCallback) refs.push_back(pullCallback.get());
  if (cancelCallback) refs.push_back(cancelCallback.get());
  for (const auto& chunk : queue) {
    // Values contain shared_ptrs which handle their own memory
  }
}

// ===========================================================================
// ReadableStreamDefaultReader implementation
// ===========================================================================

ReadableStreamDefaultReader::ReadableStreamDefaultReader(GCPtr<ReadableStream> s)
  : stream(s) {
  closedPromise = GarbageCollector::makeGC<Promise>();

  if (s) {
    // Lock the stream
    s->locked = true;
  }
}

GCPtr<Promise> ReadableStreamDefaultReader::read() {
  auto promise = GarbageCollector::makeGC<Promise>();

  if (!stream) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Reader has no stream");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  if (stream->state == ReadableStreamState::Closed) {
    // Return {value: undefined, done: true}
    auto resultObj = GarbageCollector::makeGC<Object>();
    resultObj->properties["value"] = Undefined{};
    resultObj->properties["done"] = true;
    promise->resolve(Value(resultObj));
    return GCPtr<Promise>(promise);
  }

  if (stream->state == ReadableStreamState::Errored) {
    promise->reject(*stream->storedError);
    return GCPtr<Promise>(promise);
  }

  // Stream is readable - check if there's a chunk in queue
  if (stream->controller && !stream->controller->queue.empty()) {
    auto chunk = stream->controller->queue.front();
    stream->controller->queue.pop_front();
    stream->controller->desiredSize += 1.0;

    auto resultObj = GarbageCollector::makeGC<Object>();
    resultObj->properties["value"] = *chunk.value;
    resultObj->properties["done"] = false;
    promise->resolve(Value(resultObj));

    // If queue is now empty and close was requested, close the stream
    if (stream->controller->closeRequested && stream->controller->queue.empty()) {
      stream->controller->close();
    }
  } else {
    // Add to pending read requests
    ReadRequest request;
    request.promise = promise;
    readRequests.push_back(request);

    // Try to pull more data if available
    if (stream->controller && stream->controller->pullCallback && !stream->controller->pulling) {
      // TODO: Call pull callback when integrated with interpreter
    }
  }

  return GCPtr<Promise>(promise);
}

void ReadableStreamDefaultReader::releaseLock() {
  if (!stream) {
    return;
  }

  // Reject any pending read requests
  for (auto& request : readRequests) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Reader was released");
    request.promise->reject(Value(errorObj));
  }
  readRequests.clear();

  // Unlock the stream
  stream->locked = false;
  stream.reset();
}

GCPtr<Promise> ReadableStreamDefaultReader::cancel(const Value& reason) {
  if (!stream) {
    auto promise = GarbageCollector::makeGC<Promise>();
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Reader has no stream");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  return stream->cancel(reason);
}

void ReadableStreamDefaultReader::getReferences(std::vector<GCObject*>& refs) const {
  if (stream) refs.push_back(stream.get());
  if (closedPromise) refs.push_back(closedPromise.get());
  for (const auto& req : readRequests) {
    if (req.promise) refs.push_back(req.promise.get());
  }
}

// ===========================================================================
// ReadableStream implementation
// ===========================================================================

GCPtr<ReadableStreamDefaultReader> ReadableStream::getReader() {
  if (locked) {
    return {};  // Stream already locked
  }

  auto readerPtr = GarbageCollector::makeGC<ReadableStreamDefaultReader>(GCPtr<ReadableStream>(const_cast<ReadableStream*>(this)));
  reader = readerPtr;
  locked = true;

  return GCPtr<ReadableStreamDefaultReader>(readerPtr);
}

GCPtr<Promise> ReadableStream::cancel(const Value& reason) {
  auto promise = GarbageCollector::makeGC<Promise>();

  if (state == ReadableStreamState::Closed) {
    promise->resolve(Undefined{});
    return GCPtr<Promise>(promise);
  }

  if (state == ReadableStreamState::Errored) {
    promise->reject(*storedError);
    return GCPtr<Promise>(promise);
  }

  // Mark as disturbed
  disturbed = true;

  // Close the stream
  state = ReadableStreamState::Closed;

  // Clear the queue
  if (controller) {
    controller->queue.clear();
  }

  // Call cancel callback if provided
  if (controller && controller->cancelCallback) {
    // TODO: Call cancel callback when integrated with interpreter
  }

  promise->resolve(Undefined{});
  return GCPtr<Promise>(promise);
}

GCPtr<Promise> ReadableStream::pipeTo(
    GCPtr<WritableStream> destination,
    bool preventClose,
    bool preventAbort,
    bool preventCancel) {

  auto promise = GarbageCollector::makeGC<Promise>();

  if (locked) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "ReadableStream is locked");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  if (destination->locked) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "WritableStream is locked");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  // Mark streams as disturbed/locked
  disturbed = true;

  // Get reader and writer
  auto readerPtr = getReader();
  auto writerPtr = destination->getWriter();

  if (!readerPtr || !writerPtr) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Failed to get reader/writer");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  // TODO: Implement actual piping loop
  // This would need integration with the interpreter's event loop

  promise->resolve(Undefined{});
  return GCPtr<Promise>(promise);
}

GCPtr<ReadableStream> ReadableStream::pipeThrough(
    GCPtr<TransformStream> transform,
    bool preventClose,
    bool preventAbort,
    bool preventCancel) {

  if (!transform) {
    return {};
  }

  // Pipe to the writable side of the transform
  pipeTo(transform->writable, preventClose, preventAbort, preventCancel);

  // Return the readable side of the transform
  return GCPtr<ReadableStream>(transform->readable);
}

std::pair<GCPtr<ReadableStream>, GCPtr<ReadableStream>> ReadableStream::tee() {
  // Create two new readable streams
  auto branch1 = GarbageCollector::makeGC<ReadableStream>();
  auto branch2 = GarbageCollector::makeGC<ReadableStream>();

  // Create controllers for both branches
  branch1->controller = std::make_shared<ReadableStreamDefaultController>();
  branch1->controller->stream = branch1;
  branch2->controller = std::make_shared<ReadableStreamDefaultController>();
  branch2->controller->stream = branch2;

  // TODO: Implement actual tee logic that reads from source and enqueues to both branches

  return {branch1, branch2};
}

void ReadableStream::getReferences(std::vector<GCObject*>& refs) const {
  if (storedError) {
    // storedError is a Value, which manages its own memory
  }
  if (controller) refs.push_back(controller.get());
  auto* readerPtr = reader;
  if (readerPtr) refs.push_back(readerPtr);
}

// ===========================================================================
// WritableStreamDefaultController implementation
// ===========================================================================

void WritableStreamDefaultController::error(const Value& reason) {
  auto* streamPtr = stream;
  if (!streamPtr) {
    return;
  }

  if (streamPtr->state != WritableStreamState::Writable) {
    return;  // Already errored or closed
  }

  // Clear queue and set error state
  queue.clear();
  streamPtr->state = WritableStreamState::Errored;
  streamPtr->storedError = std::make_shared<Value>(reason);

  // Reject any pending write requests
  auto* writerPtr = streamPtr->writer;
  if (writerPtr) {
    for (auto& request : writerPtr->writeRequests) {
      request.promise->reject(reason);
    }
    writerPtr->writeRequests.clear();

    // Reject closed promise
    if (writerPtr->closedPromise) {
      writerPtr->closedPromise->reject(reason);
    }
  }
}

void WritableStreamDefaultController::getReferences(std::vector<GCObject*>& refs) const {
  if (writeCallback) refs.push_back(writeCallback.get());
  if (closeCallback) refs.push_back(closeCallback.get());
  if (abortCallback) refs.push_back(abortCallback.get());
}

// ===========================================================================
// WritableStreamDefaultWriter implementation
// ===========================================================================

WritableStreamDefaultWriter::WritableStreamDefaultWriter(GCPtr<WritableStream> s)
  : stream(s) {
  closedPromise = GarbageCollector::makeGC<Promise>();
  readyPromise = GarbageCollector::makeGC<Promise>();
  readyPromise->resolve(Undefined{});  // Initially ready

  if (s) {
    s->locked = true;
  }
}

GCPtr<Promise> WritableStreamDefaultWriter::write(const Value& chunk) {
  auto promise = GarbageCollector::makeGC<Promise>();

  if (!stream) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Writer has no stream");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  if (stream->state == WritableStreamState::Errored) {
    promise->reject(*stream->storedError);
    return GCPtr<Promise>(promise);
  }

  if (stream->state == WritableStreamState::Closed ||
      stream->state == WritableStreamState::Closing) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Stream is closed");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  // Add to queue
  WriteRequest request;
  request.chunk = std::make_shared<Value>(chunk);
  request.promise = promise;
  writeRequests.push_back(request);

  // Update controller queue
  if (stream->controller) {
    stream->controller->queue.push_back(
      QueuedChunk(std::make_shared<Value>(chunk)));
    stream->controller->desiredSize -= 1.0;

    // TODO: Call write callback when integrated with interpreter
  }

  // For now, immediately resolve since we don't have async write callback
  promise->resolve(Undefined{});
  writeRequests.clear();

  return GCPtr<Promise>(promise);
}

GCPtr<Promise> WritableStreamDefaultWriter::close() {
  auto promise = GarbageCollector::makeGC<Promise>();

  if (!stream) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Writer has no stream");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  if (stream->state == WritableStreamState::Closed) {
    promise->resolve(Undefined{});
    return GCPtr<Promise>(promise);
  }

  if (stream->state == WritableStreamState::Errored) {
    promise->reject(*stream->storedError);
    return GCPtr<Promise>(promise);
  }

  stream->state = WritableStreamState::Closing;

  // Wait for pending writes to complete
  if (stream->controller && !stream->controller->queue.empty()) {
    // TODO: Queue close request after pending writes
  }

  // Call close callback if provided
  if (stream->controller && stream->controller->closeCallback) {
    // TODO: Call close callback when integrated with interpreter
  }

  stream->state = WritableStreamState::Closed;
  promise->resolve(Undefined{});

  // Resolve closed promise
  if (closedPromise) {
    closedPromise->resolve(Undefined{});
  }

  return GCPtr<Promise>(promise);
}

GCPtr<Promise> WritableStreamDefaultWriter::abort(const Value& reason) {
  auto promise = GarbageCollector::makeGC<Promise>();

  if (!stream) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Writer has no stream");
    promise->reject(Value(errorObj));
    return GCPtr<Promise>(promise);
  }

  return stream->abort(reason);
}

void WritableStreamDefaultWriter::releaseLock() {
  if (!stream) {
    return;
  }

  // Reject any pending write requests
  for (auto& request : writeRequests) {
    auto errorObj = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Writer was released");
    request.promise->reject(Value(errorObj));
  }
  writeRequests.clear();

  stream->locked = false;
  stream.reset();
}

double WritableStreamDefaultWriter::desiredSize() const {
  if (!stream || !stream->controller) {
    return 0.0;
  }
  return stream->controller->desiredSize;
}

void WritableStreamDefaultWriter::getReferences(std::vector<GCObject*>& refs) const {
  if (stream) refs.push_back(stream.get());
  if (closedPromise) refs.push_back(closedPromise.get());
  if (readyPromise) refs.push_back(readyPromise.get());
  for (const auto& req : writeRequests) {
    if (req.promise) refs.push_back(req.promise.get());
  }
}

// ===========================================================================
// WritableStream implementation
// ===========================================================================

GCPtr<WritableStreamDefaultWriter> WritableStream::getWriter() {
  if (locked) {
    return {};  // Stream already locked
  }

  auto writerPtr = GarbageCollector::makeGC<WritableStreamDefaultWriter>(GCPtr<WritableStream>(const_cast<WritableStream*>(this)));
  writer = writerPtr;
  locked = true;

  return GCPtr<WritableStreamDefaultWriter>(writerPtr);
}

GCPtr<Promise> WritableStream::abort(const Value& reason) {
  auto promise = GarbageCollector::makeGC<Promise>();

  if (state == WritableStreamState::Closed) {
    promise->resolve(Undefined{});
    return GCPtr<Promise>(promise);
  }

  if (state == WritableStreamState::Errored) {
    promise->reject(*storedError);
    return GCPtr<Promise>(promise);
  }

  // Set error state
  state = WritableStreamState::Errored;
  storedError = std::make_shared<Value>(reason);

  // Clear the queue
  if (controller) {
    controller->queue.clear();
  }

  // Call abort callback if provided
  if (controller && controller->abortCallback) {
    // TODO: Call abort callback when integrated with interpreter
  }

  promise->resolve(Undefined{});
  return GCPtr<Promise>(promise);
}

GCPtr<Promise> WritableStream::close() {
  auto promise = GarbageCollector::makeGC<Promise>();

  if (state == WritableStreamState::Closed) {
    promise->resolve(Undefined{});
    return GCPtr<Promise>(promise);
  }

  if (state == WritableStreamState::Errored) {
    promise->reject(*storedError);
    return GCPtr<Promise>(promise);
  }

  state = WritableStreamState::Closing;

  // Call close callback if provided
  if (controller && controller->closeCallback) {
    // TODO: Call close callback when integrated with interpreter
  }

  state = WritableStreamState::Closed;
  promise->resolve(Undefined{});

  return GCPtr<Promise>(promise);
}

void WritableStream::getReferences(std::vector<GCObject*>& refs) const {
  if (controller) refs.push_back(controller.get());
  auto* writerPtr = writer;
  if (writerPtr) refs.push_back(writerPtr);
  if (pendingAbortRequest) refs.push_back(pendingAbortRequest.get());
  if (closeRequest) refs.push_back(closeRequest.get());
  if (inFlightWriteRequest) refs.push_back(inFlightWriteRequest.get());
  if (inFlightCloseRequest) refs.push_back(inFlightCloseRequest.get());
}

// ===========================================================================
// TransformStreamDefaultController implementation
// ===========================================================================

void TransformStreamDefaultController::enqueue(const Value& chunk) {
  auto* streamPtr = stream;
  if (!streamPtr || !streamPtr->readable) {
    return;
  }

  if (streamPtr->readable->controller) {
    streamPtr->readable->controller->enqueue(chunk);
  }
}

void TransformStreamDefaultController::error(const Value& reason) {
  auto* streamPtr = stream;
  if (!streamPtr) {
    return;
  }

  // Error both sides
  if (streamPtr->readable && streamPtr->readable->controller) {
    streamPtr->readable->controller->error(reason);
  }
  if (streamPtr->writable && streamPtr->writable->controller) {
    streamPtr->writable->controller->error(reason);
  }
}

void TransformStreamDefaultController::terminate() {
  auto* streamPtr = stream;
  if (!streamPtr) {
    return;
  }

  // Close the readable side
  if (streamPtr->readable && streamPtr->readable->controller) {
    streamPtr->readable->controller->close();
  }
}

double TransformStreamDefaultController::desiredSize() const {
  auto* streamPtr = stream;
  if (!streamPtr || !streamPtr->readable || !streamPtr->readable->controller) {
    return 0.0;
  }
  return streamPtr->readable->controller->desiredSize;
}

void TransformStreamDefaultController::getReferences(std::vector<GCObject*>& refs) const {
  if (transformCallback) refs.push_back(transformCallback.get());
  if (flushCallback) refs.push_back(flushCallback.get());
}

// ===========================================================================
// TransformStream implementation
// ===========================================================================

void TransformStream::getReferences(std::vector<GCObject*>& refs) const {
  if (readable) refs.push_back(readable.get());
  if (writable) refs.push_back(writable.get());
  if (controller) refs.push_back(controller.get());
  if (backpressureChangePromise) refs.push_back(backpressureChangePromise.get());
}

// ===========================================================================
// Helper functions for stream creation
// ===========================================================================

GCPtr<ReadableStream> createReadableStream(
    GCPtr<Function> start,
    GCPtr<Function> pull,
    GCPtr<Function> cancel,
    double highWaterMark) {

  auto stream = GarbageCollector::makeGC<ReadableStream>();
  auto controller = std::make_shared<ReadableStreamDefaultController>();

  controller->stream = stream;
  controller->desiredSize = highWaterMark;
  controller->pullCallback = pull;
  controller->cancelCallback = cancel;

  stream->controller = controller;

  // TODO: Call start callback when integrated with interpreter

  return GCPtr<ReadableStream>(stream);
}

GCPtr<WritableStream> createWritableStream(
    GCPtr<Function> start,
    GCPtr<Function> write,
    GCPtr<Function> close,
    GCPtr<Function> abort,
    double highWaterMark) {

  auto stream = GarbageCollector::makeGC<WritableStream>();
  auto controller = std::make_shared<WritableStreamDefaultController>();

  controller->stream = stream;
  controller->desiredSize = highWaterMark;
  controller->writeCallback = write;
  controller->closeCallback = close;
  controller->abortCallback = abort;

  stream->controller = controller;

  // TODO: Call start callback when integrated with interpreter

  return GCPtr<WritableStream>(stream);
}

GCPtr<TransformStream> createTransformStream(
    GCPtr<Function> start,
    GCPtr<Function> transform,
    GCPtr<Function> flush) {

  auto transformStream = GarbageCollector::makeGC<TransformStream>();
  auto controller = std::make_shared<TransformStreamDefaultController>();

  controller->stream = transformStream;
  controller->transformCallback = transform;
  controller->flushCallback = flush;

  transformStream->controller = controller;

  // Create readable side
  transformStream->readable = GarbageCollector::makeGC<ReadableStream>();
  auto readableController = std::make_shared<ReadableStreamDefaultController>();
  readableController->stream = transformStream->readable;
  transformStream->readable->controller = readableController;

  // Create writable side
  transformStream->writable = GarbageCollector::makeGC<WritableStream>();
  auto writableController = std::make_shared<WritableStreamDefaultController>();
  writableController->stream = transformStream->writable;
  transformStream->writable->controller = writableController;

  // TODO: Call start callback when integrated with interpreter

  return GCPtr<TransformStream>(transformStream);
}

}  // namespace lightjs
