#include "fs.h"
#include "fs_compat.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <chrono>

namespace lightjs {
namespace fs {

// Synchronous implementations

Value readFileSync(const std::string& path, const std::string& encoding) {
  try {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("Cannot open file: " + path);
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string contents = buffer.str();

    // If encoding specified, return as string
    if (!encoding.empty() && (encoding == "utf8" || encoding == "utf-8")) {
      return Value(contents);
    }

    // Otherwise return as Uint8Array
    auto array = std::make_shared<TypedArray>(TypedArrayType::Uint8, contents.size());
    for (size_t i = 0; i < contents.size(); i++) {
      array->setElement(i, static_cast<uint8_t>(contents[i]));
    }
    return Value(array);

  } catch (const std::exception& e) {
    throw std::runtime_error("readFileSync failed for '" + path + "': " + e.what());
  }
}

void writeFileSync(const std::string& path, const Value& data) {
  try {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("Cannot open file for writing: " + path);
    }

    // Handle string data
    if (auto* str = std::get_if<std::string>(&data.data)) {
      file << *str;
    }
    // Handle TypedArray data
    else if (auto* typedArray = std::get_if<std::shared_ptr<TypedArray>>(&data.data)) {
      for (size_t i = 0; i < (*typedArray)->length; i++) {
        file.put(static_cast<char>((*typedArray)->getElement(i)));
      }
    }
    // Handle ArrayBuffer data
    else if (auto* arrayBuffer = std::get_if<std::shared_ptr<ArrayBuffer>>(&data.data)) {
      file.write(reinterpret_cast<const char*>((*arrayBuffer)->data.data()),
                 (*arrayBuffer)->byteLength);
    }
    else {
      // Convert to string
      file << data.toString();
    }

  } catch (const std::exception& e) {
    throw std::runtime_error("writeFileSync failed for '" + path + "': " + e.what());
  }
}

void appendFileSync(const std::string& path, const Value& data) {
  try {
    std::ofstream file(path, std::ios::binary | std::ios::app);
    if (!file) {
      throw std::runtime_error("Cannot open file for appending: " + path);
    }

    if (auto* str = std::get_if<std::string>(&data.data)) {
      file << *str;
    } else {
      file << data.toString();
    }

  } catch (const std::exception& e) {
    throw std::runtime_error("appendFileSync failed for '" + path + "': " + e.what());
  }
}

bool existsSync(const std::string& path) {
  return fs_compat::exists(path);
}

void unlinkSync(const std::string& path) {
  if (!fs_compat::remove(path)) {
    throw std::runtime_error("unlinkSync failed for '" + path + "'");
  }
}

void mkdirSync(const std::string& path, bool recursive) {
  bool success;
  if (recursive) {
    success = fs_compat::createDirectories(path);
  } else {
    success = fs_compat::createDirectory(path);
  }
  if (!success) {
    throw std::runtime_error("mkdirSync failed for '" + path + "'");
  }
}

void rmdirSync(const std::string& path, bool recursive) {
  if (recursive) {
    if (fs_compat::removeAll(path) < 0) {
      throw std::runtime_error("rmdirSync (recursive) failed for '" + path + "'");
    }
  } else {
    if (!fs_compat::remove(path)) {
      throw std::runtime_error("rmdirSync failed for '" + path + "'");
    }
  }
}

Value readdirSync(const std::string& path) {
  try {
    auto arr = std::make_shared<Array>();

    auto entries = fs_compat::readDirectory(path);
    for (const auto& entry : entries) {
      arr->elements.push_back(Value(entry.name));
    }

    return Value(arr);
  } catch (const std::exception& e) {
    throw std::runtime_error("readdirSync failed for '" + path + "': " + e.what());
  }
}

Value statSync(const std::string& path) {
  try {
    auto stats = std::make_shared<Object>();

    auto status = fs_compat::getStatus(path);
    if (!status.exists) {
      throw std::runtime_error("ENOENT: no such file or directory");
    }

    stats->properties["size"] = Value(static_cast<double>(status.fileSize));
    stats->properties["isFile"] = Value(status.isFile);
    stats->properties["isDirectory"] = Value(status.isDirectory);
    stats->properties["isSymbolicLink"] = Value(status.isSymlink);

    // Convert to milliseconds
    stats->properties["mtimeMs"] = Value(static_cast<double>(status.lastWriteTime * 1000));

    return Value(stats);
  } catch (const std::exception& e) {
    throw std::runtime_error("statSync failed for '" + path + "': " + e.what());
  }
}

void copyFileSync(const std::string& src, const std::string& dest) {
  if (!fs_compat::copyFile(src, dest, true)) {
    throw std::runtime_error("copyFileSync failed: cannot copy '" + src + "' to '" + dest + "'");
  }
}

void renameSync(const std::string& oldPath, const std::string& newPath) {
  if (!fs_compat::rename(oldPath, newPath)) {
    throw std::runtime_error("renameSync failed: cannot rename '" + oldPath + "' to '" + newPath + "'");
  }
}

// Asynchronous implementations (return Promises)

Value readFile(const std::string& path, const std::string& encoding) {
  auto promise = std::make_shared<Promise>();

  try {
    Value result = readFileSync(path, encoding);
    promise->state = PromiseState::Fulfilled;
    promise->result = result;
  } catch (const std::exception& e) {
    promise->state = PromiseState::Rejected;
    auto error = std::make_shared<Error>(ErrorType::Error);
    error->message = e.what();
    promise->result = Value(error);
  }

  return Value(promise);
}

Value writeFile(const std::string& path, const Value& data) {
  auto promise = std::make_shared<Promise>();

  try {
    writeFileSync(path, data);
    promise->state = PromiseState::Fulfilled;
    promise->result = Value(Undefined{});
  } catch (const std::exception& e) {
    promise->state = PromiseState::Rejected;
    auto error = std::make_shared<Error>(ErrorType::Error);
    error->message = e.what();
    promise->result = Value(error);
  }

  return Value(promise);
}

Value appendFile(const std::string& path, const Value& data) {
  auto promise = std::make_shared<Promise>();

  try {
    appendFileSync(path, data);
    promise->state = PromiseState::Fulfilled;
    promise->result = Value(Undefined{});
  } catch (const std::exception& e) {
    promise->state = PromiseState::Rejected;
    auto error = std::make_shared<Error>(ErrorType::Error);
    error->message = e.what();
    promise->result = Value(error);
  }

  return Value(promise);
}

} // namespace fs

// Create fs module object

std::shared_ptr<Object> createFSModule() {
  auto fsModule = std::make_shared<Object>();

  // readFileSync
  auto readFileSyncFn = std::make_shared<Function>();
  readFileSyncFn->isNative = true;
  readFileSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("readFileSync requires a path argument");
    }
    std::string path = args[0].toString();
    std::string encoding = args.size() > 1 ? args[1].toString() : "";
    return fs::readFileSync(path, encoding);
  };
  fsModule->properties["readFileSync"] = Value(readFileSyncFn);

  // writeFileSync
  auto writeFileSyncFn = std::make_shared<Function>();
  writeFileSyncFn->isNative = true;
  writeFileSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("writeFileSync requires path and data arguments");
    }
    fs::writeFileSync(args[0].toString(), args[1]);
    return Value(Undefined{});
  };
  fsModule->properties["writeFileSync"] = Value(writeFileSyncFn);

  // appendFileSync
  auto appendFileSyncFn = std::make_shared<Function>();
  appendFileSyncFn->isNative = true;
  appendFileSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("appendFileSync requires path and data arguments");
    }
    fs::appendFileSync(args[0].toString(), args[1]);
    return Value(Undefined{});
  };
  fsModule->properties["appendFileSync"] = Value(appendFileSyncFn);

  // existsSync
  auto existsSyncFn = std::make_shared<Function>();
  existsSyncFn->isNative = true;
  existsSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    return Value(fs::existsSync(args[0].toString()));
  };
  fsModule->properties["existsSync"] = Value(existsSyncFn);

  // unlinkSync
  auto unlinkSyncFn = std::make_shared<Function>();
  unlinkSyncFn->isNative = true;
  unlinkSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("unlinkSync requires a path argument");
    }
    fs::unlinkSync(args[0].toString());
    return Value(Undefined{});
  };
  fsModule->properties["unlinkSync"] = Value(unlinkSyncFn);

  // mkdirSync
  auto mkdirSyncFn = std::make_shared<Function>();
  mkdirSyncFn->isNative = true;
  mkdirSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("mkdirSync requires a path argument");
    }
    bool recursive = false;
    if (args.size() > 1) {
      if (auto* obj = std::get_if<std::shared_ptr<Object>>(&args[1].data)) {
        auto it = (*obj)->properties.find("recursive");
        if (it != (*obj)->properties.end()) {
          recursive = it->second.toBool();
        }
      }
    }
    fs::mkdirSync(args[0].toString(), recursive);
    return Value(Undefined{});
  };
  fsModule->properties["mkdirSync"] = Value(mkdirSyncFn);

  // rmdirSync
  auto rmdirSyncFn = std::make_shared<Function>();
  rmdirSyncFn->isNative = true;
  rmdirSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("rmdirSync requires a path argument");
    }
    bool recursive = false;
    if (args.size() > 1) {
      if (auto* obj = std::get_if<std::shared_ptr<Object>>(&args[1].data)) {
        auto it = (*obj)->properties.find("recursive");
        if (it != (*obj)->properties.end()) {
          recursive = it->second.toBool();
        }
      }
    }
    fs::rmdirSync(args[0].toString(), recursive);
    return Value(Undefined{});
  };
  fsModule->properties["rmdirSync"] = Value(rmdirSyncFn);

  // readdirSync
  auto readdirSyncFn = std::make_shared<Function>();
  readdirSyncFn->isNative = true;
  readdirSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("readdirSync requires a path argument");
    }
    return fs::readdirSync(args[0].toString());
  };
  fsModule->properties["readdirSync"] = Value(readdirSyncFn);

  // statSync
  auto statSyncFn = std::make_shared<Function>();
  statSyncFn->isNative = true;
  statSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("statSync requires a path argument");
    }
    return fs::statSync(args[0].toString());
  };
  fsModule->properties["statSync"] = Value(statSyncFn);

  // copyFileSync
  auto copyFileSyncFn = std::make_shared<Function>();
  copyFileSyncFn->isNative = true;
  copyFileSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("copyFileSync requires source and destination arguments");
    }
    fs::copyFileSync(args[0].toString(), args[1].toString());
    return Value(Undefined{});
  };
  fsModule->properties["copyFileSync"] = Value(copyFileSyncFn);

  // renameSync
  auto renameSyncFn = std::make_shared<Function>();
  renameSyncFn->isNative = true;
  renameSyncFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("renameSync requires old and new path arguments");
    }
    fs::renameSync(args[0].toString(), args[1].toString());
    return Value(Undefined{});
  };
  fsModule->properties["renameSync"] = Value(renameSyncFn);

  // Async versions (promises submodule)
  auto promises = std::make_shared<Object>();

  // promises.readFile
  auto readFileFn = std::make_shared<Function>();
  readFileFn->isNative = true;
  readFileFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("readFile requires a path argument");
    }
    std::string path = args[0].toString();
    std::string encoding = args.size() > 1 ? args[1].toString() : "";
    return fs::readFile(path, encoding);
  };
  promises->properties["readFile"] = Value(readFileFn);

  // promises.writeFile
  auto writeFileFn = std::make_shared<Function>();
  writeFileFn->isNative = true;
  writeFileFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("writeFile requires path and data arguments");
    }
    return fs::writeFile(args[0].toString(), args[1]);
  };
  promises->properties["writeFile"] = Value(writeFileFn);

  // promises.appendFile
  auto appendFileFn = std::make_shared<Function>();
  appendFileFn->isNative = true;
  appendFileFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      throw std::runtime_error("appendFile requires path and data arguments");
    }
    return fs::appendFile(args[0].toString(), args[1]);
  };
  promises->properties["appendFile"] = Value(appendFileFn);

  fsModule->properties["promises"] = Value(promises);

  return fsModule;
}

} // namespace lightjs
