#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define BOOST_TEST_MODULE ocl_opcua_deployment
#include <boost/test/included/unit_test.hpp>

#include <rtt/internal/PortDataAccess.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include "deployment/OpcUaDeploymentComponent.hpp"

#include <rtt/InputPort.hpp>
#include <rtt/OperationCaller.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/Property.hpp>
#include <rtt/Service.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/base/AttributeBase.hpp>
#include <rtt/base/InputPortInterface.hpp>
#include <rtt/base/OutputPortInterface.hpp>
#include <rtt/deployment/ComponentLoader.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/internal/GlobalService.hpp>
#include <rtt/plugin/PluginLoader.hpp>
#include <rtt/opcua/port_direction.hpp>
#include <rtt/opcua/datatype_registry.hpp>
#include <rtt/opcua/node_id.hpp>
#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/server.hpp>
#include <rtt/opcua/task_context_proxy.hpp>
#include <rtt/os/StartStopManager.hpp>
#include <rtt/os/startstop.h>
#include <rtt/typekit/RealTimeTypekit.hpp>
#include <rtt/types/TemplateTypeInfo.hpp>
#include <rtt/types/Types.hpp>

#include <open62541pp/client.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#include <open62541pp/services/method.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

BOOST_TEST_DONT_PRINT_LOG_VALUE(::opcua::NodeClass)
BOOST_TEST_DONT_PRINT_LOG_VALUE(::opcua::NodeId)
BOOST_TEST_DONT_PRINT_LOG_VALUE(::opcua::ValueRank)

namespace {

#ifdef _WIN32
using TestSocket = SOCKET;
using TestSocklen = int;
constexpr auto kInvalidSocket = INVALID_SOCKET;
void closeTestSocket(TestSocket socket) { ::closesocket(socket); }
#else
using TestSocket = int;
using TestSocklen = socklen_t;
constexpr auto kInvalidSocket = -1;
void closeTestSocket(TestSocket socket) { ::close(socket); }
#endif

class RttProcessFixture final {
public:
  RttProcessFixture() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("failed to initialize test sockets");
    }
#endif
    auto &suite = boost::unit_test::framework::master_test_suite();
    if (__os_init(suite.argc, suite.argv) != 0) {
      throw std::runtime_error("failed to initialize RTT test process");
    }
  }

  ~RttProcessFixture() {
#ifdef OROCOS_TARGET_XENOMAI
    RTT::os::StartStopManager::Instance()->stop();
    RTT::os::StartStopManager::Release();
#else
    __os_exit();
#endif
#ifdef _WIN32
    WSACleanup();
#endif
  }

  RttProcessFixture(const RttProcessFixture &) = delete;
  RttProcessFixture &operator=(const RttProcessFixture &) = delete;
};

BOOST_GLOBAL_FIXTURE(RttProcessFixture);

struct UnsupportedValue {
  std::int32_t value{0};
};

constexpr std::string_view kUnsupportedTypeName = "/test/OclUnsupportedValue";

std::uint16_t unusedLoopbackPort() {
  const TestSocket socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd == kInvalidSocket) {
    throw std::runtime_error("failed to create test socket");
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (::bind(socket_fd, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0) {
    closeTestSocket(socket_fd);
    throw std::runtime_error("failed to bind test socket");
  }

  TestSocklen size = sizeof(address);
  if (::getsockname(socket_fd, reinterpret_cast<sockaddr *>(&address), &size) !=
      0) {
    closeTestSocket(socket_fd);
    throw std::runtime_error("failed to inspect test socket");
  }
  const std::uint16_t port = ntohs(address.sin_port);
  closeTestSocket(socket_fd);
  return port;
}

::opcua::NodeId modelNodeId(std::uint16_t namespace_index,
                            std::initializer_list<std::string_view> segments) {
  const std::vector<std::string_view> path_segments(segments);
  return ::opcua::NodeId(namespace_index,
                         RTT::opcua::makeNodePath(path_segments));
}

std::uint16_t namespaceIndex(::opcua::Client &client) {
  const auto namespaces = client.namespaceArray();
  const auto found = std::find(namespaces.begin(), namespaces.end(),
                               RTT::opcua::kNamespaceUri);
  if (found == namespaces.end()) {
    throw std::runtime_error("RTT OPC UA namespace URI is missing");
  }
  const auto index = std::distance(namespaces.begin(), found);
  if (index < 0 || index > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("RTT OPC UA namespace index is invalid");
  }
  return static_cast<std::uint16_t>(index);
}

void requireMissingNode(::opcua::Client &client, const ::opcua::NodeId &id) {
  const auto result = ::opcua::services::readNodeClass(client, id);
  BOOST_REQUIRE(!result);
  BOOST_TEST(result.code() == UA_STATUSCODE_BADNODEIDUNKNOWN);
}

void requireTaskStateMethod(::opcua::Client &client,
                            std::uint16_t namespace_index,
                            std::string_view component_name,
                            std::string_view operation_name,
                            std::int32_t expected_code) {
  const auto operations_id = modelNodeId(
      namespace_index, {"components", component_name, "operations"});
  const auto method_id = modelNodeId(
      namespace_index,
      {"components", component_name, "operations", operation_name});
  const auto output_types_id = modelNodeId(
      namespace_index, {"components", component_name, "operations",
                        operation_name, "rttOutputTypes"});
  const auto result =
      ::opcua::services::call(client, operations_id, method_id, {});
  BOOST_REQUIRE(result.statusCode().isGood());
  BOOST_REQUIRE_EQUAL(result.outputArguments().size(), 1U);
  BOOST_TEST(result.outputArguments()[0].isScalar());
  BOOST_TEST(result.outputArguments()[0].isType(
      ::opcua::NodeId(::opcua::DataTypeId::Int32)));
  BOOST_TEST(result.outputArguments()[0].to<std::int32_t>() == expected_code);
  BOOST_TEST(::opcua::services::readValue(client, output_types_id)
                 .value()
                 .to<std::vector<std::string>>() ==
             std::vector<std::string>({"TaskState"}));
}

void requirePortDirection(
    ::opcua::Client &client, std::uint16_t namespace_index,
    std::initializer_list<std::string_view> segments,
    RTT::opcua::PortDirection expected) {
  const auto id = modelNodeId(namespace_index, segments);
  const auto value = ::opcua::services::readValue(client, id);
  BOOST_REQUIRE(value);
  BOOST_TEST(value.value().isScalar());
  BOOST_TEST(value.value().isType(
      ::opcua::NodeId(::opcua::DataTypeId::Int32)));
  BOOST_TEST(value.value().to<std::int32_t>() ==
             static_cast<std::int32_t>(expected));
}

void requirePortValue(::opcua::Client &client, std::uint16_t namespace_index,
                      std::initializer_list<std::string_view> segments,
                      const ::opcua::NodeId &expected_data_type, bool readable,
                      bool writable) {
  const auto id = modelNodeId(namespace_index, segments);
  const auto node_class = ::opcua::services::readNodeClass(client, id);
  const auto data_type = ::opcua::services::readDataType(client, id);
  const auto value_rank = ::opcua::services::readValueRank(client, id);
  const auto access = ::opcua::services::readAccessLevel(client, id);
  const auto user_access = ::opcua::services::readUserAccessLevel(client, id);
  BOOST_REQUIRE(node_class);
  BOOST_REQUIRE(data_type);
  BOOST_REQUIRE(value_rank);
  BOOST_REQUIRE(access);
  BOOST_REQUIRE(user_access);
  BOOST_TEST(node_class.value() == ::opcua::NodeClass::Variable);
  BOOST_TEST(data_type.value() == expected_data_type);
  BOOST_TEST(value_rank.value() == ::opcua::ValueRank::Scalar);
  BOOST_TEST(access.value().anyOf(::opcua::AccessLevel::CurrentRead) ==
             readable);
  BOOST_TEST(access.value().anyOf(::opcua::AccessLevel::CurrentWrite) ==
             writable);
  BOOST_TEST(user_access.value().anyOf(::opcua::AccessLevel::CurrentRead) ==
             readable);
  BOOST_TEST(user_access.value().anyOf(::opcua::AccessLevel::CurrentWrite) ==
             writable);
}

template <typename Predicate>
bool waitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

class OccupiedLoopbackPort final {
public:
  OccupiedLoopbackPort() : socket_fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
    if (socket_fd_ == kInvalidSocket) {
      throw std::runtime_error("failed to create occupied-port socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(socket_fd_, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(socket_fd_, 1) != 0) {
      release();
      throw std::runtime_error("failed to occupy loopback port");
    }

    TestSocklen size = sizeof(address);
    if (::getsockname(socket_fd_, reinterpret_cast<sockaddr *>(&address),
                      &size) != 0) {
      release();
      throw std::runtime_error("failed to inspect occupied loopback port");
    }
    port_ = ntohs(address.sin_port);
  }

  ~OccupiedLoopbackPort() { release(); }

  OccupiedLoopbackPort(const OccupiedLoopbackPort &) = delete;
  OccupiedLoopbackPort &operator=(const OccupiedLoopbackPort &) = delete;

  std::uint16_t port() const noexcept { return port_; }

  void release() noexcept {
    if (socket_fd_ != kInvalidSocket) {
      closeTestSocket(socket_fd_);
      socket_fd_ = kInvalidSocket;
    }
  }

private:
  TestSocket socket_fd_{kInvalidSocket};
  std::uint16_t port_{0U};
};

void loadRttTypes(bool add_unsupported_type = false) {
  if (RTT::types::Types()->type("Int32") == nullptr) {
    RTT::types::RealTimeTypekitPlugin().loadTypes();
  }
  if (add_unsupported_type &&
      RTT::types::Types()->type(std::string(kUnsupportedTypeName)) == nullptr) {
    BOOST_REQUIRE(RTT::types::Types()->addType(
        new RTT::types::TemplateTypeInfo<UnsupportedValue, false>(
            std::string(kUnsupportedTypeName))));
  }
}

class EchoTask final : public RTT::TaskContext {
public:
  explicit EchoTask(const std::string &name) : RTT::TaskContext(name) {
    addOperation("echo", &EchoTask::echo, this, RTT::ClientThread);
    addProperty("Gain", gain);
  }

  void addLateResource() {
    addOperation("lateEcho", &EchoTask::lateEcho, this, RTT::ClientThread);
  }

  std::int32_t gain{7};

private:
  std::int32_t echo(std::int32_t value) { return value; }
  std::int32_t lateEcho(std::int32_t value) { return value + 1; }
};

class CompleteMappingTask final : public RTT::TaskContext {
public:
  CompleteMappingTask()
      : RTT::TaskContext("CompleteMapping"),
        control(RTT::Service::Create("control")) {
    addOperation("add", &CompleteMappingTask::add, this, RTT::ClientThread)
        .arg("left", "Left operand.")
        .arg("right", "Right operand.");
    addProperty("Gain", gain);
    addAttribute("Mode", mode);
    addConstant("Model", model);
    addPort(command);
    addEventPort(trigger);
    addPort(feedback);

    control->addOperation("scale", &CompleteMappingTask::scale, this,
                          RTT::ClientThread)
        .arg("value", "Value to scale.");
    control->addProperty("Offset", offset);
    control->addAttribute("Enabled", enabled);
    control->addConstant("Unit", unit);
    control->addPort(service_command);
    control->addPort(service_feedback);
    BOOST_REQUIRE(provides()->addService(control));
  }

  void cycle() {
    if (!getActivity() || !dynamic_cast<RTT::extras::SlaveActivity*>(getActivity()))
      setActivity(new RTT::extras::SlaveActivity(0.01));
    BOOST_REQUIRE(start());
    BOOST_REQUIRE(getActivity()->execute());
    BOOST_REQUIRE(stop());
  }

  ~CompleteMappingTask() override {
    control->removePort(service_command.getName());
    control->removePort(service_feedback.getName());
    control->clear();
  }

  std::int32_t add(std::int32_t left, std::int32_t right) const {
    return left + right;
  }

  std::int32_t scale(std::int32_t value) const { return value * 2 + offset; }

  std::int32_t gain{7};
  std::string mode{"manual"};
  std::string model{"fixture-v1"};
  RTT::InputPort<std::int32_t> command{"Command"};
  RTT::InputPort<bool> trigger{"Trigger"};
  RTT::OutputPort<std::int32_t> feedback{"Feedback"};
  RTT::Service::shared_ptr control;
  std::int32_t offset{2};
  bool enabled{false};
  std::string unit{"counts"};
  RTT::InputPort<std::int32_t> service_command{"ServiceCommand"};
  RTT::OutputPort<std::int32_t> service_feedback{"ServiceFeedback"};
};

class UnsupportedTask final : public RTT::TaskContext {
public:
  UnsupportedTask() : RTT::TaskContext("UnsupportedPeer") {
    addProperty("Value", value);
  }

private:
  UnsupportedValue value{7};
};

RTT::TaskContext *createEchoTask(std::string name) {
  return new EchoTask(std::move(name));
}

class FactoryRegistration final {
public:
  FactoryRegistration(std::string name, RTT::ComponentLoaderSignature factory)
      : name_(std::move(name)) {
    RTT::ComponentLoader::Instance()->addFactory(name_, factory);
  }

  ~FactoryRegistration() {
    auto &factories = const_cast<RTT::FactoryMap &>(
        RTT::ComponentLoader::Instance()->getFactories());
    factories.erase(name_);
  }

  FactoryRegistration(const FactoryRegistration &) = delete;
  FactoryRegistration &operator=(const FactoryRegistration &) = delete;

private:
  std::string name_;
};

class TemporarySiteFile final {
public:
  explicit TemporarySiteFile(std::uint16_t port)
      : path_(std::filesystem::temp_directory_path() /
              ("ocl-opcua-site-" + std::to_string(::getpid()) + "-" +
               std::to_string(port) + ".cpf")) {
    std::ofstream output(path_);
    output << R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE properties SYSTEM "cpf.dtd">
<properties>
  <struct name="SiteEcho" type="TestEchoTask">
    <simple name="Server" type="boolean"><value>1</value></simple>
  </struct>
</properties>
)";
    if (!output) {
      throw std::runtime_error("failed to write temporary site file");
    }
  }

  ~TemporarySiteFile() {
    std::error_code error;
    std::filesystem::remove(path_, error);
  }

  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

OCL::OpcUaDeploymentOptions
deploymentOptions(std::uint16_t port = unusedLoopbackPort()) {
  OCL::OpcUaDeploymentOptions options;
  options.server.port = port;
  options.proxy.request_timeout = std::chrono::milliseconds(500);
  return options;
}

std::vector<std::string> operationNames(RTT::Service::shared_ptr service) {
  std::vector<std::string> names = service->getNames();
  std::sort(names.begin(), names.end());
  return names;
}

const std::vector<std::string> kExpectedOpcUaOperations{
    "endpointUrl",
    "isRunning",
    "lastError",
    "publicationDiagnostics",
    "publishComponent",
    "publishComponentSelected",
    "start",
    "unsupportedResources",
};

class LifecycleProbe final : public RTT::Service,
                             public OCL::DeploymentServiceLifecycle {
public:
  LifecycleProbe(OCL::DeploymentComponent &owner, const std::string &name)
      : RTT::Service(name, &owner) {}
  bool componentLoaded(RTT::TaskContext *component) override {
    loaded.push_back(component->getName());
    return !reject_load;
  }
  bool componentCanUnload(RTT::TaskContext *) override { return true; }
  void componentUnloaded(RTT::TaskContext *component) override {
    std::erase(loaded, component->getName());
  }
  void beginDeploymentShutdown() noexcept override { began.store(true); }
  void finishDeploymentShutdown() noexcept override { finished.store(true); }

  std::vector<std::string> loaded;
  bool reject_load{false};
  std::atomic_bool began{false};
  std::atomic_bool finished{false};
};

class HookOverrideDeployer final : public OCL::DeploymentComponent {
protected:
  bool componentLoaded(RTT::TaskContext *) override { return true; }
  bool componentCanUnload(RTT::TaskContext *) override { return true; }
};

struct HeldInvocation {
  std::mutex mutex;
  std::condition_variable condition;
  bool released{false};
  std::atomic_bool entered{false};
  std::atomic_bool completed{false};
  std::atomic_bool destroyed{false};

  void release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    condition.notify_all();
  }
};

class HeldTask final : public RTT::TaskContext {
public:
  explicit HeldTask(const std::string &name) : RTT::TaskContext(name) {
    provides()->addSynchronousOperation("hold", &HeldTask::hold, this);
  }
  ~HeldTask() override { invocation->destroyed.store(true); }
  bool hold() {
    std::unique_lock<std::mutex> lock(invocation->mutex);
    invocation->entered.store(true);
    const bool released = invocation->condition.wait_for(
        lock, std::chrono::seconds(5), [&] { return invocation->released; });
    invocation->completed.store(released);
    return released;
  }
  std::shared_ptr<HeldInvocation> invocation{std::make_shared<HeldInvocation>()};
};

RTT::TaskContext *createHeldTask(std::string name) { return new HeldTask(name); }

} // namespace

BOOST_AUTO_TEST_CASE(endpoint_start_publishes_no_components) {
  loadRttTypes();
  EchoTask local("LocalEcho");
  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());

  RTT::Service::shared_ptr local_opcua = deployer.provides("opcua");
  BOOST_REQUIRE(local_opcua != nullptr);
  BOOST_TEST(operationNames(local_opcua) == kExpectedOpcUaOperations);
  BOOST_TEST(!deployer.opcUaIsRunning());
  BOOST_TEST(deployer.opcUaEndpointUrl().find("opc.tcp://127.0.0.1:") == 0U);
  BOOST_REQUIRE(deployer.addPeer(&local));
  BOOST_TEST(!deployer.publishComponent(local.getName()));
  BOOST_TEST(deployer.opcUaLastError() == "OPC UA server is not running");

  BOOST_REQUIRE(deployer.startOpcUa());
  BOOST_TEST(deployer.opcUaIsRunning());
  BOOST_TEST(deployer.opcUaLastError().empty());

  ::opcua::Client client;
  client.connect(deployer.opcUaEndpointUrl());
  const std::uint16_t namespace_index = namespaceIndex(client);
  requireMissingNode(
      client,
      modelNodeId(namespace_index, {"components", deployer.getName()}));
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", local.getName()}));
  client.disconnect();

  std::string error;
  auto deployer_proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), deployer.getName(), {}, &error);
  BOOST_TEST(deployer_proxy == nullptr);
  BOOST_TEST(!error.empty());

  error.clear();
  auto local_proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), local.getName(), {}, &error);
  BOOST_TEST(local_proxy == nullptr);
  BOOST_TEST(!error.empty());

  const std::string endpoint = deployer.opcUaEndpointUrl();
  BOOST_TEST(!deployer.publishComponent("MissingPeer"));
  BOOST_TEST(!deployer.opcUaLastError().empty());
  BOOST_TEST(deployer.startOpcUa());
  BOOST_TEST(deployer.opcUaLastError().empty());
  BOOST_TEST(deployer.opcUaEndpointUrl() == endpoint);
}

BOOST_AUTO_TEST_CASE(explicit_complete_deployer_publication) {
  loadRttTypes();
  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());
  BOOST_REQUIRE(deployer.startOpcUa());
  BOOST_REQUIRE(deployer.publishComponent(deployer.getName()));
  BOOST_TEST(deployer.publicationDiagnostics(deployer.getName()).empty());
  BOOST_TEST(deployer.unsupportedResources(deployer.getName()).empty());

  std::string error;
  auto proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), deployer.getName(), {}, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  RTT::Service::shared_ptr remote_opcua = proxy->provides("opcua");
  BOOST_REQUIRE(remote_opcua != nullptr);
  BOOST_TEST(operationNames(remote_opcua) == kExpectedOpcUaOperations);
}

BOOST_AUTO_TEST_CASE(explicit_selected_deployer_publication) {
  loadRttTypes();
  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());
  RTT::Service::shared_ptr local_opcua = deployer.provides("opcua");
  BOOST_REQUIRE(local_opcua != nullptr);
  BOOST_REQUIRE(local_opcua->getOperation("publishComponentSelected") !=
                nullptr);
  BOOST_REQUIRE(local_opcua->getOperation("publicationDiagnostics") != nullptr);

  BOOST_REQUIRE(deployer.startOpcUa());
  BOOST_REQUIRE(deployer.publishComponentSelected(
      deployer.getName(), {"services/opcua/**"}));
  BOOST_TEST(deployer.publicationDiagnostics(deployer.getName()).empty());

  std::string error;
  auto proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), deployer.getName(), {}, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  RTT::Service::shared_ptr remote_opcua = proxy->provides("opcua");
  BOOST_REQUIRE(remote_opcua != nullptr);
  BOOST_TEST(operationNames(remote_opcua) == kExpectedOpcUaOperations);
}

BOOST_AUTO_TEST_CASE(selected_peer_publication_is_sparse) {
  loadRttTypes();
  EchoTask selected("SelectedEcho");
  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());
  BOOST_REQUIRE(deployer.addPeer(&selected));
  BOOST_REQUIRE(deployer.startOpcUa());
  BOOST_REQUIRE(deployer.publishComponentSelected(selected.getName(),
                                                  {"operations/echo"}));

  std::string error;
  auto proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), selected.getName(), {}, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);
  RTT::OperationCaller<std::int32_t(std::int32_t)> echo =
      proxy->getOperation("echo");
  BOOST_REQUIRE(echo.ready());
  BOOST_TEST(echo(42) == 42);
  BOOST_TEST(proxy->provides()->getProperty("Gain") == nullptr);

  ::opcua::Client client;
  client.connect(deployer.opcUaEndpointUrl());
  const std::uint16_t namespace_index = namespaceIndex(client);
  requireMissingNode(client,
                     modelNodeId(namespace_index,
                                 {"components", selected.getName(),
                                  "properties", "Gain"}));
}

BOOST_AUTO_TEST_CASE(selected_peer_publication_reports_diagnostics_atomically) {
  loadRttTypes(true);
  EchoTask healthy("HealthyEcho");
  UnsupportedTask rejected;
  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());
  BOOST_REQUIRE(deployer.addPeer(&healthy));
  BOOST_REQUIRE(deployer.addPeer(&rejected));
  BOOST_REQUIRE(deployer.startOpcUa());
  BOOST_REQUIRE(deployer.publishComponentSelected(healthy.getName(),
                                                  {"operations/echo"}));

  std::string error;
  auto healthy_proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), healthy.getName(), {}, &error);
  BOOST_REQUIRE_MESSAGE(healthy_proxy != nullptr, error);
  RTT::OperationCaller<std::int32_t(std::int32_t)> echo =
      healthy_proxy->getOperation("echo");
  BOOST_REQUIRE(echo.ready());

  BOOST_TEST(!deployer.publishComponentSelected(
      rejected.getName(), {"properties//Value", "properties/Missing"}));
  BOOST_TEST(deployer.opcUaIsRunning());
  BOOST_TEST(deployer.opcUaLastError() ==
             "selective OPC UA publication rejected component "
             "'UnsupportedPeer' with 2 diagnostic(s)");
  const std::vector<std::string> expected_selector_diagnostics{
      "OPC UA publication: component 'UnsupportedPeer' rejected selector "
      "'properties//Value': selector contains an empty segment.",
      "OPC UA publication: component 'UnsupportedPeer' selector "
      "'properties/Missing' matched no RTT resource.",
  };
  BOOST_TEST(deployer.publicationDiagnostics(rejected.getName()) ==
             expected_selector_diagnostics);
  BOOST_TEST(deployer.unsupportedResources(rejected.getName()).empty());
  BOOST_TEST(echo(21) == 21);

  ::opcua::Client client;
  client.connect(deployer.opcUaEndpointUrl());
  const std::uint16_t namespace_index = namespaceIndex(client);
  requireMissingNode(
      client,
      modelNodeId(namespace_index, {"components", rejected.getName()}));
  client.disconnect();

  BOOST_TEST(!deployer.publishComponentSelected(rejected.getName(),
                                                {"properties/Value"}));
  const std::vector<std::string> expected_publication_diagnostics{
      "OPC UA publication: component 'UnsupportedPeer' rejected resource "
      "'properties/Value': property uses RTT type "
      "'/test/OclUnsupportedValue' which has no registered OPC UA protocol.",
  };
  const std::vector<std::string> expected_legacy_diagnostics{
      "OPC UA: component 'UnsupportedPeer' rejected property 'Value' because "
      "RTT type '/test/OclUnsupportedValue' has no registered OPC UA "
      "protocol.",
  };
  BOOST_TEST(deployer.publicationDiagnostics(rejected.getName()) ==
             expected_publication_diagnostics);
  BOOST_TEST(deployer.unsupportedResources(rejected.getName()) ==
             expected_legacy_diagnostics);
  BOOST_TEST(deployer.opcUaIsRunning());
  BOOST_TEST(echo(84) == 84);
}

BOOST_AUTO_TEST_CASE(selected_publication_is_idempotent_and_rejects_conflicts) {
  loadRttTypes();
  EchoTask selected_first("SelectedFirst");
  EchoTask full_first("FullFirst");
  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());
  BOOST_REQUIRE(deployer.addPeer(&selected_first));
  BOOST_REQUIRE(deployer.addPeer(&full_first));
  BOOST_REQUIRE(deployer.startOpcUa());

  BOOST_REQUIRE(deployer.publishComponentSelected(selected_first.getName(),
                                                  {"operations/echo"}));
  BOOST_TEST(deployer.publishComponentSelected(
      selected_first.getName(), {"operations/echo", "operations/echo"}));
  BOOST_TEST(deployer.publicationDiagnostics(selected_first.getName()).empty());

  BOOST_TEST(!deployer.publishComponent(selected_first.getName()));
  const std::vector<std::string> expected_mode_conflict{
      "OPC UA publication: component 'SelectedFirst' conflicts with its "
      "existing publication: publication mode differs from the first "
      "successful publication.",
  };
  BOOST_TEST(deployer.publicationDiagnostics(selected_first.getName()) ==
             expected_mode_conflict);
  BOOST_TEST(deployer.opcUaLastError() ==
             "RTT component publication conflict for 'SelectedFirst': "
             "publication mode differs from the first successful publication");

  BOOST_TEST(deployer.publishComponentSelected(selected_first.getName(),
                                               {"operations/echo"}));
  BOOST_TEST(deployer.publicationDiagnostics(selected_first.getName()).empty());
  BOOST_TEST(deployer.opcUaLastError().empty());

  BOOST_TEST(!deployer.publishComponentSelected(selected_first.getName(),
                                                {"properties/Gain"}));
  const std::vector<std::string> expected_selection_conflict{
      "OPC UA publication: component 'SelectedFirst' conflicts with its "
      "existing publication: selection resolves to a different effective "
      "resource set.",
  };
  BOOST_TEST(deployer.publicationDiagnostics(selected_first.getName()) ==
             expected_selection_conflict);

  BOOST_REQUIRE(deployer.publishComponent(full_first.getName()));
  BOOST_TEST(!deployer.publishComponentSelected(full_first.getName(),
                                                {"operations/echo"}));
  const std::vector<std::string> expected_reverse_mode_conflict{
      "OPC UA publication: component 'FullFirst' conflicts with its existing "
      "publication: publication mode differs from the first successful "
      "publication.",
  };
  BOOST_TEST(deployer.publicationDiagnostics(full_first.getName()) ==
             expected_reverse_mode_conflict);
}

BOOST_AUTO_TEST_CASE(strict_publication_is_static_and_idempotent) {
  loadRttTypes(true);
  EchoTask local("LocalEcho");
  CompleteMappingTask complete;
  UnsupportedTask unsupported;
  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());
  BOOST_REQUIRE(deployer.addPeer(&local));
  BOOST_REQUIRE(deployer.addPeer(&complete));
  BOOST_REQUIRE(deployer.addPeer(&unsupported));
  BOOST_REQUIRE(deployer.startOpcUa());

  BOOST_TEST(!deployer.publishComponent("MissingPeer"));
  BOOST_TEST(deployer.opcUaLastError() ==
             "no such local RTT component: MissingPeer");
  BOOST_REQUIRE(deployer.publishComponent(local.getName()));

  std::string error;
  auto local_proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), local.getName(), {}, &error);
  BOOST_REQUIRE_MESSAGE(local_proxy != nullptr, error);
  RTT::OperationCaller<std::int32_t(std::int32_t)> echo =
      local_proxy->getOperation("echo");
  BOOST_REQUIRE(echo.ready());
  BOOST_TEST(echo(42) == 42);
  auto *gain = dynamic_cast<RTT::Property<std::int32_t> *>(
      local_proxy->provides()->getProperty("Gain"));
  BOOST_REQUIRE(gain != nullptr);
  gain->set(9);
  BOOST_TEST(local.gain == 9);

  BOOST_REQUIRE(deployer.publishComponent(complete.getName()));
  ::opcua::Client client;
  client.connect(deployer.opcUaEndpointUrl());
  const std::uint16_t namespace_index = namespaceIndex(client);
  requireMissingNode(
      client,
      modelNodeId(namespace_index,
                  {"components", "CompleteMapping", "lifecycleState"}));
  requireTaskStateMethod(client, namespace_index, complete.getName(),
                         "getTaskState", 4);
  requireTaskStateMethod(client, namespace_index, complete.getName(),
                         "getTargetState", 4);
  requirePortDirection(
      client, namespace_index,
      {"components", "CompleteMapping", "ports", "Command", "direction"},
      RTT::opcua::PortDirection::input);
  requirePortDirection(
      client, namespace_index,
      {"components", "CompleteMapping", "ports", "Trigger", "direction"},
      RTT::opcua::PortDirection::input);
  requirePortDirection(
      client, namespace_index,
      {"components", "CompleteMapping", "ports", "Feedback", "direction"},
      RTT::opcua::PortDirection::output);
  requirePortDirection(
      client, namespace_index,
      {"components", "CompleteMapping", "services", "control", "ports",
       "ServiceCommand", "direction"},
      RTT::opcua::PortDirection::input);
  requirePortDirection(
      client, namespace_index,
      {"components", "CompleteMapping", "services", "control", "ports",
       "ServiceFeedback", "direction"},
      RTT::opcua::PortDirection::output);
  requirePortValue(
      client, namespace_index,
      {"components", "CompleteMapping", "ports", "Command", "value"},
      ::opcua::NodeId(::opcua::DataTypeId::Int32), true, true);
  requirePortValue(
      client, namespace_index,
      {"components", "CompleteMapping", "ports", "Trigger", "value"},
      ::opcua::NodeId(::opcua::DataTypeId::Boolean), true, true);
  requirePortValue(
      client, namespace_index,
      {"components", "CompleteMapping", "ports", "Feedback", "value"},
      ::opcua::NodeId(::opcua::DataTypeId::Int32), true, false);
  requirePortValue(client, namespace_index,
                   {"components", "CompleteMapping", "services", "control",
                    "ports", "ServiceCommand", "value"},
                   ::opcua::NodeId(::opcua::DataTypeId::Int32), true, true);
  requirePortValue(client, namespace_index,
                   {"components", "CompleteMapping", "services", "control",
                    "ports", "ServiceFeedback", "value"},
                   ::opcua::NodeId(::opcua::DataTypeId::Int32), true, false);
  for (const std::initializer_list<std::string_view> method_path : {
           std::initializer_list<std::string_view>{
               "components", "CompleteMapping", "ports", "Command", "write"},
           std::initializer_list<std::string_view>{
               "components", "CompleteMapping", "ports", "Trigger", "write"},
           std::initializer_list<std::string_view>{
               "components", "CompleteMapping", "ports", "Feedback", "read"},
           std::initializer_list<std::string_view>{
               "components", "CompleteMapping", "services", "control", "ports",
               "ServiceCommand", "write"},
           std::initializer_list<std::string_view>{
               "components", "CompleteMapping", "services", "control", "ports",
               "ServiceFeedback", "read"},
       }) {
    requireMissingNode(client, modelNodeId(namespace_index, method_path));
  }
  BOOST_REQUIRE(::opcua::services::readNodeClass(
      client,
      modelNodeId(namespace_index, {"components", "CompleteMapping", "services",
                                    "Command", "operations"})));
  BOOST_REQUIRE(::opcua::services::readNodeClass(
      client,
      modelNodeId(namespace_index, {"components", "CompleteMapping", "services",
                                    "Command", "operations", "status"})));
  for (const std::string_view category :
       {"properties", "attributes", "ports", "services"}) {
    requireMissingNode(client, modelNodeId(namespace_index,
                                           {"components", "CompleteMapping",
                                            "services", "Command", category}));
  }

  const auto command_value_id =
      modelNodeId(namespace_index, {"components", "CompleteMapping", "ports",
                                    "Command", "value"});
  const auto initial_command =
      ::opcua::services::readValue(client, command_value_id);
  BOOST_REQUIRE(!initial_command);
  BOOST_TEST(initial_command.code() == UA_STATUSCODE_BADWAITINGFORINITIALDATA);

  BOOST_TEST(::opcua::services::writeValue(client, command_value_id,
                                           ::opcua::Variant(std::int32_t{61}))
                 .isGood());
  std::int32_t direct_command_value = 0;
  BOOST_REQUIRE(waitUntil([&] {
    complete.cycle();
    direct_command_value = complete.command.data();
    return complete.command.status() == RTT::NewData;
  }));
  BOOST_TEST(direct_command_value == 61);
  BOOST_TEST(::opcua::services::readValue(client, command_value_id)
                 .value()
                 .to<std::int32_t>() == 61);
  BOOST_TEST(::opcua::services::writeValue(client, command_value_id,
                                           ::opcua::Variant(std::int32_t{61}))
                 .isGood());
  BOOST_REQUIRE(waitUntil([&] {
    complete.cycle();
    direct_command_value = complete.command.data();
    return complete.command.status() == RTT::NewData;
  }));
  BOOST_TEST(direct_command_value == 61);

  complete.feedback.data() = 81; complete.cycle();
  complete.feedback.data() = 82; complete.cycle();
  complete.feedback.data() = 84; complete.cycle();
  const auto feedback_value_id =
      modelNodeId(namespace_index, {"components", "CompleteMapping", "ports",
                                    "Feedback", "value"});
  const auto direct_feedback =
      ::opcua::services::readValue(client, feedback_value_id);
  BOOST_REQUIRE(direct_feedback);
  BOOST_TEST(direct_feedback.value().to<std::int32_t>() == 84);
  client.disconnect();

  auto complete_proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), complete.getName(), {}, &error);
  BOOST_REQUIRE_MESSAGE(complete_proxy != nullptr, error);
  BOOST_TEST(complete_proxy->ready());
  BOOST_TEST(complete_proxy->getTaskState() == complete.getTaskState());
  BOOST_TEST(complete_proxy->getTargetState() == complete.getTargetState());
  BOOST_TEST(complete_proxy->isConfigured() == complete.isConfigured());
  BOOST_TEST(complete_proxy->isActive() == complete.isActive());
  BOOST_TEST(complete_proxy->isRunning() == complete.isRunning());
  BOOST_TEST(complete_proxy->inFatalError() == complete.inFatalError());
  BOOST_TEST(complete_proxy->inException() == complete.inException());
  BOOST_TEST(complete_proxy->inRunTimeError() == complete.inRunTimeError());

  RTT::OperationCaller<std::int32_t(std::int32_t, std::int32_t)> add =
      complete_proxy->getOperation("add");
  BOOST_REQUIRE(add.ready());
  BOOST_TEST(add(20, 22) == 42);
  auto *complete_gain = dynamic_cast<RTT::Property<std::int32_t> *>(
      complete_proxy->provides()->getProperty("Gain"));
  BOOST_REQUIRE(complete_gain != nullptr);
  complete_gain->set(11);
  BOOST_TEST(complete.gain == 11);
  RTT::base::AttributeBase *mode =
      complete_proxy->provides()->getAttribute("Mode");
  BOOST_REQUIRE(mode != nullptr);
  auto *mode_source = RTT::internal::AssignableDataSource<std::string>::narrow(
      mode->getDataSource().get());
  BOOST_REQUIRE(mode_source != nullptr);
  mode_source->set("automatic");
  BOOST_TEST(complete.mode == "automatic");
  RTT::base::AttributeBase *model =
      complete_proxy->provides()->getAttribute("Model");
  BOOST_REQUIRE(model != nullptr);
  BOOST_TEST(!model->getDataSource()->isAssignable());
  auto *model_source = RTT::internal::DataSource<std::string>::narrow(
      model->getDataSource().get());
  BOOST_REQUIRE(model_source != nullptr);
  BOOST_TEST(model_source->get() == "fixture-v1");

  auto *remote_command = dynamic_cast<RTT::base::InputPortInterface *>(
      complete_proxy->ports()->getPort("Command"));
  auto *remote_feedback = dynamic_cast<RTT::base::OutputPortInterface *>(
      complete_proxy->ports()->getPort("Feedback"));
  BOOST_REQUIRE(remote_command != nullptr);
  BOOST_REQUIRE(remote_feedback != nullptr);
  BOOST_REQUIRE(complete_proxy->ports()->getPort("Trigger") != nullptr);
  RTT::Service::shared_ptr command_service =
      complete_proxy->provides()->getService("Command");
  RTT::Service::shared_ptr trigger_service =
      complete_proxy->provides()->getService("Trigger");
  RTT::Service::shared_ptr feedback_service =
      complete_proxy->provides()->getService("Feedback");
  BOOST_REQUIRE(command_service);
  BOOST_REQUIRE(trigger_service);
  BOOST_REQUIRE(feedback_service);
  BOOST_REQUIRE(command_service->getOperation("status") != nullptr);
  BOOST_REQUIRE(trigger_service->getOperation("status") != nullptr);
  BOOST_REQUIRE(feedback_service->getOperation("snapshot") != nullptr);

  RTT::OutputPort<std::int32_t> command_source("CommandSource");
  BOOST_REQUIRE(command_source.createConnection(
      *remote_command,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  BOOST_TEST(RTT::internal::PortDataAccess::publish(command_source, std::int32_t{73}) == RTT::WriteSuccess);
  std::int32_t command_value = 0;
  BOOST_REQUIRE(waitUntil(
      [&] { complete.cycle(); command_value = complete.command.data();
            return complete.command.status() == RTT::NewData; }));
  BOOST_TEST(command_value == 73);

  RTT::InputPort<std::int32_t> feedback_sink("FeedbackSink");
  BOOST_REQUIRE(remote_feedback->createConnection(
      feedback_sink, RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  std::int32_t feedback_value = 0;
  BOOST_REQUIRE(waitUntil(
      [&] { return RTT::internal::PortDataAccess::receive(feedback_sink, feedback_value) == RTT::NewData; }));
  BOOST_TEST(feedback_value == 84);
  RTT::OperationCaller<std::int32_t()> last =
      feedback_service->getOperation("snapshot");
  BOOST_REQUIRE(last.ready());
  BOOST_TEST(last() == 84);

  RTT::Service::shared_ptr control =
      complete_proxy->provides()->getService("control");
  BOOST_REQUIRE(control);
  RTT::OperationCaller<std::int32_t(std::int32_t)> scale =
      control->getOperation("scale");
  BOOST_REQUIRE(scale.ready());
  auto *offset = dynamic_cast<RTT::Property<std::int32_t> *>(
      control->getProperty("Offset"));
  BOOST_REQUIRE(offset != nullptr);
  offset->set(3);
  BOOST_TEST(complete.offset == 3);
  BOOST_TEST(scale(10) == 23);
  RTT::base::AttributeBase *enabled = control->getAttribute("Enabled");
  BOOST_REQUIRE(enabled != nullptr);
  auto *enabled_source = RTT::internal::AssignableDataSource<bool>::narrow(
      enabled->getDataSource().get());
  BOOST_REQUIRE(enabled_source != nullptr);
  enabled_source->set(true);
  BOOST_TEST(complete.enabled);
  RTT::base::AttributeBase *unit = control->getAttribute("Unit");
  BOOST_REQUIRE(unit != nullptr);
  BOOST_TEST(!unit->getDataSource()->isAssignable());
  BOOST_REQUIRE(control->getPort("ServiceCommand") != nullptr);
  auto *remote_service_feedback =
      dynamic_cast<RTT::base::OutputPortInterface *>(
          control->getPort("ServiceFeedback"));
  BOOST_REQUIRE(remote_service_feedback != nullptr);
  BOOST_REQUIRE(control->getService("ServiceCommand"));
  BOOST_REQUIRE(control->getService("ServiceFeedback"));
  RTT::InputPort<std::int32_t> service_feedback_sink("ServiceFeedbackSink");
  BOOST_REQUIRE(remote_service_feedback->createConnection(
      service_feedback_sink,
      RTT::ConnPolicy::data(RTT::ConnPolicy::LOCK_FREE, false)));
  complete.service_feedback.data() = 91; complete.cycle();
  std::int32_t service_feedback_value = 0;
  BOOST_REQUIRE(waitUntil([&] {
    return RTT::internal::PortDataAccess::receive(service_feedback_sink, service_feedback_value) == RTT::NewData;
  }));
  BOOST_TEST(service_feedback_value == 91);

  service_feedback_sink.disconnect();
  feedback_sink.disconnect();
  command_source.disconnect();

  local.addLateResource();
  BOOST_TEST(deployer.publishComponent(local.getName()));
  BOOST_REQUIRE_MESSAGE(local_proxy->synchronize(&error), error);
  BOOST_TEST(!local_proxy->provides()->hasMember("lateEcho"));

  BOOST_TEST(!deployer.publishComponent(unsupported.getName()));
  BOOST_TEST(deployer.opcUaLastError() ==
             "strict OPC UA publication rejected component 'UnsupportedPeer'");
  const std::vector<std::string> expected_diagnostics{
      "OPC UA: component 'UnsupportedPeer' rejected property 'Value' because "
      "RTT type '/test/OclUnsupportedValue' has no registered OPC UA "
      "protocol."};
  BOOST_TEST(deployer.unsupportedResources(unsupported.getName()) ==
             expected_diagnostics);
  auto unsupported_proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), unsupported.getName(), {}, &error);
  BOOST_TEST(unsupported_proxy == nullptr);
  BOOST_TEST(!error.empty());

  BOOST_REQUIRE(deployer.addPeer(local_proxy.get(), "RemoteAlias"));
  BOOST_TEST(!deployer.publishComponent("RemoteAlias"));
  BOOST_TEST(deployer.opcUaLastError() ==
             "refusing to publish remote OPC UA proxy: RemoteAlias");
}

BOOST_AUTO_TEST_CASE(server_metadata_does_not_auto_publish) {
  loadRttTypes();
  RTT::ComponentLoader::Instance()->addFactory("TestEchoTask", &createEchoTask);
  OCL::OpcUaDeploymentOptions options = deploymentOptions();
  TemporarySiteFile site_file(options.server.port);
  OCL::OpcUaDeploymentComponent deployer("Deployer", site_file.path().string(),
                                         options);

  BOOST_TEST(!deployer.opcUaIsRunning());
  BOOST_REQUIRE(deployer.getPeer("SiteEcho") != nullptr);
  BOOST_REQUIRE(deployer.startOpcUa());

  std::string error;
  auto site_proxy = RTT::opcua::TaskContextProxy::create(
      deployer.opcUaEndpointUrl(), "SiteEcho", {}, &error);
  BOOST_TEST(site_proxy == nullptr);
  BOOST_TEST(!error.empty());

  BOOST_REQUIRE(deployer.publishComponent("SiteEcho"));
  site_proxy = RTT::opcua::TaskContextProxy::create(deployer.opcUaEndpointUrl(),
                                                    "SiteEcho", {}, &error);
  BOOST_REQUIRE_MESSAGE(site_proxy != nullptr, error);
  RTT::OperationCaller<std::int32_t(std::int32_t)> echo =
      site_proxy->getOperation("echo");
  BOOST_REQUIRE(echo.ready());
  BOOST_TEST(echo(84) == 84);
}

BOOST_AUTO_TEST_CASE(failed_start_freezes_registry_and_can_retry) {
  loadRttTypes();
  OccupiedLoopbackPort occupied;
  OCL::OpcUaDeploymentComponent deployer("Deployer", "",
                                         deploymentOptions(occupied.port()));

  BOOST_TEST(!RTT::opcua::dataTypeRegistryFrozen());
  BOOST_TEST(!deployer.startOpcUa());
  BOOST_TEST(!deployer.opcUaIsRunning());
  BOOST_TEST(RTT::opcua::dataTypeRegistryFrozen());
  const std::string startup_error = deployer.opcUaLastError();
  BOOST_TEST(!startup_error.empty());
  BOOST_TEST(deployer.opcUaLastError() == startup_error);

  occupied.release();
  BOOST_REQUIRE(deployer.startOpcUa());
  BOOST_TEST(deployer.opcUaIsRunning());
  BOOST_TEST(RTT::opcua::dataTypeRegistryFrozen());
  BOOST_TEST(deployer.opcUaLastError().empty());
}

BOOST_AUTO_TEST_CASE(remote_components_remain_aliased_client_peers) {
  loadRttTypes();
  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());
  BOOST_REQUIRE(deployer.startOpcUa());

  RTT::opcua::ServerOptions remote_options;
  remote_options.port = unusedLoopbackPort();
  RTT::opcua::Server remote_server(remote_options);
  std::string error;
  BOOST_REQUIRE_MESSAGE(remote_server.start(&error), error);
  EchoTask remote("RemoteEcho");
  RTT::opcua::ObjectModel remote_model(remote_server);
  BOOST_REQUIRE_MESSAGE(remote_model.publishComponent(remote, &error), error);

  BOOST_REQUIRE(deployer.connectRemote(remote_server.endpointUrl(),
                                       remote.getName(), "RemoteAlias"));
  RTT::TaskContext *peer = deployer.getPeer("RemoteAlias");
  BOOST_REQUIRE(peer != nullptr);
  BOOST_TEST(peer->getName() == remote.getName());
  BOOST_REQUIRE(dynamic_cast<RTT::opcua::TaskContextProxy *>(peer) != nullptr);
  RTT::OperationCaller<std::int32_t(std::int32_t)> echo =
      peer->getOperation("echo");
  BOOST_REQUIRE(echo.ready());
  BOOST_TEST(echo(7) == 7);

  BOOST_TEST(deployer.connectRemote(remote_server.endpointUrl(),
                                    remote.getName(), "RemoteAlias"));
  BOOST_TEST(deployer.synchronizeRemote("RemoteAlias"));
  BOOST_TEST(!deployer.publishComponent("RemoteAlias"));
  BOOST_TEST(!deployer.publishComponentSelected("RemoteAlias",
                                                {"operations/echo"}));
  BOOST_TEST(deployer.opcUaLastError() ==
             "refusing to publish remote OPC UA proxy: RemoteAlias");
  BOOST_TEST(deployer.publicationDiagnostics("RemoteAlias").empty());
  BOOST_REQUIRE(deployer.disconnectRemote("RemoteAlias"));
  BOOST_TEST(deployer.getPeer("RemoteAlias") == nullptr);
  BOOST_TEST(!deployer.disconnectRemote("RemoteAlias"));
  BOOST_TEST(!deployer.opcUaLastError().empty());
  remote_server.stop();
}

BOOST_AUTO_TEST_CASE(published_component_unload_is_rejected) {
  loadRttTypes();
  FactoryRegistration factory("TestEchoTask", &createEchoTask);

  OCL::OpcUaDeploymentComponent deployer("Deployer", "", deploymentOptions());
  BOOST_REQUIRE(deployer.loadComponent("DisposableEcho", "TestEchoTask"));
  BOOST_REQUIRE(deployer.unloadComponent("DisposableEcho"));
  BOOST_TEST(deployer.getPeer("DisposableEcho") == nullptr);

  BOOST_REQUIRE(deployer.loadComponent("ManagedEcho", "TestEchoTask"));
  RTT::TaskContext *const managed = deployer.getPeer("ManagedEcho");
  BOOST_REQUIRE(managed != nullptr);
  BOOST_REQUIRE(deployer.startOpcUa());
  BOOST_REQUIRE(deployer.publishComponentSelected("ManagedEcho",
                                                  {"operations/echo"}));

  std::string error;
  auto proxy = RTT::opcua::TaskContextProxy::create(deployer.opcUaEndpointUrl(),
                                                    "ManagedEcho", {}, &error);
  BOOST_REQUIRE_MESSAGE(proxy != nullptr, error);

  BOOST_REQUIRE(!deployer.unloadComponent("ManagedEcho"));
  BOOST_TEST(deployer.opcUaLastError() ==
             "Cannot unload component 'ManagedEcho': it is published through "
             "OPC UA");
  BOOST_TEST(deployer.getPeer("ManagedEcho") == managed);

  RTT::OperationCaller<std::int32_t(std::int32_t)> echo =
      proxy->getOperation("echo");
  BOOST_REQUIRE(echo.ready());
  BOOST_TEST(echo(21) == 21);
}

BOOST_AUTO_TEST_CASE(plugin_is_component_owned_and_rejects_duplicate_loads) {
  auto loader = RTT::plugin::PluginLoader::Instance();
  BOOST_REQUIRE(loader->loadLibrary(OCL_TEST_OPCUA_PLUGIN));
  BOOST_TEST(!RTT::internal::GlobalService::Instance()->hasService("opcua"));
  BOOST_TEST(!loader->loadService("opcua", nullptr));
  BOOST_TEST(!RTT::internal::GlobalService::Instance()->hasService("opcua"));
  RTT::TaskContext other("Other");
  BOOST_TEST(!loader->loadService("opcua", &other));
  BOOST_TEST(!other.provides()->hasService("opcua"));

  OCL::DeploymentComponent deployer;
  BOOST_REQUIRE(deployer.loadService("Deployer", "opcua"));
  auto service = deployer.provides()->getService("opcua");
  auto *opcua = dynamic_cast<OCL::OpcUaDeploymentService *>(service.get());
  BOOST_REQUIRE(opcua);
  BOOST_TEST(service->getOwner() == &deployer);
  BOOST_TEST(operationNames(service) == kExpectedOpcUaOperations);
  BOOST_TEST(opcua->opcUaEndpointUrl() == "opc.tcp://0.0.0.0:4840/rtt");
  BOOST_TEST(!opcua->opcUaIsRunning());
  BOOST_TEST(!deployer.loadService("Deployer", "opcua"));
  BOOST_TEST(!loader->loadService("opcua", &deployer));
  BOOST_TEST(deployer.provides()->getService("opcua").get() == service.get());

  OCL::OpcUaDeploymentComponent compatibility("Compatibility", "", deploymentOptions());
  BOOST_REQUIRE(compatibility.startOpcUa());
  const auto original = compatibility.provides()->getService("opcua");
  const auto endpoint = compatibility.opcUaEndpointUrl();
  BOOST_TEST(!compatibility.loadService("Compatibility", "opcua"));
  BOOST_TEST(!loader->loadService("opcua", &compatibility));
  BOOST_TEST(compatibility.provides()->getService("opcua").get() == original.get());
  BOOST_TEST(compatibility.opcUaIsRunning());
  BOOST_TEST(compatibility.opcUaEndpointUrl() == endpoint);
}

BOOST_AUTO_TEST_CASE(ordinary_deployer_preserves_publication_and_unload_veto) {
  loadRttTypes();
  FactoryRegistration factory("TestEchoTask", &createEchoTask);
  HookOverrideDeployer deployer;
  BOOST_REQUIRE(deployer.loadComponent("Before", "TestEchoTask"));
  auto service = OCL::OpcUaDeploymentService::attach(deployer, deploymentOptions());
  auto *opcua = dynamic_cast<OCL::OpcUaDeploymentService *>(service.get());
  BOOST_REQUIRE(opcua);
  BOOST_REQUIRE(deployer.loadComponent("After", "TestEchoTask"));
  RTT::OperationCaller<bool()> start = service->getOperation("start");
  BOOST_REQUIRE(start.ready());
  BOOST_REQUIRE(start());

  ::opcua::Client client;
  client.connect(opcua->opcUaEndpointUrl());
  const auto ns = namespaceIndex(client);
  requireMissingNode(client, modelNodeId(ns, {"components", "Before"}));
  requireMissingNode(client, modelNodeId(ns, {"components", "After"}));
  BOOST_REQUIRE(opcua->publishComponent("Before"));
  BOOST_REQUIRE(opcua->publishComponentSelected("After", {"operations/echo"}));
  BOOST_TEST(!deployer.unloadComponent("Before"));
  BOOST_TEST(!deployer.unloadComponent("After"));
  std::string error;
  auto proxy = RTT::opcua::TaskContextProxy::create(
      opcua->opcUaEndpointUrl(), "After", {}, &error);
  BOOST_REQUIRE_MESSAGE(proxy, error);
  RTT::OperationCaller<std::int32_t(std::int32_t)> echo = proxy->getOperation("echo");
  BOOST_REQUIRE(echo.ready());
  BOOST_TEST(echo(42) == 42);
  client.disconnect();
  proxy.reset();
  deployer.prepareDeploymentShutdown();
  BOOST_TEST(!opcua->opcUaIsRunning());
  BOOST_TEST(!start());
  BOOST_TEST(!deployer.loadComponent("TooLate", "TestEchoTask"));
  BOOST_REQUIRE(deployer.unloadComponent("Before"));
  BOOST_REQUIRE(deployer.unloadComponent("After"));
  deployer.prepareDeploymentShutdown();
}

BOOST_AUTO_TEST_CASE(lifecycle_attachment_seeds_inventory_and_rolls_back_rejection) {
  FactoryRegistration factory("TestEchoTask", &createEchoTask);
  OCL::DeploymentComponent deployer;
  BOOST_REQUIRE(deployer.loadComponent("Before", "TestEchoTask"));
  RTT::Service::shared_ptr first(new LifecycleProbe(deployer, "first"));
  auto &probe = static_cast<LifecycleProbe &>(*first);
  BOOST_REQUIRE(deployer.attachDeploymentService("first", [&] { return first; }));
  BOOST_TEST(probe.loaded == std::vector<std::string>{"Before"});
  RTT::Service::shared_ptr second(new LifecycleProbe(deployer, "second"));
  auto &rejecting = static_cast<LifecycleProbe &>(*second);
  rejecting.reject_load = true;
  BOOST_TEST(!deployer.attachDeploymentService("second", [&] { return second; }));
  BOOST_TEST(!deployer.provides()->hasService("second"));
  BOOST_TEST(rejecting.loaded.empty());
  rejecting.reject_load = false;
  BOOST_REQUIRE(deployer.attachDeploymentService("second", [&] { return second; }));
  rejecting.reject_load = true;
  BOOST_TEST(!deployer.loadComponent("Rejected", "TestEchoTask"));
  BOOST_TEST(deployer.getPeer("Rejected") == nullptr);
  BOOST_TEST(probe.loaded == std::vector<std::string>{"Before"});
  BOOST_TEST(rejecting.loaded == std::vector<std::string>{"Before"});
  rejecting.reject_load = false;
  BOOST_REQUIRE(deployer.loadComponent("Rejected", "TestEchoTask"));
  BOOST_REQUIRE(deployer.unloadComponent("Rejected"));
  BOOST_REQUIRE(deployer.unloadComponent("Before"));
  BOOST_TEST(probe.loaded.empty());
  BOOST_TEST(rejecting.loaded.empty());
}

BOOST_AUTO_TEST_CASE(shutdown_closes_all_services_before_draining_held_operation) {
  loadRttTypes();
  FactoryRegistration factory("HeldTask", &createHeldTask);
  auto deployer = std::make_unique<OCL::DeploymentComponent>();
  auto options = deploymentOptions();
  options.object_model.operation_timeout = std::chrono::milliseconds(30);
  auto service = OCL::OpcUaDeploymentService::attach(*deployer, options);
  auto *opcua = dynamic_cast<OCL::OpcUaDeploymentService *>(service.get());
  BOOST_REQUIRE(opcua);
  RTT::Service::shared_ptr observer(new LifecycleProbe(*deployer, "observer"));
  auto &probe = static_cast<LifecycleProbe &>(*observer);
  BOOST_REQUIRE(deployer->attachDeploymentService("observer", [&] { return observer; }));
  BOOST_REQUIRE(deployer->loadComponent("Held", "HeldTask"));
  auto invocation = dynamic_cast<HeldTask *>(deployer->getPeer("Held"))->invocation;
  BOOST_REQUIRE(opcua->startOpcUa());
  BOOST_REQUIRE(opcua->publishComponentSelected("Held", {"operations/hold"}));
  ::opcua::Client client;
  client.connect(opcua->opcUaEndpointUrl());
  const auto ns = namespaceIndex(client);
  const auto result = ::opcua::services::call(
      client, modelNodeId(ns, {"components", "Held", "operations"}),
      modelNodeId(ns, {"components", "Held", "operations", "hold"}), {});
  BOOST_TEST(result.statusCode() == UA_STATUSCODE_BADTIMEOUT);
  BOOST_TEST(invocation->entered.load());
  client.disconnect();

  bool all_began = false;
  bool observer_waiting = false;
  bool component_alive = false;
  // Keep RTT destruction on the RTT-initialized main thread (also on Xenomai).
  std::jthread release([&] {
    all_began = waitUntil([&] { return probe.began.load(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    observer_waiting = !probe.finished.load();
    component_alive = !invocation->destroyed.load();
    invocation->release();
  });
  deployer.reset();
  release.join();

  BOOST_TEST(all_began);
  BOOST_TEST(observer_waiting);
  BOOST_TEST(component_alive);
  BOOST_TEST(invocation->completed.load());
  BOOST_TEST(invocation->destroyed.load());
  BOOST_TEST(probe.finished.load());
  BOOST_TEST(!opcua->opcUaIsRunning());
}

BOOST_AUTO_TEST_CASE(destructor_stops_service_when_auto_unload_is_disabled) {
  loadRttTypes();
  FactoryRegistration factory("TestEchoTask", &createEchoTask);
  auto deployer = std::make_unique<OCL::DeploymentComponent>();
  BOOST_REQUIRE(deployer->properties()->getPropertyType<bool>("AutoUnload"));
  deployer->properties()->getPropertyType<bool>("AutoUnload")->set(false);
  BOOST_REQUIRE(deployer->loadComponent("Retained", "TestEchoTask"));
  auto *component = deployer->getPeer("Retained");
  auto service = OCL::OpcUaDeploymentService::attach(*deployer, deploymentOptions());
  auto *opcua = dynamic_cast<OCL::OpcUaDeploymentService *>(service.get());
  BOOST_REQUIRE(opcua);
  BOOST_REQUIRE(opcua->startOpcUa());
  BOOST_REQUIRE(opcua->publishComponent("Retained"));
  deployer.reset();
  BOOST_TEST(!opcua->opcUaIsRunning());
  BOOST_TEST(!opcua->startOpcUa());
  BOOST_TEST(component->getName() == "Retained");
  RTT::ComponentLoader::Instance()->unloadComponent(component);
}
