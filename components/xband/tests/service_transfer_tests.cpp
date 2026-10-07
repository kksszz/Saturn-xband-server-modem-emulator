#include <xband/service_transfer.hpp>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace xband;
using namespace xband::protocol;
using Json = nlohmann::json;
using O = ServiceTransfer::Outcome;
constexpr std::string_view id = "0123456789abcdef0123456789abcdef";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
class Echo : public ServiceEndpoint {
public:
    std::deque<uint8_t> output; unsigned ticks = 0, input = 0, frame = 0; size_t generate = 0;
    bool transmit(uint8_t byte, unsigned f) override { ++input; frame = f; output.push_back(byte); return true; }
    void tick(unsigned f) override { ++ticks; frame = f; for (size_t i = 0; i < generate; ++i) output.push_back(static_cast<uint8_t>(i % 251)); }
    bool peek(uint8_t &byte) const override { if (output.empty()) return false; byte = output.front(); return true; }
    void consume() override { output.pop_front(); }
    size_t pending() const override { return output.size(); }
    void reset() override { output.clear(); }
};
uint64_t activate(ConnectionLifecycle &c) {
    auto g = c.begin(0); c.hello(g, "a", 0); check(c.authenticationCompleted(g, true, id, 0), "approval"); return g;
}
void request(ServerInbox &inbox, uint64_t number, std::string type, Json body) {
    auto text = Json{{"v", 2}, {"type", type}, {"endpoint", "a"}, {"session", id}, {"id", std::to_string(number)},
        {"reply_to", nullptr}, {"body", body}}.dump();
    std::vector<uint8_t> bytes(text.size() + 4);
    encodeFrame(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()), bytes);
    check(inbox.feed(bytes, number).status == ServerInbox::Status::ready, "request validated");
}
std::vector<Json> drain(FrameQueue &queue) {
    std::vector<Json> messages; MessageInbox inbox;
    while (queue.pendingBytes()) {
        auto r = inbox.feed(queue.peek()); check(r.consumed != 0 && r.status != MessageInbox::Status::failed, "reply framing");
        queue.consume(r.consumed);
        if (inbox.peek()) { messages.push_back(*inbox.peek()); inbox.consume(); }
    }
    return messages;
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1]; ConnectionLifecycle connection; auto generation = activate(connection);
        ServerInbox inbox(connection, generation); FrameQueue queue;
        SessionOutbox outbox(queue, connection, generation, "a", std::string(id)); Echo echo;
        ServiceTransfer transfer(inbox, outbox, connection, generation, echo, FrameClock(60, 1000), std::string(id));
        Json data{{"call", id}, {"offset", "0"}, {"bytes", 3}, {"payload_b64", "QUJD"}};
        if (name == "transfer_order") {
            request(inbox, 2, "data", data); check(transfer.process() == O::handled && echo.input == 0, "data stages before advance");
            auto replies = drain(queue); check(replies.size() == 1 && replies[0]["type"] == "data_ack" && replies[0]["body"]["next_offset"] == "3", "receipt ACK");
            request(inbox, 3, "advance", {{"call", id}, {"tick", "1000"}}); check(transfer.process() == O::handled, "advance accepted");
            check(transfer.drive() == O::handled && echo.input == 3 && echo.ticks == 1 && echo.frame == 60, "service delivered at frame");
            replies = drain(queue);
            check(replies.size() == 3 && replies[0]["type"] == "data" && replies[0]["body"]["payload_b64"] == "QUJD" &&
                replies[1]["type"] == "data_ack" && replies[1]["body"]["limit"] == "65539" && replies[2]["type"] == "advance_ok" &&
                replies[2]["reply_to"] == "3", "data and consumption ACK precede completion");
            check(transfer.drive() == O::idle && echo.ticks == 1, "no replay");
        } else if (name == "transfer_ack_retry") {
            const std::array<uint8_t, 1> byte{0};
            for (size_t i = 0; i < FrameQueue::message_capacity; ++i) queue.enqueue(byte);
            request(inbox, 2, "data", data);
            check(transfer.process() == O::blocked && transfer.process() == O::blocked && echo.input == 0, "ACK blocked no duplicate admission");
            queue.reset(); check(transfer.process() == O::handled && !inbox.peek(), "retry ACK");
            drain(queue); request(inbox, 3, "advance", {{"call", id}, {"tick", "0"}}); transfer.process();
            check(transfer.drive() == O::handled && echo.input == 3 && echo.ticks == 1, "only three bytes delivered");
        } else if (name == "transfer_credit") {
            echo.generate = 70000;
            request(inbox, 2, "advance", {{"call", id}, {"tick", "0"}}); transfer.process();
            size_t delivered = 0; bool completion = false; O result = O::progress;
            auto inspect = [&] {
                for (const auto &m : drain(queue)) {
                    if (m["type"] == "advance_ok") completion = true;
                    if (m["type"] != "data") continue;
                    check(m["body"]["offset"] == std::to_string(delivered), "output offsets");
                    std::array<uint8_t, 4096> bytes{}; size_t size = 0;
                    check(decodeBase64(m["body"]["payload_b64"].get_ref<const std::string &>(), bytes, size), "output decode");
                    for (size_t i = 0; i < size; ++i) check(bytes[i] == static_cast<uint8_t>((delivered + i) % 251), "exact generated data");
                    delivered += size;
                }
            };
            for (unsigned i = 0; i < 40 && delivered < 65536; ++i) { result = transfer.drive(); inspect(); }
            check(delivered == 65536 && !completion && echo.ticks == 1, "credit boundary before completion");
            result = transfer.drive(); check(result == O::blocked && transfer.bufferedOutput() <= 4096, "bounded pending output");
            request(inbox, 3, "data_ack", {{"call", id}, {"next_offset", "65536"}, {"limit", "65537"}});
            check(transfer.process() == O::handled, "ACK processed during pending advance");
            transfer.drive(); inspect(); check(delivered == 65537 && !completion, "one-byte credit resumes partially");
            request(inbox, 4, "data_ack", {{"call", id}, {"next_offset", "65537"}, {"limit", "131073"}}); transfer.process();
            for (unsigned i = 0; i < 20 && transfer.advancing(); ++i) { transfer.drive(); inspect(); }
            check(delivered == 70000 && completion && echo.ticks == 1 && !transfer.advancing(), "full transfer without timer replay");
        } else if (name == "transfer_reply_retry") {
            request(inbox, 2, "data", data); transfer.process(); drain(queue);
            request(inbox, 3, "advance", {{"call", id}, {"tick", "0"}}); transfer.process();
            const std::array<uint8_t, 1> byte{0};
            for (size_t i = 0; i < FrameQueue::message_capacity - 1; ++i) queue.enqueue(byte);
            check(transfer.drive() == O::blocked && echo.ticks == 1, "data queued but completion blocked");
            check(transfer.drive() == O::blocked && echo.ticks == 1, "retry does not rerun timer");
            check(queue.consume((FrameQueue::message_capacity - 1) * 5), "simulate draining fixture frames");
            auto replies = drain(queue);
            check(replies.size() == 1 && replies[0]["type"] == "data", "only one data reply");
            check(transfer.drive() == O::handled && echo.ticks == 1 && echo.input == 3, "finish without replay");
            replies = drain(queue);
            check(replies.size() == 2 && replies[0]["type"] == "data_ack" && replies[1]["type"] == "advance_ok", "completion after data");
        } else if (name == "base64_encoding") {
            std::string encoded = "unchanged";
            check(!encodeBase64({}, encoded) && encoded == "unchanged", "empty rejected");
            check(!encodeBase64(std::vector<uint8_t>(4097), encoded) && encoded == "unchanged", "oversize rejected");
            for (const auto &example : std::vector<std::pair<std::vector<uint8_t>, std::string>>{
                {{65}, "QQ=="}, {{65, 66}, "QUI="}, {{65, 66, 67}, "QUJD"}, {{255, 238}, "/+4="}}) {
                check(encodeBase64(example.first, encoded) && encoded == example.second, "literal canonical encoding");
            }
            for (size_t length = 1; length <= 4096; ++length) {
                std::vector<uint8_t> source(length), decoded(length);
                for (size_t i = 0; i < length; ++i) source[i] = static_cast<uint8_t>(i % 251);
                size_t written = 0;
                check(encodeBase64(source, encoded) && decodeBase64(encoded, decoded, written) && source == decoded && written == length, "every chunk length roundtrip");
            }
        } else if (name == "transfer_reject") {
            data["call"] = "ffffffffffffffffffffffffffffffff";
            request(inbox, 2, "data", data);
            check(transfer.process() == O::failed && connection.state() == ConnectionLifecycle::State::closed && queue.pendingBytes() == 0 && echo.input == 0, "wrong call fails closed");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
