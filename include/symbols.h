#pragma once

#include "value.h"

namespace tinyjs {

class WellKnownSymbols {
public:
  static const Value& iterator();
  static const std::string& iteratorKey();
};

}
