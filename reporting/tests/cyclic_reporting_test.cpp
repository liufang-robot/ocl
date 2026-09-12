#include "reporting/ReportingComponent.hpp"
#include <rtt/OutputPort.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/os/main.h>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Reporter : public OCL::ReportingComponent {
public:
    Reporter() : ReportingComponent("reporter") {}
    bool sample() { return copydata(); }
    double value() {
        return RTT::internal::DataSource<double>::narrow(root.front().get<T_PortDS>().get())->get();
    }
};
class Producer : public RTT::TaskContext {
public:
    RTT::OutputPort<double> output{"output"};
    double next = 0;
    Producer() : TaskContext("producer") {
        addPort(output); setActivity(new RTT::extras::SlaveActivity(0.01));
    }
    void updateHook() override { output.data() = next; }
};
}
int ORO_main(int, char**) {
    try {
        Producer producer; Reporter first; Reporter second;
        first.addPeer(&producer); second.addPeer(&producer);
        require(first.reportPort("producer", "output") && second.reportPort("producer", "output"), "register observers");
        require(first.ports()->getPortNames().empty(), "reporters must not create consuming input ports");
        require(producer.start(), "start producer");
        producer.next = 12;
        require(producer.getActivity()->execute(), "publish component cycle");
        require(first.sample() && first.value() == 12, "first observer sees committed output");
        require(!first.sample(), "repeated observation must not invent freshness");
        require(second.sample() && second.value() == 12, "observer freshness is independent");
        producer.output.data() = 99;
        require(!first.sample() && first.value() == 12, "uncommitted output must remain invisible");
        producer.stop();
        require(first.unreportPort("producer", "output"), "detach snapshot observer");
        std::cout << "cyclic reporting test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
