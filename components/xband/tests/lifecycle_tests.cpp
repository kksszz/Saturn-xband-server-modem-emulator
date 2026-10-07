#include <xband/connection_lifecycle.hpp>
#include <iostream>
#include <stdexcept>
using C = xband::protocol::ConnectionLifecycle;
constexpr std::string_view session = "0123456789abcdef0123456789abcdef";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
uint64_t activate(C &c, uint64_t now = 0) {
    const auto g = c.begin(now);
    check(c.state() == C::State::hello_wait, "TCP immediately waits for hello");
    check(c.hello(g, "saturn-1", now), "validated hello");
    check(c.authenticationCompleted(g, true, session, now), "external verifier approved");
    return g;
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view test = argv[1]; C c;
        if (test == "authentication") {
            auto g = c.begin(0);
            check(!c.received(g, 2, "saturn-1", session, "2", 1) && c.state() == C::State::closed, "no data before hello verification");
            g = c.begin(0); check(c.hello(g, "saturn-1", 2), "plaintext hello accepted");
            check(!c.authenticationCompleted(g, false, session, 3) && c.failure() == C::Failure::authentication, "denied key closes");
            check(!c.authenticationCompleted(g, true, session, 4), "no second auth attempt");
            g = activate(c);
            check(c.received(g, 2, "saturn-1", session, "2", 1), "first post-hello inbound ID");
            check(!c.received(g, 2, "saturn-1", session, "2", 2) && c.failure() == C::Failure::protocol, "duplicate closes");
            g = c.begin(0); c.hello(g, "saturn-1", 0);
            check(!c.authenticationCompleted(g, true, "bad", 1), "invalid issued session rejected");
            g = c.begin(0);
            check(!c.authenticationCompleted(g, true, session, 0), "no approval before hello");
            g = c.begin(0); c.hello(g, "saturn-1", 0);
            check(!c.hello(g, "saturn-1", 1), "duplicate hello rejected");
        } else if (test == "deadlines") {
            auto g = c.begin(100);
            check(c.poll(g, 5099), "before handshake deadline");
            check(!c.poll(g, 5100) && c.failure() == C::Failure::timeout, "exact handshake deadline");
            g = c.begin(0); c.hello(g, "saturn-1", 4999);
            check(!c.authenticationCompleted(g, true, session, 5000), "hello does not extend deadline");
            g = activate(c);
            check(c.poll(g, 1999) && !c.pingDue(), "ping not due");
            check(c.poll(g, 2000) && c.pingDue() && c.pingDue(), "passive ping observation");
            check(c.pingQueued(g, 2000) && !c.pingDue(), "queued ping resets schedule");
            check(c.poll(g, 9999), "before idle deadline");
            check(!c.poll(g, 10000), "outbound ping cannot keep dead peer alive");
            g = activate(c);
            check(c.received(g, 2, "saturn-1", session, "2", 9000), "valid receive refresh");
            check(c.poll(g, 18999) && !c.poll(g, 19000), "refreshed idle deadline");
        } else if (test == "requests") {
            auto g = activate(c);
            check(c.requestQueued(g, 2, 100), "start request");
            check(!c.requestQueued(g, 3, 200), "one outstanding request");
            check(c.received(g, 2, "saturn-1", session, "2", 5000), "unrelated valid traffic");
            check(!c.poll(g, 5100), "traffic does not postpone request");
            g = activate(c); c.requestQueued(g, 2, 0);
            check(c.received(g, 2, "saturn-1", session, "2", 4999) && c.requestCompleted(g, 2, 4999), "on-time response");
            check(c.poll(g, 5000), "completed request disarmed");
            check(c.requestQueued(g, 3, 5000) && !c.requestCompleted(g, 2, 5001), "wrong reply rejected");
        } else if (test == "generations") {
            const auto old = activate(c); const auto current = activate(c, 100);
            check(old != current && !c.authenticationCompleted(old, false, "", 0), "stale callback ignored");
            check(!c.poll(old, UINT64_MAX) && c.state() == C::State::active, "old timer cannot kill new session");
            check(!c.poll(current, 99) && c.failure() == C::Failure::clock_reversed, "clock reversal closes");
            const auto g = activate(c, UINT64_MAX - 10000);
            check(c.poll(g, UINT64_MAX - 1) && !c.poll(g, UINT64_MAX), "deadline arithmetic does not overflow");
            C other; const auto other_g = activate(other);
            check(other.poll(other_g, 1), "instance independence");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << test << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
