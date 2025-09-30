/*---
description: Test async function returns a Promise
flags: [async]
---*/

async function asyncFunc() {
  return 42;
}

let result = asyncFunc();

// Result should be a Promise
assert(typeof result === "object");

$DONE();