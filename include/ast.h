#pragma once

#include <memory>
#include <vector>
#include <string>
#include <variant>
#include <optional>
#include <cstdint>

namespace tinyjs {

struct Expression;
struct Statement;

using ExprPtr = std::unique_ptr<Expression>;
using StmtPtr = std::unique_ptr<Statement>;

struct Identifier {
  std::string name;
};

struct NumberLiteral {
  double value;
};

struct BigIntLiteral {
  int64_t value;
};

struct StringLiteral {
  std::string value;
};

struct RegexLiteral {
  std::string pattern;
  std::string flags;
};

struct BoolLiteral {
  bool value;
};

struct NullLiteral {};

struct BinaryExpr {
  enum class Op {
    Add, Sub, Mul, Div, Mod,
    Equal, NotEqual, StrictEqual, StrictNotEqual,
    Less, Greater, LessEqual, GreaterEqual,
    LogicalAnd, LogicalOr
  };
  Op op;
  ExprPtr left;
  ExprPtr right;
};

struct UnaryExpr {
  enum class Op { Not, Minus, Plus, Typeof };
  Op op;
  ExprPtr argument;
};

struct AssignmentExpr {
  enum class Op { Assign, AddAssign, SubAssign, MulAssign, DivAssign };
  Op op;
  ExprPtr left;
  ExprPtr right;
};

struct UpdateExpr {
  enum class Op { Increment, Decrement };
  Op op;
  ExprPtr argument;
  bool prefix;
};

struct CallExpr {
  ExprPtr callee;
  std::vector<ExprPtr> arguments;
};

struct MemberExpr {
  ExprPtr object;
  ExprPtr property;
  bool computed;
};

struct ConditionalExpr {
  ExprPtr test;
  ExprPtr consequent;
  ExprPtr alternate;
};

struct ArrayExpr {
  std::vector<ExprPtr> elements;
};

struct ObjectProperty {
  ExprPtr key;
  ExprPtr value;
};

struct ObjectExpr {
  std::vector<ObjectProperty> properties;
};

struct AwaitExpr {
  ExprPtr argument;
};

struct FunctionExpr {
  std::vector<Identifier> params;
  std::vector<StmtPtr> body;
  std::string name;
  bool isAsync;
  FunctionExpr() : isAsync(false) {}
};

struct SuperExpr {};

struct NewExpr {
  ExprPtr callee;
  std::vector<ExprPtr> arguments;
};

struct ThisExpr {};

struct MethodDefinition {
  enum class Kind { Constructor, Method, Get, Set };
  Kind kind;
  Identifier key;
  std::vector<Identifier> params;
  std::vector<StmtPtr> body;
  bool isStatic;
  bool isAsync;
  MethodDefinition() : kind(Kind::Method), isStatic(false), isAsync(false) {}

  // Add move constructor and assignment
  MethodDefinition(MethodDefinition&&) = default;
  MethodDefinition& operator=(MethodDefinition&&) = default;

  // Delete copy constructor and assignment
  MethodDefinition(const MethodDefinition&) = delete;
  MethodDefinition& operator=(const MethodDefinition&) = delete;
};

struct ClassExpr {
  std::string name;
  ExprPtr superClass;
  std::vector<MethodDefinition> methods;
};

struct Expression {
  std::variant<
    Identifier,
    NumberLiteral,
    BigIntLiteral,
    StringLiteral,
    RegexLiteral,
    BoolLiteral,
    NullLiteral,
    BinaryExpr,
    UnaryExpr,
    AssignmentExpr,
    UpdateExpr,
    CallExpr,
    MemberExpr,
    ConditionalExpr,
    ArrayExpr,
    ObjectExpr,
    FunctionExpr,
    ClassExpr,
    AwaitExpr,
    NewExpr,
    ThisExpr,
    SuperExpr
  > node;

  template<typename T>
  Expression(T&& n) : node(std::forward<T>(n)) {}
};

struct VarDeclarator {
  Identifier id;
  ExprPtr init;
};

struct VarDeclaration {
  enum class Kind { Let, Const, Var };
  Kind kind;
  std::vector<VarDeclarator> declarations;
};

struct FunctionDeclaration {
  Identifier id;
  std::vector<Identifier> params;
  std::vector<StmtPtr> body;
  bool isAsync;
  FunctionDeclaration() : isAsync(false) {}
};

struct ClassDeclaration {
  Identifier id;
  ExprPtr superClass;
  std::vector<MethodDefinition> methods;
};

struct ReturnStmt {
  ExprPtr argument;
};

struct ExpressionStmt {
  ExprPtr expression;
};

struct BlockStmt {
  std::vector<StmtPtr> body;
};

struct IfStmt {
  ExprPtr test;
  StmtPtr consequent;
  StmtPtr alternate;
};

struct WhileStmt {
  ExprPtr test;
  StmtPtr body;
};

struct ForStmt {
  StmtPtr init;
  ExprPtr test;
  ExprPtr update;
  StmtPtr body;
};

struct BreakStmt {};
struct ContinueStmt {};

struct ThrowStmt {
  ExprPtr argument;
};

struct CatchClause {
  Identifier param;
  std::vector<StmtPtr> body;
};

struct TryStmt {
  std::vector<StmtPtr> block;
  CatchClause handler;
  std::vector<StmtPtr> finalizer;
  bool hasHandler;
  bool hasFinalizer;
};

// Import/Export nodes for ES6 modules
struct ImportSpecifier {
  Identifier imported;
  Identifier local;
};

struct ImportDeclaration {
  std::vector<ImportSpecifier> specifiers;
  std::optional<Identifier> defaultImport;
  std::optional<Identifier> namespaceImport;  // import * as name
  std::string source;
};

struct ExportSpecifier {
  Identifier local;
  Identifier exported;
};

struct ExportNamedDeclaration {
  std::vector<ExportSpecifier> specifiers;
  std::optional<std::string> source;  // for re-exports
  StmtPtr declaration;  // for export const/let/var/function
};

struct ExportDefaultDeclaration {
  ExprPtr declaration;  // can be expression or function/class
};

struct ExportAllDeclaration {
  std::string source;
  std::optional<Identifier> exported;  // export * as name from
};

struct Statement {
  std::variant<
    VarDeclaration,
    FunctionDeclaration,
    ClassDeclaration,
    ReturnStmt,
    ExpressionStmt,
    BlockStmt,
    IfStmt,
    WhileStmt,
    ForStmt,
    BreakStmt,
    ContinueStmt,
    ThrowStmt,
    TryStmt,
    ImportDeclaration,
    ExportNamedDeclaration,
    ExportDefaultDeclaration,
    ExportAllDeclaration
  > node;

  template<typename T>
  Statement(T&& n) : node(std::forward<T>(n)) {}
};

struct Program {
  std::vector<StmtPtr> body;
  bool isModule = false;
};

}