#include "jobs.hpp"
#include "services.hpp"
#include "notifications.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

// Integration test: use a migrated, disposable PostgreSQL database and stop all
// workers before running. DATABASE_URL is required; every MAX call is injected.
// Cleanup is limited to the unique synthetic identities created by this run.
namespace {
using namespace maxhelp;
int checks = 0;
bool cleanup_failed = false;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

class Fixture {
    Db& db_;
    const Config& config_;
    std::string prefix_ = "queue-test:" + uuid();
    std::int64_t user_number_;
    std::int64_t chat_number_;
    int message_number_ = 0;
public:
    const std::string user;
    const std::string chat;

    Fixture(Db& db, const Config& config)
        : db_(db), config_(config),
          user_number_(std::stoll(sha256(prefix_).substr(0, 15), nullptr, 16) + 1),
          chat_number_(-user_number_), user(std::to_string(user_number_)),
          chat(std::to_string(chat_number_)) {
        auto existing = db_.exec("SELECT count(*) AS count FROM max_dialogs WHERE max_user_id=$1", {user});
        check(existing.get(0, "count") == "0", "synthetic identity must be unique");
    }

    ~Fixture() {
        try {
            Transaction tx(db_);
            db_.exec("DELETE FROM outbox WHERE max_user_id=$1", {user});
            db_.exec("DELETE FROM bot_inbox WHERE payload->>'user_id'=$1", {user});
            db_.exec("DELETE FROM max_dialogs WHERE max_user_id=$1", {user});
            tx.commit();
        } catch (const std::exception&) {
            cleanup_failed = true;
            std::cerr << "Synthetic queue fixture cleanup failed\n";
        }
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    Json lifecycle(const std::string& type, std::int64_t timestamp) const {
        return Json{{"update_type", type}, {"timestamp", timestamp},
                    {"chat_id", chat_number_},
                    {"user", {{"user_id", user_number_}, {"is_bot", false}}}};
    }

    Json help(std::int64_t timestamp) {
        return Json{{"update_type", "message_created"}, {"timestamp", timestamp},
                    {"message", {{"recipient", {{"chat_type", "dialog"}, {"chat_id", chat_number_}}},
                                 {"sender", {{"user_id", user_number_}, {"is_bot", false}}},
                                 {"body", {{"mid", prefix_ + ":" + std::to_string(++message_number_)},
                                           {"text", "/help"}}}}}};
    }

    std::string accept(const Json& update) {
        const auto parsed = parse_update(update);
        check(parsed.has_value(), "synthetic update must parse");
        accept_webhook(db_, update);
        return parsed->key;
    }

    void drain_inbox(int expected) {
        int processed = 0;
        while (process_inbox(db_, config_)) {
            if (++processed > 50) throw std::runtime_error("unexpected concurrent inbox producer");
        }
        check(processed == expected, "unexpected number of processed inbox events");
    }

    Result job(const std::string& key) {
        return db_.exec("SELECT status,attempts,lease_token,lock_until,last_error,"
                        "extract(epoch FROM available_at-now())::int AS retry_delay "
                        "FROM outbox WHERE event_key=$1 AND max_user_id=$2", {key, user});
    }

    void expect_job(const std::string& key, const std::string& status, int attempts) {
        auto row = job(key);
        check(row.size() == 1, "expected exactly one outbox job");
        check(row.get(0, "status") == status, "unexpected outbox status: " + status);
        check(row.get(0, "attempts") == std::to_string(attempts), "unexpected attempt count");
        if (status != "processing") {
            check(row.is_null(0, "lease_token"), "completed/retried job must clear lease token");
            check(row.is_null(0, "lock_until"), "completed/retried job must clear lease expiry");
        }
    }

    int count(const std::string& table) {
        // The table name is a test-source constant, never external input.
        if (table == "bot_inbox")
            return std::stoi(db_.exec("SELECT count(*) AS count FROM bot_inbox WHERE payload->>'user_id'=$1", {user}).get(0, "count"));
        if (table == "outbox")
            return std::stoi(db_.exec("SELECT count(*) AS count FROM outbox WHERE max_user_id=$1", {user}).get(0, "count"));
        throw std::runtime_error("invalid test table");
    }

    HttpResult success(const HttpRequest& request) const {
        check(request.authorization == config_.token, "transport must receive synthetic token");
        check(request.url == config_.api_base_url + "/messages?chat_id=" + chat,
              "transport must target this fixture's chat");
        check(parse_json_strict(request.body).is_object(), "outbound body must be valid JSON");
        return {200, R"({"message":{"body":{"mid":"synthetic-delivery"}}})", 0};
    }

    bool deliver(int& calls) {
        return process_outbox(db_, config_, [&](const HttpRequest& request) {
            ++calls;
            return success(request);
        });
    }

    void established_dialog() {
        const auto key = accept(lifecycle("bot_started", 100));
        drain_inbox(1);
        int calls = 0;
        check(deliver(calls), "initial welcome must be deliverable");
        check(calls == 1, "initial welcome must send once");
        expect_job(key, "done", 1);
    }

    void insert_delayed_job(const BotEvent& event) {
        const auto response = response_for(event, config_.bot_username);
        check(response.has_value(), "delayed event must have a response");
        db_.exec("INSERT INTO outbox(event_key,kind,chat_id,max_user_id,payload,event_timestamp) "
                 "VALUES($1,$2,$3,$4,$5::jsonb,$6::bigint)",
                 {event.key, response->kind, event.chat_id, user, outbound_json(*response).dump(),
                  std::to_string(event.timestamp)});
    }
};

// Real notification records, isolated from both application and seeded users.
// Own the topic too, so this queue suite needs migrations but no catalog seed.
class ProductFixture {
    Db& db_;
    const Config& config_;
    std::string author_ = uuid(), helper_ = uuid();
    std::string topic_ = "queue-test-topic:" + uuid();
public:
    ProductFixture(Db& db, const Config& config, const Fixture& dialog)
        : db_(db), config_(config) {
        Transaction tx(db_);
        db_.exec("INSERT INTO users(id,max_user_id,display_name,provenance,product_notifications) "
                 "VALUES($1::uuid,$2,'Queue author','self_declared',true),"
                 "($3::uuid,$4,'Queue helper','self_declared',false)",
                 {author_,dialog.user,helper_,"queue-helper:"+helper_});
        db_.exec("INSERT INTO topics(id,level,label,taxonomy_version) VALUES($1,2,'Queue topic','queue-test')", {topic_});
        tx.commit();
    }
    ~ProductFixture() {
        try {
            Transaction tx(db_);
            db_.exec("DELETE FROM help_offers WHERE helper_id=$1::uuid", {helper_});
            db_.exec("DELETE FROM help_requests WHERE author_id=$1::uuid", {author_});
            db_.exec("DELETE FROM users WHERE id IN ($1::uuid,$2::uuid)", {author_,helper_});
            db_.exec("DELETE FROM topics WHERE id=$1", {topic_});
            tx.commit();
        } catch (const std::exception&) {
            cleanup_failed = true;
            std::cerr << "Synthetic notification fixture cleanup failed\n";
        }
    }
    ProductFixture(const ProductFixture&) = delete;
    ProductFixture& operator=(const ProductFixture&) = delete;

    std::string enqueue() {
        Transaction tx(db_);
        db_.exec("SELECT id FROM users WHERE id IN ($1::uuid,$2::uuid) ORDER BY id FOR UPDATE", {author_,helper_});
        const auto request=uuid(), offer=uuid();
        db_.exec("INSERT INTO help_requests(id,author_id,title,body,learning_goal,topic_id,facets,required_facets,taxonomy_version,status,expires_at) "
                 "VALUES($1::uuid,$2::uuid,'Queue question','A synthetic question for notification queue isolation.',"
                 "'understand',$3,'{}'::jsonb,'[]'::jsonb,'queue-test','open',now()+interval '1 day')",
                 {request,author_,topic_});
        db_.exec("INSERT INTO help_offers(id,request_id,helper_id,message,competency_snapshot,score,narrower) "
                 "VALUES($1::uuid,$2::uuid,$3::uuid,'Synthetic offer','{}'::jsonb,80,false)", {offer,request,helper_});
        enqueue_product_notification(db_,config_,"offer_received",offer,author_,helper_,request);
        tx.commit();
        return "product:offer_received:"+offer+":"+author_;
    }
};

void test_deduplication(Db& db, const Config& config) {
    Fixture fixture(db, config);
    const auto start = fixture.lifecycle("bot_started", 100);
    const auto help = fixture.help(110);
    const auto start_key = fixture.accept(start);
    fixture.accept(start);
    const auto help_key = fixture.accept(help);
    fixture.accept(help);
    check(fixture.count("bot_inbox") == 2, "duplicate webhook must create one inbox event");
    fixture.drain_inbox(2);
    check(fixture.count("outbox") == 2, "duplicate webhook must create one response per event");
    int calls = 0;
    check(fixture.deliver(calls), "welcome must deliver");
    check(fixture.deliver(calls), "help must deliver");
    check(!fixture.deliver(calls), "finished jobs must not be redelivered");
    check(calls == 2, "each unique event must send once");
    fixture.expect_job(start_key, "done", 1);
    fixture.expect_job(help_key, "done", 1);
    fixture.accept(start);
    fixture.accept(help);
    fixture.drain_inbox(0);
    check(!fixture.deliver(calls), "replay after successful delivery must not resend");
}

void test_backlog_restart(Db& db, const Config& config) {
    Fixture fixture(db, config);
    const auto old_start = fixture.accept(fixture.lifecycle("bot_started", 100));
    const auto old_help = fixture.accept(fixture.help(110));
    fixture.accept(fixture.lifecycle("bot_stopped", 120));
    const auto new_start = fixture.accept(fixture.lifecycle("bot_started", 130));
    fixture.drain_inbox(4);
    check(fixture.job(old_start).size() == 0, "old start must not be revived after restart");
    check(fixture.job(old_help).size() == 0, "old help must not be revived after restart");
    check(fixture.count("outbox") == 1, "only latest generation may enqueue a response");
    int calls = 0;
    check(fixture.deliver(calls), "latest start must deliver");
    check(!fixture.deliver(calls) && calls == 1, "restart must send only the fresh welcome");
    fixture.expect_job(new_start, "done", 1);
}

void test_stop_cancellation_and_stale_claim(Db& db, const Config& config) {
    Fixture fixture(db, config);
    const auto start_key = fixture.accept(fixture.lifecycle("bot_started", 100));
    const auto help_key = fixture.accept(fixture.help(110));
    fixture.drain_inbox(2);
    fixture.accept(fixture.lifecycle("bot_stopped", 120));
    fixture.expect_job(start_key, "cancelled", 0);
    fixture.expect_job(help_key, "cancelled", 0);
    const auto restart_key = fixture.accept(fixture.lifecycle("bot_started", 130));
    fixture.drain_inbox(2);

    // Model a job committed after stop's cancellation scan. The claim check
    // must independently reject the obsolete generation and wrong chat.
    auto delayed = *parse_update(fixture.help(115));
    fixture.insert_delayed_job(delayed);
    auto wrong_chat = *parse_update(fixture.help(140));
    wrong_chat.chat_id = "-" + std::to_string(std::stoll(fixture.user) + 1);
    fixture.insert_delayed_job(wrong_chat);
    int calls = 0;
    check(fixture.deliver(calls), "new generation welcome must deliver");
    check(fixture.deliver(calls), "stale generation must be consumed");
    check(fixture.deliver(calls), "wrong chat must be consumed");
    check(!fixture.deliver(calls) && calls == 1, "obsolete destinations must never reach transport");
    fixture.expect_job(restart_key, "done", 1);
    fixture.expect_job(delayed.key, "cancelled", 0);
    fixture.expect_job(wrong_chat.key, "cancelled", 0);
}

void test_equal_timestamp_inactive_wins(Db& db, const Config& config) {
    Fixture fixture(db, config);
    fixture.accept(fixture.lifecycle("bot_started", 100));
    fixture.accept(fixture.lifecycle("bot_stopped", 200));
    fixture.accept(fixture.lifecycle("bot_started", 200));
    fixture.accept(fixture.help(201));
    auto dialog = db.exec("SELECT active,event_timestamp FROM max_dialogs WHERE max_user_id=$1", {fixture.user});
    check(dialog.get(0, "active") == "f", "late start at stop timestamp must not reactivate dialog");
    check(dialog.get(0, "event_timestamp") == "200", "state timestamp must be preserved");
    fixture.drain_inbox(4);
    check(fixture.count("outbox") == 0, "inactive dialog must produce no response");

    fixture.accept(fixture.lifecycle("bot_started", 300));
    fixture.accept(fixture.lifecycle("dialog_removed", 300));
    fixture.drain_inbox(2);
    dialog = db.exec("SELECT active FROM max_dialogs WHERE max_user_id=$1", {fixture.user});
    check(dialog.get(0, "active") == "f", "removal must win a timestamp tie in either arrival order");
    int calls = 0;
    check(!fixture.deliver(calls) && calls == 0, "inactive state must not call transport");
}

void test_retry_and_success(Db& db, const Config& config) {
    Fixture fixture(db, config);
    fixture.established_dialog();
    const auto key = fixture.accept(fixture.help(110));
    fixture.drain_inbox(1);
    int calls = 0;
    check(process_outbox(db, config, [&](const HttpRequest& request) {
        ++calls;
        fixture.success(request);
        return HttpResult{429, "", 90};
    }), "rate-limited job must be attempted");
    check(calls == 1, "rate limit must perform one attempt per processing call");
    fixture.expect_job(key, "pending", 1);
    auto row = fixture.job(key);
    check(row.get(0, "last_error") == "MAX_429", "rate limit must persist a safe failure code");
    const auto delay = std::stoi(row.get(0, "retry_delay"));
    check(delay >= 85 && delay <= 90, "Retry-After must defer the next attempt");
    check(!fixture.deliver(calls) && calls == 1, "deferred retry must not call transport early");
    const auto later = fixture.accept(fixture.help(120));
    fixture.drain_inbox(1);
    check(!fixture.deliver(calls) && calls == 1, "later prompt must not overtake delayed reply in same dialog");
    db.exec("UPDATE outbox SET available_at=now()-interval '1 second' WHERE event_key=$1 AND max_user_id=$2", {key, fixture.user});
    check(fixture.deliver(calls) && calls == 2, "due retry must send once");
    fixture.expect_job(key, "done", 2);
    check(fixture.job(key).is_null(0, "last_error"), "successful retry must clear failure code");
    check(fixture.deliver(calls) && calls == 3, "later prompt delivers after earlier reply");
    fixture.expect_job(later, "done", 1);
}

void test_notification_retry_does_not_block_bot(Db& db, const Config& base) {
    Config config=base;
    config.product_notifications_enabled=true;
    Fixture fixture(db,config);
    fixture.established_dialog();
    ProductFixture products(db,config,fixture);
    const auto first_product=products.enqueue();
    fixture.expect_job(first_product,"pending",0);
    int calls=0;
    check(process_outbox(db,config,[&](const HttpRequest& request) {
        ++calls;
        fixture.success(request);
        check(parse_json_strict(request.body).at("notify")==true,"first attempt must be the product notification");
        return HttpResult{503,"",90};
    }),"product notification must enter retry through the real transport path");
    fixture.expect_job(first_product,"pending",1);
    check(calls==1&&std::stoi(fixture.job(first_product).get(0,"retry_delay"))>0,"product retry must remain deferred");

    const auto second_product=products.enqueue();
    fixture.expect_job(second_product,"pending",0);
    check(!fixture.deliver(calls)&&calls==1,"later product notification must retain product FIFO");
    const auto first_bot=fixture.accept(fixture.help(110));
    const auto second_bot=fixture.accept(fixture.help(120));
    fixture.drain_inbox(2);
    check(fixture.deliver(calls)&&calls==2,"ready bot response must bypass deferred product notification");
    fixture.expect_job(first_bot,"done",1);
    fixture.expect_job(second_bot,"pending",0);
    fixture.expect_job(first_product,"pending",1);
    fixture.expect_job(second_product,"pending",0);
    check(fixture.deliver(calls)&&calls==3,"second bot response must follow the first response");
    fixture.expect_job(second_bot,"done",1);
    check(!fixture.deliver(calls)&&calls==3,"bot bypass must not consume or accelerate product retries");

    db.exec("UPDATE outbox SET available_at=now()-interval '1 second' WHERE event_key=$1 AND max_user_id=$2", {first_product,fixture.user});
    check(fixture.deliver(calls)&&calls==4,"first notification must retry when it becomes due");
    fixture.expect_job(first_product,"done",2);
    fixture.expect_job(second_product,"pending",0);
    check(fixture.deliver(calls)&&calls==5,"second notification must follow the recovered first notification");
    fixture.expect_job(second_product,"done",1);
    check(!fixture.deliver(calls)&&calls==5,"all replies and notifications must be delivered once after their successful attempt");
}

void test_expired_lease_and_attempt_limit(Db& db, const Config& config) {
    Fixture fixture(db, config);
    fixture.established_dialog();
    const auto key = fixture.accept(fixture.help(110));
    fixture.drain_inbox(1);
    const auto old_lease = uuid();
    db.exec("UPDATE outbox SET status='processing',attempts=1,lease_token=$3::uuid,"
            "lock_until=now()+interval '60 seconds' WHERE event_key=$1 AND max_user_id=$2",
            {key, fixture.user, old_lease});
    int calls = 0;
    check(!fixture.deliver(calls) && calls == 0, "unexpired lease must prevent concurrent delivery");
    db.exec("UPDATE outbox SET lock_until=now()-interval '1 second' WHERE event_key=$1 AND max_user_id=$2", {key, fixture.user});
    check(process_outbox(db, config, [&](const HttpRequest& request) {
        ++calls;
        const auto row = fixture.job(key);
        check(row.get(0, "status") == "processing", "recovered job must hold a processing lease");
        check(!row.is_null(0, "lease_token") && row.get(0, "lease_token") != old_lease,
              "recovery must replace the old lease token");
        return fixture.success(request);
    }), "expired lease must be recovered");
    check(calls == 1, "lease recovery must make one delivery attempt");
    fixture.expect_job(key, "done", 2);

    const auto exhausted = fixture.accept(fixture.help(120));
    fixture.drain_inbox(1);
    db.exec("UPDATE outbox SET status='processing',attempts=5,lease_token=$3::uuid,"
            "lock_until=now()-interval '1 second' WHERE event_key=$1 AND max_user_id=$2",
            {exhausted, fixture.user, uuid()});
    check(!fixture.deliver(calls) && calls == 1, "exhausted lease must not send a sixth attempt");
    fixture.expect_job(exhausted, "dead", 5);
    check(fixture.job(exhausted).get(0, "last_error") == "ATTEMPTS_EXHAUSTED",
          "attempt limit must record a terminal reason");
}

void test_stop_fences_acknowledgement(Db& db, const Config& config) {
    Fixture fixture(db, config);
    fixture.established_dialog();
    const auto key = fixture.accept(fixture.help(110));
    fixture.drain_inbox(1);
    int calls = 0;
    check(process_outbox(db, config, [&](const HttpRequest& request) {
        ++calls;
        auto row = fixture.job(key);
        check(row.get(0, "status") == "processing" && !row.is_null(0, "lease_token"),
              "delivery must acquire a lease before calling transport");
        // A separate connection proves no database transaction remains open
        // across the external call and reproduces a stop during delivery.
        Db webhook_db(config.database_url);
        accept_webhook(webhook_db, fixture.lifecycle("bot_stopped", 120));
        fixture.expect_job(key, "cancelled", 1);
        return fixture.success(request);
    }), "in-flight fake delivery must complete");
    check(calls == 1, "in-flight stop scenario must call transport once");
    fixture.expect_job(key, "cancelled", 1);
    fixture.drain_inbox(1);
    check(!fixture.deliver(calls) && calls == 1, "late acknowledgement must not revive cancelled job");
}

} // namespace

int main() {
    try {
        const char* database_url = std::getenv("DATABASE_URL");
        if (!database_url || !*database_url)
            throw std::runtime_error("DATABASE_URL is required; use a migrated disposable database with all workers stopped");
        Config config;
        config.database_url = database_url;
        config.app_env = "test";
        config.token = "synthetic-queue-test-token";
        config.bot_username = "synthetic_queue_test_bot";
        config.api_base_url = "https://queue-test.invalid";
        Db db(config.database_url);
        const auto pending = db.exec("SELECT (SELECT count(*) FROM bot_inbox WHERE status='pending') + "
                                     "(SELECT count(*) FROM outbox WHERE status IN ('pending','processing')) AS count");
        check(pending.get(0, "count") == "0", "database contains pending work; use a disposable database and stop workers");
        test_deduplication(db, config);
        test_backlog_restart(db, config);
        test_stop_cancellation_and_stale_claim(db, config);
        test_equal_timestamp_inactive_wins(db, config);
        test_retry_and_success(db, config);
        test_notification_retry_does_not_block_bot(db, config);
        test_expired_lease_and_attempt_limit(db, config);
        test_stop_fences_acknowledgement(db, config);
        check(!cleanup_failed, "all synthetic rows must be cleaned up");
        std::cout << "Passed " << checks << " PostgreSQL queue checks; MAX transport was fully simulated\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Queue integration test failed: " << error.what() << '\n';
        return 1;
    }
}
