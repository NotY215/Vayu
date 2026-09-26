# benchmarks/python/hashmap.py
N = 500000

def key_of(i):
    return f"key_{(i * 2654435761) % 1000000}"

m = {}
for i in range(N):
    m[key_of(i)] = i

total = 0
for i in range(N):
    k = key_of(i)
    if k in m:
        total += m[k]
print(total)
print(len(m))