#include "OpcUaDeploymentComponent.hpp"

#include <stdexcept>
#include <utility>

namespace OCL {

OpcUaDeploymentComponent::OpcUaDeploymentComponent(
    const std::string &name, const std::string &site_file,
    OpcUaDeploymentOptions options)
    : DeploymentComponent(name, site_file),
      service_(OpcUaDeploymentService::attach(*this, std::move(options))) {
  if (!service_) {
    throw std::runtime_error("failed to attach OPC UA deployment service");
  }
}

OpcUaDeploymentComponent::~OpcUaDeploymentComponent() {
  prepareDeploymentShutdown();
}

OpcUaDeploymentService &OpcUaDeploymentComponent::opcua() const {
  return static_cast<OpcUaDeploymentService &>(*service_);
}

bool OpcUaDeploymentComponent::startOpcUa() { return opcua().startOpcUa(); }
bool OpcUaDeploymentComponent::opcUaIsRunning() const {
  return opcua().opcUaIsRunning();
}
std::string OpcUaDeploymentComponent::opcUaEndpointUrl() const {
  return opcua().opcUaEndpointUrl();
}
std::string OpcUaDeploymentComponent::opcUaLastError() const {
  return opcua().opcUaLastError();
}
bool OpcUaDeploymentComponent::enableInputWrite(const std::string &endpoint) {
  return opcua().enableInputWrite(endpoint);
}
bool OpcUaDeploymentComponent::disableInputWrite(const std::string &endpoint) {
  return opcua().disableInputWrite(endpoint);
}
bool OpcUaDeploymentComponent::publishComponent(const std::string &name) {
  return opcua().publishComponent(name);
}
bool OpcUaDeploymentComponent::publishComponentSelected(
    const std::string &name, const std::vector<std::string> &selectors) {
  return opcua().publishComponentSelected(name, selectors);
}
std::vector<std::string> OpcUaDeploymentComponent::publicationDiagnostics(
    const std::string &name) const {
  return opcua().publicationDiagnostics(name);
}
std::vector<std::string>
OpcUaDeploymentComponent::unsupportedResources(const std::string &name) const {
  return opcua().unsupportedResources(name);
}
bool OpcUaDeploymentComponent::connectRemote(const std::string &endpoint,
                                             const std::string &component,
                                             const std::string &peer) {
  return opcua().connectRemote(endpoint, component, peer);
}
bool OpcUaDeploymentComponent::disconnectRemote(const std::string &peer) {
  return opcua().disconnectRemote(peer);
}
bool OpcUaDeploymentComponent::synchronizeRemote(const std::string &peer) {
  return opcua().synchronizeRemote(peer);
}

// Preserve subclass hooks; participant dispatch now happens in the base
// deployer.
bool OpcUaDeploymentComponent::componentLoaded(RTT::TaskContext *component) {
  return DeploymentComponent::componentLoaded(component);
}
bool OpcUaDeploymentComponent::componentCanUnload(RTT::TaskContext *component) {
  return DeploymentComponent::componentCanUnload(component);
}
void OpcUaDeploymentComponent::componentUnloaded(RTT::TaskContext *component) {
  DeploymentComponent::componentUnloaded(component);
}

} // namespace OCL
