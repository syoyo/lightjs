#include "module_internal.h"

namespace lightjs {

ModuleLoader::ModuleLoader() : basePath_(".") {}

std::shared_ptr<Module> ModuleLoader::loadModule(const std::string& path, ModuleType type) {
  lastError_.reset();
  std::string normalizedPath = normalizePath(path);
  std::string cacheKey = normalizedPath + "|" + moduleTypeSuffix(type);

  auto cacheIt = cache_.find(cacheKey);
  if (cacheIt != cache_.end()) {
    return cacheIt->second;
  }

  auto source = readFile(normalizedPath);
  if (!source) {
    std::cerr << "Failed to read module: " << normalizedPath << std::endl;
    lastError_ = makeErrorValue(ErrorType::Error, "Failed to load module: " + normalizedPath);
    return nullptr;
  }

  auto module = std::make_shared<Module>(normalizedPath, *source, type);
  cache_[cacheKey] = module;

  if (!module->parse()) {
    if (auto parseError = module->getLastError()) {
      lastError_ = *parseError;
    } else {
      lastError_ = makeErrorValue(ErrorType::SyntaxError, "Failed to parse module: " + normalizedPath);
    }
    cache_.erase(cacheKey);
    return nullptr;
  }

  return module;
}

std::string ModuleLoader::resolvePath(const std::string& specifier, const std::string& parentPath) {
  std::string resolved;

  if (specifier.size() >= 2 && specifier[0] == '.' && specifier[1] == '/') {
    std::string parent = parentPath.empty()
      ? (basePath_.empty() ? fs_compat::currentPath() : basePath_)
      : fs_compat::parentPath(parentPath);
    resolved = fs_compat::joinPath(parent, specifier.substr(2));
  } else if (specifier.size() >= 3 && specifier[0] == '.' && specifier[1] == '.' && specifier[2] == '/') {
    std::string parent = parentPath.empty()
      ? (basePath_.empty() ? fs_compat::currentPath() : basePath_)
      : fs_compat::parentPath(parentPath);
    resolved = fs_compat::joinPath(parent, specifier);
  } else if (!specifier.empty() && specifier[0] == '/') {
    resolved = specifier;
  } else {
    resolved = fs_compat::joinPath(fs_compat::joinPath(basePath_, "node_modules"), specifier);
    if (!fs_compat::exists(resolved)) {
      resolved = fs_compat::joinPath(basePath_, specifier);
    }
  }

  std::string ext = fs_compat::extension(resolved);
  if (ext.empty()) {
    resolved += ".js";
  }

  return resolved;
}

std::shared_ptr<Module> ModuleLoader::getCachedModule(const std::string& path) {
  auto it = cache_.find(normalizePath(path) + "|" + moduleTypeSuffix(ModuleType::JavaScript));
  if (it != cache_.end()) {
    return it->second;
  }
  return nullptr;
}

std::optional<std::string> ModuleLoader::readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
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

}  // namespace lightjs
