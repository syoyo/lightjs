#pragma once

#include "value.h"

namespace lightjs {

class WellKnownSymbols {
public:
  static const Value& iterator();
  static const std::string& iteratorKey();
  static const Value& asyncIterator();
  static const std::string& asyncIteratorKey();
  static const Value& toStringTag();
  static const std::string& toStringTagKey();
  static const Value& toPrimitive();
  static const std::string& toPrimitiveKey();
  static const Value& matchAll();
  static const std::string& matchAllKey();
  static const Value& unscopables();
  static const std::string& unscopablesKey();
  static const Value& hasInstance();
  static const std::string& hasInstanceKey();
  static const Value& species();
  static const std::string& speciesKey();
  static const Value& isConcatSpreadable();
  static const std::string& isConcatSpreadableKey();
  static const Value& match();
  static const std::string& matchKey();
  static const Value& replace();
  static const std::string& replaceKey();
  static const Value& search();
  static const std::string& searchKey();
  static const Value& split();
  static const std::string& splitKey();
};

}
