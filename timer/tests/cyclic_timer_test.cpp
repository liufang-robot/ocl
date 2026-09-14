#include "timer/TimerComponent.hpp"
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/OperationCaller.hpp>
#include <rtt/os/main.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class TimerFixture : public OCL::TimerComponent {
public:
    TimerFixture() : TimerComponent("timer") {
        setActivity(new RTT::extras::SlaveActivity(0.01));
    }
    void expire(unsigned id) { mtimer.timeout(id); }
    // Drive the existing timeout handler deterministically, without wall-clock races.
    bool startHook() override { return true; }
    void stopHook() override {}
};
}
int ORO_main(int, char**) {
    try {
        TimerFixture timer;
        auto* first = dynamic_cast<RTT::OutputPort<std::uint64_t>*>(timer.ports()->getPort("timer_0"));
        auto* second = dynamic_cast<RTT::OutputPort<std::uint64_t>*>(timer.ports()->getPort("timer_1"));
        auto* total = dynamic_cast<RTT::OutputPort<std::uint64_t>*>(timer.ports()->getPort("timeout"));
        require(first && second && total, "timer state must be cumulative UInt64 counters");
        RTT::OperationCaller<bool(unsigned)> maxTimers = timer.getOperation("setMaxTimers");
        require(maxTimers.ready() && !maxTimers(33), "fixed timer capacity must be enforced");
        require(timer.configure() && timer.start(), "start timer component");
        require(!maxTimers(16), "reject timer resizing while active");
        for (unsigned i = 0; i != 5; ++i) timer.expire(0);
        timer.expire(1); timer.expire(1);
        require(first->snapshot() == 0, "callback must not publish process images");
        require(timer.getActivity()->execute(), "timer cyclic update");
        require(first->snapshot() == 5 && second->snapshot() == 2 && total->snapshot() == 7,
                "coalesced wakeups must retain expiration multiplicity");
        require(timer.getActivity()->execute() && total->snapshot() == 7, "counts persist across idle cycles");
        timer.expire(0);
        require(timer.getActivity()->execute() && first->snapshot() == 6 && total->snapshot() == 8,
                "later expiration increments cumulative state");
        timer.stop();
        std::cout << "cyclic timer test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
