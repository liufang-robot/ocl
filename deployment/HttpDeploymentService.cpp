// SPDX-License-Identifier: LGPL-2.1-or-later
#include "HttpDeploymentService.hpp"
#include <map>
#include <rtt/Property.hpp>
#include <rtt/PropertyBag.hpp>
#include <rtt/http/server.hpp>
#include <rtt/internal/DataSources.hpp>
#include <set>
#include <thread>

namespace OCL {
namespace {
struct Configuration {
  mutable std::mutex mutex;
  RTT::http::ServerOptions values;
  std::shared_ptr<RTT::http::Server> server{
      std::make_shared<RTT::http::Server>()};
  bool starting{false};
  bool finalShutdown{false};
  std::uint64_t epoch{0};
  std::string error;
  std::set<RTT::TaskContext *> inventory;
};

// RTT assignment can call set(value), update(), updateAction(), or write a
// set() reference followed by updated(). No path returns mutable live options.
// Reference assignment is staged per local caller thread and committed only
// while still stopped in the same configuration epoch. Bare C++ reference
// writes require updated(), as in RTT's normal reference/update contract.
template <typename T>
class ConfigurationValue final : public RTT::internal::AssignableDataSource<T> {
  using Base = RTT::internal::AssignableDataSource<T>;
  struct Scratch {
    T read{};
    T write{};
    std::uint64_t epoch{};
    bool armed{false};
  };

public:
  ConfigurationValue(std::shared_ptr<Configuration> state, std::string name,
                     T RTT::http::ServerOptions::*member)
      : state_(std::move(state)), name_(std::move(name)), member_(member) {}
  T get() const override {
    std::lock_guard lock(state_->mutex);
    return state_->values.*member_;
  }
  T value() const override { return get(); }
  typename Base::const_reference_t rvalue() const override {
    auto &scratch = local();
    scratch.read = get();
    return scratch.read;
  }
  bool evaluate() const override { return true; }
  void set(typename Base::param_t value) override {
    std::lock_guard lock(state_->mutex);
    checkStopped();
    assign(value);
  }
  typename Base::reference_t set() override {
    auto &scratch = local();
    std::lock_guard lock(state_->mutex);
    checkStopped();
    scratch.write = state_->values.*member_;
    scratch.epoch = state_->epoch;
    scratch.armed = true;
    return scratch.write;
  }
  void updated() override {
    auto &scratch = local();
    std::lock_guard lock(state_->mutex);
    checkStopped();
    if (!scratch.armed || scratch.epoch != state_->epoch) {
      reject("staged assignment expired; read a new stopped-state value");
    }
    scratch.armed = false;
    assign(scratch.write);
  }
  ConfigurationValue *clone() const override {
    return new ConfigurationValue(state_, name_, member_);
  }
  ConfigurationValue *
  copy(std::map<const RTT::base::DataSourceBase *, RTT::base::DataSourceBase *>
           &copies) const override {
    auto *self = const_cast<ConfigurationValue *>(this);
    copies[this] = self;
    return self;
  }

private:
  Scratch &local() const {
    std::lock_guard lock(scratchMutex_);
    return scratch_[std::this_thread::get_id()];
  }
  [[noreturn]] void reject(const std::string &reason) const {
    state_->error = name_ + ": " + reason;
    throw std::runtime_error(state_->error);
  }
  void checkStopped() const {
    if (state_->starting || state_->finalShutdown ||
        state_->server->state() != "Stopped") {
      reject("configuration is writable only while HTTP is Stopped");
    }
  }
  void assign(typename Base::param_t value) {
    auto candidate = state_->values;
    candidate.*member_ = value;
    // Validate this field now. Certificate/key pairing is validated as a
    // complete configuration at start, allowing scripts to set fields in any
    // order.
    auto checked = candidate;
    checked.tlsEnabled = false;
    std::string error;
    if (!RTT::http::validateOptions(checked, &error)) {
      reject(error);
    }
    if constexpr (std::is_same_v<T, std::string>) {
      if (value.find('\0') != std::string::npos) {
        reject("embedded NUL is not valid configuration text");
      }
    }
    state_->values = std::move(candidate);
    ++state_->epoch;
    state_->error.clear();
  }
  std::shared_ptr<Configuration> state_;
  std::string name_;
  T RTT::http::ServerOptions::*member_;
  mutable std::mutex scratchMutex_;
  mutable std::map<std::thread::id, Scratch> scratch_;
};
template <typename T>
void addConfiguration(RTT::Service &service,
                      const std::shared_ptr<Configuration> &state,
                      const char *name, T RTT::http::ServerOptions::*member) {
  typename RTT::internal::AssignableDataSource<T>::shared_ptr source(
      new ConfigurationValue<T>(state, name, member));
  service.properties()->ownProperty(new RTT::Property<T>(
      name, "HTTP listener configuration; writable only while Stopped.",
      source));
}
} // namespace

class HttpDeploymentService::Impl {
public:
  std::shared_ptr<Configuration> configuration{
      std::make_shared<Configuration>()};
  std::mutex management;
  bool report(bool success, std::string error = {}) {
    std::lock_guard lock(configuration->mutex);
    configuration->error = success ? "" : std::move(error);
    return success;
  }
};
RTT::Service::shared_ptr
HttpDeploymentService::attach(DeploymentComponent &owner) {
  return owner.attachDeploymentService("http", [&] {
    return RTT::Service::shared_ptr(new HttpDeploymentService(owner));
  });
}
HttpDeploymentService::HttpDeploymentService(DeploymentComponent &owner)
    : RTT::Service("http", &owner), owner_(owner),
      impl_(std::make_unique<Impl>()) {
  doc("Explicit static REST publication of application components; HTTP "
      "configuration and control remain local.");
  addOperation("start", &HttpDeploymentService::startHttp, this,
               RTT::ClientThread);
  addOperation("stop", &HttpDeploymentService::stopHttp, this,
               RTT::ClientThread);
  addOperation("isRunning", &HttpDeploymentService::httpIsRunning, this,
               RTT::ClientThread);
  addOperation("state", &HttpDeploymentService::httpState, this,
               RTT::ClientThread);
  addOperation("endpointUrl", &HttpDeploymentService::httpEndpointUrl, this,
               RTT::ClientThread);
  addOperation("lastError", &HttpDeploymentService::httpLastError, this,
               RTT::ClientThread);
  addOperation("enableInputWrite", &HttpDeploymentService::enableInputWrite, this, RTT::ClientThread)
      .doc("Enable an explicit HTTP input source while the component graph is stopped.")
      .arg("endpoint", "Component-qualified whole/member input endpoint.");
  addOperation("disableInputWrite", &HttpDeploymentService::disableInputWrite, this, RTT::ClientThread)
      .doc("Release an explicit HTTP input source while the component graph is stopped.")
      .arg("endpoint", "The exact component-qualified endpoint previously enabled.");
  addOperation("publishComponent", &HttpDeploymentService::publishComponent,
               this, RTT::ClientThread)
      .arg("component", "Local application component name");
  addOperation("publicationDiagnostics",
               &HttpDeploymentService::publicationDiagnostics, this,
               RTT::ClientThread)
      .arg("component", "Published component name");
  addOperation("pendingOperationCount",
               &HttpDeploymentService::pendingOperationCount, this,
               RTT::ClientThread);
#define HTTP_CONFIGURATION(name)                                               \
  addConfiguration(*this, impl_->configuration, #name,                         \
                   &RTT::http::ServerOptions::name)
  HTTP_CONFIGURATION(bindAddress);
  HTTP_CONFIGURATION(port);
  HTTP_CONFIGURATION(tlsEnabled);
  HTTP_CONFIGURATION(certificateFile);
  HTTP_CONFIGURATION(privateKeyFile);
  HTTP_CONFIGURATION(workerThreads);
  HTTP_CONFIGURATION(maxQueuedConnections);
  HTTP_CONFIGURATION(maxPendingOperations);
  HTTP_CONFIGURATION(operationTimeoutMs);
  HTTP_CONFIGURATION(maxRequestBodyBytes);
  HTTP_CONFIGURATION(maxResponseBodyBytes);
  HTTP_CONFIGURATION(maxJsonDepth);
  HTTP_CONFIGURATION(socketReadTimeoutMs);
  HTTP_CONFIGURATION(socketWriteTimeoutMs);
  HTTP_CONFIGURATION(keepAliveTimeoutMs);
  HTTP_CONFIGURATION(keepAliveMaxRequests);
  HTTP_CONFIGURATION(shutdownGraceMs);
#undef HTTP_CONFIGURATION
}
HttpDeploymentService::~HttpDeploymentService() {
  beginDeploymentShutdown();
  finishDeploymentShutdown();
}
bool HttpDeploymentService::startHttp() {
  std::lock_guard management(impl_->management);
  auto &state = *impl_->configuration;
  RTT::http::ServerOptions options;
  {
    std::lock_guard lock(state.mutex);
    if (state.finalShutdown || owner_.deploymentShuttingDown()) {
      state.error = "HTTP deployment is shutting down";
      return false;
    }
    state.starting = true;
    ++state.epoch;
    options = state.values;
  }
  std::string error;
  bool started = false;
  try {
    started = state.server->start(options, &error);
  } catch (...) {
    error = "HTTP startup failed";
  }
  {
    std::lock_guard lock(state.mutex);
    state.starting = false;
  }
  return impl_->report(started, std::move(error));
}
bool HttpDeploymentService::stopHttp() {
  std::lock_guard management(impl_->management);
  impl_->configuration->server->stop();
  return impl_->report(true);
}
bool HttpDeploymentService::httpIsRunning() const {
  return impl_->configuration->server->isRunning();
}
std::string HttpDeploymentService::httpState() const {
  std::lock_guard lock(impl_->configuration->mutex);
  return impl_->configuration->starting ? "Starting"
                                        : impl_->configuration->server->state();
}
std::string HttpDeploymentService::httpEndpointUrl() const {
  std::lock_guard lock(impl_->configuration->mutex);
  return RTT::http::endpointUrl(impl_->configuration->values);
}
std::string HttpDeploymentService::httpLastError() const {
  std::lock_guard lock(impl_->configuration->mutex);
  return impl_->configuration->error;
}
bool HttpDeploymentService::publishComponent(const std::string &name) {
  auto deployment = owner_.lockDeployment();
  if (owner_.deploymentShuttingDown()) {
    return impl_->report(false, "HTTP deployment is shutting down");
  }
  if (name == owner_.getName()) {
    return impl_->report(false, "HTTP v1 publishes application components; "
                                "Deployer publication is unsupported");
  }
  auto *component = owner_.getPeer(name);
  if (!component) {
    return impl_->report(false, "no such local RTT component: " + name);
  }
  if (owner_.isManagedProxy(component)) {
    return impl_->report(false,
                         "HTTP v1 does not publish managed remote proxies");
  }
  std::string error;
  const bool published =
      impl_->configuration->server->publishComponent(*component, &error);
  return impl_->report(published, std::move(error));
}
bool HttpDeploymentService::enableInputWrite(const std::string &endpoint) {
  return setInputWriteEnabled(endpoint, true);
}
bool HttpDeploymentService::disableInputWrite(const std::string &endpoint) {
  return setInputWriteEnabled(endpoint, false);
}
bool HttpDeploymentService::setInputWriteEnabled(const std::string &endpoint, bool enabled) {
  auto deployment = owner_.lockDeployment();
  if (owner_.deploymentShuttingDown())
    return impl_->report(false, "HTTP deployment is shutting down");
  const auto separator = endpoint.find('.');
  if (separator == std::string::npos || separator == 0 || separator + 1 == endpoint.size())
    return impl_->report(false, "expected component-qualified input endpoint");
  const auto name = endpoint.substr(0, separator);
  auto *component = owner_.getPeer(name);
  if (!component || owner_.isManagedProxy(component))
    return impl_->report(false, "no such local application component: " + name);
  std::string error;
  const auto relative = endpoint.substr(separator + 1);
  const bool result = enabled
      ? impl_->configuration->server->enableInputWrite(*component, relative, &error)
      : impl_->configuration->server->disableInputWrite(*component, relative, &error);
  return impl_->report(result, std::move(error));
}
std::vector<std::string>
HttpDeploymentService::publicationDiagnostics(const std::string &name) const {
  return impl_->configuration->server->publicationDiagnostics(name);
}
std::uint32_t HttpDeploymentService::pendingOperationCount() const {
  return impl_->configuration->server->pendingOperationCount();
}
bool HttpDeploymentService::componentLoaded(RTT::TaskContext *component) {
  if (!component) {
    return false;
  }
  std::lock_guard lock(impl_->configuration->mutex);
  impl_->configuration->inventory.insert(component);
  return true;
}
bool HttpDeploymentService::componentCanUnload(RTT::TaskContext *component) {
  if (!impl_->configuration->server->isPublished(component)) {
    return true;
  }
  return impl_->report(false, "Cannot unload component '" +
                                  component->getName() +
                                  "': it is published through HTTP");
}
void HttpDeploymentService::componentUnloaded(RTT::TaskContext *component) {
  std::lock_guard lock(impl_->configuration->mutex);
  impl_->configuration->inventory.erase(component);
}
void HttpDeploymentService::beginDeploymentShutdown() noexcept {
  {
    std::lock_guard lock(impl_->configuration->mutex);
    impl_->configuration->finalShutdown = true;
    ++impl_->configuration->epoch;
  }
  impl_->configuration->server->beginShutdown();
}
void HttpDeploymentService::finishDeploymentShutdown() noexcept {
  impl_->configuration->server->finishShutdown();
}
} // namespace OCL
