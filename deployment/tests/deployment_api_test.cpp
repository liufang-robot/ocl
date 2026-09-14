#include "deployment/DeploymentComponent.hpp"

#include <string>

template<class Deployer>
concept HasConnectMember = requires(Deployer& deployer, const std::string& path) {
    deployer.connectMember(path, path, path, path);
};
static_assert(!HasConnectMember<OCL::DeploymentComponent>,
              "connectMember was removed; connectPortData accepts canonical dot/index endpoints");

template<class Deployer>
concept HasConnectPort = requires(Deployer& deployer, const std::string& path) {
    deployer.connectPort(path, path);
};
static_assert(!HasConnectPort<OCL::DeploymentComponent>,
              "connectPort was renamed to connectPortData without an alias");

int main()
{
    typedef bool (OCL::DeploymentComponent::*SetPeriodicActivityOnCPU)(
        const std::string&, double, int, int, unsigned int);

    SetPeriodicActivityOnCPU method =
        &OCL::DeploymentComponent::setPeriodicActivityOnCPU;
    bool (OCL::DeploymentComponent::*connectPortData)(const std::string&, const std::string&) =
        &OCL::DeploymentComponent::connectPortData;

    bool (OCL::DeploymentComponent::*connected)(const std::string&) =
        &OCL::DeploymentComponent::isPortConnected;
    bool (OCL::DeploymentComponent::*disconnect)(const std::string&) =
        &OCL::DeploymentComponent::disconnectPort;
    std::string (OCL::DeploymentComponent::*description)(const std::string&) =
        &OCL::DeploymentComponent::getPortDescription;
    std::string (OCL::DeploymentComponent::*type)(const std::string&) =
        &OCL::DeploymentComponent::getPortType;
    int (OCL::DeploymentComponent::*direction)(const std::string&) =
        &OCL::DeploymentComponent::getPortDirection;
    return method && connectPortData && connected && disconnect && description && type && direction ? 0 : 1;
}
