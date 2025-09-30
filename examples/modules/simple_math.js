// Simple math module without advanced features

export const PI = 3.14159;

export function add(a, b) {
  return a + b;
}

export function multiply(a, b) {
  return a * b;
}

const mathLib = {
  PI: PI,
  add: add,
  multiply: multiply
};

export default mathLib;