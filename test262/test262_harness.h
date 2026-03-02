#pragma once

#include "environment.h"
#include "gc.h"
#include <memory>

namespace lightjs {

void installTest262Harness(GCPtr<Environment> env);
GCPtr<Environment> createTest262Environment();

} // namespace lightjs