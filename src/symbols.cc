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

const Value& WellKnownSymbols::hasInstance() {
  static Value s(Symbol("Symbol.hasInstance"));
  return s;
}
const std::string& WellKnownSymbols::hasInstanceKey() {
  static const std::string key = WellKnownSymbols::hasInstance().toString();
  return key;
}

const Value& WellKnownSymbols::species() {
  static Value s(Symbol("Symbol.species"));
  return s;
}
const std::string& WellKnownSymbols::speciesKey() {
  static const std::string key = WellKnownSymbols::species().toString();
  return key;
}

const Value& WellKnownSymbols::isConcatSpreadable() {
  static Value s(Symbol("Symbol.isConcatSpreadable"));
  return s;
}
const std::string& WellKnownSymbols::isConcatSpreadableKey() {
  static const std::string key = WellKnownSymbols::isConcatSpreadable().toString();
  return key;
}

const Value& WellKnownSymbols::match() {
  static Value s(Symbol("Symbol.match"));
  return s;
}
const std::string& WellKnownSymbols::matchKey() {
  static const std::string key = WellKnownSymbols::match().toString();
  return key;
}

const Value& WellKnownSymbols::replace() {
  static Value s(Symbol("Symbol.replace"));
  return s;
}
const std::string& WellKnownSymbols::replaceKey() {
  static const std::string key = WellKnownSymbols::replace().toString();
  return key;
}

const Value& WellKnownSymbols::search() {
  static Value s(Symbol("Symbol.search"));
  return s;
}
const std::string& WellKnownSymbols::searchKey() {
  static const std::string key = WellKnownSymbols::search().toString();
  return key;
}

const Value& WellKnownSymbols::split() {
  static Value s(Symbol("Symbol.split"));
  return s;
}
const std::string& WellKnownSymbols::splitKey() {
  static const std::string key = WellKnownSymbols::split().toString();
  return key;
}

}
