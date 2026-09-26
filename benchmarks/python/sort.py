# benchmarks/python/sort.py
N = 500000

a = []
seed = 12345
for _ in range(N):
    seed = (seed * 1103515245 + 12345) % 2147483648
    a.append(seed % 1000000)

a.sort()

checksum = 0
for i, v in enumerate(a):
    checksum = (checksum + v * (i + 1)) % 1000000007
print(checksum)