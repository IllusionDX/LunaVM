# bigint tests — arbitrary-precision integers (PyLong-style, base 2^30).
# Expected values were generated with CPython 3.11 (local reference).
def t(name, got, want):
    if got != want:
        print("FAIL", name, "got", got, "want", want)
    else:
        print("ok", name)

# literals beyond int64 (const folding + runtime)
t("lit-add", 123456789012345678901234567890 + 1, 123456789012345678901234567891)
t("lit-mul", 2 ** 100, 1267650600228229401496703205376)
t("lit-neg", -2 ** 80, -1208925819614629174706176)
t("lit-fold", 99999999999999999999999999 * 2, 199999999999999999999999998)
t("lit-cmp", 1234567890123456789012345678 > 999999999999999999999, True)

# arithmetic
a = 10 ** 30
b = 7 ** 25
t("add", a + b, 10 ** 30 + 7 ** 25)
t("sub", a - b, 10 ** 30 - 7 ** 25)
t("mul", a * b, 10 ** 30 * 7 ** 25)
t("neg-mul", -(10 ** 30) * (7 ** 25), -(10 ** 30 * 7 ** 25))
t("truediv-int", (10 ** 40) // (10 ** 25), 10 ** 15)
t("mod-big", (10 ** 40) % 7, (10 ** 40) % 7)
q = (10 ** 40) // 7
r = (10 ** 40) % 7
t("divmod-identity", q * 7 + r, 10 ** 40)

# floor division and modulo signs (Python semantics)
t("floordiv", -7 // 2, -4)
t("floordiv2", 7 // -2, -4)
t("floordiv3", -7 // -2, 3)
t("floordiv-big", -(10 ** 40) // 3, -3333333333333333333333333333333333333334)
t("mod", -7 % 3, 2)
t("mod2", 7 % -3, -2)
t("mod3", -7 % -3, -1)
t("mod-big-neg", -(10 ** 40) % 7, 3)

# pow
t("pow-big", 3 ** 200, 3 ** 200)
t("pow-neg-exp", 2 ** -3, 0.125)
t("pow-zero", 0 ** 0, 1)
t("pow-one", 12345678901234567890 ** 0, 1)
t("pow-neg-base", (-3) ** 101, -(3 ** 101))

# shifts
t("shl", 1 << 100, 2 ** 100)
t("shl-big", (1 << 100) << 50, 2 ** 150)
t("shr", (10 ** 40) >> 100, (10 ** 40) // (2 ** 100))
t("shr-neg", -5 >> 1, -3)
t("shr-neg2", -1 >> 100, -1)
t("shr-big", (5 ** 50) >> 60, (5 ** 50) // (2 ** 60))
t("shl-mixed", (2 ** 70) << 3, 2 ** 73)

# bitwise (infinite two's complement)
t("and", (2 ** 90) & (2 ** 90 + 7), 2 ** 90)
t("or", 12345678901234567890 | 0, 12345678901234567890)
t("xor", -(10 ** 30) ^ 0, -(10 ** 30))
t("xor2", -1 ^ 0, -1)
t("not", ~5, -6)
t("not-big", ~(10 ** 30), -(10 ** 30) - 1)
t("and-neg", -5 & 3, 3)
t("or-neg", -5 | 3, -5)
t("xor-big", (2 ** 80) ^ (2 ** 80 + 9), 9)

# comparisons
t("cmp-lt", 10 ** 40 < 10 ** 41, True)
t("cmp-gt", 10 ** 40 > 10 ** 39, True)
t("cmp-eq", 2 ** 100 == 2 ** 100, True)
t("cmp-ne", 2 ** 100 != 2 ** 99, True)
t("cmp-int", 2 ** 40 > 5, True)
t("cmp-int2", -2 ** 40 < -5, True)
t("cmp-double", 2 ** 100 > 1e30, True)
t("cmp-double2", 2 ** 53 + 1 > 2 ** 53, True)
t("eq-double", 2 ** 53 + 1 == 9007199254740992.0, False)
t("eq-double2", 2 ** 52 == 4503599627370496.0, True)
t("eq-int32", 2 ** 31 == 2147483648, True)
t("cmp-zero", 0 < 10 ** 40, True)
t("cmp-neg", -(10 ** 40) < 0, True)

# exact int() conversions
t("int-string", int("123456789012345678901234567890123456789"), 123456789012345678901234567890123456789)
t("int-string-neg", int("-987654321098765432109876543210"), -987654321098765432109876543210)
t("int-string-spaces", int("  42  "), 42)
t("int-float", int(1e100), int(1e100))
t("int-float-trunc", int(3.999), 3)
t("int-float-neg", int(-3.7), -3)
t("int-bool", int(True), 1)
t("int-hex-free", int("0" * 50 + "7"), 7)

# exact float() conversions (CPython reference values)
t("float-big", float(2 ** 100), 1.2676506002282294e30)
t("float-big2", float(10 ** 100), 1e100)
t("float-3^200", float(3 ** 200), 2.6561398887587478e95)
t("float-2^1000", float(2 ** 1000), 1.0715086071862673e301)
t("float-small", float(3), 3.0)

# div-big-truediv (int/int true division of huge ints, CPython reference)
t("truediv-huge", (10 ** 400) // (10 ** 300) == 10 ** 100, True)

# str/print rendering
t("str-big", str(10 ** 40), "1" + "0" * 40)
s = str(2 ** 1000)
t("str-big2", len(s), 302)
t("str-first", s[:3], "107")
t("str-last", s[-1:], "6")
t("neg-str", str(-(10 ** 30)), "-" + "1" + "0" * 30)
t("int-parse-roundtrip", int(str(3 ** 200)), 3 ** 200)

# bit_length
t("bitlen", (2 ** 100).bit_length(), 101)
t("bitlen2", (2 ** 100 - 1).bit_length(), 100)
t("bitlen-neg", (-(2 ** 100)).bit_length(), 101)
t("bitlen-small", 5.bit_length(), 3)

# abs on bigints
t("abs-big", abs(-(10 ** 50)), 10 ** 50)

# true division (int / int) stays exact and rounded
t("truediv", (10 ** 40) / 7, 1.4285714285714284e+39)
t("truediv-small", 1 / 3, 1.0 / 3.0)
t("truediv-cross", (2 ** 60 + 1) / (2 ** 60 - 1), (2 ** 60 + 1) / (2 ** 60 - 1))

# dict with bigint keys
d = {}
d[2 ** 100] = "big"
d[2 ** 100 + 1] = "big2"
t("dict-get", d[2 ** 100], "big")
t("dict-get2", d[2 ** 100 + 1], "big2")
t("dict-len", len(d), 2)
d[10 ** 40] = 1
d[10 ** 40] = 2
t("dict-overwrite", d[10 ** 40], 2)
t("dict-len2", len(d), 3)
try:
    x = d[2 ** 31]
    print("FAIL dict-missing: no exception")
except KeyError:
    print("ok dict-missing")

# error paths
try:
    x = 1 << -1
    print("FAIL neg-shift: no exception")
except ValueError:
    print("ok neg-shift")

try:
    x = int("abc")
    print("FAIL int-bad-literal: no exception")
except ValueError:
    print("ok int-bad-literal")

try:
    x = int(1e1000)
    print("FAIL int-inf: no exception")
except ValueError:
    print("ok int-inf")

try:
    x = float(10 ** 400)
    print("FAIL float-huge: no exception")
except Exception:
    print("ok float-huge")

try:
    x = (10 ** 400) // 0
    print("FAIL div-zero: no exception")
except Exception:
    print("ok div-zero")

try:
    x = (10 ** 400) / 10 ** 300
    t("truediv-huge-ok", x == 1e100, True)
except Exception:
    print("FAIL truediv-huge-ok: raised")

try:
    x = (10 ** 40) % 0
    print("FAIL mod-zero: no exception")
except Exception:
    print("ok mod-zero")

try:
    lst = [1, 2, 3]
    y = lst[10 ** 40]
    print("FAIL big-index: no exception")
except Exception:
    print("ok big-index")

try:
    y = 2 ** 10000000000
    print("FAIL huge-pow: no exception")
except Exception:
    print("ok huge-pow")

print("done")