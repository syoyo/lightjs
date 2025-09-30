// Simple async/await test

// Test basic async function
async function test1() {
  return 42;
}

// Test await with Promise.resolve
async function test2() {
  let value = await Promise.resolve(100);
  return value + 1;
}

// Test multiple awaits
async function test3() {
  let a = await 10;
  let b = await 20;
  return a + b;
}

// Run tests
console.log("Test 1 - Basic async function:");
let result1 = test1();
console.log(result1);

console.log("Test 2 - Await Promise.resolve:");
let result2 = test2();
console.log(result2);

console.log("Test 3 - Multiple awaits:");
let result3 = test3();
console.log(result3);