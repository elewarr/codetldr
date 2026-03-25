// Sample TypeScript/TSX module for Tree-sitter testing.
import React from 'react';

interface Props {
    name: string;
}

function greet(name: string): string {
    return `Hello, ${name}!`;
}

function GreetingComponent({ name }: Props): JSX.Element {
    const message = greet(name);
    return <div>{message}</div>;
}

class Calculator {
    add(a: number, b: number): number {
        return a + b;
    }

    multiply(a: number, b: number): number {
        return a * b;
    }
}

function App(): JSX.Element {
    const calc = new Calculator();
    const result = calc.add(2, 3);
    return <GreetingComponent name="world" />;
}

export default App;
