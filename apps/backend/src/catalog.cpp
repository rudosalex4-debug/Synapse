#include "catalog.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace maxhelp {
namespace {
void require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}
bool text(const Json& value) {
    return value.is_string() && !value.get_ref<const std::string&>().empty()
        && value.get_ref<const std::string&>().find('\0') == std::string::npos;
}
bool identifier(const Json& value, bool path = false) {
    if (!text(value)) return false;
    const auto& id = value.get_ref<const std::string&>();
    if (id.size() > 160 || id.front() == '.' || id.back() == '.' || id.find("..") != std::string::npos) return false;
    return std::all_of(id.begin(), id.end(), [path](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || (path && c == '.');
    });
}
void named(const Json& item, bool path = false) {
    require(item.is_object() && item.contains("id") && identifier(item.at("id"), path)
        && item.contains("label") && text(item.at("label")), "Invalid catalog id or label");
}
}
void validate_catalog(const Json& catalog) {
    require(catalog.is_object() && catalog.contains("version") && text(catalog.at("version")), "Catalog version required");
    require(catalog.contains("topics") && catalog.at("topics").is_array() && !catalog.at("topics").empty(), "Catalog topics required");
    require(catalog.contains("facets") && catalog.at("facets").is_array(), "Catalog facets required");
    std::map<std::string, const Json*> topics;
    for (const auto& topic : catalog.at("topics")) {
        named(topic, true);
        require(topic.contains("level") && topic.at("level").is_number_integer()
            && topic.at("level") >= 1 && topic.at("level") <= 16, "Invalid topic level");
        require(topic.contains("parent_id") && topic.contains("active") && topic.at("active").is_boolean(), "Invalid topic state");
        require(topic.contains("aliases") && topic.at("aliases").is_array(), "Invalid topic aliases");
        std::set<std::string> aliases;
        for (const auto& alias : topic.at("aliases")) {
            require(text(alias), "Invalid topic alias");
            require(aliases.insert(alias.get<std::string>()).second, "Duplicate topic alias");
        }
        require(topics.emplace(topic.at("id").get<std::string>(), &topic).second, "Duplicate topic id");
    }
    for (const auto& [id, topic] : topics) {
        const int level = topic->at("level").get<int>();
        const auto& parent = topic->at("parent_id");
        if (level == 1) {
            require(parent.is_null() && id.find('.') == std::string::npos, "Invalid root topic");
        } else {
            require(parent.is_string() && topics.contains(parent.get<std::string>()), "Missing topic parent");
            const auto& parent_id = parent.get_ref<const std::string&>();
            const auto& parent_topic = *topics.at(parent_id);
            require(parent_topic.at("level").get<int>() == level - 1, "Invalid topic hierarchy");
            const auto prefix = parent_id + ".";
            require(id.starts_with(prefix) && id.find('.', prefix.size()) == std::string::npos, "Topic path must follow its parent");
            require(!topic->at("active").get<bool>() || parent_topic.at("active").get<bool>(), "Active topic has inactive parent");
        }
    }
    std::set<std::string> facets;
    for (const auto& facet : catalog.at("facets")) {
        named(facet);
        require(facets.insert(facet.at("id").get<std::string>()).second, "Duplicate facet id");
        require(facet.contains("applicable_topic_prefixes") && facet.at("applicable_topic_prefixes").is_array()
            && !facet.at("applicable_topic_prefixes").empty(), "Facet scope required");
        std::set<std::string> prefixes;
        for (const auto& prefix : facet.at("applicable_topic_prefixes")) {
            require(text(prefix), "Invalid facet scope");
            const auto value = prefix.get<std::string>();
            require(value == "*" || topics.contains(value), "Unknown facet topic");
            require(prefixes.insert(value).second, "Duplicate facet scope");
        }
        require(!prefixes.contains("*") || prefixes.size() == 1, "Wildcard facet scope must stand alone");
        require(facet.contains("values") && facet.at("values").is_array() && !facet.at("values").empty(), "Facet values required");
        std::set<std::string> values;
        for (const auto& value : facet.at("values")) {
            named(value);
            require(values.insert(value.at("id").get<std::string>()).second, "Duplicate facet value");
        }
    }
    require(catalog.contains("rules") && catalog.at("rules").is_object(), "Catalog rules required");
    const auto& rules = catalog.at("rules");
    require(rules.value("competency_minimum_level", 0) == 2
        && rules.value("optional_deeper_levels", false)
        && rules.value("within_facet", "") == "OR"
        && rules.value("between_required_facets", "") == "AND"
        && rules.value("ancestor_does_not_claim_all_descendants", false)
        && rules.value("new_topics_require_review", false), "Unsupported catalog rules");
    // Equivalence matching has no implementation yet: never silently accept it.
    require(catalog.contains("curated_equivalences") && catalog.at("curated_equivalences").is_array()
        && catalog.at("curated_equivalences").empty(), "Catalog equivalences are not supported yet");
}
} // namespace maxhelp
