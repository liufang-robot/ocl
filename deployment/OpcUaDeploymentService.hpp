#ifndef OCL_OPCUADEPLOYMENTSERVICE_HPP
#define OCL_OPCUADEPLOYMENTSERVICE_HPP

#include "DeploymentComponent.hpp"

#include <rtt/opcua/object_model.hpp>
#include <rtt/opcua/server_options.hpp>
#include <rtt/opcua/task_context_proxy.hpp>

#include <memory>
#include <string>
#include <vector>

namespace OCL {

struct OCL_API OpcUaDeploymentOptions {
  RTT::opcua::ServerOptions server;
  RTT::opcua::ObjectModelOptions object_model;
  RTT::opcua::TaskContextProxyOptions proxy;
};

class OCL_API OpcUaDeploymentService : public RTT::Service,
                                       public DeploymentServiceLifecycle {
public:
  static RTT::Service::shared_ptr attach(DeploymentComponent &owner);
  static RTT::Service::shared_ptr attach(DeploymentComponent &owner,
                                         OpcUaDeploymentOptions options);
  ~OpcUaDeploymentService() override;

  bool startOpcUa();
  bool opcUaIsRunning() const;
  std::string opcUaEndpointUrl() const;
  std::string opcUaLastError() const;
  /** Configure a whole/member input writer while the component graph is stopped. */
  bool enableInputWrite(const std::string &endpoint);
  bool disableInputWrite(const std::string &endpoint);
  bool publishComponent(const std::string &component_name);
  bool publishComponentSelected(const std::string &component_name,
                                const std::vector<std::string> &selectors);
  std::vector<std::string>
  publicationDiagnostics(const std::string &component_name) const;
  std::vector<std::string>
  unsupportedResources(const std::string &component_name) const;

  bool connectRemote(const std::string &endpoint_url,
                     const std::string &component_name,
                     const std::string &peer_name);
  bool disconnectRemote(const std::string &peer_name);
  bool synchronizeRemote(const std::string &peer_name);

  bool componentLoaded(RTT::TaskContext *component) override;
  bool componentCanUnload(RTT::TaskContext *component) override;
  void componentUnloaded(RTT::TaskContext *component) override;
  void beginDeploymentShutdown() noexcept override;
  void finishDeploymentShutdown() noexcept override;

private:
  bool setInputWriteEnabled(const std::string &endpoint, bool enabled);
  OpcUaDeploymentService(DeploymentComponent &owner,
                         OpcUaDeploymentOptions options);
  DeploymentComponent &owner_;
  class Impl;
  std::unique_ptr<Impl> impl_;
  bool publishComponentImpl(const std::string &component_name,
                            const std::vector<std::string> *selectors);
  bool fail(const char *operation, std::string error) const;
};

} // namespace OCL

#endif
