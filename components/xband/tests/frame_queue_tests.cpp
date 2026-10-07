#include <xband/frame_queue.hpp>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
using Q = xband::FrameQueue;
using Bytes = std::vector<uint8_t>;
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
Bytes framed(const Bytes &payload) {
    Bytes out(payload.size() + 4);
    check(xband::encodeFrame(payload, out).status == xband::EncodeStatus::ok, "encode fixture"); return out;
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view test = argv[1]; Q q;
        if (test == "queue_partial") {
            const Bytes payload{65, 66, 67}; const auto expected = framed(payload);
            check(q.enqueue(payload) == Q::Result::ok, "enqueue");
            Bytes output;
            while (!q.peek().empty()) {
                check(q.pendingMessages() == 1, "partial message remains counted");
                output.push_back(q.peek()[0]); check(q.consume(1), "one-byte socket write");
            }
            check(output == expected && q.pendingMessages() == 0, "no repeated bytes");
            check(q.consume(0) && !q.consume(1), "empty consume bounds");
        } else if (test == "queue_capacity") {
            Bytes maximum(65536, 42);
            check(q.enqueue(maximum) == Q::Result::ok && q.enqueue(maximum) == Q::Result::ok, "byte cap");
            check(q.enqueue(Bytes{1}) == Q::Result::full && q.pendingBytes() == Q::byte_capacity, "no partial admission");
            check(!q.consume(q.peek().size() + 1) && q.pendingBytes() == Q::byte_capacity, "bad completion unchanged");
            q.reset();
            for (size_t i = 0; i < Q::message_capacity; ++i) check(q.enqueue(Bytes{1}) == Q::Result::ok, "message cap fill");
            check(q.enqueue(Bytes{2}) == Q::Result::full, "message count bounded");
            check(q.consume(4) && q.enqueue(Bytes{2}) == Q::Result::full, "header only does not release message slot");
            check(q.consume(1) && q.enqueue(Bytes{2}) == Q::Result::ok, "slot released on complete message");
        } else if (test == "queue_reset") {
            check(q.enqueue({}) == Q::Result::invalid && q.enqueue(Bytes(65537)) == Q::Result::invalid, "invalid payload");
            q.enqueue(Bytes{1, 2, 3}); q.consume(2); q.reset();
            check(q.peek().empty() && q.pendingMessages() == 0, "discard partial old frame");
            q.enqueue(Bytes{9}); const auto expected = framed(Bytes{9});
            check(std::equal(q.peek().begin(), q.peek().end(), expected.begin(), expected.end()), "fresh stream");
            Q independent; check(independent.pendingBytes() == 0, "independent queue");
        } else if (test == "queue_stream") {
            // Multiple wraps, coalescing, split headers/payloads, and producer
            // backpressure. Feed emitted bytes through the real frame decoder.
            xband::FrameDecoder decoder;
            size_t sent = 0, received = 0;
            constexpr size_t total = 400;
            for (size_t iteration = 0; iteration < 100000 && received < total; ++iteration) {
                for (unsigned attempt = 0; attempt < 10 && sent < total; ++attempt) {
                    Bytes payload(4096, static_cast<uint8_t>(sent % 251));
                    const auto result = q.enqueue(payload);
                    if (result == Q::Result::full) break;
                    check(result == Q::Result::ok, "stream enqueue"); ++sent;
                }
                const auto view = q.peek();
                const size_t count = std::min(view.size(), size_t{997});
                size_t cursor = 0;
                while (cursor < count) {
                    const auto result = decoder.feed(view.subspan(cursor, count - cursor));
                    check(result.consumed != 0, "decoder progress"); cursor += result.consumed;
                    if (result.status == xband::FrameDecoder::Status::ready) {
                        const auto data = decoder.peek();
                        check(data.size() == 4096 && std::all_of(data.begin(), data.end(), [&](uint8_t b) {
                            return b == static_cast<uint8_t>(received % 251);
                        }), "exact ordered frame content");
                        ++received; decoder.consume();
                    }
                }
                check(q.consume(count), "socket accepted chunk");
            }
            check(sent == total && received == total && q.pendingBytes() == 0 && q.pendingMessages() == 0, "complete stream");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << test << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
