#include "deployment/DeploymentComponent.hpp"

#include <string>

template<class Deployer>
concept HasConnectMember = requires(Deployer& deployer, const std::string& path) {
    deployer.connectMember(path, path, path, path);
};
static_assert(!HasConnectMember<OCL::DeploymentComponent>,
              "connectMember was removed; connectPort accepts canonical dot/index endpoints");

int main()
{
    typedef bool (OCL::DeploymentComponent::*SetPeriodicActivityOnCPU)(
        const std::string&, double, int, int, unsigned int);

    SetPeriodicActivityOnCPU method =
        &OCL::DeploymentComponent::setPeriodicActivityOnCPU;
    bool (OCL::DeploymentComponent::*connectPort)(const std::string&, const std::string&) =
        &OCL::DeploymentComponent::connectPort;

    bool (OCL::DeploymentComponent::*connected)(const std::string&) =
        &OCL::DeploymentComponent::isPortConnected;
    bool (OCL::DeploymentComponent::*disconnect)(const std::string&) =
        &OCL::DeploymentComponent::disconnectPort;
    return method && connectPort && connected && disconnect ? 0 : 1;
}
