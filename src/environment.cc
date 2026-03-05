#include "environment.h"
#include "crypto.h"
#include "http.h"
#include "gc.h"
#include "json.h"
#include "object_methods.h"
#include "array_methods.h"
#include "string_methods.h"
#include "math_object.h"
#include "date_object.h"
#include "event_loop.h"
#include "symbols.h"
#include "module.h"
#include "interpreter.h"
#include "wasm_js.h"
#include "unicode.h"
#include "text_encoding.h"
#include "url.h"
#include "fs.h"
#include "streams.h"
#include "lexer.h"
#include "parser.h"
#include <iostream>
#include <thread>
#include <limits>
#include <cmath>
#include <random>
#include <cctype>
#include <array>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <regex>
#include <unordered_set>

namespace lightjs {

namespace {

std::string trimAsciiWhitespace(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
    start++;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    end--;
  }
  return s.substr(start, end - start);
}

int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 10 + (c - 'A');
  return -1;
}


bool isModuleNamespaceExportKey(const GCPtr<Object>& obj, const std::string& key) {
  return std::find(obj->moduleExportNames.begin(), obj->moduleExportNames.end(), key) !=
         obj->moduleExportNames.end();
}

constexpr const char* kImportPhaseSourceSentinel = "__lightjs_import_phase_source__";
constexpr const char* kImportPhaseDeferSentinel = "__lightjs_import_phase_defer__";
constexpr const char* kWithScopeObjectBinding = "__with_scope_object__";

bool isInternalPropertyKeyForReflection(const std::string& key) {
  // Hide LightJS internal bookkeeping keys from reflection APIs.
  // Do NOT hide arbitrary "__user__" property names: Test262 relies on them.
  if (key.rfind("__non_writable_", 0) == 0) return true;
  if (key.rfind("__non_enum_", 0) == 0) return true;
  if (key.rfind("__non_configurable_", 0) == 0) return true;
  if (key.rfind("__enum_", 0) == 0) return true;
  if (key.rfind("__get_", 0) == 0) return true;
  if (key.rfind("__set_", 0) == 0) return true;
  if (key.rfind("__mapped_arg_index_", 0) == 0) return true;
  if (key.rfind("__mapped_arg_name_", 0) == 0) return true;

  static const std::unordered_set<std::string> internalKeys = {
    "__callable_object__",
    "__constructor_wrapper__",
    "__constructor__",
    "__primitive_value__",
    "__is_arguments_object__",
    "__throw_on_new__",
    "__is_arrow_function__",
    "__named_expression__",
    "__bound_target__",
    "__bound_this__",
    "__bound_args__",
    "__reflect_construct__",
    "__eval_deletable_bindings__",
    "__builtin_array_iterator__",
    "__in_class_field_initializer__",
    "__super_called__",
  };
  return internalKeys.count(key) > 0;
}

bool isVisibleWithIdentifier(const std::string& name) {
  if (name.empty()) {
    return false;
  }
  // Hide internal sentinels like "__foo__", but allow user identifiers that
  // merely begin with underscores (e.g. "__x").
  if (name.size() >= 4 && name.rfind("__", 0) == 0 &&
      name.substr(name.size() - 2) == "__") {
    return false;
  }
  return true;
}

bool isBlockedByUnscopables(const GCPtr<Object>& bindings, const std::string& name) {
  if (!bindings || !isVisibleWithIdentifier(name)) {
    return false;
  }

  const auto& unscopablesKey = WellKnownSymbols::unscopablesKey();
  Value unscopablesValue(Undefined{});
  auto unscopablesGetterIt = bindings->properties.find("__get_" + unscopablesKey);
  if (unscopablesGetterIt != bindings->properties.end() &&
      unscopablesGetterIt->second.isFunction()) {
    if (auto* interpreter = getGlobalInterpreter()) {
      unscopablesValue = interpreter->callForHarness(unscopablesGetterIt->second, {}, Value(bindings));
      if (interpreter->hasError()) {
        return true;
      }
    }
  } else {
    auto unscopablesIt = bindings->properties.find(unscopablesKey);
    if (unscopablesIt != bindings->properties.end()) {
      unscopablesValue = unscopablesIt->second;
    }
  }
  if (!unscopablesValue.isObject()) {
    return false;
  }

  std::unordered_set<Object*> visited;
  auto current = unscopablesValue.getGC<Object>();
  int depth = 0;
  while (current && depth < 64 && visited.insert(current.get()).second) {
    auto it = current->properties.find(name);
    if (it != current->properties.end()) {
      return it->second.toBool();
    }
    auto protoIt = current->properties.find("__proto__");
    if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
      break;
    }
    current = protoIt->second.getGC<Object>();
    depth++;
  }
  return false;
}

std::optional<Value> lookupWithScopeProperty(const Value& scopeValue, const std::string& name) {
  if (!isVisibleWithIdentifier(name)) {
    return std::nullopt;
  }

  Value lookupScope = scopeValue;
  if (scopeValue.isProxy()) {
    auto proxyPtr = scopeValue.getGC<Proxy>();
    if (!proxyPtr->target || !proxyPtr->target->isObject()) {
      return std::nullopt;
    }
    lookupScope = *proxyPtr->target;
    if (proxyPtr->handler && proxyPtr->handler->isObject()) {
      auto handlerObj = proxyPtr->handler->getGC<Object>();
      auto hasIt = handlerObj->properties.find("has");
      if (hasIt != handlerObj->properties.end() && hasIt->second.isFunction()) {
        if (auto* interpreter = getGlobalInterpreter()) {
          Value hasResult = interpreter->callForHarness(
            hasIt->second, {*proxyPtr->target, Value(name)}, Value(handlerObj));
          if (interpreter->hasError()) {
            return std::nullopt;
          }
          if (!hasResult.toBool()) {
            return std::nullopt;
          }
        }
      }
    }
  }
  if (!lookupScope.isObject()) {
    return std::nullopt;
  }

  std::unordered_set<Object*> visited;
  auto bindings = lookupScope.getGC<Object>();
  auto current = bindings;
  int depth = 0;
  while (current && depth < 64 && visited.insert(current.get()).second) {
    auto it = current->properties.find(name);
    if (it != current->properties.end()) {
      if (isBlockedByUnscopables(bindings, name)) {
        return std::nullopt;
      }
      return it->second;
    }
    auto getterIt = current->properties.find("__get_" + name);
    if (getterIt != current->properties.end()) {
      if (isBlockedByUnscopables(bindings, name)) {
        return std::nullopt;
      }
      return Value(Undefined{});
    }
    auto setterIt = current->properties.find("__set_" + name);
    if (setterIt != current->properties.end()) {
      if (isBlockedByUnscopables(bindings, name)) {
        return std::nullopt;
      }
      return Value(Undefined{});
    }
    auto protoIt = current->properties.find("__proto__");
    if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
      break;
    }
    current = protoIt->second.getGC<Object>();
    depth++;
  }
  return std::nullopt;
}

bool setWithScopeProperty(const Value& scopeValue, const std::string& name, const Value& value) {
  if (!isVisibleWithIdentifier(name) || !scopeValue.isObject()) {
    return false;
  }

  auto receiver = scopeValue.getGC<Object>();
  if (isBlockedByUnscopables(receiver, name)) {
    return false;
  }
  std::unordered_set<Object*> visited;
  auto current = receiver;
  int depth = 0;
  while (current && depth < 64 && visited.insert(current.get()).second) {
    auto it = current->properties.find(name);
    if (it != current->properties.end()) {
      receiver->properties[name] = value;
      return true;
    }
    auto protoIt = current->properties.find("__proto__");
    if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
      break;
    }
    current = protoIt->second.getGC<Object>();
    depth++;
  }
  return false;
}

bool deleteWithScopeProperty(const Value& scopeValue, const std::string& name) {
  if (!isVisibleWithIdentifier(name) || !scopeValue.isObject()) {
    return false;
  }

  auto receiver = scopeValue.getGC<Object>();
  // Check own properties only for delete (no prototype chain)
  auto it = receiver->properties.find(name);
  if (it != receiver->properties.end()) {
    // Check non-configurable
    if (receiver->properties.count("__non_configurable_" + name)) {
      return false;  // Can't delete non-configurable properties
    }
    receiver->properties.erase(name);
    receiver->properties.erase("__non_writable_" + name);
    receiver->properties.erase("__non_enum_" + name);
    receiver->properties.erase("__enum_" + name);
    return true;
  }
  return false;
}

void collectVarNamesFromPattern(const Expression& pattern, std::vector<std::string>& names) {
  if (auto* id = std::get_if<Identifier>(&pattern.node)) {
    names.push_back(id->name);
    return;
  }
  if (auto* assign = std::get_if<AssignmentPattern>(&pattern.node)) {
    if (assign->left) {
      collectVarNamesFromPattern(*assign->left, names);
    }
    return;
  }
  if (auto* arr = std::get_if<ArrayPattern>(&pattern.node)) {
    for (const auto& elem : arr->elements) {
      if (elem) {
        collectVarNamesFromPattern(*elem, names);
      }
    }
    if (arr->rest) {
      collectVarNamesFromPattern(*arr->rest, names);
    }
    return;
  }
  if (auto* obj = std::get_if<ObjectPattern>(&pattern.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.value) {
        collectVarNamesFromPattern(*prop.value, names);
      }
    }
    if (obj->rest) {
      collectVarNamesFromPattern(*obj->rest, names);
    }
  }
}

void collectVarNamesFromStatement(const Statement& stmt, std::vector<std::string>& names) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    if (varDecl->kind == VarDeclaration::Kind::Var) {
      for (const auto& declarator : varDecl->declarations) {
        if (declarator.pattern) {
          collectVarNamesFromPattern(*declarator.pattern, names);
        }
      }
    }
    return;
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& inner : block->body) {
      if (inner) {
        collectVarNamesFromStatement(*inner, names);
      }
    }
    return;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->consequent) {
      collectVarNamesFromStatement(*ifStmt->consequent, names);
    }
    if (ifStmt->alternate) {
      collectVarNamesFromStatement(*ifStmt->alternate, names);
    }
    return;
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->body) {
      collectVarNamesFromStatement(*whileStmt->body, names);
    }
    return;
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body) {
      collectVarNamesFromStatement(*doWhileStmt->body, names);
    }
    return;
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init) {
      collectVarNamesFromStatement(*forStmt->init, names);
    }
    if (forStmt->body) {
      collectVarNamesFromStatement(*forStmt->body, names);
    }
    return;
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left) {
      collectVarNamesFromStatement(*forInStmt->left, names);
    }
    if (forInStmt->body) {
      collectVarNamesFromStatement(*forInStmt->body, names);
    }
    return;
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left) {
      collectVarNamesFromStatement(*forOfStmt->left, names);
    }
    if (forOfStmt->body) {
      collectVarNamesFromStatement(*forOfStmt->body, names);
    }
    return;
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    for (const auto& caseClause : switchStmt->cases) {
      for (const auto& cons : caseClause.consequent) {
        if (cons) {
          collectVarNamesFromStatement(*cons, names);
        }
      }
    }
    return;
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& inner : tryStmt->block) {
      if (inner) {
        collectVarNamesFromStatement(*inner, names);
      }
    }
    if (tryStmt->hasHandler) {
      for (const auto& inner : tryStmt->handler.body) {
        if (inner) {
          collectVarNamesFromStatement(*inner, names);
        }
      }
    }
    if (tryStmt->hasFinalizer) {
      for (const auto& inner : tryStmt->finalizer) {
        if (inner) {
          collectVarNamesFromStatement(*inner, names);
        }
      }
    }
    return;
  }
  if (auto* labelled = std::get_if<LabelledStmt>(&stmt.node)) {
    if (labelled->body) {
      collectVarNamesFromStatement(*labelled->body, names);
    }
    return;
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->body) {
      collectVarNamesFromStatement(*withStmt->body, names);
    }
  }
}

void collectDeclaredNamesForDirectEval(const Statement& stmt, std::vector<std::string>& names) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& declarator : varDecl->declarations) {
      if (declarator.pattern) {
        collectVarNamesFromPattern(*declarator.pattern, names);
      }
    }
    return;
  }
  if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt.node)) {
    if (!funcDecl->id.name.empty()) {
      names.push_back(funcDecl->id.name);
    }
    return;
  }
  if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt.node)) {
    if (!classDecl->id.name.empty()) {
      names.push_back(classDecl->id.name);
    }
    return;
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& inner : block->body) {
      if (inner) {
        collectDeclaredNamesForDirectEval(*inner, names);
      }
    }
    return;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->consequent) {
      collectDeclaredNamesForDirectEval(*ifStmt->consequent, names);
    }
    if (ifStmt->alternate) {
      collectDeclaredNamesForDirectEval(*ifStmt->alternate, names);
    }
    return;
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->body) {
      collectDeclaredNamesForDirectEval(*whileStmt->body, names);
    }
    return;
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body) {
      collectDeclaredNamesForDirectEval(*doWhileStmt->body, names);
    }
    return;
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init) {
      collectDeclaredNamesForDirectEval(*forStmt->init, names);
    }
    if (forStmt->body) {
      collectDeclaredNamesForDirectEval(*forStmt->body, names);
    }
    return;
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left) {
      collectDeclaredNamesForDirectEval(*forInStmt->left, names);
    }
    if (forInStmt->body) {
      collectDeclaredNamesForDirectEval(*forInStmt->body, names);
    }
    return;
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left) {
      collectDeclaredNamesForDirectEval(*forOfStmt->left, names);
    }
    if (forOfStmt->body) {
      collectDeclaredNamesForDirectEval(*forOfStmt->body, names);
    }
    return;
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    for (const auto& caseClause : switchStmt->cases) {
      for (const auto& cons : caseClause.consequent) {
        if (cons) {
          collectDeclaredNamesForDirectEval(*cons, names);
        }
      }
    }
    return;
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& inner : tryStmt->block) {
      if (inner) {
        collectDeclaredNamesForDirectEval(*inner, names);
      }
    }
    if (tryStmt->hasHandler) {
      for (const auto& inner : tryStmt->handler.body) {
        if (inner) {
          collectDeclaredNamesForDirectEval(*inner, names);
        }
      }
    }
    if (tryStmt->hasFinalizer) {
      for (const auto& inner : tryStmt->finalizer) {
        if (inner) {
          collectDeclaredNamesForDirectEval(*inner, names);
        }
      }
    }
    return;
  }
  if (auto* labelled = std::get_if<LabelledStmt>(&stmt.node)) {
    if (labelled->body) {
      collectDeclaredNamesForDirectEval(*labelled->body, names);
    }
    return;
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->body) {
      collectDeclaredNamesForDirectEval(*withStmt->body, names);
    }
  }
}

bool bodyHasUseStrictDirective(const std::vector<StmtPtr>& body) {
  for (const auto& stmt : body) {
    if (!stmt) {
      break;
    }
    auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
    if (!exprStmt || !exprStmt->expression) {
      break;
    }
    auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node);
    if (!str) {
      break;
    }
    if (str->value == "use strict") {
      return true;
    }
  }
  return false;
}

bool expressionContainsSuperForEval(const Expression& expr);
bool statementContainsSuperForEval(const Statement& stmt);
bool expressionContainsSuperCallForEval(const Expression& expr);
bool statementContainsSuperCallForEval(const Statement& stmt);
bool expressionContainsArgumentsForEval(const Expression& expr);
bool statementContainsArgumentsForEval(const Statement& stmt);
bool expressionContainsNewTargetForEval(const Expression& expr);
bool statementContainsNewTargetForEval(const Statement& stmt);

bool expressionContainsSuperForEval(const Expression& expr) {
  if (std::holds_alternative<SuperExpr>(expr.node)) return true;
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsSuperForEval(*binary->left)) ||
           (binary->right && expressionContainsSuperForEval(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsSuperForEval(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsSuperForEval(*assign->left)) ||
           (assign->right && expressionContainsSuperForEval(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsSuperForEval(*update->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsSuperForEval(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsSuperForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsSuperForEval(*member->object)) ||
           (member->property && expressionContainsSuperForEval(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsSuperForEval(*cond->test)) ||
           (cond->consequent && expressionContainsSuperForEval(*cond->consequent)) ||
           (cond->alternate && expressionContainsSuperForEval(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsSuperForEval(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsSuperForEval(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsSuperForEval(*prop.key)) return true;
      if (prop.value && expressionContainsSuperForEval(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsSuperForEval(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsSuperForEval(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsSuperForEval(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsSuperForEval(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsSuperForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& e : arrPat->elements) {
      if (e && expressionContainsSuperForEval(*e)) return true;
    }
    return arrPat->rest && expressionContainsSuperForEval(*arrPat->rest);
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.key && expressionContainsSuperForEval(*prop.key)) return true;
      if (prop.value && expressionContainsSuperForEval(*prop.value)) return true;
    }
    return objPat->rest && expressionContainsSuperForEval(*objPat->rest);
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsSuperForEval(*assignPat->left)) ||
           (assignPat->right && expressionContainsSuperForEval(*assignPat->right));
  }
  return false;
}

bool statementContainsSuperForEval(const Statement& stmt) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern && expressionContainsSuperForEval(*decl.pattern)) return true;
      if (decl.init && expressionContainsSuperForEval(*decl.init)) return true;
    }
    return false;
  }
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsSuperForEval(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsSuperForEval(*ret->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsSuperForEval(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsSuperForEval(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsSuperForEval(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsSuperForEval(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsSuperForEval(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsSuperForEval(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsSuperForEval(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsSuperForEval(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsSuperForEval(*forStmt->update)) return true;
    return forStmt->body && statementContainsSuperForEval(*forStmt->body);
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsSuperForEval(*withStmt->object)) return true;
    return withStmt->body && statementContainsSuperForEval(*withStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsSuperForEval(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsSuperForEval(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsSuperForEval(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsSuperForEval(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsSuperForEval(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsSuperForEval(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsSuperForEval(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsSuperForEval(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsSuperForEval(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsSuperForEval(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsSuperForEval(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsSuperForEval(*label->body);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsSuperForEval(*throwStmt->argument);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsSuperForEval(*s)) return true;
    }
    if (tryStmt->handler.paramPattern && expressionContainsSuperForEval(*tryStmt->handler.paramPattern)) return true;
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsSuperForEval(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsSuperForEval(*s)) return true;
    }
    return false;
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration && expressionContainsSuperForEval(*exportDefault->declaration);
  }
  return false;
}

bool expressionContainsSuperCallForEval(const Expression& expr) {
  if (auto* fn = std::get_if<FunctionExpr>(&expr.node)) {
    // Arrow functions inherit super-call semantics from the surrounding
    // context, so Contains(SuperCall) must recurse into arrow bodies.
    if (!fn->isArrow) {
      return false;
    }
    for (const auto& p : fn->params) {
      if (p.defaultValue && expressionContainsSuperCallForEval(*p.defaultValue)) {
        return true;
      }
    }
    for (const auto& s : fn->body) {
      if (s && statementContainsSuperCallForEval(*s)) {
        return true;
      }
    }
    return false;
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && std::holds_alternative<SuperExpr>(call->callee->node)) {
      return true;
    }
    if (call->callee && expressionContainsSuperCallForEval(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsSuperCallForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsSuperCallForEval(*binary->left)) ||
           (binary->right && expressionContainsSuperCallForEval(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsSuperCallForEval(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsSuperCallForEval(*assign->left)) ||
           (assign->right && expressionContainsSuperCallForEval(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsSuperCallForEval(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsSuperCallForEval(*member->object)) ||
           (member->property && expressionContainsSuperCallForEval(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsSuperCallForEval(*cond->test)) ||
           (cond->consequent && expressionContainsSuperCallForEval(*cond->consequent)) ||
           (cond->alternate && expressionContainsSuperCallForEval(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsSuperCallForEval(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsSuperCallForEval(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsSuperCallForEval(*prop.key)) return true;
      if (prop.value && expressionContainsSuperCallForEval(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsSuperCallForEval(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsSuperCallForEval(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsSuperCallForEval(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsSuperCallForEval(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsSuperCallForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsSuperCallForEval(*assignPat->left)) ||
           (assignPat->right && expressionContainsSuperCallForEval(*assignPat->right));
  }
  return false;
}

bool statementContainsSuperCallForEval(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsSuperCallForEval(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsSuperCallForEval(*ret->argument);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsSuperCallForEval(*throwStmt->argument);
  }
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern && expressionContainsSuperCallForEval(*decl.pattern)) return true;
      if (decl.init && expressionContainsSuperCallForEval(*decl.init)) return true;
    }
    return false;
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsSuperCallForEval(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsSuperCallForEval(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsSuperCallForEval(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsSuperCallForEval(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsSuperCallForEval(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsSuperCallForEval(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsSuperCallForEval(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsSuperCallForEval(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsSuperCallForEval(*forStmt->update)) return true;
    return forStmt->body && statementContainsSuperCallForEval(*forStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsSuperCallForEval(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsSuperCallForEval(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsSuperCallForEval(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsSuperCallForEval(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsSuperCallForEval(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsSuperCallForEval(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsSuperCallForEval(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsSuperCallForEval(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsSuperCallForEval(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsSuperCallForEval(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsSuperCallForEval(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsSuperCallForEval(*label->body);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsSuperCallForEval(*s)) return true;
    }
    if (tryStmt->handler.paramPattern && expressionContainsSuperCallForEval(*tryStmt->handler.paramPattern)) return true;
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsSuperCallForEval(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsSuperCallForEval(*s)) return true;
    }
    return false;
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsSuperCallForEval(*withStmt->object)) return true;
    return withStmt->body && statementContainsSuperCallForEval(*withStmt->body);
  }
  return false;
}

bool expressionContainsArgumentsForEval(const Expression& expr) {
  if (auto* id = std::get_if<Identifier>(&expr.node)) {
    return id->name == "arguments";
  }
  if (auto* fn = std::get_if<FunctionExpr>(&expr.node)) {
    if (!fn->isArrow) {
      return false;
    }
    for (const auto& p : fn->params) {
      if (p.defaultValue && expressionContainsArgumentsForEval(*p.defaultValue)) {
        return true;
      }
    }
    for (const auto& s : fn->body) {
      if (s && statementContainsArgumentsForEval(*s)) {
        return true;
      }
    }
    return false;
  }
  if (std::holds_alternative<ClassExpr>(expr.node)) {
    return false;
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsArgumentsForEval(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsArgumentsForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsArgumentsForEval(*binary->left)) ||
           (binary->right && expressionContainsArgumentsForEval(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsArgumentsForEval(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsArgumentsForEval(*assign->left)) ||
           (assign->right && expressionContainsArgumentsForEval(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsArgumentsForEval(*update->argument);
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsArgumentsForEval(*member->object)) ||
           (member->property && expressionContainsArgumentsForEval(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsArgumentsForEval(*cond->test)) ||
           (cond->consequent && expressionContainsArgumentsForEval(*cond->consequent)) ||
           (cond->alternate && expressionContainsArgumentsForEval(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsArgumentsForEval(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsArgumentsForEval(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsArgumentsForEval(*prop.key)) return true;
      if (prop.value && expressionContainsArgumentsForEval(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsArgumentsForEval(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsArgumentsForEval(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsArgumentsForEval(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsArgumentsForEval(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsArgumentsForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* tmpl = std::get_if<TemplateLiteral>(&expr.node)) {
    for (const auto& e : tmpl->expressions) {
      if (e && expressionContainsArgumentsForEval(*e)) return true;
    }
    return false;
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsArgumentsForEval(*assignPat->left)) ||
           (assignPat->right && expressionContainsArgumentsForEval(*assignPat->right));
  }
  return false;
}

bool statementContainsArgumentsForEval(const Statement& stmt) {
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsArgumentsForEval(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsArgumentsForEval(*ret->argument);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsArgumentsForEval(*throwStmt->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsArgumentsForEval(*s)) return true;
    }
    return false;
  }
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& d : varDecl->declarations) {
      if (d.pattern && expressionContainsArgumentsForEval(*d.pattern)) return true;
      if (d.init && expressionContainsArgumentsForEval(*d.init)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsArgumentsForEval(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsArgumentsForEval(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsArgumentsForEval(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsArgumentsForEval(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsArgumentsForEval(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsArgumentsForEval(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsArgumentsForEval(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsArgumentsForEval(*forStmt->update)) return true;
    return forStmt->body && statementContainsArgumentsForEval(*forStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsArgumentsForEval(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsArgumentsForEval(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsArgumentsForEval(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsArgumentsForEval(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsArgumentsForEval(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsArgumentsForEval(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsArgumentsForEval(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsArgumentsForEval(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsArgumentsForEval(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsArgumentsForEval(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsArgumentsForEval(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsArgumentsForEval(*label->body);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsArgumentsForEval(*s)) return true;
    }
    if (tryStmt->handler.paramPattern &&
        expressionContainsArgumentsForEval(*tryStmt->handler.paramPattern)) {
      return true;
    }
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsArgumentsForEval(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsArgumentsForEval(*s)) return true;
    }
    return false;
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsArgumentsForEval(*withStmt->object)) return true;
    return withStmt->body && statementContainsArgumentsForEval(*withStmt->body);
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration &&
           expressionContainsArgumentsForEval(*exportDefault->declaration);
  }
  return false;
}

bool expressionContainsNewTargetForEval(const Expression& expr) {
  if (auto* fn = std::get_if<FunctionExpr>(&expr.node)) {
    if (!fn->isArrow) {
      return false;
    }
    for (const auto& p : fn->params) {
      if (p.defaultValue && expressionContainsNewTargetForEval(*p.defaultValue)) {
        return true;
      }
    }
    for (const auto& s : fn->body) {
      if (s && statementContainsNewTargetForEval(*s)) {
        return true;
      }
    }
    return false;
  }
  if (auto* meta = std::get_if<MetaProperty>(&expr.node)) {
    return meta->meta == "new" && meta->property == "target";
  }
  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (binary->left && expressionContainsNewTargetForEval(*binary->left)) ||
           (binary->right && expressionContainsNewTargetForEval(*binary->right));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return unary->argument && expressionContainsNewTargetForEval(*unary->argument);
  }
  if (auto* assign = std::get_if<AssignmentExpr>(&expr.node)) {
    return (assign->left && expressionContainsNewTargetForEval(*assign->left)) ||
           (assign->right && expressionContainsNewTargetForEval(*assign->right));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return update->argument && expressionContainsNewTargetForEval(*update->argument);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee && expressionContainsNewTargetForEval(*call->callee)) return true;
    for (const auto& arg : call->arguments) {
      if (arg && expressionContainsNewTargetForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    return (member->object && expressionContainsNewTargetForEval(*member->object)) ||
           (member->property && expressionContainsNewTargetForEval(*member->property));
  }
  if (auto* cond = std::get_if<ConditionalExpr>(&expr.node)) {
    return (cond->test && expressionContainsNewTargetForEval(*cond->test)) ||
           (cond->consequent && expressionContainsNewTargetForEval(*cond->consequent)) ||
           (cond->alternate && expressionContainsNewTargetForEval(*cond->alternate));
  }
  if (auto* seq = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& e : seq->expressions) {
      if (e && expressionContainsNewTargetForEval(*e)) return true;
    }
    return false;
  }
  if (auto* arr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& e : arr->elements) {
      if (e && expressionContainsNewTargetForEval(*e)) return true;
    }
    return false;
  }
  if (auto* obj = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : obj->properties) {
      if (prop.key && expressionContainsNewTargetForEval(*prop.key)) return true;
      if (prop.value && expressionContainsNewTargetForEval(*prop.value)) return true;
    }
    return false;
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return awaitExpr->argument && expressionContainsNewTargetForEval(*awaitExpr->argument);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return yieldExpr->argument && expressionContainsNewTargetForEval(*yieldExpr->argument);
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return spread->argument && expressionContainsNewTargetForEval(*spread->argument);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee && expressionContainsNewTargetForEval(*newExpr->callee)) return true;
    for (const auto& arg : newExpr->arguments) {
      if (arg && expressionContainsNewTargetForEval(*arg)) return true;
    }
    return false;
  }
  if (auto* arrPat = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& e : arrPat->elements) {
      if (e && expressionContainsNewTargetForEval(*e)) return true;
    }
    return arrPat->rest && expressionContainsNewTargetForEval(*arrPat->rest);
  }
  if (auto* objPat = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPat->properties) {
      if (prop.key && expressionContainsNewTargetForEval(*prop.key)) return true;
      if (prop.value && expressionContainsNewTargetForEval(*prop.value)) return true;
    }
    return objPat->rest && expressionContainsNewTargetForEval(*objPat->rest);
  }
  if (auto* assignPat = std::get_if<AssignmentPattern>(&expr.node)) {
    return (assignPat->left && expressionContainsNewTargetForEval(*assignPat->left)) ||
           (assignPat->right && expressionContainsNewTargetForEval(*assignPat->right));
  }
  return false;
}

bool statementContainsNewTargetForEval(const Statement& stmt) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern && expressionContainsNewTargetForEval(*decl.pattern)) return true;
      if (decl.init && expressionContainsNewTargetForEval(*decl.init)) return true;
    }
    return false;
  }
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return exprStmt->expression && expressionContainsNewTargetForEval(*exprStmt->expression);
  }
  if (auto* ret = std::get_if<ReturnStmt>(&stmt.node)) {
    return ret->argument && expressionContainsNewTargetForEval(*ret->argument);
  }
  if (auto* block = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& s : block->body) {
      if (s && statementContainsNewTargetForEval(*s)) return true;
    }
    return false;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    if (ifStmt->test && expressionContainsNewTargetForEval(*ifStmt->test)) return true;
    if (ifStmt->consequent && statementContainsNewTargetForEval(*ifStmt->consequent)) return true;
    return ifStmt->alternate && statementContainsNewTargetForEval(*ifStmt->alternate);
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    if (whileStmt->test && expressionContainsNewTargetForEval(*whileStmt->test)) return true;
    return whileStmt->body && statementContainsNewTargetForEval(*whileStmt->body);
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    if (forStmt->init && statementContainsNewTargetForEval(*forStmt->init)) return true;
    if (forStmt->test && expressionContainsNewTargetForEval(*forStmt->test)) return true;
    if (forStmt->update && expressionContainsNewTargetForEval(*forStmt->update)) return true;
    return forStmt->body && statementContainsNewTargetForEval(*forStmt->body);
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    if (withStmt->object && expressionContainsNewTargetForEval(*withStmt->object)) return true;
    return withStmt->body && statementContainsNewTargetForEval(*withStmt->body);
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    if (forInStmt->left && statementContainsNewTargetForEval(*forInStmt->left)) return true;
    if (forInStmt->right && expressionContainsNewTargetForEval(*forInStmt->right)) return true;
    return forInStmt->body && statementContainsNewTargetForEval(*forInStmt->body);
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    if (forOfStmt->left && statementContainsNewTargetForEval(*forOfStmt->left)) return true;
    if (forOfStmt->right && expressionContainsNewTargetForEval(*forOfStmt->right)) return true;
    return forOfStmt->body && statementContainsNewTargetForEval(*forOfStmt->body);
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    if (doWhileStmt->body && statementContainsNewTargetForEval(*doWhileStmt->body)) return true;
    return doWhileStmt->test && expressionContainsNewTargetForEval(*doWhileStmt->test);
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant && expressionContainsNewTargetForEval(*switchStmt->discriminant)) return true;
    for (const auto& c : switchStmt->cases) {
      if (c.test && expressionContainsNewTargetForEval(*c.test)) return true;
      for (const auto& s : c.consequent) {
        if (s && statementContainsNewTargetForEval(*s)) return true;
      }
    }
    return false;
  }
  if (auto* label = std::get_if<LabelledStmt>(&stmt.node)) {
    return label->body && statementContainsNewTargetForEval(*label->body);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return throwStmt->argument && expressionContainsNewTargetForEval(*throwStmt->argument);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& s : tryStmt->block) {
      if (s && statementContainsNewTargetForEval(*s)) return true;
    }
    if (tryStmt->handler.paramPattern && expressionContainsNewTargetForEval(*tryStmt->handler.paramPattern)) return true;
    for (const auto& s : tryStmt->handler.body) {
      if (s && statementContainsNewTargetForEval(*s)) return true;
    }
    for (const auto& s : tryStmt->finalizer) {
      if (s && statementContainsNewTargetForEval(*s)) return true;
    }
    return false;
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return exportDefault->declaration && expressionContainsNewTargetForEval(*exportDefault->declaration);
  }
  return false;
}

bool programContainsSuperForEval(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt && statementContainsSuperForEval(*stmt)) return true;
  }
  return false;
}

bool programContainsSuperCallForEval(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt && statementContainsSuperCallForEval(*stmt)) return true;
  }
  return false;
}

bool programContainsArgumentsForEval(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt && statementContainsArgumentsForEval(*stmt)) return true;
  }
  return false;
}

bool programContainsNewTargetForEval(const Program& program) {
  for (const auto& stmt : program.body) {
    if (stmt && statementContainsNewTargetForEval(*stmt)) return true;
  }
  return false;
}

void collectTopLevelFunctionDeclarationNames(const Program& program, std::vector<std::string>& names) {
  for (const auto& stmt : program.body) {
    if (!stmt) continue;
    if (auto* funcDecl = std::get_if<FunctionDeclaration>(&stmt->node)) {
      if (!funcDecl->id.name.empty()) {
        names.push_back(funcDecl->id.name);
      }
    }
  }
}

bool isGlobalObjectExtensible(const GCPtr<Object>& globalObj) {
  return globalObj && !globalObj->sealed && !globalObj->frozen;
}

bool isGlobalPropertyConfigurable(const GCPtr<Object>& globalObj, const std::string& name) {
  return globalObj->properties.find("__non_configurable_" + name) == globalObj->properties.end();
}

bool isGlobalPropertyWritable(const GCPtr<Object>& globalObj, const std::string& name) {
  return globalObj->properties.find("__non_writable_" + name) == globalObj->properties.end();
}

bool isGlobalPropertyEnumerable(const GCPtr<Object>& globalObj, const std::string& name) {
  return globalObj->properties.find("__non_enum_" + name) == globalObj->properties.end();
}

bool canDeclareGlobalVarBinding(const GCPtr<Object>& globalObj, const std::string& name) {
  if (globalObj->properties.find(name) != globalObj->properties.end()) {
    return true;
  }
  return isGlobalObjectExtensible(globalObj);
}

bool canDeclareGlobalFunctionBinding(const GCPtr<Object>& globalObj,
                                     const std::string& name,
                                     bool& shouldResetAttributes) {
  auto existing = globalObj->properties.find(name);
  if (existing == globalObj->properties.end()) {
    shouldResetAttributes = true;
    return isGlobalObjectExtensible(globalObj);
  }

  if (isGlobalPropertyConfigurable(globalObj, name)) {
    shouldResetAttributes = true;
    return true;
  }

  shouldResetAttributes = false;
  return isGlobalPropertyWritable(globalObj, name) &&
         isGlobalPropertyEnumerable(globalObj, name);
}

void resetGlobalDataPropertyAttributes(const GCPtr<Object>& globalObj, const std::string& name) {
  globalObj->properties.erase("__non_writable_" + name);
  globalObj->properties.erase("__non_enum_" + name);
  globalObj->properties.erase("__non_configurable_" + name);
  globalObj->properties.erase("__enum_" + name);
}

bool defineModuleNamespaceProperty(const GCPtr<Object>& obj,
                                   const std::string& key,
                                   const GCPtr<Object>& descriptor) {
  const std::string& toStringTagKey = WellKnownSymbols::toStringTagKey();
  const bool isExport = isModuleNamespaceExportKey(obj, key);
  const bool isToStringTag = (key == toStringTagKey);
  if (!isExport && !isToStringTag) {
    return false;
  }

  auto has = [&](const std::string& name) {
    return descriptor->properties.find(name) != descriptor->properties.end();
  };
  auto boolFieldMatches = [&](const std::string& name, bool expected) {
    if (!has(name)) return true;
    return descriptor->properties.at(name).toBool() == expected;
  };

  if (has("get") || has("set")) {
    return false;
  }

  if (isExport) {
    if (has("value")) {
      return false;
    }
    return boolFieldMatches("writable", true) &&
           boolFieldMatches("enumerable", true) &&
           boolFieldMatches("configurable", false);
  }

  if (has("value") && descriptor->properties.at("value").toString() != "Module") {
    return false;
  }
  return boolFieldMatches("writable", false) &&
         boolFieldMatches("enumerable", false) &&
         boolFieldMatches("configurable", false);
}

std::optional<std::string> readTextFile(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    return std::nullopt;
  }
  std::stringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

std::vector<std::string> parseStaticImportSpecifiers(const std::string& source) {
  std::vector<std::string> specifiers;
  static const std::regex kImportRegex(
    R"((?:^|[\n\r])\s*import\s+(?:[^'";\n]*\s+from\s+)?["']([^"']+)["']\s*;)");
  for (std::sregex_iterator it(source.begin(), source.end(), kImportRegex), end; it != end; ++it) {
    specifiers.push_back((*it)[1].str());
  }
  return specifiers;
}

bool hasTopLevelAwaitInSource(const std::string& source) {
  static const std::regex kTopLevelAwaitRegex(R"((?:^|[\n\r])\s*await\b)");
  return std::regex_search(source, kTopLevelAwaitRegex);
}

void gatherAsyncTransitiveDependencies(const std::string& modulePath,
                                       ModuleLoader* loader,
                                       std::unordered_set<std::string>& visitedModules,
                                       std::unordered_set<std::string>& queuedAsyncModules,
                                       std::vector<std::string>& orderedAsyncModules) {
  if (!loader) {
    return;
  }
  if (!visitedModules.insert(modulePath).second) {
    return;
  }

  auto sourceOpt = readTextFile(modulePath);
  if (!sourceOpt.has_value()) {
    return;
  }

  if (hasTopLevelAwaitInSource(*sourceOpt)) {
    if (queuedAsyncModules.insert(modulePath).second) {
      orderedAsyncModules.push_back(modulePath);
    }
    return;
  }

  auto specifiers = parseStaticImportSpecifiers(*sourceOpt);
  for (const auto& specifier : specifiers) {
    std::string resolvedDependency = loader->resolvePath(specifier, modulePath);
    gatherAsyncTransitiveDependencies(
      resolvedDependency, loader, visitedModules, queuedAsyncModules, orderedAsyncModules);
  }
}

}  // namespace

// Global module loader and interpreter for dynamic imports
static std::shared_ptr<ModuleLoader> g_moduleLoader;
static Interpreter* g_interpreter = nullptr;

void setGlobalModuleLoader(std::shared_ptr<ModuleLoader> loader) {
  g_moduleLoader = loader;
}

void setGlobalInterpreter(Interpreter* interpreter) {
  g_interpreter = interpreter;
}

Interpreter* getGlobalInterpreter() {
  return g_interpreter;
}

Environment::Environment(Environment* parent)
  : parent_(parent) {
  GarbageCollector::instance().reportAllocation(sizeof(Environment));
}

void Environment::define(const std::string& name, const Value& value, bool isConst) {
  bindings_[name] = value;
  tdzBindings_.erase(name);  // Remove TDZ when initialized
  if (isConst) {
    constants_[name] = true;
  }
  if (!parent_) {
    auto it = bindings_.find("globalThis");
    if (it != bindings_.end() && it->second.isObject()) {
      auto globalObj = it->second.getGC<Object>();
      globalObj->properties[name] = value;
    }
  }
}

void Environment::defineLexical(const std::string& name, const Value& value, bool isConst) {
  bindings_[name] = value;
  tdzBindings_.erase(name);
  lexicalBindings_[name] = true;
  if (isConst) {
    constants_[name] = true;
  }
}

void Environment::defineTDZ(const std::string& name) {
  bindings_[name] = Value(Undefined{});
  tdzBindings_[name] = true;
  lexicalBindings_[name] = true;
}

void Environment::removeTDZ(const std::string& name) {
  tdzBindings_.erase(name);
}

bool Environment::isTDZ(const std::string& name) const {
  // Check if binding exists in this scope and is in TDZ
  auto bindIt = bindings_.find(name);
  if (bindIt != bindings_.end()) {
    return tdzBindings_.find(name) != tdzBindings_.end();
  }
  // If not found in this scope, check parent
  if (parent_) {
    return parent_->isTDZ(name);
  }
  return false;
}

std::optional<Value> Environment::get(const std::string& name) const {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    return it->second;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end()) {
    auto withValue = lookupWithScopeProperty(withScopeIt->second, name);
    if (withValue.has_value()) {
      return withValue;
    }
  }
  if (parent_) {
    return parent_->get(name);
  }
  // At global scope, fall back to globalThis properties.
  // In JavaScript, the global object acts as the variable environment
  // for global code, so properties on globalThis are accessible as variables.
  auto globalIt = bindings_.find("globalThis");
  if (globalIt != bindings_.end() && globalIt->second.isObject()) {
    auto globalObj = globalIt->second.getGC<Object>();
    auto propIt = globalObj->properties.find(name);
    if (propIt != globalObj->properties.end()) {
      return propIt->second;
    }
  }
  return std::nullopt;
}

bool Environment::set(const std::string& name, const Value& value) {
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    if (constants_.find(name) != constants_.end()) {
      return false;
    }
    bindings_[name] = value;
    // Keep existing global object properties in sync with root-scope bindings.
    if (!parent_) {
      auto globalIt = bindings_.find("globalThis");
      if (globalIt != bindings_.end() && globalIt->second.isObject()) {
        auto globalObj = globalIt->second.getGC<Object>();
        if (globalObj->properties.find(name) != globalObj->properties.end()) {
          globalObj->properties[name] = value;
        }
      }
    }
    return true;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end() &&
      setWithScopeProperty(withScopeIt->second, name, value)) {
    return true;
  }
  if (parent_) {
    return parent_->set(name, value);
  }
  // At global scope, also check/set globalThis properties
  if (!parent_) {
    auto globalIt = bindings_.find("globalThis");
    if (globalIt != bindings_.end() && globalIt->second.isObject()) {
      auto globalObj = globalIt->second.getGC<Object>();
      auto propIt = globalObj->properties.find(name);
      if (propIt != globalObj->properties.end()) {
        propIt->second = value;
        return true;
      }
    }
  }
  return false;
}

bool Environment::has(const std::string& name) const {
  if (bindings_.find(name) != bindings_.end()) {
    return true;
  }
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end() &&
      lookupWithScopeProperty(withScopeIt->second, name).has_value()) {
    return true;
  }
  if (parent_) {
    return parent_->has(name);
  }
  // At global scope, also check globalThis properties
  auto globalIt = bindings_.find("globalThis");
  if (globalIt != bindings_.end() && globalIt->second.isObject()) {
    auto globalObj = globalIt->second.getGC<Object>();
    if (globalObj->properties.find(name) != globalObj->properties.end()) {
      return true;
    }
  }
  return false;
}

bool Environment::hasLocal(const std::string& name) const {
  return bindings_.find(name) != bindings_.end();
}

bool Environment::hasLexicalLocal(const std::string& name) const {
  auto it = lexicalBindings_.find(name);
  return it != lexicalBindings_.end() && it->second;
}

bool Environment::deleteLocalMutable(const std::string& name) {
  auto it = bindings_.find(name);
  if (it == bindings_.end()) {
    return false;
  }
  if (constants_.find(name) != constants_.end()) {
    return false;
  }
  auto lexIt = lexicalBindings_.find(name);
  if (lexIt != lexicalBindings_.end() && lexIt->second) {
    return false;
  }
  bindings_.erase(it);
  constants_.erase(name);
  lexicalBindings_.erase(name);
  tdzBindings_.erase(name);
  return true;
}

int Environment::deleteFromWithScope(const std::string& name) {
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end()) {
    if (deleteWithScopeProperty(withScopeIt->second, name)) {
      return 1;  // deleted
    }
    // Check if it exists but is non-configurable
    if (lookupWithScopeProperty(withScopeIt->second, name).has_value()) {
      return -1;  // exists but non-configurable
    }
  }
  if (parent_) {
    return parent_->deleteFromWithScope(name);
  }
  return 0;  // not found in any with scope
}

bool Environment::setVar(const std::string& name, const Value& value) {
  // For var declarations: walk the scope chain looking for bindings,
  // but skip with-scope objects. This ensures var assignments go to the
  // hoisted variable in the function/global scope, not to with-scope properties.
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    it->second = value;
    return true;
  }
  if (parent_) {
    return parent_->setVar(name, value);
  }
  return false;
}

Environment* Environment::resolveBindingEnvironment(const std::string& name) {
  if (bindings_.find(name) != bindings_.end()) {
    return this;
  }
  if (parent_) {
    return parent_->resolveBindingEnvironment(name);
  }
  return nullptr;
}

GCPtr<Object> Environment::resolveWithScopeObject(const std::string& name) const {
  // Check if this env has a local binding that shadows the name
  auto it = bindings_.find(name);
  if (it != bindings_.end()) {
    return nullptr;  // Name is a local binding, not in with-scope
  }
  // Check with-scope object
  auto withScopeIt = bindings_.find(kWithScopeObjectBinding);
  if (withScopeIt != bindings_.end() && withScopeIt->second.isObject()) {
    auto obj = withScopeIt->second.getGC<Object>();
    if (lookupWithScopeProperty(withScopeIt->second, name).has_value()) {
      return obj;
    }
    if (auto* interpreter = getGlobalInterpreter(); interpreter && interpreter->hasError()) {
      return obj;
    }
  }
  if (parent_) {
    return parent_->resolveWithScopeObject(name);
  }
  return nullptr;
}

bool Environment::isConst(const std::string& name) const {
  if (constants_.find(name) != constants_.end()) {
    return true;
  }
  if (parent_) {
    return parent_->isConst(name);
  }
  return false;
}

GCPtr<Environment> Environment::createChild() {
  return GarbageCollector::makeGC<Environment>(this);
}

GCPtr<Environment> Environment::createGlobal() {
  auto env = GarbageCollector::makeGC<Environment>();

  auto consoleFn = GarbageCollector::makeGC<Function>();
  consoleFn->isNative = true;
  consoleFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.error - writes to stderr
  auto consoleErrorFn = GarbageCollector::makeGC<Function>();
  consoleErrorFn->isNative = true;
  consoleErrorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cerr << arg.toString() << " ";
    }
    std::cerr << std::endl;
    return Value(Undefined{});
  };

  // console.warn - writes to stderr
  auto consoleWarnFn = GarbageCollector::makeGC<Function>();
  consoleWarnFn->isNative = true;
  consoleWarnFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cerr << arg.toString() << " ";
    }
    std::cerr << std::endl;
    return Value(Undefined{});
  };

  // console.info - same as log
  auto consoleInfoFn = GarbageCollector::makeGC<Function>();
  consoleInfoFn->isNative = true;
  consoleInfoFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.debug - same as log
  auto consoleDebugFn = GarbageCollector::makeGC<Function>();
  consoleDebugFn->isNative = true;
  consoleDebugFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    for (const auto& arg : args) {
      std::cout << arg.toString() << " ";
    }
    std::cout << std::endl;
    return Value(Undefined{});
  };

  // console.time/timeEnd - simple timing
  static std::unordered_map<std::string, std::chrono::steady_clock::time_point> consoleTimers;

  auto consoleTimeFn = GarbageCollector::makeGC<Function>();
  consoleTimeFn->isNative = true;
  consoleTimeFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string label = args.empty() ? "default" : args[0].toString();
    consoleTimers[label] = std::chrono::steady_clock::now();
    return Value(Undefined{});
  };

  auto consoleTimeEndFn = GarbageCollector::makeGC<Function>();
  consoleTimeEndFn->isNative = true;
  consoleTimeEndFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string label = args.empty() ? "default" : args[0].toString();
    auto it = consoleTimers.find(label);
    if (it != consoleTimers.end()) {
      auto elapsed = std::chrono::steady_clock::now() - it->second;
      auto ms = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.0;
      std::cout << label << ": " << ms << "ms" << std::endl;
      consoleTimers.erase(it);
    }
    return Value(Undefined{});
  };

  // console.assert
  auto consoleAssertFn = GarbageCollector::makeGC<Function>();
  consoleAssertFn->isNative = true;
  consoleAssertFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    bool condition = args.empty() ? false : args[0].toBool();
    if (!condition) {
      std::cerr << "Assertion failed:";
      for (size_t i = 1; i < args.size(); ++i) {
        std::cerr << " " << args[i].toString();
      }
      std::cerr << std::endl;
    }
    return Value(Undefined{});
  };

  auto consoleObj = GarbageCollector::makeGC<Object>();
  consoleObj->properties["log"] = Value(consoleFn);
  consoleObj->properties["error"] = Value(consoleErrorFn);
  consoleObj->properties["warn"] = Value(consoleWarnFn);
  consoleObj->properties["info"] = Value(consoleInfoFn);
  consoleObj->properties["debug"] = Value(consoleDebugFn);
  consoleObj->properties["time"] = Value(consoleTimeFn);
  consoleObj->properties["timeEnd"] = Value(consoleTimeEndFn);
  consoleObj->properties["assert"] = Value(consoleAssertFn);

  env->define("console", Value(consoleObj));

  auto evalFn = GarbageCollector::makeGC<Function>();
  evalFn->isNative = true;
  evalFn->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }
    if (!args[0].isString()) {
      return args[0];
    }

    Interpreter* prevInterpreter = getGlobalInterpreter();
    if (!prevInterpreter) {
      return Value(Undefined{});
    }
    bool isDirectEval = prevInterpreter->inDirectEvalInvocation();

    auto evalEnv = env;
    if (isDirectEval) {
      auto callerEnv = prevInterpreter->getEnvironment();
      if (callerEnv) {
        evalEnv = callerEnv;
      }
    }

    std::string source = args[0].toString();
    Lexer lexer(source);
    auto tokens = lexer.tokenize();

    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
      if (tokens[i].type == TokenType::Import &&
          !tokens[i].escaped &&
          tokens[i + 1].type == TokenType::Dot &&
          tokens[i + 2].type == TokenType::Identifier &&
          !tokens[i + 2].escaped &&
          tokens[i + 2].value == "meta") {
        throw std::runtime_error("SyntaxError: import.meta is not allowed in eval");
      }
    }

    // Direct eval inherits strict mode from calling context (ES2020 18.2.1.1)
    bool inheritStrict = isDirectEval && prevInterpreter->isStrictMode();

    Parser parser(tokens);
    if (inheritStrict) {
      parser.setStrictMode(true);
    }
    if (isDirectEval) {
      auto allowedPrivateNames = prevInterpreter->activePrivateNamesForEval();
      if (!allowedPrivateNames.empty()) {
        parser.setAllowedPrivateNames(allowedPrivateNames);
      }
    }
    auto program = parser.parse();
    if (!program) {
      throw std::runtime_error("SyntaxError: Parse error");
    }

    bool strictEval = inheritStrict || bodyHasUseStrictDirective(program->body);

    auto varEnv = evalEnv;
    if (isDirectEval) {
      auto scope = evalEnv;
      while (scope && !scope->hasLocal("__var_scope__")) {
        auto parent = scope->getParentPtr();
        if (!parent) {
          break;
        }
        scope = parent;
      }
      if (scope) {
        varEnv = scope;
      } else if (evalEnv) {
        varEnv = evalEnv->getRoot();
      }
    } else if (env) {
      varEnv = env->getRoot();
    }

    if (isDirectEval) {
      bool inFieldInitializer = prevInterpreter->inFieldInitializerEvaluation() ||
                                evalEnv->get("__in_class_field_initializer__").has_value();
      bool inArrow = prevInterpreter->activeFunctionIsArrow();
      bool inMethod = (prevInterpreter->activeFunctionHasHomeObject() ||
                       prevInterpreter->activeFunctionHasSuperClassBinding() ||
                       evalEnv->hasLocal("__super__"));
      // Constructor-ness is dynamic (`[[Construct]]` call state), not whether
      // the active function object is constructable.
      bool inConstructor = evalEnv->hasLocal("__new_target__");
      bool allowNewTarget = (!inArrow && (prevInterpreter->hasActiveFunction() ||
                                          evalEnv->hasLocal("__new_target__")));
      if (inFieldInitializer) {
        // Additional eval-in-initializer rules: treat as outside constructor,
        // inside method, and inside function.
        inMethod = true;
        inConstructor = false;
        allowNewTarget = true;
      } else if (inArrow) {
        inMethod = false;
        inConstructor = false;
      }

      if (!inMethod && programContainsSuperForEval(*program)) {
        throw std::runtime_error("SyntaxError: 'super' keyword is not valid here");
      }
      if (!inConstructor && programContainsSuperCallForEval(*program)) {
        throw std::runtime_error("SyntaxError: 'super' keyword is not valid here");
      }
      if (inFieldInitializer && programContainsArgumentsForEval(*program)) {
        throw std::runtime_error("SyntaxError: 'arguments' is not allowed in this context");
      }
      if (!allowNewTarget && programContainsNewTargetForEval(*program)) {
        throw std::runtime_error("SyntaxError: new.target is not allowed in this context");
      }

      if (prevInterpreter->inParameterInitializerEvaluation() && evalEnv->hasLocal("arguments")) {
        std::vector<std::string> declaredNames;
        for (const auto& stmt : program->body) {
          if (stmt) {
            collectDeclaredNamesForDirectEval(*stmt, declaredNames);
          }
        }
        for (const auto& name : declaredNames) {
          if (name == "arguments") {
            throw std::runtime_error("SyntaxError: Identifier 'arguments' has already been declared");
          }
        }
      }

      if (!strictEval) {
        std::vector<std::string> varNames;
        for (const auto& stmt : program->body) {
          if (stmt) {
            collectVarNamesFromStatement(*stmt, varNames);
          }
        }
        std::vector<std::string> topLevelFnNames;
        collectTopLevelFunctionDeclarationNames(*program, topLevelFnNames);
        varNames.insert(varNames.end(), topLevelFnNames.begin(), topLevelFnNames.end());
        std::unordered_set<std::string> seenNames;
        for (const auto& varName : varNames) {
          if (!seenNames.insert(varName).second) {
            continue;
          }
          auto scope = evalEnv;
          while (scope && scope.get() != varEnv.get()) {
            // Skip with-scope object environments (cannot contain lexical declarations).
            if (scope->hasLocal(kWithScopeObjectBinding)) {
              scope = scope->getParentPtr();
              continue;
            }
            if (scope->hasLexicalLocal(varName)) {
              throw std::runtime_error("SyntaxError: Identifier '" + varName + "' has already been declared");
            }
            scope = scope->getParentPtr();
          }
          // Also reject conflicts in the variable environment itself (not just at global scope).
          // This covers parameter bindings and top-level lexical declarations in sloppy functions.
          if (varEnv && varEnv->hasLexicalLocal(varName)) {
            throw std::runtime_error("SyntaxError: Identifier '" + varName + "' has already been declared");
          }
        }
      }
    } else {
      if (programContainsSuperForEval(*program) || programContainsSuperCallForEval(*program)) {
        throw std::runtime_error("SyntaxError: 'super' keyword is not valid here");
      }
      if (programContainsNewTargetForEval(*program)) {
        throw std::runtime_error("SyntaxError: new.target is not allowed in this context");
      }

      if (!strictEval && varEnv && !varEnv->getParent()) {
        std::vector<std::string> varNames;
        for (const auto& stmt : program->body) {
          if (stmt) {
            collectVarNamesFromStatement(*stmt, varNames);
          }
        }
        std::vector<std::string> topLevelFnNames;
        collectTopLevelFunctionDeclarationNames(*program, topLevelFnNames);
        varNames.insert(varNames.end(), topLevelFnNames.begin(), topLevelFnNames.end());

        std::unordered_set<std::string> seenNames;
        for (const auto& varName : varNames) {
          if (!seenNames.insert(varName).second) {
            continue;
          }
          if (varEnv->hasLexicalLocal(varName)) {
            throw std::runtime_error("SyntaxError: Identifier '" + varName + "' has already been declared");
          }
        }
      }
    }

    GCPtr<Environment> execOuterEnv = isDirectEval ? evalEnv : GCPtr<Environment>(nullptr);
    if (!isDirectEval && env) {
      execOuterEnv = env->getRoot();
    }
    if (!execOuterEnv) {
      execOuterEnv = varEnv ? varEnv : env;
    }
    auto execEnv = execOuterEnv->createChild();

    std::vector<std::string> varScopedNames;
    for (const auto& stmt : program->body) {
      if (stmt) {
        collectVarNamesFromStatement(*stmt, varScopedNames);
      }
    }
    std::vector<std::string> topLevelFnNames;
    collectTopLevelFunctionDeclarationNames(*program, topLevelFnNames);
    varScopedNames.insert(varScopedNames.end(), topLevelFnNames.begin(), topLevelFnNames.end());

    struct GlobalFunctionPlan {
      std::string name;
      bool resetAttributes = false;
      bool preserveExistingAttributes = false;
      bool hadNonWritable = false;
      bool hadNonEnumerable = false;
      bool hadNonConfigurable = false;
      bool hadEnumMarker = false;
    };
    struct GlobalVarPlan {
      std::string name;
      bool created = false;
      bool preserveExistingAttributes = false;
      bool hadNonWritable = false;
      bool hadNonEnumerable = false;
      bool hadNonConfigurable = false;
      bool hadEnumMarker = false;
    };
    std::vector<GlobalFunctionPlan> globalFunctionPlans;
    std::vector<GlobalVarPlan> globalVarPlans;
    GCPtr<Object> globalObj;

    if (!strictEval && varEnv && !varEnv->getParent()) {
      auto globalThis = varEnv->get("globalThis");
      if (globalThis && globalThis->isObject()) {
        globalObj = globalThis->getGC<Object>();

        std::vector<std::string> topLevelFnNames;
        collectTopLevelFunctionDeclarationNames(*program, topLevelFnNames);
        std::unordered_set<std::string> declaredFunctionNames;
        for (auto it = topLevelFnNames.rbegin(); it != topLevelFnNames.rend(); ++it) {
          if (!declaredFunctionNames.insert(*it).second) {
            continue;
          }
          bool resetAttrs = true;
          if (!canDeclareGlobalFunctionBinding(globalObj, *it, resetAttrs)) {
            throw std::runtime_error("TypeError: Cannot declare global function '" + *it + "'");
          }
          GlobalFunctionPlan plan;
          plan.name = *it;
          plan.resetAttributes = resetAttrs;
          if (globalObj->properties.find(*it) != globalObj->properties.end() && !resetAttrs) {
            plan.preserveExistingAttributes = true;
            plan.hadNonWritable = globalObj->properties.find("__non_writable_" + *it) != globalObj->properties.end();
            plan.hadNonEnumerable = globalObj->properties.find("__non_enum_" + *it) != globalObj->properties.end();
            plan.hadNonConfigurable = globalObj->properties.find("__non_configurable_" + *it) != globalObj->properties.end();
            plan.hadEnumMarker = globalObj->properties.find("__enum_" + *it) != globalObj->properties.end();
          }
          globalFunctionPlans.push_back(plan);
        }

        std::vector<std::string> declaredVarNames;
        for (const auto& stmt : program->body) {
          if (stmt) {
            collectVarNamesFromStatement(*stmt, declaredVarNames);
          }
        }
        std::unordered_set<std::string> seenVarNames;
        for (const auto& vn : declaredVarNames) {
          if (declaredFunctionNames.count(vn) != 0) {
            continue;
          }
          if (!seenVarNames.insert(vn).second) {
            continue;
          }
          bool exists = globalObj->properties.find(vn) != globalObj->properties.end();
          if (!canDeclareGlobalVarBinding(globalObj, vn)) {
            throw std::runtime_error("TypeError: Cannot declare global variable '" + vn + "'");
          }
          GlobalVarPlan plan;
          plan.name = vn;
          plan.created = !exists;
          if (exists) {
            plan.preserveExistingAttributes = true;
            plan.hadNonWritable = globalObj->properties.find("__non_writable_" + vn) != globalObj->properties.end();
            plan.hadNonEnumerable = globalObj->properties.find("__non_enum_" + vn) != globalObj->properties.end();
            plan.hadNonConfigurable = globalObj->properties.find("__non_configurable_" + vn) != globalObj->properties.end();
            plan.hadEnumMarker = globalObj->properties.find("__enum_" + vn) != globalObj->properties.end();
          }
          globalVarPlans.push_back(plan);
        }
      }
    }

    if (!strictEval && varEnv) {
      std::unordered_set<std::string> seeded;
      for (const auto& name : varScopedNames) {
        if (!seeded.insert(name).second) {
          continue;
        }
        // For direct eval in function scope, var declarations are instantiated in
        // the variable environment record (the nearest __var_scope__), and must
        // not observe bindings in outer environments (Test262 S11.13.1_A6_T1).
        // For global eval, preserve existing global bindings/properties.
        bool varExistsInVarEnv = false;
        if (varEnv->getParent() == nullptr) {
          varExistsInVarEnv = varEnv->has(name);
        } else {
          varExistsInVarEnv = varEnv->hasLocal(name);
        }
        if (!varExistsInVarEnv || varEnv->hasLexicalLocal(name)) {
          continue;
        }
        auto existing = varEnv->get(name);
        if (existing.has_value()) {
          execEnv->define(name, *existing);
        }
      }
    }

    if (!strictEval && isDirectEval && varEnv && varEnv->getParent()) {
      execEnv->define("__eval_deletable_bindings__", Value(true), true);
    }

    // Move program to heap so functions created during eval can keep AST alive
    auto programPtr = std::make_shared<Program>(std::move(*program));

    Interpreter evalInterpreter(execEnv);
    if (isDirectEval) {
      evalInterpreter.inheritDirectEvalContextFrom(*prevInterpreter);
    }
    if (strictEval) {
      evalInterpreter.setStrictMode(true);
    }
    evalInterpreter.setSourceKeepAlive(programPtr);
    Value result = Value(Undefined{});
    try {
      setGlobalInterpreter(&evalInterpreter);
      auto task = evalInterpreter.evaluate(*programPtr);
      while (!task.done()) {
        task.resume();
      }
      result = task.result();
      if (evalInterpreter.hasError()) {
        Value err = evalInterpreter.getError();
        evalInterpreter.clearError();
        setGlobalInterpreter(prevInterpreter);
        throw JsValueException(err);
      }
    } catch (...) {
      setGlobalInterpreter(prevInterpreter);
      throw;
    }

    if (!strictEval && varEnv) {
      std::unordered_set<std::string> propagated;
      for (const auto& name : varScopedNames) {
        if (!propagated.insert(name).second) {
          continue;
        }
        if (!execEnv->hasLocal(name)) {
          continue;
        }
        auto value = execEnv->get(name);
        if (!value.has_value()) {
          continue;
        }
        if (varEnv->hasLocal(name)) {
          varEnv->set(name, *value);
        } else {
          varEnv->define(name, *value);
        }
      }

      if (globalObj) {
        for (const auto& plan : globalFunctionPlans) {
          if (globalObj->properties.find(plan.name) == globalObj->properties.end()) {
            continue;
          }
          if (plan.resetAttributes) {
            resetGlobalDataPropertyAttributes(globalObj, plan.name);
            continue;
          }
          if (plan.preserveExistingAttributes) {
            if (plan.hadNonWritable) globalObj->properties["__non_writable_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_writable_" + plan.name);
            if (plan.hadNonEnumerable) globalObj->properties["__non_enum_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_enum_" + plan.name);
            if (plan.hadNonConfigurable) globalObj->properties["__non_configurable_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_configurable_" + plan.name);
            if (plan.hadEnumMarker) globalObj->properties["__enum_" + plan.name] = Value(true);
            else globalObj->properties.erase("__enum_" + plan.name);
          }
        }
        for (const auto& plan : globalVarPlans) {
          if (globalObj->properties.find(plan.name) == globalObj->properties.end()) {
            continue;
          }
          if (plan.created) {
            resetGlobalDataPropertyAttributes(globalObj, plan.name);
            continue;
          }
          if (plan.preserveExistingAttributes) {
            if (plan.hadNonWritable) globalObj->properties["__non_writable_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_writable_" + plan.name);
            if (plan.hadNonEnumerable) globalObj->properties["__non_enum_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_enum_" + plan.name);
            if (plan.hadNonConfigurable) globalObj->properties["__non_configurable_" + plan.name] = Value(true);
            else globalObj->properties.erase("__non_configurable_" + plan.name);
            if (plan.hadEnumMarker) globalObj->properties["__enum_" + plan.name] = Value(true);
            else globalObj->properties.erase("__enum_" + plan.name);
          }
        }
      }
    }

    setGlobalInterpreter(prevInterpreter);
    return result;
  };
  evalFn->properties["__is_intrinsic_eval__"] = Value(true);
  env->define("eval", Value(evalFn));

  // Symbol constructor
  auto symbolFn = GarbageCollector::makeGC<Function>();
  symbolFn->isNative = true;
  symbolFn->isConstructor = true;
  symbolFn->properties["name"] = Value(std::string("Symbol"));
  symbolFn->properties["length"] = Value(1.0);
  // Symbol is a constructor whose [[Construct]] always throws (per spec).
  // Keep it as a constructor so it can be used in `extends`, but prevent `new Symbol()`.
  symbolFn->properties["__throw_on_new__"] = Value(true);
  symbolFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string description = args.empty() ? "" : args[0].toString();
    return Value(Symbol(description));
  };
  {
    auto symbolPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    symbolFn->properties["prototype"] = Value(symbolPrototype);
    symbolPrototype->properties["constructor"] = Value(symbolFn);
    symbolPrototype->properties["name"] = Value(std::string("Symbol"));
  }
  symbolFn->properties["iterator"] = WellKnownSymbols::iterator();
  symbolFn->properties["asyncIterator"] = WellKnownSymbols::asyncIterator();
  symbolFn->properties["toStringTag"] = WellKnownSymbols::toStringTag();
  symbolFn->properties["toPrimitive"] = WellKnownSymbols::toPrimitive();
  symbolFn->properties["matchAll"] = WellKnownSymbols::matchAll();
  symbolFn->properties["unscopables"] = WellKnownSymbols::unscopables();
  symbolFn->properties["hasInstance"] = WellKnownSymbols::hasInstance();
  symbolFn->properties["species"] = WellKnownSymbols::species();
  symbolFn->properties["isConcatSpreadable"] = WellKnownSymbols::isConcatSpreadable();
  symbolFn->properties["match"] = WellKnownSymbols::match();
  symbolFn->properties["replace"] = WellKnownSymbols::replace();
  symbolFn->properties["search"] = WellKnownSymbols::search();
  symbolFn->properties["split"] = WellKnownSymbols::split();
  // Well-known symbol properties on Symbol are non-writable and non-configurable.
  const char* wellKnownNames[] = {
    "iterator", "asyncIterator", "toStringTag", "toPrimitive",
    "matchAll", "unscopables", "hasInstance", "species",
    "isConcatSpreadable", "match", "replace", "search", "split"
  };
  for (const char* name : wellKnownNames) {
    symbolFn->properties[std::string("__non_writable_") + name] = Value(true);
    symbolFn->properties[std::string("__non_configurable_") + name] = Value(true);
  }

  // Symbol.for() - global symbol registry
  static std::unordered_map<std::string, Value> globalSymbolRegistry;
  auto symbolFor = GarbageCollector::makeGC<Function>();
  symbolFor->isNative = true;
  symbolFor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string key = args.empty() ? "undefined" : args[0].toString();
    auto it = globalSymbolRegistry.find(key);
    if (it != globalSymbolRegistry.end()) {
      return it->second;
    }
    Symbol s(key);
    Value sym;
    sym.data = s;
    globalSymbolRegistry[key] = sym;
    return sym;
  };
  symbolFn->properties["for"] = Value(symbolFor);

  // Symbol.keyFor() - reverse lookup in global registry
  auto symbolKeyFor = GarbageCollector::makeGC<Function>();
  symbolKeyFor->isNative = true;
  symbolKeyFor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isSymbol()) {
      return Value(Undefined{});
    }
    const auto& sym = std::get<Symbol>(args[0].data);
    for (const auto& [key, val] : globalSymbolRegistry) {
      const auto& regSym = std::get<Symbol>(val.data);
      if (sym.id == regSym.id) {
        return Value(key);
      }
    }
    return Value(Undefined{});
  };
  symbolFn->properties["keyFor"] = Value(symbolKeyFor);
  env->define("Symbol", Value(symbolFn));

  // BigInt constructor/function
  auto bigIntFn = GarbageCollector::makeGC<Function>();
  bigIntFn->isNative = true;
  bigIntFn->isConstructor = true;

  auto arrayToString = [](const GCPtr<Array>& arr) -> std::string {
    std::string out;
    for (size_t i = 0; i < arr->elements.size(); i++) {
      if (i > 0) out += ",";
      out += arr->elements[i].toString();
    }
    return out;
  };

  auto isObjectLike = [](const Value& value) -> bool {
    // Keep this aligned with Interpreter::isObjectLike so builtins using ToPrimitive
    // (e.g. String(), Number(), BigInt()) behave consistently with the interpreter.
    return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() ||
           value.isProxy() || value.isPromise() || value.isGenerator() || value.isClass() ||
           value.isMap() || value.isSet() || value.isWeakMap() || value.isWeakSet() ||
           value.isTypedArray() || value.isArrayBuffer() || value.isDataView() || value.isError();
  };

  auto callChecked = [](const Value& callee,
                        const std::vector<Value>& callArgs,
                        const Value& thisArg) -> Value {
    if (!callee.isFunction()) {
      return Value(Undefined{});
    }

    auto fn = callee.getGC<Function>();
    if (fn->isNative) {
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() &&
          itUsesThis->second.isBool() &&
          itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.reserve(callArgs.size() + 1);
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable for callable conversion");
    }
    interpreter->clearError();
    Value result = interpreter->callForHarness(callee, callArgs, thisArg);
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw std::runtime_error(err.toString());
    }
    return result;
  };

  auto getObjectProperty = [callChecked](const GCPtr<Object>& obj,
                                         const Value& receiver,
                                         const std::string& key) -> std::pair<bool, Value> {
    auto current = obj;
    int depth = 0;
    while (current && depth <= 16) {
      depth++;

      std::string getterKey = "__get_" + key;
      auto getterIt = current->properties.find(getterKey);
      if (getterIt != current->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callChecked(getterIt->second, {}, receiver)};
        }
        return {true, Value(Undefined{})};
      }

      auto it = current->properties.find(key);
      if (it != current->properties.end()) {
        return {true, it->second};
      }

      auto protoIt = current->properties.find("__proto__");
      if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
        break;
      }
      current = protoIt->second.getGC<Object>();
    }

    return {false, Value(Undefined{})};
  };

  auto getPropertyFromBag = [getObjectProperty, callChecked](const Value& receiver,
                                                            OrderedMap<std::string, Value>& bag,
                                                            const std::string& key) -> std::pair<bool, Value> {
    std::string getterKey = "__get_" + key;
    auto getterIt = bag.find(getterKey);
    if (getterIt != bag.end()) {
      if (getterIt->second.isFunction()) {
        return {true, callChecked(getterIt->second, {}, receiver)};
      }
      return {true, Value(Undefined{})};
    }

    auto it = bag.find(key);
    if (it != bag.end()) {
      return {true, it->second};
    }

    auto protoIt = bag.find("__proto__");
    if (protoIt != bag.end() && protoIt->second.isObject()) {
      return getObjectProperty(protoIt->second.getGC<Object>(), receiver, key);
    }

    return {false, Value(Undefined{})};
  };

  std::function<std::pair<bool, Value>(const Value&, const std::string&)> getProperty;
  getProperty = [getObjectProperty, getPropertyFromBag, callChecked, &getProperty](const Value& receiver,
                                                                                  const std::string& key) -> std::pair<bool, Value> {
    if (receiver.isObject()) {
      return getObjectProperty(receiver.getGC<Object>(), receiver, key);
    }
    if (receiver.isFunction()) {
      auto fn = receiver.getGC<Function>();
      auto it = fn->properties.find(key);
      if (it != fn->properties.end()) {
        return {true, it->second};
      }
      return {false, Value(Undefined{})};
    }
    if (receiver.isRegex()) {
      auto regex = receiver.getGC<Regex>();
      std::string getterKey = "__get_" + key;
      auto getterIt = regex->properties.find(getterKey);
      if (getterIt != regex->properties.end()) {
        if (getterIt->second.isFunction()) {
          return {true, callChecked(getterIt->second, {}, receiver)};
        }
        return {true, Value(Undefined{})};
      }
      auto it = regex->properties.find(key);
      if (it != regex->properties.end()) {
        return {true, it->second};
      }
      return {false, Value(Undefined{})};
    }
    if (receiver.isGenerator()) {
      auto gen = receiver.getGC<Generator>();
      return getPropertyFromBag(receiver, gen->properties, key);
    }
    if (receiver.isPromise()) {
      auto p = receiver.getGC<Promise>();
      return getPropertyFromBag(receiver, p->properties, key);
    }
    if (receiver.isClass()) {
      auto cls = receiver.getGC<Class>();
      return getPropertyFromBag(receiver, cls->properties, key);
    }
    if (receiver.isMap()) {
      auto m = receiver.getGC<Map>();
      return getPropertyFromBag(receiver, m->properties, key);
    }
    if (receiver.isSet()) {
      auto s = receiver.getGC<Set>();
      return getPropertyFromBag(receiver, s->properties, key);
    }
    if (receiver.isWeakMap()) {
      auto m = receiver.getGC<WeakMap>();
      return getPropertyFromBag(receiver, m->properties, key);
    }
    if (receiver.isWeakSet()) {
      auto s = receiver.getGC<WeakSet>();
      return getPropertyFromBag(receiver, s->properties, key);
    }
    if (receiver.isTypedArray()) {
      auto ta = receiver.getGC<TypedArray>();
      return getPropertyFromBag(receiver, ta->properties, key);
    }
    if (receiver.isArrayBuffer()) {
      auto ab = receiver.getGC<ArrayBuffer>();
      return getPropertyFromBag(receiver, ab->properties, key);
    }
    if (receiver.isDataView()) {
      auto dv = receiver.getGC<DataView>();
      return getPropertyFromBag(receiver, dv->properties, key);
    }
    if (receiver.isError()) {
      auto err = receiver.getGC<Error>();
      return getPropertyFromBag(receiver, err->properties, key);
    }
    if (receiver.isProxy()) {
      auto proxy = receiver.getGC<Proxy>();
      if (proxy->target) {
        return getProperty(*proxy->target, key);
      }
    }
    return {false, Value(Undefined{})};
  };

  auto toPrimitive = [isObjectLike, arrayToString, getProperty, callChecked](const Value& input,
                                                                              bool preferString) -> Value {
    if (!isObjectLike(input)) {
      return input;
    }

    const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
    auto [hasExotic, exotic] = getProperty(input, toPrimitiveKey);
    if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
      if (!exotic.isFunction()) {
        throw std::runtime_error("TypeError: @@toPrimitive is not callable");
      }
      std::string hint = preferString ? "string" : "number";
      Value result = callChecked(exotic, {Value(hint)}, input);
      if (isObjectLike(result)) {
        throw std::runtime_error("TypeError: @@toPrimitive must return a primitive value");
      }
      return result;
    }

    std::array<std::string, 2> methodOrder = preferString
      ? std::array<std::string, 2>{"toString", "valueOf"}
      : std::array<std::string, 2>{"valueOf", "toString"};

    for (const auto& methodName : methodOrder) {
      auto [found, method] = getProperty(input, methodName);
      if (found) {
        if (method.isFunction()) {
          Value result = callChecked(method, {}, input);
          if (!isObjectLike(result)) {
            return result;
          }
        }
        continue;
      }

      if (methodName == "toString") {
        if (input.isArray()) {
          return Value(arrayToString(input.getGC<Array>()));
        }
        if (input.isObject()) {
          return Value(std::string("[object Object]"));
        }
        if (input.isFunction()) {
          return Value(std::string("[Function]"));
        }
        if (input.isRegex()) {
          return Value(input.toString());
        }
      }
    }

    throw std::runtime_error("TypeError: Cannot convert object to primitive value");
  };

  auto toBigIntFromPrimitive = [](const Value& v) -> bigint::BigIntValue {
    if (v.isBigInt()) {
      return v.toBigInt();
    }
    if (v.isBool()) {
      return v.toBool() ? 1 : 0;
    }
    if (v.isString()) {
      bigint::BigIntValue parsed = 0;
      if (!bigint::parseBigIntString(std::get<std::string>(v.data), parsed)) {
        throw std::runtime_error("SyntaxError: Cannot convert string to BigInt");
      }
      return parsed;
    }
    throw std::runtime_error("TypeError: Cannot convert value to BigInt");
  };

  auto toBigIntFromValue = [toPrimitive, toBigIntFromPrimitive](const Value& v) -> bigint::BigIntValue {
    Value primitive = toPrimitive(v, false);
    return toBigIntFromPrimitive(primitive);
  };

  auto toIndex = [toPrimitive](const Value& v) -> uint64_t {
    Value primitive = toPrimitive(v, false);
    if (primitive.isBigInt() || primitive.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert to index");
    }
    double n = primitive.toNumber();
    if (std::isnan(n)) return 0;
    if (!std::isfinite(n)) {
      throw std::runtime_error("RangeError: Invalid index");
    }
    n = std::trunc(n);
    if (n < 0.0 || n > 9007199254740991.0) {
      throw std::runtime_error("RangeError: Invalid index");
    }
    return static_cast<uint64_t>(n);
  };

  auto thisBigIntValue = [](const Value& thisValue) -> bigint::BigIntValue {
    if (thisValue.isBigInt()) {
      return thisValue.toBigInt();
    }
    if (thisValue.isObject()) {
      auto obj = thisValue.getGC<Object>();
      auto primitiveIt = obj->properties.find("__primitive_value__");
      if (primitiveIt != obj->properties.end() && primitiveIt->second.isBigInt()) {
        return primitiveIt->second.toBigInt();
      }
    }
    throw std::runtime_error("TypeError: BigInt method called on incompatible receiver");
  };

  auto formatBigInt = [](const bigint::BigIntValue& value, int radix) -> std::string {
    return bigint::toString(value, radix);
  };

  bigIntFn->nativeFunc = [toPrimitive, toBigIntFromPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: Cannot convert undefined to BigInt");
    }

    Value primitive = toPrimitive(args[0], false);
    if (primitive.isNumber()) {
      double n = primitive.toNumber();
      if (!std::isfinite(n) || std::floor(n) != n) {
        throw std::runtime_error("RangeError: Cannot convert Number to BigInt");
      }
      bigint::BigIntValue converted = 0;
      if (!bigint::fromIntegralDouble(n, converted)) {
        throw std::runtime_error("RangeError: Cannot convert Number to BigInt");
      }
      return Value(BigInt(converted));
    }

    return Value(BigInt(toBigIntFromPrimitive(primitive)));
  };
  bigIntFn->properties["name"] = Value(std::string("BigInt"));
  bigIntFn->properties["length"] = Value(1.0);
  bigIntFn->properties["__throw_on_new__"] = Value(true);

  auto asUintN = GarbageCollector::makeGC<Function>();
  asUintN->isNative = true;
  asUintN->properties["__throw_on_new__"] = Value(true);
  asUintN->properties["name"] = Value(std::string("asUintN"));
  asUintN->properties["length"] = Value(2.0);
  asUintN->nativeFunc = [toIndex, toBigIntFromValue](const std::vector<Value>& args) -> Value {
    uint64_t bits = toIndex(args.empty() ? Value(Undefined{}) : args[0]);
    auto n = toBigIntFromValue(args.size() > 1 ? args[1] : Value(Undefined{}));
    return Value(BigInt(bigint::asUintN(bits, n)));
  };
  bigIntFn->properties["asUintN"] = Value(asUintN);

  auto asIntN = GarbageCollector::makeGC<Function>();
  asIntN->isNative = true;
  asIntN->properties["__throw_on_new__"] = Value(true);
  asIntN->properties["name"] = Value(std::string("asIntN"));
  asIntN->properties["length"] = Value(2.0);
  asIntN->nativeFunc = [toIndex, toBigIntFromValue](const std::vector<Value>& args) -> Value {
    uint64_t bits = toIndex(args.empty() ? Value(Undefined{}) : args[0]);
    auto n = toBigIntFromValue(args.size() > 1 ? args[1] : Value(Undefined{}));
    return Value(BigInt(bigint::asIntN(bits, n)));
  };
  bigIntFn->properties["asIntN"] = Value(asIntN);

  auto bigIntProto = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto bigIntProtoToString = GarbageCollector::makeGC<Function>();
  bigIntProtoToString->isNative = true;
  bigIntProtoToString->properties["__throw_on_new__"] = Value(true);
  bigIntProtoToString->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoToString->properties["name"] = Value(std::string("toString"));
  bigIntProtoToString->properties["length"] = Value(0.0);
  bigIntProtoToString->nativeFunc = [thisBigIntValue, formatBigInt](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.toString requires BigInt");
    }

    auto n = thisBigIntValue(args[0]);
    int radix = 10;
    if (args.size() > 1 && !args[1].isUndefined()) {
      radix = static_cast<int>(std::trunc(args[1].toNumber()));
      if (radix < 2 || radix > 36) {
        throw std::runtime_error("RangeError: radix must be between 2 and 36");
      }
    }
    return Value(formatBigInt(n, radix));
  };
  bigIntProto->properties["toString"] = Value(bigIntProtoToString);

  auto bigIntProtoValueOf = GarbageCollector::makeGC<Function>();
  bigIntProtoValueOf->isNative = true;
  bigIntProtoValueOf->properties["__throw_on_new__"] = Value(true);
  bigIntProtoValueOf->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoValueOf->properties["name"] = Value(std::string("valueOf"));
  bigIntProtoValueOf->properties["length"] = Value(0.0);
  bigIntProtoValueOf->nativeFunc = [thisBigIntValue](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.valueOf requires BigInt");
    }
    return Value(BigInt(thisBigIntValue(args[0])));
  };
  bigIntProto->properties["valueOf"] = Value(bigIntProtoValueOf);

  auto bigIntProtoToLocaleString = GarbageCollector::makeGC<Function>();
  bigIntProtoToLocaleString->isNative = true;
  bigIntProtoToLocaleString->properties["__throw_on_new__"] = Value(true);
  bigIntProtoToLocaleString->properties["__uses_this_arg__"] = Value(true);
  bigIntProtoToLocaleString->properties["name"] = Value(std::string("toLocaleString"));
  bigIntProtoToLocaleString->properties["length"] = Value(0.0);
  bigIntProtoToLocaleString->nativeFunc = [thisBigIntValue](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      throw std::runtime_error("TypeError: BigInt.prototype.toLocaleString requires BigInt");
    }
    return Value(bigint::toString(thisBigIntValue(args[0])));
  };
  bigIntProto->properties["toLocaleString"] = Value(bigIntProtoToLocaleString);

  // BigInt.prototype.constructor = BigInt
  bigIntProto->properties["constructor"] = Value(bigIntFn);

  // BigInt.prototype[Symbol.toStringTag] = "BigInt" (non-writable)
  bigIntProto->properties[WellKnownSymbols::toStringTagKey()] = Value(std::string("BigInt"));
  bigIntProto->properties["__non_writable_" + WellKnownSymbols::toStringTagKey()] = Value(true);
  bigIntProto->properties["__non_enum_" + WellKnownSymbols::toStringTagKey()] = Value(true);

  // Mark all BigInt.prototype properties as non-enumerable
  bigIntProto->properties["__non_enum_toString"] = Value(true);
  bigIntProto->properties["__non_enum_valueOf"] = Value(true);
  bigIntProto->properties["__non_enum_toLocaleString"] = Value(true);
  bigIntProto->properties["__non_enum_constructor"] = Value(true);

  bigIntFn->properties["prototype"] = Value(bigIntProto);
  bigIntFn->properties["__non_writable_prototype"] = Value(true);
  bigIntFn->properties["__non_configurable_prototype"] = Value(true);

  env->define("BigInt", Value(bigIntFn));

  auto createTypedArrayConstructor = [](TypedArrayType type, const std::string& name) {
    auto func = GarbageCollector::makeGC<Function>();
    func->isNative = true;
    func->isConstructor = true;
    func->properties["name"] = Value(name);
    func->properties["length"] = Value(1.0);
    func->nativeFunc = [type](const std::vector<Value>& args) -> Value {
      if (args.empty()) {
        return Value(GarbageCollector::makeGC<TypedArray>(type, 0));
      }

      // Check if first argument is an array
      if (std::holds_alternative<GCPtr<Array>>(args[0].data)) {
        auto arr = args[0].getGC<Array>();
        auto typedArray = GarbageCollector::makeGC<TypedArray>(type, arr->elements.size());

        // Fill the typed array with values from the regular array
        for (size_t i = 0; i < arr->elements.size(); ++i) {
          double val = arr->elements[i].toNumber();
          typedArray->setElement(i, val);
        }

        return Value(typedArray);
      }

      // Otherwise treat as length
      double lengthNum = args[0].toNumber();
      if (std::isnan(lengthNum) || std::isinf(lengthNum) || lengthNum < 0) {
        // Invalid length, return empty array
        return Value(GarbageCollector::makeGC<TypedArray>(type, 0));
      }

      size_t length = static_cast<size_t>(lengthNum);
      // Sanity check: prevent allocating huge arrays
      if (length > 1000000000) { // 1GB limit
        return Value(GarbageCollector::makeGC<TypedArray>(type, 0));
      }

      return Value(GarbageCollector::makeGC<TypedArray>(type, length));
    };
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    func->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(func);
    prototype->properties["name"] = Value(name);
    return Value(func);
  };

  env->define("Int8Array", createTypedArrayConstructor(TypedArrayType::Int8, "Int8Array"));
  env->define("Uint8Array", createTypedArrayConstructor(TypedArrayType::Uint8, "Uint8Array"));
  env->define("Uint8ClampedArray", createTypedArrayConstructor(TypedArrayType::Uint8Clamped, "Uint8ClampedArray"));
  env->define("Int16Array", createTypedArrayConstructor(TypedArrayType::Int16, "Int16Array"));
  env->define("Uint16Array", createTypedArrayConstructor(TypedArrayType::Uint16, "Uint16Array"));
  env->define("Float16Array", createTypedArrayConstructor(TypedArrayType::Float16, "Float16Array"));
  env->define("Int32Array", createTypedArrayConstructor(TypedArrayType::Int32, "Int32Array"));
  env->define("Uint32Array", createTypedArrayConstructor(TypedArrayType::Uint32, "Uint32Array"));
  env->define("Float32Array", createTypedArrayConstructor(TypedArrayType::Float32, "Float32Array"));
  env->define("Float64Array", createTypedArrayConstructor(TypedArrayType::Float64, "Float64Array"));
  env->define("BigInt64Array", createTypedArrayConstructor(TypedArrayType::BigInt64, "BigInt64Array"));
  env->define("BigUint64Array", createTypedArrayConstructor(TypedArrayType::BigUint64, "BigUint64Array"));

  // ArrayBuffer constructor
  auto arrayBufferConstructor = GarbageCollector::makeGC<Function>();
  arrayBufferConstructor->isNative = true;
  arrayBufferConstructor->isConstructor = true;
  arrayBufferConstructor->properties["name"] = Value(std::string("ArrayBuffer"));
  arrayBufferConstructor->properties["length"] = Value(1.0);
  arrayBufferConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    size_t length = 0;
    if (!args.empty()) {
      length = static_cast<size_t>(args[0].toNumber());
    }
    auto buffer = GarbageCollector::makeGC<ArrayBuffer>(length);
    GarbageCollector::instance().reportAllocation(length);
    return Value(buffer);
  };
  {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    arrayBufferConstructor->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(arrayBufferConstructor);
    prototype->properties["name"] = Value(std::string("ArrayBuffer"));
  }
  env->define("ArrayBuffer", Value(arrayBufferConstructor));

  // SharedArrayBuffer (ES2017) - backed by ArrayBuffer in this runtime.
  auto sharedArrayBufferConstructor = GarbageCollector::makeGC<Function>();
  sharedArrayBufferConstructor->isNative = true;
  sharedArrayBufferConstructor->isConstructor = true;
  sharedArrayBufferConstructor->properties["name"] = Value(std::string("SharedArrayBuffer"));
  sharedArrayBufferConstructor->properties["length"] = Value(1.0);
  sharedArrayBufferConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    size_t length = 0;
    if (!args.empty()) {
      length = static_cast<size_t>(args[0].toNumber());
    }
    auto buffer = GarbageCollector::makeGC<ArrayBuffer>(length);
    GarbageCollector::instance().reportAllocation(length);
    return Value(buffer);
  };
  {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    sharedArrayBufferConstructor->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(sharedArrayBufferConstructor);
    prototype->properties["name"] = Value(std::string("SharedArrayBuffer"));
  }
  env->define("SharedArrayBuffer", Value(sharedArrayBufferConstructor));

  // DataView constructor
  auto dataViewConstructor = GarbageCollector::makeGC<Function>();
  dataViewConstructor->isNative = true;
  dataViewConstructor->isConstructor = true;
  dataViewConstructor->properties["name"] = Value(std::string("DataView"));
  dataViewConstructor->properties["length"] = Value(1.0);
  dataViewConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArrayBuffer()) {
      throw std::runtime_error("TypeError: DataView requires an ArrayBuffer");
    }

    auto buffer = args[0].getGC<ArrayBuffer>();
    size_t byteOffset = 0;
    size_t byteLength = 0;

    if (args.size() > 1) {
      byteOffset = static_cast<size_t>(args[1].toNumber());
    }
    if (args.size() > 2) {
      byteLength = static_cast<size_t>(args[2].toNumber());
    }

    auto dataView = GarbageCollector::makeGC<DataView>(buffer, byteOffset, byteLength);
    GarbageCollector::instance().reportAllocation(sizeof(DataView));
    return Value(dataView);
  };
  {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    dataViewConstructor->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(dataViewConstructor);
    prototype->properties["name"] = Value(std::string("DataView"));
  }
  env->define("DataView", Value(dataViewConstructor));

  auto cryptoObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto sha256Fn = GarbageCollector::makeGC<Function>();
  sha256Fn->isNative = true;
  sha256Fn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result = crypto::SHA256::hashHex(
      reinterpret_cast<const uint8_t*>(input.c_str()), input.length()
    );
    return Value(result);
  };
  cryptoObj->properties["sha256"] = Value(sha256Fn);

  auto hmacFn = GarbageCollector::makeGC<Function>();
  hmacFn->isNative = true;
  hmacFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(std::string(""));
    std::string key = args[0].toString();
    std::string message = args[1].toString();
    std::string result = crypto::HMAC::computeHex(
      reinterpret_cast<const uint8_t*>(key.c_str()), key.length(),
      reinterpret_cast<const uint8_t*>(message.c_str()), message.length()
    );
    return Value(result);
  };
  cryptoObj->properties["hmac"] = Value(hmacFn);

  auto toHexFn = GarbageCollector::makeGC<Function>();
  toHexFn->isNative = true;
  toHexFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result = crypto::toHex(
      reinterpret_cast<const uint8_t*>(input.c_str()), input.length()
    );
    return Value(result);
  };
  cryptoObj->properties["toHex"] = Value(toHexFn);

  // crypto.randomUUID - generate RFC 4122 version 4 UUID
  auto randomUUIDFn = GarbageCollector::makeGC<Function>();
  randomUUIDFn->isNative = true;
  randomUUIDFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Generate 16 random bytes
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 255);

    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) {
      bytes[i] = static_cast<uint8_t>(dis(gen));
    }

    // Set version (4) and variant (RFC 4122)
    bytes[6] = (bytes[6] & 0x0f) | 0x40;  // Version 4
    bytes[8] = (bytes[8] & 0x3f) | 0x80;  // Variant RFC 4122

    // Format as UUID string
    char uuid[37];
    snprintf(uuid, sizeof(uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return Value(std::string(uuid));
  };
  cryptoObj->properties["randomUUID"] = Value(randomUUIDFn);

  // crypto.getRandomValues - fill typed array with random values
  auto getRandomValuesFn = GarbageCollector::makeGC<Function>();
  getRandomValuesFn->isNative = true;
  getRandomValuesFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isTypedArray()) {
      return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "getRandomValues requires a TypedArray"));
    }

    auto typedArray = args[0].getGC<TypedArray>();
    std::random_device rd;
    std::mt19937 gen(rd());

    // Fill buffer with random bytes
    std::uniform_int_distribution<uint32_t> dis(0, 255);
    for (size_t i = 0; i < typedArray->buffer.size(); ++i) {
      typedArray->buffer[i] = static_cast<uint8_t>(dis(gen));
    }

    return args[0];  // Return the same array
  };
  cryptoObj->properties["getRandomValues"] = Value(getRandomValuesFn);

  env->define("crypto", Value(cryptoObj));

  auto fetchFn = GarbageCollector::makeGC<Function>();
  fetchFn->isNative = true;
  fetchFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    std::string url = args[0].toString();

    auto promise = GarbageCollector::makeGC<Promise>();
    http::HTTPClient client;

    try {
      http::Response httpResp = client.get(url);

      auto respObj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      respObj->properties["status"] = Value(static_cast<double>(httpResp.statusCode));
      respObj->properties["statusText"] = Value(httpResp.statusText);
      respObj->properties["ok"] = Value(httpResp.statusCode >= 200 && httpResp.statusCode < 300);

      auto textFn = GarbageCollector::makeGC<Function>();
      textFn->isNative = true;
      std::string bodyText = httpResp.bodyAsString();
      textFn->nativeFunc = [bodyText](const std::vector<Value>&) -> Value {
        return Value(bodyText);
      };
      respObj->properties["text"] = Value(textFn);

      auto headersObj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (const auto& [key, value] : httpResp.headers) {
        headersObj->properties[key] = Value(value);
      }
      respObj->properties["headers"] = Value(headersObj);

      promise->resolve(Value(respObj));
    } catch (...) {
      promise->reject(Value(std::string("Fetch failed")));
    }

    return Value(promise);
  };
  env->define("fetch", Value(fetchFn));

  // Dynamic import() function - returns a Promise
  auto importFn = GarbageCollector::makeGC<Function>();
  importFn->isNative = true;
  importFn->nativeFunc = [arrayToString](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      auto promise = GarbageCollector::makeGC<Promise>();
      auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "import() requires a module specifier");
      promise->reject(Value(err));
      return Value(promise);
    }

    auto promise = GarbageCollector::makeGC<Promise>();
    std::string specifier;
    enum class ImportPhase {
      Normal,
      Source,
      Defer,
    };
    ImportPhase importPhase = ImportPhase::Normal;
    bool hasImportOptions = false;
    Value importOptions = Value(Undefined{});
    if (args.size() > 1 && args[1].isString()) {
      const std::string& maybePhase = std::get<std::string>(args[1].data);
      if (maybePhase == kImportPhaseSourceSentinel) {
        importPhase = ImportPhase::Source;
      } else if (maybePhase == kImportPhaseDeferSentinel) {
        importPhase = ImportPhase::Defer;
      } else {
        hasImportOptions = true;
        importOptions = args[1];
      }
    } else if (args.size() > 1) {
      hasImportOptions = true;
      importOptions = args[1];
    }

    auto rejectWith = [&](ErrorType fallbackType, const std::string& fallbackMessage, const std::optional<Value>& candidate = std::nullopt) {
      if (candidate.has_value()) {
        promise->reject(*candidate);
      } else {
        auto err = GarbageCollector::makeGC<Error>(fallbackType, fallbackMessage);
        promise->reject(Value(err));
      }
    };

    auto isObjectLikeImport = [](const Value& value) -> bool {
      return value.isObject() || value.isArray() || value.isFunction() || value.isRegex() || value.isProxy();
    };

    auto valueFromErrorMessage = [](std::string message) -> Value {
      ErrorType errorType = ErrorType::Error;
      auto consumePrefix = [&](const std::string& prefix, ErrorType type) {
        if (message.rfind(prefix, 0) == 0) {
          errorType = type;
          message = message.substr(prefix.size());
          return true;
        }
        return false;
      };
      if (!consumePrefix("TypeError: ", ErrorType::TypeError) &&
          !consumePrefix("ReferenceError: ", ErrorType::ReferenceError) &&
          !consumePrefix("RangeError: ", ErrorType::RangeError) &&
          !consumePrefix("SyntaxError: ", ErrorType::SyntaxError) &&
          !consumePrefix("URIError: ", ErrorType::URIError) &&
          !consumePrefix("EvalError: ", ErrorType::EvalError) &&
          !consumePrefix("Error: ", ErrorType::Error)) {
        return Value(message);
      }
      return Value(GarbageCollector::makeGC<Error>(errorType, message));
    };

    auto makeError = [](ErrorType type, const std::string& message) -> Value {
      return Value(GarbageCollector::makeGC<Error>(type, message));
    };

    auto callImportCallable = [&](const Value& callee,
                                  const std::vector<Value>& callArgs,
                                  const Value& thisArg,
                                  Value& out,
                                  Value& abrupt) -> bool {
      if (!callee.isFunction()) {
        out = Value(Undefined{});
        return true;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const std::exception& e) {
          abrupt = valueFromErrorMessage(e.what());
          return false;
        } catch (...) {
          abrupt = makeError(ErrorType::Error, "Unknown native error");
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        abrupt = makeError(ErrorType::TypeError, "Interpreter unavailable for callable conversion");
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        abrupt = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    std::function<bool(const Value&, const std::string&, bool&, Value&, Value&)> getImportProperty;
    getImportProperty =
      [&](const Value& receiver, const std::string& key, bool& found, Value& out, Value& abrupt) -> bool {
      if (receiver.isProxy()) {
        auto proxyPtr = receiver.getGC<Proxy>();
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
          auto getTrapIt = handlerObj->properties.find("get");
          if (getTrapIt != handlerObj->properties.end() && getTrapIt->second.isFunction()) {
            Value trapOut = Value(Undefined{});
            if (!callImportCallable(getTrapIt->second, {*proxyPtr->target, Value(key), receiver}, Value(Undefined{}), trapOut, abrupt)) {
              return false;
            }
            found = true;
            out = trapOut;
            return true;
          }
        }
        if (proxyPtr->target) {
          return getImportProperty(*proxyPtr->target, key, found, out, abrupt);
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isObject()) {
        auto current = receiver.getGC<Object>();
        int depth = 0;
        while (current && depth <= 16) {
          depth++;
          std::string getterKey = "__get_" + key;
          auto getterIt = current->properties.find(getterKey);
          if (getterIt != current->properties.end()) {
            if (getterIt->second.isFunction()) {
              Value getterOut = Value(Undefined{});
              if (!callImportCallable(getterIt->second, {}, receiver, getterOut, abrupt)) {
                return false;
              }
              found = true;
              out = getterOut;
              return true;
            }
            found = true;
            out = Value(Undefined{});
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = protoIt->second.getGC<Object>();
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isFunction()) {
        auto fn = receiver.getGC<Function>();
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        found = false;
        out = Value(Undefined{});
        return true;
      }

      if (receiver.isRegex()) {
        auto regex = receiver.getGC<Regex>();
        std::string getterKey = "__get_" + key;
        auto getterIt = regex->properties.find(getterKey);
        if (getterIt != regex->properties.end()) {
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            if (!callImportCallable(getterIt->second, {}, receiver, getterOut, abrupt)) {
              return false;
            }
            found = true;
            out = getterOut;
            return true;
          }
          found = true;
          out = Value(Undefined{});
          return true;
        }
        auto it = regex->properties.find(key);
        if (it != regex->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
      }

      found = false;
      out = Value(Undefined{});
      return true;
    };

    auto toPrimitiveImport = [&](const Value& input, bool preferString, Value& out, Value& abrupt) -> bool {
      if (!isObjectLikeImport(input)) {
        out = input;
        return true;
      }

      const std::string& toPrimitiveKey = WellKnownSymbols::toPrimitiveKey();
      bool hasExotic = false;
      Value exotic = Value(Undefined{});
      if (!getImportProperty(input, toPrimitiveKey, hasExotic, exotic, abrupt)) {
        return false;
      }
      if (hasExotic && !exotic.isUndefined() && !exotic.isNull()) {
        if (!exotic.isFunction()) {
          abrupt = makeError(ErrorType::TypeError, "@@toPrimitive is not callable");
          return false;
        }
        std::string hint = preferString ? "string" : "number";
        Value result = Value(Undefined{});
        if (!callImportCallable(exotic, {Value(hint)}, input, result, abrupt)) {
          return false;
        }
        if (isObjectLikeImport(result)) {
          abrupt = makeError(ErrorType::TypeError, "@@toPrimitive must return a primitive value");
          return false;
        }
        out = result;
        return true;
      }

      std::array<std::string, 2> methodOrder = preferString
        ? std::array<std::string, 2>{"toString", "valueOf"}
        : std::array<std::string, 2>{"valueOf", "toString"};
      for (const auto& methodName : methodOrder) {
        bool found = false;
        Value method = Value(Undefined{});
        if (!getImportProperty(input, methodName, found, method, abrupt)) {
          return false;
        }
        if (found && method.isFunction()) {
          Value result = Value(Undefined{});
          if (!callImportCallable(method, {}, input, result, abrupt)) {
            return false;
          }
          if (!isObjectLikeImport(result)) {
            out = result;
            return true;
          }
        }
      }

      if (input.isArray()) {
        out = Value(arrayToString(input.getGC<Array>()));
        return true;
      }
      if (input.isObject() || input.isFunction() || input.isProxy()) {
        out = Value(std::string("[object Object]"));
        return true;
      }
      if (input.isRegex()) {
        out = Value(input.toString());
        return true;
      }

      abrupt = makeError(ErrorType::TypeError, "Cannot convert object to primitive value");
      return false;
    };

    std::function<bool(const Value&, std::vector<std::string>&, Value&)> enumerateImportAttributeKeys;
    enumerateImportAttributeKeys =
      [&](const Value& source, std::vector<std::string>& keys, Value& abrupt) -> bool {
      std::unordered_set<std::string> seen;
      auto pushKey = [&](const std::string& key) {
        if (seen.find(key) != seen.end()) return;
        seen.insert(key);
        keys.push_back(key);
      };

      if (source.isProxy()) {
        auto proxyPtr = source.getGC<Proxy>();
        if (proxyPtr->handler && proxyPtr->handler->isObject()) {
          auto handlerObj = std::get<GCPtr<Object>>(proxyPtr->handler->data);
          auto ownKeysIt = handlerObj->properties.find("ownKeys");
          if (ownKeysIt != handlerObj->properties.end() && ownKeysIt->second.isFunction()) {
            Value ownKeysResult = Value(Undefined{});
            if (!callImportCallable(ownKeysIt->second, {*proxyPtr->target}, Value(Undefined{}), ownKeysResult, abrupt)) {
              return false;
            }

            std::vector<std::string> candidateKeys;
            if (ownKeysResult.isArray()) {
              auto arr = ownKeysResult.getGC<Array>();
              for (const auto& entry : arr->elements) {
                if (entry.isString()) {
                  candidateKeys.push_back(std::get<std::string>(entry.data));
                }
              }
            }

            auto gopdIt = handlerObj->properties.find("getOwnPropertyDescriptor");
            for (const auto& key : candidateKeys) {
              bool enumerable = true;
              if (gopdIt != handlerObj->properties.end() && gopdIt->second.isFunction()) {
                Value desc = Value(Undefined{});
                if (!callImportCallable(gopdIt->second, {*proxyPtr->target, Value(key)}, Value(Undefined{}), desc, abrupt)) {
                  return false;
                }
                if (desc.isUndefined()) {
                  enumerable = false;
                } else {
                  bool foundEnumerable = false;
                  Value enumerableValue = Value(Undefined{});
                  if (!getImportProperty(desc, "enumerable", foundEnumerable, enumerableValue, abrupt)) {
                    return false;
                  }
                  enumerable = foundEnumerable && enumerableValue.toBool();
                }
              }
              if (enumerable) {
                pushKey(key);
              }
            }
            return true;
          }
        }

        if (proxyPtr->target) {
          return enumerateImportAttributeKeys(*proxyPtr->target, keys, abrupt);
        }
        return true;
      }

      if (source.isObject()) {
        auto withObj = source.getGC<Object>();
        for (const auto& [key, _] : withObj->properties) {
          if (key.rfind("__get_", 0) == 0) {
            pushKey(key.substr(6));
            continue;
          }
          if (key.rfind("__", 0) == 0) {
            continue;
          }
          pushKey(key);
        }
      } else if (source.isArray()) {
        auto arr = source.getGC<Array>();
        for (size_t i = 0; i < arr->elements.size(); ++i) {
          pushKey(std::to_string(i));
        }
      } else if (source.isFunction()) {
        auto fn = source.getGC<Function>();
        for (const auto& [key, _] : fn->properties) {
          if (key.rfind("__", 0) == 0) {
            continue;
          }
          pushKey(key);
        }
      } else {
        return false;
      }
      return true;
    };

    try {
      if (args[0].isObject()) {
        auto obj = args[0].getGC<Object>();
        auto importMetaIt = obj->properties.find("__import_meta__");
        if (importMetaIt != obj->properties.end() &&
            importMetaIt->second.isBool() &&
            importMetaIt->second.toBool()) {
          auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Cannot convert object to primitive value");
          promise->reject(Value(err));
          return Value(promise);
        }
      }
      Value primitiveSpecifier = Value(Undefined{});
      Value abrupt = Value(Undefined{});
      if (!toPrimitiveImport(args[0], true, primitiveSpecifier, abrupt)) {
        promise->reject(abrupt);
        return Value(promise);
      }
      if (primitiveSpecifier.isSymbol()) {
        auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Cannot convert a Symbol value to a string");
        promise->reject(Value(err));
        return Value(promise);
      }
      specifier = primitiveSpecifier.toString();

      if (importPhase == ImportPhase::Source) {
        auto err = GarbageCollector::makeGC<Error>(ErrorType::SyntaxError, "Source phase import is not available");
        promise->reject(Value(err));
        return Value(promise);
      }

      if (hasImportOptions && !importOptions.isUndefined()) {
        if (!isObjectLikeImport(importOptions)) {
          auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "import() options must be an object");
          promise->reject(Value(err));
          return Value(promise);
        }

        bool hasWith = false;
        Value withValue = Value(Undefined{});
        Value abrupt = Value(Undefined{});
        if (!getImportProperty(importOptions, "with", hasWith, withValue, abrupt)) {
          promise->reject(abrupt);
          return Value(promise);
        }

        if (hasWith && !withValue.isUndefined()) {
          if (!isObjectLikeImport(withValue)) {
            auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "import() options.with must be an object");
            promise->reject(Value(err));
            return Value(promise);
          }

          std::vector<std::string> keys;
          if (!enumerateImportAttributeKeys(withValue, keys, abrupt)) {
            promise->reject(abrupt);
            return Value(promise);
          }

          for (const auto& key : keys) {
            bool foundAttr = false;
            Value attrValue = Value(Undefined{});
            if (!getImportProperty(withValue, key, foundAttr, attrValue, abrupt)) {
              promise->reject(abrupt);
              return Value(promise);
            }
            if (!attrValue.isString()) {
              auto err = GarbageCollector::makeGC<Error>(ErrorType::TypeError, "import() options.with values must be strings");
              promise->reject(Value(err));
              return Value(promise);
            }
          }
        }
      }

      // Use global module loader if available
      if (g_moduleLoader && g_interpreter) {
        // Resolve the module path
        std::string resolvedPath = g_moduleLoader->resolvePath(specifier);

        // Load the module
        auto module = g_moduleLoader->loadModule(resolvedPath);
        if (!module) {
          rejectWith(ErrorType::Error, "Failed to load module: " + specifier, g_moduleLoader->getLastError());
          return Value(promise);
        }

        // Instantiate the module
        if (!module->instantiate(g_moduleLoader.get())) {
          rejectWith(ErrorType::SyntaxError, "Failed to instantiate module: " + specifier, module->getLastError());
          return Value(promise);
        }

        bool deferEvaluationUntilNamespaceAccess = false;
        if (importPhase == ImportPhase::Defer) {
          // Defer phase: eagerly evaluate only asynchronous transitive dependencies.
          std::unordered_set<std::string> visitedModules;
          std::unordered_set<std::string> queuedAsyncModules;
          std::vector<std::string> orderedAsyncModules;
          gatherAsyncTransitiveDependencies(
            resolvedPath, g_moduleLoader.get(), visitedModules, queuedAsyncModules, orderedAsyncModules);

          for (const auto& asyncModulePath : orderedAsyncModules) {
            auto asyncModule = g_moduleLoader->loadModule(asyncModulePath);
            if (!asyncModule) {
              rejectWith(
                ErrorType::Error,
                "Failed to load deferred async dependency: " + asyncModulePath,
                g_moduleLoader->getLastError());
              return Value(promise);
            }
            if (!asyncModule->instantiate(g_moduleLoader.get())) {
              rejectWith(
                ErrorType::SyntaxError,
                "Failed to instantiate deferred async dependency: " + asyncModulePath,
                asyncModule->getLastError());
              return Value(promise);
            }
            if (!asyncModule->evaluate(g_interpreter)) {
              rejectWith(
                ErrorType::Error,
                "Failed to evaluate deferred async dependency: " + asyncModulePath,
                asyncModule->getLastError());
              return Value(promise);
            }
          }

          deferEvaluationUntilNamespaceAccess = (module->getState() != Module::State::Evaluated);
        } else {
          // Normal dynamic import evaluates immediately.
          if (!module->evaluate(g_interpreter)) {
            rejectWith(ErrorType::Error, "Failed to evaluate module: " + specifier, module->getLastError());
            return Value(promise);
          }
        }

        if (module->getState() == Module::State::EvaluatingAsync) {
          auto evalPromise = module->getEvaluationPromise();
          if (evalPromise) {
            evalPromise->then(
              [promise, module](Value) -> Value {
                promise->resolve(Value(module->getNamespaceObject()));
                return Value(Undefined{});
              },
              [promise](Value reason) -> Value {
                promise->reject(reason);
                return Value(Undefined{});
              }
            );
            return Value(promise);
          }
        }

        auto moduleNamespace = module->getNamespaceObject();
        if (deferEvaluationUntilNamespaceAccess) {
          auto deferredEvalFn = GarbageCollector::makeGC<Function>();
          deferredEvalFn->isNative = true;
          deferredEvalFn->properties["__throw_on_new__"] = Value(true);
          auto deferredModule = module;
          deferredEvalFn->nativeFunc = [deferredModule](const std::vector<Value>&) -> Value {
            if (deferredModule->getState() == Module::State::Evaluated) {
              return Value(Undefined{});
            }
            Interpreter* interpreter = getGlobalInterpreter();
            if (!interpreter) {
              throw std::runtime_error("Error: Interpreter unavailable for deferred module evaluation");
            }
            if (!deferredModule->evaluate(interpreter)) {
              if (auto error = deferredModule->getLastError()) {
                throw std::runtime_error(error->toString());
              }
              throw std::runtime_error("Error: Failed to evaluate deferred module");
            }
            return Value(Undefined{});
          };
          moduleNamespace->properties["__deferred_pending__"] = Value(true);
          moduleNamespace->properties["__deferred_eval__"] = Value(deferredEvalFn);
        }

        promise->resolve(Value(moduleNamespace));
      } else {
        // Fallback: create a placeholder namespace (for cases where module loader isn't set up)
        auto moduleNamespace = GarbageCollector::makeGC<Object>();
        GarbageCollector::instance().reportAllocation(sizeof(Object));
        moduleNamespace->properties["__esModule"] = Value(true);
        moduleNamespace->properties["__moduleSpecifier"] = Value(specifier);
        promise->resolve(Value(moduleNamespace));
      }
    } catch (const std::exception& e) {
      std::string message = e.what();
      ErrorType errorType = ErrorType::Error;
      auto consumePrefix = [&](const std::string& prefix, ErrorType type) {
        if (message.rfind(prefix, 0) == 0) {
          errorType = type;
          message = message.substr(prefix.size());
          return true;
        }
        return false;
      };
      if (!consumePrefix("TypeError: ", ErrorType::TypeError) &&
          !consumePrefix("ReferenceError: ", ErrorType::ReferenceError) &&
          !consumePrefix("RangeError: ", ErrorType::RangeError) &&
          !consumePrefix("SyntaxError: ", ErrorType::SyntaxError) &&
          !consumePrefix("URIError: ", ErrorType::URIError) &&
          !consumePrefix("EvalError: ", ErrorType::EvalError) &&
          !consumePrefix("Error: ", ErrorType::Error)) {
        // Keep non-Error abrupt completions (e.g. thrown strings) as-is.
        promise->reject(Value(message));
        return Value(promise);
      }
      auto err = GarbageCollector::makeGC<Error>(errorType, message);
      promise->reject(Value(err));
    } catch (...) {
      auto err = GarbageCollector::makeGC<Error>(ErrorType::Error, "Failed to load module: " + specifier);
      promise->reject(Value(err));
    }

    return Value(promise);
  };
  env->define("import", Value(importFn));

  auto regExpPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto regExpMatchAll = GarbageCollector::makeGC<Function>();
  regExpMatchAll->isNative = true;
  regExpMatchAll->isConstructor = false;
  regExpMatchAll->properties["__uses_this_arg__"] = Value(true);
  regExpMatchAll->properties["__throw_on_new__"] = Value(true);
  regExpMatchAll->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isRegex()) {
      throw std::runtime_error("TypeError: RegExp.prototype[@@matchAll] called on non-RegExp");
    }

    auto regexPtr = args[0].getGC<Regex>();
    std::string input = args.size() > 1 ? args[1].toString() : "";
    bool global = regexPtr->flags.find('g') != std::string::npos;
    bool unicodeMode = regexPtr->flags.find('u') != std::string::npos ||
                       regexPtr->flags.find('v') != std::string::npos;
    auto utf16IndexFromEngineOffset = [&input](size_t rawOffset) -> double {
      auto accumulateUtf16ByCodePointCount = [&input](size_t codePointCountTarget) -> size_t {
        size_t cursor = 0;
        size_t codePointCount = 0;
        size_t utf16Units = 0;
        while (cursor < input.size() && codePointCount < codePointCountTarget) {
          uint32_t codePoint = unicode::decodeUTF8(input, cursor);
          utf16Units += (codePoint > 0xFFFF) ? 2 : 1;
          codePointCount++;
        }
        return utf16Units;
      };

      // Some regex backends report byte offsets, others effectively report code point offsets.
      // If the offset lands inside a UTF-8 sequence, treat it as a code point count.
      if (rawOffset < input.size() &&
          unicode::isContinuationByte(static_cast<uint8_t>(input[rawOffset]))) {
        return static_cast<double>(accumulateUtf16ByCodePointCount(rawOffset));
      }

      size_t cursor = 0;
      size_t utf16Units = 0;
      while (cursor < rawOffset && cursor < input.size()) {
        uint32_t codePoint = unicode::decodeUTF8(input, cursor);
        utf16Units += (codePoint > 0xFFFF) ? 2 : 1;
      }
      return static_cast<double>(utf16Units);
    };

    auto allMatches = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

#if USE_SIMPLE_REGEX
    std::string remaining = input;
    size_t offsetBytes = 0;
    std::vector<simple_regex::Regex::Match> matches;
    while (regexPtr->regex->search(remaining, matches)) {
      if (matches.empty()) break;
      auto matchArr = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (size_t i = 0; i < matches.size(); ++i) {
        matchArr->elements.push_back(Value(matches[i].str));
      }
      size_t matchStartBytes = offsetBytes + matches[0].start;
      double matchIndex = utf16IndexFromEngineOffset(matchStartBytes);
      matchArr->properties["index"] = Value(matchIndex);
      matchArr->properties["input"] = Value(input);
      allMatches->elements.push_back(Value(matchArr));

      if (!global) break;

      size_t matchAdvance = matches[0].start + matches[0].str.length();
      if (matchAdvance == 0) {
        if (!remaining.empty()) {
          if (unicodeMode) {
            matchAdvance = unicode::utf8SequenceLength(static_cast<uint8_t>(remaining[0]));
          } else {
            matchAdvance = 1;
          }
        } else {
          break;
        }
      }
      offsetBytes += matchAdvance;
      remaining = remaining.substr(matchAdvance);
      matches.clear();
    }
#else
    std::string::const_iterator searchStart = input.cbegin();
    std::smatch match;
    while (std::regex_search(searchStart, input.cend(), match, regexPtr->regex)) {
      auto matchArr = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (size_t i = 0; i < match.size(); ++i) {
        matchArr->elements.push_back(Value(match[i].str()));
      }
      size_t matchStartBytes = static_cast<size_t>(match.position() + (searchStart - input.cbegin()));
      double matchIndex = utf16IndexFromEngineOffset(matchStartBytes);
      matchArr->properties["index"] = Value(matchIndex);
      matchArr->properties["input"] = Value(input);
      allMatches->elements.push_back(Value(matchArr));

      if (!global) break;
      searchStart = match.suffix().first;
      if (match[0].length() == 0) {
        if (searchStart != input.cend()) {
          if (unicodeMode) {
            size_t byteOffset = static_cast<size_t>(searchStart - input.cbegin());
            size_t advance = unicode::utf8SequenceLength(static_cast<uint8_t>(input[byteOffset]));
            std::advance(searchStart, static_cast<std::ptrdiff_t>(advance));
          } else {
            ++searchStart;
          }
        } else {
          break;  // empty match at end of string, stop
        }
      }
    }
#endif

    // Return a RegExpStringIterator (object with .next() method)
    auto iterObj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto idx = std::make_shared<size_t>(0);
    auto nextFn = GarbageCollector::makeGC<Function>();
    nextFn->isNative = true;
    nextFn->nativeFunc = [allMatches, idx](const std::vector<Value>& /*args*/) -> Value {
      auto result = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      if (*idx < allMatches->elements.size()) {
        result->properties["value"] = allMatches->elements[*idx];
        result->properties["done"] = Value(false);
        (*idx)++;
      } else {
        result->properties["value"] = Value(Undefined{});
        result->properties["done"] = Value(true);
      }
      return Value(result);
    };
    iterObj->properties["next"] = Value(nextFn);
    return Value(iterObj);
  };
  regExpPrototype->properties[WellKnownSymbols::matchAllKey()] = Value(regExpMatchAll);

  auto regExpConstructor = GarbageCollector::makeGC<Function>();
  regExpConstructor->isNative = true;
  regExpConstructor->isConstructor = true;
  regExpConstructor->properties["name"] = Value(std::string("RegExp"));
  regExpConstructor->properties["length"] = Value(2.0);
  regExpConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    std::string pattern = args.empty() ? std::string("") : args[0].toString();
    std::string flags = args.size() > 1 ? args[1].toString() : "";
    auto rx = GarbageCollector::makeGC<Regex>(pattern, flags);
    // lastIndex: writable, non-enumerable, non-configurable
    rx->properties["lastIndex"] = Value(0.0);
    rx->properties["__non_enum_lastIndex"] = Value(true);
    rx->properties["__non_configurable_lastIndex"] = Value(true);
    return Value(rx);
  };
  regExpConstructor->properties["prototype"] = Value(regExpPrototype);
  env->define("RegExp", Value(regExpConstructor));

  // Error constructors
  auto createErrorConstructor = [](ErrorType type, const std::string& name) {
    auto prototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    auto func = GarbageCollector::makeGC<Function>();
    func->isNative = true;
    func->isConstructor = true;
    func->nativeFunc = [type](const std::vector<Value>& args) -> Value {
      bool hasMessage = args.size() >= 1 && !args[0].isUndefined();
      std::string message = hasMessage ? args[0].toString() : "";
      auto err = GarbageCollector::makeGC<Error>(type, message);
      if (hasMessage) {
        err->properties["message"] = Value(message);
        err->properties["__non_enum_message"] = Value(true);
      }
      return Value(err);
    };
    func->properties["__error_type__"] = Value(static_cast<double>(static_cast<int>(type)));
    func->properties["prototype"] = Value(prototype);
    prototype->properties["constructor"] = Value(func);
    prototype->properties["name"] = Value(name);
    return func;
  };

  auto errorCtor = createErrorConstructor(ErrorType::Error, "Error");
  auto typeErrorCtor = createErrorConstructor(ErrorType::TypeError, "TypeError");
  auto referenceErrorCtor = createErrorConstructor(ErrorType::ReferenceError, "ReferenceError");
  auto rangeErrorCtor = createErrorConstructor(ErrorType::RangeError, "RangeError");
  auto syntaxErrorCtor = createErrorConstructor(ErrorType::SyntaxError, "SyntaxError");
  auto uriErrorCtor = createErrorConstructor(ErrorType::URIError, "URIError");
  auto evalErrorCtor = createErrorConstructor(ErrorType::EvalError, "EvalError");

  env->define("Error", Value(errorCtor));
  env->define("TypeError", Value(typeErrorCtor));
  env->define("ReferenceError", Value(referenceErrorCtor));
  env->define("RangeError", Value(rangeErrorCtor));
  env->define("SyntaxError", Value(syntaxErrorCtor));
  env->define("URIError", Value(uriErrorCtor));
  env->define("EvalError", Value(evalErrorCtor));

  // AggregateError (ES2021) - minimal constructor/prototype for subclassing tests.
  {
    auto aggregateErrorCtor = GarbageCollector::makeGC<Function>();
    aggregateErrorCtor->isNative = true;
    aggregateErrorCtor->isConstructor = true;
    aggregateErrorCtor->properties["name"] = Value(std::string("AggregateError"));
    aggregateErrorCtor->properties["length"] = Value(2.0);
    aggregateErrorCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
      std::string message;
      if (args.size() >= 2 && !args[1].isUndefined()) {
        message = args[1].toString();
      }
      return Value(GarbageCollector::makeGC<Error>(ErrorType::Error, message));
    };
    auto aggregateErrorProto = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    aggregateErrorCtor->properties["prototype"] = Value(aggregateErrorProto);
    aggregateErrorProto->properties["constructor"] = Value(aggregateErrorCtor);
    aggregateErrorProto->properties["name"] = Value(std::string("AggregateError"));
    auto errorProtoIt = errorCtor->properties.find("prototype");
    if (errorProtoIt != errorCtor->properties.end() && errorProtoIt->second.isObject()) {
      aggregateErrorProto->properties["__proto__"] = errorProtoIt->second;
    }
    env->define("AggregateError", Value(aggregateErrorCtor));
  }

  // Map constructor
  auto mapConstructor = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  mapConstructor->isNative = true;
  mapConstructor->isConstructor = true;
  mapConstructor->properties["name"] = Value(std::string("Map"));
  mapConstructor->properties["length"] = Value(0.0);
  mapConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto mapObj = GarbageCollector::makeGC<Map>();
    GarbageCollector::instance().reportAllocation(sizeof(Map));

    if (!args.empty() && args[0].isArray()) {
      auto entriesArr = args[0].getGC<Array>();
      for (const auto& entryVal : entriesArr->elements) {
        Value k(Undefined{});
        Value v(Undefined{});
        if (entryVal.isArray()) {
          auto entryArr = entryVal.getGC<Array>();
          if (entryArr->elements.size() >= 1) k = entryArr->elements[0];
          if (entryArr->elements.size() >= 2) v = entryArr->elements[1];
          mapObj->set(k, v);
          continue;
        }
        if (entryVal.isObject()) {
          auto entryObj = entryVal.getGC<Object>();
          if (auto it0 = entryObj->properties.find("0"); it0 != entryObj->properties.end()) {
            k = it0->second;
          }
          if (auto it1 = entryObj->properties.find("1"); it1 != entryObj->properties.end()) {
            v = it1->second;
          }
          mapObj->set(k, v);
        }
      }
    }

    return Value(mapObj);
  };
  auto mapPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  mapConstructor->properties["prototype"] = Value(mapPrototype);
  mapPrototype->properties["constructor"] = Value(mapConstructor);
  env->define("Map", Value(mapConstructor));

  // Set constructor
  auto setConstructor = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  setConstructor->isNative = true;
  setConstructor->isConstructor = true;
  setConstructor->properties["name"] = Value(std::string("Set"));
  setConstructor->properties["length"] = Value(0.0);
  setConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto setObj = GarbageCollector::makeGC<Set>();
    GarbageCollector::instance().reportAllocation(sizeof(Set));

    if (!args.empty() && args[0].isArray()) {
      auto entriesArr = args[0].getGC<Array>();
      for (const auto& entryVal : entriesArr->elements) {
        setObj->add(entryVal);
      }
    }

    return Value(setObj);
  };
  auto setPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  setConstructor->properties["prototype"] = Value(setPrototype);
  setPrototype->properties["constructor"] = Value(setConstructor);
  env->define("Set", Value(setConstructor));

  // WeakMap constructor
  auto weakMapConstructor = GarbageCollector::makeGC<Function>();
  weakMapConstructor->isNative = true;
  weakMapConstructor->isConstructor = true;
  weakMapConstructor->properties["name"] = Value(std::string("WeakMap"));
  weakMapConstructor->properties["length"] = Value(0.0);
  weakMapConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    auto wm = GarbageCollector::makeGC<WeakMap>();
    GarbageCollector::instance().reportAllocation(sizeof(WeakMap));
    return Value(wm);
  };
  {
    auto weakMapPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    weakMapConstructor->properties["prototype"] = Value(weakMapPrototype);
    weakMapPrototype->properties["constructor"] = Value(weakMapConstructor);
    weakMapPrototype->properties["name"] = Value(std::string("WeakMap"));
  }
  env->define("WeakMap", Value(weakMapConstructor));

  // WeakSet constructor
  auto weakSetConstructor = GarbageCollector::makeGC<Function>();
  weakSetConstructor->isNative = true;
  weakSetConstructor->isConstructor = true;
  weakSetConstructor->properties["name"] = Value(std::string("WeakSet"));
  weakSetConstructor->properties["length"] = Value(0.0);
  weakSetConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    auto ws = GarbageCollector::makeGC<WeakSet>();
    GarbageCollector::instance().reportAllocation(sizeof(WeakSet));
    return Value(ws);
  };
  {
    auto weakSetPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    weakSetConstructor->properties["prototype"] = Value(weakSetPrototype);
    weakSetPrototype->properties["constructor"] = Value(weakSetConstructor);
    weakSetPrototype->properties["name"] = Value(std::string("WeakSet"));
  }
  env->define("WeakSet", Value(weakSetConstructor));

  // WeakRef (ES2021) - minimal constructor/prototype for subclassing tests.
  auto weakRefConstructor = GarbageCollector::makeGC<Function>();
  weakRefConstructor->isNative = true;
  weakRefConstructor->isConstructor = true;
  weakRefConstructor->properties["name"] = Value(std::string("WeakRef"));
  weakRefConstructor->properties["length"] = Value(1.0);
  weakRefConstructor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto obj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    obj->properties["__weakref_target__"] = args.empty() ? Value(Undefined{}) : args[0];
    return Value(obj);
  };
  {
    auto weakRefPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    weakRefConstructor->properties["prototype"] = Value(weakRefPrototype);
    weakRefPrototype->properties["constructor"] = Value(weakRefConstructor);
    weakRefPrototype->properties["name"] = Value(std::string("WeakRef"));
  }
  env->define("WeakRef", Value(weakRefConstructor));

  // Proxy constructor
  auto proxyConstructor = GarbageCollector::makeGC<Function>();
  proxyConstructor->isNative = true;
  proxyConstructor->isConstructor = true;
  auto isCallableTarget = [](const Value& target) -> bool {
    if (target.isFunction() || target.isClass()) return true;
    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      auto it = obj->properties.find("__callable_object__");
      return it != obj->properties.end() && it->second.isBool() && it->second.toBool();
    }
    if (target.isProxy()) {
      auto p = target.getGC<Proxy>();
      return p && p->isCallable;
    }
    return false;
  };
  proxyConstructor->nativeFunc = [isCallableTarget](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      // Need target and handler
      return Value(Undefined{});
    }
    auto proxy = GarbageCollector::makeGC<Proxy>(args[0], args[1]);
    GarbageCollector::instance().reportAllocation(sizeof(Proxy));
    proxy->isCallable = isCallableTarget(args[0]);
    return Value(proxy);
  };

  // Proxy.revocable(target, handler)
  auto proxyRevocable = GarbageCollector::makeGC<Function>();
  proxyRevocable->isNative = true;
  proxyRevocable->nativeFunc = [isCallableTarget](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) {
      return Value(Undefined{});
    }
    auto proxy = GarbageCollector::makeGC<Proxy>(args[0], args[1]);
    GarbageCollector::instance().reportAllocation(sizeof(Proxy));
    proxy->isCallable = isCallableTarget(args[0]);

    auto revokeFn = GarbageCollector::makeGC<Function>();
    revokeFn->isNative = true;
    revokeFn->nativeFunc = [proxy](const std::vector<Value>&) -> Value {
      if (proxy) {
        proxy->revoked = true;
        if (proxy->target) *proxy->target = Value(Undefined{});
        if (proxy->handler) *proxy->handler = Value(Undefined{});
      }
      return Value(Undefined{});
    };

    auto result = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    result->properties["proxy"] = Value(proxy);
    result->properties["revoke"] = Value(revokeFn);
    return Value(result);
  };
  proxyConstructor->properties["revocable"] = Value(proxyRevocable);
  env->define("Proxy", Value(proxyConstructor));

  // Reflect object with static methods
  auto reflectObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Reflect.get(target, property)
  auto reflectGet = GarbageCollector::makeGC<Function>();
  reflectGet->isNative = true;
  reflectGet->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});

    const Value& target = args[0];
    std::string prop = valueToPropertyKey(args[1]);

    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      if (obj->isModuleNamespace) {
        auto getterIt = obj->properties.find("__get_" + prop);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, target);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            return out;
          }
        }
      }
      auto it = obj->properties.find(prop);
      if (it != obj->properties.end()) {
        return it->second;
      }
    }
    return Value(Undefined{});
  };
  reflectObj->properties["get"] = Value(reflectGet);

  // Reflect.set(target, property, value)
  auto reflectSet = GarbageCollector::makeGC<Function>();
  reflectSet->isNative = true;
  reflectSet->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);

    const Value& target = args[0];
    std::string prop = valueToPropertyKey(args[1]);
    const Value& value = args[2];

    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      if (obj->isModuleNamespace) {
        return Value(false);
      }
      obj->properties[prop] = value;
      return Value(true);
    }
    return Value(false);
  };
  reflectObj->properties["set"] = Value(reflectSet);

  // Reflect.has(target, property)
  auto reflectHas = GarbageCollector::makeGC<Function>();
  reflectHas->isNative = true;
  reflectHas->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    const Value& target = args[0];
    std::string prop = valueToPropertyKey(args[1]);

    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      if (obj->isModuleNamespace) {
        if (prop == WellKnownSymbols::toStringTagKey()) {
          return Value(true);
        }
        return Value(isModuleNamespaceExportKey(obj, prop));
      }
      return Value(obj->properties.find(prop) != obj->properties.end());
    }
    return Value(false);
  };
  reflectObj->properties["has"] = Value(reflectHas);

  // Reflect.deleteProperty(target, property)
  auto reflectDelete = GarbageCollector::makeGC<Function>();
  reflectDelete->isNative = true;
  reflectDelete->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);

    const Value& target = args[0];
    std::string prop = valueToPropertyKey(args[1]);

    if (target.isObject()) {
      auto obj = target.getGC<Object>();
      if (obj->isModuleNamespace) {
        if (prop == WellKnownSymbols::toStringTagKey()) {
          return Value(false);
        }
        return Value(!isModuleNamespaceExportKey(obj, prop));
      }
      return Value(obj->properties.erase(prop) > 0);
    }
    return Value(false);
  };
  reflectObj->properties["deleteProperty"] = Value(reflectDelete);

  // Reflect.apply(target, thisArg, argumentsList)
  auto reflectApply = GarbageCollector::makeGC<Function>();
  reflectApply->isNative = true;
  reflectApply->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3 || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto func = args[0].getGC<Function>();
    const Value& thisArg = args[1];

    std::vector<Value> callArgs;
    if (args[2].isArray()) {
      auto arr = args[2].getGC<Array>();
      callArgs = arr->elements;
    }

    if (func->isNative) {
      // For native functions, we can't pass 'this' directly
      // Native functions typically don't use 'this'
      return func->nativeFunc(callArgs);
    }
    // For non-native functions, we'd need interpreter access
    return Value(Undefined{});
  };
  reflectObj->properties["apply"] = Value(reflectApply);

  // Reflect.construct(target, argumentsList, newTarget?)
  auto reflectConstruct = GarbageCollector::makeGC<Function>();
  reflectConstruct->isNative = true;
  reflectConstruct->properties["__reflect_construct__"] = Value(true);
  reflectConstruct->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Reflect.construct target is not a function");
    }

    auto func = args[0].getGC<Function>();
    if (!func->isConstructor) {
      throw std::runtime_error("TypeError: target is not a constructor");
    }

    if (args.size() >= 3) {
      if (!args[2].isFunction()) {
        throw std::runtime_error("TypeError: newTarget is not a constructor");
      }
      auto newTarget = args[2].getGC<Function>();
      if (!newTarget->isConstructor) {
        throw std::runtime_error("TypeError: newTarget is not a constructor");
      }
    }

    std::vector<Value> callArgs;
    if (args[1].isArray()) {
      auto arr = args[1].getGC<Array>();
      callArgs = arr->elements;
    }

    if (func->isNative) {
      return func->nativeFunc(callArgs);
    }
    // For non-native functions, we'd need interpreter access
    return Value(Undefined{});
  };
  reflectObj->properties["construct"] = Value(reflectConstruct);

  // Reflect.getPrototypeOf(target) - delegates to Object.getPrototypeOf logic
  auto reflectGetPrototypeOf = GarbageCollector::makeGC<Function>();
  reflectGetPrototypeOf->isNative = true;
  reflectGetPrototypeOf->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Null{});
    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();
      auto protoIt = fn->properties.find("__proto__");
      if (protoIt != fn->properties.end()) return protoIt->second;
      if (auto funcCtor = env->get("Function"); funcCtor && funcCtor->isFunction()) {
        auto funcFn = std::get<GCPtr<Function>>(funcCtor->data);
        auto fpIt = funcFn->properties.find("prototype");
        if (fpIt != funcFn->properties.end()) return fpIt->second;
      }
    }
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      auto it = obj->properties.find("__proto__");
      if (it != obj->properties.end()) return it->second;
    }
    return Value(Null{});
  };
  reflectObj->properties["getPrototypeOf"] = Value(reflectGetPrototypeOf);

  // Reflect.setPrototypeOf(target, proto) - returns false (not supported)
  auto reflectSetPrototypeOf = GarbageCollector::makeGC<Function>();
  reflectSetPrototypeOf->isNative = true;
  reflectSetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // LightJS doesn't support dynamic prototype modification
    return Value(false);
  };
  reflectObj->properties["setPrototypeOf"] = Value(reflectSetPrototypeOf);

  // Reflect.isExtensible(target) - check if object can be extended
  auto reflectIsExtensible = GarbageCollector::makeGC<Function>();
  reflectIsExtensible->isNative = true;
  reflectIsExtensible->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      auto it = cls->properties.find("__non_extensible__");
      bool nonExtensible = it != cls->properties.end() &&
                           it->second.isBool() &&
                           it->second.toBool();
      return Value(!nonExtensible);
    }
    if (args[0].isFunction() || args[0].isArray()) {
      return Value(true);  // Functions and arrays are always extensible
    }
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        return Value(false);
      }
      return Value(!obj->sealed && !obj->frozen);
    }
    return Value(false);
  };
  reflectObj->properties["isExtensible"] = Value(reflectIsExtensible);

  // Reflect.preventExtensions(target) - prevent adding new properties
  auto reflectPreventExtensions = GarbageCollector::makeGC<Function>();
  reflectPreventExtensions->isNative = true;
  reflectPreventExtensions->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(false);
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      cls->properties["__non_extensible__"] = Value(true);
      return Value(true);
    }
    if (!args[0].isObject()) {
      return Value(false);
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      return Value(true);
    }
    obj->sealed = true;
    return Value(true);
  };
  reflectObj->properties["preventExtensions"] = Value(reflectPreventExtensions);

  // Reflect.ownKeys(target) - return array of own property keys
  auto reflectOwnKeys = GarbageCollector::makeGC<Function>();
  reflectOwnKeys->isNative = true;
  reflectOwnKeys->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();

    if (args.empty()) return Value(result);

    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        for (const auto& key : obj->moduleExportNames) {
          result->elements.push_back(Value(key));
        }
        result->elements.push_back(WellKnownSymbols::toStringTag());
        return Value(result);
      }
      for (const auto& [key, _] : obj->properties) {
        // Skip internal properties (__*__ and __get_/__set_/__non_enum_/etc.)
        if (key.size() >= 4 && key.substr(0, 2) == "__" &&
            key.substr(key.size() - 2) == "__") continue;
        if (key.size() >= 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) continue;
        if (key.size() >= 7 && key.substr(0, 7) == "__enum_") continue;
        if (key.size() >= 11 && key.substr(0, 11) == "__non_enum_") continue;
        if (key.size() >= 15 && key.substr(0, 15) == "__non_writable_") continue;
        if (key.size() >= 19 && key.substr(0, 19) == "__non_configurable_") continue;
        result->elements.push_back(Value(key));
      }
    } else if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      for (size_t i = 0; i < arr->elements.size(); ++i) {
        result->elements.push_back(Value(std::to_string(i)));
      }
      result->elements.push_back(Value("length"));
    }

    return Value(result);
  };
  reflectObj->properties["ownKeys"] = Value(reflectOwnKeys);

  // Reflect.getOwnPropertyDescriptor(target, propertyKey)
  auto reflectGetOwnPropertyDescriptor = GarbageCollector::makeGC<Function>();
  reflectGetOwnPropertyDescriptor->isNative = true;
  reflectGetOwnPropertyDescriptor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});

    std::string prop = valueToPropertyKey(args[1]);

    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        auto descriptor = GarbageCollector::makeGC<Object>();
        if (prop == WellKnownSymbols::toStringTagKey()) {
          descriptor->properties["value"] = Value(std::string("Module"));
          descriptor->properties["writable"] = Value(false);
          descriptor->properties["enumerable"] = Value(false);
          descriptor->properties["configurable"] = Value(false);
          return Value(descriptor);
        }
        if (!isModuleNamespaceExportKey(obj, prop)) {
          return Value(Undefined{});
        }
        auto getterIt = obj->properties.find("__get_" + prop);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, args[0]);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            descriptor->properties["value"] = out;
          } else {
            descriptor->properties["value"] = Value(Undefined{});
          }
        } else if (auto it = obj->properties.find(prop); it != obj->properties.end()) {
          descriptor->properties["value"] = it->second;
        } else {
          descriptor->properties["value"] = Value(Undefined{});
        }
        descriptor->properties["writable"] = Value(true);
        descriptor->properties["enumerable"] = Value(true);
        descriptor->properties["configurable"] = Value(false);
        return Value(descriptor);
      }
      auto it = obj->properties.find(prop);
      if (it == obj->properties.end()) {
        return Value(Undefined{});
      }
      // Create a simplified property descriptor
      auto descriptor = GarbageCollector::makeGC<Object>();
      descriptor->properties["value"] = it->second;
      descriptor->properties["writable"] = Value(!obj->frozen);
      descriptor->properties["enumerable"] = Value(true);
      descriptor->properties["configurable"] = Value(!obj->sealed);
      return Value(descriptor);
    }

    return Value(Undefined{});
  };
  reflectObj->properties["getOwnPropertyDescriptor"] = Value(reflectGetOwnPropertyDescriptor);

  // Reflect.defineProperty(target, propertyKey, attributes)
  auto reflectDefineProperty = GarbageCollector::makeGC<Function>();
  reflectDefineProperty->isNative = true;
  reflectDefineProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3) return Value(false);
    if (!args[0].isObject()) return Value(false);

    auto obj = args[0].getGC<Object>();
    std::string prop = valueToPropertyKey(args[1]);
    if (obj->isModuleNamespace) {
      if (!args[2].isObject()) {
        return Value(false);
      }
      auto descriptor = args[2].getGC<Object>();
      return Value(defineModuleNamespaceProperty(obj, prop, descriptor));
    }

    // Check if object is sealed/frozen
    bool isNewProp = obj->properties.find(prop) == obj->properties.end();
    if ((obj->sealed && isNewProp) || obj->frozen) {
      return Value(false);
    }

    // Get value from descriptor
    if (args[2].isObject()) {
      auto descriptor = args[2].getGC<Object>();
      auto valueIt = descriptor->properties.find("value");
      if (valueIt != descriptor->properties.end()) {
        obj->properties[prop] = valueIt->second;
        return Value(true);
      }
    }

    return Value(false);
  };
  reflectObj->properties["defineProperty"] = Value(reflectDefineProperty);

  env->define("Reflect", Value(reflectObj));

  // Number object with static methods
  auto numberObj = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  numberObj->isNative = true;
  numberObj->isConstructor = true;
  numberObj->properties["__wrap_primitive__"] = Value(true);
  numberObj->properties["name"] = Value(std::string("Number"));
  numberObj->properties["length"] = Value(1.0);
  numberObj->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(0.0);
    }
    Value primitive = toPrimitive(args[0], false);
    if (primitive.isSymbol()) {
      throw std::runtime_error("TypeError: Cannot convert Symbol to number");
    }
    return Value(primitive.toNumber());
  };

  // Number.parseInt
  auto parseIntFn = GarbageCollector::makeGC<Function>();
  parseIntFn->isNative = true;
  parseIntFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    std::string str = args[0].toString();
    int radix = args.size() > 1 ? static_cast<int>(args[1].toNumber()) : 10;

    if (radix != 0 && (radix < 2 || radix > 36)) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Trim whitespace
    size_t start = str.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
    str = str.substr(start);

    // Handle sign
    bool negative = false;
    if (!str.empty() && (str[0] == '+' || str[0] == '-')) {
      negative = (str[0] == '-');
      str = str.substr(1);
    }

    // Auto-detect radix if 0
    if (radix == 0) {
      if (str.size() >= 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        radix = 16;
        str = str.substr(2);
      } else {
        radix = 10;
      }
    }

    try {
      size_t idx;
      long long result = std::stoll(str, &idx, radix);
      if (idx == 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      return Value(static_cast<double>(negative ? -result : result));
    } catch (...) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
  };
  numberObj->properties["parseInt"] = Value(parseIntFn);

  // Number.parseFloat
  auto parseFloatFn = GarbageCollector::makeGC<Function>();
  parseFloatFn->isNative = true;
  parseFloatFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::numeric_limits<double>::quiet_NaN());
    std::string str = args[0].toString();

    // Trim leading whitespace (including Unicode whitespace per spec)
    size_t start = 0;
    while (start < str.size()) {
      unsigned char c = str[start];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
        start++;
      } else if (c == 0xC2 && start + 1 < str.size() && (unsigned char)str[start+1] == 0xA0) {
        start += 2;  // NBSP U+00A0 (UTF-8: C2 A0)
      } else if (c == 0xE2 && start + 2 < str.size()) {
        unsigned char c2 = str[start+1], c3 = str[start+2];
        if (c2 == 0x80 && (c3 >= 0x80 && c3 <= 0x8A)) { start += 3; }      // U+2000-U+200A
        else if (c2 == 0x80 && (c3 == 0xA8 || c3 == 0xA9 || c3 == 0xAF)) { start += 3; }  // U+2028/2029/202F
        else if (c2 == 0x81 && c3 == 0x9F) { start += 3; }                  // U+205F
        else break;
      } else if (c == 0xE3 && start + 2 < str.size() &&
                 (unsigned char)str[start+1] == 0x80 && (unsigned char)str[start+2] == 0x80) {
        start += 3;  // U+3000
      } else if (c == 0xEF && start + 2 < str.size() &&
                 (unsigned char)str[start+1] == 0xBB && (unsigned char)str[start+2] == 0xBF) {
        start += 3;  // U+FEFF
      } else {
        break;
      }
    }
    if (start >= str.size()) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
    str = str.substr(start);

    if (str.empty()) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }

    // Handle Infinity/+Infinity/-Infinity explicitly (case-sensitive per spec)
    if (str.substr(0, 8) == "Infinity" || str.substr(0, 9) == "+Infinity") {
      return Value(std::numeric_limits<double>::infinity());
    }
    if (str.substr(0, 9) == "-Infinity") {
      return Value(-std::numeric_limits<double>::infinity());
    }
    // Reject non-spec infinity variants (inf, INFINITY, etc.)
    if ((str[0] == 'i' || str[0] == 'I') && str.substr(0, 3) != "Inf") {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }

    try {
      size_t idx;
      double result = std::stod(str, &idx);
      if (idx == 0) {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      // std::stod may parse "inf"/"INF" etc - reject non-spec Infinity
      if (std::isinf(result) && str.substr(0, 8) != "Infinity" &&
          str.substr(0, 9) != "+Infinity" && str.substr(0, 9) != "-Infinity") {
        return Value(std::numeric_limits<double>::quiet_NaN());
      }
      return Value(result);
    } catch (...) {
      return Value(std::numeric_limits<double>::quiet_NaN());
    }
  };
  numberObj->properties["parseFloat"] = Value(parseFloatFn);

  // Number.isNaN
  auto isNaNFn = GarbageCollector::makeGC<Function>();
  isNaNFn->isNative = true;
  isNaNFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isnan(num));
  };
  isNaNFn->properties["name"] = Value(std::string("isNaN"));
  isNaNFn->properties["length"] = Value(1.0);
  numberObj->properties["isNaN"] = Value(isNaNFn);
  numberObj->properties["__non_enum_isNaN"] = Value(true);

  // Number.isFinite
  auto isFiniteFn = GarbageCollector::makeGC<Function>();
  isFiniteFn->isNative = true;
  isFiniteFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isfinite(num));
  };
  isFiniteFn->properties["name"] = Value(std::string("isFinite"));
  isFiniteFn->properties["length"] = Value(1.0);
  numberObj->properties["isFinite"] = Value(isFiniteFn);
  numberObj->properties["__non_enum_isFinite"] = Value(true);

  // Number.isInteger
  auto isIntegerFn = GarbageCollector::makeGC<Function>();
  isIntegerFn->isNative = true;
  isIntegerFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    return Value(std::isfinite(num) && std::floor(num) == num);
  };
  isIntegerFn->properties["name"] = Value(std::string("isInteger"));
  isIntegerFn->properties["length"] = Value(1.0);
  numberObj->properties["isInteger"] = Value(isIntegerFn);
  numberObj->properties["__non_enum_isInteger"] = Value(true);

  // Number.isSafeInteger
  auto isSafeIntegerFn = GarbageCollector::makeGC<Function>();
  isSafeIntegerFn->isNative = true;
  isSafeIntegerFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isNumber()) return Value(false);
    double num = std::get<double>(args[0].data);
    const double MAX_SAFE_INTEGER = 9007199254740991.0;
    return Value(std::isfinite(num) && std::floor(num) == num &&
                 num >= -MAX_SAFE_INTEGER && num <= MAX_SAFE_INTEGER);
  };
  isSafeIntegerFn->properties["name"] = Value(std::string("isSafeInteger"));
  isSafeIntegerFn->properties["length"] = Value(1.0);
  numberObj->properties["isSafeInteger"] = Value(isSafeIntegerFn);
  numberObj->properties["__non_enum_isSafeInteger"] = Value(true);

  // Number constants (non-writable, non-enumerable, non-configurable per spec)
  auto defineNumberConst = [&](const std::string& name, Value val) {
    numberObj->properties[name] = val;
    numberObj->properties["__non_writable_" + name] = Value(true);
    numberObj->properties["__non_enum_" + name] = Value(true);
    numberObj->properties["__non_configurable_" + name] = Value(true);
  };
  defineNumberConst("MAX_VALUE", Value(std::numeric_limits<double>::max()));
  defineNumberConst("MIN_VALUE", Value(std::numeric_limits<double>::denorm_min()));
  defineNumberConst("POSITIVE_INFINITY", Value(std::numeric_limits<double>::infinity()));
  defineNumberConst("NEGATIVE_INFINITY", Value(-std::numeric_limits<double>::infinity()));
  defineNumberConst("NaN", Value(std::numeric_limits<double>::quiet_NaN()));
  defineNumberConst("MAX_SAFE_INTEGER", Value(9007199254740991.0));
  defineNumberConst("MIN_SAFE_INTEGER", Value(-9007199254740991.0));
  defineNumberConst("EPSILON", Value(2.220446049250313e-16));

  {
    auto numberPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    numberObj->properties["prototype"] = Value(numberPrototype);
    numberPrototype->properties["constructor"] = Value(numberObj);
    numberPrototype->properties["name"] = Value(std::string("Number"));
  }

  env->define("Number", Value(numberObj));

  // Boolean constructor
  auto booleanObj = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  booleanObj->isNative = true;
  booleanObj->isConstructor = true;
  booleanObj->properties["__wrap_primitive__"] = Value(true);
  booleanObj->properties["name"] = Value(std::string("Boolean"));
  booleanObj->properties["length"] = Value(1.0);
  booleanObj->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(false);
    }
    Value primitive = toPrimitive(args[0], false);
    return Value(primitive.toBool());
  };

  {
    auto booleanPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    booleanObj->properties["prototype"] = Value(booleanPrototype);
    booleanPrototype->properties["constructor"] = Value(booleanObj);
    booleanPrototype->properties["name"] = Value(std::string("Boolean"));

    auto boolProtoValueOf = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    boolProtoValueOf->isNative = true;
    boolProtoValueOf->properties["__uses_this_arg__"] = Value(true);
    boolProtoValueOf->properties["name"] = Value(std::string("valueOf"));
    boolProtoValueOf->properties["length"] = Value(0.0);
    boolProtoValueOf->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Boolean.prototype.valueOf requires Boolean");
      }
      Value thisArg = callArgs[0];
      if (thisArg.isBool()) return thisArg;
      if (thisArg.isObject()) {
        auto obj = thisArg.getGC<Object>();
        auto primIt = obj->properties.find("__primitive_value__");
        if (primIt != obj->properties.end() && primIt->second.isBool()) {
          return primIt->second;
        }
      }
      throw std::runtime_error("TypeError: Boolean.prototype.valueOf requires Boolean");
    };
    booleanPrototype->properties["valueOf"] = Value(boolProtoValueOf);
    booleanPrototype->properties["__non_enum_valueOf"] = Value(true);

    auto boolProtoToString = GarbageCollector::makeGC<Function>();
    GarbageCollector::instance().reportAllocation(sizeof(Function));
    boolProtoToString->isNative = true;
    boolProtoToString->properties["__uses_this_arg__"] = Value(true);
    boolProtoToString->properties["name"] = Value(std::string("toString"));
    boolProtoToString->properties["length"] = Value(0.0);
    boolProtoToString->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
      if (callArgs.empty()) {
        throw std::runtime_error("TypeError: Boolean.prototype.toString requires Boolean");
      }
      Value thisArg = callArgs[0];
      bool b = false;
      if (thisArg.isBool()) {
        b = thisArg.toBool();
      } else if (thisArg.isObject()) {
        auto obj = thisArg.getGC<Object>();
        auto primIt = obj->properties.find("__primitive_value__");
        if (primIt != obj->properties.end() && primIt->second.isBool()) {
          b = primIt->second.toBool();
        } else {
          throw std::runtime_error("TypeError: Boolean.prototype.toString requires Boolean");
        }
      } else {
        throw std::runtime_error("TypeError: Boolean.prototype.toString requires Boolean");
      }
      return Value(std::string(b ? "true" : "false"));
    };
    booleanPrototype->properties["toString"] = Value(boolProtoToString);
    booleanPrototype->properties["__non_enum_toString"] = Value(true);
  }
  env->define("Boolean", Value(booleanObj));

  // Global parseInt and parseFloat (aliases)
  env->define("parseInt", Value(parseIntFn));
  env->define("parseFloat", Value(parseFloatFn));

  // Global isNaN (different from Number.isNaN - coerces to number first)
  auto globalIsNaNFn = GarbageCollector::makeGC<Function>();
  globalIsNaNFn->isNative = true;
  globalIsNaNFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(true);
    double num = args[0].toNumber();
    return Value(std::isnan(num));
  };
  env->define("isNaN", Value(globalIsNaNFn));

  // Global isFinite
  auto globalIsFiniteFn = GarbageCollector::makeGC<Function>();
  globalIsFiniteFn->isNative = true;
  globalIsFiniteFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    double num = args[0].toNumber();
    return Value(std::isfinite(num));
  };
  env->define("isFinite", Value(globalIsFiniteFn));

  // Array constructor with static methods
  auto arrayConstructorFn = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  arrayConstructorFn->isNative = true;
  arrayConstructorFn->isConstructor = true;
  arrayConstructorFn->properties["name"] = Value(std::string("Array"));
  arrayConstructorFn->properties["length"] = Value(1.0);
  arrayConstructorFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();
    GarbageCollector::instance().reportAllocation(sizeof(Array));

    if (args.empty()) {
      return Value(result);
    }

    if (args.size() == 1 && args[0].isNumber()) {
      double lengthNum = args[0].toNumber();
      if (!std::isfinite(lengthNum) || lengthNum < 0 || std::floor(lengthNum) != lengthNum) {
        throw std::runtime_error("RangeError: Invalid array length");
      }
      result->elements.resize(static_cast<size_t>(lengthNum), Value(Undefined{}));
      return Value(result);
    }

    result->elements = args;
    return Value(result);
  };

  auto arrayConstructorObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  arrayConstructorObj->properties["__callable_object__"] = Value(true);
  arrayConstructorObj->properties["__constructor_wrapper__"] = Value(true);
  arrayConstructorObj->properties["constructor"] = Value(arrayConstructorFn);

  auto arrayPrototype = GarbageCollector::makeGC<Object>();
  arrayConstructorObj->properties["prototype"] = Value(arrayPrototype);
  arrayConstructorFn->properties["prototype"] = Value(arrayPrototype);
  arrayPrototype->properties["constructor"] = Value(arrayConstructorObj);
  env->define("__array_prototype__", Value(arrayPrototype));

  // Array.prototype.push - receives this (array) via __uses_this_arg__
  auto arrayProtoPush = GarbageCollector::makeGC<Function>();
  arrayProtoPush->isNative = true;
  arrayProtoPush->properties["__uses_this_arg__"] = Value(true);
  arrayProtoPush->properties["name"] = Value(std::string("push"));
  arrayProtoPush->properties["length"] = Value(1.0);
  arrayProtoPush->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.push called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    for (size_t i = 1; i < args.size(); ++i) {
      arr->elements.push_back(args[i]);
    }
    return Value(static_cast<double>(arr->elements.size()));
  };
  arrayPrototype->properties["push"] = Value(arrayProtoPush);

  // Array.prototype.join
  auto arrayProtoJoin = GarbageCollector::makeGC<Function>();
  arrayProtoJoin->isNative = true;
  arrayProtoJoin->properties["__uses_this_arg__"] = Value(true);
  arrayProtoJoin->properties["name"] = Value(std::string("join"));
  arrayProtoJoin->properties["length"] = Value(1.0);
  arrayProtoJoin->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.join called on non-array");
    }
    auto arr = args[0].getGC<Array>();
    std::string separator = args.size() > 1 && !args[1].isUndefined() ? args[1].toString() : ",";
    std::string result;
    for (size_t i = 0; i < arr->elements.size(); ++i) {
      if (i > 0) result += separator;
      if (!arr->elements[i].isUndefined() && !arr->elements[i].isNull()) {
        result += arr->elements[i].toString();
      }
    }
    return Value(result);
  };
  arrayPrototype->properties["join"] = Value(arrayProtoJoin);

  // Array.prototype.reduce
  auto arrayProtoReduce = GarbageCollector::makeGC<Function>();
  arrayProtoReduce->isNative = true;
  arrayProtoReduce->properties["__uses_this_arg__"] = Value(true);
  arrayProtoReduce->properties["name"] = Value(std::string("reduce"));
  arrayProtoReduce->properties["length"] = Value(1.0);
  arrayProtoReduce->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      throw std::runtime_error("TypeError: Array.prototype.reduce called on non-array");
    }
    if (args.size() < 2 || !args[1].isFunction()) {
      throw std::runtime_error("TypeError: Array.prototype.reduce requires a callback function");
    }
    auto arr = args[0].getGC<Array>();
    Value callback = args[1];

    if (!arr || arr->elements.empty()) {
      if (args.size() >= 3) {
        return args[2];
      }
      throw std::runtime_error("TypeError: Reduce of empty array with no initial value");
    }

    Value accumulator(Undefined{});
    size_t start = 0;
    if (args.size() >= 3) {
      accumulator = args[2];
      start = 0;
    } else {
      accumulator = arr->elements[0];
      start = 1;
    }

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    for (size_t i = start; i < arr->elements.size(); ++i) {
      std::vector<Value> callArgs = {
        accumulator,
        arr->elements[i],
        Value(static_cast<double>(i)),
        args[0]
      };
      accumulator = interpreter->callForHarness(callback, callArgs, Value(Undefined{}));
      if (interpreter->hasError()) {
        Value err = interpreter->getError();
        interpreter->clearError();
        throw std::runtime_error(err.toString());
      }
    }
    return accumulator;
  };
  arrayPrototype->properties["reduce"] = Value(arrayProtoReduce);
  arrayPrototype->properties["__non_enum_reduce"] = Value(true);

  // Array.prototype[Symbol.iterator] - values iterator
  {
    const auto& iterKey = WellKnownSymbols::iteratorKey();
    auto arrayProtoIterator = GarbageCollector::makeGC<Function>();
    arrayProtoIterator->isNative = true;
    arrayProtoIterator->properties["__uses_this_arg__"] = Value(true);
    arrayProtoIterator->properties["__builtin_array_iterator__"] = Value(true);
    arrayProtoIterator->nativeFunc = [](const std::vector<Value>& args) -> Value {
      if (args.empty() || !args[0].isArray()) {
        return Value(Undefined{});
      }
      auto arr = args[0].getGC<Array>();
      auto iterObj = GarbageCollector::makeGC<Object>();
      auto indexPtr = std::make_shared<size_t>(0);
      auto nextFn = GarbageCollector::makeGC<Function>();
      nextFn->isNative = true;
      nextFn->nativeFunc = [arr, indexPtr](const std::vector<Value>&) -> Value {
        auto result = GarbageCollector::makeGC<Object>();
        if (*indexPtr >= arr->elements.size()) {
          result->properties["value"] = Value(Undefined{});
          result->properties["done"] = Value(true);
        } else {
          result->properties["value"] = arr->elements[*indexPtr];
          result->properties["done"] = Value(false);
          (*indexPtr)++;
        }
        return Value(result);
      };
      iterObj->properties["next"] = Value(nextFn);
      return Value(iterObj);
    };
    arrayPrototype->properties[iterKey] = Value(arrayProtoIterator);
  }

  // Array.isArray
  auto isArrayFn = GarbageCollector::makeGC<Function>();
  isArrayFn->isNative = true;
  isArrayFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    return Value(args[0].isArray());
  };
  arrayConstructorObj->properties["isArray"] = Value(isArrayFn);

  // Array.from - creates array from array-like or iterable object
  auto fromFn = GarbageCollector::makeGC<Function>();
  fromFn->isNative = true;
  fromFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();

    if (args.empty()) {
      return Value(result);
    }

    const Value& arrayLike = args[0];
    Value mapFn = args.size() > 1 ? args[1] : Value(Undefined{});
    Value thisArg = args.size() > 2 ? args[2] : Value(Undefined{});
    bool hasMapFn = !mapFn.isUndefined();

    if (hasMapFn && !mapFn.isFunction()) {
      throw std::runtime_error("TypeError: Array.from mapper must be a function");
    }

    auto applyMap = [&](const Value& value, size_t index) -> Value {
      if (!hasMapFn) {
        return value;
      }

      std::vector<Value> mapperArgs = {value, Value(static_cast<double>(index))};
      Interpreter* interpreter = getGlobalInterpreter();
      if (interpreter) {
        Value mapped = interpreter->callForHarness(mapFn, mapperArgs, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw std::runtime_error(err.toString());
        }
        return mapped;
      }

      auto mapper = mapFn.getGC<Function>();
      if (mapper->isNative) {
        auto itUsesThis = mapper->properties.find("__uses_this_arg__");
        if (itUsesThis != mapper->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.reserve(mapperArgs.size() + 1);
          nativeArgs.push_back(thisArg);
          nativeArgs.insert(nativeArgs.end(), mapperArgs.begin(), mapperArgs.end());
          return mapper->nativeFunc(nativeArgs);
        }
        return mapper->nativeFunc(mapperArgs);
      }

      return value;
    };

    // If it's already an array, copy it
    if (arrayLike.isArray()) {
      auto srcArray = arrayLike.getGC<Array>();
      size_t index = 0;
      for (const auto& elem : srcArray->elements) {
        result->elements.push_back(applyMap(elem, index++));
      }
      return Value(result);
    }

    // If it's a string, convert each character to array element
    if (arrayLike.isString()) {
      std::string str = std::get<std::string>(arrayLike.data);
      size_t index = 0;
      for (char c : str) {
        result->elements.push_back(applyMap(Value(std::string(1, c)), index++));
      }
      return Value(result);
    }

    // If it's an iterator object, consume it
    if (arrayLike.isObject()) {
      Value iteratorValue = arrayLike;
      auto srcObj = arrayLike.getGC<Object>();
      const auto& iteratorKey = WellKnownSymbols::iteratorKey();

      auto iteratorMethodIt = srcObj->properties.find(iteratorKey);
      if (iteratorMethodIt != srcObj->properties.end() && iteratorMethodIt->second.isFunction()) {
        Interpreter* interpreter = getGlobalInterpreter();
        if (interpreter) {
          iteratorValue = interpreter->callForHarness(iteratorMethodIt->second, {}, arrayLike);
          if (interpreter->hasError()) {
            Value err = interpreter->getError();
            interpreter->clearError();
            throw std::runtime_error(err.toString());
          }
        } else {
          auto iterMethod = iteratorMethodIt->second.getGC<Function>();
          iteratorValue = iterMethod->isNative ? iterMethod->nativeFunc({}) : Value(Undefined{});
        }
      }

      if (iteratorValue.isObject()) {
        auto iterObj = iteratorValue.getGC<Object>();
        auto nextIt = iterObj->properties.find("next");
        if (nextIt != iterObj->properties.end() && nextIt->second.isFunction()) {
          size_t index = 0;
          while (true) {
            Value stepResult;
            Interpreter* interpreter = getGlobalInterpreter();
            if (interpreter) {
              stepResult = interpreter->callForHarness(nextIt->second, {}, iteratorValue);
              if (interpreter->hasError()) {
                Value err = interpreter->getError();
                interpreter->clearError();
                throw std::runtime_error(err.toString());
              }
            } else {
              auto nextFn = nextIt->second.getGC<Function>();
              stepResult = nextFn->isNative ? nextFn->nativeFunc({}) : Value(Undefined{});
            }

            if (!stepResult.isObject()) break;
            auto stepObj = stepResult.getGC<Object>();
            bool done = false;
            if (auto doneIt = stepObj->properties.find("done"); doneIt != stepObj->properties.end()) {
              done = doneIt->second.toBool();
            }
            if (done) break;

            Value element = Value(Undefined{});
            if (auto valueIt = stepObj->properties.find("value"); valueIt != stepObj->properties.end()) {
              element = valueIt->second;
            }
            result->elements.push_back(applyMap(element, index++));
          }
          return Value(result);
        }
      }
    }

    // Otherwise return empty array
    return Value(result);
  };
  arrayConstructorObj->properties["from"] = Value(fromFn);

  // Array.of - creates array from arguments
  auto ofFn = GarbageCollector::makeGC<Function>();
  ofFn->isNative = true;
  ofFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();
    result->elements = args;
    return Value(result);
  };
  arrayConstructorObj->properties["of"] = Value(ofFn);

  env->define("Array", Value(arrayConstructorObj));

  // Promise constructor
  auto promiseFunc = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  promiseFunc->isNative = true;
  promiseFunc->isConstructor = true;
  promiseFunc->properties["name"] = Value(std::string("Promise"));
  promiseFunc->properties["length"] = Value(1.0);

  auto promiseConstructor = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto promisePrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  promiseConstructor->properties["prototype"] = Value(promisePrototype);
  promisePrototype->properties["constructor"] = Value(promiseFunc);

  auto invokePromiseCallback = [](const GCPtr<Function>& callback, const Value& arg) -> Value {
    if (!callback) {
      return arg;
    }
    if (callback->isNative) {
      return callback->nativeFunc({arg});
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value out = interpreter->callForHarness(Value(callback), {arg}, Value(Undefined{}));
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      throw std::runtime_error(err.toString());
    }
    return out;
  };

  auto promiseProtoThen = GarbageCollector::makeGC<Function>();
  promiseProtoThen->isNative = true;
  promiseProtoThen->properties["__uses_this_arg__"] = Value(true);
  promiseProtoThen->properties["name"] = Value(std::string("then"));
  promiseProtoThen->properties["length"] = Value(2.0);
  promiseProtoThen->nativeFunc = [invokePromiseCallback](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isPromise()) {
      throw std::runtime_error("TypeError: Promise.prototype.then called on non-Promise");
    }

    auto promise = args[0].getGC<Promise>();
    std::function<Value(Value)> onFulfilled = nullptr;
    std::function<Value(Value)> onRejected = nullptr;

    if (args.size() > 1 && args[1].isFunction()) {
      auto callback = args[1].getGC<Function>();
      onFulfilled = [callback, invokePromiseCallback](Value v) -> Value {
        return invokePromiseCallback(callback, v);
      };
    }
    if (args.size() > 2 && args[2].isFunction()) {
      auto callback = args[2].getGC<Function>();
      onRejected = [callback, invokePromiseCallback](Value v) -> Value {
        return invokePromiseCallback(callback, v);
      };
    }

    auto chained = promise->then(onFulfilled, onRejected);
    auto ctorIt = promise->properties.find("__constructor__");
    if (ctorIt != promise->properties.end()) {
      chained->properties["__constructor__"] = ctorIt->second;
    }
    return Value(chained);
  };
  promisePrototype->properties["then"] = Value(promiseProtoThen);
  promisePrototype->properties["__non_enum_then"] = Value(true);

  auto promiseProtoCatch = GarbageCollector::makeGC<Function>();
  promiseProtoCatch->isNative = true;
  promiseProtoCatch->properties["__uses_this_arg__"] = Value(true);
  promiseProtoCatch->properties["name"] = Value(std::string("catch"));
  promiseProtoCatch->properties["length"] = Value(1.0);
  promiseProtoCatch->nativeFunc = [invokePromiseCallback](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isPromise()) {
      throw std::runtime_error("TypeError: Promise.prototype.catch called on non-Promise");
    }

    auto promise = args[0].getGC<Promise>();
    std::function<Value(Value)> onRejected = nullptr;
    if (args.size() > 1 && args[1].isFunction()) {
      auto callback = args[1].getGC<Function>();
      onRejected = [callback, invokePromiseCallback](Value v) -> Value {
        return invokePromiseCallback(callback, v);
      };
    }

    auto chained = promise->catch_(onRejected);
    auto ctorIt = promise->properties.find("__constructor__");
    if (ctorIt != promise->properties.end()) {
      chained->properties["__constructor__"] = ctorIt->second;
    }
    return Value(chained);
  };
  promisePrototype->properties["catch"] = Value(promiseProtoCatch);
  promisePrototype->properties["__non_enum_catch"] = Value(true);

  auto promiseProtoFinally = GarbageCollector::makeGC<Function>();
  promiseProtoFinally->isNative = true;
  promiseProtoFinally->properties["__uses_this_arg__"] = Value(true);
  promiseProtoFinally->properties["name"] = Value(std::string("finally"));
  promiseProtoFinally->properties["length"] = Value(1.0);
  promiseProtoFinally->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isPromise()) {
      throw std::runtime_error("TypeError: Promise.prototype.finally called on non-Promise");
    }

    auto promise = args[0].getGC<Promise>();
    std::function<Value()> onFinally = nullptr;
    if (args.size() > 1 && args[1].isFunction()) {
      auto callback = args[1].getGC<Function>();
      onFinally = [callback]() -> Value {
        if (callback->isNative) {
          return callback->nativeFunc({});
        }
        Interpreter* interpreter = getGlobalInterpreter();
        if (!interpreter) {
          throw std::runtime_error("TypeError: Interpreter unavailable");
        }
        interpreter->clearError();
        Value out = interpreter->callForHarness(Value(callback), {}, Value(Undefined{}));
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw std::runtime_error(err.toString());
        }
        return out;
      };
    }

    auto chained = promise->finally(onFinally);
    auto ctorIt = promise->properties.find("__constructor__");
    if (ctorIt != promise->properties.end()) {
      chained->properties["__constructor__"] = ctorIt->second;
    }
    return Value(chained);
  };
  promisePrototype->properties["finally"] = Value(promiseProtoFinally);
  promisePrototype->properties["__non_enum_finally"] = Value(true);

  // Promise.resolve
  auto promiseResolve = GarbageCollector::makeGC<Function>();
  promiseResolve->isNative = true;
  promiseResolve->properties["__uses_this_arg__"] = Value(true);
  promiseResolve->properties["name"] = Value(std::string("resolve"));
  promiseResolve->properties["length"] = Value(1.0);
  promiseResolve->nativeFunc = [callChecked, getProperty, env, promiseFunc](const std::vector<Value>& args) -> Value {
    Value resolution = args.size() > 1 ? args[1] : Value(Undefined{});
    if (resolution.isPromise()) {
      return resolution;
    }

    auto promise = GarbageCollector::makeGC<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);

    auto getThenProperty = [&](const Value& candidate) -> std::pair<bool, Value> {
      if (candidate.isArray()) {
        auto arr = candidate.getGC<Array>();
        auto getterIt = arr->properties.find("__get_then");
        if (getterIt != arr->properties.end()) {
          if (getterIt->second.isFunction()) {
            return {true, callChecked(getterIt->second, {}, candidate)};
          }
          return {true, Value(Undefined{})};
        }
        auto ownIt = arr->properties.find("then");
        if (ownIt != arr->properties.end()) {
          return {true, ownIt->second};
        }
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            auto protoObj = protoIt->second.getGC<Object>();
            auto protoGetterIt = protoObj->properties.find("__get_then");
            if (protoGetterIt != protoObj->properties.end()) {
              if (protoGetterIt->second.isFunction()) {
                return {true, callChecked(protoGetterIt->second, {}, candidate)};
              }
              return {true, Value(Undefined{})};
            }
            auto protoThenIt = protoObj->properties.find("then");
            if (protoThenIt != protoObj->properties.end()) {
              return {true, protoThenIt->second};
            }
          }
        }
        return {false, Value(Undefined{})};
      }

      return getProperty(candidate, "then");
    };

    auto resolveSelf = std::make_shared<std::function<void(const Value&)>>();
    *resolveSelf = [promise, resolveSelf, getThenProperty, callChecked](const Value& value) {
      if (promise->state != PromiseState::Pending) {
        return;
      }

      if (value.isPromise()) {
        auto nested = value.getGC<Promise>();
        if (nested.get() == promise) {
          promise->reject(Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Cannot resolve promise with itself")));
          return;
        }
        if (nested->state == PromiseState::Fulfilled) {
          (*resolveSelf)(nested->result);
          return;
        }
        if (nested->state == PromiseState::Rejected) {
          promise->reject(nested->result);
          return;
        }
        nested->then(
          [resolveSelf](Value fulfilled) -> Value {
            (*resolveSelf)(fulfilled);
            return fulfilled;
          },
          [promise](Value reason) -> Value {
            promise->reject(reason);
            return reason;
          });
        return;
      }

      if (value.isObject() || value.isArray() || value.isFunction() || value.isRegex() || value.isProxy()) {
        try {
          auto [foundThen, thenValue] = getThenProperty(value);
          if (foundThen && thenValue.isFunction()) {
            auto alreadyCalled = std::make_shared<bool>(false);
            auto resolveFn = GarbageCollector::makeGC<Function>();
            resolveFn->isNative = true;
            resolveFn->nativeFunc = [resolveSelf, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
              if (*alreadyCalled) {
                return Value(Undefined{});
              }
              *alreadyCalled = true;
              Value next = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
              (*resolveSelf)(next);
              return Value(Undefined{});
            };

            auto rejectFn = GarbageCollector::makeGC<Function>();
            rejectFn->isNative = true;
            rejectFn->nativeFunc = [promise, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
              if (*alreadyCalled) {
                return Value(Undefined{});
              }
              *alreadyCalled = true;
              Value reason = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
              promise->reject(reason);
              return Value(Undefined{});
            };

            callChecked(thenValue, {Value(resolveFn), Value(rejectFn)}, value);
            return;
          }
        } catch (const std::exception& e) {
          promise->reject(Value(std::string(e.what())));
          return;
        }
      }

      promise->resolve(value);
    };

    (*resolveSelf)(resolution);
    return Value(promise);
  };
  promiseConstructor->properties["resolve"] = Value(promiseResolve);

  // Promise.reject
  auto promiseReject = GarbageCollector::makeGC<Function>();
  promiseReject->isNative = true;
  promiseReject->properties["name"] = Value(std::string("reject"));
  promiseReject->properties["length"] = Value(1.0);
  promiseReject->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    auto promise = GarbageCollector::makeGC<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);
    if (!args.empty()) {
      promise->reject(args[0]);
    } else {
      promise->reject(Value(Undefined{}));
    }
    return Value(promise);
  };
  promiseConstructor->properties["reject"] = Value(promiseReject);

  // Promise.withResolvers
  auto promiseWithResolvers = GarbageCollector::makeGC<Function>();
  promiseWithResolvers->isNative = true;
  promiseWithResolvers->nativeFunc = [promiseFunc](const std::vector<Value>&) -> Value {
    auto promise = GarbageCollector::makeGC<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);

    auto resolveFunc = GarbageCollector::makeGC<Function>();
    resolveFunc->isNative = true;
    auto promisePtr = promise;
    resolveFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        promisePtr->resolve(args[0]);
      } else {
        promisePtr->resolve(Value(Undefined{}));
      }
      return Value(Undefined{});
    };

    auto rejectFunc = GarbageCollector::makeGC<Function>();
    rejectFunc->isNative = true;
    rejectFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        promisePtr->reject(args[0]);
      } else {
        promisePtr->reject(Value(Undefined{}));
      }
      return Value(Undefined{});
    };

    auto result = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    result->properties["promise"] = Value(promise);
    result->properties["resolve"] = Value(resolveFunc);
    result->properties["reject"] = Value(rejectFunc);
    return Value(result);
  };
  promiseConstructor->properties["withResolvers"] = Value(promiseWithResolvers);

  // Promise.all
  auto promiseAll = GarbageCollector::makeGC<Function>();
  promiseAll->isNative = true;
  promiseAll->properties["name"] = Value(std::string("all"));
  promiseAll->properties["length"] = Value(1.0);
  promiseAll->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = GarbageCollector::makeGC<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->properties["__constructor__"] = Value(promiseFunc);
      promise->reject(Value(std::string("Promise.all expects an array")));
      return Value(promise);
    }

    auto arr = args[0].getGC<Array>();
    auto resultPromise = GarbageCollector::makeGC<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    resultPromise->properties["__constructor__"] = Value(promiseFunc);
    auto results = GarbageCollector::makeGC<Array>();

    bool hasRejection = false;
    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = elem.getGC<Promise>();
        if (p->state == PromiseState::Rejected) {
          hasRejection = true;
          resultPromise->reject(p->result);
          break;
        } else if (p->state == PromiseState::Fulfilled) {
          results->elements.push_back(p->result);
        }
      } else {
        results->elements.push_back(elem);
      }
    }

    if (!hasRejection) {
      resultPromise->resolve(Value(results));
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["all"] = Value(promiseAll);

  // Promise.allSettled - waits for all promises to settle (resolve or reject)
  auto promiseAllSettled = GarbageCollector::makeGC<Function>();
  promiseAllSettled->isNative = true;
  promiseAllSettled->properties["__uses_this_arg__"] = Value(true);
  promiseAllSettled->properties["name"] = Value(std::string("allSettled"));
  promiseAllSettled->properties["length"] = Value(1.0);
  promiseAllSettled->nativeFunc = [callChecked, getProperty, env, promiseFunc](const std::vector<Value>& args) -> Value {
    Value constructor = args.empty() ? Value(Undefined{}) : args[0];
    Value iterable = args.size() > 1 ? args[1] : Value(Undefined{});

    auto typeErrorValue = [](const std::string& msg) -> Value {
      return Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, msg));
    };

    auto callWithThis = [](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg,
                           Value& out,
                           Value& thrown) -> bool {
      if (!callee.isFunction()) {
        thrown = Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Value is not callable"));
        return false;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        try {
          auto itUsesThis = fn->properties.find("__uses_this_arg__");
          if (itUsesThis != fn->properties.end() &&
              itUsesThis->second.isBool() &&
              itUsesThis->second.toBool()) {
            std::vector<Value> nativeArgs;
            nativeArgs.reserve(callArgs.size() + 1);
            nativeArgs.push_back(thisArg);
            nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
            out = fn->nativeFunc(nativeArgs);
          } else {
            out = fn->nativeFunc(callArgs);
          }
          return true;
        } catch (const std::exception& e) {
          thrown = Value(std::string(e.what()));
          return false;
        }
      }

      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        thrown = Value(std::string("TypeError: Interpreter unavailable"));
        return false;
      }
      interpreter->clearError();
      out = interpreter->callForHarness(callee, callArgs, thisArg);
      if (interpreter->hasError()) {
        thrown = interpreter->getError();
        interpreter->clearError();
        return false;
      }
      return true;
    };

    // === NewPromiseCapability(C) ===
    // Validate constructor is callable/constructable
    if (!constructor.isFunction() && !constructor.isClass()) {
      throw std::runtime_error("TypeError: Promise.allSettled called on non-constructor");
    }

    auto capResolve = std::make_shared<Value>(Value(Undefined{}));
    auto capReject = std::make_shared<Value>(Value(Undefined{}));

    auto executor = GarbageCollector::makeGC<Function>();
    executor->isNative = true;
    executor->isConstructor = false;
    executor->nativeFunc = [capResolve, capReject](const std::vector<Value>& executorArgs) -> Value {
      // GetCapabilitiesExecutor: spec steps 4-7
      if (!capResolve->isUndefined()) {
        throw std::runtime_error("TypeError: Promise resolve function already set");
      }
      if (!capReject->isUndefined()) {
        throw std::runtime_error("TypeError: Promise reject function already set");
      }
      *capResolve = executorArgs.size() > 0 ? executorArgs[0] : Value(Undefined{});
      *capReject = executorArgs.size() > 1 ? executorArgs[1] : Value(Undefined{});
      return Value(Undefined{});
    };

    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    interpreter->clearError();
    Value resultPromiseValue = interpreter->constructFromNative(constructor, {Value(executor)});
    if (interpreter->hasError()) {
      Value err = interpreter->getError();
      interpreter->clearError();
      if (err.isError()) {
        auto errPtr = err.getGC<Error>();
        throw std::runtime_error(errPtr->message);
      }
      throw std::runtime_error("TypeError: Failed to construct promise");
    }

    // Validate resolve and reject are callable
    if (!capResolve->isFunction() || !capReject->isFunction()) {
      throw std::runtime_error("TypeError: Promise resolve or reject function is not callable");
    }

    auto alreadyResolved = std::make_shared<bool>(false);
    auto resultPromiseVal = std::make_shared<Value>(resultPromiseValue);

    // Helper to reject the capability and return the result promise
    auto rejectCapability = [&callWithThis, capReject](const Value& reason) {
      Value out, thrown;
      callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
    };

    auto rejectAndReturn = [&rejectCapability, &resultPromiseValue](const Value& reason) -> Value {
      rejectCapability(reason);
      return resultPromiseValue;
    };

    auto getPropertyWithThrow = [&callWithThis, &env](const Value& receiver,
                                    const std::string& key,
                                    Value& out,
                                    bool& found,
                                    Value& thrown) -> bool {
      auto resolveFromObject = [&](const GCPtr<Object>& obj,
                                   const Value& originalReceiver) -> bool {
        auto current = obj;
        int depth = 0;
        while (current && depth <= 32) {
          depth++;

          auto getterIt = current->properties.find("__get_" + key);
          if (getterIt != current->properties.end()) {
            found = true;
            if (!getterIt->second.isFunction()) {
              out = Value(Undefined{});
              return true;
            }
            Value getterOut;
            if (!callWithThis(getterIt->second, {}, originalReceiver, getterOut, thrown)) {
              return false;
            }
            out = getterOut;
            return true;
          }

          auto it = current->properties.find(key);
          if (it != current->properties.end()) {
            found = true;
            out = it->second;
            return true;
          }

          auto protoIt = current->properties.find("__proto__");
          if (protoIt == current->properties.end() || !protoIt->second.isObject()) {
            break;
          }
          current = protoIt->second.getGC<Object>();
        }
        return true;
      };

      found = false;
      out = Value(Undefined{});

      if (receiver.isObject()) {
        return resolveFromObject(receiver.getGC<Object>(), receiver);
      }
      if (receiver.isFunction()) {
        auto fn = receiver.getGC<Function>();
        auto getterIt = fn->properties.find("__get_" + key);
        if (getterIt != fn->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = fn->properties.find(key);
        if (it != fn->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }
      if (receiver.isArray()) {
        auto arr = receiver.getGC<Array>();
        auto getterIt = arr->properties.find("__get_" + key);
        if (getterIt != arr->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = arr->properties.find(key);
        if (it != arr->properties.end()) {
          found = true;
          out = it->second;
          return true;
        }
        if (key == "length") {
          found = true;
          out = Value(static_cast<double>(arr->elements.size()));
          return true;
        }
        if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
          auto arrayObjPtr = std::get<GCPtr<Object>>(arrayCtor->data);
          auto protoIt = arrayObjPtr->properties.find("prototype");
          if (protoIt != arrayObjPtr->properties.end() && protoIt->second.isObject()) {
            return resolveFromObject(protoIt->second.getGC<Object>(), receiver);
          }
        }
        return true;
      }
      if (receiver.isClass()) {
        auto cls = receiver.getGC<Class>();
        auto getterIt = cls->properties.find("__get_" + key);
        if (getterIt != cls->properties.end()) {
          found = true;
          if (!getterIt->second.isFunction()) {
            out = Value(Undefined{});
            return true;
          }
          return callWithThis(getterIt->second, {}, receiver, out, thrown);
        }
        auto it = cls->properties.find(key);
        if (it != cls->properties.end()) {
          found = true;
          out = it->second;
        }
        return true;
      }

      return true;
    };

    Value promiseResolveMethod = Value(Undefined{});
    bool hasResolve = false;
    Value resolveThrown;
    if (!getPropertyWithThrow(constructor, "resolve", promiseResolveMethod, hasResolve, resolveThrown)) {
      return rejectAndReturn(resolveThrown);
    }
    if (!hasResolve || !promiseResolveMethod.isFunction()) {
      return rejectAndReturn(typeErrorValue("Promise.resolve is not callable"));
    }

    auto results = GarbageCollector::makeGC<Array>();
    auto remaining = std::make_shared<size_t>(1);

    auto resolveResultPromise = std::make_shared<std::function<void(const Value&)>>();
    *resolveResultPromise = [alreadyResolved, capResolve, capReject, resultPromiseVal,
                             resolveResultPromise, getPropertyWithThrow, callWithThis](const Value& finalValue) {
      if (*alreadyResolved) {
        return;
      }

      if (finalValue.isPromise()) {
        auto nested = finalValue.getGC<Promise>();
        // Check self-reference
        if (resultPromiseVal->isPromise() &&
            nested.get() == std::get<GCPtr<Promise>>(resultPromiseVal->data).get()) {
          *alreadyResolved = true;
          Value out, thrown;
          callWithThis(*capReject, {Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Cannot resolve promise with itself"))},
                       Value(Undefined{}), out, thrown);
          return;
        }
        if (nested->state == PromiseState::Fulfilled) {
          (*resolveResultPromise)(nested->result);
          return;
        }
        if (nested->state == PromiseState::Rejected) {
          *alreadyResolved = true;
          Value out, thrown;
          callWithThis(*capReject, {nested->result}, Value(Undefined{}), out, thrown);
          return;
        }
        nested->then(
          [resolveResultPromise](Value fulfilled) -> Value {
            (*resolveResultPromise)(fulfilled);
            return fulfilled;
          },
          [alreadyResolved, capReject, callWithThis](Value reason) -> Value {
            if (!*alreadyResolved) {
              *alreadyResolved = true;
              Value out, thrown;
              callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
            }
            return reason;
          });
        return;
      }

      if (finalValue.isObject() || finalValue.isArray() || finalValue.isFunction() ||
          finalValue.isRegex() || finalValue.isProxy()) {
        Value thenValue = Value(Undefined{});
        bool hasThen = false;
        Value thenThrown;
        if (!getPropertyWithThrow(finalValue, "then", thenValue, hasThen, thenThrown)) {
          *alreadyResolved = true;
          Value out, thrown;
          callWithThis(*capReject, {thenThrown}, Value(Undefined{}), out, thrown);
          return;
        }
        if (hasThen && thenValue.isFunction()) {
          auto alreadyCalled = std::make_shared<bool>(false);
          auto resolveFn = GarbageCollector::makeGC<Function>();
          resolveFn->isNative = true;
          resolveFn->nativeFunc = [resolveResultPromise, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
            if (*alreadyCalled) {
              return Value(Undefined{});
            }
            *alreadyCalled = true;
            Value next = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
            (*resolveResultPromise)(next);
            return Value(Undefined{});
          };
          auto rejectFn = GarbageCollector::makeGC<Function>();
          rejectFn->isNative = true;
          rejectFn->nativeFunc = [alreadyResolved, capReject, callWithThis, alreadyCalled](const std::vector<Value>& innerArgs) -> Value {
            if (*alreadyCalled) {
              return Value(Undefined{});
            }
            *alreadyCalled = true;
            if (!*alreadyResolved) {
              *alreadyResolved = true;
              Value reason = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
              Value out, thrown;
              callWithThis(*capReject, {reason}, Value(Undefined{}), out, thrown);
            }
            return Value(Undefined{});
          };

          Value ignored;
          Value thenCallThrown;
          if (!callWithThis(thenValue, {Value(resolveFn), Value(rejectFn)}, finalValue, ignored, thenCallThrown) &&
              !*alreadyCalled) {
            *alreadyResolved = true;
            Value out, thrown;
            callWithThis(*capReject, {thenCallThrown}, Value(Undefined{}), out, thrown);
          }
          return;
        }
      }

      *alreadyResolved = true;
      Value out, thrown;
      if (!callWithThis(*capResolve, {finalValue}, Value(Undefined{}), out, thrown)) {
        // If resolve throws, reject the capability with the thrown error
        Value out2, thrown2;
        callWithThis(*capReject, {thrown}, Value(Undefined{}), out2, thrown2);
      }
    };

    auto finalizeIfDone = [remaining, results, resolveResultPromise]() {
      if (*remaining == 0) {
        auto valuesArray = GarbageCollector::makeGC<Array>();
        valuesArray->elements = results->elements;
        (*resolveResultPromise)(Value(valuesArray));
      }
    };

    auto processElement = [&](size_t index, const Value& nextValue, Value& failureReason) -> bool {
      if (results->elements.size() <= index) {
        results->elements.resize(index + 1, Value(Undefined{}));
      }

      (*remaining)++;
      Value nextPromise = Value(Undefined{});
      Value resolveCallThrown;
      if (!callWithThis(promiseResolveMethod, {nextValue}, constructor, nextPromise, resolveCallThrown)) {
        failureReason = resolveCallThrown;
        return false;
      }

      auto alreadyCalled = std::make_shared<bool>(false);
      auto resolveElement = GarbageCollector::makeGC<Function>();
      resolveElement->isNative = true;
      resolveElement->properties["length"] = Value(1.0);
      resolveElement->properties["name"] = Value(std::string(""));
      resolveElement->nativeFunc = [results, remaining, index, alreadyCalled, finalizeIfDone](const std::vector<Value>& innerArgs) -> Value {
        if (*alreadyCalled) {
          return Value(Undefined{});
        }
        *alreadyCalled = true;
        Value settledValue = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
        auto entry = GarbageCollector::makeGC<Object>();
        entry->properties["status"] = Value(std::string("fulfilled"));
        entry->properties["value"] = settledValue;
        results->elements[index] = Value(entry);
        (*remaining)--;
        finalizeIfDone();
        return Value(Undefined{});
      };

      auto rejectElement = GarbageCollector::makeGC<Function>();
      rejectElement->isNative = true;
      rejectElement->properties["length"] = Value(1.0);
      rejectElement->properties["name"] = Value(std::string(""));
      rejectElement->nativeFunc = [results, remaining, index, alreadyCalled, finalizeIfDone](const std::vector<Value>& innerArgs) -> Value {
        if (*alreadyCalled) {
          return Value(Undefined{});
        }
        *alreadyCalled = true;
        Value settledReason = innerArgs.empty() ? Value(Undefined{}) : innerArgs[0];
        auto entry = GarbageCollector::makeGC<Object>();
        entry->properties["status"] = Value(std::string("rejected"));
        entry->properties["reason"] = settledReason;
        results->elements[index] = Value(entry);
        (*remaining)--;
        finalizeIfDone();
        return Value(Undefined{});
      };

      if (nextPromise.isPromise()) {
        auto promisePtr = nextPromise.getGC<Promise>();
        Value overriddenThen = Value(Undefined{});
        bool hasOverriddenThen = false;

        auto getterIt = promisePtr->properties.find("__get_then");
        if (getterIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          if (getterIt->second.isFunction()) {
            Value getterOut = Value(Undefined{});
            Value getterThrown;
            if (!callWithThis(getterIt->second, {}, nextPromise, getterOut, getterThrown)) {
              failureReason = getterThrown;
              return false;
            }
            overriddenThen = getterOut;
          }
        } else if (auto thenIt = promisePtr->properties.find("then");
                   thenIt != promisePtr->properties.end()) {
          hasOverriddenThen = true;
          overriddenThen = thenIt->second;
        }

        if (hasOverriddenThen) {
          if (!overriddenThen.isFunction()) {
            failureReason = typeErrorValue("Promise resolve result is not thenable");
            return false;
          }
          Value ignored;
          Value thenInvokeThrown;
          if (!callWithThis(overriddenThen, {Value(resolveElement), Value(rejectElement)},
                            nextPromise, ignored, thenInvokeThrown) &&
              !*alreadyCalled) {
            failureReason = thenInvokeThrown;
            return false;
          }
          return true;
        }

        promisePtr->then(
          [resolveElement](Value v) -> Value {
            return resolveElement->nativeFunc({v});
          },
          [rejectElement](Value reason) -> Value {
            return rejectElement->nativeFunc({reason});
          });
        return true;
      }

      Value thenMethod = Value(Undefined{});
      bool hasThenMethod = false;
      Value thenLookupThrown;
      if (!getPropertyWithThrow(nextPromise, "then", thenMethod, hasThenMethod, thenLookupThrown)) {
        failureReason = thenLookupThrown;
        return false;
      }
      if (!hasThenMethod || !thenMethod.isFunction()) {
        failureReason = typeErrorValue("Promise resolve result is not thenable");
        return false;
      }

      Value ignored;
      Value thenInvokeThrown;
      if (!callWithThis(thenMethod, {Value(resolveElement), Value(rejectElement)}, nextPromise, ignored, thenInvokeThrown) &&
          !*alreadyCalled) {
        failureReason = thenInvokeThrown;
        return false;
      }
      return true;
    };

    auto closeIterator = [&](const Value& iteratorValue, Value& closeFailure) -> bool {
      Value returnMethod = Value(Undefined{});
      bool hasReturn = false;
      Value returnLookupThrown;
      if (!getPropertyWithThrow(iteratorValue, "return", returnMethod, hasReturn, returnLookupThrown)) {
        closeFailure = returnLookupThrown;
        return false;
      }
      if (!hasReturn || returnMethod.isUndefined() || returnMethod.isNull()) {
        return true;
      }
      if (!returnMethod.isFunction()) {
        closeFailure = typeErrorValue("Iterator return is not callable");
        return false;
      }
      Value ignored;
      Value returnThrown;
      if (!callWithThis(returnMethod, {}, iteratorValue, ignored, returnThrown)) {
        closeFailure = returnThrown;
        return false;
      }
      return true;
    };

    const auto& iteratorKey = WellKnownSymbols::iteratorKey();
    size_t nextIndex = 0;
    // Check if array has a custom/poisoned Symbol.iterator getter before using fast path
    bool useArrayFastPath = false;
    if (iterable.isArray()) {
      auto arr = iterable.getGC<Array>();
      auto getterIt = arr->properties.find("__get_" + iteratorKey);
      auto propIt = arr->properties.find(iteratorKey);
      if (getterIt == arr->properties.end() && propIt == arr->properties.end()) {
        useArrayFastPath = true;
      }
    }
    if (useArrayFastPath) {
      auto arr = iterable.getGC<Array>();
      for (const auto& value : arr->elements) {
        Value failureReason;
        if (!processElement(nextIndex++, value, failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else if (iterable.isString()) {
      const auto& str = std::get<std::string>(iterable.data);
      for (char c : str) {
        Value failureReason;
        if (!processElement(nextIndex++, Value(std::string(1, c)), failureReason)) {
          return rejectAndReturn(failureReason);
        }
      }
    } else {
      Value iteratorMethod = Value(Undefined{});
      bool hasIteratorMethod = false;
      Value iteratorLookupThrown;
      if (!getPropertyWithThrow(iterable, iteratorKey, iteratorMethod, hasIteratorMethod, iteratorLookupThrown)) {
        return rejectAndReturn(iteratorLookupThrown);
      }
      if (!hasIteratorMethod || iteratorMethod.isUndefined() || iteratorMethod.isNull()) {
        return rejectAndReturn(typeErrorValue("Value is not iterable"));
      }
      if (!iteratorMethod.isFunction()) {
        return rejectAndReturn(typeErrorValue("Symbol.iterator is not callable"));
      }

      Value iteratorValue = Value(Undefined{});
      Value iteratorThrown;
      if (!callWithThis(iteratorMethod, {}, iterable, iteratorValue, iteratorThrown)) {
        return rejectAndReturn(iteratorThrown);
      }
      if (!iteratorValue.isObject()) {
        return rejectAndReturn(typeErrorValue("Iterator must be an object"));
      }

      while (true) {
        Value nextMethod = Value(Undefined{});
        bool hasNext = false;
        Value nextLookupThrown;
        if (!getPropertyWithThrow(iteratorValue, "next", nextMethod, hasNext, nextLookupThrown)) {
          return rejectAndReturn(nextLookupThrown);
        }
        if (!hasNext || !nextMethod.isFunction()) {
          return rejectAndReturn(typeErrorValue("Iterator next is not callable"));
        }

        Value stepResult = Value(Undefined{});
        Value nextThrown;
        if (!callWithThis(nextMethod, {}, iteratorValue, stepResult, nextThrown)) {
          return rejectAndReturn(nextThrown);
        }
        if (!stepResult.isObject()) {
          return rejectAndReturn(typeErrorValue("Iterator result is not an object"));
        }

        Value doneValue = Value(false);
        bool hasDone = false;
        Value doneThrown;
        if (!getPropertyWithThrow(stepResult, "done", doneValue, hasDone, doneThrown)) {
          return rejectAndReturn(doneThrown);
        }
        if (hasDone && doneValue.toBool()) {
          break;
        }

        Value itemValue = Value(Undefined{});
        bool hasItem = false;
        Value itemThrown;
        if (!getPropertyWithThrow(stepResult, "value", itemValue, hasItem, itemThrown)) {
          return rejectAndReturn(itemThrown);
        }

        Value failureReason;
        if (!processElement(nextIndex++, hasItem ? itemValue : Value(Undefined{}), failureReason)) {
          Value closeFailure;
          if (!closeIterator(iteratorValue, closeFailure)) {
            return rejectAndReturn(closeFailure);
          }
          return rejectAndReturn(failureReason);
        }
      }
    }

    (*remaining)--;
    finalizeIfDone();
    return resultPromiseValue;
  };
  promiseConstructor->properties["allSettled"] = Value(promiseAllSettled);

  // Promise.any - resolves when any promise fulfills, rejects if all reject
  auto promiseAny = GarbageCollector::makeGC<Function>();
  promiseAny->isNative = true;
  promiseAny->properties["name"] = Value(std::string("any"));
  promiseAny->properties["length"] = Value(1.0);
  promiseAny->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = GarbageCollector::makeGC<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->properties["__constructor__"] = Value(promiseFunc);
      promise->reject(Value(std::string("Promise.any expects an array")));
      return Value(promise);
    }

    auto arr = args[0].getGC<Array>();
    auto resultPromise = GarbageCollector::makeGC<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    resultPromise->properties["__constructor__"] = Value(promiseFunc);

    if (arr->elements.empty()) {
      // Empty array - reject with AggregateError
      auto err = GarbageCollector::makeGC<Error>(ErrorType::Error, "All promises were rejected");
      resultPromise->reject(Value(err));
      return Value(resultPromise);
    }

    auto errors = GarbageCollector::makeGC<Array>();
    bool hasResolved = false;

    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = elem.getGC<Promise>();
        if (p->state == PromiseState::Fulfilled && !hasResolved) {
          hasResolved = true;
          resultPromise->resolve(p->result);
          break;
        } else if (p->state == PromiseState::Rejected) {
          errors->elements.push_back(p->result);
        }
      } else {
        // Non-promise values are treated as fulfilled
        if (!hasResolved) {
          hasResolved = true;
          resultPromise->resolve(elem);
          break;
        }
      }
    }

    if (!hasResolved) {
      // All rejected - create AggregateError-like object
      auto err = GarbageCollector::makeGC<Error>(ErrorType::Error, "All promises were rejected");
      resultPromise->reject(Value(err));
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["any"] = Value(promiseAny);

  // Promise.race - resolves or rejects with the first settled promise
  auto promiseRace = GarbageCollector::makeGC<Function>();
  promiseRace->isNative = true;
  promiseRace->properties["name"] = Value(std::string("race"));
  promiseRace->properties["length"] = Value(1.0);
  promiseRace->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isArray()) {
      auto promise = GarbageCollector::makeGC<Promise>();
      GarbageCollector::instance().reportAllocation(sizeof(Promise));
      promise->properties["__constructor__"] = Value(promiseFunc);
      promise->reject(Value(std::string("Promise.race expects an array")));
      return Value(promise);
    }

    auto arr = args[0].getGC<Array>();
    auto resultPromise = GarbageCollector::makeGC<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    resultPromise->properties["__constructor__"] = Value(promiseFunc);

    for (const auto& elem : arr->elements) {
      if (elem.isPromise()) {
        auto p = elem.getGC<Promise>();
        if (p->state == PromiseState::Fulfilled) {
          resultPromise->resolve(p->result);
          break;
        } else if (p->state == PromiseState::Rejected) {
          resultPromise->reject(p->result);
          break;
        }
      } else {
        // Non-promise value settles immediately
        resultPromise->resolve(elem);
        break;
      }
    }

    return Value(resultPromise);
  };
  promiseConstructor->properties["race"] = Value(promiseRace);

  // Promise constructor function
  promiseFunc->nativeFunc = [promiseFunc](const std::vector<Value>& args) -> Value {
    auto promise = GarbageCollector::makeGC<Promise>();
    GarbageCollector::instance().reportAllocation(sizeof(Promise));
    promise->properties["__constructor__"] = Value(promiseFunc);

    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Promise resolver is not a function");
    }
    auto executor = args[0].getGC<Function>();

    // Create resolve and reject functions
    auto resolveFunc = GarbageCollector::makeGC<Function>();
    resolveFunc->isNative = true;
    auto promisePtr = promise;
    resolveFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        promisePtr->resolve(args[0]);
      } else {
        promisePtr->resolve(Value(Undefined{}));
      }
      return Value(Undefined{});
    };

    auto rejectFunc = GarbageCollector::makeGC<Function>();
    rejectFunc->isNative = true;
    rejectFunc->nativeFunc = [promisePtr](const std::vector<Value>& args) -> Value {
      if (!args.empty()) {
        promisePtr->reject(args[0]);
      } else {
        promisePtr->reject(Value(Undefined{}));
      }
      return Value(Undefined{});
    };

    // Call executor with resolve and reject
    if (executor->isNative) {
      try {
        executor->nativeFunc({Value(resolveFunc), Value(rejectFunc)});
      } catch (const std::exception& e) {
        promise->reject(Value(std::string(e.what())));
      }
    } else {
      Interpreter* interpreter = getGlobalInterpreter();
      if (interpreter) {
        interpreter->clearError();
        interpreter->callForHarness(Value(executor), {Value(resolveFunc), Value(rejectFunc)}, Value(Undefined{}));
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          promise->reject(err);
        }
      } else {
        promise->reject(Value(GarbageCollector::makeGC<Error>(ErrorType::TypeError, "Interpreter unavailable")));
      }
    }

    return Value(promise);
  };

  // Expose Promise as a callable constructor with static methods.
  for (const auto& [k, v] : promiseConstructor->properties) {
    promiseFunc->properties[k] = v;
  }
  env->define("Promise", Value(promiseFunc));
  // Keep intrinsic Promise reachable even if global Promise is overwritten.
  env->define("__intrinsic_Promise__", Value(promiseFunc));

  // JSON object
  auto jsonObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // JSON.parse
  auto jsonParse = GarbageCollector::makeGC<Function>();
  jsonParse->isNative = true;
  jsonParse->nativeFunc = JSON_parse;
  jsonObj->properties["parse"] = Value(jsonParse);

  // JSON.stringify
  auto jsonStringify = GarbageCollector::makeGC<Function>();
  jsonStringify->isNative = true;
  jsonStringify->nativeFunc = JSON_stringify;
  jsonObj->properties["stringify"] = Value(jsonStringify);

  // Keep intrinsic JSON reachable even if global JSON is deleted/overwritten.
  env->define("__intrinsic_JSON__", Value(jsonObj));

  // Object static methods
  auto objectConstructor = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  objectConstructor->isNative = true;
  objectConstructor->isConstructor = true;
  objectConstructor->properties["name"] = Value(std::string("Object"));
  objectConstructor->properties["length"] = Value(1.0);
  objectConstructor->nativeFunc = [env](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      auto obj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      return Value(obj);
    }

    const Value& value = args[0];
    if (!value.isBool() && !value.isNumber() && !value.isString() &&
        !value.isBigInt() && !value.isSymbol()) {
      return value;
    }

    auto wrapped = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    wrapped->properties["__primitive_value__"] = value;

    if (!value.isBigInt()) {
      auto valueOf = GarbageCollector::makeGC<Function>();
      valueOf->isNative = true;
      valueOf->properties["__uses_this_arg__"] = Value(true);
      valueOf->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
        if (!callArgs.empty() && callArgs[0].isObject()) {
          auto obj = callArgs[0].getGC<Object>();
          auto it = obj->properties.find("__primitive_value__");
          if (it != obj->properties.end()) {
            return it->second;
          }
        }
        return callArgs.empty() ? Value(Undefined{}) : callArgs[0];
      };
      wrapped->properties["valueOf"] = Value(valueOf);

      auto toString = GarbageCollector::makeGC<Function>();
      toString->isNative = true;
      toString->properties["__uses_this_arg__"] = Value(true);
      toString->nativeFunc = [](const std::vector<Value>& callArgs) -> Value {
        if (!callArgs.empty() && callArgs[0].isObject()) {
          auto obj = callArgs[0].getGC<Object>();
          auto it = obj->properties.find("__primitive_value__");
          if (it != obj->properties.end()) {
            return Value(it->second.toString());
          }
        }
        return Value(callArgs.empty() ? std::string("undefined") : callArgs[0].toString());
      };
      wrapped->properties["toString"] = Value(toString);
    }

    if (value.isBigInt()) {
      if (auto bigIntCtor = env->get("BigInt")) {
        if (bigIntCtor->isFunction()) {
          auto ctor = std::get<GCPtr<Function>>(bigIntCtor->data);
          auto protoIt = ctor->properties.find("prototype");
          if (protoIt != ctor->properties.end() && protoIt->second.isObject()) {
            wrapped->properties["__proto__"] = protoIt->second;
          }
        }
      }
    }

    return Value(wrapped);
  };

  auto objectPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  objectConstructor->properties["prototype"] = Value(objectPrototype);
  objectPrototype->properties["constructor"] = Value(objectConstructor);
  objectPrototype->properties["__non_enum_constructor"] = Value(true);
  if (auto hiddenArrayProto = env->get("__array_prototype__");
      hiddenArrayProto.has_value() && hiddenArrayProto->isObject()) {
    hiddenArrayProto->getGC<Object>()->properties["__proto__"] = Value(objectPrototype);
  }

  auto objectProtoHasOwnProperty = GarbageCollector::makeGC<Function>();
  objectProtoHasOwnProperty->isNative = true;
  objectProtoHasOwnProperty->properties["__uses_this_arg__"] = Value(true);
  objectProtoHasOwnProperty->nativeFunc = Object_hasOwnProperty;
  objectPrototype->properties["hasOwnProperty"] = Value(objectProtoHasOwnProperty);
  objectPrototype->properties["__non_enum_hasOwnProperty"] = Value(true);

  auto objectProtoIsPrototypeOf = GarbageCollector::makeGC<Function>();
  objectProtoIsPrototypeOf->isNative = true;
  objectProtoIsPrototypeOf->properties["__uses_this_arg__"] = Value(true);
  objectProtoIsPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    if (args.size() < 2) {
      return Value(false);
    }
    Value protoVal = args[0];
    if (!protoVal.isObject()) {
      return Value(false);
    }
    auto protoObj = protoVal.getGC<Object>();

    Value v = args[1];
    if (v.isUndefined() || v.isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }

    auto nextProtoValue = [&](const Value& cur) -> Value {
      if (cur.isObject()) {
        auto o = cur.getGC<Object>();
        auto it = o->properties.find("__proto__");
        return it != o->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isFunction()) {
        auto f = cur.getGC<Function>();
        auto it = f->properties.find("__proto__");
        return it != f->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isArray()) {
        auto a = cur.getGC<Array>();
        auto it = a->properties.find("__proto__");
        return it != a->properties.end() ? it->second : Value(Undefined{});
      }
      if (cur.isClass()) {
        auto c = cur.getGC<Class>();
        auto it = c->properties.find("__proto__");
        return it != c->properties.end() ? it->second : Value(Undefined{});
      }
      return Value(Undefined{});
    };

    Value cur = v;
    int depth = 0;
    while (depth < 100) {
      Value p = nextProtoValue(cur);
      if (p.isUndefined() || p.isNull()) {
        return Value(false);
      }
      if (p.isObject() && p.getGC<Object>().get() == protoObj.get()) {
        return Value(true);
      }
      cur = p;
      depth++;
    }
    return Value(false);
  };
  objectPrototype->properties["isPrototypeOf"] = Value(objectProtoIsPrototypeOf);
  objectPrototype->properties["__non_enum_isPrototypeOf"] = Value(true);

  auto objectProtoValueOf = GarbageCollector::makeGC<Function>();
  objectProtoValueOf->isNative = true;
  objectProtoValueOf->properties["__uses_this_arg__"] = Value(true);
  objectProtoValueOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    return args[0];
  };
  objectPrototype->properties["valueOf"] = Value(objectProtoValueOf);
  objectPrototype->properties["__non_enum_valueOf"] = Value(true);

  auto objectProtoToString = GarbageCollector::makeGC<Function>();
  objectProtoToString->isNative = true;
  objectProtoToString->properties["__uses_this_arg__"] = Value(true);
  objectProtoToString->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined()) {
      return Value(std::string("[object Undefined]"));
    }
    if (args[0].isNull()) {
      return Value(std::string("[object Null]"));
    }

    std::string tag = "Object";
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      auto toStringTagIt = obj->properties.find(WellKnownSymbols::toStringTagKey());
      if (toStringTagIt != obj->properties.end()) {
        tag = toStringTagIt->second.toString();
      }
    } else if (args[0].isArray()) {
      tag = "Array";
    } else if (args[0].isFunction()) {
      tag = "Function";
    } else if (args[0].isTypedArray()) {
      auto ta = args[0].getGC<TypedArray>();
      switch (ta->type) {
        case TypedArrayType::Int8: tag = "Int8Array"; break;
        case TypedArrayType::Uint8: tag = "Uint8Array"; break;
        case TypedArrayType::Uint8Clamped: tag = "Uint8ClampedArray"; break;
        case TypedArrayType::Int16: tag = "Int16Array"; break;
        case TypedArrayType::Uint16: tag = "Uint16Array"; break;
        case TypedArrayType::Int32: tag = "Int32Array"; break;
        case TypedArrayType::Uint32: tag = "Uint32Array"; break;
        case TypedArrayType::Float16: tag = "Float16Array"; break;
        case TypedArrayType::Float32: tag = "Float32Array"; break;
        case TypedArrayType::Float64: tag = "Float64Array"; break;
        case TypedArrayType::BigInt64: tag = "BigInt64Array"; break;
        case TypedArrayType::BigUint64: tag = "BigUint64Array"; break;
      }
    } else if (args[0].isArrayBuffer()) {
      tag = "ArrayBuffer";
    } else if (args[0].isDataView()) {
      tag = "DataView";
    } else if (args[0].isRegex()) {
      tag = "RegExp";
    } else if (args[0].isError()) {
      tag = "Error";
    }
    return Value(std::string("[object ") + tag + "]");
  };
  objectPrototype->properties["toString"] = Value(objectProtoToString);
  objectPrototype->properties["__non_enum_toString"] = Value(true);

  // Object.prototype.propertyIsEnumerable
  auto objectProtoPropertyIsEnumerable = GarbageCollector::makeGC<Function>();
  objectProtoPropertyIsEnumerable->isNative = true;
  objectProtoPropertyIsEnumerable->properties["__uses_this_arg__"] = Value(true);
  objectProtoPropertyIsEnumerable->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(false);
    Value thisVal = args[0];
    std::string key = valueToPropertyKey(args[1]);

    if (thisVal.isRegex()) {
      auto rx = thisVal.getGC<Regex>();
      if (key == "source" || key == "flags") return Value(true);
      if (rx->properties.find(key) == rx->properties.end() &&
          rx->properties.find("__get_" + key) == rx->properties.end()) {
        return Value(false);
      }
      auto neIt = rx->properties.find("__non_enum_" + key);
      return Value(neIt == rx->properties.end());
    }

    if (thisVal.isError()) {
      auto e = thisVal.getGC<Error>();
      if (e->properties.find(key) == e->properties.end() &&
          e->properties.find("__get_" + key) == e->properties.end()) {
        return Value(false);
      }
      auto neIt = e->properties.find("__non_enum_" + key);
      return Value(neIt == e->properties.end());
    }

    if (thisVal.isTypedArray()) {
      auto ta = thisVal.getGC<TypedArray>();
      if (key == "length" || key == "byteLength") return Value(false);
      if (!key.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
        try {
          size_t idx = std::stoull(key);
          if (idx < ta->length) return Value(true);
        } catch (...) {
        }
      }
      if (ta->properties.find(key) == ta->properties.end() &&
          ta->properties.find("__get_" + key) == ta->properties.end()) {
        return Value(false);
      }
      auto neIt = ta->properties.find("__non_enum_" + key);
      return Value(neIt == ta->properties.end());
    }

    if (thisVal.isFunction()) {
      auto fn = thisVal.getGC<Function>();
      // name, length, prototype are non-enumerable on functions
      if (key == "name" || key == "length" || key == "prototype") return Value(false);
      // Internal properties
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") return Value(false);
      auto it = fn->properties.find(key);
      if (it == fn->properties.end()) return Value(false);
      // Check enum marker: built-in function props default non-enumerable
      auto enumIt = fn->properties.find("__enum_" + key);
      return Value(enumIt != fn->properties.end());
    }
    if (thisVal.isClass()) {
      auto cls = thisVal.getGC<Class>();
      // Internal properties
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") return Value(false);
      if (key.size() > 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) return Value(false);
      if (key.size() > 10 && key.substr(0, 10) == "__non_enum") return Value(false);
      if (key.size() > 14 && key.substr(0, 14) == "__non_writable") return Value(false);
      if (key.size() > 18 && key.substr(0, 18) == "__non_configurable") return Value(false);
      auto it = cls->properties.find(key);
      if (it == cls->properties.end()) return Value(false);
      auto enumIt = cls->properties.find("__enum_" + key);
      return Value(enumIt != cls->properties.end());
    }
    if (thisVal.isObject()) {
      auto obj = thisVal.getGC<Object>();
      if (obj->isModuleNamespace) {
        if (key == WellKnownSymbols::toStringTagKey()) {
          return Value(false);
        }
        bool isExport = isModuleNamespaceExportKey(obj, key);
        if (!isExport) {
          return Value(false);
        }
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          auto getter = getterIt->second.getGC<Function>();
          if (getter && getter->isNative) {
            getter->nativeFunc({});
          }
        }
        return Value(true);
      }
      // Internal properties
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") return Value(false);
      auto it = obj->properties.find(key);
      if (it == obj->properties.end()) return Value(false);
      auto neIt = obj->properties.find("__non_enum_" + key);
      return Value(neIt == obj->properties.end());
    }
    if (thisVal.isArray()) {
      auto arr = thisVal.getGC<Array>();
      auto isCanonicalArrayIndex = [&](const std::string& s, size_t& outIdx) -> bool {
        if (s.empty()) return false;
        if (s.size() > 1 && s[0] == '0') return false;
        for (unsigned char c : s) {
          if (std::isdigit(c) == 0) return false;
        }
        try {
          outIdx = static_cast<size_t>(std::stoull(s));
        } catch (...) {
          return false;
        }
        return true;
      };
      size_t idx = 0;
      if (isCanonicalArrayIndex(key, idx)) {
        bool exists = idx < arr->elements.size() ||
                      arr->properties.find(key) != arr->properties.end() ||
                      arr->properties.find("__get_" + key) != arr->properties.end() ||
                      arr->properties.find("__set_" + key) != arr->properties.end();
        if (!exists) return Value(false);
        auto neIt = arr->properties.find("__non_enum_" + key);
        return Value(neIt == arr->properties.end());
      }
      auto it = arr->properties.find(key);
      if (it == arr->properties.end()) return Value(false);
      auto neIt = arr->properties.find("__non_enum_" + key);
      return Value(neIt == arr->properties.end());
    }
    return Value(false);
  };
  objectPrototype->properties["propertyIsEnumerable"] = Value(objectProtoPropertyIsEnumerable);
  objectPrototype->properties["__non_enum_propertyIsEnumerable"] = Value(true);

  // Annex B: Object.prototype.__lookupGetter__
  auto objectProtoLookupGetter = GarbageCollector::makeGC<Function>();
  objectProtoLookupGetter->isNative = true;
  objectProtoLookupGetter->properties["__uses_this_arg__"] = Value(true);
  objectProtoLookupGetter->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});
    Value current = args[0];
    std::string key = valueToPropertyKey(args[1]);

    auto getProps = [](const Value& v) -> OrderedMap<std::string, Value>* {
      if (v.isObject()) return &v.getGC<Object>()->properties;
      if (v.isArray()) return &v.getGC<Array>()->properties;
      if (v.isFunction()) return &v.getGC<Function>()->properties;
      if (v.isClass()) return &v.getGC<Class>()->properties;
      if (v.isPromise()) return &v.getGC<Promise>()->properties;
      if (v.isRegex()) return &v.getGC<Regex>()->properties;
      return nullptr;
    };

    for (int depth = 0; depth < 64; ++depth) {
      auto* props = getProps(current);
      if (!props) break;

      auto getterIt = props->find("__get_" + key);
      if (getterIt != props->end() && getterIt->second.isFunction()) {
        return getterIt->second;
      }
      if (props->find(key) != props->end()) {
        return Value(Undefined{});
      }

      auto protoIt = props->find("__proto__");
      if (protoIt == props->end() || protoIt->second.isNull()) break;
      current = protoIt->second;
    }
    return Value(Undefined{});
  };
  objectPrototype->properties["__lookupGetter__"] = Value(objectProtoLookupGetter);
  objectPrototype->properties["__non_enum___lookupGetter__"] = Value(true);

  // Annex B: Object.prototype.__lookupSetter__
  auto objectProtoLookupSetter = GarbageCollector::makeGC<Function>();
  objectProtoLookupSetter->isNative = true;
  objectProtoLookupSetter->properties["__uses_this_arg__"] = Value(true);
  objectProtoLookupSetter->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2) return Value(Undefined{});
    Value current = args[0];
    std::string key = valueToPropertyKey(args[1]);

    auto getProps = [](const Value& v) -> OrderedMap<std::string, Value>* {
      if (v.isObject()) return &v.getGC<Object>()->properties;
      if (v.isArray()) return &v.getGC<Array>()->properties;
      if (v.isFunction()) return &v.getGC<Function>()->properties;
      if (v.isClass()) return &v.getGC<Class>()->properties;
      if (v.isPromise()) return &v.getGC<Promise>()->properties;
      if (v.isRegex()) return &v.getGC<Regex>()->properties;
      return nullptr;
    };

    for (int depth = 0; depth < 64; ++depth) {
      auto* props = getProps(current);
      if (!props) break;

      auto setterIt = props->find("__set_" + key);
      if (setterIt != props->end() && setterIt->second.isFunction()) {
        return setterIt->second;
      }
      if (props->find(key) != props->end()) {
        return Value(Undefined{});
      }

      auto protoIt = props->find("__proto__");
      if (protoIt == props->end() || protoIt->second.isNull()) break;
      current = protoIt->second;
    }
    return Value(Undefined{});
  };
  objectPrototype->properties["__lookupSetter__"] = Value(objectProtoLookupSetter);
  objectPrototype->properties["__non_enum___lookupSetter__"] = Value(true);

  // Object.keys
  auto objectKeys = GarbageCollector::makeGC<Function>();
  objectKeys->isNative = true;
  objectKeys->nativeFunc = Object_keys;
  objectConstructor->properties["keys"] = Value(objectKeys);

  // Object.values
  auto objectValues = GarbageCollector::makeGC<Function>();
  objectValues->isNative = true;
  objectValues->nativeFunc = Object_values;
  objectConstructor->properties["values"] = Value(objectValues);

  // Object.entries
  auto objectEntries = GarbageCollector::makeGC<Function>();
  objectEntries->isNative = true;
  objectEntries->nativeFunc = Object_entries;
  objectConstructor->properties["entries"] = Value(objectEntries);

  // Object.assign
  auto objectAssign = GarbageCollector::makeGC<Function>();
  objectAssign->isNative = true;
  objectAssign->nativeFunc = Object_assign;
  objectConstructor->properties["assign"] = Value(objectAssign);

  // Object.hasOwnProperty (for prototypal access)
  auto objectHasOwnProperty = GarbageCollector::makeGC<Function>();
  objectHasOwnProperty->isNative = true;
  objectHasOwnProperty->nativeFunc = Object_hasOwnProperty;
  objectConstructor->properties["hasOwnProperty"] = Value(objectHasOwnProperty);

  // Object.getOwnPropertyNames
  auto objectGetOwnPropertyNames = GarbageCollector::makeGC<Function>();
  objectGetOwnPropertyNames->isNative = true;
  objectGetOwnPropertyNames->nativeFunc = Object_getOwnPropertyNames;
  objectConstructor->properties["getOwnPropertyNames"] = Value(objectGetOwnPropertyNames);

  auto objectGetOwnPropertySymbols = GarbageCollector::makeGC<Function>();
  objectGetOwnPropertySymbols->isNative = true;
  objectGetOwnPropertySymbols->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto result = GarbageCollector::makeGC<Array>();
    if (args.empty()) {
      return Value(result);
    }
    Value target = args[0];
    OrderedMap<std::string, Value>* props = nullptr;
    GCPtr<Object> obj;
    if (target.isObject()) {
      obj = target.getGC<Object>();
      props = &obj->properties;
    } else if (target.isFunction()) {
      props = &target.getGC<Function>()->properties;
    } else if (target.isClass()) {
      props = &target.getGC<Class>()->properties;
    } else if (target.isArray()) {
      props = &target.getGC<Array>()->properties;
    } else if (target.isRegex()) {
      props = &target.getGC<Regex>()->properties;
    } else if (target.isPromise()) {
      props = &target.getGC<Promise>()->properties;
    } else if (target.isError()) {
      props = &target.getGC<Error>()->properties;
    }
    if (!props) {
      return Value(result);
    }
    auto appendSymbolForKey = [&](const std::string& key) {
      if (key == WellKnownSymbols::iteratorKey()) {
        result->elements.push_back(WellKnownSymbols::iterator());
      } else if (key == WellKnownSymbols::asyncIteratorKey()) {
        result->elements.push_back(WellKnownSymbols::asyncIterator());
      } else if (key == WellKnownSymbols::toStringTagKey()) {
        result->elements.push_back(WellKnownSymbols::toStringTag());
      } else if (key == WellKnownSymbols::toPrimitiveKey()) {
        result->elements.push_back(WellKnownSymbols::toPrimitive());
      } else if (key == WellKnownSymbols::matchAllKey()) {
        result->elements.push_back(WellKnownSymbols::matchAll());
      } else {
        Symbol symbolValue;
        if (propertyKeyToSymbol(key, symbolValue)) {
          result->elements.push_back(Value(symbolValue));
        }
      }
    };

    if (obj && obj->isModuleNamespace) {
      appendSymbolForKey(WellKnownSymbols::toStringTagKey());
      return Value(result);
    }

    for (const auto& key : props->orderedKeys()) {
      if (!isSymbolPropertyKey(key)) continue;
      appendSymbolForKey(key);
    }
    return Value(result);
  };
  objectConstructor->properties["getOwnPropertySymbols"] = Value(objectGetOwnPropertySymbols);

  // Object.create
  auto objectCreate = GarbageCollector::makeGC<Function>();
  objectCreate->isNative = true;
  objectCreate->nativeFunc = Object_create;
  objectConstructor->properties["create"] = Value(objectCreate);

  // Object.fromEntries - converts array of [key, value] pairs to object
  auto objectFromEntries = GarbageCollector::makeGC<Function>();
  objectFromEntries->isNative = true;
  objectFromEntries->nativeFunc = Object_fromEntries;
  objectConstructor->properties["fromEntries"] = Value(objectFromEntries);

  // Object.hasOwn - checks if object has own property
  auto objectHasOwn = GarbageCollector::makeGC<Function>();
  objectHasOwn->isNative = true;
  objectHasOwn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isNull() || args[0].isUndefined()) {
      throw std::runtime_error("TypeError: Cannot convert undefined or null to object");
    }
    std::string key = args.size() > 1 ? valueToPropertyKey(args[1]) : "undefined";
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      if (obj->isModuleNamespace) {
        if (key == WellKnownSymbols::toStringTagKey()) {
          return Value(true);
        }
        if (!isModuleNamespaceExportKey(obj, key)) {
          return Value(false);
        }
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          auto getter = getterIt->second.getGC<Function>();
          if (getter && getter->isNative) {
            getter->nativeFunc({});
          }
        }
        return Value(true);
      }
      if (obj->properties.find(key) != obj->properties.end()) return Value(true);
      if (obj->properties.find("__get_" + key) != obj->properties.end()) return Value(true);
      if (obj->properties.find("__set_" + key) != obj->properties.end()) return Value(true);
      return Value(false);
    }
    if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      // Check numeric index
      bool isNum = !key.empty() && std::all_of(key.begin(), key.end(), ::isdigit);
      if (isNum) {
        try {
          size_t idx = std::stoul(key);
          if (idx < arr->elements.size()) return Value(true);
        } catch (...) {}
      }
      // Check named properties (including symbol keys)
      if (arr->properties.find(key) != arr->properties.end()) return Value(true);
      if (arr->properties.find("__get_" + key) != arr->properties.end()) return Value(true);
      if (arr->properties.find("__set_" + key) != arr->properties.end()) return Value(true);
      return Value(false);
    }
    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();
      if (fn->properties.find(key) != fn->properties.end()) return Value(true);
      if (fn->properties.find("__get_" + key) != fn->properties.end()) return Value(true);
      if (fn->properties.find("__set_" + key) != fn->properties.end()) return Value(true);
      return Value(false);
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      if (cls->properties.find(key) != cls->properties.end()) return Value(true);
      return Value(false);
    }
    return Value(false);
  };
  objectConstructor->properties["hasOwn"] = Value(objectHasOwn);

  auto objectGetPrototypeOf = GarbageCollector::makeGC<Function>();
  objectGetPrototypeOf->isNative = true;
  objectGetPrototypeOf->nativeFunc = [promisePrototype, env](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Null{});
    }
    if (args[0].isPromise()) {
      auto p = args[0].getGC<Promise>();
      if (auto it = p->properties.find("__proto__"); it != p->properties.end()) {
        return it->second;
      }
      return Value(promisePrototype);
    }
    if (args[0].isTypedArray()) {
      auto ta = args[0].getGC<TypedArray>();
      if (auto it = ta->properties.find("__proto__"); it != ta->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isArrayBuffer()) {
      auto b = args[0].getGC<ArrayBuffer>();
      if (auto it = b->properties.find("__proto__"); it != b->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isDataView()) {
      auto v = args[0].getGC<DataView>();
      if (auto it = v->properties.find("__proto__"); it != v->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isMap()) {
      auto m = args[0].getGC<Map>();
      if (auto it = m->properties.find("__proto__"); it != m->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isSet()) {
      auto s = args[0].getGC<Set>();
      if (auto it = s->properties.find("__proto__"); it != s->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isWeakMap()) {
      auto wm = args[0].getGC<WeakMap>();
      if (auto it = wm->properties.find("__proto__"); it != wm->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isWeakSet()) {
      auto ws = args[0].getGC<WeakSet>();
      if (auto it = ws->properties.find("__proto__"); it != ws->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isRegex()) {
      auto rx = args[0].getGC<Regex>();
      if (auto it = rx->properties.find("__proto__"); it != rx->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isArray()) {
      if (auto arrayCtor = env->get("Array"); arrayCtor && arrayCtor->isObject()) {
        auto arrayObj = std::get<GCPtr<Object>>(arrayCtor->data);
        auto protoIt = arrayObj->properties.find("prototype");
        if (protoIt != arrayObj->properties.end()) {
          return protoIt->second;
        }
      }
      if (auto hiddenProto = env->get("__array_prototype__"); hiddenProto.has_value()) {
        return *hiddenProto;
      }
      return Value(Null{});
    }
    if (args[0].isError()) {
      auto err = args[0].getGC<Error>();
      if (auto it = err->properties.find("__proto__"); it != err->properties.end()) {
        return it->second;
      }
      std::string ctorName = "Error";
      switch (err->type) {
        case ErrorType::TypeError:
          ctorName = "TypeError";
          break;
        case ErrorType::ReferenceError:
          ctorName = "ReferenceError";
          break;
        case ErrorType::RangeError:
          ctorName = "RangeError";
          break;
        case ErrorType::SyntaxError:
          ctorName = "SyntaxError";
          break;
        case ErrorType::URIError:
          ctorName = "URIError";
          break;
        case ErrorType::EvalError:
          ctorName = "EvalError";
          break;
        case ErrorType::Error:
        default:
          ctorName = "Error";
          break;
      }
      if (auto ctor = env->get(ctorName); ctor && ctor->isFunction()) {
        auto fn = std::get<GCPtr<Function>>(ctor->data);
        auto protoIt = fn->properties.find("prototype");
        if (protoIt != fn->properties.end()) {
          return protoIt->second;
        }
      }
      return Value(Null{});
    }
    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();
      auto protoIt = fn->properties.find("__proto__");
      if (protoIt != fn->properties.end()) {
        return protoIt->second;
      }
      // Default: Function.prototype
      if (auto funcCtor = env->get("Function"); funcCtor && funcCtor->isFunction()) {
        auto funcFn = std::get<GCPtr<Function>>(funcCtor->data);
        auto fpIt = funcFn->properties.find("prototype");
        if (fpIt != funcFn->properties.end()) {
          return fpIt->second;
        }
      }
      return Value(Null{});
    }
    if (args[0].isBigInt()) {
      // Return BigInt.prototype
      if (auto bigIntCtor = env->get("BigInt"); bigIntCtor && bigIntCtor->isFunction()) {
        auto fn = std::get<GCPtr<Function>>(bigIntCtor->data);
        auto protoIt = fn->properties.find("prototype");
        if (protoIt != fn->properties.end()) {
          return protoIt->second;
        }
      }
      return Value(Null{});
    }
    if (args[0].isGenerator()) {
      auto gen = args[0].getGC<Generator>();
      auto it = gen->properties.find("__proto__");
      if (it != gen->properties.end()) {
        return it->second;
      }
      return Value(Null{});
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      auto it = cls->properties.find("__proto__");
      if (it != cls->properties.end()) {
        return it->second;
      }
      // Default: Function.prototype
      if (auto funcCtor = env->get("Function"); funcCtor && funcCtor->isFunction()) {
        auto funcFn = std::get<GCPtr<Function>>(funcCtor->data);
        auto fpIt = funcFn->properties.find("prototype");
        if (fpIt != funcFn->properties.end()) {
          return fpIt->second;
        }
      }
      return Value(Null{});
    }
    if (!args[0].isObject()) {
      return Value(Null{});
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      return Value(Null{});
    }
    auto it = obj->properties.find("__proto__");
    if (it != obj->properties.end()) {
      return it->second;
    }
    return Value(Null{});
  };
  objectConstructor->properties["getPrototypeOf"] = Value(objectGetPrototypeOf);

  auto objectSetPrototypeOf = GarbageCollector::makeGC<Function>();
  objectSetPrototypeOf->isNative = true;
  objectSetPrototypeOf->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || (!args[0].isObject() && !args[0].isClass())) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      cls->properties["__proto__"] = args[1];
      return args[0];
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      if (args[1].isNull()) {
        return args[0];
      }
      throw std::runtime_error("TypeError: Cannot set prototype of module namespace object");
    }
    obj->properties["__proto__"] = args[1];
    return args[0];
  };
  objectConstructor->properties["setPrototypeOf"] = Value(objectSetPrototypeOf);

  auto objectIsExtensible = GarbageCollector::makeGC<Function>();
  objectIsExtensible->isNative = true;
  objectIsExtensible->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(false);
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      auto it = cls->properties.find("__non_extensible__");
      bool nonExtensible = it != cls->properties.end() &&
                           it->second.isBool() &&
                           it->second.toBool();
      return Value(!nonExtensible);
    }
    if (args[0].isFunction() || args[0].isArray()) {
      return Value(true);  // Functions and arrays are always extensible
    }
    if (!args[0].isObject()) {
      return Value(false);
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      return Value(false);
    }
    return Value(!obj->sealed && !obj->frozen);
  };
  objectConstructor->properties["isExtensible"] = Value(objectIsExtensible);

  auto objectPreventExtensions = GarbageCollector::makeGC<Function>();
  objectPreventExtensions->isNative = true;
  objectPreventExtensions->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      cls->properties["__non_extensible__"] = Value(true);
      return args[0];
    }
    if (!args[0].isObject()) {
      return args[0];
    }
    auto obj = args[0].getGC<Object>();
    if (!obj->isModuleNamespace) {
      obj->sealed = true;
    }
    return args[0];
  };
  objectConstructor->properties["preventExtensions"] = Value(objectPreventExtensions);

  // Object.freeze - makes an object immutable
  auto objectFreeze = GarbageCollector::makeGC<Function>();
  objectFreeze->isNative = true;
  objectFreeze->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = args[0].getGC<Object>();
    if (obj->isModuleNamespace) {
      throw std::runtime_error("TypeError: Cannot freeze module namespace object");
    }
    obj->frozen = true;
    obj->sealed = true;  // Frozen objects are also sealed
    return args[0];
  };
  objectConstructor->properties["freeze"] = Value(objectFreeze);

  // Object.seal - prevents adding or removing properties
  auto objectSeal = GarbageCollector::makeGC<Function>();
  objectSeal->isNative = true;
  objectSeal->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = args[0].getGC<Object>();
    obj->sealed = true;
    return args[0];
  };
  objectConstructor->properties["seal"] = Value(objectSeal);

  // Object.isFrozen - check if object is frozen
  auto objectIsFrozen = GarbageCollector::makeGC<Function>();
  objectIsFrozen->isNative = true;
  objectIsFrozen->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(true);  // Non-objects are considered frozen
    }
    auto obj = args[0].getGC<Object>();
    return Value(obj->frozen);
  };
  objectConstructor->properties["isFrozen"] = Value(objectIsFrozen);

  // Object.isSealed - check if object is sealed
  auto objectIsSealed = GarbageCollector::makeGC<Function>();
  objectIsSealed->isNative = true;
  objectIsSealed->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isObject()) {
      return Value(true);  // Non-objects are considered sealed
    }
    auto obj = args[0].getGC<Object>();
    return Value(obj->sealed);
  };
  objectConstructor->properties["isSealed"] = Value(objectIsSealed);

  // Object.is - SameValue comparison (handles NaN and -0 correctly)
  auto objectIs = GarbageCollector::makeGC<Function>();
  objectIs->isNative = true;
  objectIs->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Missing args are treated as undefined
    Value a = args.size() > 0 ? args[0] : Value(Undefined{});
    Value b = args.size() > 1 ? args[1] : Value(Undefined{});

    // Check for same type
    if (a.data.index() != b.data.index()) return Value(false);

    // Handle numbers specially (NaN === NaN, +0 !== -0)
    if (a.isNumber() && b.isNumber()) {
      double x = a.toNumber();
      double y = b.toNumber();
      if (std::isnan(x) && std::isnan(y)) return Value(true);
      if (x == 0 && y == 0) {
        return Value(std::signbit(x) == std::signbit(y));
      }
      return Value(x == y);
    }

    if (a.isUndefined() && b.isUndefined()) return Value(true);
    if (a.isNull() && b.isNull()) return Value(true);
    if (a.isBool() && b.isBool()) {
      return Value(std::get<bool>(a.data) == std::get<bool>(b.data));
    }
    if (a.isString() && b.isString()) {
      return Value(std::get<std::string>(a.data) == std::get<std::string>(b.data));
    }
    if (a.isBigInt() && b.isBigInt()) {
      return Value(a.toBigInt() == b.toBigInt());
    }
    if (a.isSymbol() && b.isSymbol()) {
      return Value(std::get<Symbol>(a.data) == std::get<Symbol>(b.data));
    }
    // For objects, check reference equality
    if (a.isObject() && b.isObject()) {
      return Value(a.getGC<Object>().get() ==
                   b.getGC<Object>().get());
    }
    if (a.isArray() && b.isArray()) {
      return Value(a.getGC<Array>().get() ==
                   b.getGC<Array>().get());
    }
    if (a.isFunction() && b.isFunction()) {
      return Value(a.getGC<Function>().get() ==
                   b.getGC<Function>().get());
    }
    if (a.isClass() && b.isClass()) {
      return Value(a.getGC<Class>().get() ==
                   b.getGC<Class>().get());
    }
    if (a.isPromise() && b.isPromise()) {
      return Value(a.getGC<Promise>().get() ==
                   b.getGC<Promise>().get());
    }
    if (a.isRegex() && b.isRegex()) {
      return Value(a.getGC<Regex>().get() ==
                   b.getGC<Regex>().get());
    }

    return Value(false);
  };
  objectConstructor->properties["is"] = Value(objectIs);

  // Object.getOwnPropertyDescriptor - get property descriptor
  auto objectGetOwnPropertyDescriptor = GarbageCollector::makeGC<Function>();
  objectGetOwnPropertyDescriptor->isNative = true;
  objectGetOwnPropertyDescriptor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 ||
        (!args[0].isObject() && !args[0].isFunction() && !args[0].isPromise() && !args[0].isRegex() && !args[0].isClass() &&
         !args[0].isArray() && !args[0].isError())) {
      return Value(Undefined{});
    }
    std::string key = valueToPropertyKey(args[1]);

    auto descriptor = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    if (args[0].isClass()) {
      auto cls = args[0].getGC<Class>();
      // Internal properties are not visible.
      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }
      auto getterIt = cls->properties.find("__get_" + key);
      if (getterIt != cls->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }
      auto setterIt = cls->properties.find("__set_" + key);
      if (setterIt != cls->properties.end() && setterIt->second.isFunction()) {
        descriptor->properties["set"] = setterIt->second;
      }

      auto it = cls->properties.find(key);
      if (it == cls->properties.end() && getterIt == cls->properties.end() && setterIt == cls->properties.end()) {
        return Value(Undefined{});
      }
      if (it != cls->properties.end()) {
        descriptor->properties["value"] = it->second;
      }
      // Check for per-property attribute markers
      if (key == "name" || key == "length") {
        // Default: non-writable, non-enumerable, configurable
        bool writable = cls->properties.find("__non_writable_" + key) == cls->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(true);
      } else if (key == "prototype") {
        bool writable = cls->properties.find("__non_writable_prototype") == cls->properties.end();
        bool configurable = cls->properties.find("__non_configurable_prototype") == cls->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(configurable);
      } else {
        bool writable = cls->properties.find("__non_writable_" + key) == cls->properties.end();
        bool enumerable = cls->properties.find("__enum_" + key) != cls->properties.end();
        bool configurable = cls->properties.find("__non_configurable_" + key) == cls->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
      }
      return Value(descriptor);
    }

    if (args[0].isFunction()) {
      auto fn = args[0].getGC<Function>();

      // Internal properties are not visible.
      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }

      auto getterIt = fn->properties.find("__get_" + key);
      if (getterIt != fn->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }

      auto it = fn->properties.find(key);
      if (it == fn->properties.end() && getterIt == fn->properties.end()) {
        return Value(Undefined{});
      }
      if (it != fn->properties.end()) {
        descriptor->properties["value"] = it->second;
      }

      // name and length: non-writable, non-enumerable, configurable
      if (key == "name" || key == "length") {
        descriptor->properties["writable"] = Value(false);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(true);
      } else if (key == "prototype") {
        bool protoWritable = fn->properties.find("__non_writable_prototype") == fn->properties.end();
        bool protoConfigurable = fn->properties.find("__non_configurable_prototype") == fn->properties.end();
        descriptor->properties["writable"] = Value(protoWritable);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(protoConfigurable);
      } else {
        // Check for per-property attribute markers
        bool writable = true;
        bool enumerable = false; // Built-in function properties default to non-enumerable
        bool configurable = true;
        auto nwIt = fn->properties.find("__non_writable_" + key);
        if (nwIt != fn->properties.end()) writable = false;
        auto neIt = fn->properties.find("__enum_" + key);
        if (neIt != fn->properties.end()) enumerable = true;
        auto ncIt = fn->properties.find("__non_configurable_" + key);
        if (ncIt != fn->properties.end()) configurable = false;
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
      }
      return Value(descriptor);
    }

    if (args[0].isArray()) {
      auto arr = args[0].getGC<Array>();
      // Internal properties are not visible.
      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }
      // `length` is a special own data property with fixed attributes.
      if (key == "length") {
        descriptor->properties["value"] = Value(static_cast<double>(arr->elements.size()));
        bool writable = arr->properties.find("__non_writable_length") == arr->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(false);
        descriptor->properties["configurable"] = Value(false);
        return Value(descriptor);
      }
      // Array index properties: consult per-index attribute markers and accessors.
      auto isCanonicalArrayIndex = [&](const std::string& s, size_t& outIdx) -> bool {
        if (s.empty()) return false;
        if (s.size() > 1 && s[0] == '0') return false;  // no leading zeros
        for (unsigned char c : s) {
          if (std::isdigit(c) == 0) return false;
        }
        try {
          outIdx = static_cast<size_t>(std::stoull(s));
        } catch (...) {
          return false;
        }
        // Ignore the 2^32-1 sentinel; keep behavior simple for our tests.
        return true;
      };
      size_t idx = 0;
      if (isCanonicalArrayIndex(key, idx)) {
        bool isArgumentsObject = false;
        auto isArgsIt = arr->properties.find("__is_arguments_object__");
        if (isArgsIt != arr->properties.end() && isArgsIt->second.isBool() && isArgsIt->second.toBool()) {
          isArgumentsObject = true;
        }
        bool isMappedArgumentsIndex = false;
        if (isArgumentsObject) {
          isMappedArgumentsIndex =
            arr->properties.find("__mapped_arg_index_" + key + "__") != arr->properties.end();
        }
        auto getterIt = arr->properties.find("__get_" + key);
        auto setterIt = arr->properties.find("__set_" + key);
        bool hasAccessor = getterIt != arr->properties.end() || setterIt != arr->properties.end();
        bool hasData = idx < arr->elements.size() || arr->properties.find(key) != arr->properties.end();
        if (!hasAccessor && !hasData) {
          return Value(Undefined{});
        }

        if (isMappedArgumentsIndex && getterIt != arr->properties.end() && getterIt->second.isFunction()) {
          // Mapped arguments exotic objects expose data descriptors for indices.
          // The value is the current parameter binding (served by our internal getter).
          auto getterFn = getterIt->second.getGC<Function>();
          Value v = getterFn->nativeFunc({});
          descriptor->properties["value"] = v;
          bool writable = arr->properties.find("__non_writable_" + key) == arr->properties.end();
          descriptor->properties["writable"] = Value(writable);
        } else if (hasAccessor) {
          if (getterIt != arr->properties.end() && getterIt->second.isFunction()) {
            descriptor->properties["get"] = getterIt->second;
          }
          if (setterIt != arr->properties.end() && setterIt->second.isFunction()) {
            descriptor->properties["set"] = setterIt->second;
          }
        } else {
          if (idx < arr->elements.size()) {
            descriptor->properties["value"] = arr->elements[idx];
          } else {
            descriptor->properties["value"] = arr->properties.at(key);
          }
          bool writable = arr->properties.find("__non_writable_" + key) == arr->properties.end();
          descriptor->properties["writable"] = Value(writable);
        }

        bool enumerable = arr->properties.find("__non_enum_" + key) == arr->properties.end();
        bool configurable = arr->properties.find("__non_configurable_" + key) == arr->properties.end();
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
        return Value(descriptor);
      }
      // Regular named properties
      auto it = arr->properties.find(key);
      if (it == arr->properties.end()) {
        return Value(Undefined{});
      }
      descriptor->properties["value"] = it->second;
      bool enumerable = arr->properties.find("__non_enum_" + key) == arr->properties.end();
      bool configurable = arr->properties.find("__non_configurable_" + key) == arr->properties.end();
      bool writable = arr->properties.find("__non_writable_" + key) == arr->properties.end();
      descriptor->properties["writable"] = Value(writable);
      descriptor->properties["enumerable"] = Value(enumerable);
      descriptor->properties["configurable"] = Value(configurable);
      return Value(descriptor);
    }

    if (args[0].isError()) {
      auto err = args[0].getGC<Error>();
      // Internal properties are not visible
      if (key.size() >= 4 && key.substr(0, 2) == "__" &&
          key.substr(key.size() - 2) == "__") {
        return Value(Undefined{});
      }
      auto getterIt = err->properties.find("__get_" + key);
      auto setterIt = err->properties.find("__set_" + key);
      auto it = err->properties.find(key);
      if (it == err->properties.end() && getterIt == err->properties.end() && setterIt == err->properties.end()) {
        return Value(Undefined{});
      }
      if (getterIt != err->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }
      if (setterIt != err->properties.end() && setterIt->second.isFunction()) {
        descriptor->properties["set"] = setterIt->second;
      }
      if (it != err->properties.end()) {
        descriptor->properties["value"] = it->second;
      }
      bool writable = err->properties.find("__non_writable_" + key) == err->properties.end();
      bool enumerable = err->properties.find("__non_enum_" + key) == err->properties.end();
      bool configurable = err->properties.find("__non_configurable_" + key) == err->properties.end();
      descriptor->properties["writable"] = Value(writable);
      descriptor->properties["enumerable"] = Value(enumerable);
      descriptor->properties["configurable"] = Value(configurable);
      return Value(descriptor);
    }

    if (args[0].isPromise()) {
      auto promise = args[0].getGC<Promise>();
      auto getterIt = promise->properties.find("__get_" + key);
      if (getterIt != promise->properties.end() && getterIt->second.isFunction()) {
        descriptor->properties["get"] = getterIt->second;
      }

      auto it = promise->properties.find(key);
      if (it == promise->properties.end() && getterIt == promise->properties.end()) {
        return Value(Undefined{});
      }
      if (it != promise->properties.end()) {
        descriptor->properties["value"] = it->second;
      }
      descriptor->properties["writable"] = Value(true);
      descriptor->properties["enumerable"] = Value(true);
      descriptor->properties["configurable"] = Value(true);
      return Value(descriptor);
    }

    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();

      // __proto__ in properties is the prototype chain link, not an own property.
      // Own __proto__ is stored as __own_prop___proto__ (from computed/defineProperty).
      if (key == "__proto__") {
        auto ownIt = obj->properties.find("__own_prop___proto__");
        if (ownIt == obj->properties.end()) {
          return Value(Undefined{});
        }
        descriptor->properties["value"] = ownIt->second;
        bool writable = obj->properties.find("__non_writable___own_prop___proto__") == obj->properties.end();
        bool enumerable = obj->properties.find("__non_enum___own_prop___proto__") == obj->properties.end();
        bool configurable = obj->properties.find("__non_configurable___own_prop___proto__") == obj->properties.end();
        descriptor->properties["writable"] = Value(writable);
        descriptor->properties["enumerable"] = Value(enumerable);
        descriptor->properties["configurable"] = Value(configurable);
        return Value(descriptor);
      }

      if (obj->isModuleNamespace) {
        if (key == WellKnownSymbols::toStringTagKey()) {
          descriptor->properties["value"] = Value(std::string("Module"));
          descriptor->properties["writable"] = Value(false);
          descriptor->properties["enumerable"] = Value(false);
          descriptor->properties["configurable"] = Value(false);
          return Value(descriptor);
        }
        if (!isModuleNamespaceExportKey(obj, key)) {
          return Value(Undefined{});
        }
        auto getterIt = obj->properties.find("__get_" + key);
        if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
          Interpreter* interpreter = getGlobalInterpreter();
          if (interpreter) {
            Value out = interpreter->callForHarness(getterIt->second, {}, args[0]);
            if (interpreter->hasError()) {
              Value err = interpreter->getError();
              interpreter->clearError();
              throw std::runtime_error(err.toString());
            }
            descriptor->properties["value"] = out;
          } else {
            descriptor->properties["value"] = Value(Undefined{});
          }
        } else if (auto it = obj->properties.find(key); it != obj->properties.end()) {
          descriptor->properties["value"] = it->second;
        } else {
          descriptor->properties["value"] = Value(Undefined{});
        }
        descriptor->properties["writable"] = Value(true);
        descriptor->properties["enumerable"] = Value(true);
        descriptor->properties["configurable"] = Value(false);
        return Value(descriptor);
      }
      // Internal properties are not visible.
      if (isInternalPropertyKeyForReflection(key)) {
        return Value(Undefined{});
      }

      auto getterIt2 = obj->properties.find("__get_" + key);
      auto setterIt2 = obj->properties.find("__set_" + key);
      auto it = obj->properties.find(key);
      if (it == obj->properties.end() && getterIt2 == obj->properties.end() && setterIt2 == obj->properties.end()) {
        return Value(Undefined{});
      }
      if (getterIt2 != obj->properties.end() && getterIt2->second.isFunction()) {
        descriptor->properties["get"] = getterIt2->second;
      }
      if (setterIt2 != obj->properties.end() && setterIt2->second.isFunction()) {
        descriptor->properties["set"] = setterIt2->second;
      }
      if (it != obj->properties.end()) {
        descriptor->properties["value"] = it->second;
      }

      // Check for per-property attribute markers
      bool writable = !obj->frozen;
      bool enumerable = true;
      bool configurable = !obj->sealed;
      auto nwIt = obj->properties.find("__non_writable_" + key);
      if (nwIt != obj->properties.end()) writable = false;
      auto neIt = obj->properties.find("__non_enum_" + key);
      if (neIt != obj->properties.end()) enumerable = false;
      auto ncIt = obj->properties.find("__non_configurable_" + key);
      if (ncIt != obj->properties.end()) configurable = false;
      descriptor->properties["writable"] = Value(writable);
      descriptor->properties["enumerable"] = Value(enumerable);
      descriptor->properties["configurable"] = Value(configurable);
      return Value(descriptor);
    }

    auto regex = args[0].getGC<Regex>();
    auto getterIt = regex->properties.find("__get_" + key);
    if (getterIt != regex->properties.end() && getterIt->second.isFunction()) {
      descriptor->properties["get"] = getterIt->second;
    }

    auto it = regex->properties.find(key);
    if (it != regex->properties.end()) {
      descriptor->properties["value"] = it->second;
    } else if (key == "source") {
      descriptor->properties["value"] = Value(regex->pattern);
    } else if (key == "flags") {
      descriptor->properties["value"] = Value(regex->flags);
    } else if (getterIt == regex->properties.end()) {
      return Value(Undefined{});
    }

    if (key == "lastIndex") {
      descriptor->properties["writable"] = Value(true);
      descriptor->properties["enumerable"] = Value(false);
      descriptor->properties["configurable"] = Value(false);
    } else {
      descriptor->properties["writable"] = Value(true);
      descriptor->properties["enumerable"] = Value(true);
      descriptor->properties["configurable"] = Value(true);
    }
    return Value(descriptor);
  };
  objectConstructor->properties["getOwnPropertyDescriptor"] = Value(objectGetOwnPropertyDescriptor);

  // Object.getOwnPropertyDescriptors (plural)
  auto objectGetOwnPropertyDescriptors = GarbageCollector::makeGC<Function>();
  objectGetOwnPropertyDescriptors->isNative = true;
  objectGetOwnPropertyDescriptors->nativeFunc = [objectGetOwnPropertyDescriptor](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Undefined{});
    auto result = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    // Collect keys from the target object
    OrderedMap<std::string, Value>* props = nullptr;
    if (args[0].isObject()) {
      props = &args[0].getGC<Object>()->properties;
    } else if (args[0].isFunction()) {
      props = &args[0].getGC<Function>()->properties;
    } else if (args[0].isClass()) {
      props = &args[0].getGC<Class>()->properties;
    } else if (args[0].isArray()) {
      props = &args[0].getGC<Array>()->properties;
    }
    if (props) {
      for (const auto& key : props->orderedKeys()) {
        // Skip internal properties
        if (key.size() >= 4 && key.substr(0, 2) == "__" && key.substr(key.size() - 2) == "__") continue;
        if (key.size() > 6 && (key.substr(0, 6) == "__get_" || key.substr(0, 6) == "__set_")) continue;
        if (key.size() > 10 && key.substr(0, 10) == "__non_enum") continue;
        if (key.size() > 14 && key.substr(0, 14) == "__non_writable") continue;
        if (key.size() > 18 && key.substr(0, 18) == "__non_configurable") continue;
        if (key.size() > 7 && key.substr(0, 7) == "__enum_") continue;
        Value desc = objectGetOwnPropertyDescriptor->nativeFunc({args[0], Value(key)});
        if (!desc.isUndefined()) {
          result->properties[key] = desc;
        }
      }
    }
    return Value(result);
  };
  objectConstructor->properties["getOwnPropertyDescriptors"] = Value(objectGetOwnPropertyDescriptors);

  // Object.defineProperty - define property with descriptor
  auto objectDefineProperty = GarbageCollector::makeGC<Function>();
  objectDefineProperty->isNative = true;
  objectDefineProperty->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 3 ||
        (!args[0].isObject() && !args[0].isFunction() && !args[0].isPromise() && !args[0].isRegex() && !args[0].isArray())) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    std::string key = valueToPropertyKey(args[1]);

    if (args[2].isObject()) {
      auto descriptor = args[2].getGC<Object>();
      auto readDescriptorField = [&](const std::string& name) -> std::optional<Value> {
        auto it = descriptor->properties.find(name);
        if (it != descriptor->properties.end()) {
          return it->second;
        }
        if (descriptor->shape) {
          int offset = descriptor->shape->getPropertyOffset(name);
          if (offset >= 0) {
            Value slotValue;
            if (descriptor->getSlot(offset, slotValue)) {
              return slotValue;
            }
          }
        }
        return std::nullopt;
      };
      if (args[0].isObject()) {
        auto obj = args[0].getGC<Object>();
        if (obj->isModuleNamespace) {
          if (!defineModuleNamespaceProperty(obj, key, descriptor)) {
            throw std::runtime_error("TypeError: Cannot redefine module namespace property");
          }
          return args[0];
        }

        if (obj->frozen) {
          return args[0];
        }
        if (obj->sealed && obj->properties.find(key) == obj->properties.end()) {
          return args[0];
        }

        bool hadExistingProperty =
          (obj->properties.find(key) != obj->properties.end()) ||
          (obj->properties.find("__get_" + key) != obj->properties.end()) ||
          (obj->properties.find("__set_" + key) != obj->properties.end());

        auto valueField = readDescriptorField("value");
        auto getField = readDescriptorField("get");
        auto setField = readDescriptorField("set");
        bool hasValueField = valueField.has_value();
        bool hasGetField = getField.has_value();
        bool hasSetField = setField.has_value();

        if (hasValueField) {
          obj->properties[key] = *valueField;
        }

        if (hasGetField && getField->isFunction()) {
          obj->properties["__get_" + key] = *getField;
        }

        if (hasSetField && setField->isFunction()) {
          obj->properties["__set_" + key] = *setField;
        }

        // Defining a new data property with attributes-only descriptor
        // still creates the property with value `undefined`.
        if (!hadExistingProperty && !hasValueField && !hasGetField && !hasSetField) {
          obj->properties[key] = Value(Undefined{});
        }

        // Ensure accessor-only properties have a visible key for enumeration.
        // The getter/setter takes priority in member access, so this placeholder
        // value is only used for property enumeration (for-in, Object.keys, etc.).
        if (!hadExistingProperty && !hasValueField && (hasGetField || hasSetField)) {
          if (obj->properties.find(key) == obj->properties.end()) {
            obj->properties[key] = Value(Undefined{});
          }
        }

        // Handle writable descriptor
        auto writableField = readDescriptorField("writable");
        auto enumField = readDescriptorField("enumerable");
        auto configField = readDescriptorField("configurable");

        if (writableField.has_value()) {
          if (!writableField->toBool()) {
            obj->properties["__non_writable_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_writable_" + key);
          }
        }

        // Handle enumerable descriptor
        if (enumField.has_value()) {
          if (!enumField->toBool()) {
            obj->properties["__non_enum_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_enum_" + key);
          }
        }

        // Handle configurable descriptor
        if (configField.has_value()) {
          if (!configField->toBool()) {
            obj->properties["__non_configurable_" + key] = Value(true);
          } else {
            obj->properties.erase("__non_configurable_" + key);
          }
        }

        // Defaults: for new properties, unspecified attributes default to false.
        if (!hadExistingProperty) {
          bool isAccessorDescriptor = hasGetField || hasSetField;
          bool isDataDescriptor = !isAccessorDescriptor &&
                                  (hasValueField || writableField.has_value() || (!hasGetField && !hasSetField));
          if (isDataDescriptor && !writableField.has_value()) {
            obj->properties["__non_writable_" + key] = Value(true);
          }
          if (!enumField.has_value()) {
            obj->properties["__non_enum_" + key] = Value(true);
          }
          if (!configField.has_value()) {
            obj->properties["__non_configurable_" + key] = Value(true);
          }
        }
      } else if (args[0].isFunction()) {
        auto fn = args[0].getGC<Function>();
        if (auto valueField = readDescriptorField("value"); valueField.has_value()) {
          fn->properties[key] = *valueField;
        }

        if (auto getField = readDescriptorField("get");
            getField.has_value() && getField->isFunction()) {
          fn->properties["__get_" + key] = *getField;
        }

        if (auto setField = readDescriptorField("set");
            setField.has_value() && setField->isFunction()) {
          fn->properties["__set_" + key] = *setField;
        }
      } else if (args[0].isPromise()) {
        auto promise = args[0].getGC<Promise>();
        if (auto valueField = readDescriptorField("value"); valueField.has_value()) {
          promise->properties[key] = *valueField;
        }

        if (auto getField = readDescriptorField("get");
            getField.has_value() && getField->isFunction()) {
          promise->properties["__get_" + key] = *getField;
        }

        if (auto setField = readDescriptorField("set");
            setField.has_value() && setField->isFunction()) {
          promise->properties["__set_" + key] = *setField;
        }
      } else if (args[0].isArray()) {
        auto arr = args[0].getGC<Array>();
        bool isArgumentsObject = false;
        auto isArgsIt = arr->properties.find("__is_arguments_object__");
        if (isArgsIt != arr->properties.end() && isArgsIt->second.isBool() && isArgsIt->second.toBool()) {
          isArgumentsObject = true;
        }
        auto isCanonicalArrayIndex = [&](const std::string& s) -> bool {
          if (s.empty()) return false;
          if (s.size() > 1 && s[0] == '0') return false;
          for (unsigned char c : s) {
            if (std::isdigit(c) == 0) return false;
          }
          return true;
        };
        bool isIndexKey = isCanonicalArrayIndex(key);
        bool hadMappedArgumentsBinding =
          isArgumentsObject && isIndexKey &&
          (arr->properties.find("__mapped_arg_index_" + key + "__") != arr->properties.end());

        auto valueField = readDescriptorField("value");
        auto getField = readDescriptorField("get");
        auto setField = readDescriptorField("set");
        auto writableField = readDescriptorField("writable");
        auto enumField = readDescriptorField("enumerable");
        auto configField = readDescriptorField("configurable");

        bool isAccessorDescriptor = getField.has_value() || setField.has_value();
        bool makeNonWritable = writableField.has_value() && !writableField->toBool();

        // DefineOwnProperty invariants (minimal): reject invalid redefinitions of
        // non-configurable properties so Test262's define-failure tests pass.
        auto sameValue = [](const Value& a, const Value& b) -> bool {
          if (a.data.index() != b.data.index()) return false;
          if (a.isNumber() && b.isNumber()) {
            double x = a.toNumber();
            double y = b.toNumber();
            if (std::isnan(x) && std::isnan(y)) return true;
            if (x == 0.0 && y == 0.0) return std::signbit(x) == std::signbit(y);
            return x == y;
          }
          if (a.isUndefined() && b.isUndefined()) return true;
          if (a.isNull() && b.isNull()) return true;
          if (a.isBool() && b.isBool()) return std::get<bool>(a.data) == std::get<bool>(b.data);
          if (a.isString() && b.isString()) return std::get<std::string>(a.data) == std::get<std::string>(b.data);
          if (a.isBigInt() && b.isBigInt()) return a.toBigInt() == b.toBigInt();
          if (a.isSymbol() && b.isSymbol()) return std::get<Symbol>(a.data) == std::get<Symbol>(b.data);
          // Objects compare by reference identity in SameValue.
          if (a.isFunction() && b.isFunction()) return a.getGC<Function>().get() == b.getGC<Function>().get();
          if (a.isArray() && b.isArray()) return a.getGC<Array>().get() == b.getGC<Array>().get();
          if (a.isObject() && b.isObject()) return a.getGC<Object>().get() == b.getGC<Object>().get();
          if (a.isClass() && b.isClass()) return a.getGC<Class>().get() == b.getGC<Class>().get();
          if (a.isRegex() && b.isRegex()) return a.getGC<Regex>().get() == b.getGC<Regex>().get();
          if (a.isPromise() && b.isPromise()) return a.getGC<Promise>().get() == b.getGC<Promise>().get();
          return false;
        };

        bool currentNonConfigurable = arr->properties.find("__non_configurable_" + key) != arr->properties.end();
        bool currentEnumerable = arr->properties.find("__non_enum_" + key) == arr->properties.end();
        bool currentWritable = arr->properties.find("__non_writable_" + key) == arr->properties.end();
        bool currentIsMapped = hadMappedArgumentsBinding;
        bool currentHasAccessor =
          arr->properties.find("__get_" + key) != arr->properties.end() ||
          arr->properties.find("__set_" + key) != arr->properties.end();
        bool currentIsAccessor = currentHasAccessor && !currentIsMapped;

        auto getCurrentIndexValue = [&]() -> Value {
          if (currentIsMapped) {
            auto it = arr->properties.find("__get_" + key);
            if (it != arr->properties.end() && it->second.isFunction()) {
              auto fn = it->second.getGC<Function>();
              return fn->nativeFunc({});
            }
          }
          bool isNumeric = true;
          size_t idx = 0;
          try { idx = std::stoul(key); } catch (...) { isNumeric = false; }
          if (isNumeric && idx < arr->elements.size()) {
            return arr->elements[idx];
          }
          auto it = arr->properties.find(key);
          if (it != arr->properties.end()) return it->second;
          return Value(Undefined{});
        };

        if (currentNonConfigurable) {
          if (configField.has_value() && configField->toBool()) {
            throw std::runtime_error("TypeError: Cannot redefine property");
          }
          if (enumField.has_value() && enumField->toBool() != currentEnumerable) {
            throw std::runtime_error("TypeError: Cannot redefine property");
          }
          if (currentIsAccessor) {
            if (valueField.has_value() || writableField.has_value()) {
              throw std::runtime_error("TypeError: Cannot redefine property");
            }
          } else {
            if (isAccessorDescriptor) {
              throw std::runtime_error("TypeError: Cannot redefine property");
            }
            if (!currentWritable) {
              if (writableField.has_value() && writableField->toBool()) {
                throw std::runtime_error("TypeError: Cannot redefine property");
              }
              if (valueField.has_value()) {
                Value cur = getCurrentIndexValue();
                if (!sameValue(cur, *valueField)) {
                  throw std::runtime_error("TypeError: Cannot redefine property");
                }
              }
            }
          }
        }

        // Arguments exotic object: redefining a mapped index with an accessor removes mapping.
        if (hadMappedArgumentsBinding && isAccessorDescriptor) {
          arr->properties.erase("__get_" + key);
          arr->properties.erase("__set_" + key);
          arr->properties.erase("__mapped_arg_index_" + key + "__");
          hadMappedArgumentsBinding = false;
        }

        if (valueField.has_value()) {
          // For numeric keys, set in elements array; for others, use properties
          bool isNumeric = true;
          size_t idx = 0;
          try { idx = std::stoul(key); } catch (...) { isNumeric = false; }
          // If this is a mapped arguments index, keep the parameter binding in sync.
          if (hadMappedArgumentsBinding && isNumeric) {
            auto setIt = arr->properties.find("__set_" + key);
            if (setIt != arr->properties.end() && setIt->second.isFunction()) {
              auto fn = setIt->second.getGC<Function>();
              fn->nativeFunc({*valueField});
            }
          }
          if (isNumeric && idx < arr->elements.size()) {
            arr->elements[idx] = *valueField;
          } else {
            arr->properties[key] = *valueField;
          }
        }

        if (getField.has_value() && getField->isFunction()) {
          arr->properties["__get_" + key] = *getField;
          // For numeric indices, ensure elements array covers this index
          // so iteration sees it (getter takes priority over element value)
          bool isNumIdx = true;
          size_t gIdx = 0;
          try { gIdx = std::stoul(key); } catch (...) { isNumIdx = false; }
          if (isNumIdx && gIdx >= arr->elements.size()) {
            arr->elements.resize(gIdx + 1, Value(Undefined{}));
          }
        }

        if (setField.has_value() && setField->isFunction()) {
          arr->properties["__set_" + key] = *setField;
        }

        // Ensure non-index accessor-only properties have a visible key for enumeration.
        if (!isIndexKey && !valueField.has_value() && (getField.has_value() || setField.has_value())) {
          if (arr->properties.find(key) == arr->properties.end()) {
            arr->properties[key] = Value(Undefined{});
          }
        }

        // Handle writable descriptor
        if (writableField.has_value()) {
          if (!writableField->toBool()) {
            arr->properties["__non_writable_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_writable_" + key);
          }
        }

        // Handle enumerable descriptor
        if (enumField.has_value()) {
          if (!enumField->toBool()) {
            arr->properties["__non_enum_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_enum_" + key);
          }
        }

        // Handle configurable descriptor
        if (configField.has_value()) {
          if (!configField->toBool()) {
            arr->properties["__non_configurable_" + key] = Value(true);
          } else {
            arr->properties.erase("__non_configurable_" + key);
          }
        }

        // Arguments exotic object: certain descriptor changes remove the parameter mapping.
        if (hadMappedArgumentsBinding) {
          if (makeNonWritable) {
            // Snapshot the current mapped value into the actual index property
            // before removing the mapping (value may have been updated via param assignment).
            if (!valueField.has_value()) {
              Value cur = getCurrentIndexValue();
              bool isNumeric = true;
              size_t idx = 0;
              try { idx = std::stoul(key); } catch (...) { isNumeric = false; }
              if (isNumeric && idx < arr->elements.size()) {
                arr->elements[idx] = cur;
              } else if (isNumeric) {
                arr->properties[key] = cur;
              }
            }
            arr->properties.erase("__mapped_arg_index_" + key + "__");
            arr->properties.erase("__get_" + key);
            arr->properties.erase("__set_" + key);
          }
        }
      } else if (args[0].isRegex()) {
        auto regex = args[0].getGC<Regex>();
        if (auto valueField = readDescriptorField("value"); valueField.has_value()) {
          regex->properties[key] = *valueField;
        }

        if (auto getField = readDescriptorField("get");
            getField.has_value() && getField->isFunction()) {
          regex->properties["__get_" + key] = *getField;
        }

        if (auto setField = readDescriptorField("set");
            setField.has_value() && setField->isFunction()) {
          regex->properties["__set_" + key] = *setField;
        }
      }
    }
    return args[0];
  };
  objectConstructor->properties["defineProperty"] = Value(objectDefineProperty);

  // Object.defineProperties - define multiple properties
  auto objectDefineProperties = GarbageCollector::makeGC<Function>();
  objectDefineProperties->isNative = true;
  objectDefineProperties->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.size() < 2 || !args[0].isObject() || !args[1].isObject()) {
      return args.empty() ? Value(Undefined{}) : args[0];
    }
    auto obj = args[0].getGC<Object>();
    auto props = args[1].getGC<Object>();

    if (obj->frozen) {
      return args[0];
    }

    for (const auto& [key, descriptor] : props->properties) {
      if (obj->sealed && obj->properties.find(key) == obj->properties.end()) {
        continue;  // Skip new properties on sealed object
      }
      if (!descriptor.isObject()) {
        continue;
      }

      auto descObj = descriptor.getGC<Object>();
      auto readDescriptorField = [&](const std::string& name) -> std::optional<Value> {
        auto it = descObj->properties.find(name);
        if (it != descObj->properties.end()) {
          return it->second;
        }
        if (descObj->shape) {
          int offset = descObj->shape->getPropertyOffset(name);
          if (offset >= 0) {
            Value slotValue;
            if (descObj->getSlot(offset, slotValue)) {
              return slotValue;
            }
          }
        }
        return std::nullopt;
      };

      bool hadExistingProperty =
        (obj->properties.find(key) != obj->properties.end()) ||
        (obj->properties.find("__get_" + key) != obj->properties.end()) ||
        (obj->properties.find("__set_" + key) != obj->properties.end());

      auto valueField = readDescriptorField("value");
      auto getField = readDescriptorField("get");
      auto setField = readDescriptorField("set");
      bool hasValueField = valueField.has_value();
      bool hasGetField = getField.has_value();
      bool hasSetField = setField.has_value();

      if (hasValueField) {
        obj->properties[key] = *valueField;
      }
      if (hasGetField && getField->isFunction()) {
        obj->properties["__get_" + key] = *getField;
      }
      if (hasSetField && setField->isFunction()) {
        obj->properties["__set_" + key] = *setField;
      }
      if (!hadExistingProperty && !hasValueField && !hasGetField && !hasSetField) {
        obj->properties[key] = Value(Undefined{});
      }

      if (auto writableField = readDescriptorField("writable"); writableField.has_value()) {
        if (!writableField->toBool()) {
          obj->properties["__non_writable_" + key] = Value(true);
        } else {
          obj->properties.erase("__non_writable_" + key);
        }
      }
      if (auto enumField = readDescriptorField("enumerable"); enumField.has_value()) {
        if (!enumField->toBool()) {
          obj->properties["__non_enum_" + key] = Value(true);
        } else {
          obj->properties.erase("__non_enum_" + key);
        }
      }
      if (auto configField = readDescriptorField("configurable"); configField.has_value()) {
        if (!configField->toBool()) {
          obj->properties["__non_configurable_" + key] = Value(true);
        } else {
          obj->properties.erase("__non_configurable_" + key);
        }
      }
    }
    return args[0];
  };
  objectConstructor->properties["defineProperties"] = Value(objectDefineProperties);

  env->define("Object", Value(objectConstructor));

  // Math object
  auto mathObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Math constants
  auto defineMathConst = [&](const std::string& name, double value) {
    mathObj->properties[name] = Value(value);
    mathObj->properties["__non_writable_" + name] = Value(true);
    mathObj->properties["__non_enum_" + name] = Value(true);
    mathObj->properties["__non_configurable_" + name] = Value(true);
  };
  defineMathConst("PI", 3.141592653589793);
  defineMathConst("E", 2.718281828459045);
  defineMathConst("LN2", 0.6931471805599453);
  defineMathConst("LN10", 2.302585092994046);
  defineMathConst("LOG2E", 1.4426950408889634);
  defineMathConst("LOG10E", 0.4342944819032518);
  defineMathConst("SQRT1_2", 0.7071067811865476);
  defineMathConst("SQRT2", 1.4142135623730951);

  // Math methods
  auto mathAbs = GarbageCollector::makeGC<Function>();
  mathAbs->isNative = true;
  mathAbs->nativeFunc = Math_abs;
  mathObj->properties["abs"] = Value(mathAbs);

  auto mathCeil = GarbageCollector::makeGC<Function>();
  mathCeil->isNative = true;
  mathCeil->nativeFunc = Math_ceil;
  mathObj->properties["ceil"] = Value(mathCeil);

  auto mathFloor = GarbageCollector::makeGC<Function>();
  mathFloor->isNative = true;
  mathFloor->nativeFunc = Math_floor;
  mathObj->properties["floor"] = Value(mathFloor);

  auto mathRound = GarbageCollector::makeGC<Function>();
  mathRound->isNative = true;
  mathRound->nativeFunc = Math_round;
  mathObj->properties["round"] = Value(mathRound);

  auto mathTrunc = GarbageCollector::makeGC<Function>();
  mathTrunc->isNative = true;
  mathTrunc->nativeFunc = Math_trunc;
  mathObj->properties["trunc"] = Value(mathTrunc);

  auto mathMax = GarbageCollector::makeGC<Function>();
  mathMax->isNative = true;
  mathMax->nativeFunc = Math_max;
  mathObj->properties["max"] = Value(mathMax);

  auto mathMin = GarbageCollector::makeGC<Function>();
  mathMin->isNative = true;
  mathMin->nativeFunc = Math_min;
  mathObj->properties["min"] = Value(mathMin);

  auto mathPow = GarbageCollector::makeGC<Function>();
  mathPow->isNative = true;
  mathPow->nativeFunc = Math_pow;
  mathObj->properties["pow"] = Value(mathPow);

  auto mathSqrt = GarbageCollector::makeGC<Function>();
  mathSqrt->isNative = true;
  mathSqrt->nativeFunc = Math_sqrt;
  mathObj->properties["sqrt"] = Value(mathSqrt);

  auto mathSin = GarbageCollector::makeGC<Function>();
  mathSin->isNative = true;
  mathSin->nativeFunc = Math_sin;
  mathObj->properties["sin"] = Value(mathSin);

  auto mathCos = GarbageCollector::makeGC<Function>();
  mathCos->isNative = true;
  mathCos->nativeFunc = Math_cos;
  mathObj->properties["cos"] = Value(mathCos);

  auto mathTan = GarbageCollector::makeGC<Function>();
  mathTan->isNative = true;
  mathTan->nativeFunc = Math_tan;
  mathObj->properties["tan"] = Value(mathTan);

  auto mathRandom = GarbageCollector::makeGC<Function>();
  mathRandom->isNative = true;
  mathRandom->nativeFunc = Math_random;
  mathObj->properties["random"] = Value(mathRandom);

  auto mathSign = GarbageCollector::makeGC<Function>();
  mathSign->isNative = true;
  mathSign->nativeFunc = Math_sign;
  mathObj->properties["sign"] = Value(mathSign);

  auto mathLog = GarbageCollector::makeGC<Function>();
  mathLog->isNative = true;
  mathLog->nativeFunc = Math_log;
  mathObj->properties["log"] = Value(mathLog);

  auto mathLog10 = GarbageCollector::makeGC<Function>();
  mathLog10->isNative = true;
  mathLog10->nativeFunc = Math_log10;
  mathObj->properties["log10"] = Value(mathLog10);

  auto mathExp = GarbageCollector::makeGC<Function>();
  mathExp->isNative = true;
  mathExp->nativeFunc = Math_exp;
  mathObj->properties["exp"] = Value(mathExp);

  auto mathCbrt = GarbageCollector::makeGC<Function>();
  mathCbrt->isNative = true;
  mathCbrt->nativeFunc = Math_cbrt;
  mathObj->properties["cbrt"] = Value(mathCbrt);

  auto mathLog2 = GarbageCollector::makeGC<Function>();
  mathLog2->isNative = true;
  mathLog2->nativeFunc = Math_log2;
  mathObj->properties["log2"] = Value(mathLog2);

  auto mathHypot = GarbageCollector::makeGC<Function>();
  mathHypot->isNative = true;
  mathHypot->nativeFunc = Math_hypot;
  mathObj->properties["hypot"] = Value(mathHypot);

  auto mathExpm1 = GarbageCollector::makeGC<Function>();
  mathExpm1->isNative = true;
  mathExpm1->nativeFunc = Math_expm1;
  mathObj->properties["expm1"] = Value(mathExpm1);

  auto mathLog1p = GarbageCollector::makeGC<Function>();
  mathLog1p->isNative = true;
  mathLog1p->nativeFunc = Math_log1p;
  mathObj->properties["log1p"] = Value(mathLog1p);

  auto mathFround = GarbageCollector::makeGC<Function>();
  mathFround->isNative = true;
  mathFround->nativeFunc = Math_fround;
  mathObj->properties["fround"] = Value(mathFround);

  auto mathClz32 = GarbageCollector::makeGC<Function>();
  mathClz32->isNative = true;
  mathClz32->nativeFunc = Math_clz32;
  mathObj->properties["clz32"] = Value(mathClz32);

  auto mathImul = GarbageCollector::makeGC<Function>();
  mathImul->isNative = true;
  mathImul->nativeFunc = Math_imul;
  mathObj->properties["imul"] = Value(mathImul);

  auto registerMathFn = [&](const std::string& name, std::function<Value(const std::vector<Value>&)> fn, int length = 1) {
    auto f = GarbageCollector::makeGC<Function>();
    f->isNative = true;
    f->nativeFunc = fn;
    f->properties["name"] = Value(name);
    f->properties["length"] = Value(static_cast<double>(length));
    f->properties["__non_writable_name"] = Value(true);
    f->properties["__non_configurable_name"] = Value(true);
    f->properties["__non_writable_length"] = Value(true);
    f->properties["__non_configurable_length"] = Value(true);
    mathObj->properties[name] = Value(f);
    mathObj->properties["__non_enum_" + name] = Value(true);
  };
  registerMathFn("asin", Math_asin);
  registerMathFn("acos", Math_acos);
  registerMathFn("atan", Math_atan);
  registerMathFn("atan2", Math_atan2, 2);
  registerMathFn("sinh", Math_sinh);
  registerMathFn("cosh", Math_cosh);
  registerMathFn("tanh", Math_tanh);
  registerMathFn("asinh", Math_asinh);
  registerMathFn("acosh", Math_acosh);
  registerMathFn("atanh", Math_atanh);

  env->define("Math", Value(mathObj));

  // Date constructor
  auto dateConstructor = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  dateConstructor->isNative = true;
  dateConstructor->isConstructor = true;
  dateConstructor->properties["name"] = Value(std::string("Date"));
  dateConstructor->properties["length"] = Value(1.0);
  // `Date()` called as a function returns a string (not a Date object).
  // Construction is handled via `__native_construct__` below.
  dateConstructor->nativeFunc = [](const std::vector<Value>&) -> Value {
    return Value(std::string("[object Date]"));
  };

  auto dateConstruct = GarbageCollector::makeGC<Function>();
  GarbageCollector::instance().reportAllocation(sizeof(Function));
  dateConstruct->isNative = true;
  dateConstruct->isConstructor = true;
  dateConstruct->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.size() == 1) {
      Value primitive = toPrimitive(args[0], false);
      if (primitive.isSymbol() || primitive.isBigInt()) {
        throw std::runtime_error("TypeError: Cannot convert to number");
      }
      if (primitive.isString()) {
        return Date_constructor({primitive});
      }
      return Date_constructor({Value(primitive.toNumber())});
    }
    return Date_constructor(args);
  };
  dateConstructor->properties["__native_construct__"] = Value(dateConstruct);

  {
    auto datePrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    dateConstructor->properties["prototype"] = Value(datePrototype);
    datePrototype->properties["constructor"] = Value(dateConstructor);
    datePrototype->properties["name"] = Value(std::string("Date"));
  }

  // Date static methods
  auto dateNow = GarbageCollector::makeGC<Function>();
  dateNow->isNative = true;
  dateNow->nativeFunc = Date_now;
  dateConstructor->properties["now"] = Value(dateNow);

  auto dateParse = GarbageCollector::makeGC<Function>();
  dateParse->isNative = true;
  dateParse->nativeFunc = Date_parse;
  dateConstructor->properties["parse"] = Value(dateParse);

  env->define("Date", Value(dateConstructor));

  // String constructor with static methods
  auto stringConstructorFn = GarbageCollector::makeGC<Function>();
  stringConstructorFn->isNative = true;
  stringConstructorFn->isConstructor = true;
  stringConstructorFn->properties["__wrap_primitive__"] = Value(true);
  stringConstructorFn->nativeFunc = [toPrimitive](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(std::string(""));
    }
    Value primitive = toPrimitive(args[0], true);
    if (primitive.isNumber()) {
      double value = primitive.toNumber();
      if (std::isnan(value)) return Value(std::string("NaN"));
      if (std::isinf(value)) return Value(std::string(value < 0 ? "-Infinity" : "Infinity"));
      if (value == 0.0) return Value(std::string("0"));

      double integral = std::trunc(value);
      if (integral == value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(0) << value;
        return Value(oss.str());
      }

      std::ostringstream oss;
      oss << std::setprecision(15) << value;
      std::string out = oss.str();
      auto expPos = out.find_first_of("eE");
      if (expPos != std::string::npos) {
        std::string mantissa = out.substr(0, expPos);
        std::string exponent = out.substr(expPos + 1);
        char sign = '\0';
        size_t idx = 0;
        if (!exponent.empty() && (exponent[0] == '+' || exponent[0] == '-')) {
          sign = exponent[0];
          idx = 1;
        }
        while (idx < exponent.size() && exponent[idx] == '0') idx++;
        std::string expDigits = (idx < exponent.size()) ? exponent.substr(idx) : "0";
        out = mantissa + "e";
        if (sign == '-') out += "-";
        out += expDigits;
      } else {
        auto dot = out.find('.');
        if (dot != std::string::npos) {
          while (!out.empty() && out.back() == '0') out.pop_back();
          if (!out.empty() && out.back() == '.') out.pop_back();
        }
      }
      return Value(out);
    }
    return Value(primitive.toString());
  };

  // Wrap in an Object to hold static methods
  auto stringConstructorObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // The constructor itself
  stringConstructorObj->properties["__callable_object__"] = Value(true);
  stringConstructorObj->properties["__constructor_wrapper__"] = Value(true);
  stringConstructorObj->properties["constructor"] = Value(stringConstructorFn);

  // String.fromCharCode
  auto fromCharCode = GarbageCollector::makeGC<Function>();
  fromCharCode->isNative = true;
  fromCharCode->nativeFunc = String_fromCharCode;
  stringConstructorObj->properties["fromCharCode"] = Value(fromCharCode);

  // String.fromCodePoint
  auto fromCodePoint = GarbageCollector::makeGC<Function>();
  fromCodePoint->isNative = true;
  fromCodePoint->nativeFunc = String_fromCodePoint;
  stringConstructorObj->properties["fromCodePoint"] = Value(fromCodePoint);

  // String.raw
  auto stringRaw = GarbageCollector::makeGC<Function>();
  stringRaw->isNative = true;
  stringRaw->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || (!args[0].isObject() && !args[0].isArray())) {
      return Value(std::string(""));
    }
    // Get the template object's raw property
    GCPtr<Array> rawArr;
    if (args[0].isObject()) {
      auto obj = args[0].getGC<Object>();
      auto rawIt = obj->properties.find("raw");
      if (rawIt != obj->properties.end() && rawIt->second.isArray()) {
        rawArr = rawIt->second.getGC<Array>();
      }
    }
    if (!rawArr) return Value(std::string(""));

    std::string result;
    size_t literalCount = rawArr->elements.size();
    for (size_t i = 0; i < literalCount; i++) {
      result += rawArr->elements[i].toString();
      if (i + 1 < literalCount && i + 1 < args.size()) {
        result += args[i + 1].toString();
      }
    }
    return Value(result);
  };
  stringConstructorObj->properties["raw"] = Value(stringRaw);

  auto stringPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto stringMatchAll = GarbageCollector::makeGC<Function>();
  stringMatchAll->isNative = true;
  stringMatchAll->isConstructor = false;
  stringMatchAll->properties["name"] = Value(std::string("matchAll"));
  stringMatchAll->properties["length"] = Value(1.0);
  stringMatchAll->properties["__uses_this_arg__"] = Value(true);
  stringMatchAll->properties["__throw_on_new__"] = Value(true);
  stringMatchAll->nativeFunc = [regExpPrototype](const std::vector<Value>& args) -> Value {
    if (args.empty() || args[0].isUndefined() || args[0].isNull()) {
      throw std::runtime_error("TypeError: String.prototype.matchAll called on null or undefined");
    }

    Value thisValue = args[0];
    Value regexp = args.size() > 1 ? args[1] : Value(Undefined{});
    Interpreter* interpreter = getGlobalInterpreter();
    const std::string& matchAllKey = WellKnownSymbols::matchAllKey();

    auto callChecked = [&](const Value& callee,
                           const std::vector<Value>& callArgs,
                           const Value& thisArg) -> Value {
      if (!callee.isFunction()) {
        return Value(Undefined{});
      }

      if (interpreter) {
        Value out = interpreter->callForHarness(callee, callArgs, thisArg);
        if (interpreter->hasError()) {
          Value err = interpreter->getError();
          interpreter->clearError();
          throw std::runtime_error(err.toString());
        }
        return out;
      }

      auto fn = callee.getGC<Function>();
      if (fn->isNative) {
        auto itUsesThis = fn->properties.find("__uses_this_arg__");
        if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.reserve(callArgs.size() + 1);
          nativeArgs.push_back(thisArg);
          nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
          return fn->nativeFunc(nativeArgs);
        }
        return fn->nativeFunc(callArgs);
      }

      return Value(Undefined{});
    };

    auto getObjectValue = [&](const GCPtr<Object>& obj,
                              const Value& thisArg,
                              const std::string& key) -> Value {
      std::string getterName = "__get_" + key;
      auto getterIt = obj->properties.find(getterName);
      if (getterIt != obj->properties.end() && getterIt->second.isFunction()) {
        return callChecked(getterIt->second, {}, thisArg);
      }

      auto it = obj->properties.find(key);
      if (it != obj->properties.end()) {
        return it->second;
      }
      return Value(Undefined{});
    };

    auto getRegexValue = [&](const GCPtr<Regex>& regex,
                             const std::string& key) -> Value {
      Value regexValue(regex);

      std::string getterName = "__get_" + key;
      auto getterIt = regex->properties.find(getterName);
      if (getterIt != regex->properties.end() && getterIt->second.isFunction()) {
        return callChecked(getterIt->second, {}, regexValue);
      }

      auto it = regex->properties.find(key);
      if (it != regex->properties.end()) {
        return it->second;
      }

      if (regExpPrototype) {
        std::string protoGetterName = "__get_" + key;
        auto protoGetterIt = regExpPrototype->properties.find(protoGetterName);
        if (protoGetterIt != regExpPrototype->properties.end() && protoGetterIt->second.isFunction()) {
          return callChecked(protoGetterIt->second, {}, regexValue);
        }

        auto protoIt = regExpPrototype->properties.find(key);
        if (protoIt != regExpPrototype->properties.end()) {
          return protoIt->second;
        }
      }

      if (key == "source") {
        return Value(regex->pattern);
      }
      if (key == "flags") {
        return Value(regex->flags);
      }

      return Value(Undefined{});
    };

    std::string input;
    if (thisValue.isObject()) {
      auto obj = thisValue.getGC<Object>();
      Value toPrimitive = Value(Undefined{});
      auto toPrimitiveIt = obj->properties.find(WellKnownSymbols::toPrimitiveKey());
      if (toPrimitiveIt != obj->properties.end()) {
        toPrimitive = toPrimitiveIt->second;
      } else {
        auto fallbackIt = obj->properties.find("undefined");
        if (fallbackIt != obj->properties.end()) {
          toPrimitive = fallbackIt->second;
        }
      }
      if (toPrimitive.isFunction()) {
        input = callChecked(toPrimitive, {}, thisValue).toString();
      } else {
        input = thisValue.toString();
      }
    } else {
      input = thisValue.toString();
    }

    if (!regexp.isUndefined() && !regexp.isNull()) {
      if (regexp.isRegex()) {
        auto regex = regexp.getGC<Regex>();
        Value flagsValue = getRegexValue(regex, "flags");
        if (flagsValue.isUndefined() || flagsValue.isNull()) {
          throw std::runtime_error("TypeError: RegExp flags is undefined or null");
        }
        if (flagsValue.toString().find('g') == std::string::npos) {
          throw std::runtime_error("TypeError: String.prototype.matchAll requires a global RegExp");
        }
      }

      Value matcher(Undefined{});
      if (regexp.isRegex()) {
        matcher = getRegexValue(regexp.getGC<Regex>(), matchAllKey);
      } else if (regexp.isObject()) {
        matcher = getObjectValue(regexp.getGC<Object>(), regexp, matchAllKey);
      } else if (regexp.isFunction()) {
        auto fn = regexp.getGC<Function>();
        auto it = fn->properties.find(matchAllKey);
        if (it != fn->properties.end()) {
          matcher = it->second;
        }
      }

      if (!matcher.isUndefined() && !matcher.isNull()) {
        if (!matcher.isFunction()) {
          throw std::runtime_error("TypeError: @@matchAll is not callable");
        }
        return callChecked(matcher, {Value(input)}, regexp);
      }
    }

    std::string pattern;
    if (regexp.isRegex()) {
      pattern = regexp.getGC<Regex>()->pattern;
    } else if (regexp.isUndefined()) {
      pattern = "";  // RegExp(undefined) uses empty pattern
    } else {
      pattern = regexp.toString();
    }

    auto rx = GarbageCollector::makeGC<Regex>(pattern, "g");
    Value rxValue(rx);
    Value matcher = getRegexValue(GCPtr<Regex>(rx), matchAllKey);
    if (!matcher.isFunction()) {
      throw std::runtime_error("TypeError: RegExp @@matchAll is not callable");
    }
    return callChecked(matcher, {Value(input)}, rxValue);
  };
  stringPrototype->properties["matchAll"] = Value(stringMatchAll);
  stringPrototype->properties["__non_enum_matchAll"] = Value(true);
  stringConstructorObj->properties["prototype"] = Value(stringPrototype);
  stringPrototype->properties["constructor"] = Value(stringConstructorObj);
  stringPrototype->properties["__non_enum_constructor"] = Value(true);
  stringPrototype->properties["__proto__"] = Value(objectPrototype);

  // For simplicity, we can make the Object callable by storing the function
  env->define("String", Value(stringConstructorObj));

  // WebAssembly global object
  env->define("WebAssembly", wasm_js::createWebAssemblyGlobal());

  // globalThis - reference to the global object
  // Create a proxy object that reflects the current global environment
  auto globalThisObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Copy all current global bindings into globalThis
  for (const auto& [name, value] : env->bindings_) {
    globalThisObj->properties[name] = value;
  }
  // Expose selected intrinsics as configurable global properties.
  // (We keep the intrinsic itself off `globalThis` to avoid leaking internal names.)
  if (auto intrinsicJSON = env->get("__intrinsic_JSON__")) {
    globalThisObj->properties["JSON"] = *intrinsicJSON;
    globalThisObj->properties.erase("__intrinsic_JSON__");
  }
  // Immutable global properties (not declarative bindings).
  globalThisObj->properties["undefined"] = Value(Undefined{});
  globalThisObj->properties["Infinity"] = Value(std::numeric_limits<double>::infinity());
  globalThisObj->properties["NaN"] = Value(std::numeric_limits<double>::quiet_NaN());
  const char* immutableGlobalNames[] = {"undefined", "Infinity", "NaN"};
  for (const char* name : immutableGlobalNames) {
    globalThisObj->properties[std::string("__non_writable_") + name] = Value(true);
    globalThisObj->properties[std::string("__non_configurable_") + name] = Value(true);
    globalThisObj->properties[std::string("__non_enum_") + name] = Value(true);
  }

  // Define globalThis pointing to the global object
  env->define("globalThis", Value(globalThisObj));
  // 'this' at global scope should be globalThis
  env->define("this", Value(globalThisObj));
  env->define("__var_scope__", Value(true), true);

  // Also add globalThis to itself
  globalThisObj->properties["globalThis"] = Value(globalThisObj);

  // Timer functions - setTimeout
  auto setTimeoutFn = GarbageCollector::makeGC<Function>();
  setTimeoutFn->isNative = true;
  setTimeoutFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = args[0].getGC<Function>();
    int64_t delayMs = args.size() > 1 ? static_cast<int64_t>(args[1].toNumber()) : 0;

    // Create a timer callback that executes the JS function
    auto& loop = EventLoopContext::instance().getLoop();
    TimerId id = loop.setTimeout([callback]() -> Value {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        return callback->nativeFunc({});
      }
      // For non-native functions, we can't easily execute them here
      // This will be improved when we integrate with the interpreter
      return Value(Undefined{});
    }, delayMs);

    return Value(static_cast<double>(id));
  };
  env->define("setTimeout", Value(setTimeoutFn));

  // Timer functions - setInterval
  auto setIntervalFn = GarbageCollector::makeGC<Function>();
  setIntervalFn->isNative = true;
  setIntervalFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = args[0].getGC<Function>();
    int64_t intervalMs = args.size() > 1 ? static_cast<int64_t>(args[1].toNumber()) : 0;

    // Create an interval timer callback
    auto& loop = EventLoopContext::instance().getLoop();
    TimerId id = loop.setInterval([callback]() -> Value {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        return callback->nativeFunc({});
      }
      return Value(Undefined{});
    }, intervalMs);

    return Value(static_cast<double>(id));
  };
  env->define("setInterval", Value(setIntervalFn));

  // Timer functions - clearTimeout
  auto clearTimeoutFn = GarbageCollector::makeGC<Function>();
  clearTimeoutFn->isNative = true;
  clearTimeoutFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    TimerId id = static_cast<TimerId>(args[0].toNumber());
    auto& loop = EventLoopContext::instance().getLoop();
    loop.clearTimer(id);

    return Value(Undefined{});
  };
  env->define("clearTimeout", Value(clearTimeoutFn));

  // Timer functions - clearInterval (same implementation as clearTimeout)
  auto clearIntervalFn = GarbageCollector::makeGC<Function>();
  clearIntervalFn->isNative = true;
  clearIntervalFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) {
      return Value(Undefined{});
    }

    TimerId id = static_cast<TimerId>(args[0].toNumber());
    auto& loop = EventLoopContext::instance().getLoop();
    loop.clearTimer(id);

    return Value(Undefined{});
  };
  env->define("clearInterval", Value(clearIntervalFn));

  // queueMicrotask function
  auto queueMicrotaskFn = GarbageCollector::makeGC<Function>();
  queueMicrotaskFn->isNative = true;
  queueMicrotaskFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      return Value(Undefined{});
    }

    auto callback = args[0].getGC<Function>();

    // Queue the microtask
    auto& loop = EventLoopContext::instance().getLoop();
    loop.queueMicrotask([callback]() {
      // Execute the callback function
      if (callback->isNative && callback->nativeFunc) {
        callback->nativeFunc({});
      }
    });

    return Value(Undefined{});
  };
  env->define("queueMicrotask", Value(queueMicrotaskFn));

  // TextEncoder and TextDecoder
  env->define("TextEncoder", Value(createTextEncoderConstructor()));
  env->define("TextDecoder", Value(createTextDecoderConstructor()));

  // URL and URLSearchParams
  env->define("URL", Value(createURLConstructor()));
  env->define("URLSearchParams", Value(createURLSearchParamsConstructor()));

  // AbortController and AbortSignal
  auto abortControllerCtor = GarbageCollector::makeGC<Function>();
  abortControllerCtor->isNative = true;
  abortControllerCtor->isConstructor = true;
  abortControllerCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto controller = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));

    // Create AbortSignal
    auto signal = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(false);
    signal->properties["reason"] = Value(Undefined{});

    // Event listeners storage
    auto listeners = GarbageCollector::makeGC<Array>();
    signal->properties["_listeners"] = Value(listeners);

    // addEventListener method
    auto addEventListenerFn = GarbageCollector::makeGC<Function>();
    addEventListenerFn->isNative = true;
    addEventListenerFn->nativeFunc = [listeners](const std::vector<Value>& args) -> Value {
      if (args.size() >= 2 && args[0].toString() == "abort" && args[1].isFunction()) {
        listeners->elements.push_back(args[1]);
      }
      return Value(Undefined{});
    };
    signal->properties["addEventListener"] = Value(addEventListenerFn);

    // removeEventListener method
    auto removeEventListenerFn = GarbageCollector::makeGC<Function>();
    removeEventListenerFn->isNative = true;
    removeEventListenerFn->nativeFunc = [listeners](const std::vector<Value>& args) -> Value {
      // Simple implementation - just mark for removal
      return Value(Undefined{});
    };
    signal->properties["removeEventListener"] = Value(removeEventListenerFn);

    controller->properties["signal"] = Value(signal);

    // abort method
    auto abortFn = GarbageCollector::makeGC<Function>();
    abortFn->isNative = true;
    abortFn->nativeFunc = [signal, listeners](const std::vector<Value>& args) -> Value {
      // Check if already aborted
      if (signal->properties["aborted"].toBool()) {
        return Value(Undefined{});
      }

      signal->properties["aborted"] = Value(true);
      signal->properties["reason"] = args.empty() ?
          Value(std::string("AbortError: The operation was aborted")) : args[0];

      // Call all abort listeners
      for (const auto& listener : listeners->elements) {
        if (listener.isFunction()) {
          auto fn = listener.getGC<Function>();
          if (fn->isNative && fn->nativeFunc) {
            fn->nativeFunc({});
          }
        }
      }

      return Value(Undefined{});
    };
    controller->properties["abort"] = Value(abortFn);

    return Value(controller);
  };
  env->define("AbortController", Value(abortControllerCtor));

  // AbortSignal.abort() static method
  auto abortSignalObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  auto abortStaticFn = GarbageCollector::makeGC<Function>();
  abortStaticFn->isNative = true;
  abortStaticFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto signal = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(true);
    signal->properties["reason"] = args.empty() ?
        Value(std::string("AbortError: The operation was aborted")) : args[0];
    return Value(signal);
  };
  abortSignalObj->properties["abort"] = Value(abortStaticFn);

  // AbortSignal.timeout() static method
  auto timeoutStaticFn = GarbageCollector::makeGC<Function>();
  timeoutStaticFn->isNative = true;
  timeoutStaticFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto signal = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    signal->properties["aborted"] = Value(false);
    signal->properties["reason"] = Value(Undefined{});
    // Note: Actual timeout implementation would require event loop integration
    return Value(signal);
  };
  abortSignalObj->properties["timeout"] = Value(timeoutStaticFn);

  env->define("AbortSignal", Value(abortSignalObj));

  // Streams API - ReadableStream
  auto readableStreamCtor = GarbageCollector::makeGC<Function>();
  readableStreamCtor->isNative = true;
  readableStreamCtor->isConstructor = true;
  readableStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create underlying source callbacks from argument
    GCPtr<Function> startFn = {};
    GCPtr<Function> pullFn = {};
    GCPtr<Function> cancelFn = {};
    double highWaterMark = 1.0;

    if (!args.empty() && args[0].isObject()) {
      auto srcObj = args[0].getGC<Object>();

      // Get start callback
      auto startIt = srcObj->properties.find("start");
      if (startIt != srcObj->properties.end() && startIt->second.isFunction()) {
        startFn = startIt->second.getGC<Function>();
      }

      // Get pull callback
      auto pullIt = srcObj->properties.find("pull");
      if (pullIt != srcObj->properties.end() && pullIt->second.isFunction()) {
        pullFn = pullIt->second.getGC<Function>();
      }

      // Get cancel callback
      auto cancelIt = srcObj->properties.find("cancel");
      if (cancelIt != srcObj->properties.end() && cancelIt->second.isFunction()) {
        cancelFn = cancelIt->second.getGC<Function>();
      }
    }

    // Create the stream
    auto stream = createReadableStream(startFn, pullFn, cancelFn, highWaterMark);
    return Value(stream);
  };
  env->define("ReadableStream", Value(readableStreamCtor));

  // Streams API - WritableStream
  auto writableStreamCtor = GarbageCollector::makeGC<Function>();
  writableStreamCtor->isNative = true;
  writableStreamCtor->isConstructor = true;
  writableStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create underlying sink callbacks from argument
    GCPtr<Function> startFn = {};
    GCPtr<Function> writeFn = {};
    GCPtr<Function> closeFn = {};
    GCPtr<Function> abortFn = {};
    double highWaterMark = 1.0;

    if (!args.empty() && args[0].isObject()) {
      auto sinkObj = args[0].getGC<Object>();

      // Get start callback
      auto startIt = sinkObj->properties.find("start");
      if (startIt != sinkObj->properties.end() && startIt->second.isFunction()) {
        startFn = startIt->second.getGC<Function>();
      }

      // Get write callback
      auto writeIt = sinkObj->properties.find("write");
      if (writeIt != sinkObj->properties.end() && writeIt->second.isFunction()) {
        writeFn = writeIt->second.getGC<Function>();
      }

      // Get close callback
      auto closeIt = sinkObj->properties.find("close");
      if (closeIt != sinkObj->properties.end() && closeIt->second.isFunction()) {
        closeFn = closeIt->second.getGC<Function>();
      }

      // Get abort callback
      auto abortIt = sinkObj->properties.find("abort");
      if (abortIt != sinkObj->properties.end() && abortIt->second.isFunction()) {
        abortFn = abortIt->second.getGC<Function>();
      }
    }

    // Create the stream
    auto stream = createWritableStream(startFn, writeFn, closeFn, abortFn, highWaterMark);
    return Value(stream);
  };
  env->define("WritableStream", Value(writableStreamCtor));

  // Streams API - TransformStream
  auto transformStreamCtor = GarbageCollector::makeGC<Function>();
  transformStreamCtor->isNative = true;
  transformStreamCtor->isConstructor = true;
  transformStreamCtor->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // Create transformer callbacks from argument
    GCPtr<Function> startFn = {};
    GCPtr<Function> transformFn = {};
    GCPtr<Function> flushFn = {};

    if (!args.empty() && args[0].isObject()) {
      auto transformerObj = args[0].getGC<Object>();

      // Get start callback
      auto startIt = transformerObj->properties.find("start");
      if (startIt != transformerObj->properties.end() && startIt->second.isFunction()) {
        startFn = startIt->second.getGC<Function>();
      }

      // Get transform callback
      auto transformIt = transformerObj->properties.find("transform");
      if (transformIt != transformerObj->properties.end() && transformIt->second.isFunction()) {
        transformFn = transformIt->second.getGC<Function>();
      }

      // Get flush callback
      auto flushIt = transformerObj->properties.find("flush");
      if (flushIt != transformerObj->properties.end() && flushIt->second.isFunction()) {
        flushFn = flushIt->second.getGC<Function>();
      }
    }

    // Create the stream
    auto stream = createTransformStream(startFn, transformFn, flushFn);
    return Value(stream);
  };
  env->define("TransformStream", Value(transformStreamCtor));

  // File System module (fs)
  globalThisObj->properties["fs"] = Value(createFSModule());

  // performance.now() - high-resolution timing
  static auto startTime = std::chrono::steady_clock::now();

  auto performanceNowFn = GarbageCollector::makeGC<Function>();
  performanceNowFn->isNative = true;
  performanceNowFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - startTime).count();
    return Value(static_cast<double>(elapsed) / 1000.0);  // Return milliseconds
  };

  auto performanceObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  performanceObj->properties["now"] = Value(performanceNowFn);
  env->define("performance", Value(performanceObj));
  globalThisObj->properties["performance"] = Value(performanceObj);

  // structuredClone - deep clone objects, arrays, and primitives
  // Use heap-owned recursion so callable lifetime remains valid after this setup scope.
  auto deepClone = std::make_shared<std::function<Value(const Value&)>>();
  *deepClone = [deepClone](const Value& val) -> Value {
    // Primitives are returned as-is (they're already copies)
    if (val.isUndefined() || val.isNull() || val.isBool() ||
        val.isNumber() || val.isString() || val.isBigInt() || val.isSymbol()) {
      return val;
    }

    // Clone arrays
    if (val.isArray()) {
      auto arr = val.getGC<Array>();
      auto newArr = GarbageCollector::makeGC<Array>();
      GarbageCollector::instance().reportAllocation(sizeof(Array));
      for (const auto& elem : arr->elements) {
        newArr->elements.push_back((*deepClone)(elem));
      }
      return Value(newArr);
    }

    // Clone objects
    if (val.isObject()) {
      auto obj = val.getGC<Object>();
      auto newObj = GarbageCollector::makeGC<Object>();
      GarbageCollector::instance().reportAllocation(sizeof(Object));
      for (const auto& [key, value] : obj->properties) {
        newObj->properties[key] = (*deepClone)(value);
      }
      return Value(newObj);
    }

    // Functions, Promises, etc. cannot be cloned - return as-is
    return val;
  };

  auto structuredCloneFn = GarbageCollector::makeGC<Function>();
  structuredCloneFn->isNative = true;
  structuredCloneFn->nativeFunc = [deepClone](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(Undefined{});
    return (*deepClone)(args[0]);
  };
  env->define("structuredClone", Value(structuredCloneFn));
  globalThisObj->properties["structuredClone"] = Value(structuredCloneFn);

  // Base64 encoding table
  static const char base64Chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  // btoa - encode string to Base64
  auto btoaFn = GarbageCollector::makeGC<Function>();
  btoaFn->isNative = true;
  btoaFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();
    std::string result;
    result.reserve((input.size() + 2) / 3 * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
      uint32_t n = static_cast<uint8_t>(input[i]) << 16;
      if (i + 1 < input.size()) n |= static_cast<uint8_t>(input[i + 1]) << 8;
      if (i + 2 < input.size()) n |= static_cast<uint8_t>(input[i + 2]);

      result += base64Chars[(n >> 18) & 0x3F];
      result += base64Chars[(n >> 12) & 0x3F];
      result += (i + 1 < input.size()) ? base64Chars[(n >> 6) & 0x3F] : '=';
      result += (i + 2 < input.size()) ? base64Chars[n & 0x3F] : '=';
    }
    return Value(result);
  };
  env->define("btoa", Value(btoaFn));
  globalThisObj->properties["btoa"] = Value(btoaFn);

  // atob - decode Base64 to string
  auto atobFn = GarbageCollector::makeGC<Function>();
  atobFn->isNative = true;
  atobFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string(""));
    std::string input = args[0].toString();

    // Build decode lookup table
    static int decodeTable[256] = {-1};
    static bool tableInit = false;
    if (!tableInit) {
      for (int i = 0; i < 256; ++i) decodeTable[i] = -1;
      for (int i = 0; i < 64; ++i) decodeTable[static_cast<uint8_t>(base64Chars[i])] = i;
      tableInit = true;
    }

    std::string result;
    result.reserve(input.size() * 3 / 4);

    int bits = 0;
    int bitCount = 0;
    for (char c : input) {
      if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
      int value = decodeTable[static_cast<uint8_t>(c)];
      if (value == -1) continue;  // Skip invalid characters

      bits = (bits << 6) | value;
      bitCount += 6;

      if (bitCount >= 8) {
        bitCount -= 8;
        result += static_cast<char>((bits >> bitCount) & 0xFF);
      }
    }
    return Value(result);
  };
  env->define("atob", Value(atobFn));
  globalThisObj->properties["atob"] = Value(atobFn);

  // encodeURIComponent - encode URI component
  auto encodeURIComponentFn = GarbageCollector::makeGC<Function>();
  encodeURIComponentFn->isNative = true;
  encodeURIComponentFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size() * 3);  // Worst case

    for (unsigned char c : input) {
      // Unreserved characters (RFC 3986): A-Z a-z 0-9 - _ . ~
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
        result += static_cast<char>(c);
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        result += hex;
      }
    }
    return Value(result);
  };
  env->define("encodeURIComponent", Value(encodeURIComponentFn));
  globalThisObj->properties["encodeURIComponent"] = Value(encodeURIComponentFn);

  // decodeURIComponent - decode URI component
  auto decodeURIComponentFn = GarbageCollector::makeGC<Function>();
  decodeURIComponentFn->isNative = true;
  decodeURIComponentFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '%' && i + 2 < input.size()) {
        int value = 0;
        if (std::sscanf(input.c_str() + i + 1, "%2x", &value) == 1) {
          result += static_cast<char>(value);
          i += 2;
          continue;
        }
      }
      result += input[i];
    }
    return Value(result);
  };
  env->define("decodeURIComponent", Value(decodeURIComponentFn));
  globalThisObj->properties["decodeURIComponent"] = Value(decodeURIComponentFn);

  // encodeURI - encode full URI (leaves more characters unencoded)
  auto encodeURIFn = GarbageCollector::makeGC<Function>();
  encodeURIFn->isNative = true;
  encodeURIFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size() * 3);

    for (unsigned char c : input) {
      // Reserved and unreserved characters that should NOT be encoded in full URI
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
          c == ':' || c == '/' || c == '?' || c == '#' || c == '[' || c == ']' ||
          c == '@' || c == '!' || c == '$' || c == '&' || c == '\'' ||
          c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == ';' || c == '=') {
        result += static_cast<char>(c);
      } else {
        char hex[4];
        snprintf(hex, sizeof(hex), "%%%02X", c);
        result += hex;
      }
    }
    return Value(result);
  };
  env->define("encodeURI", Value(encodeURIFn));
  globalThisObj->properties["encodeURI"] = Value(encodeURIFn);

  // decodeURI - decode full URI
  auto decodeURIFn = GarbageCollector::makeGC<Function>();
  decodeURIFn->isNative = true;
  decodeURIFn->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty()) return Value(std::string("undefined"));
    std::string input = args[0].toString();
    std::string result;
    result.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
      if (input[i] == '%' && i + 2 < input.size()) {
        int value = 0;
        if (std::sscanf(input.c_str() + i + 1, "%2x", &value) == 1) {
          result += static_cast<char>(value);
          i += 2;
          continue;
        }
      }
      result += input[i];
    }
    return Value(result);
  };
  env->define("decodeURI", Value(decodeURIFn));
  globalThisObj->properties["decodeURI"] = Value(decodeURIFn);

  // ===== Function constructor =====
  auto functionConstructor = GarbageCollector::makeGC<Function>();
  functionConstructor->isNative = true;
  functionConstructor->isConstructor = true;
  functionConstructor->properties["name"] = Value(std::string("Function"));
  functionConstructor->properties["length"] = Value(1.0);
  functionConstructor->nativeFunc = [env, objectPrototype](const std::vector<Value>& args) -> Value {
    std::vector<std::string> params;
    std::string body;

    if (!args.empty()) {
      for (size_t i = 0; i + 1 < args.size(); ++i) {
        params.push_back(args[i].toString());
      }
      body = args.back().toString();
    }

    std::string source = "function anonymous(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0) source += ",";
      source += params[i];
    }
    source += ") {\n";
    source += body;
    source += "\n}";

    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
      if (tokens[i].type == TokenType::Import &&
          !tokens[i].escaped &&
          tokens[i + 1].type == TokenType::Dot &&
          tokens[i + 2].type == TokenType::Identifier &&
          !tokens[i + 2].escaped &&
          tokens[i + 2].value == "meta") {
        throw std::runtime_error("SyntaxError: Cannot use import.meta outside a module");
      }
    }
    Parser parser(tokens);
    auto program = parser.parse();
    if (!program || program->body.empty()) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto compiledProgram = std::make_shared<Program>(std::move(*program));
    auto* fnDecl = std::get_if<FunctionDeclaration>(&compiledProgram->body[0]->node);
    if (!fnDecl) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto fn = GarbageCollector::makeGC<Function>();
    fn->isNative = false;
    fn->isAsync = fnDecl->isAsync;
    fn->isGenerator = fnDecl->isGenerator;
    fn->isConstructor = true;
    fn->closure = env;

    auto hasUseStrictDirective = [](const std::vector<StmtPtr>& bodyStmts) -> bool {
      for (const auto& stmt : bodyStmts) {
        if (!stmt) break;
        auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
        if (!exprStmt || !exprStmt->expression) break;
        auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node);
        if (!str) break;
        if (str->value == "use strict") return true;
      }
      return false;
    };
    fn->isStrict = hasUseStrictDirective(fnDecl->body);

    for (const auto& param : fnDecl->params) {
      FunctionParam funcParam;
      funcParam.name = param.name.name;
      if (param.defaultValue) {
        funcParam.defaultValue = std::shared_ptr<void>(
          const_cast<Expression*>(param.defaultValue.get()),
          [](void*) {});
      }
      fn->params.push_back(funcParam);
    }

    if (fnDecl->restParam.has_value()) {
      fn->restParam = fnDecl->restParam->name;
    }

    fn->body = std::shared_ptr<void>(
      const_cast<std::vector<StmtPtr>*>(&fnDecl->body),
      [](void*) {});
    fn->astOwner = compiledProgram;
    fn->properties["name"] = Value(std::string("anonymous"));
    fn->properties["length"] = Value(static_cast<double>(fn->params.size()));

    auto fnPrototype = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    fnPrototype->properties["constructor"] = Value(fn);
    fnPrototype->properties["__proto__"] = Value(objectPrototype);
    fn->properties["prototype"] = Value(fnPrototype);

    // Set __proto__ to Function.prototype for proper prototype chain
    auto funcVal = env->get("Function");
    if (funcVal.has_value() && funcVal->isFunction()) {
      auto funcCtor = std::get<GCPtr<Function>>(funcVal->data);
      auto protoIt = funcCtor->properties.find("prototype");
      if (protoIt != funcCtor->properties.end()) {
        fn->properties["__proto__"] = protoIt->second;
      }
    }

    return Value(fn);
  };

  // Function.prototype - a minimal prototype with call/apply/bind
  // Per spec, Function.prototype is itself callable (it's a function that accepts any arguments and returns undefined)
  auto functionPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  functionPrototype->properties["__proto__"] = Value(objectPrototype);
  functionPrototype->properties["__callable_object__"] = Value(true);

  auto fpToString = GarbageCollector::makeGC<Function>();
  fpToString->isNative = true;
  fpToString->properties["name"] = Value(std::string("toString"));
  fpToString->properties["length"] = Value(0.0);
  fpToString->properties["__uses_this_arg__"] = Value(true);
  fpToString->nativeFunc = [](const std::vector<Value>&) -> Value {
    return Value(std::string("[Function]"));
  };
  functionPrototype->properties["toString"] = Value(fpToString);
  functionPrototype->properties["__non_enum_toString"] = Value(true);

  // Function.prototype.call - uses __uses_this_arg__ so args[0] = this (the function to call)
  auto fpCall = GarbageCollector::makeGC<Function>();
  fpCall->isNative = true;
  fpCall->properties["name"] = Value(std::string("call"));
  fpCall->properties["length"] = Value(1.0);
  fpCall->properties["__uses_this_arg__"] = Value(true);
  fpCall->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // args[0] = this (the function to invoke), args[1] = thisArg, args[2+] = call args
    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Function.prototype.call called on non-function");
    }
    auto fn = args[0].getGC<Function>();
    Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> callArgs;
    if (args.size() > 2) {
      callArgs.insert(callArgs.end(), args.begin() + 2, args.end());
    }
    if (fn->isNative) {
      // For native functions that use this_arg, prepend thisArg
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    return interpreter->callForHarness(Value(fn), callArgs, thisArg);
  };
  functionPrototype->properties["call"] = Value(fpCall);
  functionPrototype->properties["__non_enum_call"] = Value(true);

  // Function.prototype.apply
  auto fpApply = GarbageCollector::makeGC<Function>();
  fpApply->isNative = true;
  fpApply->properties["name"] = Value(std::string("apply"));
  fpApply->properties["length"] = Value(2.0);
  fpApply->properties["__uses_this_arg__"] = Value(true);
  fpApply->nativeFunc = [](const std::vector<Value>& args) -> Value {
    if (args.empty() || !args[0].isFunction()) {
      throw std::runtime_error("TypeError: Function.prototype.apply called on non-function");
    }
    auto fn = args[0].getGC<Function>();
    Value thisArg = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> callArgs;
    if (args.size() > 2 && args[2].isArray()) {
      auto arr = args[2].getGC<Array>();
      callArgs = arr->elements;
    }
    if (fn->isNative) {
      auto itUsesThis = fn->properties.find("__uses_this_arg__");
      if (itUsesThis != fn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
        std::vector<Value> nativeArgs;
        nativeArgs.push_back(thisArg);
        nativeArgs.insert(nativeArgs.end(), callArgs.begin(), callArgs.end());
        return fn->nativeFunc(nativeArgs);
      }
      return fn->nativeFunc(callArgs);
    }
    Interpreter* interpreter = getGlobalInterpreter();
    if (!interpreter) {
      throw std::runtime_error("TypeError: Interpreter unavailable");
    }
    return interpreter->callForHarness(Value(fn), callArgs, thisArg);
  };
  functionPrototype->properties["apply"] = Value(fpApply);
  functionPrototype->properties["__non_enum_apply"] = Value(true);

  // Function.prototype.bind
  auto fpBind = GarbageCollector::makeGC<Function>();
  fpBind->isNative = true;
  fpBind->properties["name"] = Value(std::string("bind"));
  fpBind->properties["length"] = Value(1.0);
  fpBind->properties["__uses_this_arg__"] = Value(true);
  fpBind->nativeFunc = [](const std::vector<Value>& args) -> Value {
    // args[0] = this (the function to bind), args[1] = boundThis, args[2+] = bound args
    if (args.empty() || (!args[0].isFunction() && !args[0].isClass())) {
      throw std::runtime_error("TypeError: Function.prototype.bind called on non-function");
    }
    Value target = args[0];
    GCPtr<Function> targetFn;
    GCPtr<Class> targetCls;
    bool targetIsConstructor = false;
    if (target.isFunction()) {
      targetFn = target.getGC<Function>();
      targetIsConstructor = targetFn && targetFn->isConstructor;
    } else if (target.isClass()) {
      targetCls = target.getGC<Class>();
      targetIsConstructor = true;
    }
    Value boundThis = args.size() > 1 ? args[1] : Value(Undefined{});
    std::vector<Value> boundArgs;
    if (args.size() > 2) {
      boundArgs.insert(boundArgs.end(), args.begin() + 2, args.end());
    }
    auto boundFn = GarbageCollector::makeGC<Function>();
    boundFn->isNative = true;
    boundFn->isConstructor = targetIsConstructor;
    boundFn->properties["__bound_target__"] = target;
    boundFn->properties["__bound_this__"] = boundThis;
    auto boundArgsArr = GarbageCollector::makeGC<Array>();
    boundArgsArr->elements = boundArgs;
    boundFn->properties["__bound_args__"] = Value(boundArgsArr);

    std::string targetName;
    if (targetFn) {
      targetName = targetFn->properties.count("name") ? targetFn->properties["name"].toString() : "";
    } else if (targetCls) {
      auto it = targetCls->properties.find("name");
      if (it != targetCls->properties.end() && it->second.isString()) {
        targetName = it->second.toString();
      } else {
        targetName = targetCls->name;
      }
    }
    boundFn->properties["name"] = Value(std::string("bound " + targetName));
    boundFn->nativeFunc = [target, boundThis, boundArgs](const std::vector<Value>& callArgs) -> Value {
      // [[Call]] of a bound function: call target with boundThis and boundArgs + callArgs.
      // Bound class constructors are not callable without 'new'.
      if (target.isClass()) {
        throw std::runtime_error("TypeError: Class constructor cannot be invoked without 'new'");
      }
      auto targetFn = target.getGC<Function>();
      if (!targetFn) {
        throw std::runtime_error("TypeError: Bound target is not callable");
      }
      std::vector<Value> finalArgs = boundArgs;
      finalArgs.insert(finalArgs.end(), callArgs.begin(), callArgs.end());
      if (targetFn->isNative) {
        auto itUsesThis = targetFn->properties.find("__uses_this_arg__");
        if (itUsesThis != targetFn->properties.end() && itUsesThis->second.isBool() && itUsesThis->second.toBool()) {
          std::vector<Value> nativeArgs;
          nativeArgs.push_back(boundThis);
          nativeArgs.insert(nativeArgs.end(), finalArgs.begin(), finalArgs.end());
          return targetFn->nativeFunc(nativeArgs);
        }
        return targetFn->nativeFunc(finalArgs);
      }
      Interpreter* interpreter = getGlobalInterpreter();
      if (!interpreter) {
        throw std::runtime_error("TypeError: Interpreter unavailable");
      }
      return interpreter->callForHarness(Value(targetFn), finalArgs, boundThis);
    };
    return Value(boundFn);
  };
  functionPrototype->properties["bind"] = Value(fpBind);
  functionPrototype->properties["__non_enum_bind"] = Value(true);

  // %FunctionPrototype%.caller / .arguments are restricted in strict mode and
  // for classes. Model as ThrowTypeError accessors.
  auto throwTypeErrorAccessor = GarbageCollector::makeGC<Function>();
  throwTypeErrorAccessor->isNative = true;
  throwTypeErrorAccessor->nativeFunc = [](const std::vector<Value>&) -> Value {
    throw std::runtime_error("TypeError: 'caller', 'callee', and 'arguments' properties may not be accessed");
  };
  functionPrototype->properties["__get_caller"] = Value(throwTypeErrorAccessor);
  functionPrototype->properties["__set_caller"] = Value(throwTypeErrorAccessor);
  functionPrototype->properties["__get_arguments"] = Value(throwTypeErrorAccessor);
  functionPrototype->properties["__set_arguments"] = Value(throwTypeErrorAccessor);
  functionPrototype->properties["__non_enum_caller"] = Value(true);
  functionPrototype->properties["__non_enum_arguments"] = Value(true);

  functionPrototype->properties["constructor"] = Value(functionConstructor);
  functionPrototype->properties["__non_enum_constructor"] = Value(true);
  functionConstructor->properties["prototype"] = Value(functionPrototype);
  // Set Function constructor's own __proto__ to Function.prototype
  functionConstructor->properties["__proto__"] = Value(functionPrototype);
  env->define("Function", Value(functionConstructor));
  globalThisObj->properties["Function"] = Value(functionConstructor);

  // Generator function intrinsics setup
  auto generatorFunctionPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  auto generatorPrototype = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));
  
  // %GeneratorFunction.prototype% inherits from Function.prototype
  generatorFunctionPrototype->properties["__proto__"] = Value(functionPrototype);
  generatorFunctionPrototype->properties["__get_caller"] = Value(throwTypeErrorAccessor);
  generatorFunctionPrototype->properties["__set_caller"] = Value(throwTypeErrorAccessor);
  generatorFunctionPrototype->properties["__get_arguments"] = Value(throwTypeErrorAccessor);
  generatorFunctionPrototype->properties["__set_arguments"] = Value(throwTypeErrorAccessor);
  generatorFunctionPrototype->properties["__non_enum_caller"] = Value(true);
  generatorFunctionPrototype->properties["__non_enum_arguments"] = Value(true);
  // %GeneratorFunction.prototype.prototype% is %GeneratorPrototype%
  generatorFunctionPrototype->properties["prototype"] = Value(generatorPrototype);
  // %GeneratorPrototype% inherits from Object.prototype
  if (auto objCtor = env->get("Object"); objCtor && objCtor->isFunction()) {
    auto fn = std::get<GCPtr<Function>>(objCtor->data);
    auto protoIt = fn->properties.find("prototype");
    if (protoIt != fn->properties.end()) {
      generatorPrototype->properties["__proto__"] = protoIt->second;
    }
  }
  // %GeneratorPrototype%.constructor is %GeneratorFunction.prototype%
  generatorPrototype->properties["constructor"] = Value(generatorFunctionPrototype);

  // GeneratorFunction constructor (not a global binding; reachable via
  // Object.getPrototypeOf(function*(){}).constructor)
  auto generatorFunctionConstructor = GarbageCollector::makeGC<Function>();
  generatorFunctionConstructor->isNative = true;
  generatorFunctionConstructor->isConstructor = true;
  generatorFunctionConstructor->properties["name"] = Value(std::string("GeneratorFunction"));
  generatorFunctionConstructor->properties["length"] = Value(1.0);
  // %GeneratorFunction% inherits from %Function%.
  generatorFunctionConstructor->properties["__proto__"] = Value(functionPrototype);
  generatorFunctionConstructor->nativeFunc = [env, generatorFunctionPrototype, generatorPrototype](const std::vector<Value>& args) -> Value {
    std::vector<std::string> params;
    std::string body;

    if (!args.empty()) {
      for (size_t i = 0; i + 1 < args.size(); ++i) {
        params.push_back(args[i].toString());
      }
      body = args.back().toString();
    }

    std::string source = "function* anonymous(";
    for (size_t i = 0; i < params.size(); ++i) {
      if (i > 0) source += ",";
      source += params[i];
    }
    source += ") {\n";
    source += body;
    source += "\n}";

    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
      if (tokens[i].type == TokenType::Import &&
          !tokens[i].escaped &&
          tokens[i + 1].type == TokenType::Dot &&
          tokens[i + 2].type == TokenType::Identifier &&
          !tokens[i + 2].escaped &&
          tokens[i + 2].value == "meta") {
        throw std::runtime_error("SyntaxError: Cannot use import.meta outside a module");
      }
    }
    Parser parser(tokens);
    auto program = parser.parse();
    if (!program || program->body.empty()) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto compiledProgram = std::make_shared<Program>(std::move(*program));
    auto* fnDecl = std::get_if<FunctionDeclaration>(&compiledProgram->body[0]->node);
    if (!fnDecl || !fnDecl->isGenerator) {
      throw std::runtime_error("SyntaxError: Function constructor parse error");
    }

    auto fn = GarbageCollector::makeGC<Function>();
    fn->isNative = false;
    fn->isAsync = fnDecl->isAsync;
    fn->isGenerator = fnDecl->isGenerator;
    fn->isConstructor = false;
    fn->closure = env;

    auto hasUseStrictDirective = [](const std::vector<StmtPtr>& bodyStmts) -> bool {
      for (const auto& stmt : bodyStmts) {
        if (!stmt) break;
        auto* exprStmt = std::get_if<ExpressionStmt>(&stmt->node);
        if (!exprStmt || !exprStmt->expression) break;
        auto* str = std::get_if<StringLiteral>(&exprStmt->expression->node);
        if (!str) break;
        if (str->value == "use strict") return true;
      }
      return false;
    };
    fn->isStrict = hasUseStrictDirective(fnDecl->body);

    for (const auto& param : fnDecl->params) {
      FunctionParam funcParam;
      funcParam.name = param.name.name;
      if (param.defaultValue) {
        funcParam.defaultValue = std::shared_ptr<void>(
          const_cast<Expression*>(param.defaultValue.get()),
          [](void*) {});
      }
      fn->params.push_back(funcParam);
    }
    if (fnDecl->restParam.has_value()) {
      fn->restParam = fnDecl->restParam->name;
    }
    fn->body = std::shared_ptr<void>(
      const_cast<std::vector<StmtPtr>*>(&fnDecl->body),
      [](void*) {});
    fn->astOwner = compiledProgram;
    fn->properties["name"] = Value(std::string("anonymous"));
    fn->properties["length"] = Value(static_cast<double>(fn->params.size()));

    // Generator functions have a `.prototype` that is the generator object prototype.
    auto genProtoObj = GarbageCollector::makeGC<Object>();
    GarbageCollector::instance().reportAllocation(sizeof(Object));
    genProtoObj->properties["__proto__"] = Value(generatorPrototype);
    fn->properties["prototype"] = Value(genProtoObj);
    fn->properties["__non_enum_prototype"] = Value(true);
    fn->properties["__non_configurable_prototype"] = Value(true);

    // %GeneratorFunction.prototype% as [[Prototype]] of generator function objects.
    fn->properties["__proto__"] = Value(generatorFunctionPrototype);

    return Value(fn);
  };

  // Wire: GeneratorFunction.prototype and .constructor
  generatorFunctionConstructor->properties["prototype"] = Value(generatorFunctionPrototype);
  generatorFunctionPrototype->properties["constructor"] = Value(generatorFunctionConstructor);
  generatorFunctionPrototype->properties["__non_enum_constructor"] = Value(true);

  // Store these in environment for internal use
  env->define("__generator_function_prototype__", Value(generatorFunctionPrototype));
  env->define("__generator_prototype__", Value(generatorPrototype));

  // ===== Deferred prototype chain setup =====
  // These must happen after all constructors are defined.

  // BigInt.prototype.__proto__ = Object.prototype
  if (auto bigIntCtor = env->get("BigInt"); bigIntCtor && bigIntCtor->isFunction()) {
    auto bigIntFnPtr = std::get<GCPtr<Function>>(bigIntCtor->data);
    auto protoIt = bigIntFnPtr->properties.find("prototype");
    if (protoIt != bigIntFnPtr->properties.end() && protoIt->second.isObject()) {
      auto bigIntProtoPtr = protoIt->second.getGC<Object>();
      bigIntProtoPtr->properties["__proto__"] = Value(objectPrototype);
    }
    // BigInt.__proto__ = Function.prototype
    bigIntFnPtr->properties["__proto__"] = Value(functionPrototype);
  }

  // Promise.prototype.__proto__ = Object.prototype (already set via promisePrototype)
  // Promise.__proto__ = Function.prototype
  if (auto promiseCtor = env->get("Promise"); promiseCtor && promiseCtor->isFunction()) {
    auto promiseFnPtr = std::get<GCPtr<Function>>(promiseCtor->data);
    promiseFnPtr->properties["__proto__"] = Value(functionPrototype);
  }

  // Mark built-in constructors as non-enumerable on globalThis (per ES spec)
  static const char* builtinNames[] = {
    "BigInt", "Object", "Array", "String", "Number", "Boolean", "Symbol",
    "Promise", "RegExp", "Map", "Set", "Proxy", "Reflect", "Error",
    "TypeError", "RangeError", "ReferenceError", "SyntaxError", "URIError",
    "EvalError", "Function", "Date", "Math", "JSON", "console",
    "ArrayBuffer", "DataView", "Int8Array", "Uint8Array", "Uint8ClampedArray",
    "Int16Array", "Uint16Array", "Int32Array", "Uint32Array",
    "Float16Array", "Float32Array", "Float64Array",
    "BigInt64Array", "BigUint64Array", "WeakRef", "FinalizationRegistry",
    "globalThis", "undefined", "NaN", "Infinity",
    "eval", "parseInt", "parseFloat", "isNaN", "isFinite",
    "encodeURIComponent", "decodeURIComponent", "encodeURI", "decodeURI",
    "setTimeout", "clearTimeout", "setInterval", "clearInterval",
    "queueMicrotask", "structuredClone", "btoa", "atob",
    "fetch", "crypto", "WebAssembly", "performance"
  };
  for (const char* name : builtinNames) {
    if (globalThisObj->properties.count(name)) {
      globalThisObj->properties["__non_enum_" + std::string(name)] = Value(true);
    }
  }

  return env;
}

Environment* Environment::getRoot() {
  Environment* current = this;
  while (current->parent_) {
    current = current->parent_.get();
  }
  return current;
}

GCPtr<Object> Environment::getGlobal() const {
  // Walk up to the root environment
  const Environment* current = this;
  while (current->parent_) {
    current = current->parent_.get();
  }

  // Use the live global object when available.
  auto globalThisIt = current->bindings_.find("globalThis");
  if (globalThisIt != current->bindings_.end() && globalThisIt->second.isObject()) {
    return globalThisIt->second.getGC<Object>();
  }

  auto globalObj = GarbageCollector::makeGC<Object>();
  GarbageCollector::instance().reportAllocation(sizeof(Object));

  // Add all global bindings to the object
  for (const auto& [name, value] : current->bindings_) {
    globalObj->properties[name] = value;
  }

  return GCPtr<Object>(globalObj);
}

}
