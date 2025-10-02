function test(a, b) {
  console.log('Inside test');
  return a + b;
}
console.log('Calling test');
let x = test(10, 20);
console.log('Result: ' + x);
