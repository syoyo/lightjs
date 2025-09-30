#pragma once

#include "environment.h"
#include <memory>

namespace tinyjs {

void installTest262Harness(std::shared_ptr<Environment> env);
std::shared_ptr<Environment> createTest262Environment();

} // namespace tinyjs