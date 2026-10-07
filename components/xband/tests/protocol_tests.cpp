#include <xband/protocol_state.hpp>
#include <xband/service_call.hpp>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace xband::protocol;
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
constexpr std::string_view session = "0123456789abcdef0123456789abcdef";
void run(std::string_view test) {
    if (test == "fields") {
        uint64_t n = 7;
        for (auto bad : {"", "01", "-1", "+1", " 1", "1.0", "1e1", "18446744073709551616"})
            check(!decimal(bad, n) && n == 7, "invalid number must not change output");
        check(decimal("18446744073709551615", n) && n == UINT64_MAX, "uint64 maximum");
        check(decimal("0", n) && n == 0, "zero offset");
        check(token(session) && !token("0123456789ABCDEF0123456789ABCDEF"), "lowercase token");
        check(endpoint("saturn-1_A") && !endpoint("a/b") && !endpoint("") && !endpoint(std::string(33, 'a')), "endpoint syntax");
    } else if (test == "session") {
        SessionGuard guard;
        check(guard.accept(2, "a", session, "1") == Result::inactive, "must bind first");
        check(guard.bind("a", session, 2), "post hello ID");
        check(guard.accept(2, "a", session, "2") == Result::ok, "correct envelope");
        check(guard.accept(2, "a", session, "2") == Result::sequence, "duplicate fatal");
        check(guard.accept(2, "a", session, "3") == Result::inactive, "sticky rejection");
        check(guard.bind("a", session, 1), "rebind");
        check(guard.accept(2, "a", "ffffffffffffffffffffffffffffffff", "1") == Result::stale_session, "old session");
        check(guard.bind("a", session, UINT64_MAX), "last ID");
        check(guard.accept(2, "a", session, "18446744073709551615") == Result::ok, "last ID accepted");
        check(guard.accept(2, "a", session, "18446744073709551615") == Result::sequence, "never wrap");
        for (unsigned version : {1u, 3u}) {
            guard.bind("a", session, 1);
            check(guard.accept(version, "a", session, "1") == Result::invalid, "version rejected");
        }
        guard.bind("a", session, 1);
        check(guard.accept(2, "b", session, "1") == Result::invalid, "endpoint binding");
        check(!guard.bind("a", session, 0), "invalid bind clears session");
    } else if (test == "window") {
        ReceiveWindow window;
        std::vector<uint8_t> chunk(4096, 42);
        for (uint64_t offset = 0; offset < 65536; offset += 4096)
            check(window.accept(offset, chunk) == Result::ok, "fill window");
        check(window.pending() == 65536 && window.nextOffset() == 65536 && window.limit() == 65536, "ack does not grant credit");
        check(window.consume(3) == Result::ok && window.limit() == 65539, "guest consumption grants credit");
        const std::array<uint8_t, 3> tail{1, 2, 3};
        check(window.accept(65536, tail) == Result::ok, "wrap ring");
        check(window.peek().size() == 65533 && window.peek()[0] == 42, "ordered first span");
        check(window.consume(65533) == Result::ok && window.peek().size() == 3, "wrapped tail visible");
        check(window.peek()[0] == 1 && window.peek()[2] == 3, "tail preserved");
        check(window.consume(3) == Result::ok && window.pending() == 0 && window.limit() == 131075, "all credit restored");
    } else if (test == "window_errors") {
        ReceiveWindow window, other;
        const std::array<uint8_t, 1> byte{9};
        check(window.accept(1, byte) == Result::sequence && window.failed(), "gap");
        check(window.accept(0, byte) == Result::inactive && window.peek().empty(), "fault blocks use");
        window.reset();
        check(window.accept(0, byte) == Result::ok && other.nextOffset() == 0, "independent windows");
        check(window.accept(0, byte) == Result::sequence, "duplicate");
        window.reset();
        std::vector<uint8_t> chunk(4096, 4);
        for (uint64_t offset = 0; offset < 65536; offset += 4096) window.accept(offset, chunk);
        check(window.accept(65536, byte) == Result::capacity && window.nextOffset() == 65536, "over capacity unchanged");
        window.reset();
        check(window.pending() == 0 && window.limit() == 65536, "disconnect reset");
        check(window.accept(0, {}) == Result::invalid, "empty data");
        window.reset();
        check(window.accept(0, std::vector<uint8_t>(4097)) == Result::invalid, "chunk maximum");
        window.reset();
        check(window.consume(1) == Result::invalid, "cannot consume absent data");
    } else if (test == "sender") {
        SendWindow sender;
        for (unsigned i = 0; i < 16; ++i) check(sender.commit(4096) == Result::ok, "fill send credit");
        check(sender.commit(1) == Result::capacity && !sender.failed(), "backpressure is retryable");
        check(sender.acknowledge(65536, 65536) == Result::ok, "receipt alone grants no credit");
        check(sender.commit(1) == Result::capacity, "still full");
        check(sender.acknowledge(65536, 65539) == Result::ok, "consumption grants credit");
        check(sender.commit(3) == Result::ok && sender.nextOffset() == 65539, "resume once");
        check(sender.acknowledge(65536, 65539) == Result::ok, "unchanged ACK allowed");
        check(sender.commit(0) == Result::invalid && sender.commit(4097) == Result::invalid, "chunk validation");
        sender.reset();
        check(sender.nextOffset() == 0 && sender.acknowledged() == 0 && sender.limit() == 65536, "fresh call counters");
    } else if (test == "sender_errors") {
        SendWindow sender;
        check(sender.acknowledge(1, 65536) == Result::sequence, "ACK of unsent bytes");
        check(sender.commit(1) == Result::inactive, "fatal ACK is sticky");
        sender.reset(); sender.commit(3); sender.acknowledge(3, 65536);
        check(sender.acknowledge(2, 65536) == Result::sequence, "ACK regression");
        sender.reset(); sender.commit(3); sender.acknowledge(3, 65539);
        check(sender.acknowledge(3, 65538) == Result::capacity, "credit regression");
        sender.reset();
        check(sender.acknowledge(0, 65537) == Result::capacity, "invented credit");
        sender.reset();
        check(sender.acknowledge(0, UINT64_MAX) == Result::capacity, "huge credit rejected without overflow");
    } else if (test == "call") {
        ServiceCall call;
        const std::array<uint8_t, 3> bytes{1, 2, 3};
        check(call.sent(1) == Result::inactive, "no data before open");
        check(call.beginOpen(0) == Result::invalid, "request nonzero");
        check(call.beginOpen(2) == Result::ok && call.beginOpen(3) == Result::inactive, "one outstanding open");
        check(call.opened(2, session) == Result::ok && call.state() == ServiceCall::State::service, "open reply matches");
        check(call.receive(session, 0, bytes) == Result::ok && call.sent(3) == Result::ok, "bidirectional call data");
        check(call.consume(1) == Result::ok && call.receiveLimit() == 65537, "guest credit");
        check(call.beginClose(4) == Result::ok && call.peek().empty(), "hangup discards data");
        check(call.receive(session, 3, bytes) == Result::inactive, "closing blocks data");
        check(call.closed(4, session) == Result::ok && call.call().empty(), "close completes");
        check(call.beginOpen(5) == Result::ok && call.opened(5, "ffffffffffffffffffffffffffffffff") == Result::ok, "new call");
        check(call.sendOffset() == 0 && call.receiveOffset() == 0, "no old offsets");
        check(call.receive(session, 0, bytes) == Result::invalid && call.state() == ServiceCall::State::error, "old call rejected");
        check(call.peek().empty() && call.call().empty(), "fault discards call state");
    } else if (test == "transfer") {
        SendWindow sender;
        ReceiveWindow receiver;
        constexpr size_t total = 300000;
        size_t produced = 0, consumed = 0;
        std::array<uint8_t, 4096> chunk{};
        // Deliberately unequal producer/consumer sizes exercise full buffers,
        // backpressure, repeated ring wrapping and partial guest consumption.
        for (size_t iteration = 0; consumed < total && iteration < total; ++iteration) {
            if (produced < total) {
                const size_t count = total - produced < chunk.size() ? total - produced : chunk.size();
                for (size_t i = 0; i < count; ++i) chunk[i] = static_cast<uint8_t>((produced + i) % 251);
                const auto offset = sender.nextOffset();
                const auto result = sender.commit(count);
                if (result == Result::ok) {
                    check(receiver.accept(offset, std::span(chunk).first(count)) == Result::ok, "receiver accepts reserved credit");
                    produced += count;
                } else check(result == Result::capacity, "only expected backpressure");
            }
            const auto view = receiver.peek();
            const size_t count = view.size() < 997 ? view.size() : 997;
            for (size_t i = 0; i < count; ++i)
                check(view[i] == static_cast<uint8_t>((consumed + i) % 251), "byte order and content");
            check(receiver.consume(count) == Result::ok, "partial consume");
            consumed += count;
            check(sender.acknowledge(receiver.nextOffset(), receiver.limit()) == Result::ok, "receiver credit valid");
        }
        check(produced == total && consumed == total && receiver.pending() == 0, "bounded transfer completes");
        check(sender.acknowledged() == total, "all data acknowledged");
    } else if (test == "call_errors") {
        ServiceCall call;
        call.beginOpen(1);
        check(call.opened(2, session) == Result::invalid, "wrong reply");
        check(call.beginOpen(3) == Result::inactive, "no implicit recovery");
        call.disconnect(); call.beginOpen(3);
        check(call.opened(3, "bad") == Result::invalid, "bad token");
        call.disconnect(); call.beginOpen(4); call.opened(4, session);
        check(call.acknowledged(session, 1, 65536) == Result::sequence && call.state() == ServiceCall::State::error, "ACK fault ends call");
        call.disconnect(); call.beginOpen(5); call.opened(5, session); call.beginClose(6);
        check(call.closed(7, session) == Result::invalid, "wrong close reply");
        call.disconnect(); call.beginOpen(8); call.abort();
        check(call.state() == ServiceCall::State::error && call.call().empty(), "timeout cleanup");
    } else throw std::runtime_error("unknown test");
}
int main(int argc, char **argv) {
    try { if (argc != 2) return 2; run(argv[1]); std::cout << "PASS " << argv[1] << '\n'; return 0; }
    catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
