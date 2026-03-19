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
void setGlobalArrayPrototype(const Value& proto);
Value getGlobalArrayPrototype();
GCPtr<Array> makeArrayWithPrototype();
void setGlobalObjectPrototype(const Value& proto);
GCPtr<Object> makeObjectWithPrototype();
class Environment : public GCObject, public std::enable_shared_from_this<Environment> {
public:
  Environment() = default;
  explicit Environment(Environment* parent);

  void define(const std::string& name, const Value& value, bool isConst = false);
  // Set binding directly without syncing to globalThis (for hoisting existing properties)
  void setBindingDirect(const std::string& name, const Value& value);
  void defineImmutableNFE(const std::string& name, const Value& value);
  void defineLexical(const std::string& name, const Value& value, bool isConst = false);
  void defineTDZ(const std::string& name);  // Define name in temporal dead zone
  void removeTDZ(const std::string& name);  // Remove TDZ marker (initialize binding)
  bool isTDZ(const std::string& name) const;  // Check if name is in TDZ
  std::optional<Value> get(const std::string& name) const;
  std::optional<Value> getIgnoringWith(const std::string& name) const;
  bool set(const std::string& name, const Value& value);
  bool has(const std::string& name) const;
  bool hasLocal(const std::string& name) const;
  bool hasLexicalLocal(const std::string& name) const;
  bool deleteLocalMutable(const std::string& name);
  bool isConst(const std::string& name) const;
  bool isSilentImmutable(const std::string& name) const;
  // Delete from with-scope object: 0=not found, 1=deleted, -1=non-configurable
  int deleteFromWithScope(const std::string& name);
  // Set a var binding, bypassing with-scope objects (for var declarations)
  bool setVar(const std::string& name, const Value& value);
  // Resolve the declarative environment that currently owns `name`.
  // Ignores with-scope object bindings.
  Environment* resolveBindingEnvironment(const std::string& name);
  // Resolve the original with-scope binding object/proxy for `name`.
  std::optional<Value> resolveWithScopeValue(const std::string& name) const;
  std::optional<Value> getWithScopeBindingValue(const Value& scopeValue,
                                                const std::string& name,
                                                bool strict) const;
  bool setWithScopeBindingValue(const Value& scopeValue,
                                const std::string& name,
                                const Value& value,
                                bool strict) const;
  // Resolve where a name would be written (returns with-scope object if applicable)
  GCPtr<Object> resolveWithScopeObject(const std::string& name) const;

  static GCPtr<Environment> createGlobal();
  GCPtr<Environment> createChild();
  Environment* getParent() const { return parent_.get(); }
  GCPtr<Environment> getParentPtr() const { return parent_; }
  GCPtr<Object> getGlobal() const;
  Environment* getRoot();

  // GCObject interface
  const char* typeName() const override { return "Environment"; }
  void getReferences(std::vector<GCObject*>& refs) const override;

private:
  GCPtr<Environment> parent_;
  std::unordered_map<std::string, Value> bindings_;
  std::unordered_map<std::string, bool> constants_;
  std::unordered_map<std::string, bool> silentImmutables_;  // NFE name bindings
  std::unordered_map<std::string, bool> tdzBindings_;  // temporal dead zone
  std::unordered_map<std::string, bool> lexicalBindings_;
};

}
