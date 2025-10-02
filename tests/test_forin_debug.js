let obj = {a: 1, b: 2};
let count = 0;
for (let key in obj) {
  count = count + 1;
}
console.log('count:', count);