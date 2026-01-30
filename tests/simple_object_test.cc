#include <iostream>
#include <string>
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"

using namespace lightjs;

int main() {
    std::cout << "Simple Object Test" << std::endl;

    std::string code = R"(
        let obj = { name: "test" };
        console.log("Object name:", obj.name);
        console.log("Object keys call:");
        Object.keys(obj);
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

    } catch (const std::exception& e) {
        std::cout << "Error: " << e.what() << std::endl;
    }

    return 0;
}