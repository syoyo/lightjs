#include "symbols.h"

namespace lightjs {

const Value& WellKnownSymbols::iterator() {
  static Value symbolIterator(Symbol("Symbol.iterator"));
  return symbolIterator;
}

const std::string& WellKnownSymbols::iteratorKey() {
  static const std::string key = WellKnownSymbols::iterator().toString();
  return key;
}

}
