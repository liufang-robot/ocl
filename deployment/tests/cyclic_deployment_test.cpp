#include "deployment/DeploymentComponent.hpp"
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/os/main.h>
#include <rtt/types/StructTypeInfo.hpp>
#include <boost/serialization/nvp.hpp>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <stdexcept>

namespace {
struct YZ {
    double y = 0, z = 0;
    template<class Archive> void serialize(Archive& ar, unsigned) {
        ar & BOOST_SERIALIZATION_NVP(y) & BOOST_SERIALIZATION_NVP(z);
    }
};
struct XY {
    double x = 0, y = 0;
    template<class Archive> void serialize(Archive& ar, unsigned) {
        ar & BOOST_SERIALIZATION_NVP(x) & BOOST_SERIALIZATION_NVP(y);
    }
};
struct Nested {
    YZ sample;
    template<class Archive> void serialize(Archive& ar, unsigned) {
        ar & BOOST_SERIALIZATION_NVP(sample);
    }
};
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Producer : public RTT::TaskContext {
public:
    RTT::OutputPort<YZ> sample{"sample"};
    RTT::OutputPort<Nested> nested{"nested"};
    Producer() : TaskContext("Source") {
        auto motion = RTT::Service::Create("motion");
        auto io = RTT::Service::Create("io");
        provides()->addService(motion); motion->addService(io);
        io->addPort(sample); addPort(nested);
        setActivity(new RTT::extras::SlaveActivity(0.01));
    }
    void updateHook() override {
        sample.data().y = 7; sample.data().z = 11;
        nested.data().sample.y = 13; nested.data().sample.z = 17;
    }
};
class Scalar : public RTT::TaskContext {
public:
    RTT::OutputPort<double> value{"value"};
    Scalar() : TaskContext("Scalar") {
        addPort(value); setActivity(new RTT::extras::SlaveActivity(0.01));
    }
    void updateHook() override { value.data() = 42; }
};
class Consumer : public RTT::TaskContext {
public:
    RTT::InputPort<XY> fused{"fused"};
    RTT::InputPort<YZ> copy{"copy"}, selected{"selected"};
    RTT::InputPort<Nested> nested{"nested"};
    unsigned cycles = 0;
    Consumer() : TaskContext("Sink") {
        addPort(fused); addPort(copy); addPort(nested); addPort(selected);
        setActivity(new RTT::extras::SlaveActivity(0.01));
    }
    void updateHook() override {
        require(fused.data().x == 7 && fused.data().y == 42, "multi-source member assembly");
        require(copy.data().y == 7 && copy.data().z == 11, "whole port connection");
        require(nested.data().sample.z == 11, "whole value to member connection");
        require(selected.data().y == 13 && selected.data().z == 17, "member to whole value connection");
        ++cycles;
    }
};
void cycle(RTT::TaskContext& component) {
    require(component.getActivity()->execute(), "native cycle execution");
}

void rejectRemovedPolicies() {
    struct PolicyFile {
        const char* path = "cyclic-policy-rejection.cpf";
        ~PolicyFile() { std::remove(path); }
        void write(const char* name, int type) {
            std::ofstream file(path);
            file << "<?xml version=\"1.0\"?><properties><struct name=\"" << name
                 << "\" type=\"ConnPolicy\"><simple name=\"type\" type=\"long\"><value>"
                 << type << "</value></simple><simple name=\"size\" type=\"long\">"
                 << "<value>12</value></simple></struct></properties>";
            require(file.good(), "write connection policy fixture");
        }
    } file;
    struct RestoreDefault {
        RTT::ConnPolicy saved = RTT::ConnPolicy::Default();
        ~RestoreDefault() { RTT::ConnPolicy::Default() = saved; }
    } restore;
    RTT::ConnPolicy::Default() = RTT::ConnPolicy::data();
    for (int type : {1, 2}) {
        for (const char* name : {"removed_policy", "Default"}) {
            OCL::DeploymentComponent deployer("policy_deployer");
            file.write(name, type);
            require(!deployer.loadComponents(file.path), "reject removed numeric XML connection policy");
            require(RTT::ConnPolicy::Default().type == RTT::ConnPolicy::DATA,
                    "rejected XML policy must not change the default");
        }
        RTT::TaskContext source("policy_source"), sink("policy_sink");
        RTT::OutputPort<double> output("output");
        RTT::InputPort<double> input("input");
        source.addPort(output); sink.addPort(input);
        OCL::DeploymentComponent deployer("policy_deployer");
        deployer.addPeer(&source); deployer.addPeer(&sink);
        RTT::ConnPolicy policy = RTT::ConnPolicy::data();
        policy.type = type; policy.size = 12;
        require(!deployer.connect("policy_source.output", "policy_sink.input", policy),
                "reject removed numeric policy in connect");
        require(!input.connected() && !output.connected(), "rejected policy must not create a channel");
    }
    OCL::DeploymentComponent deployer("data_policy_deployer");
    file.write("latest_values", RTT::ConnPolicy::DATA);
    require(deployer.loadComponents(file.path), "accept latest-value XML connection policy");
}
}
int ORO_main(int, char**) {
    try {
        rejectRemovedPolicies();
        RTT::types::Types()->addType(new RTT::types::StructTypeInfo<YZ>("ocl_cyclic_yz"));
        RTT::types::Types()->addType(new RTT::types::StructTypeInfo<XY>("ocl_cyclic_xy"));
        RTT::types::Types()->addType(new RTT::types::StructTypeInfo<Nested>("ocl_cyclic_nested"));
        Producer producer; Scalar scalar; Consumer consumer;
        OCL::DeploymentComponent deployer("cyclic_deployer");
        deployer.addPeer(&producer); deployer.addPeer(&scalar); deployer.addPeer(&consumer);
        for (const auto* operation : {"connect", "connectPorts", "connectPort", "connectMember", "finalizeConnections"})
            require(deployer.provides()->hasOperation(operation), "missing cyclic deployment operation");
        require(!deployer.provides()->hasOperation("connectTwoPorts"), "removed connectTwoPorts operation remains available");
        require(deployer.runScript(OCL_CYCLIC_CONNECTION_SCRIPT), "real deployment script");
        require(!deployer.connectMember("Source.motion.io.sample", "y", "Sink.fused", "x"), "reject duplicate writer");
        require(!deployer.connectMember("Source.motion.io.sample", "missing", "Sink.fused", "x"), "reject unknown member");
        require(!deployer.connectPort("Source", "Sink.copy"), "reject incomplete qualified port path");
        require(!deployer.connectPort("Source.motion.io.sample", "Sink.fused"), "reject different parent types");
        require(!deployer.connectPort("Source.motion.missing.sample", "Sink.copy"), "reject unknown service");
        require(!deployer.connectPort("Sink.copy", "Source.motion.io.sample"), "reject reversed direction");
        require(deployer.finalizeConnections(), "finalization survives rejected declarations");
        require(producer.start() && scalar.start() && consumer.start(), "start all components");
        require(!deployer.finalizeConnections(), "reject active finalization");
        require(!deployer.connectPort("Source.motion.io.sample", "Sink.copy"), "reject active topology mutation");
        cycle(producer); cycle(scalar); cycle(consumer);
        require(consumer.cycles == 1, "consumer hook completed");
        cycle(consumer);
        require(consumer.cycles == 2, "retained input values without new source cycles");
        consumer.stop(); scalar.stop(); producer.stop();
        std::cout << "cyclic deployment test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
