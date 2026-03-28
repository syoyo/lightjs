#include "module_internal.h"

namespace lightjs {

bool Module::hasExport(const std::string& name) const {
  if (ambiguousReExports_.count(name) > 0) {
    return false;
  }
  if (name == "default" && defaultExport_.has_value()) {
    return true;
  }
  if (exportBindings_.count(name) > 0 || exports_.count(name) > 0) {
    return true;
  }
  if (!ast_) {
    return false;
  }

  std::unordered_set<std::string> resolveSet;
  auto resolveExportExists = [&](const Module* module,
                                 const std::string& exportName,
                                 auto&& resolveExportExistsRef) -> bool {
    if (!module || !module->ast_) {
      return false;
    }

    std::string visitKey = module->path_ + ":" + exportName;
    if (!resolveSet.insert(visitKey).second) {
      return false;
    }

    if (module->ambiguousReExports_.count(exportName) > 0) {
      return false;
    }
    if (module->exportBindings_.count(exportName) > 0 ||
        module->exports_.count(exportName) > 0) {
      return true;
    }

    for (const auto& stmt : module->ast_->body) {
      if (!stmt) {
        continue;
      }
      if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
        if (!exportNamed->source) {
          continue;
        }
        for (const auto& spec : exportNamed->specifiers) {
          if (spec.exported.name != exportName) {
            continue;
          }
          auto depIt = module->sourceDependencies_.find(
            dependencyCacheKey(*exportNamed->source,
                               moduleTypeFromAttributes(exportNamed->attributes)));
          if (depIt == module->sourceDependencies_.end() || !depIt->second) {
            return false;
          }
          return resolveExportExistsRef(depIt->second.get(),
                                        spec.local.name,
                                        resolveExportExistsRef);
        }
      } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
        if (!exportAll->exported.has_value() ||
            exportAll->exported->name != exportName) {
          continue;
        }
        auto depIt = module->sourceDependencies_.find(
          dependencyCacheKey(exportAll->source,
                             moduleTypeFromAttributes(exportAll->attributes)));
        return depIt != module->sourceDependencies_.end() && depIt->second != nullptr;
      }
    }

    for (const auto& stmt : module->ast_->body) {
      if (!stmt) {
        continue;
      }
      auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node);
      if (!exportAll || exportAll->exported.has_value()) {
        continue;
      }
      auto depIt = module->sourceDependencies_.find(
        dependencyCacheKey(exportAll->source,
                           moduleTypeFromAttributes(exportAll->attributes)));
      if (depIt == module->sourceDependencies_.end() || !depIt->second) {
        continue;
      }
      if (resolveExportExistsRef(depIt->second.get(), exportName, resolveExportExistsRef)) {
        return true;
      }
    }

    return false;
  };

  return resolveExportExists(this, name, resolveExportExists);
}

std::vector<std::string> Module::getExportNames() const {
  std::vector<std::string> names;
  names.reserve(exportBindings_.size() + exports_.size());
  std::unordered_set<std::string> seen;

  if (defaultExport_.has_value()) {
    seen.insert("default");
    names.push_back("default");
  }

  for (const auto& [name, _] : exportBindings_) {
    if (ambiguousReExports_.count(name) > 0) {
      continue;
    }
    if (seen.insert(name).second) {
      names.push_back(name);
    }
  }
  for (const auto& [name, _] : exports_) {
    if (ambiguousReExports_.count(name) > 0) {
      continue;
    }
    if (seen.insert(name).second) {
      names.push_back(name);
    }
  }

  std::sort(names.begin(), names.end());
  return names;
}

bool Module::initializeDeclaredExports(Interpreter* interpreter) {
  if (!ast_ || !environment_) {
    return true;
  }

  std::vector<const FunctionDeclaration*> hoistedFunctionDeclarations;
  struct HoistedDefaultFunction {
    const Expression* expression = nullptr;
    std::string bindingName;
    bool assignDefaultName = false;
  };
  std::vector<HoistedDefaultFunction> hoistedDefaultFunctions;

  auto ensureVarBinding = [this](const std::string& name) {
    if (!environment_->hasLocal(name)) {
      environment_->define(name, Value(Undefined{}));
    }
  };
  auto ensureTDZBinding = [this](const std::string& name) {
    if (!environment_->hasLocal(name)) {
      environment_->defineTDZ(name);
    }
  };

  auto registerVarDeclaration = [&](const VarDeclaration& varDecl, bool exported) {
    for (const auto& decl : varDecl.declarations) {
      std::unordered_set<std::string> boundNames;
      collectBoundNamesFromPattern(decl.pattern, boundNames);
      for (const auto& name : boundNames) {
        if (exported) {
          exportBindings_[name] = name;
        }
        if (varDecl.kind == VarDeclaration::Kind::Var) {
          ensureVarBinding(name);
        } else {
          ensureTDZBinding(name);
        }
      }
    }
  };

  auto registerFunctionDeclaration = [&](const FunctionDeclaration& funcDecl, bool exported) {
    if (exported) {
      exportBindings_[funcDecl.id.name] = funcDecl.id.name;
    }
    ensureVarBinding(funcDecl.id.name);
    hoistedFunctionDeclarations.push_back(&funcDecl);
  };

  auto registerClassDeclaration = [&](const ClassDeclaration& classDecl, bool exported) {
    if (exported) {
      exportBindings_[classDecl.id.name] = classDecl.id.name;
    }
    ensureTDZBinding(classDecl.id.name);
  };

  for (const auto& stmt : ast_->body) {
    if (!stmt) {
      continue;
    }

    if (auto* varDecl = std::get_if<VarDeclaration>(&stmt->node)) {
      registerVarDeclaration(*varDecl, false);
      continue;
    }

    if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
      registerFunctionDeclaration(*funcDecl, false);
      continue;
    }

    if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt->node)) {
      registerClassDeclaration(*classDecl, false);
      continue;
    }

    if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
      if (exportNamed->declaration) {
        if (auto* varDecl = std::get_if<VarDeclaration>(&exportNamed->declaration->node)) {
          registerVarDeclaration(*varDecl, true);
        } else if (auto* funcDecl = std::get_if<FunctionDeclaration>(&exportNamed->declaration->node)) {
          registerFunctionDeclaration(*funcDecl, true);
        } else if (auto* classDecl = std::get_if<ClassDeclaration>(&exportNamed->declaration->node)) {
          registerClassDeclaration(*classDecl, true);
        }
      } else if (!exportNamed->source && !exportNamed->specifiers.empty()) {
        for (const auto& spec : exportNamed->specifiers) {
          exportBindings_[spec.exported.name] = spec.local.name;
        }
      }
      continue;
    }

    if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt->node)) {
      bool hasNamedDefaultBinding = false;
      if (exportDefault->isHoistableDeclaration && exportDefault->declaration) {
        if (auto* fnExpr = std::get_if<FunctionExpr>(&exportDefault->declaration->node)) {
          if (!fnExpr->isArrow) {
            std::string bindingName = fnExpr->name.empty()
              ? kSyntheticDefaultExportBinding
              : fnExpr->name;
            exportBindings_["default"] = bindingName;
            ensureVarBinding(bindingName);
            hoistedDefaultFunctions.push_back(
              HoistedDefaultFunction{exportDefault->declaration.get(), bindingName, fnExpr->name.empty()});
            hasNamedDefaultBinding = true;
          }
        } else if (auto* clsExpr = std::get_if<ClassExpr>(&exportDefault->declaration->node)) {
          if (!clsExpr->name.empty()) {
            exportBindings_["default"] = clsExpr->name;
            ensureTDZBinding(clsExpr->name);
            hasNamedDefaultBinding = true;
          }
        }
      }
      if (!hasNamedDefaultBinding) {
        exportBindings_["default"] = kSyntheticDefaultExportBinding;
        ensureTDZBinding(kSyntheticDefaultExportBinding);
      }
    }
  }

  if (!interpreter) {
    return true;
  }

  auto prevEnv = interpreter->getEnvironment();
  bool prevStrictMode = interpreter->strictMode_;
  struct ModuleInstantiationScopeGuard {
    Interpreter* interpreter;
    GCPtr<Environment> prevEnv;
    bool prevStrictMode;
    ~ModuleInstantiationScopeGuard() {
      interpreter->setEnvironment(prevEnv);
      interpreter->strictMode_ = prevStrictMode;
    }
  } scopeGuard{interpreter, prevEnv, prevStrictMode};

  interpreter->setEnvironment(environment_);
  interpreter->strictMode_ = true;

  for (const auto* functionDecl : hoistedFunctionDeclarations) {
    auto task = interpreter->evaluateFuncDecl(*functionDecl);
    while (!task.done()) {
      task.resume();
    }
    if (interpreter->hasError()) {
      lastError_ = interpreter->getError();
      interpreter->clearError();
      return false;
    }
  }

  for (const auto& defaultFunction : hoistedDefaultFunctions) {
    auto task = interpreter->evaluate(*defaultFunction.expression);
    Value functionValue = Value(Undefined{});
    while (!task.done()) {
      task.resume();
    }
    functionValue = task.result();
    if (interpreter->hasError()) {
      lastError_ = interpreter->getError();
      interpreter->clearError();
      return false;
    }
    if (defaultFunction.assignDefaultName) {
      setDefaultExportNameIfNeeded(functionValue);
    }
    if (functionValue.isFunction()) {
      auto fn = functionValue.getGC<Function>();
      auto namedExprIt = fn->properties.find("__named_expression__");
      if (namedExprIt != fn->properties.end()) {
        fn->properties["__named_expression__"] = Value(false);
      }
    }
    environment_->define(defaultFunction.bindingName, functionValue);
  }

  return true;
}

bool Module::rebuildIndirectExports() {
  if (!ast_) {
    return true;
  }

  exports_.clear();
  ambiguousReExports_.clear();
  std::unordered_set<std::string> explicitExportNames;

  struct ResolvedBindingIdentity {
    const Module* module = nullptr;
    std::string bindingName;
    Object* namespaceObject = nullptr;
  };

  auto resolveBindingIdentity = [&](const std::shared_ptr<Module>& module,
                                    const std::string& exportName,
                                    auto&& resolveBindingIdentityRef,
                                    std::unordered_set<std::string>& visited)
                                    -> std::optional<ResolvedBindingIdentity> {
    if (!module) {
      return std::nullopt;
    }

    std::string visitKey = module->path_ + ":" + exportName;
    if (!visited.insert(visitKey).second) {
      return std::nullopt;
    }
    if (module->ambiguousReExports_.count(exportName) > 0) {
      return std::nullopt;
    }

    auto directBinding = module->exportBindings_.find(exportName);
    if (directBinding != module->exportBindings_.end()) {
      const auto& localName = directBinding->second;
      if (module->environment_ && module->environment_->hasLocal(localName)) {
        auto localValue = module->environment_->get(localName);
        if (localValue && localValue->isModuleBinding()) {
          const auto& imported = std::get<ModuleBinding>(localValue->data);
          return resolveBindingIdentityRef(imported.module.lock(),
                                           imported.exportName,
                                           resolveBindingIdentityRef,
                                           visited);
        }
        if (localValue && localValue->isObject()) {
          auto obj = std::get<GCPtr<Object>>(localValue->data);
          if (obj && obj->isModuleNamespace) {
            ResolvedBindingIdentity identity;
            identity.namespaceObject = obj.get();
            return identity;
          }
        }
      }
      ResolvedBindingIdentity identity;
      identity.module = module.get();
      identity.bindingName = localName;
      return identity;
    }

    auto exportIt = module->exports_.find(exportName);
    if (exportIt != module->exports_.end()) {
      if (exportIt->second.isModuleBinding()) {
        const auto& imported = std::get<ModuleBinding>(exportIt->second.data);
        return resolveBindingIdentityRef(imported.module.lock(),
                                         imported.exportName,
                                         resolveBindingIdentityRef,
                                         visited);
      }
      if (exportIt->second.isObject()) {
        auto obj = exportIt->second.getGC<Object>();
        if (obj && obj->isModuleNamespace) {
          ResolvedBindingIdentity identity;
          identity.namespaceObject = obj.get();
          return identity;
        }
      }
      ResolvedBindingIdentity identity;
      identity.module = module.get();
      identity.bindingName = exportName;
      return identity;
    }

    return std::nullopt;
  };

  auto identityForValue = [&](const Value& value) -> std::optional<ResolvedBindingIdentity> {
    if (value.isModuleBinding()) {
      const auto& binding = std::get<ModuleBinding>(value.data);
      std::unordered_set<std::string> visited;
      return resolveBindingIdentity(binding.module.lock(),
                                    binding.exportName,
                                    resolveBindingIdentity,
                                    visited);
    }
    if (value.isObject()) {
      auto obj = value.getGC<Object>();
      if (obj && obj->isModuleNamespace) {
        ResolvedBindingIdentity identity;
        identity.namespaceObject = obj.get();
        return identity;
      }
    }
    return std::nullopt;
  };

  auto equivalentBinding = [&](const Value& a, const Value& b) -> bool {
    auto lhsIdentity = identityForValue(a);
    auto rhsIdentity = identityForValue(b);
    if (!lhsIdentity.has_value() || !rhsIdentity.has_value()) {
      return false;
    }
    if (lhsIdentity->namespaceObject || rhsIdentity->namespaceObject) {
      return lhsIdentity->namespaceObject != nullptr &&
             lhsIdentity->namespaceObject == rhsIdentity->namespaceObject;
    }
    return lhsIdentity->module != nullptr &&
           lhsIdentity->module == rhsIdentity->module &&
           lhsIdentity->bindingName == rhsIdentity->bindingName;
  };

  for (const auto& stmt : ast_->body) {
    if (!stmt) {
      continue;
    }
    if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
      if (!exportNamed->source || exportNamed->specifiers.empty()) {
        continue;
      }
      auto depIt = sourceDependencies_.find(
        dependencyCacheKey(*exportNamed->source,
                           moduleTypeFromAttributes(exportNamed->attributes)));
      if (depIt == sourceDependencies_.end() || !depIt->second) {
        lastError_ = makeErrorValue(
          ErrorType::SyntaxError,
          "Unable to resolve export source '" + *exportNamed->source + "'"
        );
        return false;
      }
      auto importedModule = depIt->second;
      for (const auto& spec : exportNamed->specifiers) {
        if (!importedModule->hasExport(spec.local.name)) {
          lastError_ = makeErrorValue(
            ErrorType::SyntaxError,
            "Module '" + *exportNamed->source + "' does not export '" + spec.local.name + "'"
          );
          return false;
        }
        ModuleBinding binding{importedModule, spec.local.name};
        exports_[spec.exported.name] = Value(binding);
        explicitExportNames.insert(spec.exported.name);
      }
      continue;
    }
    if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
      if (!exportAll->exported.has_value()) {
        continue;
      }
      auto depIt = sourceDependencies_.find(
        dependencyCacheKey(exportAll->source,
                           moduleTypeFromAttributes(exportAll->attributes)));
      if (depIt == sourceDependencies_.end() || !depIt->second) {
        lastError_ = makeErrorValue(
          ErrorType::SyntaxError,
          "Unable to resolve export source '" + exportAll->source + "'"
        );
        return false;
      }
      exports_[exportAll->exported->name] = Value(depIt->second->getNamespaceObject());
      explicitExportNames.insert(exportAll->exported->name);
    }
  }

  for (const auto& stmt : ast_->body) {
    if (!stmt) {
      continue;
    }
    auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node);
    if (!exportAll || exportAll->exported.has_value()) {
      continue;
    }

    auto depIt = sourceDependencies_.find(
      dependencyCacheKey(exportAll->source,
                         moduleTypeFromAttributes(exportAll->attributes)));
    if (depIt == sourceDependencies_.end() || !depIt->second) {
      lastError_ = makeErrorValue(
        ErrorType::SyntaxError,
        "Unable to resolve export source '" + exportAll->source + "'"
      );
      return false;
    }
    auto importedModule = depIt->second;
    auto exportNames = importedModule->getExportNames();
    for (const auto& name : exportNames) {
      if (name == "default") {
        continue;
      }
      if (exportBindings_.count(name) > 0 || explicitExportNames.count(name) > 0) {
        continue;
      }
      if (ambiguousReExports_.count(name) > 0) {
        continue;
      }

      Value candidate(ModuleBinding{importedModule, name});
      auto it = exports_.find(name);
      if (it == exports_.end()) {
        exports_[name] = candidate;
        continue;
      }

      if (!equivalentBinding(it->second, candidate)) {
        ambiguousReExports_.insert(name);
        exports_.erase(name);
      }
    }
  }

  return true;
}

std::optional<Value> Module::getExport(const std::string& name) const {
  if (ambiguousReExports_.count(name) > 0) {
    return std::nullopt;
  }
  if (name == "default" && defaultExport_.has_value()) {
    return *defaultExport_;
  }
  auto binding = exportBindings_.find(name);
  if (binding != exportBindings_.end() && environment_) {
    if (!environment_->hasLocal(binding->second)) {
      return std::nullopt;
    }
    if (environment_->isTDZ(binding->second)) {
      return std::nullopt;
    }
    if (auto value = environment_->get(binding->second)) {
      if (value->isModuleBinding()) {
        const auto& importedBinding = std::get<ModuleBinding>(value->data);
        auto importedModule = importedBinding.module.lock();
        if (!importedModule) {
          return std::nullopt;
        }
        return importedModule->getExport(importedBinding.exportName);
      }
      return *value;
    }
  }
  auto it = exports_.find(name);
  if (it != exports_.end()) {
    if (it->second.isModuleBinding()) {
      const auto& binding = std::get<ModuleBinding>(it->second.data);
      auto module = binding.module.lock();
      if (!module) {
        return std::nullopt;
      }
      return module->getExport(binding.exportName);
    }
    return it->second;
  }
  return std::nullopt;
}

std::unordered_map<std::string, Value> Module::getAllExports() const {
  std::unordered_map<std::string, Value> all;
  auto names = getExportNames();
  for (const auto& name : names) {
    if (auto value = getExport(name)) {
      all[name] = *value;
    } else {
      all[name] = Value(Undefined{});
    }
  }
  return all;
}

GCPtr<Object> Module::getNamespaceObject(bool deferred) {
  auto& namespaceSlot = deferred ? deferredNamespaceObject_ : namespaceObject_;
  if (!namespaceSlot) {
    namespaceSlot = GarbageCollector::makeGC<Object>();
    namespaceSlot->isModuleNamespace = true;
    namespaceSlot->properties["__esModule"] = Value(true);
  }

  auto moduleNamespace = namespaceSlot;
  moduleNamespace->isDeferredModuleNamespace = deferred;
  for (const auto& name : moduleNamespace->moduleExportNames) {
    moduleNamespace->properties.erase(name);
    moduleNamespace->properties.erase("__get_" + name);
  }
  moduleNamespace->moduleExportNames.clear();

  auto exportNames = getExportNames();
  moduleNamespace->moduleExportNames.reserve(exportNames.size());
  for (const auto& name : exportNames) {
    moduleNamespace->moduleExportNames.push_back(name);
    if (auto value = getExport(name)) {
      moduleNamespace->properties[name] = *value;
    } else {
      moduleNamespace->properties[name] = Value(Undefined{});
    }

    auto getter = GarbageCollector::makeGC<Function>();
    getter->isNative = true;
    getter->nativeFunc = [module = this, name](const std::vector<Value>&) -> Value {
      if (auto current = module->getExport(name)) {
        return *current;
      }
      throw std::runtime_error("ReferenceError: Cannot access '" + name + "' before initialization");
    };
    moduleNamespace->properties["__get_" + name] = Value(getter);
  }
  moduleNamespace->properties[WellKnownSymbols::toStringTagKey()] =
    Value(std::string(deferred ? "Deferred Module" : "Module"));
  return moduleNamespace;
}

}  // namespace lightjs
