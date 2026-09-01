import math

# --- Number-theoretic and representation functions ---
print(math.ceil(2.1), math.ceil(-2.1), math.ceil(5))
print(math.floor(2.9), math.floor(-2.9), math.floor(-5))
print(math.trunc(2.9), math.trunc(-2.9), math.trunc(7))
print(math.comb(52, 5), math.comb(10, 0), math.comb(10, 11), math.comb(0, 0))
print(math.copysign(3, -0.0), math.copysign(-2, 5))
print(math.fabs(-3.5), math.fabs(3.5))
print(math.factorial(0), math.factorial(5), math.factorial(25))
print(math.fmod(7, 3), math.fmod(-7, 3) < 0)
print(math.frexp(8.0), math.frexp(0.0))
print(math.ldexp(0.5, 3), math.ldexp(1, 10))
m, e = math.modf(3.7)
print(m, e)
m2, e2 = math.modf(-3.7)
print(m2, e2)
print(math.gcd(48, 18), math.gcd(-48, 18), math.gcd(0, 0), math.gcd(0, 5))
print(math.gcd(), math.lcm(), math.lcm(4, 6), math.lcm(4, 6, 8), math.lcm(0, 5))
print(math.isqrt(17), math.isqrt(16), math.isqrt(0), math.isqrt(10**30))
print(math.perm(5, 2), math.perm(5), math.perm(10, 0), math.perm(0, 0))
print(math.nextafter(1.0, 2.0) > 1.0, math.nextafter(2.0, 1.0) < 2.0)
print(math.nextafter(1.0, 1.0) == 1.0)
print(math.ulp(1.0) == 2.220446049250313e-16)
print(math.ulp(0.0) == 5e-324, math.ulp(math.inf) == math.inf)
print(math.remainder(7, 3))
print(math.isfinite(1), math.isfinite(math.inf), math.isfinite(math.nan))
print(math.isinf(math.inf), math.isinf(-math.inf), math.isinf(3), math.isinf(math.nan))
print(math.isnan(math.nan), math.isnan(3), math.isnan(math.inf))
print(math.isclose(1.0, 1.0), math.isclose(1.0, 1.1))
print(math.isclose(1.0, 1.09, rel_tol=0.1), math.isclose(0.0, 1e-12, abs_tol=1e-9))
print(math.isclose(math.inf, math.inf), math.isclose(math.inf, -math.inf))
print(math.isclose(math.nan, math.nan))
print(math.fsum([0.1] * 10) == 1.0, math.fsum([1, 2.5, 3]), math.fsum([]))
print(math.prod([1, 2, 3, 4]), math.prod([]), math.prod([2, 3], start=10))

# --- Power and logarithmic ---
print(math.sqrt(16), math.cbrt(27.0))
print(math.exp(0), math.exp(1) == math.e)
print(math.exp2(10), math.expm1(0))
print(math.log(math.e), math.log(8, 2), math.log(100, 10), math.log(1))
print(math.log2(8), math.log10(1000), math.log1p(0))
print(math.pow(2, 10), math.pow(9, 0.5), math.pow(2, 0))

# --- Trigonometric ---
print(math.sin(0), math.cos(0), math.tan(0))
print(math.asin(0), math.acos(1), math.atan(0))
print(math.atan2(1, 1) == math.pi / 4)
print(math.hypot(3, 4), math.hypot(5))
print(math.dist([0, 0], [3, 4]), math.dist((1, 2, 3), (1, 2, 3)))

# --- Angular conversion ---
print(math.degrees(math.pi) == 180.0, math.radians(180) == math.pi)

# --- Hyperbolic ---
print(math.sinh(0), math.cosh(0), math.tanh(0))
print(math.asinh(0), math.acosh(1), math.atanh(0))

# --- Special ---
print(math.erf(0), math.erfc(0))
print(math.gamma(5) == 24.0, math.lgamma(1))

# --- Constants ---
print(math.pi == 3.141592653589793)
print(math.e == 2.718281828459045)
print(math.tau == 2 * math.pi)
print(math.inf > 1e308, -math.inf < -1e308)
print(math.nan != math.nan)

# --- int passthrough: ceil/floor/trunc keep exact ints ---
print(math.ceil(10**20), math.floor(-(10**20)))

# --- Error behavior (CPython parity) ---
try:
    math.sqrt(-1)
except ValueError as e:
    print("sqrt:", e)
try:
    math.exp(1000)
except OverflowError as e:
    print("exp:", e)
try:
    math.factorial(-1)
except ValueError as e:
    print("fact-neg:", e)
try:
    math.factorial(2.5)
except (TypeError, ValueError) as e:
    print("fact-frac:", e)
try:
    math.factorial("x")
except TypeError as e:
    print("fact-str:", e)
try:
    math.isqrt(-1)
except ValueError as e:
    print("isqrt-neg:", e)
try:
    math.isqrt(2.0)
except TypeError as e:
    print("isqrt-float:", e)
try:
    math.ceil(float("nan"))
except ValueError as e:
    print("ceil-nan:", e)
try:
    math.floor(float("inf"))
except OverflowError as e:
    print("floor-inf:", e)
try:
    math.pow(-1, 0.5)
except ValueError as e:
    print("pow:", e)
try:
    math.gamma(0)
except ValueError as e:
    print("gamma0:", e)
try:
    math.lgamma(-3)
except ValueError as e:
    print("lgamma:", e)
try:
    math.gcd(4, 2.5)
except TypeError as e:
    print("gcd-float:", e)
try:
    math.isclose(1, 1, rel_tol=-1)
except ValueError as e:
    print("isclose:", e)
try:
    math.acosh(0.5)
except ValueError as e:
    print("acosh:", e)
try:
    math.atanh(2)
except ValueError as e:
    print("atanh:", e)
try:
    math.fmod(1, 0)
except ValueError as e:
    print("fmod0:", e)
try:
    math.log(0)
except ValueError as e:
    print("log0:", e)
try:
    math.ldexp(1, 1.5)
except TypeError as e:
    print("ldexp:", e)

# --- kwargs rejection on plain natives (CPython: TypeError) ---
try:
    math.pow(base=2, exp=3)
except TypeError as e:
    print("pow-kw:", e)
try:
    math.sin(1.0, foo=2)
except TypeError as e:
    print("sin-kw:", e)
