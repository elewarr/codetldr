// Sample C++ module for Tree-sitter testing.
#include <iostream>
#include <string>
#include <vector>

std::string greet(const std::string& name) {
    return "Hello, " + name + "!";
}

std::string farewell(const std::string& name) {
    return "Goodbye, " + name + "!";
}

class Calculator {
public:
    Calculator() = default;

    int add(int a, int b) {
        int result = a + b;
        history_.push_back(result);
        return result;
    }

    int multiply(int a, int b) {
        return a * b;
    }

    const std::vector<int>& history() const {
        return history_;
    }

private:
    std::vector<int> history_;
};

int main() {
    std::string msg = greet("world");
    std::string bye = farewell("world");
    Calculator calc;
    int result = calc.add(2, 3);
    int product = calc.multiply(result, 2);
    std::cout << msg << std::endl;
    return 0;
}
