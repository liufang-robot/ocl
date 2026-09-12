#include "reporting/ReportingComponent.hpp"
#include <rtt/OutputPort.hpp>
#include <rtt/types/StructTypeInfo.hpp>
#include <rtt/types/SequenceTypeInfo.hpp>
#include <boost/serialization/nvp.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/os/main.h>
#include <iostream>
#include <stdexcept>
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
struct Motion {
    double position = 0, velocity = 0;
    template<class Archive> void serialize(Archive& ar, unsigned) {
        ar & BOOST_SERIALIZATION_NVP(position) & BOOST_SERIALIZATION_NVP(velocity);
    }
};
class NullMarshaller : public RTT::marsh::MarshallInterface {
public:
    void serialize(RTT::base::PropertyBase*) override {}
    void serialize(const RTT::PropertyBag&) override {}
    void flush() override {}
};
class UnsupportedPort : public RTT::OutputPort<double> {
public:
    UnsupportedPort() : OutputPort("unsupported") {}
    RTT::base::DataSourceBase::shared_ptr getDataSource() const override { return {}; }
};
class Reporter : public OCL::ReportingComponent {
public:
    Reporter() : ReportingComponent("reporter") {}
    ~Reporter() { if (!report.empty()) cleanReport(); }
    bool sample() { return copydata(); }
    void build() { makeReport2(); }
    std::size_t sources() const { return root.size(); }
    RTT::PropertyBag& fields(const char* name) {
        auto* nested = dynamic_cast<RTT::Property<RTT::PropertyBag>*>(report.getProperty(name));
        require(nested != nullptr, "report must decompose a structured sample");
        return nested->value();
    }
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
        RTT::types::Types()->addType(new RTT::types::StructTypeInfo<Motion>("ocl_report_motion"));
        RTT::TaskContext structured("structured");
        structured.setActivity(new RTT::extras::SlaveActivity(0.01));
        RTT::OutputPort<Motion> motion("motion"); structured.addPort(motion);
        Reporter detail; detail.addPeer(&structured);
        require(detail.reportPort("structured", "motion"), "register structured observer");
        motion.data().position = 5; motion.data().velocity = 7;
        require(structured.start() && structured.getActivity()->execute(), "commit structured sample");
        require(detail.sample(), "structured observer is fresh"); detail.build();
        auto& fields = detail.fields("structured.motion");
        require(fields.size() == 2, "default decomposition must retain readonly snapshot fields");
        auto* position = dynamic_cast<RTT::Property<double>*>(fields.getProperty("position"));
        auto* velocity = dynamic_cast<RTT::Property<double>*>(fields.getProperty("velocity"));
        require(position && velocity && position->get() == 5 && velocity->get() == 7, "report contains committed structured fields");
        position->set(99);
        require(motion.snapshot().position == 5, "report storage must not mutate source snapshots");
        motion.data().position = 11; motion.data().velocity = 13;
        require(structured.getActivity()->execute() && detail.sample(), "refresh structured report storage");
        require(position->get() == 11 && velocity->get() == 13, "decomposed fields retain bindings across refresh");
        structured.stop();
        Reporter active; active.addPeer(&structured);
        active.setActivity(new RTT::extras::SlaveActivity(0.01));
        active.addMarshaller(new NullMarshaller, new NullMarshaller);
        require(active.reportPort("structured", "motion") && active.start(), "start structured reporter");
        require(!active.unreportComponent("structured") && active.sources() == 1, "active unregister must preserve report sources");
        require(!active.reportComponent("structured") && active.sources() == 1, "active registration is rejected");
        active.stop();
        RTT::OutputPort<std::vector<double>> sequence("sequence"); structured.addPort(sequence);
        sequence.data() = {};
        require(structured.start() && structured.getActivity()->execute(), "publish initial sequence");
        Reporter resizing; resizing.addPeer(&structured);
        resizing.setActivity(new RTT::extras::SlaveActivity(0.01));
        resizing.addMarshaller(new NullMarshaller, new NullMarshaller);
        require(resizing.reportPort("structured", "sequence") && resizing.start(), "start sequence reporter");
        require(resizing.fields("structured.sequence").size() == 0, "empty initial sequence decomposition");
        sequence.data() = {5, 7, 11, 13};
        require(structured.getActivity()->execute() && resizing.getActivity()->execute(), "report expanded sequence");
        require(resizing.fields("structured.sequence").size() == 4, "resize rebuilds fields before report emission");
        auto* fourth = dynamic_cast<RTT::Property<double>*>(resizing.fields("structured.sequence").getProperty("3"));
        require(fourth && fourth->get() == 13, "expanded sequence reports committed values");
        sequence.data() = {17};
        require(structured.getActivity()->execute() && resizing.getActivity()->execute(), "report contracted sequence");
        require(resizing.fields("structured.sequence").size() == 1, "shrinking removes old member references");
        resizing.stop(); structured.stop();
        structured.ports()->removePort("sequence");
        typedef std::vector<std::vector<double>> Matrix;
        RTT::types::Types()->addType(new RTT::types::SequenceTypeInfo<Matrix>("ocl_report_matrix"));
        RTT::OutputPort<Matrix> matrix("matrix"); structured.addPort(matrix);
        matrix.data() = {{1, 2}, {3}};
        require(structured.start() && structured.getActivity()->execute(), "commit nested sequences");
        Reporter nested; nested.addPeer(&structured);
        nested.setActivity(new RTT::extras::SlaveActivity(0.01));
        nested.addMarshaller(new NullMarshaller, new NullMarshaller);
        require(nested.reportPort("structured", "matrix") && nested.start(), "start nested sequence reporter");
        matrix.data() = Matrix(100, std::vector<double>{19, 23});
        require(structured.getActivity()->execute() && nested.getActivity()->execute(), "report relocated outer sequence");
        auto& rows = nested.fields("structured.matrix");
        require(rows.size() == 100, "outer sequence rebuilds without reading stale inner references");
        auto* row = dynamic_cast<RTT::Property<RTT::PropertyBag>*>(rows.getProperty("99"));
        require(row && row->value().size() == 2, "nested sequence fields rebuilt");
        auto* last = dynamic_cast<RTT::Property<double>*>(row->value().getProperty("1"));
        require(last && last->get() == 23, "nested sequence values follow the committed snapshot");
        nested.stop(); structured.stop(); structured.ports()->removePort("matrix");
        UnsupportedPort unsupported; structured.addPort(unsupported);
        require(!detail.reportPort("structured", "unsupported"), "missing snapshot codecs must fail safely");
        structured.ports()->removePort("unsupported"); structured.ports()->removePort("motion");
        std::cout << "cyclic reporting test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
