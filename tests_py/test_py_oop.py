class Animal:
    def __init__(self, name):
        self.name = name

    def describe(self):
        return self.name + " (animal)"

class Dog(Animal):
    def __init__(self, name, age):
        super().__init__(name)
        self.age = age

    def describe(self):
        return self.name + " (dog, " + str(self.age) + " years)"

a = Animal("Milo")
print(a.describe())

d = Dog("Rex", 3)
print(d.describe())


x = None
if x is None:
    print("x is None")
else:
    print("bad is")

if x is not None:
    print("bad is not")
else:
    print("x is not None ok")


v = 3
if v in [1, 2, 3]:
    print("v in list")
else:
    print("bad in")

if v not in [1, 2]:
    print("v not in list")
else:
    print("bad not in")


def grade(n):
    if n >= 90:
        return "A"
    elif n >= 80:
        return "B"
    elif n >= 70:
        return "C"
    else:
        return "F"

print(grade(95))
print(grade(85))
print(grade(75))
print(grade(50))


f = lambda a, b: a + b
print(f(2, 3))

g = lambda: "zero arity"
print(g())


try:
    raise ValueError("bad value")
except ValueError as e:
    print("caught ValueError:")


class Empty:
    pass

e = Empty()
print(e is Empty())