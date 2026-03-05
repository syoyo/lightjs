#include "symbols.h"

namespace lightjs {

const Value& WellKnownSymbols::iterator() {
  static Value symbolIterator(Symbol("Symbol.iterator"));
  return symbolIterator;
}

const std::string& WellKnownSymbols::iteratorKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::iterator());
  return key;
}

const Value& WellKnownSymbols::asyncIterator() {
  static Value symbolAsyncIterator(Symbol("Symbol.asyncIterator"));
  return symbolAsyncIterator;
}

const std::string& WellKnownSymbols::asyncIteratorKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::asyncIterator());
  return key;
}

const Value& WellKnownSymbols::toStringTag() {
  static Value symbolToStringTag(Symbol("Symbol.toStringTag"));
  return symbolToStringTag;
}

const std::string& WellKnownSymbols::toStringTagKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::toStringTag());
  return key;
}

const Value& WellKnownSymbols::toPrimitive() {
  static Value symbolToPrimitive(Symbol("Symbol.toPrimitive"));
  return symbolToPrimitive;
}

const std::string& WellKnownSymbols::toPrimitiveKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::toPrimitive());
  return key;
}

const Value& WellKnownSymbols::matchAll() {
  static Value symbolMatchAll(Symbol("Symbol.matchAll"));
  return symbolMatchAll;
}

const std::string& WellKnownSymbols::matchAllKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::matchAll());
  return key;
}

const Value& WellKnownSymbols::unscopables() {
  static Value symbolUnscopables(Symbol("Symbol.unscopables"));
  return symbolUnscopables;
}

const std::string& WellKnownSymbols::unscopablesKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::unscopables());
  return key;
}

const Value& WellKnownSymbols::hasInstance() {
  static Value s(Symbol("Symbol.hasInstance"));
  return s;
}
const std::string& WellKnownSymbols::hasInstanceKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::hasInstance());
  return key;
}

const Value& WellKnownSymbols::species() {
  static Value s(Symbol("Symbol.species"));
  return s;
}
const std::string& WellKnownSymbols::speciesKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::species());
  return key;
}

const Value& WellKnownSymbols::isConcatSpreadable() {
  static Value s(Symbol("Symbol.isConcatSpreadable"));
  return s;
}
const std::string& WellKnownSymbols::isConcatSpreadableKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::isConcatSpreadable());
  return key;
}

const Value& WellKnownSymbols::match() {
  static Value s(Symbol("Symbol.match"));
  return s;
}
const std::string& WellKnownSymbols::matchKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::match());
  return key;
}

const Value& WellKnownSymbols::replace() {
  static Value s(Symbol("Symbol.replace"));
  return s;
}
const std::string& WellKnownSymbols::replaceKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::replace());
  return key;
}

const Value& WellKnownSymbols::search() {
  static Value s(Symbol("Symbol.search"));
  return s;
}
const std::string& WellKnownSymbols::searchKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::search());
  return key;
}

const Value& WellKnownSymbols::split() {
  static Value s(Symbol("Symbol.split"));
  return s;
}
const std::string& WellKnownSymbols::splitKey() {
  static const std::string key = valueToPropertyKey(WellKnownSymbols::split());
  return key;
}

}
