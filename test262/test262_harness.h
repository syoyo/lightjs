#pragma once

#include "environment.h"
#include <memory>

namespace lightjs {

void installTest262Harness(std::shared_ptr<Environment> env);
std::shared_ptr<Environment> createTest262Environment();

} // namespace lightjs