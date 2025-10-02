// Test for...of loop with array (this works!)
console.log('=== Testing for...of with array ===');
let arr = [10, 20, 30];
let sum = 0;
for (let val of arr) {
  sum = sum + val;
}
console.log('Sum:', sum);

// Test for...of loop with string
console.log('=== Testing for...of with string ===');
let str = 'hello';
let result = '';
for (let c of str) {
  result = result + c;
}
console.log('Result:', result);

// Test do...while loop
console.log('=== Testing do...while ===');
let count = 0;
do {
  count = count + 1;
} while (count < 5);
console.log('Count:', count);

// Test switch statement
console.log('=== Testing switch ===');
function getDayName(day) {
  let name = '';
  switch (day) {
    case 1:
      name = 'Monday';
      break;
    case 2:
      name = 'Tuesday';
      break;
    case 3:
      name = 'Wednesday';
      break;
    default:
      name = 'Unknown';
  }
  return name;
}

console.log('Day 1:', getDayName(1));
console.log('Day 2:', getDayName(2));
console.log('Day 3:', getDayName(3));
console.log('Day 99:', getDayName(99));

// Test switch fall-through
console.log('=== Testing switch fall-through ===');
function calcScore(level) {
  let score = 0;
  switch (level) {
    case 3:
      score = score + 100;
    case 2:
      score = score + 50;
    case 1:
      score = score + 25;
      break;
    default:
      score = 0;
  }
  return score;
}

console.log('Level 1 score:', calcScore(1));
console.log('Level 2 score:', calcScore(2));
console.log('Level 3 score:', calcScore(3));