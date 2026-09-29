#pragma once
#include "domain.hpp"
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace maxhelp {
using MatchFacets = std::map<std::string, std::set<std::string>>;
struct MatchCompetency {
    std::string id, user_id, topic_id, experience;
    MatchFacets facets;
    bool available = true;
    unsigned load = 0, capacity = 2;
    long long last_notified = 0;
};
struct MatchQuery {
    std::string author_id, topic_id, desired_experience;
    MatchFacets facets;
    std::set<std::string> required_facets, blocked_users;
    std::size_t limit = 20;
};
struct MatchResult {
    std::string user_id, competency_id;
    double score = 0;
    bool narrower = false;
    unsigned load = 0;
    long long last_notified = 0;
};
struct MatchPage {
    std::vector<MatchResult> results;
    std::size_t inspected_competencies = 0, eligible_users = 0;
};
// Offline algorithm prototype, not a live authorization or notification service.
// Build from an immutable validated catalog and a consistent competency snapshot.
class MatchingIndex {
    struct Posting { std::size_t competency; bool narrower; };
    std::vector<MatchCompetency> competencies_;
    std::unordered_map<std::string, std::vector<Posting>> postings_;
    std::set<std::string> selectable_topics_;
public:
    MatchingIndex(const Json& catalog, std::vector<MatchCompetency> competencies);
    MatchPage search(const MatchQuery& query) const;
};
} // namespace maxhelp
