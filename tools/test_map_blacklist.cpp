#include "bot_info.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fails = 0;

static void expect(bool cond, const char* name) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", name);
        ++g_fails;
    } else {
        std::printf("ok %s\n", name);
    }
}

int main() {
    const std::vector<std::string> tokens = {"3171695956", "cabin"};
    std::string hit;

    expect(!botid::MapNameMatchesBlacklist("", tokens), "empty map");
    expect(!botid::MapNameMatchesBlacklist("de_dust2", tokens), "dust2");
    expect(!botid::MapNameMatchesBlacklist("de_nuke", tokens), "nuke");
    expect(!botid::MapNameMatchesBlacklist("de_dust2", {}), "empty tokens");

    hit.clear();
    expect(botid::MapNameMatchesBlacklist("workshop/3171695956/cabin", tokens, &hit), "workshop cabin");
    expect(hit == "3171695956", "workshop token is id first");

    hit.clear();
    expect(botid::MapNameMatchesBlacklist("Cabin", tokens, &hit), "Cabin case");
    expect(hit == "cabin", "Cabin token");

    hit.clear();
    expect(botid::MapNameMatchesBlacklist("workshop/3171695956/foo", tokens, &hit), "id only");
    expect(hit == "3171695956", "id token");

    return g_fails ? 1 : 0;
}
