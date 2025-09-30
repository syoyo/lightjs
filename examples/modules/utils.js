// utils.js - Utility functions module

export function formatNumber(num, decimals = 2) {
  return num.toFixed(decimals);
}

export function isEven(n) {
  return n % 2 === 0;
}

export function isOdd(n) {
  return n % 2 !== 0;
}

export function range(start, end) {
  let result = [];
  for (let i = start; i <= end; i++) {
    result.push(i);
  }
  return result;
}

export function sum(arr) {
  let total = 0;
  for (let val of arr) {
    total += val;
  }
  return total;
}