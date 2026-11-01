#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "kds/stats/wait_event.hpp"

// CJ's WE12 (`instructions/v3.2.0/workorder-cj-wait-events-ash.md`): the event
// dictionary, `docs/spec/wait-events.md`, must say what the registry says.

#ifndef KDS_SOURCE_DIR
#error "KDS_SOURCE_DIR is set by tests/CMakeLists.txt"
#endif

namespace kds::stats {
namespace {

struct Section {
    std::string name;
    std::map<std::string, std::string> fields;  // "Class" -> "Commit", "p1" -> "target_lsn — ..."
};

std::vector<Section> ReadDictionary() {
    const std::string path = std::string(KDS_SOURCE_DIR) + "/docs/spec/wait-events.md";
    std::ifstream in(path);
    if (!in) ADD_FAILURE() << "cannot open " << path;
    std::vector<Section> out;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("### ", 0) == 0) {
            out.push_back({line.substr(4), {}});
        } else if (!out.empty() && line.rfind("- **", 0) == 0) {
            const std::size_t close = line.find(":**");
            if (close == std::string::npos) continue;
            std::string value = line.substr(close + 3);
            if (!value.empty() && value.front() == ' ') value.erase(0, 1);
            const std::string key = line.substr(4, close - 4);
            if (!out.back().fields.emplace(key, value).second) {
                ADD_FAILURE() << out.back().name << " repeats " << key;
            }
        }
    }
    return out;
}

// A parameter's documented name: the first word, or "" for "(unused)".
std::string ParamName(const std::string& text) {
    if (text.rfind("(unused)", 0) == 0) return "";
    return text.substr(0, text.find_first_of(" "));
}

TEST(WaitEventDoc, EveryRegistryEventIsDocumentedInRegistryOrderAndNothingElse) {
    const std::vector<Section> doc = ReadDictionary();
    ASSERT_EQ(doc.size(), kWaitEventCount) << "one section per event";
    for (std::size_t i = 0; i < kWaitEventCount; ++i) {
        const WaitEventDef& d = kWaitEvents[i];
        const Section& s = doc[i];
        EXPECT_EQ(s.name, d.name) << "section " << i;
        const auto field = [&](const char* key) {
            const auto it = s.fields.find(key);
            EXPECT_NE(it, s.fields.end()) << d.name << " lacks " << key;
            return it == s.fields.end() ? std::string() : it->second;
        };
        EXPECT_EQ(field("Class"), WaitClassName(d.wait_class)) << d.name;
        EXPECT_EQ(field("Kind"), d.kind == WaitKind::kTimed ? "timed" : "counter") << d.name;
        EXPECT_EQ(field("In DB time"), d.in_db_time ? "yes" : "no") << d.name;
        EXPECT_EQ(ParamName(field("p1")), d.p1) << d.name;
        EXPECT_EQ(ParamName(field("p2")), d.p2) << d.name;
        EXPECT_EQ(ParamName(field("p3")), d.p3) << d.name;
        for (const char* key : {"Meaning", "Usual causes", "Remedies", "Sites"}) {
            EXPECT_FALSE(field(key).empty()) << d.name << " has an empty " << key;
        }
    }
}

}  // namespace
}  // namespace kds::stats
