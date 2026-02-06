#include "module.h"
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "event_loop.h"
#include "symbols.h"
#include "fs_compat.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_set>
#include <algorithm>

namespace lightjs {

namespace {
Value makeErrorValue(ErrorType type, const std::string& message) {
  return Value(std::make_shared<Error>(type, message));
}

Value makeErrorFromExceptionMessage(const std::string& message) {
  auto consumePrefix = [&](const std::string& prefix, ErrorType type) -> std::optional<Value> {
    if (message.rfind(prefix, 0) == 0) {
      return makeErrorValue(type, message.substr(prefix.size()));
    }
    return std::nullopt;
  };

  if (auto parsed = consumePrefix("TypeError: ", ErrorType::TypeError)) return *parsed;
  if (auto parsed = consumePrefix("ReferenceError: ", ErrorType::ReferenceError)) return *parsed;
  if (auto parsed = consumePrefix("RangeError: ", ErrorType::RangeError)) return *parsed;
  if (auto parsed = consumePrefix("SyntaxError: ", ErrorType::SyntaxError)) return *parsed;
  if (auto parsed = consumePrefix("URIError: ", ErrorType::URIError)) return *parsed;
  if (auto parsed = consumePrefix("EvalError: ", ErrorType::EvalError)) return *parsed;
  if (auto parsed = consumePrefix("Error: ", ErrorType::Error)) return *parsed;
  return makeErrorValue(ErrorType::Error, message);
}

void collectBoundNamesFromPattern(const ExprPtr& pattern, std::unordered_set<std::string>& names) {
  if (!pattern) {
    return;
  }

  if (auto* assign = std::get_if<AssignmentPattern>(&pattern->node)) {
    collectBoundNamesFromPattern(assign->left, names);
    return;
  }

  if (auto* id = std::get_if<Identifier>(&pattern->node)) {
    if (!id->name.empty()) {
      names.insert(id->name);
    }
    return;
  }

  if (auto* arr = std::get_if<ArrayPattern>(&pattern->node)) {
    for (const auto& element : arr->elements) {
      collectBoundNamesFromPattern(element, names);
    }
    collectBoundNamesFromPattern(arr->rest, names);
    return;
  }

  if (auto* obj = std::get_if<ObjectPattern>(&pattern->node)) {
    for (const auto& prop : obj->properties) {
      collectBoundNamesFromPattern(prop.value, names);
    }
    collectBoundNamesFromPattern(obj->rest, names);
  }
}

bool addLexicalName(const std::string& name,
                    std::unordered_set<std::string>& lexicalNames,
                    std::string& duplicateName) {
  if (name.empty()) {
    return true;
  }
  if (!lexicalNames.insert(name).second) {
    duplicateName = name;
    return false;
  }
  return true;
}

bool collectModuleDeclarationNames(const Statement& stmt,
                                   std::unordered_set<std::string>& lexicalNames,
                                   std::unordered_set<std::string>& varNames,
                                   std::string& duplicateName);

bool collectModuleDeclarationNames(const Statement& stmt,
                                   std::unordered_set<std::string>& lexicalNames,
                                   std::unordered_set<std::string>& varNames,
                                   std::string& duplicateName) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& declarator : varDecl->declarations) {
      std::unordered_set<std::string> boundNames;
      collectBoundNamesFromPattern(declarator.pattern, boundNames);
      if (varDecl->kind == VarDeclaration::Kind::Var) {
        varNames.insert(boundNames.begin(), boundNames.end());
      } else {
        for (const auto& name : boundNames) {
          if (!addLexicalName(name, lexicalNames, duplicateName)) {
            return false;
          }
        }
      }
    }
    return true;
  }

  if (auto* fnDecl = std::get_if<FunctionDeclaration>(&stmt.node)) {
    return addLexicalName(fnDecl->id.name, lexicalNames, duplicateName);
  }

  if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt.node)) {
    return addLexicalName(classDecl->id.name, lexicalNames, duplicateName);
  }

  if (auto* importDecl = std::get_if<ImportDeclaration>(&stmt.node)) {
    if (importDecl->defaultImport &&
        !addLexicalName(importDecl->defaultImport->name, lexicalNames, duplicateName)) {
      return false;
    }
    if (importDecl->namespaceImport &&
        !addLexicalName(importDecl->namespaceImport->name, lexicalNames, duplicateName)) {
      return false;
    }
    for (const auto& specifier : importDecl->specifiers) {
      if (!addLexicalName(specifier.local.name, lexicalNames, duplicateName)) {
        return false;
      }
    }
    return true;
  }

  if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
    if (exportNamed->declaration) {
      return collectModuleDeclarationNames(*exportNamed->declaration, lexicalNames, varNames, duplicateName);
    }
    return true;
  }

  return true;
}

void setDefaultExportNameIfNeeded(Value& result) {
  if (result.isFunction()) {
    auto fn = std::get<std::shared_ptr<Function>>(result.data);
    auto nameIt = fn->properties.find("name");
    bool shouldSet = true;
    if (nameIt != fn->properties.end() && nameIt->second.isString()) {
      shouldSet = std::get<std::string>(nameIt->second.data).empty();
    }
    if (shouldSet) {
      fn->properties["name"] = Value(std::string("default"));
    }
    return;
  }

  if (result.isClass()) {
    auto cls = std::get<std::shared_ptr<Class>>(result.data);
    if (cls->name.empty() && cls->staticMethods.find("name") == cls->staticMethods.end()) {
      cls->name = "default";
    }
  }
}

bool exprContainsTopLevelAwait(const Expression& expr);

bool stmtContainsTopLevelAwait(const Statement& stmt) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& declarator : varDecl->declarations) {
      if (declarator.init && exprContainsTopLevelAwait(*declarator.init)) {
        return true;
      }
    }
    return false;
  }

  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && exprContainsTopLevelAwait(*exprStmt->expression);
  }

  if (auto* returnStmt = std::get_if<ReturnStmt>(&stmt.node)) {
    return returnStmt->argument && exprContainsTopLevelAwait(*returnStmt->argument);
  }

  if (auto* blockStmt = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& child : blockStmt->body) {
      if (child && stmtContainsTopLevelAwait(*child)) {
        return true;
      }
    }
    return false;
  }

  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && exprContainsTopLevelAwait(*ifStmt->test)) {
      return true;
    }
    if (ifStmt->consequent && stmtContainsTopLevelAwait(*ifStmt->consequent)) {
      return true;
    }
    if (ifStmt->alternate && stmtContainsTopLevelAwait(*ifStmt->alternate)) {
      return true;
    }
    return false;
  }

  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    return (whileStmt->test && exprContainsTopLevelAwait(*whileStmt->test)) ||
           (whileStmt->body && stmtContainsTopLevelAwait(*whileStmt->body));
  }

  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    return (doWhileStmt->body && stmtContainsTopLevelAwait(*doWhileStmt->body)) ||
           (doWhileStmt->test && exprContainsTopLevelAwait(*doWhileStmt->test));
  }

  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && stmtContainsTopLevelAwait(*forStmt->init)) {
      return true;
    }
    if (forStmt->test && exprContainsTopLevelAwait(*forStmt->test)) {
      return true;
    }
    if (forStmt->update && exprContainsTopLevelAwait(*forStmt->update)) {
      return true;
    }
    return forStmt->body && stmtContainsTopLevelAwait(*forStmt->body);
  }

  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    return (forInStmt->left && stmtContainsTopLevelAwait(*forInStmt->left)) ||
           (forInStmt->right && exprContainsTopLevelAwait(*forInStmt->right)) ||
           (forInStmt->body && stmtContainsTopLevelAwait(*forInStmt->body));
  }

  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->isAwait) {
      return true;
    }
    return (forOfStmt->left && stmtContainsTopLevelAwait(*forOfStmt->left)) ||
           (forOfStmt->right && exprContainsTopLevelAwait(*forOfStmt->right)) ||
           (forOfStmt->body && stmtContainsTopLevelAwait(*forOfStmt->body));
  }

  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && exprContainsTopLevelAwait(*switchStmt->discriminant)) {
      return true;
    }
    for (const auto& caseClause : switchStmt->cases) {
      if (caseClause.test && exprContainsTopLevelAwait(*caseClause.test)) {
        return true;
      }
      for (const auto& caseStmt : caseClause.consequent) {
        if (caseStmt && stmtContainsTopLevelAwait(*caseStmt)) {
          return true;
        }
      }
    }
    return false;
  }

  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& tryStmtNode : tryStmt->block) {
      if (tryStmtNode && stmtContainsTopLevelAwait(*tryStmtNode)) {
        return true;
      }
    }
    if (tryStmt->hasHandler) {
      for (const auto& handlerStmt : tryStmt->handler.body) {
        if (handlerStmt && stmtContainsTopLevelAwait(*handlerStmt)) {
          return true;
        }
      }
    }
    if (tryStmt->hasFinalizer) {
      for (const auto& finalStmt : tryStmt->finalizer) {
        if (finalStmt && stmtContainsTopLevelAwait(*finalStmt)) {
          return true;
        }
      }
    }
    return false;
  }

  if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
    if (exportNamed->declaration) {
      return stmtContainsTopLevelAwait(*exportNamed->declaration);
    }
    return false;
  }

  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration && exprContainsTopLevelAwait(*exportDefault->declaration);
  }

  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && exprContainsTopLevelAwait(*throwStmt->argument);
  }

  return false;
}

bool exprContainsTopLevelAwait(const Expression& expr) {
  if (std::holds_alternative<AwaitExpr>(expr.node)) {
    return true;
  }

  if (std::holds_alternative<FunctionExpr>(expr.node) ||
      std::holds_alternative<ClassExpr>(expr.node)) {
    return false;
  }

  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return exprContainsTopLevelAwait(*binary->left) || exprContainsTopLevelAwait(*binary->right);
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return exprContainsTopLevelAwait(*unary->argument);
  }
  if (auto* assignment = std::get_if<AssignmentExpr>(&expr.node)) {
    return exprContainsTopLevelAwait(*assignment->left) || exprContainsTopLevelAwait(*assignment->right);
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return exprContainsTopLevelAwait(*update->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (exprContainsTopLevelAwait(*call->callee)) {
      return true;
    }
    for (const auto& arg : call->arguments) {
      if (arg && exprContainsTopLevelAwait(*arg)) {
        return true;
      }
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return exprContainsTopLevelAwait(*member->object) ||
           (member->property && exprContainsTopLevelAwait(*member->property));
  }
  if (auto* conditional = std::get_if<ConditionalExpr>(&expr.node)) {
    return exprContainsTopLevelAwait(*conditional->test) ||
           exprContainsTopLevelAwait(*conditional->consequent) ||
           exprContainsTopLevelAwait(*conditional->alternate);
  }
  if (auto* sequence = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& child : sequence->expressions) {
      if (child && exprContainsTopLevelAwait(*child)) {
        return true;
      }
    }
    return false;
  }
  if (auto* arrayExpr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& element : arrayExpr->elements) {
      if (element && exprContainsTopLevelAwait(*element)) {
        return true;
      }
    }
    return false;
  }
  if (auto* objExpr = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : objExpr->properties) {
      if (prop.key && exprContainsTopLevelAwait(*prop.key)) {
        return true;
      }
      if (prop.value && exprContainsTopLevelAwait(*prop.value)) {
        return true;
      }
    }
    return false;
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && exprContainsTopLevelAwait(*yieldExpr->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && exprContainsTopLevelAwait(*newExpr->callee)) {
      return true;
    }
    for (const auto& arg : newExpr->arguments) {
      if (arg && exprContainsTopLevelAwait(*arg)) {
        return true;
      }
    }
    return false;
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && exprContainsTopLevelAwait(*spread->argument);
  }
  if (auto* arrPattern = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& element : arrPattern->elements) {
      if (element && exprContainsTopLevelAwait(*element)) {
        return true;
      }
    }
    return arrPattern->rest && exprContainsTopLevelAwait(*arrPattern->rest);
  }
  if (auto* objPattern = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPattern->properties) {
      if (prop.key && exprContainsTopLevelAwait(*prop.key)) {
        return true;
      }
      if (prop.value && exprContainsTopLevelAwait(*prop.value)) {
        return true;
      }
    }
    return objPattern->rest && exprContainsTopLevelAwait(*objPattern->rest);
  }
  if (auto* assignPattern = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPattern->left && exprContainsTopLevelAwait(*assignPattern->left)) ||
           (assignPattern->right && exprContainsTopLevelAwait(*assignPattern->right));
  }

  return false;
}

bool isAwaitExpressionStatement(const Statement& stmt, const AwaitExpr** out) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    if (exprStmt->expression) {
      if (auto* awaitExpr = std::get_if<AwaitExpr>(&exprStmt->expression->node)) {
        if (out) {
          *out = awaitExpr;
        }
        return true;
      }
    }
  }
  return false;
}

bool isAwaitExportDefault(const Statement& stmt, const AwaitExpr** out) {
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    if (exportDefault->declaration) {
      if (auto* awaitExpr = std::get_if<AwaitExpr>(&exportDefault->declaration->node)) {
        if (out) {
          *out = awaitExpr;
        }
        return true;
      }
    }
  }
  return false;
}

std::shared_ptr<Promise> coerceToPromise(const Value& value) {
  if (value.isPromise()) {
    return std::get<std::shared_ptr<Promise>>(value.data);
  }
  return Promise::resolved(value);
}

thread_local std::vector<Module*> g_evalStack;
}  // namespace

Module::Module(const std::string& path, const std::string& source)
  : path_(path), source_(source), state_(State::Uninstantiated) {}

bool Module::parse() {
  lastError_.reset();
  try {
    Lexer lexer(source_);
    auto tokens = lexer.tokenize();

    Parser parser(tokens, true);
    ast_ = parser.parse();

    if (ast_) {
      ast_->isModule = true;  // Mark as module
      isAsync_ = false;
      std::unordered_set<std::string> lexicalNames;
      std::unordered_set<std::string> varNames;
      std::string duplicateName;
      for (const auto& stmt : ast_->body) {
        if (!collectModuleDeclarationNames(*stmt, lexicalNames, varNames, duplicateName)) {
          lastError_ = makeErrorValue(
            ErrorType::SyntaxError,
            "Identifier '" + duplicateName + "' has already been declared"
          );
          return false;
        }
      }
      for (const auto& stmt : ast_->body) {
        if (stmt && stmtContainsTopLevelAwait(*stmt)) {
          isAsync_ = true;
          break;
        }
      }
      for (const auto& name : lexicalNames) {
        if (varNames.count(name) > 0) {
          lastError_ = makeErrorValue(
            ErrorType::SyntaxError,
            "Identifier '" + name + "' has already been declared"
          );
          return false;
        }
      }
      return true;
    }
    lastError_ = makeErrorValue(ErrorType::SyntaxError, "Invalid module syntax");
  } catch (const std::exception& e) {
    std::cerr << "Module parse error in " << path_ << ": " << e.what() << std::endl;
    lastError_ = makeErrorFromExceptionMessage(e.what());
  }

  return false;
}

bool Module::instantiate(ModuleLoader* loader) {
  if (state_ != State::Uninstantiated) {
    if (state_ == State::Instantiating) {
      return true;
    }
    return state_ >= State::Instantiated;
  }

  lastError_.reset();
  state_ = State::Instantiating;
  dependencies_.clear();
  sourceDependencies_.clear();

  if (!ast_) {
    if (!parse()) {
      if (!lastError_) {
        lastError_ = makeErrorValue(ErrorType::SyntaxError, "Invalid module syntax");
      }
      return false;
    }
  }

  // Create module environment. Reuse current interpreter globals when available
  // so harness globals (assert/$DONE) remain visible inside loaded modules.
  if (auto* hostInterpreter = getGlobalInterpreter()) {
    environment_ = hostInterpreter->getEnvironment()->createChild();
  } else {
    environment_ = Environment::createGlobal();
  }

  auto loadDependency = [&](const std::string& moduleRequest) -> std::shared_ptr<Module> {
    auto existing = sourceDependencies_.find(moduleRequest);
    if (existing != sourceDependencies_.end()) {
      return existing->second;
    }

    std::string resolvedPath = loader->resolvePath(moduleRequest, path_);
    auto importedModule = loader->loadModule(resolvedPath);
    if (!importedModule) {
      std::cerr << "Failed to load module: " << moduleRequest << std::endl;
      if (auto loadError = loader->getLastError()) {
        lastError_ = *loadError;
      } else {
        lastError_ = makeErrorValue(ErrorType::Error, "Failed to load module: " + moduleRequest);
      }
      return nullptr;
    }

    // Allow self-reexport forms such as `export { x as y } from "./self.js"`.
    if (importedModule.get() == this) {
      sourceDependencies_[moduleRequest] = importedModule;
      return importedModule;
    }

    if (!importedModule->instantiate(loader)) {
      if (auto importError = importedModule->getLastError()) {
        lastError_ = *importError;
      } else {
        lastError_ = makeErrorValue(ErrorType::SyntaxError, "Failed to instantiate module: " + moduleRequest);
      }
      return nullptr;
    }

    sourceDependencies_[moduleRequest] = importedModule;
    bool alreadyTracked = false;
    for (const auto& dep : dependencies_) {
      if (dep == importedModule) {
        alreadyTracked = true;
        break;
      }
    }
    if (!alreadyTracked) {
      dependencies_.push_back(importedModule);
    }
    return importedModule;
  };

  // Process module requests from imports and re-exports.
  for (const auto& stmt : ast_->body) {
    if (auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node)) {
      if (!loadDependency(importDecl->source)) {
        return false;
      }
    } else if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
      if (exportNamed->source && !loadDependency(*exportNamed->source)) {
        return false;
      }
    } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
      if (!loadDependency(exportAll->source)) {
        return false;
      }
    }
  }

  state_ = State::Instantiated;
  lastError_.reset();
  return true;
}

void Module::ensureEvaluationPromise() {
  if (!topLevelPromise_) {
    topLevelPromise_ = std::make_shared<Promise>();
  }
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
    for (const auto& stmt : ast_->body) {
      auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node);
      if (!importDecl) {
        continue;
      }

      auto depIt = sourceDependencies_.find(importDecl->source);
      if (depIt == sourceDependencies_.end() || !depIt->second) {
        lastError_ = makeErrorValue(ErrorType::SyntaxError, "Unable to resolve import '" + importDecl->source + "'");
        state_ = State::Instantiated;
        return false;
      }
      auto importedModule = depIt->second;
      bool isSelfImport = importedModule.get() == this;

      if (importDecl->defaultImport) {
        if (isSelfImport) {
          ModuleBinding binding{weak_from_this(), "default"};
          environment_->define(importDecl->defaultImport->name, Value(binding));
        } else {
          auto defaultExport = importedModule->getExport("default");
          if (!defaultExport) {
            lastError_ = makeErrorValue(
              ErrorType::SyntaxError,
              "Module '" + importDecl->source + "' does not export 'default'"
            );
            state_ = State::Instantiated;
            return false;
          }
          environment_->define(importDecl->defaultImport->name, *defaultExport);
        }
      }

      if (importDecl->namespaceImport) {
        environment_->define(importDecl->namespaceImport->name, Value(importedModule->getNamespaceObject()));
      }

      for (const auto& spec : importDecl->specifiers) {
        if (isSelfImport) {
          ModuleBinding binding{weak_from_this(), spec.imported.name};
          environment_->define(spec.local.name, Value(binding));
          continue;
        }

        auto exportValue = importedModule->getExport(spec.imported.name);
        if (!exportValue) {
          lastError_ = makeErrorValue(
            ErrorType::SyntaxError,
            "Module '" + importDecl->source + "' does not export '" + spec.imported.name + "'"
          );
          state_ = State::Instantiated;
          return false;
        }
        environment_->define(spec.local.name, *exportValue);
      }
    }
    importsBound_ = true;
  }

  return evaluateBody(interpreter);
}

bool Module::evaluateBody(Interpreter* interpreter) {
  auto prevEnv = interpreter->getEnvironment();
  interpreter->setEnvironment(environment_);
  state_ = State::Evaluating;

  try {
    while (nextStatementIndex_ < ast_->body.size()) {
      const auto& stmtPtr = ast_->body[nextStatementIndex_];
      if (!stmtPtr) {
        nextStatementIndex_++;
        continue;
      }
      const auto& stmt = *stmtPtr;

      const AwaitExpr* awaitExpr = nullptr;
      if (isAsync_ && isAwaitExpressionStatement(stmt, &awaitExpr)) {
        Value argValue = Value(Undefined{});
        if (awaitExpr && awaitExpr->argument) {
          auto argTask = interpreter->evaluate(*awaitExpr->argument);
          LIGHTJS_RUN_TASK(argTask, argValue);
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
          LIGHTJS_RUN_TASK(argTask, argValue);
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
          defaultExport_ = result;
          exports_["default"] = result;
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

      // Handle exports and regular statements.
      if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
        if (exportNamed->declaration) {
          auto task = interpreter->evaluate(*exportNamed->declaration);
          LIGHTJS_RUN_TASK_VOID(task);
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

          if (auto* varDecl = std::get_if<VarDeclaration>(&exportNamed->declaration->node)) {
            for (const auto& decl : varDecl->declarations) {
              std::unordered_set<std::string> boundNames;
              collectBoundNamesFromPattern(decl.pattern, boundNames);
              for (const auto& name : boundNames) {
                exportBindings_[name] = name;
                if (auto value = environment_->get(name)) {
                  exports_[name] = *value;
                }
              }
            }
          } else if (auto* funcDecl = std::get_if<FunctionDeclaration>(&exportNamed->declaration->node)) {
            exportBindings_[funcDecl->id.name] = funcDecl->id.name;
            auto value = environment_->get(funcDecl->id.name);
            if (value) {
              exports_[funcDecl->id.name] = *value;
            }
          } else if (auto* classDecl = std::get_if<ClassDeclaration>(&exportNamed->declaration->node)) {
            exportBindings_[classDecl->id.name] = classDecl->id.name;
            auto value = environment_->get(classDecl->id.name);
            if (value) {
              exports_[classDecl->id.name] = *value;
            }
          }
        } else if (exportNamed->source && !exportNamed->specifiers.empty()) {
          auto depIt = sourceDependencies_.find(*exportNamed->source);
          if (depIt == sourceDependencies_.end() || !depIt->second) {
            lastError_ = makeErrorValue(
              ErrorType::SyntaxError,
              "Unable to resolve export source '" + *exportNamed->source + "'"
            );
            interpreter->setEnvironment(prevEnv);
            state_ = State::Instantiated;
            if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
              topLevelPromise_->reject(lastError_.value());
            }
            return false;
          }

          auto importedModule = depIt->second;
          for (const auto& spec : exportNamed->specifiers) {
            auto exportValue = importedModule->getExport(spec.local.name);
            if (!exportValue) {
              lastError_ = makeErrorValue(
                ErrorType::SyntaxError,
                "Module '" + *exportNamed->source + "' does not export '" + spec.local.name + "'"
              );
              interpreter->setEnvironment(prevEnv);
              state_ = State::Instantiated;
              if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
                topLevelPromise_->reject(lastError_.value());
              }
              return false;
            }
            exports_[spec.exported.name] = *exportValue;
          }
        } else if (!exportNamed->specifiers.empty()) {
          for (const auto& spec : exportNamed->specifiers) {
            exportBindings_[spec.exported.name] = spec.local.name;
            auto value = environment_->get(spec.local.name);
            if (value) {
              exports_[spec.exported.name] = *value;
            }
          }
        }
      } else if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
        auto task = interpreter->evaluate(*exportDefault->declaration);
        Value result;
        LIGHTJS_RUN_TASK(task, result);
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
        bool hasLiveDefaultBinding = false;
        if (auto* fnExpr = std::get_if<FunctionExpr>(&exportDefault->declaration->node)) {
          if (!fnExpr->name.empty()) {
            environment_->define(fnExpr->name, result);
            exportBindings_["default"] = fnExpr->name;
            hasLiveDefaultBinding = true;
          }
        } else if (auto* clsExpr = std::get_if<ClassExpr>(&exportDefault->declaration->node)) {
          if (!clsExpr->name.empty()) {
            environment_->define(clsExpr->name, result);
            exportBindings_["default"] = clsExpr->name;
            hasLiveDefaultBinding = true;
          }
        }
        if (!hasLiveDefaultBinding) {
          defaultExport_ = result;
          exports_["default"] = *defaultExport_;
        } else {
          exports_["default"] = result;
        }
      } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt.node)) {
        auto depIt = sourceDependencies_.find(exportAll->source);
        if (depIt == sourceDependencies_.end() || !depIt->second) {
          lastError_ = makeErrorValue(
            ErrorType::SyntaxError,
            "Unable to resolve export source '" + exportAll->source + "'"
          );
          interpreter->setEnvironment(prevEnv);
          state_ = State::Instantiated;
          if (topLevelPromise_ && topLevelPromise_->state == PromiseState::Pending) {
            topLevelPromise_->reject(lastError_.value());
          }
          return false;
        }

        if (exportAll->exported.has_value()) {
          exports_[exportAll->exported->name] = Value(depIt->second->getNamespaceObject());
        } else {
          auto exports = depIt->second->getAllExports();
          for (const auto& [name, value] : exports) {
            if (name == "default") {
              continue;
            }
            if (ambiguousReExports_.count(name) > 0) {
              continue;
            }
            if (exports_.find(name) != exports_.end()) {
              ambiguousReExports_.insert(name);
              exports_.erase(name);
            } else {
              exports_[name] = value;
            }
          }
        }
      } else if (std::holds_alternative<ImportDeclaration>(stmt.node)) {
        // Imports are handled before executing statements.
      } else {
        auto task = interpreter->evaluate(stmt);
        LIGHTJS_RUN_TASK_VOID(task);
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

    for (const auto& dep : dependencies_) {
      if (!dep->evaluate(interpreter)) {
        interpreter->setSuppressMicrotasks(prevSuppress);
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

std::optional<Value> Module::getExport(const std::string& name) const {
  if (ambiguousReExports_.count(name) > 0) {
    return std::nullopt;
  }
  auto binding = exportBindings_.find(name);
  if (binding != exportBindings_.end() && environment_) {
    if (auto value = environment_->get(binding->second)) {
      return *value;
    }
  }
  auto it = exports_.find(name);
  if (it != exports_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::unordered_map<std::string, Value> Module::getAllExports() const {
  auto all = exports_;
  if (environment_) {
    for (const auto& [exportName, localName] : exportBindings_) {
      if (auto value = environment_->get(localName)) {
        all[exportName] = *value;
      }
    }
  }
  for (const auto& name : ambiguousReExports_) {
    all.erase(name);
  }
  return all;
}

std::shared_ptr<Object> Module::getNamespaceObject() {
  auto moduleNamespace = namespaceObject_.lock();
  if (!moduleNamespace) {
    moduleNamespace = std::make_shared<Object>();
    moduleNamespace->isModuleNamespace = true;
    moduleNamespace->properties["__esModule"] = Value(true);
    namespaceObject_ = moduleNamespace;
  }

  for (const auto& name : moduleNamespace->moduleExportNames) {
    moduleNamespace->properties.erase(name);
    moduleNamespace->properties.erase("__get_" + name);
  }
  moduleNamespace->moduleExportNames.clear();

  auto exports = getAllExports();
  moduleNamespace->moduleExportNames.reserve(exports.size());
  for (const auto& [name, value] : exports) {
    moduleNamespace->moduleExportNames.push_back(name);
    moduleNamespace->properties[name] = value;

    auto getter = std::make_shared<Function>();
    getter->isNative = true;
    getter->nativeFunc = [module = this, name](const std::vector<Value>&) -> Value {
      if (auto current = module->getExport(name)) {
        return *current;
      }
      return Value(Undefined{});
    };
    moduleNamespace->properties["__get_" + name] = Value(getter);
  }
  std::sort(moduleNamespace->moduleExportNames.begin(), moduleNamespace->moduleExportNames.end());
  moduleNamespace->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("Module"));
  return moduleNamespace;
}

// ModuleLoader implementation

ModuleLoader::ModuleLoader() : basePath_(".") {}

std::shared_ptr<Module> ModuleLoader::loadModule(const std::string& path) {
  lastError_.reset();
  std::string normalizedPath = normalizePath(path);

  // Check cache
  auto cached = getCachedModule(normalizedPath);
  if (cached) {
    return cached;
  }

  // Read file
  auto source = readFile(normalizedPath);
  if (!source) {
    std::cerr << "Failed to read module: " << normalizedPath << std::endl;
    lastError_ = makeErrorValue(ErrorType::Error, "Failed to load module: " + normalizedPath);
    return nullptr;
  }

  // Create and cache module
  auto module = std::make_shared<Module>(normalizedPath, *source);
  cache_[normalizedPath] = module;

  // Parse the module
  if (!module->parse()) {
    if (auto parseError = module->getLastError()) {
      lastError_ = *parseError;
    } else {
      lastError_ = makeErrorValue(ErrorType::SyntaxError, "Failed to parse module: " + normalizedPath);
    }
    cache_.erase(normalizedPath);
    return nullptr;
  }

  return module;
}

std::string ModuleLoader::resolvePath(const std::string& specifier, const std::string& parentPath) {
  std::string resolved;

  // Handle relative paths
  if (specifier.size() >= 2 && specifier[0] == '.' && specifier[1] == '/') {
    // ./relative path
    std::string parent = parentPath.empty()
      ? (basePath_.empty() ? fs_compat::currentPath() : basePath_)
      : fs_compat::parentPath(parentPath);
    resolved = fs_compat::joinPath(parent, specifier.substr(2));
  } else if (specifier.size() >= 3 && specifier[0] == '.' && specifier[1] == '.' && specifier[2] == '/') {
    // ../relative path
    std::string parent = parentPath.empty()
      ? (basePath_.empty() ? fs_compat::currentPath() : basePath_)
      : fs_compat::parentPath(parentPath);
    resolved = fs_compat::joinPath(parent, specifier);
  }
  // Handle absolute paths
  else if (!specifier.empty() && specifier[0] == '/') {
    resolved = specifier;
  }
  // Handle node_modules style imports (simplified)
  else {
    resolved = fs_compat::joinPath(fs_compat::joinPath(basePath_, "node_modules"), specifier);
    if (!fs_compat::exists(resolved)) {
      resolved = fs_compat::joinPath(basePath_, specifier);
    }
  }

  // Add .js extension if not present
  std::string ext = fs_compat::extension(resolved);
  if (ext.empty()) {
    resolved += ".js";
  }

  return resolved;
}

std::shared_ptr<Module> ModuleLoader::getCachedModule(const std::string& path) {
  auto it = cache_.find(normalizePath(path));
  if (it != cache_.end()) {
    return it->second;
  }
  return nullptr;
}

std::optional<std::string> ModuleLoader::readFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

std::string ModuleLoader::normalizePath(const std::string& path) {
  try {
    return fs_compat::canonicalPath(path);
  } catch (...) {
    return fs_compat::absolutePath(path);
  }
}

} // namespace lightjs
