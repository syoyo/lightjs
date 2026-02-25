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
constexpr const char* kSyntheticDefaultExportBinding = "__lightjs_default_export__";

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
    bool hasOwnNameProperty = cls->properties.find("name") != cls->properties.end();
    if (cls->name.empty() && !hasOwnNameProperty) {
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
  environment_->define("this", Value(Undefined{}));

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

bool Module::hasExport(const std::string& name) const {
  if (ambiguousReExports_.count(name) > 0) {
    return false;
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
      // Circular lookup for the same export name.
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
          auto depIt = module->sourceDependencies_.find(*exportNamed->source);
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
        auto depIt = module->sourceDependencies_.find(exportAll->source);
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
      auto depIt = module->sourceDependencies_.find(exportAll->source);
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
    std::shared_ptr<Environment> prevEnv;
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
    LIGHTJS_RUN_TASK_VOID(task);
    if (interpreter->hasError()) {
      lastError_ = interpreter->getError();
      interpreter->clearError();
      return false;
    }
  }

  for (const auto& defaultFunction : hoistedDefaultFunctions) {
    auto task = interpreter->evaluate(*defaultFunction.expression);
    Value functionValue = Value(Undefined{});
    LIGHTJS_RUN_TASK(task, functionValue);
    if (interpreter->hasError()) {
      lastError_ = interpreter->getError();
      interpreter->clearError();
      return false;
    }
    if (defaultFunction.assignDefaultName) {
      setDefaultExportNameIfNeeded(functionValue);
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
          auto obj = std::get<std::shared_ptr<Object>>(localValue->data);
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
        auto obj = std::get<std::shared_ptr<Object>>(exportIt->second.data);
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
      auto obj = std::get<std::shared_ptr<Object>>(value.data);
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
      auto depIt = sourceDependencies_.find(*exportNamed->source);
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
      auto depIt = sourceDependencies_.find(exportAll->source);
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

    auto depIt = sourceDependencies_.find(exportAll->source);
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

      auto depIt = sourceDependencies_.find(importDecl->source);
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
        environment_->define(importDecl->namespaceImport->name, Value(importedModule->getNamespaceObject()), true);
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
    std::shared_ptr<Environment> prevEnv;
    bool prevStrictMode;
    ~ModuleEvalScopeGuard() {
      interpreter->setEnvironment(prevEnv);
      interpreter->strictMode_ = prevStrictMode;
    }
  } scopeGuard{interpreter, prevEnv, prevStrictMode};

  interpreter->setEnvironment(environment_);
  interpreter->strictMode_ = true;
  state_ = State::Evaluating;

  // Hoist var declarations before evaluating the body (like scripts do).
  // This is needed because `export var x = ...` uses set() not define(),
  // which requires the variable to already exist from hoisting.
  interpreter->hoistVarDeclarations(ast_->body);

  try {
    while (nextStatementIndex_ < ast_->body.size()) {
      const auto& stmtPtr = ast_->body[nextStatementIndex_];
      if (!stmtPtr) {
        nextStatementIndex_++;
        continue;
      }
      const auto& stmt = *stmtPtr;

      // Function/generator declarations are initialized during module declaration
      // instantiation and must not rebind during body execution.
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
    if (!initializeDeclaredExports(interpreter)) {
      state_ = State::Instantiated;
      return false;
    }
    // Mark as evaluating before traversing dependencies so cycle participants
    // observe this module as in-progress and don't re-enter body execution.
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

    for (const auto& dep : dependencies_) {
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

std::optional<Value> Module::getExport(const std::string& name) const {
  if (ambiguousReExports_.count(name) > 0) {
    return std::nullopt;
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

  auto exportNames = getExportNames();
  moduleNamespace->moduleExportNames.reserve(exportNames.size());
  for (const auto& name : exportNames) {
    moduleNamespace->moduleExportNames.push_back(name);
    if (auto value = getExport(name)) {
      moduleNamespace->properties[name] = *value;
    } else {
      moduleNamespace->properties[name] = Value(Undefined{});
    }

    auto getter = std::make_shared<Function>();
    getter->isNative = true;
    getter->nativeFunc = [module = this, name](const std::vector<Value>&) -> Value {
      if (auto current = module->getExport(name)) {
        return *current;
      }
      throw std::runtime_error("ReferenceError: Cannot access '" + name + "' before initialization");
    };
    moduleNamespace->properties["__get_" + name] = Value(getter);
  }
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
