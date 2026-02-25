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

const Value& WellKnownSymbols::toPrimitive() {
  static Value symbolToPrimitive(Symbol("Symbol.toPrimitive"));
  return symbolToPrimitive;
}

const std::string& WellKnownSymbols::toPrimitiveKey() {
  static const std::string key = WellKnownSymbols::toPrimitive().toString();
  return key;
}

const Value& WellKnownSymbols::matchAll() {
  static Value symbolMatchAll(Symbol("Symbol.matchAll"));
  return symbolMatchAll;
}

const std::string& WellKnownSymbols::matchAllKey() {
  static const std::string key = WellKnownSymbols::matchAll().toString();
  return key;
}

const Value& WellKnownSymbols::unscopables() {
  static Value symbolUnscopables(Symbol("Symbol.unscopables"));
  return symbolUnscopables;
}

const std::string& WellKnownSymbols::unscopablesKey() {
  static const std::string key = WellKnownSymbols::unscopables().toString();
  return key;
}

}
