# benchmarks/python/string_concat.py
N = 100000
CHUNK = "abcdefghijklmnop"

parts = []
for _ in range(N):
    parts.append(CHUNK)
s = "".join(parts)
print(len(s))