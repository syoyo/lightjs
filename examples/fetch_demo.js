// Fetch API Demo for TinyJS

// Example 1: Fetch a local file using file:// protocol
console.log("=== File Protocol Example ===");
let fileResp = fetch("file:///home/syoyo/work/tinyjs/build/test.txt");
console.log("Status:", fileResp.status);
console.log("OK:", fileResp.ok);
console.log("Content:", fileResp.text());

// Example 2: Error handling - file not found
console.log("\n=== Error Handling Example ===");
let notFound = fetch("file:///nonexistent.txt");
console.log("Status:", notFound.status);
console.log("OK:", notFound.ok);

// Example 3: With async/await (if you want to use it)
async function fetchExample() {
  let response = await fetch("file:///home/syoyo/work/tinyjs/build/test.txt");
  return response.text();
}

console.log("\n=== Async/Await Example ===");
let result = fetchExample();
console.log("Result:", result);

// Note: HTTP/HTTPS examples
// HTTP example (requires network connection):
// let httpResp = fetch("http://example.com");
// console.log(httpResp.status);
// console.log(httpResp.text());
//
// HTTPS is planned but not yet implemented