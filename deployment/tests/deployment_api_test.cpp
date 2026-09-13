#include "deployment/DeploymentComponent.hpp"

#include <string>

template<class Deployer>
concept HasConnectMember = requires(Deployer& deployer, const std::string& path) {
    deployer.connectMember(path, path, path, path);
};
static_assert(!HasConnectMember<OCL::DeploymentComponent>,
              "connectMember was removed; connectPort accepts optional ::selectors");

int main()
{
    typedef bool (OCL::DeploymentComponent::*SetPeriodicActivityOnCPU)(
        const std::string&, double, int, int, unsigned int);

    SetPeriodicActivityOnCPU method =
        &OCL::DeploymentComponent::setPeriodicActivityOnCPU;
    bool (OCL::DeploymentComponent::*connectPort)(const std::string&, const std::string&) =
        &OCL::DeploymentComponent::connectPort;

    return method && connectPort ? 0 : 1;
}
