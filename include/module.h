#pragma once

#include "value.h"
#include "ast.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

namespace lightjs {

class Environment;
class Interpreter;

// Represents a loaded ES6 module
class Module : public std::enable_shared_from_this<Module> {
public:
  // Module states
  enum class State {
    Uninstantiated,
    Instantiating,
    Instantiated,
    Evaluating,
    EvaluatingAsync,
    Evaluated
  };

  Module(const std::string& path, const std::string& source);

  // Parse the module source code
  bool parse();

  // Instantiate the module (resolve imports)
  bool instantiate(class ModuleLoader* loader);

  // Evaluate the module
  bool evaluate(Interpreter* interpreter);

  // Get exported value by name
  std::optional<Value> getExport(const std::string& name) const;

  // Get all exports
  std::unordered_map<std::string, Value> getAllExports() const;

  // Get (and refresh) the module namespace object for this module.
  std::shared_ptr<Object> getNamespaceObject();

  // Module metadata
  std::string getPath() const { return path_; }
  State getState() const { return state_; }
  std::optional<Value> getLastError() const { return lastError_; }
  std::shared_ptr<Promise> getEvaluationPromise() const { return topLevelPromise_; }
  bool isAsyncModule() const { return isAsync_; }

private:
  bool resumeEvaluation(Interpreter* interpreter);
  bool evaluateBody(Interpreter* interpreter);
  void scheduleResume(Interpreter* interpreter);
  void notifyAsyncParents(Interpreter* interpreter);
  void ensureEvaluationPromise();
  void propagateAsyncFailure(const Value& reason, Interpreter* interpreter);

  std::string path_;
  std::string source_;
  State state_;

  // Parsed AST
  std::optional<Program> ast_;

  // Module environment
  std::shared_ptr<Environment> environment_;

  // Exported bindings
  std::unordered_map<std::string, Value> exports_;

  // Default export
  std::optional<Value> defaultExport_;

  // Dependencies (imported modules)
  std::vector<std::shared_ptr<Module>> dependencies_;
  std::unordered_map<std::string, std::shared_ptr<Module>> sourceDependencies_;
  std::unordered_set<std::string> ambiguousReExports_;
  std::unordered_map<std::string, std::string> exportBindings_;
  std::weak_ptr<Object> namespaceObject_;

  // Most recent module-level failure reason.
  std::optional<Value> lastError_;

  bool isAsync_ = false;
  bool evaluationInitialized_ = false;
  bool importsBound_ = false;
  bool waitingOnAsyncDeps_ = false;
  bool waitingOnAwait_ = false;
  bool resumeScheduled_ = false;
  size_t nextStatementIndex_ = 0;
  size_t pendingAsyncDeps_ = 0;
  std::shared_ptr<Promise> topLevelPromise_;
  std::vector<std::weak_ptr<Module>> asyncParents_;
  std::weak_ptr<Module> cycleRoot_;
  std::function<void(const Value&)> pendingAwaitFulfilled_;
};

// Module loader manages module loading and caching
class ModuleLoader {
public:
  ModuleLoader();

  // Load a module by path
  std::shared_ptr<Module> loadModule(const std::string& path);

  // Resolve module path (handles relative paths, node_modules, etc.)
  std::string resolvePath(const std::string& specifier, const std::string& parentPath = "");

  // Get cached module
  std::shared_ptr<Module> getCachedModule(const std::string& path);

  // Set base path for module resolution
  void setBasePath(const std::string& basePath) { basePath_ = basePath; }
  std::optional<Value> getLastError() const { return lastError_; }

private:
  // Read file contents
  std::optional<std::string> readFile(const std::string& path);

  // Normalize file path
  std::string normalizePath(const std::string& path);

  // Module cache
  std::unordered_map<std::string, std::shared_ptr<Module>> cache_;

  // Base path for module resolution
  std::string basePath_;

  // Most recent load failure reason.
  std::optional<Value> lastError_;
};

} // namespace lightjs
