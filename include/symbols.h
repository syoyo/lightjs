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
};

}
