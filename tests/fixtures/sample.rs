/// Sample Rust module for Tree-sitter testing.

fn greet(name: &str) -> String {
    format!("Hello, {}!", name)
}

fn farewell(name: &str) -> String {
    format!("Goodbye, {}!", name)
}

struct Calculator {
    history: Vec<i64>,
}

impl Calculator {
    fn new() -> Self {
        Calculator { history: Vec::new() }
    }

    fn add(&mut self, a: i64, b: i64) -> i64 {
        let result = a + b;
        self.history.push(result);
        result
    }

    fn multiply(&self, a: i64, b: i64) -> i64 {
        a * b
    }
}

fn main() {
    let msg = greet("world");
    let bye = farewell("world");
    let mut calc = Calculator::new();
    let result = calc.add(2, 3);
    let product = calc.multiply(result, 2);
    println!("{}", msg);
}
