/*---
description: Test await expression in async function
flags: [async]
---*/

async function test() {
  let value = await Promise.resolve(100);
  assert.sameValue(value, 100);

  let direct = await 42;
  assert.sameValue(direct, 42);

  $DONE();
}

test();