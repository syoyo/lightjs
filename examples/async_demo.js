// TinyJS Async/Await Feature Demo

console.log("=== TinyJS Async/Await Demo ===");

// 1. Basic async function
async function getNumber() {
  return 42;
}

// 2. Async function with await
async function doubleNumber() {
  let num = await getNumber();
  return num * 2;
}

// 3. Using Promise.resolve
async function usePromiseResolve() {
  let value = await Promise.resolve(100);
  console.log("Promise.resolve value:", value);
  return value;
}

// 4. Multiple awaits in sequence
async function sequence() {
  let a = await Promise.resolve(1);
  let b = await Promise.resolve(2);
  let c = await Promise.resolve(3);
  console.log("Sequence values:", a, b, c);
  return a + b + c;
}

// 5. Await non-promise values
async function awaitNonPromise() {
  let direct = await 50;
  let str = await "hello";
  console.log("Direct values:", direct, str);
  return direct;
}

// 6. Nested async calls
async function level1() {
  return 10;
}

async function level2() {
  let val = await level1();
  return val * 2;
}

async function level3() {
  let val = await level2();
  return val * 2;
}

// 7. Promise.all example
async function promiseAllExample() {
  let p1 = Promise.resolve(10);
  let p2 = Promise.resolve(20);
  let p3 = Promise.resolve(30);

  let results = await Promise.all([p1, p2, p3]);
  console.log("Promise.all results:", results);
  return results;
}

// Execute all demos
console.log("\n1. Basic async function:");
let result1 = getNumber();
console.log("   Returns:", result1);

console.log("\n2. Async with await:");
let result2 = doubleNumber();
console.log("   Returns:", result2);

console.log("\n3. Promise.resolve:");
usePromiseResolve();

console.log("\n4. Sequential awaits:");
sequence();

console.log("\n5. Await non-promise:");
awaitNonPromise();

console.log("\n6. Nested async:");
level3();

console.log("\n7. Promise.all:");
promiseAllExample();

console.log("\n=== Demo Complete ===");