xs = [1, 2, 3]
for i in xs:
    if i == 2:
        continue
    print(i)

n = 0
total = 0
while n < 3:
    total = total + 1
    n = n + 1
print(total)

for i in xs:
    if i == 3:
        break
    print(i)

if False:
    print("bad")
else:
    print("else ok")