#include <iostream>
#include <string>
#include "lexer.h"
#include "parser.h"
#include "interpreter.h"
#include "environment.h"

using namespace tinyjs;

void runTest(const std::string& name, const std::string& code, const std::string& expected = "") {
    std::cout << "Test: " << name << std::endl;

    try {
        Lexer lexer(code);
        auto tokens = lexer.tokenize();

        Parser parser(tokens);
        auto program = parser.parse();

        if (!program) {
            std::cout << "  FAILED: Parse error" << std::endl;
            return;
        }

        auto env = Environment::createGlobal();
        Interpreter interpreter(env);

        auto task = interpreter.evaluate(*program);
        while (!task.done()) {
            std::coroutine_handle<>::from_address(task.handle.address()).resume();
        }

        Value result = task.result();
        std::string resultStr = result.toString();

        std::cout << "  Result: " << resultStr << std::endl;

        if (!expected.empty()) {
            if (resultStr == expected) {
                std::cout << "  PASSED" << std::endl;
            } else {
                std::cout << "  FAILED: Expected '" << expected << "', got '" << resultStr << "'" << std::endl;
            }
        } else {
            std::cout << "  (no assertion)" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "  FAILED: " << e.what() << std::endl;
    }

    std::cout << std::endl;
}

int main() {
    std::cout << "TinyJS JSON and Object Methods Test" << std::endl;
    std::cout << "====================================" << std::endl << std::endl;

    // JSON.stringify tests
    runTest("JSON.stringify simple object", R"(
        let obj = { name: "John", age: 30 };
        JSON.stringify(obj);
    )");

    runTest("JSON.stringify array", R"(
        let arr = [1, 2, 3, "hello"];
        JSON.stringify(arr);
    )");

    runTest("JSON.stringify primitives", R"(
        JSON.stringify(42);
    )", "42");

    runTest("JSON.stringify string", R"(
        JSON.stringify("hello world");
    )", "\"hello world\"");

    runTest("JSON.stringify boolean", R"(
        JSON.stringify(true);
    )", "true");

    runTest("JSON.stringify null", R"(
        JSON.stringify(null);
    )", "null");

    // JSON.parse tests
    runTest("JSON.parse simple object", R"(
        let str = '{"name":"John","age":30}';
        let obj = JSON.parse(str);
        obj.name;
    )", "John");

    runTest("JSON.parse array", R"(
        let str = '[1,2,3,"hello"]';
        let arr = JSON.parse(str);
        arr[3];
    )", "hello");

    runTest("JSON.parse number", R"(
        JSON.parse("42");
    )", "42");

    runTest("JSON.parse string", R"(
        JSON.parse('"hello"');
    )", "hello");

    runTest("JSON.parse boolean", R"(
        JSON.parse("true");
    )", "true");

    // Object.keys tests
    runTest("Object.keys", R"(
        let obj = { a: 1, b: 2, c: 3 };
        let keys = Object.keys(obj);
        keys[0];
    )", "a");

    // Object.values tests
    runTest("Object.values", R"(
        let obj = { a: 1, b: 2, c: 3 };
        let values = Object.values(obj);
        values[0];
    )", "1");

    // Object.entries tests
    runTest("Object.entries", R"(
        let obj = { a: 1, b: 2 };
        let entries = Object.entries(obj);
        entries[0][0];
    )", "a");

    runTest("Object.entries value", R"(
        let obj = { a: 1, b: 2 };
        let entries = Object.entries(obj);
        entries[0][1];
    )", "1");

    // Object.assign tests
    runTest("Object.assign", R"(
        let target = { a: 1 };
        let source = { b: 2, c: 3 };
        Object.assign(target, source);
        target.b;
    )", "2");

    runTest("Object.assign multiple sources", R"(
        let target = { a: 1 };
        let source1 = { b: 2 };
        let source2 = { c: 3 };
        Object.assign(target, source1, source2);
        target.c;
    )", "3");

    // Round-trip JSON test
    runTest("JSON round-trip", R"(
        let original = { name: "Alice", age: 25, hobbies: ["reading", "coding"] };
        let jsonStr = JSON.stringify(original);
        let parsed = JSON.parse(jsonStr);
        parsed.name;
    )", "Alice");

    runTest("JSON round-trip array access", R"(
        let original = { name: "Alice", age: 25, hobbies: ["reading", "coding"] };
        let jsonStr = JSON.stringify(original);
        let parsed = JSON.parse(jsonStr);
        parsed.hobbies[1];
    )", "coding");

    // Complex object operations
    runTest("Complex object manipulation", R"(
        let data = { users: [{ name: "John" }, { name: "Jane" }] };
        let jsonStr = JSON.stringify(data);
        let parsed = JSON.parse(jsonStr);
        let keys = Object.keys(parsed);
        keys[0];
    )", "users");

    std::cout << "All tests completed!" << std::endl;
    return 0;
}