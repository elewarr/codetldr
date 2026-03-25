// Sample JavaScript module for Tree-sitter testing.

function greet(name) {
    return `Hello, ${name}!`;
}

function farewell(name) {
    return `Goodbye, ${name}!`;
}

class Calculator {
    constructor() {
        this.history = [];
    }

    add(a, b) {
        const result = a + b;
        this.history.push(result);
        return result;
    }

    multiply(a, b) {
        return a * b;
    }
}

function main() {
    const msg = greet("world");
    const bye = farewell("world");
    const calc = new Calculator();
    const result = calc.add(2, 3);
    const product = calc.multiply(result, 2);
    console.log(msg);
}

main();
