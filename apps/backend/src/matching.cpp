#include "matching.hpp"
#include "catalog.hpp"
#include <algorithm>

namespace maxhelp {
namespace {
bool better(const MatchResult& a, const MatchResult& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.load != b.load) return a.load < b.load;
    if (a.last_notified != b.last_notified) return a.last_notified < b.last_notified;
    if (a.user_id != b.user_id) return a.user_id < b.user_id;
    return a.competency_id < b.competency_id;
}
bool intersects(const std::set<std::string>& a, const std::set<std::string>& b) {
    auto left = a.begin(), right = b.begin();
    while (left != a.end() && right != b.end()) {
        if (*left == *right) return true;
        if (*left < *right) ++left; else ++right;
    }
    return false;
}
}
MatchingIndex::MatchingIndex(const Json& catalog, std::vector<MatchCompetency> competencies)
    : competencies_(std::move(competencies)) {
    validate_catalog(catalog);
    std::unordered_map<std::string, std::string> parents;
    for (const auto& topic : catalog.at("topics")) {
        if (!topic.at("active").get<bool>()) continue;
        auto id = topic.at("id").get<std::string>();
        parents[id] = topic.at("parent_id").is_null() ? "" : topic.at("parent_id").get<std::string>();
        if (topic.at("level").get<int>() >= 2) selectable_topics_.insert(id);
    }
    for (std::size_t i = 0; i < competencies_.size(); ++i) {
        const auto& skill = competencies_[i];
        if (!selectable_topics_.contains(skill.topic_id)) continue;
        for (auto topic = skill.topic_id; !topic.empty(); topic = parents.at(topic))
            postings_[topic].push_back({i, topic != skill.topic_id});
    }
}
MatchPage MatchingIndex::search(const MatchQuery& query) const {
    if (!selectable_topics_.contains(query.topic_id) || query.limit == 0 || query.limit > 50)
        throw std::invalid_argument("Invalid match query topic or limit");
    for (const auto& required : query.required_facets)
        if (!query.facets.contains(required) || query.facets.at(required).empty())
            throw std::invalid_argument("Required facet must have values");
    MatchPage page;
    auto found = postings_.find(query.topic_id);
    if (found == postings_.end()) return page;
    std::unordered_map<std::string, MatchResult> best;
    for (const auto& posting : found->second) {
        ++page.inspected_competencies;
        const auto& skill = competencies_[posting.competency];
        if (skill.user_id == query.author_id || !skill.available || skill.load >= skill.capacity
            || query.blocked_users.contains(skill.user_id)) continue;
        bool eligible = true;
        std::size_t hits = 0;
        for (const auto& [facet, values] : query.facets) {
            auto existing = skill.facets.find(facet);
            const bool hit = existing != skill.facets.end() && intersects(values, existing->second);
            if (hit) ++hits;
            else if (query.required_facets.contains(facet)) { eligible = false; break; }
        }
        if (!eligible) continue;
        double numerator = posting.narrower ? 51.0 : 60.0, denominator = 60.0;
        if (!query.facets.empty()) {
            numerator += 30.0 * static_cast<double>(hits) / static_cast<double>(query.facets.size());
            denominator += 30.0;
        }
        if (!query.desired_experience.empty()) {
            numerator += skill.experience == query.desired_experience ? 10.0 : 0.0;
            denominator += 10.0;
        }
        MatchResult result{skill.user_id, skill.id, 100.0 * numerator / denominator,
            posting.narrower, skill.load, skill.last_notified};
        if (result.score < 70.0) continue;
        auto current = best.find(skill.user_id);
        if (current == best.end() || better(result, current->second)) best[skill.user_id] = std::move(result);
    }
    page.eligible_users = best.size();
    page.results.reserve(best.size());
    for (auto& [user, result] : best) page.results.push_back(std::move(result));
    const auto count = std::min(query.limit, page.results.size());
    std::partial_sort(page.results.begin(), page.results.begin() + static_cast<std::ptrdiff_t>(count),
        page.results.end(), better);
    page.results.resize(count);
    return page;
}
} // namespace maxhelp
