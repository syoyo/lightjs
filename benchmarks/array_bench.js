// Array operations benchmark

// Array push/pop
function benchArrayPushPop() {
  let arr = [];
  for (let i = 0; i < 10000; i++) {
    arr.push(i);
  }
  for (let i = 0; i < 5000; i++) {
    arr.pop();
  }
  return arr.length;
}

// Array iteration
function benchArrayIteration() {
  let arr = [];
  for (let i = 0; i < 10000; i++) {
    arr.push(i);
  }
  let sum = 0;
  for (let i = 0; i < arr.length; i++) {
    sum += arr[i];
  }
  return sum;
}

// Array methods
function benchArrayMethods() {
  let arr = [];
  for (let i = 0; i < 1000; i++) {
    arr.push(i);
  }

  let doubled = arr.map(x => x * 2);
  let evens = doubled.filter(x => x % 2 === 0);
  let sum = evens.reduce((acc, x) => acc + x, 0);

  return sum;
}

// Array slice/concat
function benchArraySliceConcat() {
  let arr1 = [];
  for (let i = 0; i < 1000; i++) {
    arr1.push(i);
  }

  let arr2 = arr1.slice(100, 500);
  let arr3 = arr1.concat(arr2);

  return arr3.length;
}

let result1 = benchArrayPushPop();
let result2 = benchArrayIteration();
let result3 = benchArrayMethods();
let result4 = benchArraySliceConcat();

console.log("Array benchmark completed:", result1, result2, result3, result4);
