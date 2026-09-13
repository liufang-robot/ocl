#include "deployment/DeploymentComponent.hpp"
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/OperationCaller.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/os/main.h>
#include <rtt/types/StructTypeInfo.hpp>
#include <rtt/types/CArrayTypeInfo.hpp>
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
    YZ axes[3];
    double values[3] = {};
    int mode = 0;
    template<class Archive> void serialize(Archive& ar, unsigned) {
        ar & BOOST_SERIALIZATION_NVP(sample)
           & boost::serialization::make_nvp("axes", boost::serialization::make_array(axes, 3))
           & boost::serialization::make_nvp("values", boost::serialization::make_array(values, 3))
           & BOOST_SERIALIZATION_NVP(mode);
    }
};
struct ShortArray {
    double values[2] = {};
    template<class Archive> void serialize(Archive& ar, unsigned) {
        ar & boost::serialization::make_nvp("values", boost::serialization::make_array(values, 2));
    }
};
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
class Producer : public RTT::TaskContext {
public:
    RTT::OutputPort<YZ> sample{"sample"};
    RTT::OutputPort<Nested> nested{"nested"};
    RTT::OutputPort<ShortArray> shortArray{"short_array"};
    Producer() : TaskContext("Source") {
        auto motion = RTT::Service::Create("motion");
        auto io = RTT::Service::Create("io");
        provides()->addService(motion); motion->addService(io);
        io->addPort(sample); addPort(nested); addPort(shortArray);
        setActivity(new RTT::extras::SlaveActivity(0.01));
    }
    void updateHook() override {
        sample.data().y = 7; sample.data().z = 11;
        nested.data().sample.y = 13; nested.data().sample.z = 17;
        nested.data().axes[2].y = 23; nested.data().axes[2].z = 29;
        nested.data().values[0] = 31; nested.data().values[1] = 37; nested.data().values[2] = 41;
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
    RTT::InputPort<Nested> invalid{"invalid"};
    RTT::InputPort<double> scalar{"scalar"}, unmapped{"unmapped"};
    unsigned cycles = 0;
    Consumer() : TaskContext("Sink") {
        addPort(fused); addPort(copy); addPort(nested); addPort(selected);
        addPort(invalid); addPort(scalar); addPort(unmapped);
        setActivity(new RTT::extras::SlaveActivity(0.01));
    }
    void updateHook() override {
        require(fused.data().x == 7 && fused.data().y == 42, "multi-source member assembly");
        require(copy.data().y == 7 && copy.data().z == 11, "whole port connection");
        require(nested.data().sample.z == 11, "whole value to member connection");
        require(selected.data().y == 13 && selected.data().z == 17, "member to whole value connection");
        require(nested.data().axes[0].y == 23 && nested.data().axes[0].z == 29, "fixed-array struct selection");
        require(nested.data().axes[1].y == 42 && nested.data().axes[1].z == 0, "scalar to nested fixed-array member");
        require(nested.data().values[0] == 31 && nested.data().values[1] == 37 && nested.data().values[2] == 41,
                "whole fixed-array member connection");
        require(scalar.data() == 23, "nested fixed-array member to scalar");
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
        RTT::types::Types()->addType(new RTT::types::CArrayTypeInfo<RTT::types::carray<YZ>>("ocl_cyclic_axes"));
        RTT::types::Types()->addType(new RTT::types::CArrayTypeInfo<RTT::types::carray<double>>("ocl_cyclic_values"));
        RTT::types::Types()->addType(new RTT::types::StructTypeInfo<Nested>("ocl_cyclic_nested"));
        RTT::types::Types()->addType(new RTT::types::StructTypeInfo<ShortArray>("ocl_cyclic_short_array"));
        Producer producer; Scalar scalar; Consumer consumer;
        OCL::DeploymentComponent deployer("cyclic_deployer");
        deployer.addPeer(&producer); deployer.addPeer(&scalar); deployer.addPeer(&consumer);
        RTT::OutputPort<double> self_output("self_output");
        RTT::InputPort<double> self_input("self_input");
        deployer.addPort(self_output); deployer.addPort(self_input);
        require(deployer.connectPort("this.self_output", "cyclic_deployer.self_input"),
                "retain the this alias for deployer-owned ports");
        require(deployer.isPortConnected("this.self_input"), "inspect the deployer-owned input");
        require(deployer.disconnectPort("this.self_input"), "disconnect the deployer-owned input");
        for (const auto* operation : {"connect", "connectPorts", "connectPort", "finalizeConnections", "isPortConnected", "disconnectPort"})
            require(deployer.provides()->hasOperation(operation), "missing cyclic deployment operation");
        require(!deployer.provides()->hasOperation("connectTwoPorts"), "removed connectTwoPorts operation remains available");
        require(!deployer.provides()->hasOperation("connectMember"), "removed connectMember operation remains available");
        for (const char* source : {"", "::sample", "Source.nested::sample", "Source.nested::sample::y",
                                   "Source.nested.:sample", "Source..nested", "Source.nested.", "Source"}) {
            require(!deployer.connectPort(source, "Sink.invalid"), "reject malformed source endpoint");
            require(!consumer.invalid.connected(), "malformed source must not create a connection");
        }
        for (const char* destination : {"", "::sample", "Sink.invalid::sample", "Sink.invalid::sample::y",
                                        "Sink.invalid.:sample", "Sink..invalid", "Sink.invalid.", "Sink"}) {
            require(!deployer.connectPort("Source.nested", destination), "reject malformed destination endpoint");
            require(!consumer.invalid.connected(), "malformed destination must not create a connection");
        }
        for (const char* source : {"Source.nested.missing", "Source.nested.axes[-1].y", "Source.nested.axes[3].y",
                                   "Source.nested.axes[1x].y", "Source.nested.axes[1].", "Source.nested.sample..y"}) {
            require(!deployer.connectPort(source, "Sink.unmapped"), "reject invalid member or fixed-array selector");
            require(!consumer.unmapped.connected(), "invalid member must not create a connection");
        }
        require(!deployer.connectPort("Source.motion.io.sample::y", "Sink.unmapped"), "reject otherwise valid legacy source selector");
        require(!deployer.connectPort("Scalar.value", "Sink.invalid::sample.y"), "reject otherwise valid legacy destination selector");
        require(!consumer.unmapped.connected() && !consumer.invalid.connected(), "legacy syntax creates no writers");
        require(!deployer.connectPort("Source.nested.mode", "Sink.unmapped"), "reject different selected types");
        require(!deployer.connectPort("Source.short_array.values", "Sink.invalid.values"), "reject different fixed-array shapes");
        require(deployer.runScript(OCL_CYCLIC_CONNECTION_SCRIPT), "real deployment script");
        require(!deployer.connectPort("Source.motion.io.sample.y", "Sink.fused.x"), "reject duplicate writer");
        require(!deployer.connectPort("Source.nested", "Sink.nested"), "reject whole writer overlapping members");
        require(!deployer.connectPort("Scalar.value", "Sink.copy.y"), "reject member writer overlapping whole value");
        require(!deployer.connectPort("Scalar.value", "Sink.nested.axes[0].y"), "reject writer overlapping selected struct");
        require(!deployer.connectPort("Source", "Sink.copy"), "reject incomplete qualified port path");
        require(!deployer.connectPort("Source.motion.io.sample", "Sink.fused"), "reject different parent types");
        require(!deployer.connectPort("Source.motion.missing.sample", "Sink.copy"), "reject unknown service");
        require(!deployer.connectPort("Sink.copy", "Source.motion.io.sample"), "reject reversed direction");
        RTT::OperationCaller<bool(const std::string&)> connected = deployer.getOperation("isPortConnected");
        RTT::OperationCaller<bool(const std::string&)> disconnect = deployer.getOperation("disconnectPort");
        require(connected.ready() && disconnect.ready(), "explicit port management operations");
        require(connected("Sink.copy") && connected("Source.motion.io.sample"), "whole port connectivity includes cyclic sources");
        require(!connected("Sink.unmapped") && !connected("Missing.port"), "unconnected or unknown port is false");
        require(!connected("Sink.copy.y"), "connectivity requires whole port path");
        require(!disconnect("Sink.copy.y"), "member path cannot disconnect siblings");
        require(connected("Sink.copy"), "invalid disconnect leaves graph intact");
        require(deployer.finalizeConnections(), "finalization survives rejected declarations");
        require(producer.start() && scalar.start() && consumer.start(), "start all components");
        require(!deployer.finalizeConnections(), "reject active finalization");
        require(!deployer.connectPort("Source.motion.io.sample", "Sink.copy"), "reject active topology mutation");
        require(!disconnect("Sink.copy"), "active disconnect is rejected");
        cycle(producer); cycle(scalar); cycle(consumer);
        require(consumer.cycles == 1, "consumer hook completed");
        cycle(consumer);
        require(consumer.cycles == 2, "retained input values without new source cycles");
        consumer.stop(); scalar.stop(); producer.stop();
        require(disconnect("Sink.fused"), "disconnect all member writers of one port");
        require(!connected("Sink.fused") && connected("Sink.copy"), "disconnect preserves unrelated ports");
        require(disconnect("Sink.copy"), "disconnect whole writer");
        require(!connected("Sink.copy"), "whole writer removed");
        std::cout << "cyclic deployment test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
