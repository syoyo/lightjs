#pragma once

#include "value.h"
#include <unordered_map>
#include <memory>
#include <optional>

namespace lightjs {

// Forward declarations
class ModuleLoader;
class Interpreter;

// Set global module loader for dynamic imports
void setGlobalModuleLoader(std::shared_ptr<ModuleLoader> loader);
void setGlobalInterpreter(Interpreter* interpreter);
Interpreter* getGlobalInterpreter();
class Environment : public GCObject, public std::enable_shared_from_this<Environment> {
public:
  Environment() = default;
  explicit Environment(Environment* parent);

  void define(const std::string& name, const Value& value, bool isConst = false);
  void defineLexical(const std::string& name, const Value& value, bool isConst = false);
  void defineTDZ(const std::string& name);  // Define name in temporal dead zone
  void removeTDZ(const std::string& name);  // Remove TDZ marker (initialize binding)
  bool isTDZ(const std::string& name) const;  // Check if name is in TDZ
  std::optional<Value> get(const std::string& name) const;
  bool set(const std::string& name, const Value& value);
  bool has(const std::string& name) const;
  bool hasLocal(const std::string& name) const;
  bool isConst(const std::string& name) const;
  // Delete from with-scope object: 0=not found, 1=deleted, -1=non-configurable
  int deleteFromWithScope(const std::string& name);
  // Set a var binding, bypassing with-scope objects (for var declarations)
  bool setVar(const std::string& name, const Value& value);
  // Resolve where a name would be written (returns with-scope object if applicable)
  GCPtr<Object> resolveWithScopeObject(const std::string& name) const;

  static GCPtr<Environment> createGlobal();
  GCPtr<Environment> createChild();
  Environment* getParent() const { return parent_.get(); }
  GCPtr<Object> getGlobal() const;
  Environment* getRoot();

  // GCObject interface
  const char* typeName() const override { return "Environment"; }
  void getReferences(std::vector<GCObject*>& refs) const override;

private:
  GCPtr<Environment> parent_;
  std::unordered_map<std::string, Value> bindings_;
  std::unordered_map<std::string, bool> constants_;
  std::unordered_map<std::string, bool> tdzBindings_;  // temporal dead zone
};

}
