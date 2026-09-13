#include "deployment/HttpDeploymentService.hpp"
#include <future>
#include <httplib.h>
#include <iostream>
#include <rtt/OperationCaller.hpp>
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/internal/PortDataAccess.hpp>
#include <rtt/Property.hpp>
#include <rtt/deployment/ComponentLoader.hpp>
#include <rtt/internal/DataSources.hpp>
#include <rtt/internal/GlobalService.hpp>
#include <rtt/os/main.h>
#include <rtt/plugin/PluginLoader.hpp>
#ifdef OCL_HTTP_TEST_OPCUA
#include "deployment/OpcUaDeploymentService.hpp"
#include <open62541pp/client.hpp>
#include <open62541pp/services/attribute_highlevel.hpp>
#endif

namespace {
void require(bool value, const char *message) {
  if (!value) {
    throw std::runtime_error(message);
  }
}
template <typename F> void rejects(F &&function, const char *message) {
  bool rejected = false;
  try {
    function();
  } catch (...) {
    rejected = true;
  }
  require(rejected, message);
}
std::uint32_t unusedPort() {
  httplib::Server socket;
  const auto port = socket.bind_to_any_port("127.0.0.1");
  require(port > 0, "reserve test listener port");
  socket.stop();
  return static_cast<std::uint32_t>(port);
}
template <typename T>
RTT::internal::AssignableDataSource<T> &property(RTT::Service &service,
                                                 const char *name) {
  auto *value = dynamic_cast<RTT::internal::AssignableDataSource<T> *>(
      service.getProperty(name)->getDataSource().get());
  require(value != nullptr, "HTTP property uses a canonical RTT type");
  return *value;
}
struct Hold {
  std::mutex mutex;
  std::condition_variable wake;
  bool entered{false}, released{false};
  void wait() {
    std::unique_lock lock(mutex);
    entered = true;
    wake.notify_all();
    wake.wait(lock, [&] { return released; });
  }
  void release() {
    std::lock_guard lock(mutex);
    released = true;
    wake.notify_all();
  }
};
Hold operationHold, participantHold;
std::atomic<bool> componentAlive{false};
class Application : public RTT::TaskContext {
public:
  explicit Application(const std::string &name) : RTT::TaskContext(name) {
    componentAlive = true;
    addProperty("gain", gain);
    provides("io")->addPort(command);
    provides("io")->addPort(output);
    setActivity(new RTT::extras::SlaveActivity(0.01));
    addOperation("hold", &Application::hold, this, RTT::ClientThread);
  }
  ~Application() { componentAlive = false; }
  bool hold() {
    operationHold.wait();
    return true;
  }
  int gain{11};
  RTT::InputPort<double> command{"command"};
  RTT::OutputPort<double> output{"output"};
};
RTT::TaskContext *createApplication(std::string name) {
  return new Application(name);
}
class DrainGate : public RTT::Service, public OCL::DeploymentServiceLifecycle {
public:
  explicit DrainGate(OCL::DeploymentComponent &owner)
      : RTT::Service("drainGate", &owner) {}
  bool componentLoaded(RTT::TaskContext *) override { return true; }
  bool componentCanUnload(RTT::TaskContext *) override { return true; }
  void componentUnloaded(RTT::TaskContext *) override {}
  void beginDeploymentShutdown() noexcept override {}
  void finishDeploymentShutdown() noexcept override { participantHold.wait(); }
};
} // namespace

int ORO_main(int, char **) {
  try {
    RTT::ComponentLoader::Instance()->addFactory("HttpApplication",
                                                 &createApplication);
    OCL::DeploymentComponent deployer;
    struct Release {
      ~Release() {
        participantHold.release();
        operationHold.release();
      }
    } release;
    require(deployer.loadComponent("app", "HttpApplication"),
            "load application before attaching HTTP");
    const auto loader = RTT::plugin::PluginLoader::Instance();
    require(loader->loadLibrary(OCL_TEST_HTTP_PLUGIN),
            "discover installed-style HTTP plugin");
    require(!loader->loadService("http", nullptr) &&
                !RTT::internal::GlobalService::Instance()->hasService("http"),
            "global HTTP creation is rejected");
    RTT::TaskContext ordinary("ordinary");
    require(!loader->loadService("http", &ordinary),
            "ordinary TaskContext cannot own HTTP");
    auto gate = deployer.attachDeploymentService("drainGate", [&] {
      return RTT::Service::shared_ptr(new DrainGate(deployer));
    });
    require(static_cast<bool>(gate), "attach an earlier draining participant");
    require(deployer.loadService("Deployer", "http"),
            "ordinary deployer loads HTTP service plugin");
    auto service = deployer.provides()->getService("http");
    auto *http = dynamic_cast<OCL::HttpDeploymentService *>(service.get());
    require(http != nullptr, "plugin uses shared HTTP service implementation");
    RTT::OperationCaller<bool(const std::string &)> enable = service->getOperation("enableInputWrite");
    RTT::OperationCaller<bool(const std::string &)> disable = service->getOperation("disableInputWrite");
    require(enable.ready() && disable.ready(), "HTTP exposes explicit input source configuration");
    require(!deployer.loadService("Deployer", "http") &&
                deployer.provides()->getService("http") == service,
            "duplicate preserves original service");
    auto &port = property<std::uint32_t>(*http, "port");
    auto &width = property<std::uint32_t>(*http, "workerThreads");
    port.set(unusedPort());
    width.set(2);
    property<std::uint32_t>(*http, "operationTimeoutMs").set(100);
    property<std::uint32_t>(*http, "shutdownGraceMs").set(100);
    // The guarded reference/update path is used by RTT codecs and operation
    // outputs as well as scripting's direct update/updateAction assignment.
    width.set() = 3;
    width.updated();
    require(width.get() == 3, "stopped reference/update assignment works");
    RTT::base::DataSourceBase::shared_ptr two =
        new RTT::internal::ValueDataSource<std::uint32_t>(2);
    require(width.update(two.get()) && width.get() == 2,
            "stopped erased RTT update works");
    std::unique_ptr<RTT::base::ActionInterface> assignment(
        width.updateAction(two.get()));
    require(assignment && assignment->execute(),
            "stopped RTT assignment action works");
    rejects([&] { port.set(0); }, "invalid port is rejected at assignment");
    const auto selectedPort = port.get();
#ifdef OCL_HTTP_TEST_OPCUA
    OCL::OpcUaDeploymentOptions uaOptions;
    uaOptions.server.bind_address = "127.0.0.1";
    uaOptions.server.port = static_cast<std::uint16_t>(unusedPort());
    auto uaService = OCL::OpcUaDeploymentService::attach(deployer, uaOptions);
    auto *ua = dynamic_cast<OCL::OpcUaDeploymentService *>(uaService.get());
    require(ua && ua->startOpcUa(), "OPC UA runs in the same deployer");
    ::opcua::Client uaClient;
    uaClient.connect(ua->opcUaEndpointUrl());
    port.set(uaOptions.server.port);
    require(!http->startHttp(), "HTTP bind conflict fails locally");
    require(
        static_cast<bool>(::opcua::services::readValue(
            uaClient, ::opcua::NodeId(0, UA_NS0ID_SERVER_SERVERSTATUS_STATE))),
        "OPC UA survives HTTP bind failure");
    port.set(selectedPort);
#endif
    auto &stagedWidth = width.set();
    stagedWidth = 7;
    RTT::OperationCaller<bool()> start = http->getOperation("start");
    RTT::OperationCaller<bool()> stop = http->getOperation("stop");
    require(start.ready() && stop.ready() && start(),
            "RTT local HTTP start operation");
    require(http->httpState() == "Running" && http->httpIsRunning(),
            "running state");
    rejects([&] { width.updated(); },
            "reference obtained before start cannot change active options");
    rejects([&] { width.set(8); }, "running value setter rejected");
    rejects([&] { width.update(two.get()); },
            "running erased RTT update rejected");
    rejects([&] { assignment->execute(); },
            "assignment action prepared while stopped cannot bypass guard");
    require(width.get() == 2 && http->httpLastError().find("workerThreads") !=
                                    std::string::npos,
            "configuration remains unchanged and reports property");
    httplib::Client browser("127.0.0.1", static_cast<int>(selectedPort));
    require(!http->publishComponent("Deployer"),
            "Deployer and service self-control remain unpublished");
    require(http->publishComponent("app"),
            "publish local application through HTTP");
    auto *app = dynamic_cast<Application *>(deployer.getPeer("app"));
    require(app && !app->command.connected() && !app->output.connected(),
            "HTTP publication preserves the port graph");
    require(!enable("app.io.output") && !enable("Missing.io.command") &&
                !enable("app.io.command::x"), "HTTP input source rejects output and malformed endpoints");
    require(enable("app.io.command"), "HTTP facade resolves nested input endpoint");
    auto response = browser.Post("/api/v1/components/app/services/io/ports/command/samples",
                                 "{\"value\":18}", "application/json");
    require(response && response->status == 204 && app->command.data() == 0.0,
            "HTTP acknowledgement only stages input");
    require(app->start(), "start HTTP input component");
    require(!disable("app.io.command"), "running component retains its configured writer");
    require(app->getActivity()->execute(), "acquire HTTP input in a cycle");
    require(app->stop() && app->command.data() == 18.0, "component acquired staged input");
    require(disable("app.io.command") && !app->command.connected(), "HTTP facade releases input source");
    response = browser.Get("/api/v1/components/app/properties/gain");
    require(response && response->status == 200,
            "HTTP plugin serves its application");
    require(!deployer.unloadComponent("app"),
            "published component unload rejected");
    require(stop() && !deployer.unloadComponent("app"),
            "HTTP stop retains unload protection");
    require(start(), "same service restarts");
#ifdef OCL_HTTP_TEST_OPCUA
    require(
        static_cast<bool>(::opcua::services::readValue(
            uaClient, ::opcua::NodeId(0, UA_NS0ID_SERVER_SERVERSTATUS_STATE))),
        "OPC UA survives HTTP stop/restart");
    uaClient.disconnect();
#endif
    response = browser.Post("/api/v1/components/app/operations/hold",
                            "{\"arguments\":[]}", "application/json");
    require(response && response->status == 504 &&
                http->pendingOperationCount() == 1,
            "timed-out component call is retained");
    std::future<void> shutdown;
    // Declare cleanup after the future so failure releases both gates before
    // std::future waits for the coordinated shutdown worker.
    struct ReleaseShutdown {
      ~ReleaseShutdown() {
        participantHold.release();
        operationHold.release();
      }
    } releaseShutdown;
    shutdown = std::async(std::launch::async,
                          [&] { deployer.prepareDeploymentShutdown(); });
    {
      std::unique_lock lock(participantHold.mutex);
      require(participantHold.wake.wait_for(
                  lock, std::chrono::seconds(2),
                  [&] { return participantHold.entered; }),
              "earlier participant entered final drain");
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (http->httpState() != "Stopped" &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    require(http->httpState() == "Stopped" && componentAlive,
            "HTTP network cleanup progresses while another participant drains");
    participantHold.release();
    require(shutdown.wait_for(std::chrono::milliseconds(30)) ==
                    std::future_status::timeout &&
                componentAlive,
            "component storage survives the unfinished HTTP call");
    operationHold.release();
    require(shutdown.wait_for(std::chrono::seconds(2)) ==
                std::future_status::ready,
            "released operation permits final shutdown");
    shutdown.get();
    require(http->pendingOperationCount() == 0 && !start(),
            "final shutdown drains calls and rejects restart");
    require(deployer.unloadComponent("app") && !componentAlive,
            "component unload is permitted only after final cleanup");
    std::cout << "HTTP service ownership, guarded configuration, coexistence "
                 "and deployment shutdown passed\n";
    return 0;
  } catch (const std::exception &exception) {
    std::cerr << exception.what() << '\n';
    return 1;
  }
}
