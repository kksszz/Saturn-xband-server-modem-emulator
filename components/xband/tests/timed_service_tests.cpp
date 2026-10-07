#include <xband/timed_service.hpp>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>
using namespace xband;
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
class Probe : public ServiceEndpoint {
public:
    std::vector<unsigned> frames; std::vector<int> events; std::deque<uint8_t> output;
    bool block = false, throwing = false;
    bool transmit(uint8_t byte, unsigned frame) override {
        if (block) return false;
        frames.push_back(frame); events.push_back(byte); output.push_back(byte); return true;
    }
    void tick(unsigned frame) override {
        if (throwing) throw std::runtime_error("timer failure");
        frames.push_back(frame); events.push_back(-1);
    }
    bool peek(uint8_t &byte) const override { if (output.empty()) return false; byte = output.front(); return true; }
    void consume() override { output.pop_front(); }
    size_t pending() const override { return output.size(); }
    void reset() override { output.clear(); events.clear(); frames.clear(); }
};
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1];
        if (name == "clock_mapping") {
            unsigned frame = 77; FrameClock c(60, 1000);
            check(c.map(16, frame) && frame == 0 && c.map(17, frame) && frame == 1, "explicit floor mapping");
            check(c.map(3000, frame) && frame == 180, "180 frames is not 180 milliseconds");
            FrameClock fractional(60000, 28636360ULL * 1001);
            check(fractional.map(28636360ULL * 1001, frame) && frame == 60000, "fractional rate rational");
            frame = 77; check(!FrameClock(0, 1).map(1, frame) && frame == 77, "invalid ratio unchanged");
            check(!FrameClock(1, 0).map(1, frame), "zero denominator");
            check(FrameClock(1, 1).map(std::numeric_limits<unsigned>::max(), frame), "maximum legacy frame");
            check(!FrameClock(1, 1).map(UINT64_MAX, frame), "no legacy frame wrap");
        } else {
            Probe service; TimedService pump(service, FrameClock(60, 1000));
            const std::array<uint8_t, 3> bytes{1, 2, 3}; std::vector<uint8_t> delivered;
            auto sink = [&](uint8_t b) { delivered.push_back(b); return true; };
            check(pump.accept(0, bytes) == protocol::Result::ok && service.events.empty(), "input staged without service side effects");
            if (name == "timed_backpressure") {
                check(pump.beginAdvance(1000), "begin virtual advance"); service.block = true;
                check(pump.resume(sink) == TimedService::Step::blocked && pump.pendingInput() == 3, "unaccepted input retained");
                check(!pump.beginAdvance(2000), "only one advance at a time");
                check(pump.accept(3, bytes) == protocol::Result::capacity, "no new input during advance"); service.block = false;
                check(pump.resume([](uint8_t) { return false; }) == TimedService::Step::blocked, "output backpressure");
                check(service.events == std::vector<int>({1, 2, 3, -1}) && service.pending() == 3, "input before timer, output retained");
                check(pump.resume(sink) == TimedService::Step::done && delivered == std::vector<uint8_t>({1, 2, 3}), "resume no replay");
                check(service.events.size() == 4 && pump.completedTick() == 1000 && pump.receiveLimit() == 65539, "one timer and restored credit");
                check(pump.resume(sink) == TimedService::Step::idle, "completed advance not repeated");
            } else if (name == "timed_budget") {
                check(pump.beginAdvance(1000), "begin");
                check(pump.resume(sink, 0) == TimedService::Step::progress && service.events.empty(), "zero work budget");
                TimedService::Step result = TimedService::Step::progress;
                for (unsigned i = 0; i < 20 && result != TimedService::Step::done; ++i) result = pump.resume(sink, 1);
                check(result == TimedService::Step::done && service.events.size() == 4 && delivered.size() == 3, "budgeted progress");
                for (auto frame : service.frames) check(frame == 60, "common emulated frame");
                check(pump.beginAdvance(1000) && pump.resume(sink) == TimedService::Step::done, "equal tick allowed for new request");
                check(!pump.beginAdvance(999) && pump.failed(), "backward time fatal");
            } else if (name == "timed_failure") {
                service.throwing = true; pump.beginAdvance(1000);
                check(pump.resume(sink) == TimedService::Step::failed && delivered.empty(), "timer exception fatal");
                check(pump.resume(sink) == TimedService::Step::failed && service.events.size() == 3, "no replay after uncertain failure");
                check(!pump.beginAdvance(1000), "cannot reuse failed pump");
            } else throw std::runtime_error("unknown test");
        }
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
