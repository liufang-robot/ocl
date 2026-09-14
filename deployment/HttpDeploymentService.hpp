// SPDX-License-Identifier: LGPL-2.1-or-later
#ifndef OCL_HTTPDEPLOYMENTSERVICE_HPP
#define OCL_HTTPDEPLOYMENTSERVICE_HPP

#include "DeploymentComponent.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace OCL {
class OCL_API HttpDeploymentService : public RTT::Service,
                                      public DeploymentServiceLifecycle {
public:
  static RTT::Service::shared_ptr attach(DeploymentComponent &owner);
  ~HttpDeploymentService() override;
  bool startHttp();
  bool stopHttp();
  bool httpIsRunning() const;
  std::string httpState() const;
  std::string httpEndpointUrl() const;
  std::string httpLastError() const;
  /** Configure a whole/member input writer while the component graph is stopped. */
  bool enableInputWrite(const std::string &endpoint);
  bool disableInputWrite(const std::string &endpoint);
  bool publishComponent(const std::string &name);
  std::vector<std::string>
  publicationDiagnostics(const std::string &name) const;
  std::uint32_t pendingOperationCount() const;
  bool componentLoaded(RTT::TaskContext *) override;
  bool componentCanUnload(RTT::TaskContext *) override;
  void componentUnloaded(RTT::TaskContext *) override;
  void beginDeploymentShutdown() noexcept override;
  void finishDeploymentShutdown() noexcept override;

private:
  bool setInputWriteEnabled(const std::string &endpoint, bool enabled);
  explicit HttpDeploymentService(DeploymentComponent &owner);
  DeploymentComponent &owner_;
  class Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace OCL
#endif
