#include "fs.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <chrono>

namespace lightjs {
namespace fs {

// Helper to convert filesystem error to string
static std::string fsErrorMessage(const std::string& operation, const std::string& path,
                                   const std::filesystem::filesystem_error& e) {
  return operation + " failed for '" + path + "': " + e.what();
}

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
    auto array = std::make_shared<TypedArray>(TypedArrayType::Uint8Array, contents.size());
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
    if (auto* str = std::get_if<std::string>(&data.value)) {
      file << *str;
    }
    // Handle TypedArray data
    else if (auto* typedArray = std::get_if<std::shared_ptr<TypedArray>>(&data.value)) {
      for (size_t i = 0; i < (*typedArray)->length; i++) {
        file.put(static_cast<char>((*typedArray)->getElement(i)));
      }
    }
    // Handle ArrayBuffer data
    else if (auto* arrayBuffer = std::get_if<std::shared_ptr<ArrayBuffer>>(&data.value)) {
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

    if (auto* str = std::get_if<std::string>(&data.value)) {
      file << *str;
    } else {
      file << data.toString();
    }

  } catch (const std::exception& e) {
    throw std::runtime_error("appendFileSync failed for '" + path + "': " + e.what());
  }
}

bool existsSync(const std::string& path) {
  return std::filesystem::exists(path);
}

void unlinkSync(const std::string& path) {
  try {
    std::filesystem::remove(path);
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error(fsErrorMessage("unlinkSync", path, e));
  }
}

void mkdirSync(const std::string& path, bool recursive) {
  try {
    if (recursive) {
      std::filesystem::create_directories(path);
    } else {
      std::filesystem::create_directory(path);
    }
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error(fsErrorMessage("mkdirSync", path, e));
  }
}

void rmdirSync(const std::string& path, bool recursive) {
  try {
    if (recursive) {
      std::filesystem::remove_all(path);
    } else {
      std::filesystem::remove(path);
    }
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error(fsErrorMessage("rmdirSync", path, e));
  }
}

Value readdirSync(const std::string& path) {
  try {
    auto arr = std::make_shared<Array>();

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
      arr->elements.push_back(Value(entry.path().filename().string()));
    }

    return Value(arr);
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error(fsErrorMessage("readdirSync", path, e));
  }
}

Value statSync(const std::string& path) {
  try {
    auto stats = std::make_shared<Object>();

    auto status = std::filesystem::status(path);
    auto fileSize = std::filesystem::file_size(path);
    auto modTime = std::filesystem::last_write_time(path);

    stats->properties["size"] = Value(static_cast<double>(fileSize));
    stats->properties["isFile"] = Value(std::filesystem::is_regular_file(path));
    stats->properties["isDirectory"] = Value(std::filesystem::is_directory(path));
    stats->properties["isSymbolicLink"] = Value(std::filesystem::is_symlink(path));

    // Convert file time to Unix timestamp (milliseconds)
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
      modTime - std::filesystem::file_time_type::clock::now() +
      std::chrono::system_clock::now()
    );
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      sctp.time_since_epoch()
    ).count();
    stats->properties["mtimeMs"] = Value(static_cast<double>(ms));

    return Value(stats);
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error(fsErrorMessage("statSync", path, e));
  }
}

void copyFileSync(const std::string& src, const std::string& dest) {
  try {
    std::filesystem::copy_file(src, dest, std::filesystem::copy_options::overwrite_existing);
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error("copyFileSync failed: " + std::string(e.what()));
  }
}

void renameSync(const std::string& oldPath, const std::string& newPath) {
  try {
    std::filesystem::rename(oldPath, newPath);
  } catch (const std::filesystem::filesystem_error& e) {
    throw std::runtime_error("renameSync failed: " + std::string(e.what()));
  }
}

// Asynchronous implementations (return Promises)

Value readFile(const std::string& path, const std::string& encoding) {
  auto promise = std::make_shared<Promise>();

  try {
    Value result = readFileSync(path, encoding);
    promise->state = PromiseState::Fulfilled;
    promise->result = std::make_shared<Value>(result);
  } catch (const std::exception& e) {
    promise->state = PromiseState::Rejected;
    auto error = std::make_shared<Error>(ErrorType::Error);
    error->message = e.what();
    promise->result = std::make_shared<Value>(std::shared_ptr<Error>(error));
  }

  return Value(promise);
}

Value writeFile(const std::string& path, const Value& data) {
  auto promise = std::make_shared<Promise>();

  try {
    writeFileSync(path, data);
    promise->state = PromiseState::Fulfilled;
    promise->result = std::make_shared<Value>(Undefined{});
  } catch (const std::exception& e) {
    promise->state = PromiseState::Rejected;
    auto error = std::make_shared<Error>(ErrorType::Error);
    error->message = e.what();
    promise->result = std::make_shared<Value>(std::shared_ptr<Error>(error));
  }

  return Value(promise);
}

Value appendFile(const std::string& path, const Value& data) {
  auto promise = std::make_shared<Promise>();

  try {
    appendFileSync(path, data);
    promise->state = PromiseState::Fulfilled;
    promise->result = std::make_shared<Value>(Undefined{});
  } catch (const std::exception& e) {
    promise->state = PromiseState::Rejected;
    auto error = std::make_shared<Error>(ErrorType::Error);
    error->message = e.what();
    promise->result = std::make_shared<Value>(std::shared_ptr<Error>(error));
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
      if (auto* obj = std::get_if<std::shared_ptr<Object>>(&args[1].value)) {
        auto it = (*obj)->properties.find("recursive");
        if (it != (*obj)->properties.end()) {
          recursive = it->second.isTruthy();
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
      if (auto* obj = std::get_if<std::shared_ptr<Object>>(&args[1].value)) {
        auto it = (*obj)->properties.find("recursive");
        if (it != (*obj)->properties.end()) {
          recursive = it->second.isTruthy();
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
