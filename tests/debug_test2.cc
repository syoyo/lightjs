#include <iostream>
#include <string>
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"
#include "object_methods.h"

using namespace tinyjs;

int main() {
    std::cout << "Debug Test 2 - Direct function call" << std::endl;

    // Test Object_keys directly
    auto obj = std::make_shared<Object>();
    obj->properties["a"] = Value(1.0);
    obj->properties["b"] = Value(2.0);
    obj->properties["c"] = Value(3.0);

    std::vector<Value> args;
    args.push_back(Value(obj));

    std::cout << "Created object with keys: a, b, c" << std::endl;

    try {
        Value result = Object_keys(args);
        std::cout << "Object_keys returned type: " << (int)result.data.index() << std::endl;

        if (result.isArray()) {
            auto arr = std::get<std::shared_ptr<Array>>(result.data);
            std::cout << "Array size: " << arr->elements.size() << std::endl;

            for (size_t i = 0; i < arr->elements.size(); ++i) {
                const Value& elem = arr->elements[i];
                std::cout << "Element " << i << " type: " << (int)elem.data.index() << std::endl;
                std::cout << "Element " << i << " isString: " << elem.isString() << std::endl;
                if (elem.isString()) {
                    std::cout << "Element " << i << " value: " << std::get<std::string>(elem.data) << std::endl;
                }
                std::cout << "Element " << i << " toString: " << elem.toString() << std::endl;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
    }

    return 0;
}