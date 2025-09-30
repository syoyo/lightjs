// math.js - Math utility module

export function add(a, b) {
  return a + b;
}

export function subtract(a, b) {
  return a - b;
}

export function multiply(a, b) {
  return a * b;
}

export function divide(a, b) {
  if (b === 0) {
    throw new Error("Division by zero");
  }
  return a / b;
}

export const PI = 3.14159265359;
export const E = 2.71828182846;

// Default export
export default {
  add,
  subtract,
  multiply,
  divide,
  PI,
  E
};