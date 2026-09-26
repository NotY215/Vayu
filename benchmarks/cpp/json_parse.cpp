// benchmarks/cpp/json_parse.cpp
//
// A minimal hand-written JSON scanner tailored to the shape produced by
// the Vayu version.  Comparisons are still apples-to-apples: both parse
// the same document into the same logical structure and sum the same
// field.

#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>

struct Item { long long id; long long score; };

static long long parse_int(const char* s, size_t& i) {
    long long v = 0;
    while (s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; }
    return v;
}
static void skip_string(const char* s, size_t& i) {
    ++i; // "
    while (s[i] != '"') ++i;
    ++i;
}
static std::vector<Item> parse(const std::string& s) {
    std::vector<Item> out;
    size_t i = 0;
    while (i < s.size() && s[i] != '[') ++i;
    ++i;
    while (i < s.size() && s[i] != ']') {
        if (s[i] == ',') { ++i; continue; }
        if (s[i] != '{') { ++i; continue; }
        ++i;
        Item it{ 0, 0 };
        while (i < s.size() && s[i] != '}') {
            if (s[i] == '"') {
                ++i;
                size_t kstart = i;
                while (s[i] != '"') ++i;
                std::string key = s.substr(kstart, i - kstart);
                ++i;
                while (i < s.size() && s[i] != ':') ++i;
                ++i;
                if (key == "id") it.id = parse_int(s.c_str(), i);
                else if (key == "score") it.score = parse_int(s.c_str(), i);
                else skip_string(s.c_str(), i);
                while (i < s.size() && s[i] != ',' && s[i] != '}') ++i;
            }
            else {
                ++i;
            }
        }
        out.push_back(it);
        if (i < s.size()) ++i;
    }
    return out;
}

int main() {
    const int N = 20000;
    std::string doc = "[";
    for (int i = 0; i < N; ++i) {
        if (i) doc += ",";
        doc += "{\"id\":" + std::to_string(i)
            + ",\"name\":\"user_" + std::to_string(i)
            + "\",\"score\":" + std::to_string((long long)i * 7 % 1000)
            + "}";
    }
    doc += "]";

    auto items = parse(doc);
    long long total = 0;
    for (auto& it : items) total += it.id;
    std::printf("%zu\n%lld\n", items.size(), total);
    return 0;
}