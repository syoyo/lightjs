#pragma once

#include "module.h"
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "event_loop.h"
#include "json.h"
#include "symbols.h"
#include "fs_compat.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <unordered_set>
#include <algorithm>

namespace lightjs {

namespace {
constexpr const char* kSyntheticDefaultExportBinding = "__lightjs_default_export__";

const char* moduleTypeSuffix(ModuleType type) {
  switch (type) {
    case ModuleType::JavaScript:
      return "js";
    case ModuleType::Json:
      return "json";
    case ModuleType::Bytes:
      return "bytes";
  }
  return "js";
}

ModuleType moduleTypeFromAttributes(const std::vector<ImportAttribute>& attributes) {
  for (const auto& attribute : attributes) {
    if (attribute.key != "type") {
      continue;
    }
    if (attribute.value == "json") {
      return ModuleType::Json;
    }
    if (attribute.value == "bytes") {
      return ModuleType::Bytes;
    }
  }
  return ModuleType::JavaScript;
}

std::string dependencyCacheKey(const std::string& source, ModuleType type) {
  return source + "|" + moduleTypeSuffix(type);
}

Value makeErrorValue(ErrorType type, const std::string& message) {
  return Value(GarbageCollector::makeGC<Error>(type, message));
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

  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    if (!exportDefault->declaration) {
      return true;
    }
    if (auto* fnExpr = std::get_if<FunctionExpr>(&exportDefault->declaration->node)) {
      if (!fnExpr->name.empty()) {
        return addLexicalName(fnExpr->name, lexicalNames, duplicateName);
      }
      return true;
    }
    if (auto* clsExpr = std::get_if<ClassExpr>(&exportDefault->declaration->node)) {
      if (!clsExpr->name.empty()) {
        return addLexicalName(clsExpr->name, lexicalNames, duplicateName);
      }
      return true;
    }
    return true;
  }

  return true;
}

bool validatePrivateNamesInExpression(const Expression& expr,
                                      const std::unordered_set<std::string>& visiblePrivateNames,
                                      std::string& invalidPrivateName);
bool validatePrivateNamesInStatement(const Statement& stmt,
                                     const std::unordered_set<std::string>& visiblePrivateNames,
                                     std::string& invalidPrivateName);

bool validatePrivateNamesInClass(const ExprPtr& superClass,
                                 const std::vector<MethodDefinition>& methods,
                                 const std::unordered_set<std::string>& outerPrivateNames,
                                 std::string& invalidPrivateName) {
  if (superClass &&
      !validatePrivateNamesInExpression(*superClass, outerPrivateNames, invalidPrivateName)) {
    return false;
  }

  std::unordered_set<std::string> classPrivateNames = outerPrivateNames;
  for (const auto& method : methods) {
    if (method.isPrivate && !method.key.name.empty() && method.key.name[0] == '#') {
      classPrivateNames.insert(method.key.name);
    }
  }

  for (const auto& method : methods) {
    if (method.initializer &&
        !validatePrivateNamesInExpression(*method.initializer, classPrivateNames, invalidPrivateName)) {
      return false;
    }
    for (const auto& stmt : method.body) {
      if (stmt &&
          !validatePrivateNamesInStatement(*stmt, classPrivateNames, invalidPrivateName)) {
        return false;
      }
    }
  }

  return true;
}

bool validatePrivateNamesInExpression(const Expression& expr,
                                      const std::unordered_set<std::string>& visiblePrivateNames,
                                      std::string& invalidPrivateName) {
  if (auto* identifier = std::get_if<Identifier>(&expr.node)) {
    if (!identifier->name.empty() && identifier->name[0] == '#' &&
        visiblePrivateNames.count(identifier->name) == 0) {
      invalidPrivateName = identifier->name;
      return false;
    }
    return true;
  }

  if (auto* binary = std::get_if<BinaryExpr>(&expr.node)) {
    return (!binary->left ||
            validatePrivateNamesInExpression(*binary->left, visiblePrivateNames, invalidPrivateName)) &&
           (!binary->right ||
            validatePrivateNamesInExpression(*binary->right, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* unary = std::get_if<UnaryExpr>(&expr.node)) {
    return !unary->argument ||
           validatePrivateNamesInExpression(*unary->argument, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* assignment = std::get_if<AssignmentExpr>(&expr.node)) {
    return (!assignment->left ||
            validatePrivateNamesInExpression(*assignment->left, visiblePrivateNames, invalidPrivateName)) &&
           (!assignment->right ||
            validatePrivateNamesInExpression(*assignment->right, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* update = std::get_if<UpdateExpr>(&expr.node)) {
    return !update->argument ||
           validatePrivateNamesInExpression(*update->argument, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* call = std::get_if<CallExpr>(&expr.node)) {
    if (call->callee &&
        !validatePrivateNamesInExpression(*call->callee, visiblePrivateNames, invalidPrivateName)) {
      return false;
    }
    for (const auto& arg : call->arguments) {
      if (arg &&
          !validatePrivateNamesInExpression(*arg, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* member = std::get_if<MemberExpr>(&expr.node)) {
    if (member->object &&
        !validatePrivateNamesInExpression(*member->object, visiblePrivateNames, invalidPrivateName)) {
      return false;
    }
    if (!member->property) {
      return true;
    }
    if (!member->computed) {
      if (auto* propertyIdentifier = std::get_if<Identifier>(&member->property->node)) {
        if (!propertyIdentifier->name.empty() && propertyIdentifier->name[0] == '#' &&
            visiblePrivateNames.count(propertyIdentifier->name) == 0) {
          invalidPrivateName = propertyIdentifier->name;
          return false;
        }
        return true;
      }
    }
    return validatePrivateNamesInExpression(*member->property, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* conditional = std::get_if<ConditionalExpr>(&expr.node)) {
    return (!conditional->test ||
            validatePrivateNamesInExpression(*conditional->test, visiblePrivateNames, invalidPrivateName)) &&
           (!conditional->consequent ||
            validatePrivateNamesInExpression(*conditional->consequent, visiblePrivateNames, invalidPrivateName)) &&
           (!conditional->alternate ||
            validatePrivateNamesInExpression(*conditional->alternate, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* sequence = std::get_if<SequenceExpr>(&expr.node)) {
    for (const auto& child : sequence->expressions) {
      if (child &&
          !validatePrivateNamesInExpression(*child, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* arrayExpr = std::get_if<ArrayExpr>(&expr.node)) {
    for (const auto& child : arrayExpr->elements) {
      if (child &&
          !validatePrivateNamesInExpression(*child, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* objExpr = std::get_if<ObjectExpr>(&expr.node)) {
    for (const auto& prop : objExpr->properties) {
      if (prop.key &&
          !validatePrivateNamesInExpression(*prop.key, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
      if (prop.value &&
          !validatePrivateNamesInExpression(*prop.value, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* functionExpr = std::get_if<FunctionExpr>(&expr.node)) {
    for (const auto& param : functionExpr->params) {
      if (param.defaultValue &&
          !validatePrivateNamesInExpression(*param.defaultValue, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    for (const auto& stmt : functionExpr->body) {
      if (stmt &&
          !validatePrivateNamesInStatement(*stmt, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* classExpr = std::get_if<ClassExpr>(&expr.node)) {
    return validatePrivateNamesInClass(
      classExpr->superClass,
      classExpr->methods,
      visiblePrivateNames,
      invalidPrivateName);
  }
  if (auto* awaitExpr = std::get_if<AwaitExpr>(&expr.node)) {
    return !awaitExpr->argument ||
           validatePrivateNamesInExpression(*awaitExpr->argument, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* yieldExpr = std::get_if<YieldExpr>(&expr.node)) {
    return !yieldExpr->argument ||
           validatePrivateNamesInExpression(*yieldExpr->argument, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* newExpr = std::get_if<NewExpr>(&expr.node)) {
    if (newExpr->callee &&
        !validatePrivateNamesInExpression(*newExpr->callee, visiblePrivateNames, invalidPrivateName)) {
      return false;
    }
    for (const auto& arg : newExpr->arguments) {
      if (arg &&
          !validatePrivateNamesInExpression(*arg, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* spread = std::get_if<SpreadElement>(&expr.node)) {
    return !spread->argument ||
           validatePrivateNamesInExpression(*spread->argument, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* arrPattern = std::get_if<ArrayPattern>(&expr.node)) {
    for (const auto& element : arrPattern->elements) {
      if (element &&
          !validatePrivateNamesInExpression(*element, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return !arrPattern->rest ||
           validatePrivateNamesInExpression(*arrPattern->rest, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* objPattern = std::get_if<ObjectPattern>(&expr.node)) {
    for (const auto& prop : objPattern->properties) {
      if (prop.key &&
          !validatePrivateNamesInExpression(*prop.key, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
      if (prop.value &&
          !validatePrivateNamesInExpression(*prop.value, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return !objPattern->rest ||
           validatePrivateNamesInExpression(*objPattern->rest, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* assignPattern = std::get_if<AssignmentPattern>(&expr.node)) {
    return (!assignPattern->left ||
            validatePrivateNamesInExpression(*assignPattern->left, visiblePrivateNames, invalidPrivateName)) &&
           (!assignPattern->right ||
            validatePrivateNamesInExpression(*assignPattern->right, visiblePrivateNames, invalidPrivateName));
  }

  return true;
}

bool validatePrivateNamesInStatement(const Statement& stmt,
                                     const std::unordered_set<std::string>& visiblePrivateNames,
                                     std::string& invalidPrivateName) {
  if (auto* varDecl = std::get_if<VarDeclaration>(&stmt.node)) {
    for (const auto& decl : varDecl->declarations) {
      if (decl.pattern &&
          !validatePrivateNamesInExpression(*decl.pattern, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
      if (decl.init &&
          !validatePrivateNamesInExpression(*decl.init, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* functionDecl = std::get_if<FunctionDeclaration>(&stmt.node)) {
    for (const auto& param : functionDecl->params) {
      if (param.defaultValue &&
          !validatePrivateNamesInExpression(*param.defaultValue, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    for (const auto& bodyStmt : functionDecl->body) {
      if (bodyStmt &&
          !validatePrivateNamesInStatement(*bodyStmt, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* classDecl = std::get_if<ClassDeclaration>(&stmt.node)) {
    return validatePrivateNamesInClass(
      classDecl->superClass,
      classDecl->methods,
      visiblePrivateNames,
      invalidPrivateName);
  }
  if (auto* returnStmt = std::get_if<ReturnStmt>(&stmt.node)) {
    return !returnStmt->argument ||
           validatePrivateNamesInExpression(*returnStmt->argument, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* exprStmt = std::get_if<ExpressionStmt>(&stmt.node)) {
    return !exprStmt->expression ||
           validatePrivateNamesInExpression(*exprStmt->expression, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* blockStmt = std::get_if<BlockStmt>(&stmt.node)) {
    for (const auto& child : blockStmt->body) {
      if (child &&
          !validatePrivateNamesInStatement(*child, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    return true;
  }
  if (auto* ifStmt = std::get_if<IfStmt>(&stmt.node)) {
    return (!ifStmt->test ||
            validatePrivateNamesInExpression(*ifStmt->test, visiblePrivateNames, invalidPrivateName)) &&
           (!ifStmt->consequent ||
            validatePrivateNamesInStatement(*ifStmt->consequent, visiblePrivateNames, invalidPrivateName)) &&
           (!ifStmt->alternate ||
            validatePrivateNamesInStatement(*ifStmt->alternate, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* whileStmt = std::get_if<WhileStmt>(&stmt.node)) {
    return (!whileStmt->test ||
            validatePrivateNamesInExpression(*whileStmt->test, visiblePrivateNames, invalidPrivateName)) &&
           (!whileStmt->body ||
            validatePrivateNamesInStatement(*whileStmt->body, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* forStmt = std::get_if<ForStmt>(&stmt.node)) {
    return (!forStmt->init ||
            validatePrivateNamesInStatement(*forStmt->init, visiblePrivateNames, invalidPrivateName)) &&
           (!forStmt->test ||
            validatePrivateNamesInExpression(*forStmt->test, visiblePrivateNames, invalidPrivateName)) &&
           (!forStmt->update ||
            validatePrivateNamesInExpression(*forStmt->update, visiblePrivateNames, invalidPrivateName)) &&
           (!forStmt->body ||
            validatePrivateNamesInStatement(*forStmt->body, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* withStmt = std::get_if<WithStmt>(&stmt.node)) {
    return (!withStmt->object ||
            validatePrivateNamesInExpression(*withStmt->object, visiblePrivateNames, invalidPrivateName)) &&
           (!withStmt->body ||
            validatePrivateNamesInStatement(*withStmt->body, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* forInStmt = std::get_if<ForInStmt>(&stmt.node)) {
    return (!forInStmt->left ||
            validatePrivateNamesInStatement(*forInStmt->left, visiblePrivateNames, invalidPrivateName)) &&
           (!forInStmt->right ||
            validatePrivateNamesInExpression(*forInStmt->right, visiblePrivateNames, invalidPrivateName)) &&
           (!forInStmt->body ||
            validatePrivateNamesInStatement(*forInStmt->body, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* forOfStmt = std::get_if<ForOfStmt>(&stmt.node)) {
    return (!forOfStmt->left ||
            validatePrivateNamesInStatement(*forOfStmt->left, visiblePrivateNames, invalidPrivateName)) &&
           (!forOfStmt->right ||
            validatePrivateNamesInExpression(*forOfStmt->right, visiblePrivateNames, invalidPrivateName)) &&
           (!forOfStmt->body ||
            validatePrivateNamesInStatement(*forOfStmt->body, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* doWhileStmt = std::get_if<DoWhileStmt>(&stmt.node)) {
    return (!doWhileStmt->body ||
            validatePrivateNamesInStatement(*doWhileStmt->body, visiblePrivateNames, invalidPrivateName)) &&
           (!doWhileStmt->test ||
            validatePrivateNamesInExpression(*doWhileStmt->test, visiblePrivateNames, invalidPrivateName));
  }
  if (auto* switchStmt = std::get_if<SwitchStmt>(&stmt.node)) {
    if (switchStmt->discriminant &&
        !validatePrivateNamesInExpression(*switchStmt->discriminant, visiblePrivateNames, invalidPrivateName)) {
      return false;
    }
    for (const auto& switchCase : switchStmt->cases) {
      if (switchCase.test &&
          !validatePrivateNamesInExpression(*switchCase.test, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
      for (const auto& caseStmt : switchCase.consequent) {
        if (caseStmt &&
            !validatePrivateNamesInStatement(*caseStmt, visiblePrivateNames, invalidPrivateName)) {
          return false;
        }
      }
    }
    return true;
  }
  if (std::holds_alternative<BreakStmt>(stmt.node) ||
      std::holds_alternative<ContinueStmt>(stmt.node)) {
    return true;
  }
  if (auto* labelledStmt = std::get_if<LabelledStmt>(&stmt.node)) {
    return !labelledStmt->body ||
           validatePrivateNamesInStatement(*labelledStmt->body, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* throwStmt = std::get_if<ThrowStmt>(&stmt.node)) {
    return !throwStmt->argument ||
           validatePrivateNamesInExpression(*throwStmt->argument, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* tryStmt = std::get_if<TryStmt>(&stmt.node)) {
    for (const auto& child : tryStmt->block) {
      if (child &&
          !validatePrivateNamesInStatement(*child, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
    }
    if (tryStmt->hasHandler) {
      if (tryStmt->handler.paramPattern &&
          !validatePrivateNamesInExpression(
            *tryStmt->handler.paramPattern, visiblePrivateNames, invalidPrivateName)) {
        return false;
      }
      for (const auto& child : tryStmt->handler.body) {
        if (child &&
            !validatePrivateNamesInStatement(*child, visiblePrivateNames, invalidPrivateName)) {
          return false;
        }
      }
    }
    if (tryStmt->hasFinalizer) {
      for (const auto& child : tryStmt->finalizer) {
        if (child &&
            !validatePrivateNamesInStatement(*child, visiblePrivateNames, invalidPrivateName)) {
          return false;
        }
      }
    }
    return true;
  }
  if (auto* exportNamed = std::get_if<ExportNamedDeclaration>(&stmt.node)) {
    return !exportNamed->declaration ||
           validatePrivateNamesInStatement(*exportNamed->declaration, visiblePrivateNames, invalidPrivateName);
  }
  if (auto* exportDefault = std::get_if<ExportDefaultDeclaration>(&stmt.node)) {
    return !exportDefault->declaration ||
           validatePrivateNamesInExpression(*exportDefault->declaration, visiblePrivateNames, invalidPrivateName);
  }
  return true;
}

void setDefaultExportNameIfNeeded(Value& result) {
  if (result.isFunction()) {
    auto fn = result.getGC<Function>();
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
    auto cls = result.getGC<Class>();
    auto nameIt = cls->properties.find("name");
    bool shouldSet = nameIt == cls->properties.end() ||
                     (nameIt->second.isString() && std::get<std::string>(nameIt->second.data).empty());
    if (cls->name.empty() && shouldSet) {
      cls->name = "default";
      cls->properties["name"] = Value(std::string("default"));
      cls->properties["__non_writable_name"] = Value(true);
      cls->properties["__non_enum_name"] = Value(true);
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

GCPtr<Promise> coerceToPromise(const Value& value) {
  if (value.isPromise()) {
    return value.getGC<Promise>();
  }
  return Promise::resolved(value);
}

}  // namespace

extern thread_local std::vector<Module*> g_evalStack;

}  // namespace lightjs
