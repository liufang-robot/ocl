#define BOOST_TEST_MODULE taskbrowser_value_renderer
#include <boost/test/included/unit_test.hpp>

#include "taskbrowser/internal/StructuredValueRenderer.hpp"

#include <boost/intrusive_ptr.hpp>
#include <boost/serialization/nvp.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/internal/PortDataAccess.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/Property.hpp>
#include <rtt/PropertyBag.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/SequenceTypeInfo.hpp>
#include <rtt/types/StructTypeInfo.hpp>
#include <rtt/types/TemplateTypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <cstddef>
#include <cstdint>
#include <istream>
#include <map>
#include <ostream>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "taskbrowser/TaskBrowser.hpp"

namespace renderer_test {

struct Point { double x{0.0}; double y{0.0}; };
struct Envelope { Point point; std::int32_t quality{0}; };
struct Opaque { std::int32_t value{0}; };
struct Empty {};
struct TextValue { std::string text; };
struct SizeCapacityValue {
  std::int32_t size{7};
  std::int32_t capacity{11};
};
struct Level4 { std::int32_t value{4}; };
struct Level3 { Level4 level4; };
struct Level2 { Level3 level3; };
struct Level1 { Level2 level2; };
struct WideValue {
  std::int32_t m00{0}, m01{1}, m02{2}, m03{3}, m04{4}, m05{5}, m06{6};
  std::int32_t m07{7}, m08{8}, m09{9}, m10{10}, m11{11}, m12{12}, m13{13};
  std::int32_t m14{14}, m15{15}, m16{16}, m17{17}, m18{18}, m19{19}, m20{20};
};

std::ostream &operator<<(std::ostream &stream, const Point &value) {
  return stream << "Point{" << value.x << ", " << value.y << '}';
}
std::ostream &operator<<(std::ostream &stream, const Envelope &value) {
  return stream << "Envelope{" << value.point << ", " << value.quality << '}';
}
std::ostream &operator<<(std::ostream &stream, const Opaque &value) {
  return stream << "Opaque{" << value.value << '}';
}

std::istream &operator>>(std::istream &stream, Point &) { return stream; }
std::istream &operator>>(std::istream &stream, Envelope &) { return stream; }
std::istream &operator>>(std::istream &stream, Opaque &) { return stream; }

} // namespace renderer_test

namespace OCL::detail {

std::ostream &operator<<(std::ostream &stream, StructuredValueRenderStatus status) {
  return stream << static_cast<int>(status);
}

} // namespace OCL::detail

namespace boost::serialization {

template <class Archive>
void serialize(Archive &archive, renderer_test::Point &value, const unsigned int) {
  archive & make_nvp("x", value.x);
  archive & make_nvp("y", value.y);
}
template <class Archive>
void serialize(Archive &archive, renderer_test::Envelope &value, const unsigned int) {
  archive & make_nvp("point", value.point);
  archive & make_nvp("quality", value.quality);
}
template <class Archive>
void serialize(Archive &, renderer_test::Empty &, const unsigned int) {}
template <class Archive>
void serialize(Archive &archive, renderer_test::TextValue &value, const unsigned int) {
  archive & make_nvp("text", value.text);
}
template <class Archive>
void serialize(Archive &archive, renderer_test::SizeCapacityValue &value,
               const unsigned int) {
  archive & make_nvp("size", value.size);
  archive & make_nvp("capacity", value.capacity);
}
template <class Archive>
void serialize(Archive &archive, renderer_test::Level4 &value, const unsigned int) {
  archive & make_nvp("value", value.value);
}
template <class Archive>
void serialize(Archive &archive, renderer_test::Level3 &value, const unsigned int) {
  archive & make_nvp("level4", value.level4);
}
template <class Archive>
void serialize(Archive &archive, renderer_test::Level2 &value, const unsigned int) {
  archive & make_nvp("level3", value.level3);
}
template <class Archive>
void serialize(Archive &archive, renderer_test::Level1 &value, const unsigned int) {
  archive & make_nvp("level2", value.level2);
}
template <class Archive>
void serialize(Archive &archive, renderer_test::WideValue &value, const unsigned int) {
  archive & make_nvp("m00", value.m00);
  archive & make_nvp("m01", value.m01);
  archive & make_nvp("m02", value.m02);
  archive & make_nvp("m03", value.m03);
  archive & make_nvp("m04", value.m04);
  archive & make_nvp("m05", value.m05);
  archive & make_nvp("m06", value.m06);
  archive & make_nvp("m07", value.m07);
  archive & make_nvp("m08", value.m08);
  archive & make_nvp("m09", value.m09);
  archive & make_nvp("m10", value.m10);
  archive & make_nvp("m11", value.m11);
  archive & make_nvp("m12", value.m12);
  archive & make_nvp("m13", value.m13);
  archive & make_nvp("m14", value.m14);
  archive & make_nvp("m15", value.m15);
  archive & make_nvp("m16", value.m16);
  archive & make_nvp("m17", value.m17);
  archive & make_nvp("m18", value.m18);
  archive & make_nvp("m19", value.m19);
  archive & make_nvp("m20", value.m20);
}

} // namespace boost::serialization

namespace {

void loadRendererTypes() {
  auto types = RTT::types::Types();
  if (types->type("Float64") == nullptr) {
    RTT::types::RealTimeTypekitPlugin().loadTypes();
  }
  if (types->type("/test/taskbrowser/Point") == nullptr) {
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::Point, true>("/test/taskbrowser/Point")));
    BOOST_REQUIRE(types->addType(new RTT::types::SequenceTypeInfo<std::vector<renderer_test::Point>>("/test/taskbrowser/PointArray")));
    BOOST_REQUIRE(types->addType(new RTT::types::SequenceTypeInfo<std::vector<std::vector<std::int32_t>>>("/test/taskbrowser/Int32Matrix")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::Envelope, true>("/test/taskbrowser/Envelope")));
    BOOST_REQUIRE(types->addType(new RTT::types::TemplateTypeInfo<renderer_test::Opaque, true>("/test/taskbrowser/Opaque")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::Empty, false>("/test/taskbrowser/Empty")));
    BOOST_REQUIRE(types->addType(new RTT::types::SequenceTypeInfo<std::vector<renderer_test::Empty>>("/test/taskbrowser/EmptyArray")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::TextValue, false>("/test/taskbrowser/TextValue")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::SizeCapacityValue, false>("/test/taskbrowser/SizeCapacityValue")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::Level4, false>("/test/taskbrowser/Level4")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::Level3, false>("/test/taskbrowser/Level3")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::Level2, false>("/test/taskbrowser/Level2")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::Level1, false>("/test/taskbrowser/Level1")));
    BOOST_REQUIRE(types->addType(new RTT::types::StructTypeInfo<renderer_test::WideValue, false>("/test/taskbrowser/WideValue")));
  }
}

template <typename T>
RTT::base::DataSourceBase::shared_ptr valueSource(T value) {
  return new RTT::internal::ValueDataSource<T>(std::move(value));
}

template <typename T>
class CountingDataSource final : public RTT::internal::DataSource<T> {
public:
  explicit CountingDataSource(T value, bool succeeds = true)
      : value_(std::move(value)), succeeds_(succeeds) {}
  bool evaluate() const override { ++evaluation_count_; return succeeds_; }
  typename RTT::internal::DataSource<T>::result_t get() const override { return value_; }
  typename RTT::internal::DataSource<T>::result_t value() const override { return value_; }
  typename RTT::internal::DataSource<T>::const_reference_t rvalue() const override { return value_; }
  CountingDataSource *clone() const override { return new CountingDataSource(value_, succeeds_); }
  CountingDataSource *copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *> &copies) const override {
    const auto existing = copies.find(this);
    if (existing != copies.end()) return static_cast<CountingDataSource *>(existing->second);
    auto *copy = new CountingDataSource(value_, succeeds_);
    copies[this] = copy;
    return copy;
  }
  std::size_t evaluationCount() const { return evaluation_count_; }
private:
  T value_;
  bool succeeds_;
  mutable std::size_t evaluation_count_{0};
};

template <typename T>
class MutationProbeDataSource final : public RTT::internal::AssignableDataSource<T> {
public:
  explicit MutationProbeDataSource(T value) : value_(std::move(value)) {}
  bool evaluate() const override { ++evaluation_count_; return true; }
  typename RTT::internal::DataSource<T>::result_t get() const override { return value_; }
  typename RTT::internal::DataSource<T>::result_t value() const override { return value_; }
  typename RTT::internal::DataSource<T>::const_reference_t rvalue() const override { return value_; }
  void set(typename RTT::internal::AssignableDataSource<T>::param_t value) override { ++set_value_count_; value_ = value; }
  typename RTT::internal::AssignableDataSource<T>::reference_t set() override { ++mutable_reference_count_; return value_; }
  void updated() override { ++updated_count_; }
  MutationProbeDataSource *clone() const override { return new MutationProbeDataSource(value_); }
  MutationProbeDataSource *copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *> &copies) const override {
    const auto existing = copies.find(this);
    if (existing != copies.end()) return static_cast<MutationProbeDataSource *>(existing->second);
    auto *copy = new MutationProbeDataSource(value_);
    copies[this] = copy;
    return copy;
  }
  std::size_t evaluationCount() const { return evaluation_count_; }
  std::size_t setValueCount() const { return set_value_count_; }
  std::size_t mutableReferenceCount() const { return mutable_reference_count_; }
  std::size_t updatedCount() const { return updated_count_; }
private:
  T value_;
  mutable std::size_t evaluation_count_{0};
  std::size_t set_value_count_{0};
  std::size_t mutable_reference_count_{0};
  std::size_t updated_count_{0};
};

} // namespace

class TaskBrowserProbe final : public OCL::TaskBrowser {
public:
  explicit TaskBrowserProbe(RTT::TaskContext *task)
      : OCL::TaskBrowser(task) {
    setColorTheme(nocolors);
  }

  std::string render(RTT::base::DataSourceBase::shared_ptr source,
                     bool recurse = true) {
    sresult.str("");
    sresult.clear();
    printResult(source.get(), recurse);
    return sresult.str();
  }

  std::string listing(const std::string &path = "", bool serviceHelp = false) {
    std::ostringstream output;
    struct RestoreOutput {
      std::streambuf *previous;
      ~RestoreOutput() { std::cout.rdbuf(previous); }
    } restore{std::cout.rdbuf(output.rdbuf())};
    if (serviceHelp) printService(path);
    else printInfo(path);
    return output.str();
  }
};

struct TaskBrowserFixture {
  TaskBrowserFixture()
      : task("taskbrowser-value-renderer"), browser(&task) {
    loadRendererTypes();
  }

  RTT::TaskContext task;
  TaskBrowserProbe browser;
};

struct TaskBrowserConnectionsFixture {
  TaskBrowserConnectionsFixture()
      : source("pair_source"), scalar_source("scalar_source"), sink("sink"),
        pair("output"), scalar("output"), replacement("replacement"),
        whole("whole"), input("input"), selected("selected"), unused("unused"),
        output("result"), browser(&sink) {
    loadRendererTypes();
    sink.setActivity(new RTT::extras::SlaveActivity(0.01));
    source.provides("telemetry")->provides("deep")->addPort(pair);
    scalar_source.addPort(scalar);
    scalar_source.addPort(replacement);
    sink.addPort(whole);
    auto service = sink.provides("io")->provides("deep");
    service->addPort(input);
    service->addPort(selected);
    service->addPort(unused);
    service->addPort(output);
    BOOST_REQUIRE(pair.connectTo(&whole));
    // Deliberately create y before x; the listing should follow destination order.
    BOOST_REQUIRE(RTT::connectMembers(pair, "y", input, "y"));
    BOOST_REQUIRE(RTT::connectMembers(scalar, "", input, "x"));
    BOOST_REQUIRE(RTT::connectMembers(pair, "x", selected, ""));
  }

  ~TaskBrowserConnectionsFixture() {
    if (sink.isRunning()) sink.stop();
  }

  RTT::TaskContext source, scalar_source, sink;
  RTT::OutputPort<renderer_test::Point> pair;
  RTT::OutputPort<double> scalar, replacement;
  RTT::InputPort<renderer_test::Point> whole, input;
  RTT::InputPort<double> selected, unused;
  RTT::OutputPort<double> output;
  TaskBrowserProbe browser;
};

BOOST_FIXTURE_TEST_CASE(taskbrowser_lists_whole_and_member_sources_without_consuming_input,
                        TaskBrowserConnectionsFixture) {
  pair.data() = renderer_test::Point{10.0, 20.0};
  scalar.data() = 100.0;
  RTT::internal::PortDataAccess::commit(pair);
  RTT::internal::PortDataAccess::commit(scalar);

  const auto root = browser.listing();
  BOOST_TEST(root.find("whole <- pair_source.telemetry.deep.output") != std::string::npos);
  BOOST_TEST(root.find("In(C)") != std::string::npos);

  for (const bool serviceHelp : {false, true}) {
    const auto listing = browser.listing("io.deep", serviceHelp);
    BOOST_TEST_CONTEXT((serviceHelp ? "help" : "ls") << " io.deep produced:\n" << listing) {
      const auto x = listing.find("input.x <- scalar_source.output");
      const auto y = listing.find("input.y <- pair_source.telemetry.deep.output.y");
      BOOST_REQUIRE(x != std::string::npos);
      BOOST_REQUIRE(y != std::string::npos);
      BOOST_TEST(x < y);
      BOOST_TEST(listing.find("selected <- pair_source.telemetry.deep.output.x") != std::string::npos);
      BOOST_TEST(listing.find("unused <-") == std::string::npos);
      BOOST_TEST(listing.find("result <-") == std::string::npos);
      BOOST_TEST(listing.find("In(U)") != std::string::npos);
      BOOST_TEST(listing.find("Out(U)") != std::string::npos);
    }
  }

  BOOST_TEST(input.status() == RTT::NoData);
  BOOST_REQUIRE(sink.start());
  sink.getActivity()->execute();
  BOOST_TEST(input.status() == RTT::NewData);
  BOOST_TEST(input.data().x == 100.0);
  BOOST_TEST(input.data().y == 20.0);
  BOOST_TEST(whole.data().x == 10.0);
  BOOST_TEST(selected.data() == 10.0);
}

BOOST_FIXTURE_TEST_CASE(taskbrowser_updates_sources_after_disconnect_and_reconnect,
                        TaskBrowserConnectionsFixture) {
  BOOST_REQUIRE(scalar.disconnect(&input));
  BOOST_REQUIRE(RTT::connectMembers(replacement, "", input, "x"));
  BOOST_REQUIRE(pair.disconnect(&whole));

  const auto service = browser.listing("io.deep");
  BOOST_TEST(service.find("input.x <- scalar_source.replacement") != std::string::npos);
  BOOST_TEST(service.find("input.x <- scalar_source.output") == std::string::npos);
  BOOST_TEST(service.find("input.y <- pair_source.telemetry.deep.output.y") != std::string::npos);
  const auto root = browser.listing();
  BOOST_TEST(root.find("whole <-") == std::string::npos);
  BOOST_TEST(root.find("In(U)") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(taskbrowser_prints_the_exact_named_value,
                        TaskBrowserFixture) {
  const auto source = valueSource(renderer_test::Envelope{{3.0, 4.0}, 5});
  BOOST_TEST(browser.render(source) ==
             " = {point: {x: 3.0, y: 4.0}, quality: 5}");
}

BOOST_FIXTURE_TEST_CASE(taskbrowser_reads_a_structured_root_once,
                        TaskBrowserFixture) {
  boost::intrusive_ptr<CountingDataSource<renderer_test::Envelope>> source(
      new CountingDataSource<renderer_test::Envelope>(
          renderer_test::Envelope{{3.0, 4.0}, 5}));
  BOOST_TEST(browser.render(source) ==
             " = {point: {x: 3.0, y: 4.0}, quality: 5}");
  BOOST_TEST(source->evaluationCount() == 1U);
}

BOOST_FIXTURE_TEST_CASE(taskbrowser_uses_compact_arrays_and_can_show_indices,
                        TaskBrowserFixture) {
  const auto integers = valueSource(std::vector<std::int32_t>{11, 24});
  BOOST_TEST(browser.render(valueSource(std::vector<double>{-43.5, 323.34})) ==
             " = [-43.5, 323.34]");
  BOOST_TEST(browser.render(integers) == " = [11, 24]");

  std::string action = "indices";
  browser.browserAction(action);
  BOOST_TEST(browser.render(integers) == " = [[0]: 11, [1]: 24]");
  action = "noindices";
  browser.browserAction(action);
  BOOST_TEST(browser.render(integers) == " = [11, 24]");
}

BOOST_FIXTURE_TEST_CASE(taskbrowser_reports_a_failed_root_snapshot,
                        TaskBrowserFixture) {
  boost::intrusive_ptr<CountingDataSource<renderer_test::Envelope>> source(
      new CountingDataSource<renderer_test::Envelope>(renderer_test::Envelope{}, false));
  BOOST_TEST(browser.render(source) == " = (evaluation failed)");
  BOOST_TEST(source->evaluationCount() == 1U);
}

BOOST_FIXTURE_TEST_CASE(property_bags_keep_the_specialized_display,
                        TaskBrowserFixture) {
  RTT::PropertyBag bag;
  bag.ownProperty(new RTT::Property<std::int32_t>("answer", "", 42));
  const auto source = valueSource(bag);

  const std::string summary = browser.render(source, false);
  BOOST_TEST(summary.find("1") != std::string::npos);
  BOOST_TEST(summary.find("Properties") != std::string::npos);

  const std::string expanded = browser.render(source, true);
  BOOST_TEST(expanded.find("answer") != std::string::npos);
  BOOST_TEST(expanded.find("42") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(renders_named_structures_instead_of_stream_operators) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(valueSource(renderer_test::Envelope{{3.0, 4.0}, 5}));
  BOOST_TEST(result.status == OCL::detail::StructuredValueRenderStatus::rendered);
  BOOST_TEST(result.text == "{point: {x: 3.0, y: 4.0}, quality: 5}");
}

BOOST_AUTO_TEST_CASE(preserves_a_decimal_marker_for_floating_values) {
  loadRendererTypes();
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(3.0F)).text == "3.0");
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(-4.0)).text == "-4.0");
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(1.25)).text == "1.25");
}

BOOST_AUTO_TEST_CASE(keeps_opaque_stream_representation) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(valueSource(renderer_test::Opaque{7}));
  BOOST_TEST(result.text == "Opaque{7}");
}

BOOST_AUTO_TEST_CASE(preserves_scalar_and_hexadecimal_representation) {
  loadRendererTypes();
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(true)).text == "true");
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(std::string("alpha"))).text == "alpha");
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource('x')).text == "x");
  OCL::detail::StructuredValueRenderOptions options;
  options.hexadecimal = true;
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(std::int32_t{26}), options).text == "1a");
}

BOOST_AUTO_TEST_CASE(renders_an_empty_member_aware_structure) {
  loadRendererTypes();
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(renderer_test::Empty{})).text == "{}");
}

BOOST_AUTO_TEST_CASE(size_and_capacity_fields_remain_named_structure_members) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(
      valueSource(renderer_test::SizeCapacityValue{}));
  BOOST_TEST(result.status == OCL::detail::StructuredValueRenderStatus::rendered);
  BOOST_TEST(result.text == "{size: 7, capacity: 11}");
}

class MissingQualityDataSource final
    : public RTT::internal::ValueDataSource<renderer_test::Envelope> {
public:
  using RTT::internal::ValueDataSource<renderer_test::Envelope>::ValueDataSource;

  RTT::base::DataSourceBase::shared_ptr
  getMember(const std::string &name) override {
    if (name == "quality") {
      return {};
    }
    return RTT::internal::ValueDataSource<renderer_test::Envelope>::getMember(name);
  }
};

class FailingQualityDataSource final
    : public RTT::internal::ValueDataSource<renderer_test::Envelope> {
public:
  explicit FailingQualityDataSource(renderer_test::Envelope value)
      : RTT::internal::ValueDataSource<renderer_test::Envelope>(std::move(value)),
        quality_(new CountingDataSource<std::int32_t>(5, false)) {}

  RTT::base::DataSourceBase::shared_ptr
  getMember(const std::string &name) override {
    if (name == "quality") {
      return quality_;
    }
    return RTT::internal::ValueDataSource<renderer_test::Envelope>::getMember(name);
  }

  std::vector<std::string> getMemberNames() const override {
    return {"quality", "point"};
  }

  std::size_t qualityEvaluationCount() const {
    return quality_->evaluationCount();
  }

private:
  boost::intrusive_ptr<CountingDataSource<std::int32_t>> quality_;
};

bool balancedDelimiters(const std::string &text) {
  std::vector<char> open;
  for (const char character : text) {
    if (character == '{' || character == '[') {
      open.push_back(character);
    } else if (character == '}' || character == ']') {
      if (open.empty()) return false;
      const char expected = character == '}' ? '{' : '[';
      if (open.back() != expected) return false;
      open.pop_back();
    }
  }
  return open.empty();
}

BOOST_AUTO_TEST_CASE(previews_zero_one_three_and_four_sequence_items) {
  loadRendererTypes();
  using PointArray = std::vector<renderer_test::Point>;

  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(PointArray{})).text == "[]");
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(PointArray{{1.0, 2.0}})).text ==
             "[/test/taskbrowser/Point{x: 1.0, y: 2.0}]");
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(
                 PointArray{{1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}})).text ==
             "[\n  /test/taskbrowser/Point{x: 1.0, y: 2.0},\n"
             "  /test/taskbrowser/Point{x: 3.0, y: 4.0},\n"
             "  /test/taskbrowser/Point{x: 5.0, y: 6.0}\n]");

  const auto four = OCL::detail::renderStructuredValue(valueSource(PointArray{
      {1.0, 2.0}, {3.0, 4.0}, {5.0, 6.0}, {7.0, 8.0}}));
  BOOST_TEST(four.text.find("x: 7.0") == std::string::npos);
  BOOST_TEST(four.text.find("... 1 items omitted") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(renders_primitive_floating_sequence_elements) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(
      valueSource(std::vector<double>{1.0, 2.5, 3.0}));

  BOOST_TEST(result.status == OCL::detail::StructuredValueRenderStatus::rendered);
  BOOST_TEST(result.text == "[1.0, 2.5, 3.0]");
}

BOOST_AUTO_TEST_CASE(renders_primitive_integer_sequence_elements) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(
      valueSource(std::vector<std::int32_t>{10, 20}));

  BOOST_TEST(result.status == OCL::detail::StructuredValueRenderStatus::rendered);
  BOOST_TEST(result.text == "[10, 20]");
}

BOOST_AUTO_TEST_CASE(renders_primitive_string_sequence_elements) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(
      valueSource(std::vector<std::string>{"alpha", "beta"}));

  BOOST_TEST(result.status == OCL::detail::StructuredValueRenderStatus::rendered);
  BOOST_TEST(result.text == "[alpha, beta]");
}

BOOST_AUTO_TEST_CASE(previews_a_thousand_sequence_items_with_a_bounded_multiline_form) {
  loadRendererTypes();
  using PointArray = std::vector<renderer_test::Point>;
  PointArray values(1000, renderer_test::Point{1.0, 2.0});
  BOOST_TEST(OCL::detail::renderStructuredValue(valueSource(values)).text ==
             "[\n  /test/taskbrowser/Point{x: 1.0, y: 2.0},\n"
             "  /test/taskbrowser/Point{x: 1.0, y: 2.0},\n"
             "  /test/taskbrowser/Point{x: 1.0, y: 2.0},\n"
             "  ... 997 items omitted\n]");
}

BOOST_AUTO_TEST_CASE(renders_nested_arrays_without_redundant_indices) {
  loadRendererTypes();
  using Matrix = std::vector<std::vector<std::int32_t>>;
  const auto source = valueSource(Matrix{{1, 2}, {}, {3, 4}});
  BOOST_TEST(OCL::detail::renderStructuredValue(source).text == "[[1, 2], [], [3, 4]]");

  OCL::detail::StructuredValueRenderOptions options;
  options.compact_width = 16;
  BOOST_TEST(OCL::detail::renderStructuredValue(source, options).text ==
             "[\n  [1, 2],\n  [],\n  [3, 4]\n]");
  options.max_structural_depth = 1;
  BOOST_TEST(OCL::detail::renderStructuredValue(source, options).text ==
             "[\n  [...],\n  [...],\n  [...]\n]");
}

BOOST_AUTO_TEST_CASE(retains_custom_types_in_compact_expanded_and_collapsed_arrays) {
  loadRendererTypes();
  const auto source = valueSource(std::vector<renderer_test::Point>{{1.0, 2.0}});
  OCL::detail::StructuredValueRenderOptions options;
  options.compact_width = 30;
  BOOST_TEST(OCL::detail::renderStructuredValue(source, options).text ==
             "[\n  /test/taskbrowser/Point{\n    x: 1.0\n    y: 2.0\n  }\n]");
  options.max_structural_depth = 1;
  options.compact_width = 100;
  BOOST_TEST(OCL::detail::renderStructuredValue(source, options).text ==
             "[/test/taskbrowser/Point{...}]");
  BOOST_TEST(OCL::detail::renderStructuredValue(
                 valueSource(std::vector<renderer_test::Empty>{{}})).text ==
             "[/test/taskbrowser/Empty{}]");
}

BOOST_AUTO_TEST_CASE(indexed_arrays_use_the_same_recursive_formatting) {
  loadRendererTypes();
  OCL::detail::StructuredValueRenderOptions options;
  options.sequence_indices = true;
  options.hexadecimal = true;
  BOOST_TEST(OCL::detail::renderStructuredValue(
                 valueSource(std::vector<std::int32_t>{26, 31}), options).text ==
             "[[0]: 1a, [1]: 1f]");
  BOOST_TEST(OCL::detail::renderStructuredValue(
                 valueSource(std::vector<renderer_test::Point>{{1.0, 2.0}}), options).text ==
             "[[0]: /test/taskbrowser/Point{x: 1.0, y: 2.0}]");
  options.compact_width = 1;
  BOOST_TEST(OCL::detail::renderStructuredValue(
                 valueSource(std::vector<std::int32_t>{26, 31}), options).text ==
             "[\n  [0]: 1a,\n  [1]: 1f\n]");
}

BOOST_AUTO_TEST_CASE(array_labels_and_separators_respect_the_output_budget) {
  loadRendererTypes();
  const auto source = valueSource(std::vector<renderer_test::Point>(4, {1.0, 2.0}));
  for (const bool indices : {false, true}) {
    for (const std::size_t width : {0U, 100U, 1000U}) {
      for (std::size_t budget = 23U; budget <= 220U; ++budget) {
        OCL::detail::StructuredValueRenderOptions options;
        options.sequence_indices = indices;
        options.compact_width = width;
        options.max_result_bytes = budget;
        const auto result = OCL::detail::renderStructuredValue(source, options);
        BOOST_TEST(result.status == OCL::detail::StructuredValueRenderStatus::rendered);
        BOOST_TEST(result.text.size() + 3U <= budget);
        BOOST_TEST(result.text.find("omitted") != std::string::npos);
        BOOST_TEST(balancedDelimiters(result.text));
      }
    }
  }
}

BOOST_AUTO_TEST_CASE(collapses_structural_depth_four) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(
      valueSource(renderer_test::Level1{}));
  BOOST_TEST(result.text == "{level2: {level3: {level4: {...}}}}");
}

BOOST_AUTO_TEST_CASE(limits_a_structure_to_twenty_members) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(
      valueSource(renderer_test::WideValue{}));
  BOOST_TEST(result.text.find("m19: 19") != std::string::npos);
  BOOST_TEST(result.text.find("m20: 20") == std::string::npos);
  BOOST_TEST(result.text.find("... 1 members omitted") != std::string::npos);
  BOOST_TEST(result.text.find('\n') != std::string::npos);
}

BOOST_AUTO_TEST_CASE(switches_to_multiline_above_one_hundred_characters) {
  loadRendererTypes();
  const auto at_limit = OCL::detail::renderStructuredValue(
      valueSource(renderer_test::TextValue{std::string(89, 'a')}));
  const auto above_limit = OCL::detail::renderStructuredValue(
      valueSource(renderer_test::TextValue{std::string(90, 'a')}));

  BOOST_TEST(at_limit.text.find('\n') == std::string::npos);
  BOOST_TEST(at_limit.text.size() + 3U == 100U);
  BOOST_TEST(above_limit.text ==
             "{\n  text: " + std::string(90, 'a') + "\n}");
}

BOOST_AUTO_TEST_CASE(continues_after_an_unavailable_member) {
  loadRendererTypes();
  auto snapshot = new MissingQualityDataSource(
      renderer_test::Envelope{{3.0, 4.0}, 5});
  BOOST_TEST(OCL::detail::renderStructuredSnapshotForTest(snapshot) ==
             "{point: {x: 3.0, y: 4.0}, quality: <unavailable>}");
}

BOOST_AUTO_TEST_CASE(continues_after_a_scalar_member_fails_evaluation) {
  loadRendererTypes();
  auto *probe = new FailingQualityDataSource(
      renderer_test::Envelope{{3.0, 4.0}, 5});
  RTT::base::DataSourceBase::shared_ptr snapshot(probe);
  BOOST_TEST(OCL::detail::renderStructuredSnapshotForTest(snapshot) ==
             "{quality: <unavailable>, point: {x: 3.0, y: 4.0}}");
  BOOST_TEST(probe->qualityEvaluationCount() == 1U);
}

BOOST_AUTO_TEST_CASE(enforces_a_delimiter_safe_byte_budget) {
  loadRendererTypes();
  const auto result = OCL::detail::renderStructuredValue(
      valueSource(renderer_test::TextValue{std::string(6000, 'x')}));
  BOOST_TEST(result.text.size() + 3U <= 4096U);
  BOOST_TEST(result.text.find("bytes omitted") != std::string::npos);
  BOOST_TEST(result.text.front() == '{');
  BOOST_TEST(result.text.back() == '}');
  BOOST_TEST(balancedDelimiters(result.text));

  const std::regex omitted_pattern(R"(\.\.\. ([0-9]+) bytes omitted)");
  std::smatch omitted_match;
  BOOST_REQUIRE(std::regex_search(result.text, omitted_match, omitted_pattern));
  const std::size_t value_begin = result.text.find("text: ") + 6U;
  const std::size_t marker_begin =
      static_cast<std::size_t>(omitted_match.position(0));
  const std::size_t retained = marker_begin - value_begin;
  const std::size_t omitted =
      static_cast<std::size_t>(std::stoull(omitted_match.str(1)));
  BOOST_TEST(retained + omitted == 6000U);
}

BOOST_AUTO_TEST_CASE(truncates_structural_output_without_breaking_delimiters) {
  loadRendererTypes();
  OCL::detail::StructuredValueRenderOptions small_budget;
  small_budget.max_result_bytes = 80;
  const auto bounded = OCL::detail::renderStructuredValue(
      valueSource(renderer_test::WideValue{}), small_budget);
  BOOST_TEST(bounded.text.size() + 3U <= 80U);
  BOOST_TEST(bounded.text.find("... output omitted") != std::string::npos);
  BOOST_TEST(balancedDelimiters(bounded.text));
}

BOOST_AUTO_TEST_CASE(uses_a_compact_explicit_omission_when_multiline_cannot_fit) {
  loadRendererTypes();
  OCL::detail::StructuredValueRenderOptions small_budget;
  small_budget.max_result_bytes = 24;
  const auto bounded = OCL::detail::renderStructuredValue(
      valueSource(renderer_test::WideValue{}), small_budget);
  BOOST_TEST(bounded.text == "{... output omitted}");
  BOOST_TEST(bounded.text.size() + 3U <= 24U);
  BOOST_TEST(balancedDelimiters(bounded.text));
}

BOOST_AUTO_TEST_CASE(rejects_zero_byte_budget_without_evaluating_the_source) {
  loadRendererTypes();
  boost::intrusive_ptr<CountingDataSource<renderer_test::Envelope>> source(
      new CountingDataSource<renderer_test::Envelope>(renderer_test::Envelope{}));
  OCL::detail::StructuredValueRenderOptions options;
  options.max_result_bytes = 0U;

  const auto result = OCL::detail::renderStructuredValue(source, options);

  BOOST_TEST(result.status ==
             OCL::detail::StructuredValueRenderStatus::evaluation_failed);
  BOOST_TEST(result.text.empty());
  BOOST_TEST(result.text.size() <= options.max_result_bytes);
  BOOST_TEST(source->evaluationCount() == 0U);
}

BOOST_AUTO_TEST_CASE(rejects_budget_below_minimum_without_evaluating_the_source) {
  loadRendererTypes();
  boost::intrusive_ptr<CountingDataSource<renderer_test::Envelope>> source(
      new CountingDataSource<renderer_test::Envelope>(renderer_test::Envelope{}));
  OCL::detail::StructuredValueRenderOptions options;
  options.max_result_bytes =
      OCL::detail::StructuredValueRenderOptions::minimum_max_result_bytes - 1U;

  const auto result = OCL::detail::renderStructuredValue(source, options);

  BOOST_TEST(result.status ==
             OCL::detail::StructuredValueRenderStatus::evaluation_failed);
  BOOST_TEST(result.text.empty());
  BOOST_TEST(result.text.size() + 3U <= options.max_result_bytes);
  BOOST_TEST(source->evaluationCount() == 0U);
}

BOOST_AUTO_TEST_CASE(minimum_byte_budget_is_explicit_and_bounded) {
  loadRendererTypes();
  boost::intrusive_ptr<CountingDataSource<renderer_test::WideValue>> source(
      new CountingDataSource<renderer_test::WideValue>(renderer_test::WideValue{}));
  OCL::detail::StructuredValueRenderOptions options;
  options.max_result_bytes =
      OCL::detail::StructuredValueRenderOptions::minimum_max_result_bytes;

  const auto result = OCL::detail::renderStructuredValue(source, options);

  BOOST_TEST(result.status == OCL::detail::StructuredValueRenderStatus::rendered);
  BOOST_TEST(result.text == "{... output omitted}");
  BOOST_TEST(result.text.size() + 3U <= options.max_result_bytes);
  BOOST_TEST(balancedDelimiters(result.text));
  BOOST_TEST(source->evaluationCount() == 1U);
}

BOOST_AUTO_TEST_CASE(snapshots_a_structured_source_exactly_once) {
  loadRendererTypes();
  boost::intrusive_ptr<CountingDataSource<renderer_test::Envelope>> source(
      new CountingDataSource<renderer_test::Envelope>(
          renderer_test::Envelope{{3.0, 4.0}, 5}));
  const auto result = OCL::detail::renderStructuredValue(source);
  BOOST_TEST(result.status == OCL::detail::StructuredValueRenderStatus::rendered);
  BOOST_TEST(source->evaluationCount() == 1U);
  BOOST_TEST(result.text == "{point: {x: 3.0, y: 4.0}, quality: 5}");
}

BOOST_AUTO_TEST_CASE(never_traverses_or_mutates_the_source) {
  loadRendererTypes();
  boost::intrusive_ptr<MutationProbeDataSource<renderer_test::Envelope>> source(
      new MutationProbeDataSource<renderer_test::Envelope>(
          renderer_test::Envelope{{3.0, 4.0}, 5}));
  const auto result = OCL::detail::renderStructuredValue(source);
  BOOST_TEST(result.text == "{point: {x: 3.0, y: 4.0}, quality: 5}");
  BOOST_TEST(source->evaluationCount() == 1U);
  BOOST_TEST(source->setValueCount() == 0U);
  BOOST_TEST(source->mutableReferenceCount() == 0U);
  BOOST_TEST(source->updatedCount() == 0U);
}
