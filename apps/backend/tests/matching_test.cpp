#include "matching.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace maxhelp;
int main(int argc, char** argv) {
    try {
        std::ifstream input(argc > 1 ? argv[1] : SYNAPSE_CATALOG_PATH);
        Json catalog; input >> catalog;
        std::vector<MatchCompetency> skills;
        auto add = [&](std::string id, std::string user, std::string topic, MatchFacets facets = {}) -> MatchCompetency& {
            skills.push_back({std::move(id),std::move(user),std::move(topic),"practice",std::move(facets)});
            return skills.back();
        };
        add("exact","u1","career.internships.practice",{{"career_stage",{"intern"}}});
        add("split-broad","split","career.internships",{{"career_stage",{"intern"}}});
        add("split-narrow","split","career.internships.practice",{{"career_stage",{"junior"}}});
        add("self","author","career.internships.practice",{{"career_stage",{"intern"}}});
        add("busy","busy","career.internships.practice",{{"career_stage",{"intern"}}}).load=2;
        add("off","off","career.internships.practice",{{"career_stage",{"intern"}}}).available=false;
        add("blocked","blocked","career.internships.practice",{{"career_stage",{"intern"}}});
        add("other","other","pathways.admissions.program_choice");
        add("second","u1","career.internships.practice",{{"career_stage",{"intern"}}});
        MatchingIndex index(catalog, skills);
        MatchQuery query; query.topic_id="career.internships.practice"; query.author_id="author";
        query.facets={{"career_stage",{"intern"}}}; query.required_facets={"career_stage"}; query.blocked_users={"blocked"};
        auto page=index.search(query);
        int checks=0;
        auto check=[&](bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);++checks;};
        check(page.results.size()==1 && page.results[0].user_id=="u1","Eligibility, coherent competency, user deduplication");
        check(page.results[0].score==100 && !page.results[0].narrower,"Exact score and explanation");
        check(page.results[0].competency_id=="exact","Stable competency tie break");
        query.topic_id="career.internships";
        page=index.search(query);
        check(page.results.size()==2 && page.results[0].user_id=="split","Exact broad topic ranks before narrower");
        check(page.results[1].narrower && page.results[1].score==90,"Narrower scope is explicit");
        query.topic_id="pathways.courses";
        query.facets.clear(); query.required_facets.clear();
        check(index.search(query).results.empty(),"Unrelated branch produces no candidates");
        query.topic_id="career";
        try { index.search(query); throw std::runtime_error("L1 accepted"); } catch(const std::invalid_argument&) { ++checks; }
        query.topic_id="digital.spreadsheets.pivot_tables";
        try { index.search(query); throw std::runtime_error("Archived topic accepted"); } catch(const std::invalid_argument&) { ++checks; }
        std::cout<<"PASS "<<checks<<" matching correctness checks\n";
        using Clock=std::chrono::steady_clock;
        for (const std::size_t count : {10000U,100000U}) {
            for (bool dense : {false,true}) {
                std::vector<MatchCompetency> sample; sample.reserve(count);
                for (std::size_t i=0;i<count;++i) {
                    const bool relevant=dense || i%100==0;
                    sample.push_back({std::to_string(i),"user-"+std::to_string(i),
                        relevant?"career.internships.practice":"pathways.admissions.program_choice","practice",{}});
                }
                const auto begin=Clock::now();
                MatchingIndex measured(catalog,std::move(sample));
                const auto built=Clock::now();
                MatchQuery target;target.topic_id="career.internships.practice";target.limit=20;
                const auto result=measured.search(target);
                const auto end=Clock::now();
                check(result.inspected_competencies==(dense?count:count/100),"Index must visit only relevant postings");
                check(result.results.size()==20,"Top K limit");
                std::cout<<Json{{"competencies",count},{"distribution",dense?"all_relevant":"one_percent_relevant"},
                    {"inspected",result.inspected_competencies},{"eligibleUsers",result.eligible_users},
                    {"returned",result.results.size()},
                    {"build_ms",std::chrono::duration<double,std::milli>(built-begin).count()},
                    {"query_ms",std::chrono::duration<double,std::milli>(end-built).count()},
                    {"kind","synthetic_in_memory_not_http_load_test"}}.dump()<<'\n';
            }
        }
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
