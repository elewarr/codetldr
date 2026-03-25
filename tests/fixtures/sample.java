// Sample Java module for Tree-sitter testing.
import java.util.ArrayList;
import java.util.List;

public class sample {

    interface Greeter {
        String greet(String name);
    }

    static String greet(String name) {
        return "Hello, " + name + "!";
    }

    static String farewell(String name) {
        return "Goodbye, " + name + "!";
    }

    static class Calculator implements Greeter {
        private List<Integer> history = new ArrayList<>();

        @Override
        public String greet(String name) {
            return "Calculator says hello, " + name + "!";
        }

        public int add(int a, int b) {
            int result = a + b;
            history.add(result);
            return result;
        }

        public int multiply(int a, int b) {
            return a * b;
        }
    }

    public static void main(String[] args) {
        String msg = greet("world");
        String bye = farewell("world");
        Calculator calc = new Calculator();
        int result = calc.add(2, 3);
        int product = calc.multiply(result, 2);
        System.out.println(msg);
    }
}
