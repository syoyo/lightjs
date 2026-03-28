#include "module_internal.h"

namespace lightjs {

thread_local std::vector<Module*> g_evalStack;

void Module::ensureEvaluationPromise() {
  if (!topLevelPromise_) {
    topLevelPromise_ = GarbageCollector::makeGC<Promise>();
  }
}

bool Module::readyForSyncExecution(std::unordered_set<const Module*>& seen) const {
  if (!seen.insert(this).second) {
    return true;
  }
  if (state_ == State::Evaluated) {
    return true;
  }
  if (state_ == State::Evaluating || state_ == State::EvaluatingAsync) {
    return false;
  }
  if (isAsync_) {
    return false;
  }
  if (!ast_) {
    return true;
  }

  for (const auto& stmt : ast_->body) {
    if (!stmt) {
      continue;
    }
    if (auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node)) {
      ModuleType depType = moduleTypeFromAttributes(importDecl->attributes);
      auto depIt = sourceDependencies_.find(dependencyCacheKey(importDecl->source, depType));
      if (depIt != sourceDependencies_.end() && depIt->second &&
          !depIt->second->readyForSyncExecution(seen)) {
        return false;
      }
    } else if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
      if (!exportNamed->source) {
        continue;
      }
      ModuleType depType = moduleTypeFromAttributes(exportNamed->attributes);
      auto depIt = sourceDependencies_.find(dependencyCacheKey(*exportNamed->source, depType));
      if (depIt != sourceDependencies_.end() && depIt->second &&
          !depIt->second->readyForSyncExecution(seen)) {
        return false;
      }
    } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
      ModuleType depType = moduleTypeFromAttributes(exportAll->attributes);
      auto depIt = sourceDependencies_.find(dependencyCacheKey(exportAll->source, depType));
      if (depIt != sourceDependencies_.end() && depIt->second &&
          !depIt->second->readyForSyncExecution(seen)) {
        return false;
      }
    }
  }
  return true;
}

void Module::scheduleResume(Interpreter* interpreter) {
  if (resumeScheduled_) {
    return;
  }
  resumeScheduled_ = true;
  auto weakSelf = weak_from_this();
  EventLoopContext::instance().getLoop().queueMicrotask([weakSelf, interpreter]() {
    if (auto module = weakSelf.lock()) {
      module->resumeScheduled_ = false;
      module->resumeEvaluation(interpreter);
    }
  });
}

void Module::notifyAsyncParents(Interpreter* interpreter) {
  for (auto it = asyncParents_.begin(); it != asyncParents_.end(); ++it) {
    auto parent = it->lock();
    if (!parent) {
      continue;
    }
    if (parent->pendingAsyncDeps_ > 0) {
      parent->pendingAsyncDeps_--;
    }
    if (parent->waitingOnAsyncDeps_ && parent->pendingAsyncDeps_ == 0) {
      parent->waitingOnAsyncDeps_ = false;
      parent->scheduleResume(interpreter);
    }
  }
  asyncParents_.clear();
}

void Module::propagateAsyncFailure(const Value& reason, Interpreter* interpreter) {
  std::unordered_set<Module*> visited;
  std::function<void(Module*)> propagate = [&](Module* module) {
    if (!module) {
      return;
    }
    for (const auto& weakParent : module->asyncParents_) {
      auto parent = weakParent.lock();
      if (!parent) {
        continue;
      }
      if (!visited.insert(parent.get()).second) {
        continue;
      }
      parent->lastError_ = reason;
      parent->state_ = State::Instantiated;
      parent->pendingAsyncDeps_ = 0;
      parent->waitingOnAsyncDeps_ = false;
      parent->waitingOnAwait_ = false;
      parent->ensureEvaluationPromise();
      if (parent->topLevelPromise_ && parent->topLevelPromise_->state == PromiseState::Pending) {
        parent->topLevelPromise_->reject(reason);
      }
      propagate(parent.get());
    }
    module->asyncParents_.clear();
  };

  propagate(this);
  if (interpreter) {
    interpreter->clearError();
  }
}

bool Module::resumeEvaluation(Interpreter* interpreter) {
  if (!importsBound_) {
    if (!rebuildIndirectExports()) {
      state_ = State::Instantiated;
      return false;
    }

    for (const auto& stmt : ast_->body) {
      auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node);
      if (!importDecl) {
        continue;
      }

      ModuleType depType = moduleTypeFromAttributes(importDecl->attributes);
      auto depIt = sourceDependencies_.find(
        dependencyCacheKey(importDecl->source, depType));
      if (depIt == sourceDependencies_.end() || !depIt->second) {
        lastError_ = makeErrorValue(ErrorType::SyntaxError, "Unable to resolve import '" + importDecl->source + "'");
        state_ = State::Instantiated;
        return false;
      }
      auto importedModule = depIt->second;

      if (importDecl->defaultImport) {
        if (!importedModule->hasExport("default")) {
          lastError_ = makeErrorValue(
            ErrorType::SyntaxError,
            "Module '" + importDecl->source + "' does not export 'default'"
          );
          state_ = State::Instantiated;
          return false;
        }
        ModuleBinding binding{importedModule, "default"};
        environment_->define(importDecl->defaultImport->name, Value(binding), true);
      }

      if (importDecl->namespaceImport) {
        auto namespaceObject =
          importedModule->getNamespaceObject(importDecl->phase == ImportDeclaration::Phase::Defer);
        if (importDecl->phase == ImportDeclaration::Phase::Defer &&
            importedModule->getState() != Module::State::Evaluated) {
          auto deferredEvalFn = GarbageCollector::makeGC<Function>();
          deferredEvalFn->isNative = true;
          deferredEvalFn->properties["__throw_on_new__"] = Value(true);
          auto deferredModule = importedModule;
          deferredEvalFn->nativeFunc = [deferredModule](const std::vector<Value>&) -> Value {
            if (deferredModule->getState() == Module::State::Evaluated) {
              return Value(Undefined{});
            }
            if (deferredModule->hasStartedEvaluation()) {
              if (auto error = deferredModule->getLastError()) {
                throw JsValueException(*error);
              }
            }
            std::unordered_set<const Module*> seen;
            if (!deferredModule->readyForSyncExecution(seen)) {
              throw JsValueException(
                makeErrorValue(ErrorType::TypeError, "Deferred module is not ready for synchronous evaluation"));
            }
            Interpreter* activeInterpreter = getGlobalInterpreter();
            if (!activeInterpreter) {
              throw std::runtime_error("Error: Interpreter unavailable for deferred module evaluation");
            }
            if (!deferredModule->evaluate(activeInterpreter)) {
              if (auto error = deferredModule->getLastError()) {
                throw JsValueException(*error);
              }
              throw std::runtime_error("Error: Failed to evaluate deferred module");
            }
            return Value(Undefined{});
          };
          namespaceObject->properties["__deferred_pending__"] = Value(true);
          namespaceObject->properties["__deferred_eval__"] = Value(deferredEvalFn);
        }
        environment_->define(
          importDecl->namespaceImport->name,
          Value(namespaceObject),
          true);
      }

      for (const auto& spec : importDecl->specifiers) {
        if (!importedModule->hasExport(spec.imported.name)) {
          lastError_ = makeErrorValue(
            ErrorType::SyntaxError,
            "Module '" + importDecl->source + "' does not export '" + spec.imported.name + "'"
          );
          state_ = State::Instantiated;
          return false;
        }
        ModuleBinding binding{importedModule, spec.imported.name};
        environment_->define(spec.local.name, Value(binding), true);
      }
    }
    importsBound_ = true;
  }

  return evaluateBody(interpreter);
}

bool Module::evaluateBody(Interpreter* interpreter) {
  auto prevEnv = interpreter->getEnvironment();
  bool prevStrictMode = interpreter->strictMode_;
  struct ModuleEvalScopeGuard {
    Interpreter* interpreter;
    GCPtr<Environment> prevEnv;
    bool prevStrictMode;
    ~ModuleEvalScopeGuard() {
      interpreter->setEnvironment(prevEnv);
      interpreter->strictMode_ = prevStrictMode;
    }
  } scopeGuard{interpreter, prevEnv, prevStrictMode};

  interpreter->setEnvironment(environment_);
  interpreter->strictMode_ = true;
  state_ = State::Evaluating;

  interpreter->hoistVarDeclarations(ast_->body);

  try {
    while (nextStatementIndex_ < ast_->body.size()) {
      const auto& stmtPtr = ast_->body[nextStatementIndex_];
      if (!stmtPtr) {
        nextStatementIndex_++;
        continue;
      }
      const auto& stmt = *stmtPtr;

      if (std::holds_alternative<FunctionDeclaration>(stmt.node)) {
        nextStatementIndex_++;
        continue;
      }
      if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
        if (exportNamed->declaration &&
            std::holds_alternative<FunctionDeclaration>(exportNamed->declaration->node)) {
          nextStatementIndex_++;
          continue;
        }
      }
      if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
        if (exportDefault->isHoistableDeclaration && exportDefault->declaration) {
          if (auto* fnExpr = std::get_if<FunctionExpr>(&exportDefault->declaration->node)) {
            if (!fnExpr->isArrow) {
              nextStatementIndex_++;
              continue;
            }
          }
        }
      }

      const AwaitExpr* awaitExpr = nullptr;
      if (isAsync_ && isAwaitExpressionStatement(stmt, &awaitExpr)) {
        Value argValue = Value(Undefined{});
        if (awaitExpr && awaitExpr->argument) {
          auto argTask = interpreter->evaluate(*awaitExpr->argument);
          while (!argTask.done()) {
            argTask.resume();
          }
          argValue = argTask.result();
          if (interpreter->hasError()) {
            lastError_ = interpreter->getError();
            interpreter->clearError();
            interpreter->setEnvironment(prevEnv);
            state_ = State::Instantiated;
            if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
              topLevelPromise_->reject(lastError_.value());
            }
            return false;
          }
        }

        auto promise = coerceToPromise(argValue);
        ensureEvaluationPromise();
        waitingOnAwait_ = true;
        state_ = State::EvaluatingAsync;
        pendingAwaitFulfilled_ = nullptr;
        auto weakSelf = weak_from_this();
        promise->then(
          [weakSelf, interpreter](Value value) -> Value {
            if (auto module = weakSelf.lock()) {
              module->waitingOnAwait_ = false;
              if (module->pendingAwaitFulfilled_) {
                module->pendingAwaitFulfilled_(value);
                module->pendingAwaitFulfilled_ = nullptr;
              }
              module->resumeEvaluation(interpreter);
            }
            return Value(Undefined{});
          },
          [weakSelf, interpreter](Value reason) -> Value {
            if (auto module = weakSelf.lock()) {
              module->waitingOnAwait_ = false;
              module->lastError_ = reason;
              module->state_ = State::Instantiated;
              if (module->topLevelPromise_ && module->topLevelPromise_->state == PromiseState::Pending) {
                module->topLevelPromise_->reject(reason);
              }
              module->propagateAsyncFailure(reason, interpreter);
            }
            return Value(Undefined{});
          }
        );

        nextStatementIndex_++;
        interpreter->setEnvironment(prevEnv);
        return true;
      }

      if (isAsync_ && isAwaitExportDefault(stmt, &awaitExpr)) {
        Value argValue = Value(Undefined{});
        if (awaitExpr && awaitExpr->argument) {
          auto argTask = interpreter->evaluate(*awaitExpr->argument);
          while (!argTask.done()) {
            argTask.resume();
          }
          argValue = argTask.result();
          if (interpreter->hasError()) {
            lastError_ = interpreter->getError();
            interpreter->clearError();
            interpreter->setEnvironment(prevEnv);
            state_ = State::Instantiated;
            if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
              topLevelPromise_->reject(lastError_.value());
            }
            return false;
          }
        }

        auto promise = coerceToPromise(argValue);
        ensureEvaluationPromise();
        waitingOnAwait_ = true;
        state_ = State::EvaluatingAsync;
        pendingAwaitFulfilled_ = [this](const Value& value) {
          Value result = value;
          setDefaultExportNameIfNeeded(result);
          auto bindingIt = exportBindings_.find("default");
          if (bindingIt != exportBindings_.end()) {
            environment_->define(bindingIt->second, result);
          } else {
            defaultExport_ = result;
            exports_["default"] = result;
          }
        };

        auto weakSelf = weak_from_this();
        promise->then(
          [weakSelf, interpreter](Value value) -> Value {
            if (auto module = weakSelf.lock()) {
              module->waitingOnAwait_ = false;
              if (module->pendingAwaitFulfilled_) {
                module->pendingAwaitFulfilled_(value);
                module->pendingAwaitFulfilled_ = nullptr;
              }
              module->resumeEvaluation(interpreter);
            }
            return Value(Undefined{});
          },
          [weakSelf, interpreter](Value reason) -> Value {
            if (auto module = weakSelf.lock()) {
              module->waitingOnAwait_ = false;
              module->lastError_ = reason;
              module->state_ = State::Instantiated;
              if (module->topLevelPromise_ && module->topLevelPromise_->state == PromiseState::Pending) {
                module->topLevelPromise_->reject(reason);
              }
              module->propagateAsyncFailure(reason, interpreter);
            }
            return Value(Undefined{});
          }
        );

        nextStatementIndex_++;
        interpreter->setEnvironment(prevEnv);
        return true;
      }

      if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
        if (exportNamed->declaration) {
          auto task = interpreter->evaluate(*exportNamed->declaration);
          while (!task.done()) {
            task.resume();
          }
          if (interpreter->hasError()) {
            lastError_ = interpreter->getError();
            interpreter->clearError();
            interpreter->setEnvironment(prevEnv);
            state_ = State::Instantiated;
            if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
              topLevelPromise_->reject(lastError_.value());
            }
            return false;
          }
        }
      } else if (std::holds_alternative<ExportDefaultDeclaration>(stmt.node)) {
        auto task = interpreter->evaluate(stmt);
        Value result;
        while (!task.done()) {
          task.resume();
        }
        result = task.result();
        if (interpreter->hasError()) {
          lastError_ = interpreter->getError();
          interpreter->clearError();
          interpreter->setEnvironment(prevEnv);
          state_ = State::Instantiated;
          if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
            topLevelPromise_->reject(lastError_.value());
          }
          return false;
        }
        setDefaultExportNameIfNeeded(result);
        auto bindingIt = exportBindings_.find("default");
        if (bindingIt != exportBindings_.end()) {
          environment_->define(bindingIt->second, result);
        } else {
          defaultExport_ = result;
          exports_["default"] = result;
        }
      } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt.node)) {
        (void)exportAll;
      } else if (std::holds_alternative<ImportDeclaration>(stmt.node)) {
      } else {
        auto task = interpreter->evaluate(stmt);
        while (!task.done()) {
          task.resume();
        }
        if (interpreter->hasError()) {
          lastError_ = interpreter->getError();
          interpreter->clearError();
          interpreter->setEnvironment(prevEnv);
          state_ = State::Instantiated;
          if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
            topLevelPromise_->reject(lastError_.value());
          }
          return false;
        }
      }

      nextStatementIndex_++;
    }
  } catch (const std::exception& e) {
    std::cerr << "Module evaluation error in " << path_ << ": " << e.what() << std::endl;
    lastError_ = makeErrorFromExceptionMessage(e.what());
    interpreter->setEnvironment(prevEnv);
    state_ = State::Instantiated;
    if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
      topLevelPromise_->reject(lastError_.value());
    }
    return false;
  }

  interpreter->setEnvironment(prevEnv);
  state_ = State::Evaluated;
  lastError_.reset();
  if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
    topLevelPromise_->resolve(Value(Undefined{}));
  }
  notifyAsyncParents(interpreter);
  return true;
}

bool Module::evaluate(Interpreter* interpreter) {
  lastError_.reset();
  if (state_ == State::Evaluated || state_ == State::Evaluating || state_ == State::EvaluatingAsync) {
    return true;
  }

  if (state_ != State::Instantiated) {
    std::cerr << "Module must be instantiated before evaluation" << std::endl;
    lastError_ = makeErrorValue(ErrorType::Error, "Module must be instantiated before evaluation");
    return false;
  }

  if (type_ != ModuleType::JavaScript) {
    if (type_ == ModuleType::Bytes && !defaultExport_.has_value()) {
      std::vector<uint8_t> bytes(source_.begin(), source_.end());
      auto buffer = GarbageCollector::makeGC<ArrayBuffer>(bytes);
      GarbageCollector::instance().reportAllocation(sizeof(ArrayBuffer));
      buffer->immutable = true;
      buffer->properties["immutable"] = Value(true);
      buffer->properties["__non_writable_immutable"] = Value(true);
      buffer->properties["__non_enum_immutable"] = Value(true);

      auto view = GarbageCollector::makeGC<TypedArray>(
        TypedArrayType::Uint8, buffer, 0, buffer->byteLength);
      GarbageCollector::instance().reportAllocation(sizeof(TypedArray));
      buffer->views.push_back(view);

      auto wireBuiltinPrototype = [&](const char* ctorName, auto& target) {
        if (!interpreter) {
          return;
        }
        auto ctor = interpreter->getEnvironment()->get(ctorName);
        if (!ctor || !ctor->isFunction()) {
          return;
        }
        target->properties["constructor"] = *ctor;
        target->properties["__constructor__"] = *ctor;
        auto prototypeIt = ctor->getGC<Function>()->properties.find("prototype");
        if (prototypeIt != ctor->getGC<Function>()->properties.end()) {
          target->properties["__proto__"] = prototypeIt->second;
        }
      };

      wireBuiltinPrototype("ArrayBuffer", buffer);
      wireBuiltinPrototype("Uint8Array", view);
      defaultExport_ = Value(view);
    }

    if (!defaultExport_.has_value()) {
      lastError_ = makeErrorValue(ErrorType::SyntaxError, "Synthetic module has no default export");
      return false;
    }

    exports_.clear();
    exports_["default"] = *defaultExport_;
    state_ = State::Evaluated;
    lastError_.reset();
    return true;
  }

  if (!evaluationInitialized_) {
    evaluationInitialized_ = true;
    exports_.clear();
    defaultExport_.reset();
    ambiguousReExports_.clear();
    exportBindings_.clear();
    importsBound_ = false;
    waitingOnAsyncDeps_ = false;
    waitingOnAwait_ = false;
    pendingAsyncDeps_ = 0;
    nextStatementIndex_ = 0;
    asyncParents_.clear();
    cycleRoot_ = weak_from_this();
    if (!initializeDeclaredExports(interpreter)) {
      state_ = State::Instantiated;
      return false;
    }
    state_ = State::Evaluating;

    struct EvalStackGuard {
      std::vector<Module*>& stack;
      Module* module;
      EvalStackGuard(std::vector<Module*>& s, Module* m) : stack(s), module(m) { stack.push_back(m); }
      ~EvalStackGuard() {
        if (!stack.empty() && stack.back() == module) {
          stack.pop_back();
        }
      }
    } stackGuard(g_evalStack, this);

    bool prevSuppress = interpreter->suppressMicrotasks();
    interpreter->setSuppressMicrotasks(true);
    std::unordered_set<Module*> waitingOn;

    auto appendAsyncTransitiveDependencies =
      [&](const std::shared_ptr<Module>& root,
          auto&& appendAsyncTransitiveDependenciesRef,
          std::unordered_set<Module*>& seen,
          std::unordered_set<Module*>& queued,
          std::vector<std::shared_ptr<Module>>& out) -> void {
        if (!root || !seen.insert(root.get()).second) {
          return;
        }
        if (root->state_ == State::Evaluating ||
            root->state_ == State::EvaluatingAsync ||
            root->state_ == State::Evaluated) {
          return;
        }
        if (root->isAsync_) {
          if (queued.insert(root.get()).second) {
            out.push_back(root);
          }
          return;
        }
        if (!root->ast_) {
          return;
        }
        for (const auto& stmt : root->ast_->body) {
          if (!stmt) {
            continue;
          }
          if (auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node)) {
            ModuleType depType = moduleTypeFromAttributes(importDecl->attributes);
            auto depIt = root->sourceDependencies_.find(
              dependencyCacheKey(importDecl->source, depType));
            if (depIt != root->sourceDependencies_.end() && depIt->second) {
              appendAsyncTransitiveDependenciesRef(
                depIt->second, appendAsyncTransitiveDependenciesRef, seen, queued, out);
            }
          } else if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
            if (!exportNamed->source) {
              continue;
            }
            ModuleType depType = moduleTypeFromAttributes(exportNamed->attributes);
            auto depIt = root->sourceDependencies_.find(
              dependencyCacheKey(*exportNamed->source, depType));
            if (depIt != root->sourceDependencies_.end() && depIt->second) {
              appendAsyncTransitiveDependenciesRef(
                depIt->second, appendAsyncTransitiveDependenciesRef, seen, queued, out);
            }
          } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
            ModuleType depType = moduleTypeFromAttributes(exportAll->attributes);
            auto depIt = root->sourceDependencies_.find(
              dependencyCacheKey(exportAll->source, depType));
            if (depIt != root->sourceDependencies_.end() && depIt->second) {
              appendAsyncTransitiveDependenciesRef(
                depIt->second, appendAsyncTransitiveDependenciesRef, seen, queued, out);
            }
          }
        }
      };

    std::vector<std::shared_ptr<Module>> evaluationList;
    std::unordered_set<Module*> queuedDependencies;
    for (const auto& stmt : ast_->body) {
      if (!stmt) {
        continue;
      }
      if (auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node)) {
        ModuleType depType = moduleTypeFromAttributes(importDecl->attributes);
        auto depIt = sourceDependencies_.find(dependencyCacheKey(importDecl->source, depType));
        if (depIt == sourceDependencies_.end() || !depIt->second) {
          continue;
        }
        if (importDecl->phase == ImportDeclaration::Phase::Defer) {
          std::unordered_set<Module*> seenAsync;
          appendAsyncTransitiveDependencies(
            depIt->second, appendAsyncTransitiveDependencies, seenAsync, queuedDependencies, evaluationList);
        } else if (queuedDependencies.insert(depIt->second.get()).second) {
          evaluationList.push_back(depIt->second);
        }
      } else if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
        if (!exportNamed->source) {
          continue;
        }
        ModuleType depType = moduleTypeFromAttributes(exportNamed->attributes);
        auto depIt = sourceDependencies_.find(dependencyCacheKey(*exportNamed->source, depType));
        if (depIt != sourceDependencies_.end() && depIt->second &&
            queuedDependencies.insert(depIt->second.get()).second) {
          evaluationList.push_back(depIt->second);
        }
      } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
        ModuleType depType = moduleTypeFromAttributes(exportAll->attributes);
        auto depIt = sourceDependencies_.find(dependencyCacheKey(exportAll->source, depType));
        if (depIt != sourceDependencies_.end() && depIt->second &&
            queuedDependencies.insert(depIt->second.get()).second) {
          evaluationList.push_back(depIt->second);
        }
      }
    }

    for (const auto& dep : evaluationList) {
      if (!dep->evaluate(interpreter)) {
        interpreter->setSuppressMicrotasks(prevSuppress);
        state_ = State::Instantiated;
        if (auto depError = dep->getLastError()) {
          lastError_ = *depError;
        } else {
          lastError_ = makeErrorValue(ErrorType::Error, "Failed to evaluate dependency");
        }
        return false;
      }

      if (dep->state_ == State::Evaluating) {
        auto depRoot = dep->cycleRoot_.lock();
        if (depRoot) {
          cycleRoot_ = depRoot;
        } else {
          cycleRoot_ = dep;
        }
      }

      if (dep->state_ == State::EvaluatingAsync) {
        std::shared_ptr<Module> waitModule = dep;
        auto depRoot = dep->cycleRoot_.lock();
        auto thisRoot = cycleRoot_.lock();
        if (depRoot && thisRoot && depRoot != thisRoot) {
          waitModule = depRoot;
        }
        if (waitModule.get() != this && waitingOn.insert(waitModule.get()).second) {
          pendingAsyncDeps_++;
          waitModule->asyncParents_.push_back(weak_from_this());
        }
      }
    }
    interpreter->setSuppressMicrotasks(prevSuppress);

    if (pendingAsyncDeps_ > 0) {
      waitingOnAsyncDeps_ = true;
      state_ = State::EvaluatingAsync;
      ensureEvaluationPromise();
      return true;
    }
  }

  if (isAsync_ || pendingAsyncDeps_ > 0) {
    ensureEvaluationPromise();
  }

  return resumeEvaluation(interpreter);
}

}  // namespace lightjs
