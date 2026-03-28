#include "module_internal.h"

namespace lightjs {

Module::Module(const std::string& path, const std::string& source, ModuleType type)
  : path_(path), source_(source), type_(type), state_(State::Uninstantiated) {}

bool Module::parse() {
  lastError_.reset();
  if (type_ == ModuleType::Json) {
    try {
      defaultExport_ = JSON_parse({Value(source_)});
      ast_ = Program{};
      ast_->isModule = true;
      isAsync_ = false;
      return true;
    } catch (const std::exception& e) {
      lastError_ = makeErrorFromExceptionMessage(e.what());
      return false;
    }
  }
  if (type_ == ModuleType::Bytes) {
    ast_ = Program{};
    ast_->isModule = true;
    isAsync_ = false;
    return true;
  }
  try {
    Lexer lexer(source_);
    auto tokens = lexer.tokenize();

    Parser parser(tokens, true);
    ast_ = parser.parse();

    if (ast_) {
      ast_->isModule = true;
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
      for (const auto& name : lexicalNames) {
        if (varNames.count(name) > 0) {
          lastError_ = makeErrorValue(
            ErrorType::SyntaxError,
            "Identifier '" + name + "' has already been declared"
          );
          return false;
        }
      }

      std::unordered_set<std::string> declaredNames = lexicalNames;
      declaredNames.insert(varNames.begin(), varNames.end());
      std::unordered_set<std::string> exportedNames;

      auto addExportedName = [&](const std::string& name) -> bool {
        if (!exportedNames.insert(name).second) {
          lastError_ = makeErrorValue(ErrorType::SyntaxError, "Duplicate export '" + name + "'");
          return false;
        }
        return true;
      };

      auto ensureLocalExportBinding = [&](const std::string& name) -> bool {
        if (declaredNames.count(name) == 0) {
          lastError_ = makeErrorValue(ErrorType::SyntaxError, "Export '" + name + "' is not defined in module");
          return false;
        }
        return true;
      };

      auto registerExportedFromDeclaration = [&](const Statement& declaration) -> bool {
        if (auto* varDecl = std::get_if<VarDeclaration>(&declaration.node)) {
          for (const auto& declarator : varDecl->declarations) {
            std::unordered_set<std::string> names;
            collectBoundNamesFromPattern(declarator.pattern, names);
            for (const auto& name : names) {
              if (!addExportedName(name)) {
                return false;
              }
            }
          }
          return true;
        }
        if (auto* fnDecl = std::get_if<FunctionDeclaration>(&declaration.node)) {
          return addExportedName(fnDecl->id.name);
        }
        if (auto* classDecl = std::get_if<ClassDeclaration>(&declaration.node)) {
          return addExportedName(classDecl->id.name);
        }
        return true;
      };

      for (const auto& stmt : ast_->body) {
        if (!stmt) {
          continue;
        }
        if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
          if (exportNamed->declaration) {
            if (!registerExportedFromDeclaration(*exportNamed->declaration)) {
              return false;
            }
          } else if (exportNamed->source) {
            for (const auto& spec : exportNamed->specifiers) {
              if (!addExportedName(spec.exported.name)) {
                return false;
              }
            }
          } else {
            for (const auto& spec : exportNamed->specifiers) {
              if (!ensureLocalExportBinding(spec.local.name)) {
                return false;
              }
              if (!addExportedName(spec.exported.name)) {
                return false;
              }
            }
          }
          continue;
        }
        if (std::holds_alternative<ExportDefaultDeclaration>(stmt->node)) {
          if (!addExportedName("default")) {
            return false;
          }
          continue;
        }
        if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
          if (exportAll->exported.has_value()) {
            if (!addExportedName(exportAll->exported->name)) {
              return false;
            }
          }
        }
      }

      std::unordered_set<std::string> visiblePrivateNames;
      std::string invalidPrivateName;
      for (const auto& stmt : ast_->body) {
        if (stmt &&
            !validatePrivateNamesInStatement(*stmt, visiblePrivateNames, invalidPrivateName)) {
          if (!invalidPrivateName.empty()) {
            lastError_ = makeErrorValue(
              ErrorType::SyntaxError, "Invalid private name '" + invalidPrivateName + "'");
          } else {
            lastError_ = makeErrorValue(ErrorType::SyntaxError, "Invalid private name");
          }
          return false;
        }
      }

      std::function<bool(const Expression&)> containsForbiddenModuleExpr;
      containsForbiddenModuleExpr = [&](const Expression& expr) -> bool {
        if (std::holds_alternative<SuperExpr>(expr.node)) {
          return true;
        }
        if (auto* meta = std::get_if<MetaProperty>(&expr.node)) {
          return meta->meta == "new" && meta->property == "target";
        }
        if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
          return unary->argument && containsForbiddenModuleExpr(*unary->argument);
        }
        if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
          return (binary->left && containsForbiddenModuleExpr(*binary->left)) ||
                 (binary->right && containsForbiddenModuleExpr(*binary->right));
        }
        if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
          return (assign->left && containsForbiddenModuleExpr(*assign->left)) ||
                 (assign->right && containsForbiddenModuleExpr(*assign->right));
        }
        if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
          return update->argument && containsForbiddenModuleExpr(*update->argument);
        }
        if (auto* call = std::get_if<CallExpr>(&expr.node)) {
          if (call->callee && containsForbiddenModuleExpr(*call->callee)) {
            return true;
          }
          for (const auto& arg : call->arguments) {
            if (arg && containsForbiddenModuleExpr(*arg)) {
              return true;
            }
          }
          return false;
        }
        if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
          return (member->object && containsForbiddenModuleExpr(*member->object)) ||
                 (member->property && containsForbiddenModuleExpr(*member->property));
        }
        if (auto* conditional = std::get_if<ConditionalExpr>(&expr.node)) {
          return (conditional->test && containsForbiddenModuleExpr(*conditional->test)) ||
                 (conditional->consequent && containsForbiddenModuleExpr(*conditional->consequent)) ||
                 (conditional->alternate && containsForbiddenModuleExpr(*conditional->alternate));
        }
        if (auto* sequence = std::get_if<SequenceExpr>(&expr.node)) {
          for (const auto& element : sequence->expressions) {
            if (element && containsForbiddenModuleExpr(*element)) {
              return true;
            }
          }
          return false;
        }
        if (auto* array = std::get_if<ArrayExpr>(&expr.node)) {
          for (const auto& element : array->elements) {
            if (element && containsForbiddenModuleExpr(*element)) {
              return true;
            }
          }
          return false;
        }
        if (auto* object = std::get_if<ObjectExpr>(&expr.node)) {
          for (const auto& property : object->properties) {
            if (property.key && containsForbiddenModuleExpr(*property.key)) {
              return true;
            }
            if (property.value && containsForbiddenModuleExpr(*property.value)) {
              return true;
            }
          }
          return false;
        }
        if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
          return awaitExpr->argument && containsForbiddenModuleExpr(*awaitExpr->argument);
        }
        if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
          return yieldExpr->argument && containsForbiddenModuleExpr(*yieldExpr->argument);
        }
        if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
          if (newExpr->callee && containsForbiddenModuleExpr(*newExpr->callee)) {
            return true;
          }
          for (const auto& arg : newExpr->arguments) {
            if (arg && containsForbiddenModuleExpr(*arg)) {
              return true;
            }
          }
          return false;
        }
        if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
          return spread->argument && containsForbiddenModuleExpr(*spread->argument);
        }
        if (auto* assignPattern = std::get_if<AssignmentPattern>(&expr.node)) {
          return (assignPattern->left && containsForbiddenModuleExpr(*assignPattern->left)) ||
                 (assignPattern->right && containsForbiddenModuleExpr(*assignPattern->right));
        }
        if (auto* arrPattern = std::get_if<ArrayPattern>(&expr.node)) {
          for (const auto& element : arrPattern->elements) {
            if (element && containsForbiddenModuleExpr(*element)) {
              return true;
            }
          }
          return arrPattern->rest && containsForbiddenModuleExpr(*arrPattern->rest);
        }
        if (auto* objPattern = std::get_if<ObjectPattern>(&expr.node)) {
          for (const auto& property : objPattern->properties) {
            if (property.key && containsForbiddenModuleExpr(*property.key)) {
              return true;
            }
            if (property.value && containsForbiddenModuleExpr(*property.value)) {
              return true;
            }
          }
          return objPattern->rest && containsForbiddenModuleExpr(*objPattern->rest);
        }
        if (auto* classExpr = std::get_if<ClassExpr>(&expr.node)) {
          return classExpr->superClass && containsForbiddenModuleExpr(*classExpr->superClass);
        }
        if (std::holds_alternative<FunctionExpr>(expr.node)) {
          return false;
        }
        return false;
      };

      auto reportForbiddenModuleSyntax = [&]() -> bool {
        lastError_ = makeErrorValue(ErrorType::SyntaxError, "Invalid use of restricted syntax in module body");
        return false;
      };

      std::vector<std::pair<std::string, bool>> labelStack;
      std::function<bool(const Statement&, bool, bool)> validateModuleStatement;
      validateModuleStatement = [&](const Statement& statement, bool inIteration, bool inSwitch) -> bool {
        if (auto* varDecl = std::get_if<VarDeclaration>(&statement.node)) {
          for (const auto& declarator : varDecl->declarations) {
            if (declarator.init && containsForbiddenModuleExpr(*declarator.init)) {
              return reportForbiddenModuleSyntax();
            }
          }
          return true;
        }
        if (auto* exprStmt = std::get_if<ExpressionStmt>(&statement.node)) {
          if (exprStmt->expression && containsForbiddenModuleExpr(*exprStmt->expression)) {
            return reportForbiddenModuleSyntax();
          }
          return true;
        }
        if (auto* returnStmt = std::get_if<ReturnStmt>(&statement.node)) {
          if (returnStmt->argument && containsForbiddenModuleExpr(*returnStmt->argument)) {
            return reportForbiddenModuleSyntax();
          }
          return true;
        }
        if (auto* throwStmt = std::get_if<ThrowStmt>(&statement.node)) {
          if (throwStmt->argument && containsForbiddenModuleExpr(*throwStmt->argument)) {
            return reportForbiddenModuleSyntax();
          }
          return true;
        }
        if (auto* block = std::get_if<BlockStmt>(&statement.node)) {
          for (const auto& child : block->body) {
            if (child && !validateModuleStatement(*child, inIteration, inSwitch)) {
              return false;
            }
          }
          return true;
        }
        if (auto* ifStmt = std::get_if<IfStmt>(&statement.node)) {
          if (ifStmt->test && containsForbiddenModuleExpr(*ifStmt->test)) {
            return reportForbiddenModuleSyntax();
          }
          if (ifStmt->consequent && !validateModuleStatement(*ifStmt->consequent, inIteration, inSwitch)) {
            return false;
          }
          if (ifStmt->alternate && !validateModuleStatement(*ifStmt->alternate, inIteration, inSwitch)) {
            return false;
          }
          return true;
        }
        if (auto* whileStmt = std::get_if<WhileStmt>(&statement.node)) {
          if (whileStmt->test && containsForbiddenModuleExpr(*whileStmt->test)) {
            return reportForbiddenModuleSyntax();
          }
          return whileStmt->body && validateModuleStatement(*whileStmt->body, true, inSwitch);
        }
        if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&statement.node)) {
          if (doWhileStmt->test && containsForbiddenModuleExpr(*doWhileStmt->test)) {
            return reportForbiddenModuleSyntax();
          }
          return doWhileStmt->body && validateModuleStatement(*doWhileStmt->body, true, inSwitch);
        }
        if (auto* forStmt = std::get_if<ForStmt>(&statement.node)) {
          if (forStmt->init && !validateModuleStatement(*forStmt->init, inIteration, inSwitch)) {
            return false;
          }
          if (forStmt->test && containsForbiddenModuleExpr(*forStmt->test)) {
            return reportForbiddenModuleSyntax();
          }
          if (forStmt->update && containsForbiddenModuleExpr(*forStmt->update)) {
            return reportForbiddenModuleSyntax();
          }
          return forStmt->body && validateModuleStatement(*forStmt->body, true, inSwitch);
        }
        if (auto* forInStmt = std::get_if<ForInStmt>(&statement.node)) {
          if (forInStmt->left && !validateModuleStatement(*forInStmt->left, inIteration, inSwitch)) {
            return false;
          }
          if (forInStmt->right && containsForbiddenModuleExpr(*forInStmt->right)) {
            return reportForbiddenModuleSyntax();
          }
          return forInStmt->body && validateModuleStatement(*forInStmt->body, true, inSwitch);
        }
        if (auto* forOfStmt = std::get_if<ForOfStmt>(&statement.node)) {
          if (forOfStmt->left && !validateModuleStatement(*forOfStmt->left, inIteration, inSwitch)) {
            return false;
          }
          if (forOfStmt->right && containsForbiddenModuleExpr(*forOfStmt->right)) {
            return reportForbiddenModuleSyntax();
          }
          return forOfStmt->body && validateModuleStatement(*forOfStmt->body, true, inSwitch);
        }
        if (auto* switchStmt = std::get_if<SwitchStmt>(&statement.node)) {
          if (switchStmt->discriminant && containsForbiddenModuleExpr(*switchStmt->discriminant)) {
            return reportForbiddenModuleSyntax();
          }
          for (const auto& switchCase : switchStmt->cases) {
            if (switchCase.test && containsForbiddenModuleExpr(*switchCase.test)) {
              return reportForbiddenModuleSyntax();
            }
            for (const auto& child : switchCase.consequent) {
              if (child && !validateModuleStatement(*child, inIteration, true)) {
                return false;
              }
            }
          }
          return true;
        }
        if (auto* breakStmt = std::get_if<BreakStmt>(&statement.node)) {
          if (breakStmt->label.empty()) {
            if (!inIteration && !inSwitch) {
              lastError_ = makeErrorValue(ErrorType::SyntaxError, "Illegal break statement");
              return false;
            }
            return true;
          }
          for (auto it = labelStack.rbegin(); it != labelStack.rend(); ++it) {
            if (it->first == breakStmt->label) {
              return true;
            }
          }
          lastError_ = makeErrorValue(ErrorType::SyntaxError, "Undefined break target");
          return false;
        }
        if (auto* continueStmt = std::get_if<ContinueStmt>(&statement.node)) {
          if (continueStmt->label.empty()) {
            if (!inIteration) {
              lastError_ = makeErrorValue(ErrorType::SyntaxError, "Illegal continue statement");
              return false;
            }
            return true;
          }
          for (auto it = labelStack.rbegin(); it != labelStack.rend(); ++it) {
            if (it->first == continueStmt->label && it->second) {
              return true;
            }
          }
          lastError_ = makeErrorValue(ErrorType::SyntaxError, "Undefined continue target");
          return false;
        }
        if (auto* labelledStmt = std::get_if<LabelledStmt>(&statement.node)) {
          for (const auto& [label, _] : labelStack) {
            if (label == labelledStmt->label) {
              lastError_ = makeErrorValue(ErrorType::SyntaxError, "Duplicate label '" + labelledStmt->label + "'");
              return false;
            }
          }
          bool labelsIteration = labelledStmt->body &&
            (std::holds_alternative<WhileStmt>(labelledStmt->body->node) ||
             std::holds_alternative<ForStmt>(labelledStmt->body->node) ||
             std::holds_alternative<ForInStmt>(labelledStmt->body->node) ||
             std::holds_alternative<ForOfStmt>(labelledStmt->body->node) ||
             std::holds_alternative<DoWhileStmt>(labelledStmt->body->node));
          labelStack.push_back({labelledStmt->label, labelsIteration});
          bool ok = labelledStmt->body && validateModuleStatement(*labelledStmt->body, inIteration, inSwitch);
          labelStack.pop_back();
          return ok;
        }
        if (auto* withStmt = std::get_if<WithStmt>(&statement.node)) {
          if (withStmt->object && containsForbiddenModuleExpr(*withStmt->object)) {
            return reportForbiddenModuleSyntax();
          }
          return withStmt->body && validateModuleStatement(*withStmt->body, inIteration, inSwitch);
        }
        if (auto* tryStmt = std::get_if<TryStmt>(&statement.node)) {
          for (const auto& child : tryStmt->block) {
            if (child && !validateModuleStatement(*child, inIteration, inSwitch)) {
              return false;
            }
          }
          if (tryStmt->hasHandler) {
            if (tryStmt->handler.paramPattern && containsForbiddenModuleExpr(*tryStmt->handler.paramPattern)) {
              return reportForbiddenModuleSyntax();
            }
            for (const auto& child : tryStmt->handler.body) {
              if (child && !validateModuleStatement(*child, inIteration, inSwitch)) {
                return false;
              }
            }
          }
          if (tryStmt->hasFinalizer) {
            for (const auto& child : tryStmt->finalizer) {
              if (child && !validateModuleStatement(*child, inIteration, inSwitch)) {
                return false;
              }
            }
          }
          return true;
        }
        if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&statement.node)) {
          if (exportNamed->declaration) {
            return validateModuleStatement(*exportNamed->declaration, inIteration, inSwitch);
          }
          return true;
        }
        if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&statement.node)) {
          if (exportDefault->declaration && containsForbiddenModuleExpr(*exportDefault->declaration)) {
            return reportForbiddenModuleSyntax();
          }
          return true;
        }
        if (std::holds_alternative<FunctionDeclaration>(statement.node) ||
            std::holds_alternative<ClassDeclaration>(statement.node) ||
            std::holds_alternative<ImportDeclaration>(statement.node) ||
            std::holds_alternative<ExportAllDeclaration>(statement.node)) {
          return true;
        }
        return true;
      };

      for (const auto& stmt : ast_->body) {
        if (stmt && !validateModuleStatement(*stmt, false, false)) {
          return false;
        }
      }

      for (const auto& stmt : ast_->body) {
        if (stmt && stmtContainsTopLevelAwait(*stmt)) {
          isAsync_ = true;
          break;
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
  eagerDependencyKeys_.clear();

  if (!ast_) {
    if (!parse()) {
      if (!lastError_) {
        lastError_ = makeErrorValue(ErrorType::SyntaxError, "Invalid module syntax");
      }
      return false;
    }
  }

  if (auto* hostInterpreter = getGlobalInterpreter()) {
    environment_ = hostInterpreter->getEnvironment()->createChild();
  } else {
    environment_ = Environment::createGlobal();
  }
  environment_->define("this", Value(Undefined{}));
  if (!initializeDeclaredExports(nullptr)) {
    if (!lastError_) {
      lastError_ = makeErrorValue(ErrorType::SyntaxError, "Failed to initialize module exports");
    }
    return false;
  }

  auto loadDependency = [&](const std::string& moduleRequest,
                            ModuleType moduleType) -> std::shared_ptr<Module> {
    std::string dependencyKey = dependencyCacheKey(moduleRequest, moduleType);
    auto existing = sourceDependencies_.find(dependencyKey);
    if (existing != sourceDependencies_.end()) {
      return existing->second;
    }

    std::string resolvedPath = loader->resolvePath(moduleRequest, path_);
    auto importedModule = loader->loadModule(resolvedPath, moduleType);
    if (!importedModule) {
      std::cerr << "Failed to load module: " << moduleRequest << std::endl;
      if (auto loadError = loader->getLastError()) {
        lastError_ = *loadError;
      } else {
        lastError_ = makeErrorValue(ErrorType::Error, "Failed to load module: " + moduleRequest);
      }
      return nullptr;
    }

    if (importedModule.get() == this) {
      sourceDependencies_[dependencyKey] = importedModule;
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

    sourceDependencies_[dependencyKey] = importedModule;
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

  for (const auto& stmt : ast_->body) {
    auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node);
    if (!importDecl || importDecl->phase != ImportDeclaration::Phase::Source) {
      continue;
    }
    lastError_ = makeErrorValue(
      ErrorType::TypeError,
      "Source phase import is not available for source text modules");
    return false;
  }

  for (const auto& stmt : ast_->body) {
    if (auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node)) {
      ModuleType depType = moduleTypeFromAttributes(importDecl->attributes);
      std::string depKey = dependencyCacheKey(importDecl->source, depType);
      if (!loadDependency(importDecl->source, depType)) {
        return false;
      }
      if (importDecl->phase != ImportDeclaration::Phase::Defer) {
        eagerDependencyKeys_.insert(depKey);
      }
    } else if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
      if (exportNamed->source) {
        ModuleType depType = moduleTypeFromAttributes(exportNamed->attributes);
        std::string depKey = dependencyCacheKey(*exportNamed->source, depType);
        if (!loadDependency(*exportNamed->source, depType)) {
          return false;
        }
        eagerDependencyKeys_.insert(depKey);
      }
    } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
      ModuleType depType = moduleTypeFromAttributes(exportAll->attributes);
      std::string depKey = dependencyCacheKey(exportAll->source, depType);
      if (!loadDependency(exportAll->source, depType)) {
        return false;
      }
      eagerDependencyKeys_.insert(depKey);
    }
  }

  state_ = State::Instantiated;
  lastError_.reset();
  return true;
}

}  // namespace lightjs
