#include "catalog.hpp"
#include <functional>
#include <iostream>
using maxhelp::Json;
int main() {
    int checks = 0;
    try {
        const Json valid = {
            {"version", "test.1"},
            {"topics", Json::array({
                {{"id","science"},{"parent_id",nullptr},{"level",1},{"label","Science"},{"aliases",Json::array()},{"active",true}},
                {{"id","science.math"},{"parent_id","science"},{"level",2},{"label","Math"},{"aliases",Json::array()},{"active",true}},
                {{"id","science.math.percent"},{"parent_id","science.math"},{"level",3},{"label","Percent"},{"aliases",Json::array()},{"active",true}}
            })},
            {"facets", Json::array({
                {{"id","level"},{"label","Level"},{"applicable_topic_prefixes",Json::array({"science"})},
                 {"values",Json::array({{{"id","beginner"},{"label","Beginner"}}})}}
            })},
            {"curated_equivalences",Json::array()},
            {"rules",{{"competency_minimum_level",2},{"optional_deeper_levels",true},{"within_facet","OR"},
                {"between_required_facets","AND"},{"ancestor_does_not_claim_all_descendants",true},{"new_topics_require_review",true}}}
        };
        maxhelp::validate_catalog(valid); ++checks;
        auto rejects = [&](const std::function<void(Json&)>& mutate) {
            auto candidate = valid; mutate(candidate);
            try { maxhelp::validate_catalog(candidate); }
            catch (const std::exception&) { ++checks; return; }
            throw std::runtime_error("Malformed catalog was accepted; case " + std::to_string(checks));
        };
        rejects([](Json& c){ c.erase("version"); });
        rejects([](Json& c){ c["topics"].push_back(c["topics"][0]); });
        rejects([](Json& c){ c["topics"][0]["parent_id"]="science.math"; });
        rejects([](Json& c){ c["topics"][1]["parent_id"]="missing"; });
        rejects([](Json& c){ c["topics"][1]["parent_id"]="science.math.percent"; }); // cycle
        rejects([](Json& c){ c["topics"][1]["level"]=0; });
        rejects([](Json& c){ c["topics"][1]["level"]=2.5; });
        rejects([](Json& c){ c["topics"][2]["id"]="languages.english"; });
        rejects([](Json& c){ c["topics"][0]["active"]=false; });
        rejects([](Json& c){ c["topics"][1]["aliases"]=Json::array({"Math","Math"}); });
        rejects([](Json& c){ c["topics"][1]["label"]=""; });
        rejects([](Json& c){ c["facets"].push_back(c["facets"][0]); });
        rejects([](Json& c){ c["facets"][0]["applicable_topic_prefixes"]=Json::array({"sci"}); });
        rejects([](Json& c){ c["facets"][0]["applicable_topic_prefixes"]=Json::array({"*","science"}); });
        rejects([](Json& c){ c["facets"][0]["values"].push_back(c["facets"][0]["values"][0]); });
        rejects([](Json& c){ c["facets"][0]["values"]=Json::array(); });
        rejects([](Json& c){ c["rules"]["competency_minimum_level"]=1; });
        rejects([](Json& c){ c["rules"]["ancestor_does_not_claim_all_descendants"]=false; });
        rejects([](Json& c){ c["curated_equivalences"].push_back({"science","science.math"}); });
        auto inactive = valid;
        inactive["topics"][1]["active"]=false; inactive["topics"][2]["active"]=false;
        maxhelp::validate_catalog(inactive); ++checks;
        auto unordered = valid;
        std::swap(unordered["topics"][0], unordered["topics"][2]);
        maxhelp::validate_catalog(unordered); ++checks;
        std::cout << checks << " catalog checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
