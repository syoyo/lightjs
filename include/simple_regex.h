#pragma once

#include <string>
#include <vector>

namespace lightjs {
namespace simple_regex {

class Regex {
public:
  Regex(const std::string& pattern, bool caseInsensitive = false);

  bool search(const std::string& text) const;

  struct Match {
    std::string str;
    size_t position;
    size_t length;
  };

  bool search(const std::string& text, std::vector<Match>& matches) const;

  std::string replace(const std::string& text, const std::string& replacement) const;

private:
  std::string pattern_;
  bool caseInsensitive_;

  enum class NodeType {
    Literal,
    Any,
    CharClass,
    Star,
    Plus,
    Question,
    Concat,
    Alternate,
    StartAnchor,
    EndAnchor
  };

  struct CharClass {
    std::vector<char> chars;
    std::vector<std::pair<char, char>> ranges;
    bool negated;

    CharClass() : negated(false) {}

    bool matches(char c) const {
      bool found = false;
      for (char ch : chars) {
        if (ch == c) {
          found = true;
          break;
        }
      }
      if (!found) {
        for (const auto& range : ranges) {
          if (c >= range.first && c <= range.second) {
            found = true;
            break;
          }
        }
      }
      return negated ? !found : found;
    }
  };

  struct Node {
    NodeType type;
    char literal;
    CharClass charClass;
    Node* left;
    Node* right;

    Node(NodeType t) : type(t), literal('\0'), left(nullptr), right(nullptr) {}
    ~Node() {
      delete left;
      delete right;
    }
  };

  Node* root_;

  Node* parse();
  Node* parseAlternate();
  Node* parseConcat();
  Node* parseSuffix();
  Node* parsePrimary();
  Node* parseCharClass();

  bool matchInternal(const Node* node, const std::string& text, size_t pos, size_t& endPos) const;
  bool matchStar(const Node* node, const std::string& text, size_t pos, size_t& endPos) const;
  bool matchPlus(const Node* node, const std::string& text, size_t pos, size_t& endPos) const;
  bool matchQuestion(const Node* node, const std::string& text, size_t pos, size_t& endPos) const;

  char toLower(char c) const {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
  }

  bool charsEqual(char a, char b) const {
    if (caseInsensitive_) {
      return toLower(a) == toLower(b);
    }
    return a == b;
  }

  size_t pos_;
};

}
}