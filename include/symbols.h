#pragma once

#include "value.h"

namespace lightjs {

class WellKnownSymbols {
public:
  static const Value& iterator();
  static const std::string& iteratorKey();
};

}
