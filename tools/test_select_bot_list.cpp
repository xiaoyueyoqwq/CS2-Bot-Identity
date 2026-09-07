#include "bot_info.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static int g_fails = 0;

static void expect(bool cond, const char* name) {
    if (!cond) {
        std::cerr << "FAIL " << name << "\n";
        ++g_fails;
    } else {
        std::cout << "ok " << name << "\n";
    }
}

static fs::path make_tree() {
    auto tmp = fs::temp_directory_path() / "botid-lang-test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "addons/counterstrikesharp/configs");
    fs::create_directories(tmp / "addons/BotIdentity/lang");
    return tmp;
}

static void write_core(const fs::path& root, const std::string& body) {
    std::ofstream f(root / "addons/counterstrikesharp/configs/core.json");
    f << body;
}

int main() {
    auto tmp = make_tree();
    const std::string base = tmp.string();

    // CSS core.json puts arrays and bools before ServerLanguage.
    write_core(tmp,
               "{\n"
               "  \"PublicChatTrigger\": [\"!\"],\n"
               "  \"SilentChatTrigger\": [\"/\"],\n"
               "  \"FollowCS2ServerGuidelines\": true,\n"
               "  \"PluginHotReloadEnabled\": true,\n"
               "  \"ServerLanguage\": \"zh-Hans-CN\"\n"
               "}\n");
    auto sel = botid::SelectBotList(base);
    expect(sel.cssLanguageFound && sel.cssLanguage == "zh-Hans-CN" &&
               sel.matched == "zh-CN" && sel.relativeFile == "lang/zh-CN.json",
           "css zh-Hans-CN after arrays");

    write_core(tmp, "{\"ServerLanguage\":\"zh-CN\"}");
    sel = botid::SelectBotList(base);
    expect(sel.cssLanguageFound && sel.matched == "zh-CN", "css zh-CN");

    write_core(tmp, "{\"ServerLanguage\":\"zh-TW\"}");
    sel = botid::SelectBotList(base);
    expect(sel.cssLanguageFound && sel.matched == "default" &&
               sel.relativeFile == "bots.json",
           "css zh-TW");

    write_core(tmp, "{\"ServerLanguage\":\"zh-Hant\"}");
    sel = botid::SelectBotList(base);
    expect(sel.cssLanguageFound && sel.matched == "default", "css zh-Hant");

    write_core(tmp, "{\"ServerLanguage\":\"en\"}");
    sel = botid::SelectBotList(base);
    expect(sel.cssLanguageFound && sel.matched == "default", "css en");

    write_core(tmp, "{\"PublicChatTrigger\":[\"!\"]}");
    sel = botid::SelectBotList(base);
    expect(!sel.cssLanguageFound && sel.matched == "default", "missing key");

    fs::remove(tmp / "addons/counterstrikesharp/configs/core.json");
    sel = botid::SelectBotList(base);
    expect(!sel.cssLanguageFound && sel.matched == "default", "missing core.json");

    fs::remove_all(tmp);
    return g_fails ? 1 : 0;
}
