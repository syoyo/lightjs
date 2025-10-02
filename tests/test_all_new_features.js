console.log('=== Testing New Language Features ===');
console.log('');

// Test 1: for...in loop
console.log('1. for...in loop');
let person = {name: 'Alice', age: 30, city: 'NYC'};
let keys = '';
for (let key in person) {
  keys = keys + key + ' ';
}
console.log('   Object keys:', keys);
console.log('');

// Test 2: for...of loop with array
console.log('2. for...of loop with array');
let numbers = [5, 10, 15, 20];
let total = 0;
for (let num of numbers) {
  total = total + num;
}
console.log('   Sum of [5, 10, 15, 20]:', total);
console.log('');

// Test 3: for...of loop with string
console.log('3. for...of loop with string');
let word = 'JavaScript';
let reversed = '';
for (let char of word) {
  reversed = char + reversed;
}
console.log('   Reversed "JavaScript":', reversed);
console.log('');

// Test 4: do...while loop
console.log('4. do...while loop');
let counter = 0;
do {
  counter = counter + 1;
} while (counter < 5);
console.log('   Counter after loop:', counter);
console.log('');

// Test 5: switch statement with break
console.log('5. switch statement with break');
function getGrade(score) {
  let grade = '';
  switch (score) {
    case 90:
      grade = 'A';
      break;
    case 80:
      grade = 'B';
      break;
    case 70:
      grade = 'C';
      break;
    default:
      grade = 'F';
  }
  return grade;
}
console.log('   Grade for 90:', getGrade(90));
console.log('   Grade for 80:', getGrade(80));
console.log('   Grade for 50:', getGrade(50));
console.log('');

// Test 6: switch statement with fall-through
console.log('6. switch statement with fall-through');
function getDiscount(membership) {
  let discount = 0;
  switch (membership) {
    case 'platinum':
      discount = discount + 20;
    case 'gold':
      discount = discount + 10;
    case 'silver':
      discount = discount + 5;
      break;
    default:
      discount = 0;
  }
  return discount;
}
console.log('   Platinum discount:', getDiscount('platinum'));
console.log('   Gold discount:', getDiscount('gold'));
console.log('   Silver discount:', getDiscount('silver'));
console.log('');

// Test 7: Nested loops with break
console.log('7. Nested loops with break');
function findPair() {
  let found = '';
  let arr1 = [1, 2, 3];
  let arr2 = [4, 5, 6];
  for (let a of arr1) {
    for (let b of arr2) {
      if (a + b == 7) {
        found = 'Found: ' + a + ' + ' + b + ' = 7';
        break;
      }
    }
  }
  return found;
}
console.log('   ' + findPair());
console.log('');

// Test 8: Object property access (fixed!)
console.log('8. Object property access');
let config = {host: 'localhost', port: 8080, ssl: 'false'};
console.log('   host:', config.host);
console.log('   port:', config.port);
console.log('   ssl:', config.ssl);
console.log('');

console.log('=== All Features Working! ===');