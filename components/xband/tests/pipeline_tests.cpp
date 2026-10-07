#include <xband/message_pipeline.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace xband;
using namespace xband::protocol;
using Json = nlohmann::json;
using Bytes = std::vector<uint8_t>;
constexpr std::string_view token_value = "0123456789abcdef0123456789abcdef";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
std::string data(uint64_t offset = 0) {
    return Json{{"v", 2}, {"type", "data"}, {"endpoint", "saturn-1"}, {"session", token_value},
        {"id", "2"}, {"reply_to", nullptr}, {"body", {{"call", token_value}, {"offset", std::to_string(offset)},
        {"bytes", 3}, {"payload_b64", "QUJD"}}}}.dump();
}
Bytes wire(std::string_view text) {
    Bytes out(text.size() + 4);
    check(encodeFrame(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()), out).status == EncodeStatus::ok, "fixture");
    return out;
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        std::string_view test = argv[1];
        if (test == "inbox") {
            const auto frame = wire(data());
            for (size_t split = 0; split <= frame.size(); ++split) {
                MessageInbox inbox;
                check(inbox.feed(std::span(frame).first(split)).consumed == split, "split prefix");
                check(inbox.feed(std::span(frame).subspan(split)).status == MessageInbox::Status::ready, "split suffix");
                check(inbox.peek() && (*inbox.peek())["type"] == "data", "validated payload");
            }
            MessageInbox inbox; auto combined = frame; combined.insert(combined.end(), frame.begin(), frame.end());
            auto result = inbox.feed(combined);
            check(result.consumed == frame.size(), "one frame held");
            check(inbox.feed(frame).consumed == 0 && inbox.peek(), "downstream backpressure retains message");
            check(inbox.consume() && !inbox.consume(), "consume once");
            check(inbox.feed(std::span(combined).subspan(result.consumed)).status == MessageInbox::Status::ready, "remaining coalesced frame");
        } else if (test == "inbox_errors") {
            MessageInbox inbox;
            check(inbox.feed(wire("{}")).status == MessageInbox::Status::failed && !inbox.peek(), "schema failure");
            check(inbox.feed(wire(data())).consumed == 0 && !inbox.consume(), "sticky failure");
            inbox.reset(); const auto frame = wire(data());
            inbox.feed(std::span(frame).first(3)); inbox.reset();
            check(inbox.feed(frame).status == MessageInbox::Status::ready, "new session has no fragments");
            inbox.reset(); check(inbox.feed(Bytes{0, 0, 0, 0}).status == MessageInbox::Status::failed, "bad frame length");
        } else if (test == "admission") {
            FrameQueue q; SendWindow w;
            for (size_t i = 0; i < FrameQueue::message_capacity; ++i) q.enqueue(Bytes{1});
            check(enqueueData(q, w, token_value, data()) == Admission::queue_full && w.nextOffset() == 0, "full queue preserves credit");
            q.reset();
            check(enqueueData(q, w, token_value, data()) == Admission::queued && w.nextOffset() == 3, "admitted once");
            const auto pending = q.pendingBytes();
            check(enqueueData(q, w, token_value, data()) == Admission::invalid && q.pendingBytes() == pending, "stale offset no duplicate");
            check(enqueueData(q, w, "ffffffffffffffffffffffffffffffff", data(3)) == Admission::invalid && w.nextOffset() == 3, "wrong call");
            w.reset(); q.reset();
            for (unsigned i = 0; i < 16; ++i) w.commit(4096);
            check(enqueueData(q, w, token_value, data(65536)) == Admission::credit_full && q.pendingBytes() == 0, "full credit preserves queue");
        } else if (test == "pipeline") {
            FrameQueue q; SendWindow sender; MessageInbox inbox; ReceiveWindow receiver;
            check(enqueueData(q, sender, token_value, data()) == Admission::queued, "data admitted");
            while (q.pendingBytes() != 0) {
                const auto result = inbox.feed(q.peek().first(1));
                check(result.consumed == 1 && q.consume(1), "one-byte transport simulation");
            }
            check(sender.nextOffset() == 3 && inbox.peek(), "one accounting commit regardless of partial writes");
            const auto &b = (*inbox.peek())["body"];
            std::array<uint8_t, 4096> bytes{}; size_t size = 0;
            check(decodeBase64(b["payload_b64"].get_ref<const std::string &>(), bytes, size), "payload decode");
            check(receiver.accept(0, std::span(bytes).first(size)) == Result::ok, "receiver admits bytes");
            inbox.consume();
            check(sender.acknowledge(receiver.nextOffset(), receiver.limit()) == Result::ok, "ACK accepted");
            check(receiver.peek().size() == 3 && receiver.peek()[0] == 65 && receiver.peek()[2] == 67, "exact delivered data");
            check(receiver.consume(3) == Result::ok && sender.acknowledge(receiver.nextOffset(), receiver.limit()) == Result::ok, "consumption releases credit");
            q.reset(); sender.reset(); inbox.reset(); receiver.reset();
            check(sender.nextOffset() == 0 && !inbox.peek(), "disconnect clears all layers");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << test << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
