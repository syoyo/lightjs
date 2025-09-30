#include "module.h"
#include "interpreter.h"
#include "environment.h"
#include <iostream>
#include <filesystem>

using namespace tinyjs;
namespace fs = std::filesystem;

int main() {
  std::cout << "=== TinyJS Module System Test ===" << std::endl << std::endl;

  // Set up module loader
  ModuleLoader loader;
  loader.setBasePath(fs::current_path().string());

  // Load the main module
  std::string mainPath = "../examples/modules/main.js";
  auto mainModule = loader.loadModule(mainPath);

  if (!mainModule) {
    std::cerr << "Failed to load main module" << std::endl;
    return 1;
  }

  std::cout << "Module loaded: " << mainModule->getPath() << std::endl;

  // Instantiate the module (resolve imports)
  if (!mainModule->instantiate(&loader)) {
    std::cerr << "Failed to instantiate module" << std::endl;
    return 1;
  }

  std::cout << "Module instantiated successfully" << std::endl;

  // Create interpreter and evaluate
  auto env = Environment::createGlobal();
  Interpreter interpreter(env);

  if (!mainModule->evaluate(&interpreter)) {
    std::cerr << "Failed to evaluate module" << std::endl;
    return 1;
  }

  std::cout << "Module evaluated successfully" << std::endl;

  // Check exports
  auto exports = mainModule->getAllExports();
  std::cout << "\nModule exports:" << std::endl;
  for (const auto& [name, value] : exports) {
    std::cout << "  " << name << ": " << value.toString() << std::endl;
  }

  // Try to get a specific export
  auto runTests = mainModule->getExport("runTests");
  if (runTests) {
    std::cout << "\nFound exported function 'runTests'" << std::endl;
  }

  std::cout << "\n=== Module Test Complete ===" << std::endl;

  return 0;
}