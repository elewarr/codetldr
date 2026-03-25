// Sample Kotlin module for Tree-sitter testing.

fun greet(name: String): String {
    return "Hello, $name!"
}

fun farewell(name: String): String {
    return "Goodbye, $name!"
}

class Calculator {
    private val history = mutableListOf<Int>()

    fun add(a: Int, b: Int): Int {
        val result = a + b
        history.add(result)
        return result
    }

    fun multiply(a: Int, b: Int): Int {
        return a * b
    }
}

fun main() {
    val msg = greet("world")
    val bye = farewell("world")
    val calc = Calculator()
    val result = calc.add(2, 3)
    val product = calc.multiply(result, 2)
    println(msg)
}
