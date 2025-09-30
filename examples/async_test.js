// Test async/await functionality

// Test 1: Basic async function
async function delay(ms) {
  return new Promise((resolve) => {
    setTimeout(() => resolve("done"), ms);
  });
}

// Test 2: Async function returning a value
async function getValue() {
  return 42;
}

// Test 3: Await in async function
async function testAwait() {
  let result = await getValue();
  console.log("Result from getValue:", result);
  return result;
}

// Test 4: Promise.resolve
let resolvedPromise = Promise.resolve(123);
console.log("Promise.resolve created");

// Test 5: Promise.reject
let rejectedPromise = Promise.reject("error");
console.log("Promise.reject created");

// Test 6: Multiple awaits
async function multipleAwaits() {
  let a = await Promise.resolve(1);
  let b = await Promise.resolve(2);
  let c = await Promise.resolve(3);
  return a + b + c;
}

// Test 7: Async function with error handling
async function errorTest() {
  try {
    await Promise.reject("Test error");
  } catch (e) {
    console.log("Caught error:", e);
    return "handled";
  }
}

// Execute tests
console.log("Starting async tests...");

// Run test functions
testAwait();
multipleAwaits().then(result => console.log("Multiple awaits result:", result));
errorTest();

console.log("Tests initiated");