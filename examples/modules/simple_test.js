// Simple module test

import { add, PI } from "./simple_math.js";

console.log("Testing modules:");
console.log("PI =", PI);

let result = add(10, 20);
console.log("add(10, 20) =", result);

export const testResult = result;