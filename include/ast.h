#pragma once

#include <memory>
#include <vector>
#include <string>
#include <variant>
#include <optional>
#include <cstdint>
#include "bigint.h"
#include "object_shape.h"

namespace lightjs {

// Source location for error messages
struct SourceLocation {
  uint32_t line = 0;
  uint32_t column = 0;

  SourceLocation() = default;
  SourceLocation(uint32_t l, uint32_t c) : line(l), column(c) {}
};

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
  bigint::BigIntValue value;
};

struct StringLiteral {
  std::string value;
};

struct TemplateLiteral {
  std::vector<std::string> quasis;  // Static string parts
  std::vector<ExprPtr> expressions;  // Interpolated expressions
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
    Add, Sub, Mul, Div, Mod, Exp,  // Exp for exponentiation
    Equal, NotEqual, StrictEqual, StrictNotEqual,
    Less, Greater, LessEqual, GreaterEqual,
    BitwiseAnd, BitwiseXor, BitwiseOr,
    LeftShift, RightShift, UnsignedRightShift,
    LogicalAnd, LogicalOr, NullishCoalescing,
    In, Instanceof  // Property/type checking operators
  };
  Op op;
  ExprPtr left;
  ExprPtr right;
};

struct UnaryExpr {
  enum class Op { Not, Minus, Plus, Typeof, Void, BitNot, Delete };
  Op op;
  ExprPtr argument;
};

struct AssignmentExpr {
  enum class Op { Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign, ExpAssign,
    BitwiseAndAssign, BitwiseOrAssign, BitwiseXorAssign,
    LeftShiftAssign, RightShiftAssign, UnsignedRightShiftAssign,
    AndAssign, OrAssign, NullishAssign };
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
  bool optional = false;  // Optional call (?.())
  bool inOptionalChain = false;  // Part of an optional chain (propagate short-circuit)
};

struct MemberExpr {
  ExprPtr object;
  ExprPtr property;
  bool computed;
  bool optional;  // Optional chaining (?.)
  bool inOptionalChain = false;  // Part of an optional chain (propagate short-circuit)
  mutable PropertyCache cache;  // Inline cache for property access optimization
  MemberExpr() : computed(false), optional(false), inOptionalChain(false) {}
};

struct ConditionalExpr {
  ExprPtr test;
  ExprPtr consequent;
  ExprPtr alternate;
};

struct SequenceExpr {
  std::vector<ExprPtr> expressions;
};

struct ArrayExpr {
  std::vector<ExprPtr> elements;
};

struct ObjectProperty {
  ExprPtr key;
  ExprPtr value;
  bool isSpread = false;  // For spread properties (...obj)
  bool isComputed = false;  // For computed property names ([expr])
};

struct ObjectExpr {
  std::vector<ObjectProperty> properties;
};

struct AwaitExpr {
  ExprPtr argument;
};

struct YieldExpr {
  ExprPtr argument;
  bool delegate = false;  // yield* (delegate to another iterator)
};

struct Parameter {
  Identifier name;
  ExprPtr defaultValue;  // Optional default value
};

struct FunctionExpr {
  std::vector<Parameter> params;
  std::optional<Identifier> restParam;  // Rest parameter (e.g., ...args)
  std::vector<StmtPtr> body;
  std::string name;
  bool isAsync;
  bool isGenerator;  // Generator function (function*)
  bool isArrow;  // Arrow function expression (e.g., (x) => x * 2)
  FunctionExpr() : isAsync(false), isGenerator(false), isArrow(false) {}
};

struct SuperExpr {};

struct SpreadElement {
  ExprPtr argument;
};

struct NewExpr {
  ExprPtr callee;
  std::vector<ExprPtr> arguments;
};

struct ThisExpr {};

struct MethodDefinition {
  enum class Kind { Constructor, Method, Get, Set, Field };
  Kind kind;
  Identifier key;
  ExprPtr computedKey;  // For computed property names in class elements ([expr])
  std::vector<Parameter> params;
  std::optional<Identifier> restParam;  // Rest parameter (...args)
  std::vector<StmtPtr> body;
  ExprPtr initializer;  // For field initializers (Kind::Field)
  bool isStatic;
  bool isAsync;
  bool isGenerator;
  bool isPrivate;
  bool computed;
  MethodDefinition()
      : kind(Kind::Method),
        isStatic(false),
        isAsync(false),
        isGenerator(false),
        isPrivate(false),
        computed(false) {}

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

// Destructuring patterns
struct ArrayPattern {
  std::vector<ExprPtr> elements;  // Can be Identifier or nested patterns
  ExprPtr rest;  // Rest element (...rest)
};

struct ObjectPattern {
  struct Property {
    ExprPtr key;
    ExprPtr value;  // Pattern to bind to
    bool computed = false;
  };
  std::vector<Property> properties;
  ExprPtr rest;  // Rest properties (...rest)
};

struct AssignmentPattern {
  ExprPtr left;   // Binding pattern
  ExprPtr right;  // Default initializer
};

// import.meta - ES2020
struct MetaProperty {
  std::string meta;  // "meta" for import.meta
  std::string property;  // property name if accessed (e.g., "url")
};

struct Expression {
  std::variant<
    Identifier,
    NumberLiteral,
    BigIntLiteral,
    StringLiteral,
    TemplateLiteral,
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
    SequenceExpr,
    ArrayExpr,
    ObjectExpr,
    FunctionExpr,
    ClassExpr,
    AwaitExpr,
    YieldExpr,
    NewExpr,
    ThisExpr,
    SuperExpr,
    SpreadElement,
    ArrayPattern,
    ObjectPattern,
    AssignmentPattern,
    MetaProperty
  > node;

  SourceLocation loc;
  bool parenthesized = false;

  template<typename T>
  Expression(T&& n) : node(std::forward<T>(n)) {}
};

struct VarDeclarator {
  ExprPtr pattern;  // Can be Identifier, ArrayPattern, or ObjectPattern
  ExprPtr init;
};

struct VarDeclaration {
  enum class Kind { Let, Const, Var };
  Kind kind;
  std::vector<VarDeclarator> declarations;
};

struct FunctionDeclaration {
  Identifier id;
  std::vector<Parameter> params;
  std::optional<Identifier> restParam;  // Rest parameter (e.g., ...args)
  std::vector<StmtPtr> body;
  bool isAsync;
  bool isGenerator;  // Generator function (function*)
  FunctionDeclaration() : isAsync(false), isGenerator(false) {}
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

struct WithStmt {
  ExprPtr object;
  StmtPtr body;
};

struct ForInStmt {
  StmtPtr left;  // VarDeclaration or Identifier
  ExprPtr right;
  StmtPtr body;
};

struct ForOfStmt {
  StmtPtr left;  // VarDeclaration or Identifier
  ExprPtr right;
  StmtPtr body;
  bool isAwait = false;
};

struct DoWhileStmt {
  StmtPtr body;
  ExprPtr test;
};

struct SwitchCase {
  ExprPtr test;  // nullptr for default case
  std::vector<StmtPtr> consequent;
};

struct SwitchStmt {
  ExprPtr discriminant;
  std::vector<SwitchCase> cases;
};

struct BreakStmt {
  std::string label;
};
struct ContinueStmt {
  std::string label;
};

struct LabelledStmt {
  std::string label;
  StmtPtr body;
};

struct ThrowStmt {
  ExprPtr argument;
};

struct CatchClause {
  Identifier param;
  ExprPtr paramPattern;
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
  bool isHoistableDeclaration = false;
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
    WithStmt,
    ForInStmt,
    ForOfStmt,
    DoWhileStmt,
    SwitchStmt,
    BreakStmt,
    ContinueStmt,
    LabelledStmt,
    ThrowStmt,
    TryStmt,
    ImportDeclaration,
    ExportNamedDeclaration,
    ExportDefaultDeclaration,
    ExportAllDeclaration
  > node;

  SourceLocation loc;

  template<typename T>
  Statement(T&& n) : node(std::forward<T>(n)) {}
};

struct Program {
  std::vector<StmtPtr> body;
  bool isModule = false;
};

}
