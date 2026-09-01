class Color(Enum):
    RED = auto()
    GREEN = auto()
    BLUE = auto()

print(Color.RED)
print(Color.GREEN)
print(Color.BLUE)

class Direction(Enum):
    NORTH = 0
    SOUTH = 1
    EAST = 2
    WEST = 3

print(Direction.NORTH)
print(Direction.SOUTH)
print(Direction.EAST)
print(Direction.WEST)

class Mixed(Enum):
    A = 10
    B = auto()
    C = 99

print(Mixed.A)
print(Mixed.B)
print(Mixed.C)
