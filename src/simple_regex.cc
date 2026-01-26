#include "simple_regex.h"

namespace lightjs {
namespace simple_regex {

Regex::Regex(const std::string& pattern, bool caseInsensitive)
  : pattern_(pattern), caseInsensitive_(caseInsensitive), pos_(0), root_(nullptr) {
  root_ = parse();
}

Regex::Node* Regex::parse() {
  pos_ = 0;
  return parseAlternate();
}

Regex::Node* Regex::parseAlternate() {
  Node* left = parseConcat();

  while (pos_ < pattern_.size() && pattern_[pos_] == '|') {
    pos_++;
    Node* alt = new Node(NodeType::Alternate);
    alt->left = left;
    alt->right = parseConcat();
    left = alt;
  }

  return left;
}

Regex::Node* Regex::parseConcat() {
  Node* left = parseSuffix();

  while (pos_ < pattern_.size() && pattern_[pos_] != ')' && pattern_[pos_] != '|') {
    Node* right = parseSuffix();
    if (right == nullptr) break;

    Node* concat = new Node(NodeType::Concat);
    concat->left = left;
    concat->right = right;
    left = concat;
  }

  return left;
}

Regex::Node* Regex::parseSuffix() {
  Node* node = parsePrimary();
  if (node == nullptr) return nullptr;

  if (pos_ < pattern_.size()) {
    if (pattern_[pos_] == '*') {
      pos_++;
      Node* star = new Node(NodeType::Star);
      star->left = node;
      return star;
    } else if (pattern_[pos_] == '+') {
      pos_++;
      Node* plus = new Node(NodeType::Plus);
      plus->left = node;
      return plus;
    } else if (pattern_[pos_] == '?') {
      pos_++;
      Node* question = new Node(NodeType::Question);
      question->left = node;
      return question;
    }
  }

  return node;
}

Regex::Node* Regex::parsePrimary() {
  if (pos_ >= pattern_.size()) return nullptr;

  char c = pattern_[pos_];

  if (c == '^') {
    pos_++;
    return new Node(NodeType::StartAnchor);
  }

  if (c == '$') {
    pos_++;
    return new Node(NodeType::EndAnchor);
  }

  if (c == '.') {
    pos_++;
    return new Node(NodeType::Any);
  }

  if (c == '[') {
    return parseCharClass();
  }

  if (c == '(') {
    pos_++;
    Node* node = parseAlternate();
    if (pos_ < pattern_.size() && pattern_[pos_] == ')') {
      pos_++;
    }
    return node;
  }

  if (c == ')' || c == '|' || c == '*' || c == '+' || c == '?') {
    return nullptr;
  }

  if (c == '\\' && pos_ + 1 < pattern_.size()) {
    pos_++;
    c = pattern_[pos_];
  }

  pos_++;
  Node* node = new Node(NodeType::Literal);
  node->literal = c;
  return node;
}

Regex::Node* Regex::parseCharClass() {
  if (pos_ >= pattern_.size() || pattern_[pos_] != '[') {
    return nullptr;
  }

  pos_++;
  Node* node = new Node(NodeType::CharClass);

  if (pos_ < pattern_.size() && pattern_[pos_] == '^') {
    node->charClass.negated = true;
    pos_++;
  }

  while (pos_ < pattern_.size() && pattern_[pos_] != ']') {
    char c = pattern_[pos_];

    if (c == '\\' && pos_ + 1 < pattern_.size()) {
      pos_++;
      c = pattern_[pos_];
    }

    if (pos_ + 2 < pattern_.size() && pattern_[pos_ + 1] == '-' && pattern_[pos_ + 2] != ']') {
      char start = c;
      pos_ += 2;
      char end = pattern_[pos_];
      if (end == '\\' && pos_ + 1 < pattern_.size()) {
        pos_++;
        end = pattern_[pos_];
      }
      node->charClass.ranges.push_back({start, end});
    } else {
      node->charClass.chars.push_back(c);
    }

    pos_++;
  }

  if (pos_ < pattern_.size() && pattern_[pos_] == ']') {
    pos_++;
  }

  return node;
}

bool Regex::search(const std::string& text) const {
  if (root_ == nullptr) return false;

  if (root_->type == NodeType::StartAnchor) {
    size_t endPos = 0;
    return matchInternal(root_->left ? root_->left : root_, text, 0, endPos);
  }

  for (size_t i = 0; i <= text.size(); ++i) {
    size_t endPos = 0;
    if (matchInternal(root_, text, i, endPos)) {
      return true;
    }
  }

  return false;
}

bool Regex::search(const std::string& text, std::vector<Match>& matches) const {
  matches.clear();
  if (root_ == nullptr) return false;

  if (root_->type == NodeType::StartAnchor) {
    size_t endPos = 0;
    if (matchInternal(root_->left ? root_->left : root_, text, 0, endPos)) {
      Match m;
      m.str = text.substr(0, endPos);
      m.position = 0;
      m.length = endPos;
      matches.push_back(m);
      return true;
    }
    return false;
  }

  for (size_t i = 0; i <= text.size(); ++i) {
    size_t endPos = 0;
    if (matchInternal(root_, text, i, endPos)) {
      Match m;
      m.str = text.substr(i, endPos - i);
      m.position = i;
      m.length = endPos - i;
      matches.push_back(m);
      return true;
    }
  }

  return false;
}

std::string Regex::replace(const std::string& text, const std::string& replacement) const {
  std::vector<Match> matches;
  if (!search(text, matches)) {
    return text;
  }

  const Match& match = matches[0];
  std::string result;
  result.reserve(text.size());

  result.append(text, 0, match.position);
  result.append(replacement);
  result.append(text, match.position + match.length, std::string::npos);

  return result;
}

bool Regex::matchInternal(const Node* node, const std::string& text, size_t pos, size_t& endPos) const {
  if (node == nullptr) {
    endPos = pos;
    return true;
  }

  switch (node->type) {
    case NodeType::Literal:
      if (pos < text.size() && charsEqual(text[pos], node->literal)) {
        endPos = pos + 1;
        return true;
      }
      return false;

    case NodeType::Any:
      if (pos < text.size() && text[pos] != '\n') {
        endPos = pos + 1;
        return true;
      }
      return false;

    case NodeType::CharClass: {
      if (pos >= text.size()) return false;
      char c = text[pos];
      if (caseInsensitive_) c = toLower(c);

      CharClass cc = node->charClass;
      if (caseInsensitive_) {
        for (char& ch : cc.chars) {
          ch = toLower(ch);
        }
        for (auto& range : cc.ranges) {
          range.first = toLower(range.first);
          range.second = toLower(range.second);
        }
      }

      if (cc.matches(c)) {
        endPos = pos + 1;
        return true;
      }
      return false;
    }

    case NodeType::Star:
      return matchStar(node, text, pos, endPos);

    case NodeType::Plus:
      return matchPlus(node, text, pos, endPos);

    case NodeType::Question:
      return matchQuestion(node, text, pos, endPos);

    case NodeType::Concat: {
      size_t midPos = 0;
      if (matchInternal(node->left, text, pos, midPos)) {
        return matchInternal(node->right, text, midPos, endPos);
      }
      return false;
    }

    case NodeType::Alternate:
      if (matchInternal(node->left, text, pos, endPos)) {
        return true;
      }
      return matchInternal(node->right, text, pos, endPos);

    case NodeType::StartAnchor:
      if (pos == 0) {
        endPos = pos;
        return true;
      }
      return false;

    case NodeType::EndAnchor:
      if (pos == text.size()) {
        endPos = pos;
        return true;
      }
      return false;
  }

  return false;
}

bool Regex::matchStar(const Node* node, const std::string& text, size_t pos, size_t& endPos) const {
  endPos = pos;

  std::vector<size_t> positions;
  positions.push_back(pos);

  size_t currentPos = pos;
  while (true) {
    size_t nextPos = 0;
    if (matchInternal(node->left, text, currentPos, nextPos)) {
      currentPos = nextPos;
      positions.push_back(currentPos);
    } else {
      break;
    }
  }

  for (size_t i = positions.size(); i > 0; --i) {
    endPos = positions[i - 1];
    return true;
  }

  return false;
}

bool Regex::matchPlus(const Node* node, const std::string& text, size_t pos, size_t& endPos) const {
  size_t firstPos = 0;
  if (!matchInternal(node->left, text, pos, firstPos)) {
    return false;
  }

  return matchStar(node, text, firstPos, endPos) || (endPos = firstPos, true);
}

bool Regex::matchQuestion(const Node* node, const std::string& text, size_t pos, size_t& endPos) const {
  size_t matchPos = 0;
  if (matchInternal(node->left, text, pos, matchPos)) {
    endPos = matchPos;
    return true;
  }

  endPos = pos;
  return true;
}

}
}