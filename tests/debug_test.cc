#include <iostream>
#include <string>
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"

using namespace lightjs;

int main() {
    std::cout << "Debug Test" << std::endl;

    std::string code = R"(
        let obj = { a: 1, b: 2 };
        console.log("Object created");
        let keys = Object.keys(obj);
        console.log("Keys called");
        keys;
    )";

    try {
        Lexer lexer(code);
        auto tokens = lexer.tokenize();

        Parser parser(tokens);
        auto program = parser.parse();

        if (!program) {
            std::cout << "Parse error!" << std::endl;
            return 1;
        }

        auto env = Environment::createGlobal();
        Interpreter interpreter(env);

        auto task = interpreter.evaluate(*program);
                Value result;
        LIGHTJS_RUN_TASK(task, result);
        std::cout << "Final result type: " << (int)result.data.index() << std::endl;
        std::cout << "Final result: " << result.toString() << std::endl;

        if (result.isArray()) {
            auto arr = std::get<std::shared_ptr<Array>>(result.data);
            std::cout << "Array size: " << arr->elements.size() << std::endl;
            for (size_t i = 0; i < arr->elements.size(); ++i) {
                std::cout << "Element " << i << ": " << arr->elements[i].toString() << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
    }

    return 0;
}