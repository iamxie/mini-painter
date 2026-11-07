#include <catch2/catch_test_macros.hpp>

#include "core/Spreads.h"

using namespace mp;

namespace {
bool same(const std::vector<Spread>& s, std::initializer_list<std::pair<int, int>> want) {
    if (s.size() != want.size()) return false;
    size_t i = 0;
    for (auto [a, b] : want) {
        if (s[i].first != a || s[i].second != b) return false;
        ++i;
    }
    return true;
}
} // namespace

TEST_CASE("buildSpreads：第一張單獨，其餘兩兩配對，尾數單獨") {
    std::vector<bool> w(8, false);
    CHECK(same(buildSpreads(w, true, false), {{0, -1}, {1, 2}, {3, 4}, {5, 6}, {7, -1}}));
    CHECK(same(buildSpreads(w, false, false), {{0, 1}, {2, 3}, {4, 5}, {6, 7}}));
}

TEST_CASE("buildSpreads：寬圖單獨一屏並重新配對") {
    std::vector<bool> w(8, false);
    w[5] = true;
    CHECK(same(buildSpreads(w, true, false), {{0, -1}, {1, 2}, {3, 4}, {5, -1}, {6, 7}}));
    // 兩張寬圖相鄰
    w[6] = true;
    CHECK(same(buildSpreads(w, true, false), {{0, -1}, {1, 2}, {3, 4}, {5, -1}, {6, -1}, {7, -1}}));
    // 寬圖在第二張：第一張單獨後，第二張也單獨
    std::vector<bool> w2 = {false, true, false, false};
    CHECK(same(buildSpreads(w2, false, false), {{0, -1}, {1, -1}, {2, 3}}));
}

TEST_CASE("buildSpreads：錯開一頁") {
    std::vector<bool> w(6, false);
    CHECK(same(buildSpreads(w, false, true), {{0, -1}, {1, 2}, {3, 4}, {5, -1}}));
    CHECK(same(buildSpreads(w, true, true), {{0, -1}, {1, -1}, {2, 3}, {4, 5}}));
}

TEST_CASE("buildSpreads：空清單與單張") {
    CHECK(buildSpreads({}, true, true).empty());
    CHECK(same(buildSpreads({false}, true, true), {{0, -1}}));
    CHECK(same(buildSpreads({false}, false, false), {{0, -1}}));
    CHECK(same(buildSpreads({true}, false, false), {{0, -1}}));
}

TEST_CASE("spreadIndexOf") {
    auto s = buildSpreads(std::vector<bool>(5, false), true, false); // {0},{1,2},{3,4}
    CHECK(spreadIndexOf(s, 0) == 0);
    CHECK(spreadIndexOf(s, 1) == 1);
    CHECK(spreadIndexOf(s, 2) == 1);
    CHECK(spreadIndexOf(s, 4) == 2);
    CHECK(spreadIndexOf(s, 5) == -1);
    CHECK(spreadIndexOf({}, 0) == -1);
}
