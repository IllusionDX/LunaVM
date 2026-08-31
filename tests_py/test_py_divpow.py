# True division: / on ints yields float (Python 3 semantics)
print(10 / 4)
print(4 / 2)
print(-10 / 4)
print(10 / 4.0)

# Floor division: //
print(7 // 2)
print(-7 // 2)
print(7 // -2)
print(-7 // -2)
print(7.0 // 2)
print(-7.5 // 2)

# Power: **
print(2 ** 3)
print(2 ** -1)
print(-2 ** 2)
print(2 ** 3 ** 2)
print(5 ** 0)
print(0 ** 5)
print(0 ** 0)
print(10 ** 2)
print(2.0 ** 3)
print(3 ** 4)

# Compound assignments
a = 10
a //= 3
print(a)
b = 2
b **= 3
print(b)
c = 7
c **= 2
print(c)
d = 20
d //= 7
print(d)
e = 2
e **= 10
print(e)

# Precedence checks
print(2 ** 3 * 2)
print(10 // 3 * 2)
print(2 ** 3 // 2)
print(1 + 2 ** 3)
print(5 - 2 ** 2)
print(8 / 4 // 2)
print(-3 ** 2)
print(2 ** 1 ** 3)

# Mixed compound on expressions
f = 2
f += 10
print(f)