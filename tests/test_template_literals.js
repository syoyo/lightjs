// Test basic template literal
let name = 'Alice';
let greeting = `Hello, ${name}!`;
console.log(greeting);

// Test with multiple interpolations
let a = 5;
let b = 10;
let result = `${a} + ${b} = ${a + b}`;
console.log(result);

// Test with expressions
let x = 3;
let y = 4;
let msg = `The result of ${x} * ${y} is ${x * y}`;
console.log(msg);

// Test with function calls
function double(n) {
  return n * 2;
}
let doubled = `Double of 7 is ${double(7)}`;
console.log(doubled);