#include "module.h"
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace tinyjs {

Module::Module(const std::string& path, const std::string& source)
  : path_(path), source_(source), state_(State::Uninstantiated) {}

bool Module::parse() {
  try {
    Lexer lexer(source_);
    auto tokens = lexer.tokenize();

    Parser parser(tokens);
    ast_ = parser.parse();

    if (ast_) {
      ast_->isModule = true;  // Mark as module
      return true;
    }
  } catch (const std::exception& e) {
    std::cerr << "Module parse error in " << path_ << ": " << e.what() << std::endl;
  }

  return false;
}

bool Module::instantiate(ModuleLoader* loader) {
  if (state_ != State::Uninstantiated) {
    return state_ >= State::Instantiated;
  }

  state_ = State::Instantiating;

  if (!ast_) {
    if (!parse()) {
      return false;
    }
  }

  // Create module environment
  environment_ = Environment::createGlobal();

  // Process imports
  for (const auto& stmt : ast_->body) {
    if (auto* importDecl = std::get_if<ImportDeclaration>(&stmt->node)) {
      // Resolve and load the imported module
      std::string resolvedPath = loader->resolvePath(importDecl->source, path_);
      auto importedModule = loader->loadModule(resolvedPath);

      if (!importedModule) {
        std::cerr << "Failed to load module: " << importDecl->source << std::endl;
        return false;
      }

      // Instantiate the imported module if needed
      if (!importedModule->instantiate(loader)) {
        return false;
      }

      dependencies_.push_back(importedModule);

      // Bind imports to local environment
      if (importDecl->defaultImport) {
        auto defaultExport = importedModule->getExport("default");
        if (defaultExport) {
          environment_->define(importDecl->defaultImport->name, *defaultExport);
        }
      }

      if (importDecl->namespaceImport) {
        // Create namespace object with all exports
        auto namespaceObj = std::make_shared<Object>();
        auto exports = importedModule->getAllExports();
        for (const auto& [name, value] : exports) {
          namespaceObj->properties[name] = value;
        }
        environment_->define(importDecl->namespaceImport->name, Value(namespaceObj));
      }

      for (const auto& spec : importDecl->specifiers) {
        auto exportValue = importedModule->getExport(spec.imported.name);
        if (exportValue) {
          environment_->define(spec.local.name, *exportValue);
        } else {
          std::cerr << "Module " << importDecl->source << " does not export '"
                    << spec.imported.name << "'" << std::endl;
          return false;
        }
      }
    }
  }

  state_ = State::Instantiated;
  return true;
}

bool Module::evaluate(Interpreter* interpreter) {
  if (state_ == State::Evaluated) {
    return true;
  }

  if (state_ != State::Instantiated) {
    std::cerr << "Module must be instantiated before evaluation" << std::endl;
    return false;
  }

  state_ = State::Evaluating;

  // Evaluate dependencies first
  for (const auto& dep : dependencies_) {
    if (!dep->evaluate(interpreter)) {
      return false;
    }
  }

  // Save current interpreter environment
  auto prevEnv = interpreter->getEnvironment();
  interpreter->setEnvironment(environment_);

  // Evaluate module body
  try {
    for (const auto& stmt : ast_->body) {
      // Handle exports
      if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt->node)) {
        if (exportNamed->declaration) {
          // Export declaration (export const x = 1)
          auto task = interpreter->evaluate(*exportNamed->declaration);
          while (!task.done()) {
            std::coroutine_handle<>::from_address(task.handle.address()).resume();
          }

          // Extract exported bindings
          if (auto* varDecl = std::get_if<VarDeclaration>(&exportNamed->declaration->node)) {
            for (const auto& decl : varDecl->declarations) {
              // Only support simple identifier patterns in exports for now
              if (auto* id = std::get_if<Identifier>(&decl.pattern->node)) {
                auto value = environment_->get(id->name);
                if (value) {
                  exports_[id->name] = *value;
                }
              }
            }
          } else if (auto* funcDecl = std::get_if<FunctionDeclaration>(&exportNamed->declaration->node)) {
            auto value = environment_->get(funcDecl->id.name);
            if (value) {
              exports_[funcDecl->id.name] = *value;
            }
          }
        } else if (!exportNamed->specifiers.empty()) {
          // Export list (export { x, y as z })
          for (const auto& spec : exportNamed->specifiers) {
            auto value = environment_->get(spec.local.name);
            if (value) {
              exports_[spec.exported.name] = *value;
            }
          }
        }
      } else if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt->node)) {
        // Export default
        auto task = interpreter->evaluate(*exportDefault->declaration);
        while (!task.done()) {
          std::coroutine_handle<>::from_address(task.handle.address()).resume();
        }
        defaultExport_ = task.result();
        exports_["default"] = *defaultExport_;
      } else if (auto* exportAll = std::get_if<ExportAllDeclaration>(&stmt->node)) {
        // Export all (export * from "module")
        // This requires re-exporting from another module
        // Implementation would go here
      } else {
        // Regular statement
        auto task = interpreter->evaluate(*stmt);
        while (!task.done()) {
          std::coroutine_handle<>::from_address(task.handle.address()).resume();
        }
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "Module evaluation error in " << path_ << ": " << e.what() << std::endl;
    interpreter->setEnvironment(prevEnv);
    return false;
  }

  // Restore interpreter environment
  interpreter->setEnvironment(prevEnv);

  state_ = State::Evaluated;
  return true;
}

std::optional<Value> Module::getExport(const std::string& name) const {
  auto it = exports_.find(name);
  if (it != exports_.end()) {
    return it->second;
  }
  return std::nullopt;
}

std::unordered_map<std::string, Value> Module::getAllExports() const {
  return exports_;
}

// ModuleLoader implementation

ModuleLoader::ModuleLoader() : basePath_(".") {}

std::shared_ptr<Module> ModuleLoader::loadModule(const std::string& path) {
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
    return nullptr;
  }

  // Create and cache module
  auto module = std::make_shared<Module>(normalizedPath, *source);
  cache_[normalizedPath] = module;

  // Parse the module
  if (!module->parse()) {
    cache_.erase(normalizedPath);
    return nullptr;
  }

  return module;
}

std::string ModuleLoader::resolvePath(const std::string& specifier, const std::string& parentPath) {
  fs::path resolved;

  // Handle relative paths
  if (specifier.starts_with("./") || specifier.starts_with("../")) {
    fs::path parent = parentPath.empty() ? fs::current_path() : fs::path(parentPath).parent_path();
    resolved = parent / specifier;
  }
  // Handle absolute paths
  else if (specifier.starts_with("/")) {
    resolved = fs::path(specifier);
  }
  // Handle node_modules style imports (simplified)
  else {
    resolved = fs::path(basePath_) / "node_modules" / specifier;
    if (!fs::exists(resolved)) {
      resolved = fs::path(basePath_) / specifier;
    }
  }

  // Add .js extension if not present
  if (resolved.extension().empty()) {
    resolved += ".js";
  }

  return resolved.string();
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
    return fs::canonical(path).string();
  } catch (const fs::filesystem_error&) {
    return fs::absolute(path).string();
  }
}

} // namespace tinyjs