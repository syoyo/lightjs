#pragma once

#include <memory>
#include <vector>
#include <string>
#include <variant>

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

struct StringLiteral {
  std::string value;
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

struct FunctionExpr {
  std::vector<Identifier> params;
  std::vector<StmtPtr> body;
  std::string name;
};

struct Expression {
  std::variant<
    Identifier,
    NumberLiteral,
    StringLiteral,
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
    FunctionExpr
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

struct Statement {
  std::variant<
    VarDeclaration,
    FunctionDeclaration,
    ReturnStmt,
    ExpressionStmt,
    BlockStmt,
    IfStmt,
    WhileStmt,
    ForStmt,
    BreakStmt,
    ContinueStmt,
    ThrowStmt,
    TryStmt
  > node;

  template<typename T>
  Statement(T&& n) : node(std::forward<T>(n)) {}
};

struct Program {
  std::vector<StmtPtr> body;
};

}