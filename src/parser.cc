#include "parser.h"
#include "lexer.h"
#include <stdexcept>

namespace tinyjs {

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

const Token& Parser::current() const {
  if (pos_ >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[pos_];
}

const Token& Parser::peek(size_t offset) const {
  if (pos_ + offset >= tokens_.size()) {
    return tokens_.back();
  }
  return tokens_[pos_ + offset];
}

const Token& Parser::advance() {
  if (pos_ < tokens_.size()) {
    return tokens_[pos_++];
  }
  return tokens_.back();
}

bool Parser::match(TokenType type) const {
  return current().type == type;
}

bool Parser::expect(TokenType type) {
  if (!match(type)) {
    return false;
  }
  advance();
  return true;
}

void Parser::consumeSemicolon() {
  if (match(TokenType::Semicolon)) {
    advance();
  }
}

std::optional<Program> Parser::parse() {
  Program program;
  while (!match(TokenType::EndOfFile)) {
    if (auto stmt = parseStatement()) {
      program.body.push_back(std::move(stmt));
    } else {
      return std::nullopt;
    }
  }
  return program;
}

StmtPtr Parser::parseStatement() {
  switch (current().type) {
    case TokenType::Let:
    case TokenType::Const:
    case TokenType::Var:
      return parseVarDeclaration();
    case TokenType::Async:
      if (peek().type == TokenType::Function) {
        return parseFunctionDeclaration();
      }
      return parseExpressionStatement();
    case TokenType::Function:
      return parseFunctionDeclaration();
    case TokenType::Class:
      return parseClassDeclaration();
    case TokenType::Return:
      return parseReturnStatement();
    case TokenType::If:
      return parseIfStatement();
    case TokenType::While:
      return parseWhileStatement();
    case TokenType::Do:
      return parseDoWhileStatement();
    case TokenType::For:
      return parseForStatement();
    case TokenType::Switch:
      return parseSwitchStatement();
    case TokenType::Break:
      advance();
      consumeSemicolon();
      return std::make_unique<Statement>(BreakStmt{});
    case TokenType::Continue:
      advance();
      consumeSemicolon();
      return std::make_unique<Statement>(ContinueStmt{});
    case TokenType::Throw:
      return std::make_unique<Statement>(ThrowStmt{parseExpression()});
    case TokenType::Try:
      return parseTryStatement();
    case TokenType::Import:
      return parseImportDeclaration();
    case TokenType::Export:
      return parseExportDeclaration();
    case TokenType::LeftBrace:
      return parseBlockStatement();
    default:
      return parseExpressionStatement();
  }
}

StmtPtr Parser::parseVarDeclaration() {
  VarDeclaration::Kind kind;
  switch (current().type) {
    case TokenType::Let: kind = VarDeclaration::Kind::Let; break;
    case TokenType::Const: kind = VarDeclaration::Kind::Const; break;
    case TokenType::Var: kind = VarDeclaration::Kind::Var; break;
    default: return nullptr;
  }
  advance();

  VarDeclaration decl;
  decl.kind = kind;

  do {
    if (!decl.declarations.empty()) {
      expect(TokenType::Comma);
    }

    // Parse pattern (identifier, array pattern, or object pattern)
    ExprPtr pattern = parsePattern();
    if (!pattern) {
      return nullptr;
    }

    ExprPtr init = nullptr;
    if (match(TokenType::Equal)) {
      advance();
      init = parseExpression();
    }

    decl.declarations.push_back({std::move(pattern), std::move(init)});
  } while (match(TokenType::Comma));

  consumeSemicolon();
  return std::make_unique<Statement>(std::move(decl));
}

StmtPtr Parser::parseFunctionDeclaration() {
  bool isAsync = false;
  if (match(TokenType::Async)) {
    isAsync = true;
    advance();
  }

  expect(TokenType::Function);

  // Check for generator function (function*)
  bool isGenerator = false;
  if (match(TokenType::Star)) {
    isGenerator = true;
    advance();
  }

  if (!match(TokenType::Identifier)) {
    return nullptr;
  }
  std::string name = current().value;
  advance();

  expect(TokenType::LeftParen);

  std::vector<Parameter> params;
  std::optional<Identifier> restParam;

  while (!match(TokenType::RightParen)) {
    if (!params.empty()) {
      expect(TokenType::Comma);
    }

    // Check for rest parameter
    if (match(TokenType::DotDotDot)) {
      advance();
      if (match(TokenType::Identifier)) {
        restParam = Identifier{current().value};
        advance();
      }
      // Rest parameter must be last
      break;
    } else if (match(TokenType::Identifier)) {
      Parameter param;
      param.name = Identifier{current().value};
      advance();

      // Check for default value
      if (match(TokenType::Equal)) {
        advance();
        param.defaultValue = parseAssignment();
      }

      params.push_back(std::move(param));
    } else {
      // Unexpected token in parameter list
      break;
    }
  }
  expect(TokenType::RightParen);

  auto block = parseBlockStatement();
  if (!block) {
    return nullptr;
  }

  auto blockStmt = std::get_if<BlockStmt>(&block->node);
  if (!blockStmt) {
    return nullptr;
  }

  FunctionDeclaration funcDecl;
  funcDecl.id = {name};
  funcDecl.params = std::move(params);
  funcDecl.restParam = restParam;
  funcDecl.body = std::move(blockStmt->body);
  funcDecl.isAsync = isAsync;
  funcDecl.isGenerator = isGenerator;

  return std::make_unique<Statement>(std::move(funcDecl));
}

StmtPtr Parser::parseClassDeclaration() {
  expect(TokenType::Class);

  if (!match(TokenType::Identifier)) {
    return nullptr;
  }
  std::string className = current().value;
  advance();

  ExprPtr superClass;
  if (match(TokenType::Extends)) {
    advance();
    superClass = parsePrimary();
  }

  expect(TokenType::LeftBrace);

  std::vector<MethodDefinition> methods;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    MethodDefinition method;

    // Check for static
    if (match(TokenType::Static)) {
      method.isStatic = true;
      advance();
    }

    // Check for async
    if (match(TokenType::Async)) {
      method.isAsync = true;
      advance();
    }

    // Check for getter/setter
    if (match(TokenType::Get)) {
      method.kind = MethodDefinition::Kind::Get;
      advance();
    } else if (match(TokenType::Set)) {
      method.kind = MethodDefinition::Kind::Set;
      advance();
    }

    // Method name
    if (match(TokenType::Identifier)) {
      std::string methodName = current().value;
      if (methodName == "constructor") {
        method.kind = MethodDefinition::Kind::Constructor;
      }
      method.key.name = methodName;
      advance();
    } else {
      return nullptr;
    }

    // Parameters
    expect(TokenType::LeftParen);
    while (!match(TokenType::RightParen)) {
      if (match(TokenType::Identifier)) {
        method.params.push_back({current().value});
        advance();
        if (match(TokenType::Comma)) {
          advance();
        }
      } else {
        break;
      }
    }
    expect(TokenType::RightParen);

    // Method body
    expect(TokenType::LeftBrace);
    while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
      method.body.push_back(parseStatement());
    }
    expect(TokenType::RightBrace);

    methods.push_back(std::move(method));
  }
  expect(TokenType::RightBrace);

  auto decl = std::make_unique<Statement>(ClassDeclaration{
    {className},
    std::move(superClass),
    std::move(methods)
  });

  return decl;
}

StmtPtr Parser::parseReturnStatement() {
  expect(TokenType::Return);

  ExprPtr argument = nullptr;
  if (!match(TokenType::Semicolon) && !match(TokenType::EndOfFile)) {
    argument = parseExpression();
  }

  consumeSemicolon();
  return std::make_unique<Statement>(ReturnStmt{std::move(argument)});
}

StmtPtr Parser::parseIfStatement() {
  expect(TokenType::If);
  expect(TokenType::LeftParen);
  auto test = parseExpression();
  expect(TokenType::RightParen);

  auto consequent = parseStatement();
  StmtPtr alternate = nullptr;

  if (match(TokenType::Else)) {
    advance();
    alternate = parseStatement();
  }

  return std::make_unique<Statement>(IfStmt{
    std::move(test),
    std::move(consequent),
    std::move(alternate)
  });
}

StmtPtr Parser::parseWhileStatement() {
  expect(TokenType::While);
  expect(TokenType::LeftParen);
  auto test = parseExpression();
  expect(TokenType::RightParen);
  auto body = parseStatement();

  return std::make_unique<Statement>(WhileStmt{std::move(test), std::move(body)});
}

StmtPtr Parser::parseForStatement() {
  expect(TokenType::For);
  expect(TokenType::LeftParen);

  // Check if it's a for...in or for...of loop
  size_t savedPos = pos_;
  bool isForInOrOf = false;
  bool isForOf = false;

  // Try to detect for...in or for...of pattern
  if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Var)) {
    advance();
    if (match(TokenType::Identifier)) {
      advance();
      if (match(TokenType::In)) {
        isForInOrOf = true;
        isForOf = false;
      } else if (match(TokenType::Of)) {
        isForInOrOf = true;
        isForOf = true;
      }
    }
  } else if (match(TokenType::Identifier)) {
    advance();
    if (match(TokenType::In)) {
      isForInOrOf = true;
      isForOf = false;
    } else if (match(TokenType::Of)) {
      isForInOrOf = true;
      isForOf = true;
    }
  }

  // Reset position
  pos_ = savedPos;

  if (isForInOrOf) {
    // Parse for...in or for...of
    StmtPtr left = nullptr;
    if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Var)) {
      VarDeclaration::Kind kind;
      switch (current().type) {
        case TokenType::Let: kind = VarDeclaration::Kind::Let; break;
        case TokenType::Const: kind = VarDeclaration::Kind::Const; break;
        case TokenType::Var: kind = VarDeclaration::Kind::Var; break;
        default: return nullptr;
      }
      advance();

      if (!match(TokenType::Identifier)) {
        return nullptr;
      }
      std::string name = current().value;
      advance();

      VarDeclaration decl;
      decl.kind = kind;
      decl.declarations.push_back({std::make_unique<Expression>(Identifier{name}), nullptr});
      left = std::make_unique<Statement>(std::move(decl));
    } else {
      auto expr = parseExpression();
      left = std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
    }

    bool isOf = match(TokenType::Of);
    if (isOf) {
      advance();
    } else {
      expect(TokenType::In);
    }

    auto right = parseExpression();
    expect(TokenType::RightParen);
    auto body = parseStatement();

    if (isOf) {
      return std::make_unique<Statement>(ForOfStmt{
        std::move(left),
        std::move(right),
        std::move(body)
      });
    } else {
      return std::make_unique<Statement>(ForInStmt{
        std::move(left),
        std::move(right),
        std::move(body)
      });
    }
  }

  // Regular for loop
  StmtPtr init = nullptr;
  if (!match(TokenType::Semicolon)) {
    if (match(TokenType::Let) || match(TokenType::Const) || match(TokenType::Var)) {
      init = parseVarDeclaration();
    } else {
      auto expr = parseExpression();
      expect(TokenType::Semicolon);
      init = std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
    }
  } else {
    advance();
  }

  ExprPtr test = nullptr;
  if (!match(TokenType::Semicolon)) {
    test = parseExpression();
  }
  expect(TokenType::Semicolon);

  ExprPtr update = nullptr;
  if (!match(TokenType::RightParen)) {
    update = parseExpression();
  }
  expect(TokenType::RightParen);

  auto body = parseStatement();

  return std::make_unique<Statement>(ForStmt{
    std::move(init),
    std::move(test),
    std::move(update),
    std::move(body)
  });
}

StmtPtr Parser::parseDoWhileStatement() {
  expect(TokenType::Do);
  auto body = parseStatement();
  expect(TokenType::While);
  expect(TokenType::LeftParen);
  auto test = parseExpression();
  expect(TokenType::RightParen);
  consumeSemicolon();

  return std::make_unique<Statement>(DoWhileStmt{std::move(body), std::move(test)});
}

StmtPtr Parser::parseSwitchStatement() {
  expect(TokenType::Switch);
  expect(TokenType::LeftParen);
  auto discriminant = parseExpression();
  expect(TokenType::RightParen);
  expect(TokenType::LeftBrace);

  std::vector<SwitchCase> cases;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    if (match(TokenType::Case)) {
      advance();
      auto test = parseExpression();
      expect(TokenType::Colon);

      std::vector<StmtPtr> consequent;
      while (!match(TokenType::Case) && !match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        // Check for 'default' keyword
        if (match(TokenType::Default)) {
          break;
        }
        if (auto stmt = parseStatement()) {
          consequent.push_back(std::move(stmt));
        }
      }

      cases.push_back(SwitchCase{std::move(test), std::move(consequent)});
    } else if (match(TokenType::Default)) {
      advance();
      expect(TokenType::Colon);

      std::vector<StmtPtr> consequent;
      while (!match(TokenType::Case) && !match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
        if (auto stmt = parseStatement()) {
          consequent.push_back(std::move(stmt));
        }
      }

      cases.push_back(SwitchCase{nullptr, std::move(consequent)});
    } else {
      break;
    }
  }

  expect(TokenType::RightBrace);
  return std::make_unique<Statement>(SwitchStmt{std::move(discriminant), std::move(cases)});
}

StmtPtr Parser::parseBlockStatement() {
  expect(TokenType::LeftBrace);

  std::vector<StmtPtr> body;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    if (auto stmt = parseStatement()) {
      body.push_back(std::move(stmt));
    }
  }

  expect(TokenType::RightBrace);
  return std::make_unique<Statement>(BlockStmt{std::move(body)});
}

StmtPtr Parser::parseExpressionStatement() {
  auto expr = parseExpression();
  consumeSemicolon();
  return std::make_unique<Statement>(ExpressionStmt{std::move(expr)});
}

StmtPtr Parser::parseTryStatement() {
  expect(TokenType::Try);
  auto tryBlock = parseBlockStatement();

  auto tryBlockStmt = std::get_if<BlockStmt>(&tryBlock->node);
  if (!tryBlockStmt) {
    return nullptr;
  }

  CatchClause handler;
  bool hasHandler = false;
  std::vector<StmtPtr> finalizer;
  bool hasFinalizer = false;

  if (match(TokenType::Catch)) {
    advance();
    hasHandler = true;

    if (match(TokenType::LeftParen)) {
      advance();
      if (match(TokenType::Identifier)) {
        handler.param = {current().value};
        advance();
      }
      expect(TokenType::RightParen);
    }

    auto catchBlock = parseBlockStatement();
    auto catchBlockStmt = std::get_if<BlockStmt>(&catchBlock->node);
    if (catchBlockStmt) {
      handler.body = std::move(catchBlockStmt->body);
    }
  }

  if (match(TokenType::Finally)) {
    advance();
    hasFinalizer = true;
    auto finallyBlock = parseBlockStatement();
    auto finallyBlockStmt = std::get_if<BlockStmt>(&finallyBlock->node);
    if (finallyBlockStmt) {
      finalizer = std::move(finallyBlockStmt->body);
    }
  }

  return std::make_unique<Statement>(TryStmt{
    std::move(tryBlockStmt->body),
    std::move(handler),
    std::move(finalizer),
    hasHandler,
    hasFinalizer
  });
}

StmtPtr Parser::parseImportDeclaration() {
  expect(TokenType::Import);

  ImportDeclaration import;

  // Check for default import: import foo from "module"
  if (match(TokenType::Identifier)) {
    import.defaultImport = Identifier{current().value};
    advance();

    // Check for additional named imports: import foo, { bar } from "module"
    if (match(TokenType::Comma)) {
      advance();
    }
  }

  // Check for namespace import: import * as name from "module"
  if (match(TokenType::Star)) {
    advance();
    expect(TokenType::As);
    if (match(TokenType::Identifier)) {
      import.namespaceImport = Identifier{current().value};
      advance();
    }
  }
  // Check for named imports: import { foo, bar as baz } from "module"
  else if (match(TokenType::LeftBrace)) {
    advance();

    while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
      if (!import.specifiers.empty()) {
        expect(TokenType::Comma);
      }

      if (match(TokenType::Identifier)) {
        ImportSpecifier spec;
        spec.imported = Identifier{current().value};
        spec.local = spec.imported;
        advance();

        // Check for renaming: foo as bar
        if (match(TokenType::As)) {
          advance();
          if (match(TokenType::Identifier)) {
            spec.local = Identifier{current().value};
            advance();
          }
        }

        import.specifiers.push_back(spec);
      }
    }

    expect(TokenType::RightBrace);
  }

  // Expect 'from' keyword
  expect(TokenType::From);

  // Expect module source string
  if (match(TokenType::String)) {
    import.source = current().value;
    advance();
  }

  consumeSemicolon();

  return std::make_unique<Statement>(std::move(import));
}

StmtPtr Parser::parseExportDeclaration() {
  expect(TokenType::Export);

  // Export default declaration
  if (match(TokenType::Default)) {
    advance();

    ExportDefaultDeclaration exportDefault;

    // Can be expression or function/class declaration
    if (match(TokenType::Function) || match(TokenType::Async)) {
      exportDefault.declaration = parseFunctionExpression();
    } else {
      exportDefault.declaration = parseAssignment();
    }

    consumeSemicolon();
    return std::make_unique<Statement>(std::move(exportDefault));
  }

  // Export all declaration: export * from "module" or export * as name from "module"
  if (match(TokenType::Star)) {
    advance();

    ExportAllDeclaration exportAll;

    if (match(TokenType::As)) {
      advance();
      if (match(TokenType::Identifier)) {
        exportAll.exported = Identifier{current().value};
        advance();
      }
    }

    expect(TokenType::From);

    if (match(TokenType::String)) {
      exportAll.source = current().value;
      advance();
    }

    consumeSemicolon();
    return std::make_unique<Statement>(std::move(exportAll));
  }

  // Export named declaration
  ExportNamedDeclaration exportNamed;

  // Export variable/function declaration
  if (match(TokenType::Const) || match(TokenType::Let) ||
      match(TokenType::Var) || match(TokenType::Function) ||
      match(TokenType::Async)) {
    exportNamed.declaration = parseStatement();
    return std::make_unique<Statement>(std::move(exportNamed));
  }

  // Export list: export { foo, bar as baz }
  if (match(TokenType::LeftBrace)) {
    advance();

    while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
      if (!exportNamed.specifiers.empty()) {
        expect(TokenType::Comma);
      }

      if (match(TokenType::Identifier)) {
        ExportSpecifier spec;
        spec.local = Identifier{current().value};
        spec.exported = spec.local;
        advance();

        // Check for renaming: foo as bar
        if (match(TokenType::As)) {
          advance();
          if (match(TokenType::Identifier)) {
            spec.exported = Identifier{current().value};
            advance();
          }
        }

        exportNamed.specifiers.push_back(spec);
      }
    }

    expect(TokenType::RightBrace);

    // Check for re-export: export { foo } from "module"
    if (match(TokenType::From)) {
      advance();
      if (match(TokenType::String)) {
        exportNamed.source = current().value;
        advance();
      }
    }

    consumeSemicolon();
  }

  return std::make_unique<Statement>(std::move(exportNamed));
}

ExprPtr Parser::parseExpression() {
  return parseAssignment();
}

ExprPtr Parser::parseAssignment() {
  // Check for arrow functions
  // Case 1: Single parameter without parentheses (e.g., x => x * 2)
  if (match(TokenType::Identifier)) {
    size_t savedPos = pos_;
    auto paramName = current().value;
    advance();

    if (match(TokenType::Arrow)) {
      advance(); // consume =>

      FunctionExpr func;
      func.isArrow = true;
      Parameter param;
      param.name = Identifier{paramName};
      func.params.push_back(std::move(param));

      // Parse body: either expression or block statement
      if (match(TokenType::LeftBrace)) {
        // Block body { ... }
        auto blockStmt = parseBlockStatement();
        if (auto* block = std::get_if<BlockStmt>(&blockStmt->node)) {
          func.body = std::move(block->body);
        }
      } else {
        // Expression body (implicit return)
        auto expr = parseAssignment();
        auto returnStmt = std::make_unique<Statement>(ReturnStmt{std::move(expr)});
        func.body.push_back(std::move(returnStmt));
      }

      return std::make_unique<Expression>(std::move(func));
    }

    // Not an arrow function, restore position
    pos_ = savedPos;
  }

  // Case 2: Parenthesized parameters (e.g., (x, y) => x + y) or (x) => x * 2
  if (match(TokenType::LeftParen)) {
    size_t savedPos = pos_;
    advance();

    // Try to parse as arrow function parameters
    std::vector<Parameter> params;
    std::optional<Identifier> restParam;
    bool isArrowFunc = false;

    if (!match(TokenType::RightParen)) {
      // Parse parameter list
      do {
        if (match(TokenType::DotDotDot)) {
          // Rest parameter
          advance();
          if (!match(TokenType::Identifier)) {
            // Not a valid arrow function, restore
            pos_ = savedPos;
            goto normal_parse;
          }
          restParam = Identifier{current().value};
          advance();
          break;
        }

        if (!match(TokenType::Identifier)) {
          // Not a valid arrow function, restore
          pos_ = savedPos;
          goto normal_parse;
        }
        Parameter param;
        param.name = Identifier{current().value};
        advance();

        // Check for default value (arrow functions support defaults)
        if (match(TokenType::Equal)) {
          advance();
          param.defaultValue = parseAssignment();
        }

        params.push_back(std::move(param));

        if (match(TokenType::Comma)) {
          advance();
        } else {
          break;
        }
      } while (true);
    }

    if (match(TokenType::RightParen)) {
      advance();
      if (match(TokenType::Arrow)) {
        isArrowFunc = true;
      }
    }

    if (isArrowFunc) {
      advance(); // consume =>

      FunctionExpr func;
      func.isArrow = true;
      func.params = std::move(params);
      func.restParam = restParam;

      // Parse body: either expression or block statement
      if (match(TokenType::LeftBrace)) {
        // Block body { ... }
        auto blockStmt = parseBlockStatement();
        if (auto* block = std::get_if<BlockStmt>(&blockStmt->node)) {
          func.body = std::move(block->body);
        }
      } else {
        // Expression body (implicit return)
        auto expr = parseAssignment();
        auto returnStmt = std::make_unique<Statement>(ReturnStmt{std::move(expr)});
        func.body.push_back(std::move(returnStmt));
      }

      return std::make_unique<Expression>(std::move(func));
    }

    // Not an arrow function, restore
    pos_ = savedPos;
  }

normal_parse:
  auto left = parseConditional();

  if (match(TokenType::Equal) || match(TokenType::PlusEqual) ||
      match(TokenType::MinusEqual) || match(TokenType::StarEqual) ||
      match(TokenType::SlashEqual) || match(TokenType::AmpAmpEqual) ||
      match(TokenType::PipePipeEqual) || match(TokenType::QuestionQuestionEqual)) {

    AssignmentExpr::Op op;
    switch (current().type) {
      case TokenType::Equal: op = AssignmentExpr::Op::Assign; break;
      case TokenType::PlusEqual: op = AssignmentExpr::Op::AddAssign; break;
      case TokenType::MinusEqual: op = AssignmentExpr::Op::SubAssign; break;
      case TokenType::StarEqual: op = AssignmentExpr::Op::MulAssign; break;
      case TokenType::SlashEqual: op = AssignmentExpr::Op::DivAssign; break;
      case TokenType::AmpAmpEqual: op = AssignmentExpr::Op::AndAssign; break;
      case TokenType::PipePipeEqual: op = AssignmentExpr::Op::OrAssign; break;
      case TokenType::QuestionQuestionEqual: op = AssignmentExpr::Op::NullishAssign; break;
      default: op = AssignmentExpr::Op::Assign; break;
    }
    advance();

    auto right = parseAssignment();
    return std::make_unique<Expression>(AssignmentExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseConditional() {
  auto expr = parseNullishCoalescing();

  if (match(TokenType::Question)) {
    advance();
    auto consequent = parseExpression();
    expect(TokenType::Colon);
    auto alternate = parseExpression();
    return std::make_unique<Expression>(ConditionalExpr{
      std::move(expr),
      std::move(consequent),
      std::move(alternate)
    });
  }

  return expr;
}

ExprPtr Parser::parseNullishCoalescing() {
  auto left = parseLogicalOr();

  while (match(TokenType::QuestionQuestion)) {
    advance();
    auto right = parseLogicalOr();
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::NullishCoalescing,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseLogicalOr() {
  auto left = parseLogicalAnd();

  while (match(TokenType::PipePipe)) {
    advance();
    auto right = parseLogicalAnd();
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::LogicalOr,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseLogicalAnd() {
  auto left = parseEquality();

  while (match(TokenType::AmpAmp)) {
    advance();
    auto right = parseEquality();
    left = std::make_unique<Expression>(BinaryExpr{
      BinaryExpr::Op::LogicalAnd,
      std::move(left),
      std::move(right)
    });
  }

  return left;
}

ExprPtr Parser::parseEquality() {
  auto left = parseRelational();

  while (match(TokenType::EqualEqual) || match(TokenType::EqualEqualEqual) ||
         match(TokenType::BangEqual) || match(TokenType::BangEqualEqual)) {

    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::EqualEqual: op = BinaryExpr::Op::Equal; break;
      case TokenType::EqualEqualEqual: op = BinaryExpr::Op::StrictEqual; break;
      case TokenType::BangEqual: op = BinaryExpr::Op::NotEqual; break;
      case TokenType::BangEqualEqual: op = BinaryExpr::Op::StrictNotEqual; break;
      default: op = BinaryExpr::Op::Equal; break;
    }
    advance();

    auto right = parseRelational();
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseRelational() {
  auto left = parseAdditive();

  while (match(TokenType::Less) || match(TokenType::Greater) ||
         match(TokenType::LessEqual) || match(TokenType::GreaterEqual)) {

    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::Less: op = BinaryExpr::Op::Less; break;
      case TokenType::Greater: op = BinaryExpr::Op::Greater; break;
      case TokenType::LessEqual: op = BinaryExpr::Op::LessEqual; break;
      case TokenType::GreaterEqual: op = BinaryExpr::Op::GreaterEqual; break;
      default: op = BinaryExpr::Op::Less; break;
    }
    advance();

    auto right = parseAdditive();
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseAdditive() {
  auto left = parseMultiplicative();

  while (match(TokenType::Plus) || match(TokenType::Minus)) {
    BinaryExpr::Op op = match(TokenType::Plus) ? BinaryExpr::Op::Add : BinaryExpr::Op::Sub;
    advance();
    auto right = parseMultiplicative();
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseMultiplicative() {
  auto left = parseExponentiation();

  while (match(TokenType::Star) || match(TokenType::Slash) || match(TokenType::Percent)) {
    BinaryExpr::Op op;
    switch (current().type) {
      case TokenType::Star: op = BinaryExpr::Op::Mul; break;
      case TokenType::Slash: op = BinaryExpr::Op::Div; break;
      case TokenType::Percent: op = BinaryExpr::Op::Mod; break;
      default: op = BinaryExpr::Op::Mul; break;
    }
    advance();
    auto right = parseExponentiation();
    left = std::make_unique<Expression>(BinaryExpr{op, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseExponentiation() {
  auto left = parseUnary();

  // Right-to-left associativity for **
  if (match(TokenType::StarStar)) {
    advance();
    auto right = parseExponentiation();  // Right associative
    return std::make_unique<Expression>(BinaryExpr{BinaryExpr::Op::Exp, std::move(left), std::move(right)});
  }

  return left;
}

ExprPtr Parser::parseUnary() {
  if (match(TokenType::Await)) {
    advance();
    auto argument = parseUnary();
    return std::make_unique<Expression>(AwaitExpr{std::move(argument)});
  }

  if (match(TokenType::Yield)) {
    advance();
    bool delegate = false;

    // Check for yield* (delegate to another iterator)
    if (match(TokenType::Star)) {
      delegate = true;
      advance();
    }

    // yield can be used without an argument
    ExprPtr argument = nullptr;
    if (!match(TokenType::Semicolon) && !match(TokenType::RightBrace) &&
        !match(TokenType::RightParen) && !match(TokenType::Comma)) {
      argument = parseAssignment();
    }

    return std::make_unique<Expression>(YieldExpr{std::move(argument), delegate});
  }

  if (match(TokenType::Bang) || match(TokenType::Minus) ||
      match(TokenType::Plus) || match(TokenType::Typeof)) {

    UnaryExpr::Op op;
    switch (current().type) {
      case TokenType::Bang: op = UnaryExpr::Op::Not; break;
      case TokenType::Minus: op = UnaryExpr::Op::Minus; break;
      case TokenType::Plus: op = UnaryExpr::Op::Plus; break;
      case TokenType::Typeof: op = UnaryExpr::Op::Typeof; break;
      default: op = UnaryExpr::Op::Not; break;
    }
    advance();

    auto argument = parseUnary();
    return std::make_unique<Expression>(UnaryExpr{op, std::move(argument)});
  }

  if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
    UpdateExpr::Op op = match(TokenType::PlusPlus) ?
      UpdateExpr::Op::Increment : UpdateExpr::Op::Decrement;
    advance();
    auto argument = parseUnary();
    return std::make_unique<Expression>(UpdateExpr{op, std::move(argument), true});
  }

  return parsePostfix();
}

ExprPtr Parser::parsePostfix() {
  auto expr = parseCall();

  if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
    UpdateExpr::Op op = match(TokenType::PlusPlus) ?
      UpdateExpr::Op::Increment : UpdateExpr::Op::Decrement;
    advance();
    return std::make_unique<Expression>(UpdateExpr{op, std::move(expr), false});
  }

  return expr;
}

ExprPtr Parser::parseCall() {
  auto expr = parsePrimary();
  expr = parseMemberSuffix(std::move(expr));

  while (true) {
    if (match(TokenType::LeftParen)) {
      advance();
      std::vector<ExprPtr> args;

      while (!match(TokenType::RightParen)) {
        if (!args.empty()) {
          expect(TokenType::Comma);
        }

        if (match(TokenType::DotDotDot)) {
          advance();
          auto arg = parseExpression();
          args.push_back(std::make_unique<Expression>(SpreadElement{std::move(arg)}));
        } else {
          args.push_back(parseExpression());
        }
      }

      expect(TokenType::RightParen);
      expr = std::make_unique<Expression>(CallExpr{std::move(expr), std::move(args)});
      expr = parseMemberSuffix(std::move(expr));
    } else {
      break;
    }
  }

  return expr;
}

ExprPtr Parser::parseMember() {
  auto expr = parsePrimary();
  return parseMemberSuffix(std::move(expr));
}

ExprPtr Parser::parseMemberSuffix(ExprPtr expr) {
  while (true) {
    if (match(TokenType::Dot) || match(TokenType::QuestionDot)) {
      bool isOptional = match(TokenType::QuestionDot);
      advance();
      if (match(TokenType::Identifier) || match(TokenType::From) || match(TokenType::Of)) {
        auto prop = std::make_unique<Expression>(Identifier{current().value});
        advance();
        MemberExpr member;
        member.object = std::move(expr);
        member.property = std::move(prop);
        member.computed = false;
        member.optional = isOptional;
        expr = std::make_unique<Expression>(std::move(member));
      }
    } else if (match(TokenType::LeftBracket)) {
      advance();
      auto prop = parseExpression();
      expect(TokenType::RightBracket);
      MemberExpr member;
      member.object = std::move(expr);
      member.property = std::move(prop);
      member.computed = true;
      member.optional = false;
      expr = std::make_unique<Expression>(std::move(member));
    } else {
      break;
    }
  }
  return expr;
}

ExprPtr Parser::parsePrimary() {
  if (match(TokenType::Number)) {
    double value = std::stod(current().value);
    advance();
    return std::make_unique<Expression>(NumberLiteral{value});
  }

  if (match(TokenType::BigInt)) {
    int64_t value = std::stoll(current().value);
    advance();
    return std::make_unique<Expression>(BigIntLiteral{value});
  }

  if (match(TokenType::String)) {
    std::string value = current().value;
    advance();
    return std::make_unique<Expression>(StringLiteral{value});
  }

  if (match(TokenType::TemplateLiteral)) {
    std::string content = current().value;
    advance();

    // Parse the template literal content to extract quasis and expressions
    std::vector<std::string> quasis;
    std::vector<ExprPtr> expressions;

    std::string currentQuasi;
    size_t i = 0;
    while (i < content.length()) {
      if (i + 1 < content.length() && content[i] == '$' && content[i+1] == '{') {
        // Found interpolation start
        quasis.push_back(currentQuasi);
        currentQuasi.clear();

        // Find the matching closing brace
        i += 2;
        int braceCount = 1;
        std::string exprStr;
        while (i < content.length() && braceCount > 0) {
          if (content[i] == '{') braceCount++;
          else if (content[i] == '}') braceCount--;

          if (braceCount > 0) {
            exprStr += content[i];
          }
          i++;
        }

        // Parse the expression
        Lexer exprLexer(exprStr);
        auto exprTokens = exprLexer.tokenize();
        Parser exprParser(exprTokens);
        if (auto expr = exprParser.parseExpression()) {
          expressions.push_back(std::move(expr));
        }
      } else {
        currentQuasi += content[i];
        i++;
      }
    }
    quasis.push_back(currentQuasi);

    return std::make_unique<Expression>(TemplateLiteral{std::move(quasis), std::move(expressions)});
  }

  if (match(TokenType::Regex)) {
    std::string value = current().value;
    advance();
    size_t sep = value.find("||");
    std::string pattern = value.substr(0, sep);
    std::string flags = (sep != std::string::npos) ? value.substr(sep + 2) : "";
    return std::make_unique<Expression>(RegexLiteral{pattern, flags});
  }

  if (match(TokenType::True)) {
    advance();
    return std::make_unique<Expression>(BoolLiteral{true});
  }

  if (match(TokenType::False)) {
    advance();
    return std::make_unique<Expression>(BoolLiteral{false});
  }

  if (match(TokenType::Null)) {
    advance();
    return std::make_unique<Expression>(NullLiteral{});
  }

  if (match(TokenType::Identifier)) {
    std::string name = current().value;
    advance();
    return std::make_unique<Expression>(Identifier{name});
  }

  // Dynamic import: import(specifier)
  if (match(TokenType::Import)) {
    advance();
    if (match(TokenType::LeftParen)) {
      // This is a dynamic import expression
      // Create a CallExpr with "import" as the callee identifier
      auto importId = std::make_unique<Expression>(Identifier{"import"});

      advance(); // consume '('
      std::vector<ExprPtr> args;

      if (!match(TokenType::RightParen)) {
        args.push_back(parseExpression());
        while (match(TokenType::Comma)) {
          advance();
          if (match(TokenType::RightParen)) break;
          args.push_back(parseExpression());
        }
      }

      expect(TokenType::RightParen);
      return std::make_unique<Expression>(CallExpr{std::move(importId), std::move(args)});
    }
    // If not followed by '(', it's a static import statement (error here)
    return nullptr;
  }

  if (match(TokenType::Async)) {
    if (peek().type == TokenType::Function) {
      return parseFunctionExpression();
    }
  }

  if (match(TokenType::Function)) {
    return parseFunctionExpression();
  }

  if (match(TokenType::Class)) {
    return parseClassExpression();
  }

  if (match(TokenType::New)) {
    return parseNewExpression();
  }

  if (match(TokenType::This)) {
    advance();
    return std::make_unique<Expression>(ThisExpr{});
  }

  if (match(TokenType::Super)) {
    advance();
    return std::make_unique<Expression>(SuperExpr{});
  }

  if (match(TokenType::LeftParen)) {
    advance();
    auto expr = parseExpression();
    expect(TokenType::RightParen);
    return expr;
  }

  if (match(TokenType::LeftBracket)) {
    return parseArrayExpression();
  }

  if (match(TokenType::LeftBrace)) {
    return parseObjectExpression();
  }

  return nullptr;
}

ExprPtr Parser::parseArrayExpression() {
  expect(TokenType::LeftBracket);

  std::vector<ExprPtr> elements;
  while (!match(TokenType::RightBracket)) {
    if (!elements.empty()) {
      expect(TokenType::Comma);
      if (match(TokenType::RightBracket)) break;
    }

    // Check for spread element
    if (match(TokenType::DotDotDot)) {
      advance();
      auto arg = parseExpression();
      elements.push_back(std::make_unique<Expression>(SpreadElement{std::move(arg)}));
    } else {
      elements.push_back(parseExpression());
    }
  }

  expect(TokenType::RightBracket);
  return std::make_unique<Expression>(ArrayExpr{std::move(elements)});
}

ExprPtr Parser::parseObjectExpression() {
  expect(TokenType::LeftBrace);

  std::vector<ObjectProperty> properties;
  while (!match(TokenType::RightBrace)) {
    if (!properties.empty()) {
      expect(TokenType::Comma);
      if (match(TokenType::RightBrace)) break;
    }

    // Check for spread syntax
    if (match(TokenType::DotDotDot)) {
      advance();
      auto spreadExpr = parseExpression();
      ObjectProperty prop;
      prop.isSpread = true;
      prop.value = std::move(spreadExpr);
      // key is nullptr for spread properties
      properties.push_back(std::move(prop));
    } else {
      ExprPtr key;
      bool isComputed = false;

      // Check for computed property name [expr]
      if (match(TokenType::LeftBracket)) {
        advance();
        key = parseExpression();
        expect(TokenType::RightBracket);
        isComputed = true;
      } else if (match(TokenType::Identifier)) {
        std::string identName = current().value;
        key = std::make_unique<Expression>(Identifier{identName});
        advance();

        // Check for shorthand property notation
        if (match(TokenType::Comma) || match(TokenType::RightBrace)) {
          // Shorthand: {x} means {x: x}
          ObjectProperty prop;
          prop.key = std::move(key);
          prop.value = std::make_unique<Expression>(Identifier{identName});
          prop.isSpread = false;
          properties.push_back(std::move(prop));
          continue;
        }
      } else if (match(TokenType::String)) {
        key = std::make_unique<Expression>(StringLiteral{current().value});
        advance();
      } else if (match(TokenType::Number)) {
        key = std::make_unique<Expression>(NumberLiteral{std::stod(current().value)});
        advance();
      }

      expect(TokenType::Colon);
      auto value = parseExpression();

      ObjectProperty prop;
      prop.key = std::move(key);
      prop.value = std::move(value);
      prop.isSpread = false;
      prop.isComputed = isComputed;
      properties.push_back(std::move(prop));
    }
  }

  expect(TokenType::RightBrace);
  return std::make_unique<Expression>(ObjectExpr{std::move(properties)});
}

ExprPtr Parser::parseFunctionExpression() {
  bool isAsync = false;
  if (match(TokenType::Async)) {
    isAsync = true;
    advance();
  }

  expect(TokenType::Function);

  // Check for generator function (function*)
  bool isGenerator = false;
  if (match(TokenType::Star)) {
    isGenerator = true;
    advance();
  }

  std::string name;
  if (match(TokenType::Identifier)) {
    name = current().value;
    advance();
  }

  expect(TokenType::LeftParen);

  std::vector<Parameter> params;
  std::optional<Identifier> restParam;

  while (!match(TokenType::RightParen)) {
    if (!params.empty()) {
      expect(TokenType::Comma);
    }

    // Check for rest parameter
    if (match(TokenType::DotDotDot)) {
      advance();
      if (match(TokenType::Identifier)) {
        restParam = Identifier{current().value};
        advance();
      }
      // Rest parameter must be last
      break;
    } else if (match(TokenType::Identifier)) {
      Parameter param;
      param.name = Identifier{current().value};
      advance();

      // Check for default value
      if (match(TokenType::Equal)) {
        advance();
        param.defaultValue = parseAssignment();
      }

      params.push_back(std::move(param));
    } else {
      // Unexpected token in parameter list
      break;
    }
  }

  expect(TokenType::RightParen);

  auto block = parseBlockStatement();
  auto blockStmt = std::get_if<BlockStmt>(&block->node);

  FunctionExpr funcExpr;
  funcExpr.params = std::move(params);
  funcExpr.restParam = restParam;
  funcExpr.name = name;
  funcExpr.isAsync = isAsync;
  funcExpr.isGenerator = isGenerator;
  if (blockStmt) {
    funcExpr.body = std::move(blockStmt->body);
  }

  return std::make_unique<Expression>(std::move(funcExpr));
}

ExprPtr Parser::parseClassExpression() {
  expect(TokenType::Class);

  std::string className;
  if (match(TokenType::Identifier)) {
    className = current().value;
    advance();
  }

  ExprPtr superClass;
  if (match(TokenType::Extends)) {
    advance();
    superClass = parsePrimary();
  }

  expect(TokenType::LeftBrace);

  std::vector<MethodDefinition> methods;
  while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
    MethodDefinition method;

    // Check for static
    if (match(TokenType::Static)) {
      method.isStatic = true;
      advance();
    }

    // Check for async
    if (match(TokenType::Async)) {
      method.isAsync = true;
      advance();
    }

    // Check for getter/setter
    if (match(TokenType::Get)) {
      method.kind = MethodDefinition::Kind::Get;
      advance();
    } else if (match(TokenType::Set)) {
      method.kind = MethodDefinition::Kind::Set;
      advance();
    }

    // Method name
    if (match(TokenType::Identifier)) {
      std::string methodName = current().value;
      if (methodName == "constructor") {
        method.kind = MethodDefinition::Kind::Constructor;
      }
      method.key.name = methodName;
      advance();
    } else {
      return nullptr;
    }

    // Parameters
    expect(TokenType::LeftParen);
    while (!match(TokenType::RightParen)) {
      if (match(TokenType::Identifier)) {
        method.params.push_back({current().value});
        advance();
        if (match(TokenType::Comma)) {
          advance();
        }
      } else {
        break;
      }
    }
    expect(TokenType::RightParen);

    // Method body
    expect(TokenType::LeftBrace);
    while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
      method.body.push_back(parseStatement());
    }
    expect(TokenType::RightBrace);

    methods.push_back(std::move(method));
  }
  expect(TokenType::RightBrace);

  ClassExpr classExpr;
  classExpr.name = className;
  classExpr.superClass = std::move(superClass);
  classExpr.methods = std::move(methods);

  return std::make_unique<Expression>(std::move(classExpr));
}

ExprPtr Parser::parseNewExpression() {
  expect(TokenType::New);

  auto callee = parseMember();

  std::vector<ExprPtr> args;
  if (match(TokenType::LeftParen)) {
    advance();
    while (!match(TokenType::RightParen)) {
      args.push_back(parseAssignment());
      if (!match(TokenType::RightParen)) {
        expect(TokenType::Comma);
      }
    }
    expect(TokenType::RightParen);
  }

  NewExpr newExpr;
  newExpr.callee = std::move(callee);
  newExpr.arguments = std::move(args);

  return std::make_unique<Expression>(std::move(newExpr));
}

ExprPtr Parser::parsePattern() {
  // Check for array destructuring pattern
  if (match(TokenType::LeftBracket)) {
    return parseArrayPattern();
  }

  // Check for object destructuring pattern
  if (match(TokenType::LeftBrace)) {
    return parseObjectPattern();
  }

  // Otherwise it must be an identifier
  if (match(TokenType::Identifier)) {
    std::string name = current().value;
    advance();
    return std::make_unique<Expression>(Identifier{name});
  }

  return nullptr;
}

ExprPtr Parser::parseArrayPattern() {
  expect(TokenType::LeftBracket);

  ArrayPattern pattern;

  while (!match(TokenType::RightBracket) && pos_ < tokens_.size()) {
    if (!pattern.elements.empty()) {
      expect(TokenType::Comma);
      // Allow trailing comma
      if (match(TokenType::RightBracket)) {
        break;
      }
    }

    // Check for rest element (...rest)
    if (match(TokenType::DotDotDot)) {
      advance();
      pattern.rest = parsePattern();
      if (!pattern.rest) {
        return nullptr;
      }
      // Rest must be last element
      expect(TokenType::RightBracket);
      break;
    }

    // Check for hole in array pattern (e.g., [a, , c])
    if (match(TokenType::Comma)) {
      pattern.elements.push_back(nullptr);
      continue;
    }

    // Parse nested pattern or identifier
    ExprPtr element = parsePattern();
    if (!element) {
      return nullptr;
    }
    pattern.elements.push_back(std::move(element));
  }

  expect(TokenType::RightBracket);

  return std::make_unique<Expression>(std::move(pattern));
}

ExprPtr Parser::parseObjectPattern() {
  expect(TokenType::LeftBrace);

  ObjectPattern pattern;

  while (!match(TokenType::RightBrace) && pos_ < tokens_.size()) {
    if (!pattern.properties.empty()) {
      expect(TokenType::Comma);
      // Allow trailing comma
      if (match(TokenType::RightBrace)) {
        break;
      }
    }

    // Check for rest properties (...rest)
    if (match(TokenType::DotDotDot)) {
      advance();
      pattern.rest = parsePattern();
      if (!pattern.rest) {
        return nullptr;
      }
      // Rest must be last element
      expect(TokenType::RightBrace);
      break;
    }

    // Parse property key
    ExprPtr key;
    if (match(TokenType::Identifier)) {
      std::string name = current().value;
      advance();
      key = std::make_unique<Expression>(Identifier{name});
    } else if (match(TokenType::String)) {
      std::string value = current().value;
      advance();
      key = std::make_unique<Expression>(StringLiteral{value});
    } else {
      return nullptr;
    }

    // Check for shorthand property (e.g., {x} instead of {x: x})
    ExprPtr value;
    if (match(TokenType::Colon)) {
      advance();
      value = parsePattern();
      if (!value) {
        return nullptr;
      }
    } else {
      // Shorthand - use key as both key and value pattern
      if (auto* id = std::get_if<Identifier>(&key->node)) {
        value = std::make_unique<Expression>(Identifier{id->name});
      } else {
        return nullptr;  // Shorthand only works with identifiers
      }
    }

    ObjectPattern::Property prop;
    prop.key = std::move(key);
    prop.value = std::move(value);
    pattern.properties.push_back(std::move(prop));
  }

  expect(TokenType::RightBrace);

  return std::make_unique<Expression>(std::move(pattern));
}

}
