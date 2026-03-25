// Sample Swift module for Tree-sitter testing.
import Foundation

protocol Greeter {
    func greet(name: String) -> String
}

func greet(name: String) -> String {
    return "Hello, \(name)!"
}

func farewell(name: String) -> String {
    return "Goodbye, \(name)!"
}

class Calculator: Greeter {
    private var history: [Int] = []

    func greet(name: String) -> String {
        return "Calculator says hello, \(name)!"
    }

    func add(a: Int, b: Int) -> Int {
        let result = a + b
        history.append(result)
        return result
    }

    func multiply(a: Int, b: Int) -> Int {
        return a * b
    }
}

func main() {
    let msg = greet(name: "world")
    let bye = farewell(name: "world")
    let calc = Calculator()
    let result = calc.add(a: 2, b: 3)
    let product = calc.multiply(a: result, b: 2)
    print(msg)
}

main()
