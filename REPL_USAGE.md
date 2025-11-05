# TinyJS REPL - Interactive JavaScript Shell

## Overview

The TinyJS REPL (Read-Eval-Print Loop) provides an interactive command-line interface for executing JavaScript code in real-time. It's perfect for:
- Quick experimentation with JavaScript features
- Testing code snippets
- Learning JavaScript
- Debugging and prototyping

## Building the REPL

```bash
cd build
cmake ..
make tinyjs_repl
```

## Running the REPL

```bash
./tinyjs_repl
```

You'll see the welcome message:
```
TinyJS REPL v1.0.0
Type '.help' for help, '.exit' to quit

>
```

## Features

### 1. **Expression Evaluation**
Results of expressions are automatically printed:

```javascript
> 2 + 2
4
> Math.sqrt(16)
4
> "Hello" + " " + "World"
Hello World
```

### 2. **Persistent Environment**
Variables and functions persist across evaluations:

```javascript
> let x = 42
> x + 8
50
> function greet(name) { return "Hello, " + name; }
> greet("Alice")
Hello, Alice
```

### 3. **Multi-line Input**
The REPL automatically detects incomplete input and continues on the next line:

```javascript
> function factorial(n) {
...   if (n <= 1) return 1;
...   return n * factorial(n - 1);
... }
> factorial(5)
120
```

Multi-line detection works for:
- Unbalanced `{ }` braces
- Unbalanced `( )` parentheses
- Unbalanced `[ ]` brackets

### 4. **Special Commands**

- `.help` - Display help message
- `.exit` or `.quit` - Exit the REPL
- `Ctrl+D` - EOF signal (also exits)

### 5. **Error Handling**
Syntax and runtime errors are caught and displayed:

```javascript
> let x =
Parse error: Invalid syntax
> undefinedVariable
Error: ReferenceError: undefinedVariable is not defined
```

### 6. **Full ES2020 Support**
The REPL supports all TinyJS features:

**Modern Syntax:**
```javascript
> const arr = [1, 2, 3, 4, 5]
> arr.map(x => x * 2)
[2, 4, 6, 8, 10]
> const { x, y } = { x: 10, y: 20 }
> x + y
30
```

**Async/Await:**
```javascript
> async function fetchData() { return 42; }
> let result = await fetchData()
> result
42
```

**Classes:**
```javascript
> class Point {
...   constructor(x, y) {
...     this.x = x;
...     this.y = y;
...   }
...   distance() {
...     return Math.sqrt(this.x ** 2 + this.y ** 2);
...   }
... }
> let p = new Point(3, 4)
> p.distance()
5
```

**Template Literals:**
```javascript
> let name = "World"
> `Hello, ${name}!`
Hello, World!
```

## Usage Examples

### Quick Calculations
```javascript
> 123 * 456
56088
> Math.PI * 2
6.283185307179586
```

### Variable Testing
```javascript
> let nums = [1, 2, 3, 4, 5]
> nums.reduce((sum, n) => sum + n, 0)
15
```

### Function Development
```javascript
> function isPrime(n) {
...   if (n <= 1) return false;
...   for (let i = 2; i * i <= n; i++) {
...     if (n % i === 0) return false;
...   }
...   return true;
... }
> [2, 3, 4, 5, 6, 7, 8, 9, 10].filter(isPrime)
[2, 3, 5, 7]
```

### Object Manipulation
```javascript
> let user = { name: "Alice", age: 30 }
> Object.keys(user)
["name", "age"]
> Object.values(user)
["Alice", 30]
```

## Tips

1. **Multi-line Editing**: If you make a mistake while typing a multi-line input, you can't go back. Press Ctrl+C (if supported) or complete the input and start over.

2. **Statement vs Expression**: Statements (like variable declarations) don't print a result, only expressions do:
   ```javascript
   > let x = 5    // No output (statement)
   > x + 1        // 6 (expression)
   ```

3. **Inspect Objects**: Use `JSON.stringify()` to see object contents:
   ```javascript
   > let obj = { a: 1, b: 2, c: 3 }
   > JSON.stringify(obj)
   {"a":1,"b":2,"c":3}
   ```

4. **Clear Console**: The REPL doesn't have a built-in clear command, but you can:
   - Exit and restart
   - Use your terminal's clear command before running

## Limitations

- No command history (up/down arrow navigation)
- No tab completion
- No syntax highlighting
- No editing of previous lines in multi-line mode
- Some complex features may cause crashes (report bugs!)

## Future Enhancements

Potential improvements:
- readline integration for history/editing
- Tab completion for variables and functions
- Syntax highlighting
- .load command to load files
- .save command to save session
- Better error messages with line numbers

## See Also

- [TinyJS README](README.md) - Main project documentation
- [CLAUDE.md](CLAUDE.md) - Development guidelines
