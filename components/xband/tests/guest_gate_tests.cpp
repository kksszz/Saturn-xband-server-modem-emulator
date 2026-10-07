#include <xband/guest_execution_gate.hpp>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>
using xband::GuestExecutionGate;
void check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view name = argv[1];
        if (name == "guest_gate_ratios") {
            for (const auto ratio : {std::pair<uint64_t,uint64_t>{39375000ULL*120,176}, {39375000ULL*8,11}, {28437500ULL*15,16}, {28437500,1}}) {
                GuestExecutionGate gate(ratio.first, ratio.second);
                check(gate.budget(100, 1, ratio.first, ratio.second) == 0, "initial held");
                uint64_t elapsed = 0;
                for (unsigned i = 0; i < 1000; ++i) {
                    check(gate.release(), "release"); elapsed += GuestExecutionGate::quantum;
                    check(gate.budget(100+elapsed, 1, ratio.first, ratio.second) == 0 && !gate.failed(), "boundary");
                    // Test range keeps this independent reference multiplication safe.
                    check(gate.tick() == elapsed * 1000000000ULL * ratio.second / ratio.first, "exact rational tick without accumulated rounding");
                }
            }
        } else if (name == "guest_gate_hold") {
            GuestExecutionGate gate(1000000,1); gate.budget(0,1,1000000,1);
            check(gate.release() && !gate.release(), "no duplicate grant");
            check(gate.budget(4096+10,1,1000000,1) == 0 && !gate.failed() && gate.tick() == 4106000, "small instruction overrun charged");
            check(gate.budget(4106,1,1000000,1) == 0 && gate.tick() == 4106000, "held clock stable");
            gate.budget(4107,1,1000000,1); check(gate.failed(), "no unexplained progress while held");
        } else if (name == "guest_gate_discontinuity") {
            for (unsigned variant = 0; variant < 4; ++variant) {
                GuestExecutionGate gate(1000000,1); gate.budget(100,1,1000000,1); gate.release();
                if (variant == 0) gate.budget(100,2,1000000,1);
                if (variant == 1) gate.budget(100,1,2000000,1);
                if (variant == 2) gate.budget(99,1,1000000,1);
                if (variant == 3) gate.budget(100+4096+65,1,1000000,1);
                check(gate.failed() && !gate.release(), "discontinuity cannot resume");
            }
        } else if (name == "guest_gate_limits") {
            GuestExecutionGate zero(0,1), overflow(1,UINT64_MAX);
            check(zero.failed() && overflow.failed(), "invalid frequency");
            GuestExecutionGate end(1000000,1); end.budget(UINT64_MAX-10,1,1000000,1);
            check(!end.release(), "grant overflow rejected");
        } else throw std::runtime_error("unknown case");
        std::cout << "PASS " << name << '\n'; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
