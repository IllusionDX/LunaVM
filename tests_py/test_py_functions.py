def add(a, b):
    return a + b

def greet(name):
    return f"hello {name}"

def mul(x, y=2):
    return x * y

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

print(add(2, 3))
print(greet("world"))
print(mul(3))
print(mul(3, 4))
print(fib(10))


def counter():
    c = 0

    def inc():
        c = c + 1
        return c

    return inc

up = counter()
print(up())
print(up())
print(up())