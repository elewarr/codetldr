-- Sample Lua file for CodeTLDR testing
-- Covers all 3 function_declaration forms + function_call + local function

local M = {}

-- Form 1: Plain identifier (function_declaration name: identifier)
function greet(name)
    return "Hello, " .. name
end

-- Form 2: Dot index expression (function_declaration name: dot_index_expression)
function M.calculate(a, b)
    return M.add(a, b)
end

function M.add(x, y)
    return x + y
end

-- Form 3: Method (colon) index expression (function_declaration name: method_index_expression)
function M:process(data)
    return self.calculate(data, 0)
end

function M:init(value)
    self.value = value
    greet("world")
end

-- Local function
local function helper(x)
    return x * 2
end

-- Calls: direct, dot, method
local result = greet("test")
local val = M.calculate(1, 2)
local obj = M
obj:init(42)
helper(result)

return M
