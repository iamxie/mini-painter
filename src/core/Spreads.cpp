#include "core/Spreads.h"

namespace mp {

std::vector<Spread> buildSpreads(const std::vector<bool>& wide, bool firstAlone, bool shiftOne) {
    std::vector<Spread> out;
    int n = static_cast<int>(wide.size());
    int i = 0;
    if (firstAlone && i < n) { out.push_back({i, -1}); ++i; }
    if (shiftOne && i < n) { out.push_back({i, -1}); ++i; }
    while (i < n) {
        if (wide[static_cast<size_t>(i)]) { out.push_back({i, -1}); ++i; continue; }
        if (i + 1 < n && !wide[static_cast<size_t>(i) + 1]) { out.push_back({i, i + 1}); i += 2; continue; }
        out.push_back({i, -1});
        ++i;
    }
    return out;
}

int spreadIndexOf(const std::vector<Spread>& spreads, int page) {
    for (size_t k = 0; k < spreads.size(); ++k)
        if (spreads[k].first == page || spreads[k].second == page) return static_cast<int>(k);
    return -1;
}

} // namespace mp
