console.log('=== Spread and Rest Operators Test ===');
console.log('');

// 1. Array Spread
console.log('1. Array Spread:');
let arr1 = [1, 2, 3];
let arr2 = [4, 5, 6];
let combined = [...arr1, ...arr2];
console.log('  Combined: ' + combined.join(', '));

let prefix = [0, ...arr1];
console.log('  Prefix: ' + prefix.join(', '));

let suffix = [...arr2, 7, 8];
console.log('  Suffix: ' + suffix.join(', '));

console.log('');

// 2. String Spread in Array
console.log('2. String Spread in Array:');
let chars = [...'hello'];
console.log('  Characters: ' + chars.join(', '));

console.log('');

// 3. Rest Parameters
console.log('3. Rest Parameters:');
function sum(...numbers) {
  let total = 0;
  for (let i = 0; i < numbers.length; i = i + 1) {
    total = total + numbers[i];
  }
  return total;
}
console.log('  sum(1, 2, 3): ' + sum(1, 2, 3));
console.log('  sum(10, 20, 30, 40): ' + sum(10, 20, 30, 40));

console.log('');

// 4. Rest Parameters with Regular Parameters
console.log('4. Mixed Parameters:');
function greet(greeting, ...names) {
  let result = greeting;
  for (let i = 0; i < names.length; i = i + 1) {
    result = result + ' ' + names[i];
  }
  return result;
}
console.log('  greet("Hello", "Alice", "Bob"): ' + greet('Hello', 'Alice', 'Bob'));
console.log('  greet("Hi", "Charlie"): ' + greet('Hi', 'Charlie'));

console.log('');

// 5. Spread in Function Calls
console.log('5. Spread in Function Calls:');
function multiply(a, b, c) {
  return a * b * c;
}
let nums = [2, 3, 4];
console.log('  multiply(...[2, 3, 4]): ' + multiply(...nums));

console.log('');

// 6. Combining Spread and Rest
console.log('6. Combining Spread and Rest:');
function logAll(...args) {
  for (let i = 0; i < args.length; i = i + 1) {
    console.log('    arg[' + i + ']: ' + args[i]);
  }
}
let data = [10, 20, 30];
console.log('  logAll(...[10, 20, 30]):');
logAll(...data);

console.log('');
console.log('=== All Spread/Rest Tests Passed! ===');
