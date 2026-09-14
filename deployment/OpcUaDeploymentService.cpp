#include "OpcUaDeploymentService.hpp"

#include <rtt/Logger.hpp>
#include <rtt/Service.hpp>
#include <rtt/opcua/datatype_registry.hpp>
#include <rtt/opcua/type_protocol.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <iterator>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace OCL {

namespace {

std::vector<std::string> diagnosticMessages(
    const std::vector<RTT::opcua::UnsupportedResource> &diagnostics) {
  std::vector<std::string> messages;
  messages.reserve(diagnostics.size());
  std::transform(diagnostics.begin(), diagnostics.end(),
                 std::back_inserter(messages),
                 [](const RTT::opcua::UnsupportedResource &resource) {
                   return resource.message();
                 });
  return messages;
}

std::vector<std::string> diagnosticMessages(
    const std::vector<RTT::opcua::PublicationDiagnostic> &diagnostics) {
  std::vector<std::string> messages;
  messages.reserve(diagnostics.size());
  std::transform(diagnostics.begin(), diagnostics.end(),
                 std::back_inserter(messages),
                 [](const RTT::opcua::PublicationDiagnostic &diagnostic) {
                   return diagnostic.message();
                 });
  return messages;
}

} // namespace

class OpcUaDeploymentService::Impl {
public:
  enum class State {
    created,
    starting,
    running,
    start_failed,
    stopping,
    destroyed,
  };

  struct RemotePeer {
    std::string endpoint_url;
    std::string component_name;
    std::unique_ptr<RTT::opcua::TaskContextProxy> proxy;
  };

  explicit Impl(OpcUaDeploymentOptions configured_options)
      : options(std::move(configured_options)), server(options.server) {}

  OpcUaDeploymentOptions options;
  RTT::opcua::Server server;
  std::unique_ptr<RTT::opcua::ObjectModel> model;
  std::map<std::string, RTT::TaskContext *, std::less<>> published;
  std::map<std::string, RemotePeer> remote_peers;
  mutable std::mutex mutex;
  std::mutex admission_mutex;
  std::atomic_bool stopping{false};
  std::once_flag shutdown_once;
  State state{State::created};
  std::string last_error;
};

RTT::Service::shared_ptr
OpcUaDeploymentService::attach(DeploymentComponent &owner) {
  OpcUaDeploymentOptions options;
  options.server.bind_address = "0.0.0.0";
  options.server.application_name = owner.getName() + " OPC UA";
  return attach(owner, std::move(options));
}

RTT::Service::shared_ptr
OpcUaDeploymentService::attach(DeploymentComponent &owner,
                               OpcUaDeploymentOptions options) {
  return owner.attachDeploymentService("opcua", [&] {
    return RTT::Service::shared_ptr(
        new OpcUaDeploymentService(owner, std::move(options)));
  });
}

OpcUaDeploymentService::OpcUaDeploymentService(DeploymentComponent &owner,
                                               OpcUaDeploymentOptions options)
    : RTT::Service("opcua", &owner), owner_(owner),
      impl_(std::make_unique<Impl>(std::move(options))) {
  auto *opcua = this;
  opcua->doc("Publishes RTT components and manages remote OPC UA peers.");
  opcua
      ->addOperation("start", &OpcUaDeploymentService::startOpcUa, this,
                     RTT::ClientThread)
      .doc("Starts the OPC UA endpoint after local package imports complete.");
  opcua
      ->addOperation("isRunning", &OpcUaDeploymentService::opcUaIsRunning, this,
                     RTT::ClientThread)
      .doc("Reports whether the OPC UA deployment server is running.");
  opcua
      ->addOperation("endpointUrl", &OpcUaDeploymentService::opcUaEndpointUrl,
                     this, RTT::ClientThread)
      .doc("Returns the OPC UA endpoint URL.");
  opcua
      ->addOperation("lastError", &OpcUaDeploymentService::opcUaLastError, this,
                     RTT::ClientThread)
      .doc("Returns the most recent OPC UA deployment error.");
  opcua
      ->addOperation("publicationDiagnostics",
                     &OpcUaDeploymentService::publicationDiagnostics, this,
                     RTT::ClientThread)
      .doc("Returns structured diagnostics from a rejected publication.")
      .arg("component", "RTT component name.");
  opcua
      ->addOperation("unsupportedResources",
                     &OpcUaDeploymentService::unsupportedResources, this,
                     RTT::ClientThread)
      .doc("Returns diagnostics from a rejected component publication.")
      .arg("component", "RTT component name.");
  opcua->addOperation("enableInputWrite", &OpcUaDeploymentService::enableInputWrite, this, RTT::ClientThread)
      .doc("Enable an explicit OPC UA input source while the component graph is stopped.")
      .arg("endpoint", "Component-qualified whole/member input endpoint.");
  opcua->addOperation("disableInputWrite", &OpcUaDeploymentService::disableInputWrite, this, RTT::ClientThread)
      .doc("Release an explicit OPC UA input source while the component graph is stopped.")
      .arg("endpoint", "The exact component-qualified endpoint previously enabled.");
  opcua
      ->addOperation("publishComponent",
                     &OpcUaDeploymentService::publishComponent, this,
                     RTT::ClientThread)
      .doc("Publishes one local RTT component on the running endpoint.")
      .arg("component", "Local RTT component name.");
  opcua
      ->addOperation("publishComponentSelected",
                     &OpcUaDeploymentService::publishComponentSelected, this,
                     RTT::ClientThread)
      .doc("Publishes selected resources of one local RTT component.")
      .arg("component", "Local RTT component name.")
      .arg("selectors", "OPC UA publication selectors.");
}

OpcUaDeploymentService::~OpcUaDeploymentService() {
  beginDeploymentShutdown();
  finishDeploymentShutdown();
}

void OpcUaDeploymentService::beginDeploymentShutdown() noexcept {
  impl_->stopping.store(true);
  {
    std::lock_guard<std::mutex> lock(impl_->admission_mutex);
    if (impl_->model) {
      impl_->model->beginShutdown();
    }
  }
  impl_->server.requestStop();
}

void OpcUaDeploymentService::finishDeploymentShutdown() noexcept {
  beginDeploymentShutdown();
  std::call_once(impl_->shutdown_once, [this] {
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->state = Impl::State::stopping;
    }
    impl_->server.stop();

    std::unique_ptr<RTT::opcua::ObjectModel> model;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      std::lock_guard<std::mutex> admission_lock(impl_->admission_mutex);
      impl_->state = Impl::State::stopping;
      model = std::move(impl_->model);
    }
    model.reset();

    std::map<std::string, Impl::RemotePeer> remote_peers;
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      remote_peers.swap(impl_->remote_peers);
      impl_->published.clear();
    }
    for (auto &[peer_name, remote] : remote_peers) {
      owner_.RTT::TaskContext::removePeer(peer_name);
      remote.proxy->disconnect();
    }
    remote_peers.clear();
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->state = Impl::State::destroyed;
    }
  });
}

bool OpcUaDeploymentService::opcUaIsRunning() const {
  if (!impl_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state == Impl::State::running && impl_->server.isRunning();
}

std::string OpcUaDeploymentService::opcUaEndpointUrl() const {
  if (!impl_) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->server.endpointUrl();
}

std::string OpcUaDeploymentService::opcUaLastError() const {
  if (!impl_) {
    return "OPC UA deployment service is unavailable";
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->last_error;
}

std::vector<std::string> OpcUaDeploymentService::unsupportedResources(
    const std::string &component_name) const {
  if (!impl_) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->model ? diagnosticMessages(
                            impl_->model->unsupportedResources(component_name))
                      : std::vector<std::string>{};
}

std::vector<std::string> OpcUaDeploymentService::publicationDiagnostics(
    const std::string &component_name) const {
  if (!impl_) {
    return {};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->model ? diagnosticMessages(impl_->model->publicationDiagnostics(
                            component_name))
                      : std::vector<std::string>{};
}

bool OpcUaDeploymentService::startOpcUa() {
  if (!impl_) {
    return false;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping.load()) {
    impl_->last_error = "OPC UA deployment service is stopping";
    return false;
  }
  if (impl_->state == Impl::State::running) {
    if (!impl_->server.isRunning()) {
      impl_->last_error = impl_->server.lastError();
      if (impl_->last_error.empty()) {
        impl_->last_error = "OPC UA server is not running";
      }
      return false;
    }
    impl_->last_error.clear();
    return true;
  }
  if (impl_->state == Impl::State::stopping ||
      impl_->state == Impl::State::destroyed) {
    impl_->last_error = "OPC UA deployment service is stopping";
    return false;
  }

  impl_->state = Impl::State::starting;
  auto fail_start = [this](std::string error) {
    impl_->state = Impl::State::start_failed;
    impl_->last_error = std::move(error);
    RTT::Logger::log().logf(RTT::Logger::Error,
                            "OpcUaDeploymentService::startOpcUa", "%s",
                            impl_->last_error.c_str());
    return false;
  };

  std::string error;
  if (!RTT::opcua::registerCanonicalTypeProtocols(&error)) {
    return fail_start(error.empty()
                          ? "failed to register canonical OPC UA types"
                          : std::move(error));
  }
  if (!RTT::opcua::freezeDataTypeRegistry(&error)) {
    return fail_start(error.empty()
                          ? "failed to freeze OPC UA datatype registry"
                          : std::move(error));
  }
  if (!impl_->server.start(&error)) {
    impl_->server.stop();
    return fail_start(error.empty() ? "failed to start OPC UA server"
                                    : std::move(error));
  }

  std::unique_ptr<RTT::opcua::ObjectModel> candidate;
  try {
    candidate = std::make_unique<RTT::opcua::ObjectModel>(
        impl_->server, impl_->options.object_model);
    {
      std::lock_guard<std::mutex> admission_lock(impl_->admission_mutex);
      if (impl_->stopping.load()) {
        candidate->beginShutdown();
        impl_->server.requestStop();
        return fail_start("OPC UA deployment service is stopping");
      }
      impl_->model = std::move(candidate);
    }
    impl_->state = Impl::State::running;
    impl_->last_error.clear();
    return true;
  } catch (const std::exception &exception) {
    impl_->server.stop();
    candidate.reset();
    return fail_start(exception.what());
  } catch (...) {
    impl_->server.stop();
    candidate.reset();
    return fail_start("failed to construct OPC UA object model");
  }
}

bool OpcUaDeploymentService::enableInputWrite(const std::string &endpoint) {
  return setInputWriteEnabled(endpoint, true);
}
bool OpcUaDeploymentService::disableInputWrite(const std::string &endpoint) {
  return setInputWriteEnabled(endpoint, false);
}
bool OpcUaDeploymentService::setInputWriteEnabled(const std::string &endpoint, bool enabled) {
  if (!impl_) return false;
  auto deployment = owner_.lockDeployment();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping.load() || owner_.deploymentShuttingDown() ||
      impl_->state != Impl::State::running || !impl_->model) {
    impl_->last_error = "OPC UA server is not running";
    return false;
  }
  const auto separator = endpoint.find('.');
  if (separator == std::string::npos || separator == 0 || separator + 1 == endpoint.size()) {
    impl_->last_error = "expected component-qualified input endpoint";
    return false;
  }
  const auto name = endpoint.substr(0, separator);
  auto *component = name == owner_.getName() ? &owner_ : owner_.getPeer(name);
  if (!component || owner_.isManagedProxy(component)) {
    impl_->last_error = "no such local RTT component: " + name;
    return false;
  }
  std::string error;
  const auto relative = endpoint.substr(separator + 1);
  const bool result = enabled
      ? impl_->model->enableInputWrite(*component, relative, &error)
      : impl_->model->disableInputWrite(*component, relative, &error);
  impl_->last_error = result ? "" : std::move(error);
  return result;
}

bool OpcUaDeploymentService::publishComponent(
    const std::string &component_name) {
  return publishComponentImpl(component_name, nullptr);
}

bool OpcUaDeploymentService::publishComponentSelected(
    const std::string &component_name,
    const std::vector<std::string> &selectors) {
  return publishComponentImpl(component_name, &selectors);
}

bool OpcUaDeploymentService::publishComponentImpl(
    const std::string &component_name,
    const std::vector<std::string> *selectors) {
  if (!impl_) {
    return false;
  }
  if (impl_->stopping.load()) {
    return false;
  }
  auto deployment_lock = owner_.lockDeployment();
  const char *operation =
      selectors == nullptr ? "OpcUaDeploymentService::publishComponent"
                           : "OpcUaDeploymentService::publishComponentSelected";
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping.load() || owner_.deploymentShuttingDown() ||
      impl_->state != Impl::State::running || !impl_->model) {
    impl_->last_error = "OPC UA server is not running";
    RTT::Logger::log().logf(RTT::Logger::Error, operation, "%s",
                            impl_->last_error.c_str());
    return false;
  }

  RTT::TaskContext *component = component_name == owner_.getName()
                                    ? &owner_
                                    : owner_.getPeer(component_name);
  if (component == nullptr) {
    impl_->last_error = "no such local RTT component: " + component_name;
    RTT::Logger::log().logf(RTT::Logger::Error, operation, "%s",
                            impl_->last_error.c_str());
    return false;
  }
  if (dynamic_cast<RTT::opcua::TaskContextProxy *>(component) != nullptr) {
    impl_->last_error =
        "refusing to publish remote OPC UA proxy: " + component_name;
    RTT::Logger::log().logf(RTT::Logger::Error, operation, "%s",
                            impl_->last_error.c_str());
    return false;
  }

  std::string error;
  const bool published =
      selectors == nullptr ? impl_->model->publishComponent(*component, &error)
                           : impl_->model->publishComponentSelected(
                                 *component, *selectors, &error);
  if (!published) {
    impl_->last_error =
        error.empty() ? "failed to publish RTT component" : std::move(error);
    RTT::Logger::log().logf(RTT::Logger::Error, operation, "%s",
                            impl_->last_error.c_str());
    return false;
  }

  impl_->published.insert_or_assign(component_name, component);
  impl_->last_error.clear();
  return true;
}

bool OpcUaDeploymentService::connectRemote(
    const std::string &endpoint_url, const std::string &component_name,
    const std::string &requested_peer_name) {
  if (impl_->stopping.load()) {
    return false;
  }
  auto deployment_lock = owner_.lockDeployment();
  if (endpoint_url.empty()) {
    return fail("connectRemote", "endpoint URL must not be empty");
  }
  if (component_name.empty()) {
    return fail("connectRemote", "remote component name must not be empty");
  }
  const std::string peer_name =
      requested_peer_name.empty() ? component_name : requested_peer_name;

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping.load()) {
    return false;
  }
  const auto existing = impl_->remote_peers.find(peer_name);
  if (existing != impl_->remote_peers.end()) {
    if (existing->second.endpoint_url != endpoint_url ||
        existing->second.component_name != component_name) {
      impl_->last_error =
          "peer name is already used by another remote: " + peer_name;
      RTT::Logger::log().logf(RTT::Logger::Error,
                              "OpcUaDeploymentService::connectRemote", "%s",
                              impl_->last_error.c_str());
      return false;
    }
    std::string error;
    if (!existing->second.proxy->synchronize(&error)) {
      impl_->last_error = std::move(error);
      return false;
    }
    impl_->last_error.clear();
    return true;
  }
  if (owner_.getPeer(peer_name) != nullptr) {
    impl_->last_error = "peer name is already in use: " + peer_name;
    RTT::Logger::log().logf(RTT::Logger::Error,
                            "OpcUaDeploymentService::connectRemote", "%s",
                            impl_->last_error.c_str());
    return false;
  }

  std::string error;
  auto proxy = RTT::opcua::TaskContextProxy::create(
      endpoint_url, component_name, impl_->options.proxy, &error);
  if (!proxy) {
    impl_->last_error =
        error.empty() ? "failed to create remote proxy" : std::move(error);
    RTT::Logger::log().logf(RTT::Logger::Error,
                            "OpcUaDeploymentService::connectRemote", "%s",
                            impl_->last_error.c_str());
    return false;
  }

  auto [inserted, was_inserted] = impl_->remote_peers.emplace(
      peer_name,
      Impl::RemotePeer{endpoint_url, component_name, std::move(proxy)});
  if (!was_inserted || !owner_.RTT::TaskContext::addPeer(
                           inserted->second.proxy.get(), peer_name)) {
    impl_->remote_peers.erase(peer_name);
    impl_->last_error = "failed to add remote deployer peer: " + peer_name;
    RTT::Logger::log().logf(RTT::Logger::Error,
                            "OpcUaDeploymentService::connectRemote", "%s",
                            impl_->last_error.c_str());
    return false;
  }

  impl_->last_error.clear();
  return true;
}

bool OpcUaDeploymentService::disconnectRemote(const std::string &peer_name) {
  if (impl_->stopping.load()) {
    return false;
  }
  auto deployment_lock = owner_.lockDeployment();
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping.load()) {
    return false;
  }
  const auto found = impl_->remote_peers.find(peer_name);
  if (found == impl_->remote_peers.end()) {
    impl_->last_error = "no such OPC UA remote peer: " + peer_name;
    RTT::Logger::log().logf(RTT::Logger::Error,
                            "OpcUaDeploymentService::disconnectRemote", "%s",
                            impl_->last_error.c_str());
    return false;
  }

  owner_.RTT::TaskContext::removePeer(peer_name);
  found->second.proxy->disconnect();
  impl_->remote_peers.erase(found);
  impl_->last_error.clear();
  return true;
}

bool OpcUaDeploymentService::synchronizeRemote(const std::string &peer_name) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->stopping.load()) {
    return false;
  }
  const auto found = impl_->remote_peers.find(peer_name);
  if (found == impl_->remote_peers.end()) {
    impl_->last_error = "no such OPC UA remote peer: " + peer_name;
    RTT::Logger::log().logf(RTT::Logger::Error,
                            "OpcUaDeploymentService::synchronizeRemote", "%s",
                            impl_->last_error.c_str());
    return false;
  }

  std::string error;
  if (!found->second.proxy->synchronize(&error)) {
    impl_->last_error =
        error.empty() ? "failed to synchronize remote peer" : std::move(error);
    RTT::Logger::log().logf(RTT::Logger::Error,
                            "OpcUaDeploymentService::synchronizeRemote", "%s",
                            impl_->last_error.c_str());
    return false;
  }
  impl_->last_error.clear();
  return true;
}

bool OpcUaDeploymentService::componentLoaded(RTT::TaskContext *component) {
  if (component == nullptr) {
    return false;
  }
  if (dynamic_cast<RTT::opcua::TaskContextProxy *>(component) != nullptr) {
    return owner_.markManagedProxy(component);
  }
  return true;
}

bool OpcUaDeploymentService::componentCanUnload(RTT::TaskContext *component) {
  if (!impl_ || component == nullptr) {
    return true;
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto published = impl_->published.find(component->getName());
  if (published == impl_->published.end() || published->second != component) {
    return true;
  }

  impl_->last_error = "Cannot unload component '" + component->getName() +
                      "': it is published through OPC UA";
  RTT::Logger::log().logf(RTT::Logger::Error,
                          "OpcUaDeploymentService::componentCanUnload", "%s",
                          impl_->last_error.c_str());
  return false;
}

void OpcUaDeploymentService::componentUnloaded(RTT::TaskContext *) {}

bool OpcUaDeploymentService::fail(const char *operation,
                                  std::string error) const {
  if (!impl_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->last_error = std::move(error);
  RTT::Logger::log().logf(RTT::Logger::Error, operation, "%s",
                          impl_->last_error.c_str());
  return false;
}

} // namespace OCL
