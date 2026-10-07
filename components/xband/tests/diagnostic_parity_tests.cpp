#include <xband/service_control.hpp>
#include <xband/service_transfer.hpp>
#include <local_service_endpoint.hpp>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace xband::protocol;
using Bytes = std::vector<uint8_t>;
using Json = nlohmann::json;
constexpr std::string_view token_value = "0123456789abcdef0123456789abcdef";
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct Harness {
    LocalPPPProbe reference, routed;
    LocalServiceEndpoint service{routed};
    ConnectionLifecycle connection;
    uint64_t generation = 0, next_input = 2, input_offset = 0, output_offset = 0, next_output = 2;
    xband::FrameQueue queue;
    std::unique_ptr<ServerInbox> inbox;
    std::unique_ptr<SessionOutbox> outbox;
    std::unique_ptr<ServiceControl> control;
    std::unique_ptr<ServiceTransfer> transfer;
    explicit Harness(bool ipcp = false) {
        reference.enableIPCP = routed.enableIPCP = ipcp;
        generation = connection.begin(0); connection.hello(generation, "parity", 0);
        check(connection.authenticationCompleted(generation, true, token_value, 0), "fixture approval");
        inbox = std::make_unique<ServerInbox>(connection, generation);
        outbox = std::make_unique<SessionOutbox>(queue, connection, generation, "parity", std::string(token_value));
        control = std::make_unique<ServiceControl>(*inbox, connection, generation, *outbox, service);
        request("open", {{"target", "local-service"}, {"subscriber", "0001"}});
        check(control->process(token_value) == ServiceControl::Outcome::handled, "open real diagnostic endpoint");
        const auto replies = drain(); check(replies.size() == 1 && replies[0]["type"] == "open_ok", "open reply");
        // Deliberate fixture ratio: 1000 test ticks per legacy frame, not a claim
        // about Saturn clock configuration or any particular game's video mode.
        transfer = std::make_unique<ServiceTransfer>(*inbox, *outbox, connection, generation, service,
            xband::FrameClock(1, 1000), std::string(token_value));
    }
    uint64_t request(std::string type, Json body) {
        const auto number = next_input++;
        const auto text = Json{{"v", 2}, {"type", type}, {"endpoint", "parity"}, {"session", token_value},
            {"id", std::to_string(number)}, {"reply_to", nullptr}, {"body", std::move(body)}}.dump();
        Bytes wire(text.size() + 4);
        xband::encodeFrame(std::span(reinterpret_cast<const uint8_t *>(text.data()), text.size()), wire);
        for (auto byte : wire) check(inbox->feed(std::span(&byte, 1), number).consumed == 1, "one-byte framed input");
        check(inbox->peek() != nullptr, "request ready"); return number;
    }
    std::vector<Json> drain() {
        MessageInbox decoded; std::vector<Json> messages;
        while (queue.pendingBytes()) {
            auto r = decoded.feed(queue.peek().first(1)); check(r.consumed == 1, "one-byte framed output"); queue.consume(1);
            if (decoded.peek()) {
                check((*decoded.peek())["id"] == std::to_string(next_output++), "continuous output IDs");
                messages.push_back(*decoded.peek()); decoded.consume();
            }
        }
        return messages;
    }
    void batch(const Bytes &bytes, unsigned frame, size_t budget = 4096) {
        for (auto byte : bytes) reference.feed(byte, frame);
        reference.tick(frame);
        const auto expected = reference.out; reference.out.clear();
        if (!bytes.empty()) {
            std::string encoded; check(encodeBase64(bytes, encoded), "fixture data encode");
            request("data", {{"call", token_value}, {"offset", std::to_string(input_offset)}, {"bytes", bytes.size()}, {"payload_b64", encoded}});
            input_offset += bytes.size();
            check(transfer->process() == ServiceTransfer::Outcome::handled, "data staged");
            auto ack = drain(); check(ack.size() == 1 && ack[0]["type"] == "data_ack", "receipt ACK only");
        }
        const auto request_id = request("advance", {{"call", token_value}, {"tick", std::to_string(uint64_t{frame} * 1000)}});
        check(transfer->process() == ServiceTransfer::Outcome::handled, "advance accepted");
        Bytes actual; bool completed = false;
        for (unsigned iteration = 0; iteration < 20000 && transfer->advancing(); ++iteration) {
            check(transfer->drive(budget) != ServiceTransfer::Outcome::failed, "drive diagnostic service");
            for (const auto &reply : drain()) {
                check(!completed, "no reply after advance_ok in batch");
                if (reply["type"] == "data") {
                    check(reply["body"]["offset"] == std::to_string(output_offset), "reply data offsets");
                    std::array<uint8_t, 4096> decoded{}; size_t count = 0;
                    check(decodeBase64(reply["body"]["payload_b64"].get_ref<const std::string &>(), decoded, count), "decode diagnostic reply");
                    actual.insert(actual.end(), decoded.begin(), decoded.begin() + count); output_offset += count;
                } else if (reply["type"] == "advance_ok") {
                    check(reply["reply_to"] == std::to_string(request_id), "advance correlation"); completed = true;
                } else check(reply["type"] == "data_ack", "only expected replies");
            }
        }
        check(completed && actual == expected, "byte-identical diagnostic replies");
        check(reference.valid == routed.valid && reference.invalid == routed.invalid && reference.open() == routed.open() &&
            reference.tries == routed.tries && reference.lastRequest == routed.lastRequest && reference.stopped == routed.stopped &&
            reference.ipOpen() == routed.ipOpen() && reference.ipTries == routed.ipTries && reference.ipStopped == routed.ipStopped &&
            reference.frame == routed.frame && reference.escaped == routed.escaped, "parser state parity");
        if (output_offset) {
            request("data_ack", {{"call", token_value}, {"next_offset", std::to_string(output_offset)}, {"limit", std::to_string(output_offset + 65536)}});
            check(transfer->process() == ServiceTransfer::Outcome::handled, "client consumes diagnostic output");
        }
    }
};
Bytes packet(uint16_t protocol, uint8_t code, uint8_t id, Bytes options = {}) {
    const auto length = options.size() + 4;
    Bytes bytes{0xff, 3, static_cast<uint8_t>(protocol >> 8), static_cast<uint8_t>(protocol), code, id,
        static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
    bytes.insert(bytes.end(), options.begin(), options.end()); return LocalPPPProbe::encode(bytes);
}
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1];
        if (name == "parity_lcp") {
            Harness h; h.batch(packet(0xc021, 1, 1), 0, 1);
            h.batch(packet(0xc021, 2, 0x51), 1); check(h.routed.open(), "LCP opened");
            h.batch(packet(0xc021, 5, 2), 2); check(h.routed.stopped, "termination");
            Harness rejected; rejected.batch(packet(0xc021, 1, 1, {3, 4, 0xc0, 0x23}), 0);
            check(!rejected.routed.peerAck, "authentication option rejected as before");
        } else if (name == "parity_retry") {
            Harness h; h.batch(packet(0xc021, 1, 1), 0);
            for (unsigned frame : {179u, 180u, 359u, 360u, 540u, 720u, 899u, 900u}) h.batch({}, frame, 1);
            check(h.routed.tries == 5 && h.routed.stopped, "same 180-frame retry/stop boundary");
        } else if (name == "parity_fragments") {
            auto input = packet(0xc021, 1, 0x7e, {5, 6, 0x7d, 0x7e, 0, 1});
            for (size_t split = 0; split <= input.size(); ++split) {
                Harness h; h.batch(Bytes(input.begin(), input.begin() + split), 0, 1);
                h.batch(Bytes(input.begin() + split, input.end()), 0, 1);
            }
            Harness invalid; invalid.batch(Bytes{0x7e, 0xff, 0x55, 0x66, 0x77, 0x88, 0x99, 0x7e}, 0);
            check(invalid.routed.invalid != 0, "bad FCS rejected");
        } else if (name == "parity_ipcp") {
            Harness h(true); h.batch(packet(0xc021, 1, 1), 0); h.batch(packet(0xc021, 2, 0x51), 1);
            h.batch(packet(0x8021, 1, 2, {3, 6, 0, 0, 0, 0}), 2);
            h.batch(packet(0x8021, 1, 3, {3, 6, 10, 0, 0, 2}), 3);
            h.batch(packet(0x8021, 2, 0x61, {3, 6, 10, 0, 0, 1}), 4);
            check(h.routed.ipOpen(), "IPCP negotiation parity");
        } else throw std::runtime_error("unknown test");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
