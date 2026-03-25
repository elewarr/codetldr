"""Sample Python module for Tree-sitter testing."""


def greet(name: str) -> str:
    """Return a greeting message."""
    return f"Hello, {name}!"


def farewell(name: str) -> str:
    return f"Goodbye, {name}!"


class Calculator:
    """A simple calculator."""

    def add(self, a: int, b: int) -> int:
        return a + b

    def multiply(self, a: int, b: int) -> int:
        return a * b


def main():
    msg = greet("world")
    bye = farewell("world")
    calc = Calculator()
    result = calc.add(2, 3)
    product = calc.multiply(result, 2)
    print(msg)
