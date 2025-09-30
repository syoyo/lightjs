// main.js - Test module imports

// Import specific functions
import { add, multiply, PI } from "./math.js";

// Import everything as namespace
import * as math from "./math.js";

// Import default export
import mathLib from "./math.js";

// Import from utils
import { isEven, range, sum } from "./utils.js";

console.log("=== Module Test ===");

// Test specific imports
console.log("add(5, 3) =", add(5, 3));
console.log("multiply(4, 7) =", multiply(4, 7));
console.log("PI =", PI);

// Test namespace import
console.log("math.subtract(10, 4) =", math.subtract(10, 4));
console.log("math.E =", math.E);

// Test default import
console.log("mathLib.divide(20, 5) =", mathLib.divide(20, 5));

// Test utils imports
console.log("isEven(6) =", isEven(6));
console.log("range(1, 5) =", range(1, 5));
console.log("sum([1, 2, 3, 4, 5]) =", sum([1, 2, 3, 4, 5]));

// Export our own function
export function runTests() {
  console.log("Running module tests...");
  return add(10, 20) + multiply(2, 3);
}