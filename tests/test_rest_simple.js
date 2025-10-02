function test(...args) {
  console.log('Inside test function');
  return 42;
}
console.log('About to call test');
let x = test(1, 2);
console.log('Result: ' + x);
