#include "bot_info.h"

#include <cstdio>
#include <string>

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
    using botid::IsSimplifiedChinese;
    using botid::NormalizeLanguageTag;

    expect(NormalizeLanguageTag(" ZH_Hans_CN ") == "zh-hans-cn", "normalize");

    expect(IsSimplifiedChinese("zh-Hans-CN"), "zh-Hans-CN");
    expect(IsSimplifiedChinese("zh-CN"), "zh-CN");
    expect(IsSimplifiedChinese("zh-cn"), "zh-cn");
    expect(IsSimplifiedChinese("zh"), "zh");
    expect(IsSimplifiedChinese("zh_Hans_CN"), "zh_Hans_CN");
    expect(IsSimplifiedChinese(" ZH-CN "), "padded zh-CN");
    expect(IsSimplifiedChinese("zh-SG"), "zh-SG");

    expect(!IsSimplifiedChinese("zh-TW"), "zh-TW");
    expect(!IsSimplifiedChinese("zh-Hant"), "zh-Hant");
    expect(!IsSimplifiedChinese("zh-HK"), "zh-HK");
    expect(!IsSimplifiedChinese("zh-MO"), "zh-MO");
    expect(!IsSimplifiedChinese("zh-Hant-CN"), "zh-Hant-CN");
    expect(!IsSimplifiedChinese("en"), "en");
    expect(!IsSimplifiedChinese(""), "empty");
    expect(!IsSimplifiedChinese("ja-JP"), "ja-JP");
    expect(!IsSimplifiedChinese("en-US"), "en-US");

    return g_fails ? 1 : 0;
}
