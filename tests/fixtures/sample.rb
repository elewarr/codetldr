# Sample Ruby file for CodeTLDR testing
# Covers: method, singleton_method, class, module, call

module Greetings
  def hello(name)
    puts greet(name)
  end

  def self.default_greeting
    "Hello"
  end
end

class Calculator
  def initialize(value)
    @value = value
    log("initialized")
  end

  def add(other)
    Calculator.new(@value + other.value)
  end

  def to_s
    @value.to_s
  end

  def name=(new_name)
    @name = new_name
  end

  def self.from_string(str)
    Calculator.new(str.to_i)
  end

  private

  def log(message)
    puts message
  end
end

module Utils
  class StringHelper
    def self.capitalize_all(words)
      words.map { |w| w.capitalize }
    end

    def format(template)
      template.to_s
    end
  end
end
