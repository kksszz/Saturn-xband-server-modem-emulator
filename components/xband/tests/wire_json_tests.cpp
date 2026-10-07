#include <xband/wire_json.hpp>
#include <iostream>
#include <stdexcept>
#include <chrono>
using namespace xband::protocol;
using Json = nlohmann::json;
void check(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
Json message(std::string type = "ping") {
    return {{"v", 2}, {"type", type}, {"endpoint", "saturn-1"}, {"session", "0123456789abcdef0123456789abcdef"},
        {"id", "1"}, {"reply_to", nullptr}, {"body", Json::object()}};
}
int main(int argc,char **argv) {
    try {
        if(argc==2&&std::string_view(argv[1])=="--benchmark"){
            auto m=message("advance");m["body"]={{"call",m["session"]},{"tick","123456"}};
            const auto input=m.dump();const auto start=std::chrono::steady_clock::now();
            for(unsigned i=0;i<30000;++i)check(bool(parseWire(input)),"benchmark parse");
            std::cout<<"WIRE_BENCH messages=30000 elapsed_us="<<std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count()<<'\n';
            return 0;
        }
        check(bool(parseWire(message().dump())), "valid ping");
        for (auto input : {"{}{}", "[]", "null", "{\"a\":1,}", "{/*comment*/}", "{\"a\":NaN}"})
            check(!parseWire(input), "malformed or non-envelope");
        check(parseWire("{\"a\":1,\"\\u0061\":2}").error == WireError::duplicate_key, "escaped duplicate");
        check(parseWire("{\"a\":{\"x\":1,\"x\":2}}").error == WireError::duplicate_key, "nested duplicate");
        std::string many="{";
        for(unsigned i=0;i<12;++i)many+="\"k"+std::to_string(i)+"\":0,";
        check(parseWire(many+"\"k0\":1}").error==WireError::duplicate_key,"overflow duplicate of inline key");
        check(parseWire(many+"\"k11\":1}").error==WireError::duplicate_key,"overflow duplicate of hashed key");
        check(parseWire(many+"\"unique\":1}").error==WireError::envelope,"large unique object parsed");
        check(parseWire("{\"a\":{\"x\":1},\"b\":{\"x\":2}}").error==WireError::envelope,"sibling object keys independent");
        std::string nested="0";
        for(unsigned i=0;i<8;++i)nested="{\"x\":"+nested+"}";
        check(parseWire(nested).error==WireError::envelope,"eight object levels parsed");
        check(parseWire("{\"x\":"+nested+"}").error==WireError::depth,"ninth object level rejected");
        check(parseWire(std::string(9, '[') + "0" + std::string(9, ']')).error == WireError::depth, "depth bounded");
        check(parseWire(std::string(8, '[') + "0" + std::string(8, ']')).error == WireError::envelope, "depth eight parsed");
        check(parseWire(std::string(65537, ' ')).error == WireError::size, "size bounded");
        check(parseWire(std::string("\xef\xbb\xbf") + message().dump()).error == WireError::syntax, "BOM rejected");
        check(parseWire("{\"x\":\"\xc0\xaf\"}").error == WireError::syntax, "invalid UTF8");
        check(parseWire("{\"x\":\"\\ud800\"}").error == WireError::syntax, "unpaired surrogate");
        auto m = message(); m["extra"] = 1; check(!parseWire(m.dump()), "unknown envelope key");
        m = message(); m["v"] = 2.0; check(!parseWire(m.dump()), "integer version only");
        m = message(); m["id"] = "01"; check(!parseWire(m.dump()), "canonical ID");
        m = message(); m["id"] = 1; check(!parseWire(m.dump()), "string ID");
        m = message(); m["body"]["extra"] = 1; check(!parseWire(m.dump()), "empty ping body");
        m = message("pong"); check(!parseWire(m.dump()), "response needs reply");
        m["reply_to"] = "1"; check(bool(parseWire(m.dump())), "valid pong");
        m = message("open"); m["body"] = {{"target", "local-service"}, {"subscriber", "001234"}};
        check(bool(parseWire(m.dump())), "phone preserves leading zeros");
        m["body"]["subscriber"] = "+123"; check(!parseWire(m.dump()), "phone digits only");
        for (auto type : {"open_ok", "close_ok", "advance_ok", "advance", "close", "data_ack"}) {
            m = message(type); m["body"]["call"] = m["session"];
            const std::string t = type;
            if (t.ends_with("_ok")) m["reply_to"] = "1";
            if (t.starts_with("advance")) m["body"]["tick"] = "0";
            if (t == "close") m["body"]["reason"] = "hangup";
            if (t == "data_ack") m["body"].update({{"next_offset", "0"}, {"limit", "65536"}});
            check(bool(parseWire(m.dump())), "valid control message");
            m["body"]["extra"] = 0; check(!parseWire(m.dump()), "unknown body key");
        }
        m = message("hello"); m["session"] = "";
        m["body"] = {{"client", "test"}, {"clock_hz", 1000000}, {"auth_key", std::string(64, 'a')}};
        check(bool(parseWire(m.dump())), "hello syntax only");
        m["body"]["clock_hz"] = -1; check(!parseWire(m.dump()), "negative clock");
        m = message("hello_ok"); m["body"] = {{"new_session", m["session"]}, {"max_chunk", 4096}, {"rx_window", 65536}};
        m["session"] = ""; m["reply_to"] = "1"; check(bool(parseWire(m.dump())), "hello response");
        for (auto type : {"unknown"})
            check(parseWire(message(type).dump()).error == WireError::unsupported, "fail closed for unsupported types");
        m = message("data");
        m["body"] = {{"call", m["session"]}, {"offset", "0"}, {"bytes", 3}, {"payload_b64", "QUJD"}};
        check(bool(parseWire(m.dump())), "data ABC");
        m["body"]["bytes"] = 2; check(!parseWire(m.dump()), "decoded length mismatch");
        m["body"]["bytes"] = 1; m["body"]["payload_b64"] = "QR==";
        check(!parseWire(m.dump()), "noncanonical padding bits");
        m["body"]["payload_b64"] = "QQ=="; check(bool(parseWire(m.dump())), "one byte");
        m["body"]["bytes"] = 4096; m["body"]["payload_b64"] = std::string(5462, 'A') + "==";
        check(bool(parseWire(m.dump())), "maximum chunk");
        m["body"]["bytes"] = 4097; check(!parseWire(m.dump()), "oversized chunk");
        size_t decoded = 7;
        for (auto bad : {"", "QQ", "QQ=", "=AAA", "A===", "AA=A", "QR==", "QUJ=", "QQ==\n", "__==", "--=="})
            check(!base64Size(bad, decoded) && decoded == 7, "bad Base64 leaves size untouched");
        std::array<uint8_t, 3> output{9, 9, 9}; size_t written = 7;
        check(!decodeBase64("QUJD", std::span(output).first(2), written) && output[0] == 9 && written == 7, "short buffer untouched");
        check(!decodeBase64("QUJ=", output, written) && output[0] == 9, "invalid input untouched");
        check(decodeBase64("QUJD", output, written) && output == std::array<uint8_t, 3>{65, 66, 67} && written == 3, "literal decoding");
        check(decodeBase64("/w==", output, written) && output[0] == 255 && written == 1, "binary byte");
        check(decodeBase64("/+4=", output, written) && output[0] == 255 && output[1] == 238 && written == 2, "two bytes");
        m = message("snapshot");
        m["body"] = {{"call", nullptr}, {"subscriber", nullptr}, {"state", "idle"}, {"sent_bytes", "0"}, {"received_bytes", "0"},
            {"card", {{"inserted", false}, {"read_state", "absent"}, {"number", nullptr}, {"remaining_units", nullptr}, {"nominal_units", nullptr}}}};
        check(bool(parseWire(m.dump())), "absent card");
        auto snapshot = m;
        m["body"]["card"]["remaining_units"] = 10; check(!parseWire(m.dump()), "absent card has no balance");
        m = snapshot; m["body"]["sent_bytes"] = "1"; check(!parseWire(m.dump()), "no call counters");
        m = snapshot; m["body"]["card"]["inserted"] = true; check(!parseWire(m.dump()), "inserted is not absent");
        m["body"]["card"].update({{"read_state", "readable"}, {"number", "960500132270"}, {"remaining_units", 7}, {"nominal_units", 10}});
        check(bool(parseWire(m.dump())), "readable card");
        m["body"]["card"]["remaining_units"] = 11; check(!parseWire(m.dump()), "balance cannot exceed nominal");
        m = snapshot; m["body"]["card"].update({{"inserted", true}, {"read_state", "unreadable"}});
        check(bool(parseWire(m.dump())), "unreadable card with unknown fields");
        m["body"]["card"]["extra"] = false; check(!parseWire(m.dump()), "card unknown field");
        m = message("error");
        m["body"] = {{"code", "INTERNAL_ERROR"}, {"message", "Failed"}, {"scope", "connection"}, {"call", nullptr}};
        check(bool(parseWire(m.dump())), "unsolicited error");
        m["reply_to"] = "1"; check(bool(parseWire(m.dump())), "correlated error");
        m["body"]["scope"] = "call"; check(!parseWire(m.dump()), "call error needs token");
        m["body"]["call"] = m["session"]; check(bool(parseWire(m.dump())), "call error");
        m["session"] = ""; check(!parseWire(m.dump()), "preauth cannot address call");
        m["body"].update({{"scope", "connection"}, {"call", nullptr}, {"code", "AUTH_FAILED"}});
        check(bool(parseWire(m.dump())), "pre-session auth failure");
        m["body"]["code"] = "UNKNOWN"; check(!parseWire(m.dump()), "unknown error code");
        std::cout << "PASS wire JSON validation\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
