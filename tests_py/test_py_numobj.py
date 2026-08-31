# Virtual objects: immediate numbers resolve methods/attributes via their
# canonical classes (py_fe int_class / float_class) without boxing.
print((3).bit_length())
print((-7).bit_length())
print((1024).bit_length())
print((2.5).is_integer())
print((8.0).is_integer())
print((-2.5).is_integer())

x = 5
print(x.bit_length())

# real / imag — zero-arg natives evaluated eagerly on attribute access
print((2).real)
print((2).imag)
print((3.5).real)
print((3.5).imag)

# dunder conversions through the virtual classes
print(int(3.7))
print(float(4))
print((0).__bool__())
print((1).__bool__())
print((0.0).__bool__())
print((2.5).__bool__())

# attribute access without boxing yields the canonical class
print((3).__class__)