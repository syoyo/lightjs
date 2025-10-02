// Test for...in loop
let obj = {a: 1, b: 2, c: 3};
let keys = '';
for (let key in obj) {
  keys = keys + key;
}
console.log('for...in result:', keys);

// Test for...of loop with array
let arr = [10, 20, 30];
let sum = 0;
for (let val of arr) {
  sum = sum + val;
}
console.log('for...of array result:', sum);

// Test for...of loop with string
let str = 'abc';
let chars = '';
for (let c of str) {
  chars = chars + c;
}
console.log('for...of string result:', chars);

// Test do...while loop
let count = 0;
do {
  count = count + 1;
} while (count < 3);
console.log('do...while result:', count);

// Test switch statement
function testSwitch(val) {
  let result = '';
  switch (val) {
    case 1:
      result = 'one';
      break;
    case 2:
      result = 'two';
      break;
    case 3:
      result = 'three';
      break;
    default:
      result = 'other';
  }
  return result;
}

console.log('switch(1):', testSwitch(1));
console.log('switch(2):', testSwitch(2));
console.log('switch(3):', testSwitch(3));
console.log('switch(4):', testSwitch(4));

// Test switch fall-through
function testSwitchFallThrough(val) {
  let result = 0;
  switch (val) {
    case 1:
      result = result + 1;
    case 2:
      result = result + 2;
      break;
    case 3:
      result = result + 3;
      break;
  }
  return result;
}

console.log('switch fallthrough(1):', testSwitchFallThrough(1));
console.log('switch fallthrough(2):', testSwitchFallThrough(2));
console.log('switch fallthrough(3):', testSwitchFallThrough(3));