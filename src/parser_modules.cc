#include "parser_internal.h"

namespace lightjs {

StmtPtr Parser::parseImportDeclaration() {
  expect(TokenType::Import);

  ImportDeclaration import;

  // Side-effect import: import "module";
  if (match(TokenType::String)) {
    import.source = current().value;
    advance();

    if (!parseImportAttributes(import.attributes)) {
      error_ = true;
      return nullptr;
    }

    if (!consumeSemicolonOrASI()) {
      error_ = true;
      return nullptr;
    }

    return std::make_unique<Statement>(std::move(import));
  }

  bool hasImportClause = false;

  if (match(TokenType::Identifier) &&
      !current().escaped &&
      current().value == "source" &&
      isIdentifierLikeToken(peek().type) &&
      !peek().escaped &&
      peek(2).type == TokenType::From &&
      !peek(2).escaped) {
    import.phase = ImportDeclaration::Phase::Source;
    advance();
  }

  if (match(TokenType::Identifier) &&
      !current().escaped &&
      current().value == "defer") {
    if (peek().type == TokenType::Star) {
      import.phase = ImportDeclaration::Phase::Defer;
      advance();
    } else if (peek().type != TokenType::From && peek().type != TokenType::Comma) {
      error_ = true;
      return nullptr;
    }
  }

  // Check for default import: import foo from "module"
  if (isIdentifierLikeToken(current().type)) {
    hasImportClause = true;
    if (strictMode_ && isStrictModeRestrictedIdentifier(current().value)) {
      error_ = true;
      return nullptr;
    }
    import.defaultImport = Identifier{current().value};
    advance();

    // Check for additional named imports: import foo, { bar } from "module"
    if (match(TokenType::Comma)) {
      advance();
      if (!match(TokenType::Star) && !match(TokenType::LeftBrace)) {
        error_ = true;
        return nullptr;
      }
    }
  }

  // Check for namespace import: import * as name from "module"
  if (match(TokenType::Star)) {
    hasImportClause = true;
    advance();
    if (!match(TokenType::As) || current().escaped) {
      return nullptr;
    }
    advance();
    if (!isIdentifierLikeToken(current().type)) {
      return nullptr;
    }
    if (strictMode_ && isStrictModeRestrictedIdentifier(current().value)) {
      error_ = true;
      return nullptr;
    }
    import.namespaceImport = Identifier{current().value};
    advance();
  }
  // Check for named imports: import { foo, bar as baz } from "module"
  else if (match(TokenType::LeftBrace)) {
    hasImportClause = true;
    advance();

    while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
      if (!import.specifiers.empty()) {
        if (!expect(TokenType::Comma)) {
          return nullptr;
        }
        if (match(TokenType::RightBrace)) {
          break;
        }
      }

      ImportSpecifier spec;
      if (match(TokenType::String)) {
        if (!isWellFormedUnicodeString(current().value)) {
          error_ = true;
          return nullptr;
        }
        spec.imported = Identifier{current().value};
        advance();
        if (!match(TokenType::As) || current().escaped) {
          return nullptr;
        }
        advance();
        if (!isIdentifierLikeToken(current().type)) {
          return nullptr;
        }
        spec.local = Identifier{current().value};
        advance();
      } else if (isIdentifierNameToken(current().type)) {
        spec.imported = Identifier{current().value};
        spec.local = spec.imported;
        advance();
        // Check for renaming: foo as bar
        if (match(TokenType::As)) {
          if (current().escaped) {
            return nullptr;
          }
          advance();
          if (!isIdentifierLikeToken(current().type)) {
            return nullptr;
          }
          spec.local = Identifier{current().value};
          advance();
        }
      } else {
        return nullptr;
      }

      if (strictMode_ && isStrictModeRestrictedIdentifier(spec.local.name)) {
        error_ = true;
        return nullptr;
      }

      import.specifiers.push_back(spec);
    }

    expect(TokenType::RightBrace);
  }

  if (!hasImportClause) {
    error_ = true;
    return nullptr;
  }

  if (import.phase == ImportDeclaration::Phase::Defer &&
      (!import.namespaceImport.has_value() || import.defaultImport.has_value() || !import.specifiers.empty())) {
    error_ = true;
    return nullptr;
  }

  if (import.phase == ImportDeclaration::Phase::Source &&
      (!import.defaultImport.has_value() || import.namespaceImport.has_value() || !import.specifiers.empty())) {
    error_ = true;
    return nullptr;
  }

  // Expect 'from' keyword
  if (!match(TokenType::From) || current().escaped) {
    error_ = true;
    return nullptr;
  }
  advance();

  // Expect module source string
  if (!match(TokenType::String)) {
    error_ = true;
    return nullptr;
  }
  import.source = current().value;
  advance();

  if (!parseImportAttributes(import.attributes)) {
    error_ = true;
    return nullptr;
  }

  if (!consumeSemicolonOrASI()) {
    error_ = true;
    return nullptr;
  }

  return std::make_unique<Statement>(std::move(import));
}

StmtPtr Parser::parseExportDeclaration() {
  expect(TokenType::Export);

  // Export default declaration
  if (match(TokenType::Default)) {
    if (current().escaped) {
      error_ = true;
      return nullptr;
    }
    advance();

    ExportDefaultDeclaration exportDefault;

    // Can be expression or function/class declaration
    if (match(TokenType::Function) || match(TokenType::Async)) {
      exportDefault.isHoistableDeclaration = true;
      exportDefault.declaration = parseFunctionExpression();
      if (!exportDefault.declaration) {
        error_ = true;
        return nullptr;
      }
      consumeSemicolon();
      return std::make_unique<Statement>(std::move(exportDefault));
    }

    if (match(TokenType::Class)) {
      exportDefault.declaration = parseClassExpression();
      if (!exportDefault.declaration) {
        error_ = true;
        return nullptr;
      }
      consumeSemicolon();
      return std::make_unique<Statement>(std::move(exportDefault));
    }

    if (match(TokenType::Const) || match(TokenType::Let) || match(TokenType::Var)) {
      error_ = true;
      return nullptr;
    }

    exportDefault.declaration = parseAssignment();
    if (!exportDefault.declaration) {
      error_ = true;
      return nullptr;
    }

    if (!consumeSemicolonOrASI()) {
      error_ = true;
      return nullptr;
    }

    return std::make_unique<Statement>(std::move(exportDefault));
  }

  // Export all declaration: export * from "module" or export * as name from "module"
  if (match(TokenType::Star)) {
    advance();

    ExportAllDeclaration exportAll;

    if (match(TokenType::As)) {
      if (current().escaped) {
        error_ = true;
        return nullptr;
      }
      advance();
      if (!isIdentifierNameToken(current().type) && !match(TokenType::String)) {
        error_ = true;
        return nullptr;
      }
      if (match(TokenType::String) && !isWellFormedUnicodeString(current().value)) {
        error_ = true;
        return nullptr;
      }
      exportAll.exported = Identifier{current().value};
      advance();
    }

    if (!match(TokenType::From) || current().escaped) {
      error_ = true;
      return nullptr;
    }
    advance();

    if (!match(TokenType::String)) {
      error_ = true;
      return nullptr;
    }
    exportAll.source = current().value;
    advance();

    if (!parseImportAttributes(exportAll.attributes)) {
      error_ = true;
      return nullptr;
    }

    if (!consumeSemicolonOrASI()) {
      error_ = true;
      return nullptr;
    }

    return std::make_unique<Statement>(std::move(exportAll));
  }

  // Export named declaration
  ExportNamedDeclaration exportNamed;

  // Export variable/function declaration
  if (match(TokenType::Const) || match(TokenType::Let) ||
      match(TokenType::Var) || match(TokenType::Function) ||
      match(TokenType::Async) || match(TokenType::Class)) {
    exportNamed.declaration = parseStatement();
    if (!exportNamed.declaration) {
      error_ = true;
      return nullptr;
    }
    return std::make_unique<Statement>(std::move(exportNamed));
  }

  // Export list: export { foo, bar as baz }
  if (match(TokenType::LeftBrace)) {
    advance();
    std::vector<bool> localIsString;

    while (!match(TokenType::RightBrace) && !match(TokenType::EndOfFile)) {
      if (!exportNamed.specifiers.empty()) {
        if (!expect(TokenType::Comma)) {
          error_ = true;
          return nullptr;
        }
        if (match(TokenType::RightBrace)) {
          break;
        }
      }

      bool specLocalIsString = false;
      if (!isIdentifierNameToken(current().type) && !match(TokenType::String)) {
        error_ = true;
        return nullptr;
      }
      if (match(TokenType::String)) {
        specLocalIsString = true;
        if (!isWellFormedUnicodeString(current().value)) {
          error_ = true;
          return nullptr;
        }
      }

      ExportSpecifier spec;
      spec.local = Identifier{current().value};
      spec.exported = spec.local;
      advance();

      // Check for renaming: foo as bar
      if (match(TokenType::As)) {
        if (current().escaped) {
          error_ = true;
          return nullptr;
        }
        advance();
        if (!isIdentifierNameToken(current().type) && !match(TokenType::String)) {
          error_ = true;
          return nullptr;
        }
        if (match(TokenType::String) && !isWellFormedUnicodeString(current().value)) {
          error_ = true;
          return nullptr;
        }
        spec.exported = Identifier{current().value};
        advance();
      }

      exportNamed.specifiers.push_back(spec);
      localIsString.push_back(specLocalIsString);
    }

    if (!expect(TokenType::RightBrace)) {
      error_ = true;
      return nullptr;
    }

    // Check for re-export: export { foo } from "module"
    bool hasSource = false;
    if (match(TokenType::From)) {
      if (current().escaped) {
        error_ = true;
        return nullptr;
      }
      advance();
      if (!match(TokenType::String)) {
        error_ = true;
        return nullptr;
      }
      exportNamed.source = current().value;
      advance();
      hasSource = true;

      if (!parseImportAttributes(exportNamed.attributes)) {
        error_ = true;
        return nullptr;
      }
    }

    if (!hasSource) {
      for (bool isStringLocal : localIsString) {
        if (isStringLocal) {
          error_ = true;
          return nullptr;
        }
      }
    }

    if (!consumeSemicolonOrASI()) {
      error_ = true;
      return nullptr;
    }
    return std::make_unique<Statement>(std::move(exportNamed));
  }

  error_ = true;
  return nullptr;
}


}  // namespace lightjs
