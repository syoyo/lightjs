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

const Value& WellKnownSymbols::asyncIterator() {
  static Value symbolAsyncIterator(Symbol("Symbol.asyncIterator"));
  return symbolAsyncIterator;
}

const std::string& WellKnownSymbols::asyncIteratorKey() {
  static const std::string key = WellKnownSymbols::asyncIterator().toString();
  return key;
}

const Value& WellKnownSymbols::toStringTag() {
  static Value symbolToStringTag(Symbol("Symbol.toStringTag"));
  return symbolToStringTag;
}

const std::string& WellKnownSymbols::toStringTagKey() {
  static const std::string key = WellKnownSymbols::toStringTag().toString();
  return key;
}

}
