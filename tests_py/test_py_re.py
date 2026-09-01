import re

print(re.sub("\\d+", "N", "a1b22c333"))
print(re.sub("x", "y", "xxhx", 1))
print(re.split("\\s+", "one  two   three"))
print(re.split("\\s+", "a b c d", 2))
print(re.findall("\\d+", "a1 b22 c333"))
print(re.findall("(\\w)(\\w)", "abcd"))
print(re.escape("hello.world*"))

# match/search basic
m = re.match("\\d+", "123abc")
print(m.group(0))
print(m.start())
print(m.end())
print(m.span())

# search
m = re.search("b+", "abbbc")
print(m.group(0))
print(m.start())

# fullmatch
print(re.fullmatch("\\d+", "12345") != None)
print(re.fullmatch("\\d+", "12345x") != None)

# groups: nested
m = re.search("((a)(b))", "ab")
print(m.group(0))
print(m.group(1))
print(m.group(2))
print(m.group(3))
print(m.groups())

# named groups
m = re.search("(?P<user>\\w+)@(?P<host>\\w+)", "bob@example")
print(m.group("user"))
print(m.group("host"))
print(m.group(0))

# greedy vs lazy
print(re.search("<(.*)>", "<a><b>").group(1))
print(re.search("<(.*?)>", "<a><b>").group(1))

# repetition ranges
print(re.match("a{2,4}", "aaaaa").group(0))
print(re.match("a{2,4}?", "aaaaa").group(0))

# anchors and boundaries
print(re.search("\\bword\\b", "a word here").group(0))
print(re.search("^ab$", "ab").group(0))

# lookahead / lookbehind
print(re.search("foo(?=bar)", "foobar").group(0))
print(re.search("foo(?!bar)", "foobaz").group(0))
print(re.search("(?<=foo)bar", "foobar").group(0))
print(re.search("(?<!foo)bar", "bazbar").group(0))

# backreference
print(re.search("(\\w+) \\1", "hello hello").group(0))

# (?P=name) named backreference
print(re.search("(?P<w>\\w+) (?P=w)", "hey hey").group(0))

# inline flags
print(re.search("(?i)HELLO", "say HELLO").group(0))
print(re.search("HELLO", "say hello", re.I).group(0))
print(re.match("^b", "a\nb", re.M).group(0))
print(re.search("a.b", "a\nb", re.S).group(0))

# non-capturing groups
print(re.search("(?:ab)+c", "ababc").group(0))

# negated char class
print(re.search("[^abc]+", "xyza").group(0))

# hex escapes
print(re.match("\\x41\\x42", "ABC").group(0))

# compile + reuse in loop
pat = re.compile("(\\w+)\\s+(\\w+)")
m = pat.search("hello world")
print(m.group(1))
print(m.group(2))

# Pattern methods
print(pat.match("foo bar baz").group(2))
print(pat.findall("aa bb cc"))

# empty match edge cases
print(re.match("(a*)*", "aaa").group(1))
print(re.findall("a*", "aab"))

# no match -> None
print(re.match("\\d+", "abc") == None)
print(re.search("z", "abc") == None)
