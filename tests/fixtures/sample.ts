// Sample TypeScript module for Tree-sitter testing.

interface Greeter {
    greet(name: string): string;
}

function greet(name: string): string {
    return `Hello, ${name}!`;
}

function farewell(name: string): string {
    return `Goodbye, ${name}!`;
}

class Calculator implements Greeter {
    private history: number[] = [];

    greet(name: string): string {
        return `Calculator says hello, ${name}!`;
    }

    add(a: number, b: number): number {
        const result = a + b;
        this.history.push(result);
        return result;
    }

    multiply(a: number, b: number): number {
        return a * b;
    }
}

function main(): void {
    const msg = greet("world");
    const bye = farewell("world");
    const calc = new Calculator();
    const result = calc.add(2, 3);
    const product = calc.multiply(result, 2);
    console.log(msg);
}

main();
