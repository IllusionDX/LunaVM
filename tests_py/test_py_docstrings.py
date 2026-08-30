doc = """hello
world"""
print(doc)

doc2 = '''single
triple'''
print(doc2)

print("""inline""")

def f():
    """function docstring"""
    return 42

print(f())

class C:
    """class docstring"""
    def __init__(self):
        self.x = 1

    def m(self):
        """method docstring"""
        return self.x + 1

print(C().m())

# line comment is ignored
print("after comment")

m = """multi
line string
"""
print(m)

print("escaped \"quote\" inside triple")