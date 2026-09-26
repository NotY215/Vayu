# benchmarks/python/arith.py
n = 1_000_000

sum_int = 0
for i in range(1, n + 1):
    sum_int += i * 3 - i // 4

sum_float = 0.0
for i in range(1, n + 1):
    fi = float(i)
    sum_float += fi * 1.5 / (fi + 1.0)

print(sum_int)
print(sum_float)