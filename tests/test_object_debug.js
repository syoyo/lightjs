console.log('Creating object...');
let obj = {a: 1};
console.log('Object created');
console.log('typeof obj:', typeof obj);

console.log('Getting keys...');
let keys = Object.keys(obj);
console.log('keys:', keys);
console.log('keys.length:', keys.length);

if (keys.length > 0) {
  console.log('First key:', keys[0]);
}

console.log('Accessing obj.a directly...');
let val = obj.a;
console.log('obj.a =', val);