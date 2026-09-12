/***************************************************************************
  tag: Peter Soetens  Thu Jul 3 15:34:40 CEST 2008  DeploymentComponent.cpp

                        DeploymentComponent.cpp -  description
                           -------------------
    begin                : Thu July 03 2008
    copyright            : (C) 2008 Peter Soetens
    email                : peter.soetens@fmtc.be

 ***************************************************************************
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Lesser General Public            *
 *   License as published by the Free Software Foundation; either          *
 *   version 2.1 of the License, or (at your option) any later version.    *
 *                                                                         *
 *   This library is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU     *
 *   Lesser General Public License for more details.                       *
 *                                                                         *
 *   You should have received a copy of the GNU Lesser General Public      *
 *   License along with this library; if not, write to the Free Software   *
 *   Foundation, Inc., 59 Temple Place,                                    *
 *   Suite 330, Boston, MA  02111-1307  USA                                *
 *                                                                         *
 ***************************************************************************/



#include <rtt/RTT.hpp>
#include "DeploymentComponent.hpp"
#include <rtt/deployment/ComponentLoader.hpp>
#include <rtt/extras/Activities.hpp>
#include <rtt/extras/SequentialActivity.hpp>
#include <rtt/extras/FileDescriptorActivity.hpp>
#include <rtt/marsh/PropertyMarshaller.hpp>
#include <rtt/marsh/PropertyDemarshaller.hpp>
#include <rtt/scripting/Scripting.hpp>
#include <rtt/ConnPolicy.hpp>
#include <rtt/plugin/PluginLoader.hpp>
#include <rtt/types/GlobalsRepository.hpp>

# if defined(_POSIX_VERSION)
#   define USE_SIGNALS 1
# endif

#ifdef USE_SIGNALS
#include <signal.h>
#endif

#include <boost/algorithm/string.hpp>
#include <rtt/base/OperationCallerBaseInvoker.hpp>

#include <cstdio>
#include <cstdlib>

#include "ocl/Component.hpp"
#include <rtt/marsh/PropertyLoader.hpp>

#undef _POSIX_C_SOURCE
#include <sys/types.h>
#include <iostream>
#include <fstream>
#include <set>



using namespace Orocos;

namespace OCL
{
    using namespace std;
    using namespace RTT;
    using namespace RTT::marsh;
    using namespace RTT::detail;

    /**
     * I'm using a set to speed up lookups.
     */
    static std::set<string> valid_names;

    static int got_signal = -1;

    // Signal code only on Posix:
#if defined(USE_SIGNALS)
    // catch ctrl+c signal
    void ctrl_c_catcher(int sig)
    {
    	// Ctrl-C received (or any other signal)
    	got_signal = sig;
    }
#endif

#define ORO_str(s) ORO__str(s)
#define ORO__str(s) #s

    DeploymentComponent::DeploymentComponent(std::string name, std::string siteFile)
        : RTT::TaskContext(name, Stopped),
          defaultWaitPeriodPolicy(ORO_WAIT_ABS),
          autoUnload("AutoUnload",
                     "Stop, cleanup and unload all components loaded by the DeploymentComponent when it is destroyed.",
                     true),
          validConfig("Valid", false),
          sched_RT("ORO_SCHED_RT", ORO_SCHED_RT ),
          sched_OTHER("ORO_SCHED_OTHER", ORO_SCHED_OTHER ),
          lowest_Priority("LowestPriority", RTT::os::LowestPriority ),
          highest_Priority("HighestPriority", RTT::os::HighestPriority ),
          target("Target",
                 ORO_str(OROCOS_TARGET) ),
          nextGroup(0)
    {
        this->addProperty( "RTT_COMPONENT_PATH", compPath ).doc("Locations to look for components. Use a colon or semi-colon separated list of paths. Defaults to the environment variable with the same name.");
        this->addProperty( "DefaultWaitPeriodPolicy", defaultWaitPeriodPolicy ).doc("The default value for the wait period policy property for threads of newly created activities (ORO_WAIT_ABS or ORO_WAIT_REL).");
        this->addProperty( autoUnload );
        this->addAttribute( target );

        this->addAttribute( validConfig );
        this->addAttribute( sched_RT );
        this->addAttribute( sched_OTHER );
        this->addAttribute( lowest_Priority );
        this->addAttribute( highest_Priority );


        this->addOperation("reloadLibrary", &DeploymentComponent::reloadLibrary, this, ClientThread).doc("Reload a new component library into memory.").arg("FilePath", "The absolute file name of the to be reloaded library. Warning: this is a low-level function only to be used during development/testing.");
        this->addOperation("loadLibrary", &DeploymentComponent::loadLibrary, this, ClientThread).doc("Load a new library (component, plugin or typekit) into memory.").arg("Name", "The absolute or relative name of the to be loaded library. Warning: this is a low-level function you should only use if import() doesn't work for you.");
        this->addOperation("import", &DeploymentComponent::import, this, ClientThread).doc("Import all components, plugins and typekits from a given package or directory in the search path.").arg("Package", "The name absolute or relative name of a directory or package.");
        this->addOperation("path", &DeploymentComponent::path, this, ClientThread).doc("Add additional directories to the component search path without importing them.").arg("Paths", "A colon or semi-colon separated list of paths to search for packages.");

        this->addOperation("loadComponent", &DeploymentComponent::loadComponent, this, ClientThread).doc("Load a new component instance from a library.").arg("Name", "The name of the to be created component").arg("Type", "The component type, used to lookup the library.");
        this->addOperation("configureComponent", (bool (DeploymentComponent::*)(const std::string&))&DeploymentComponent::configureComponent, this, ClientThread).doc("Configure a component who is a peer of this deployer by name.").arg("Name", "The component's name");
        this->addOperation("startComponent", (bool (DeploymentComponent::*)(const std::string&))&DeploymentComponent::startComponent, this, ClientThread).doc("Start a component who is a peer of this deployer by name.").arg("Name", "The component's name");
        this->addOperation("stopComponent", (bool (DeploymentComponent::*)(const std::string&))&DeploymentComponent::stopComponent, this, ClientThread).doc("Stop a component who is a peer of this deployer by name.").arg("Name", "The component's name");
        // avoid warning about overriding
        this->provides()->removeOperation("loadService");
        this->addOperation("loadService", &DeploymentComponent::loadService, this, ClientThread).doc("Load a discovered service or plugin in an existing component.").arg("Name", "The name of the component which will receive the service").arg("Service", "The name of the service or plugin.");
        this->addOperation("unloadComponent", &DeploymentComponent::unloadComponent, this, ClientThread).doc("Unload a loaded component instance.").arg("Name", "The name of the to be created component");
        this->addOperation("displayComponentTypes", &DeploymentComponent::displayComponentTypes, this, ClientThread).doc("Print out a list of all component types this component can create.");
        this->addOperation("getComponentTypes", &DeploymentComponent::getComponentTypes, this, ClientThread).doc("return a vector of all component types this component can create.");

        this->addOperation("loadConfiguration", &DeploymentComponent::loadConfiguration, this, ClientThread).doc("Load a new XML configuration from a file (identical to loadComponents).").arg("File", "The file which contains the new configuration.");
        this->addOperation("loadConfigurationString", &DeploymentComponent::loadConfigurationString, this, ClientThread).doc("Load a new XML configuration from a string.").arg("Text", "The string which contains the new configuration.");
        this->addOperation("clearConfiguration", &DeploymentComponent::clearConfiguration, this, ClientThread).doc("Clear all configuration settings.");

        this->addOperation("loadComponents", &DeploymentComponent::loadComponents, this, ClientThread).doc("Load components listed in an XML configuration file.").arg("File", "The file which contains the new configuration.");
        this->addOperation("configureComponents", &DeploymentComponent::configureComponents, this, ClientThread).doc("Apply a loaded configuration to the components and configure() them if AutoConf is set.");
        this->addOperation("startComponents", &DeploymentComponent::startComponents, this, ClientThread).doc("Start the components configured for AutoStart.");
        this->addOperation("stopComponents", &DeploymentComponent::stopComponents, this, ClientThread).doc("Stop all the configured components (with or without AutoStart).");
        this->addOperation("cleanupComponents", &DeploymentComponent::cleanupComponents, this, ClientThread).doc("Cleanup all the configured components (with or without AutoConf).");
        this->addOperation("unloadComponents", &DeploymentComponent::unloadComponents, this, ClientThread).doc("Unload all the previously loaded components.");

        this->addOperation("runScript", &DeploymentComponent::runScript, this, ClientThread).doc("Runs a script.").arg("File", "An Orocos program script.");
        this->addOperation("kickStart", &DeploymentComponent::kickStart, this, ClientThread).doc("Calls loadComponents, configureComponents and startComponents in a row.").arg("File", "The file which contains the XML configuration to use.");
        this->addOperation("kickOutAll", &DeploymentComponent::kickOutAll, this, ClientThread).doc("Calls stopComponents, cleanupComponents and unloadComponents in a row.");

        this->addOperation("kickOutComponent", &DeploymentComponent::kickOutComponent, this, ClientThread).doc("Calls stopComponents, cleanupComponent and unloadComponent in a row.").arg("comp_name", "component name");
        this->addOperation("kickOut", &DeploymentComponent::kickOut, this, ClientThread).doc("Calls stopComponents, cleanupComponents and unloadComponents in a row.").arg("File", "The file which contains the name of the components to kickOut (for example, the same used in kickStart).");

        this->addOperation("waitForInterrupt", &DeploymentComponent::waitForInterrupt, this, ClientThread).doc("This operation waits for the SIGINT signal and then returns. This allows you to wait in a script for ^C.");
        this->addOperation("waitForSignal", &DeploymentComponent::waitForSignal, this, ClientThread).doc("This operation waits for the signal of the argument and then returns. This allows you to wait in a script for any signal except SIGKILL and SIGSTOP.").arg("signal number","The signal number to wait for.");


        this->addOperation("connectPort", &DeploymentComponent::connectPort, this, ClientThread)
            .doc("Declare an exactly typed whole-port cyclic connection.")
            .arg("Source", "Service-qualified output port.").arg("Destination", "Service-qualified input port.");
        this->addOperation("connectMember", &DeploymentComponent::connectMember, this, ClientThread)
            .doc("Declare a typed cyclic member mapping. Empty member paths select whole values.")
            .arg("Source", "Service-qualified output port.").arg("SourceMember", "Member or fixed-array path.")
            .arg("Destination", "Service-qualified input port.").arg("DestinationMember", "Member or fixed-array path.");
        this->addOperation("finalizeConnections", &DeploymentComponent::finalizeConnections, this, ClientThread)
            .doc("Validate and prepare cyclic connections of all peers before activation.");

        // Work around compiler ambiguity:
        typedef bool(DeploymentComponent::*DCFun)(const std::string&, const std::string&);
        DCFun cp = &DeploymentComponent::connectPeers;
        this->addOperation("connectPeers", cp, this, ClientThread).doc("Connect two Components known to this Component.").arg("One", "The first component.").arg("Two", "The second component.");
        cp = &DeploymentComponent::connectPorts;
        this->addOperation("connectPorts", cp, this, ClientThread).doc("DEPRECATED. Connect the Data Ports of two Components known to this Component.").arg("One", "The first component.").arg("Two", "The second component.");
        typedef bool(DeploymentComponent::*DC4Fun)(const std::string&, const std::string&,
                                                   const std::string&, const std::string&);
        DC4Fun cp4 = &DeploymentComponent::connectPorts;
        this->addOperation("connectTwoPorts", cp4, this, ClientThread).doc("DEPRECATED. Connect two ports of Components known to this Component.")
                .arg("One", "The first component.")
                .arg("PortOne", "The port name of the first component.")
                .arg("Two", "The second component.")
                .arg("PortTwo", "The port name of the second component.");
        this->addOperation("createStream", &DeploymentComponent::createStream, this, ClientThread).doc("DEPRECATED. Creates a stream to or from a port.")
                .arg("component", "The component which owns 'port'.")
                .arg("port", "The port to create a stream from or to.")
                .arg("policy", "The connection policy which serves to describe the stream to be created.");

        // New API:
        this->addOperation("connect", &DeploymentComponent::connect, this, ClientThread).doc("Creates a connection between two ports.")
                .arg("portOne", "The first port of the connection. Use a dot-separated-path.")
                .arg("portTwo", "The second port of the connection. Use a dot-separated-path.")
                .arg("policy", "The connection policy which serves to describe the stream to be created. Use 'ConnPolicy()' to use the default.");
        this->addOperation("stream", &DeploymentComponent::stream, this, ClientThread).doc("Creates a stream to or from a port.")
                .arg("port", "The port to create a stream from or to. Use a dot-separated-path.")
                .arg("policy", "The connection policy which serves to describe the stream to be created. Use 'ConnPolicy()' to use the default.");

        this->addOperation("connectServices", (bool(DeploymentComponent::*)(const std::string&, const std::string&))&DeploymentComponent::connectServices, this, ClientThread).doc("Connect the matching provides/requires services of two Components known to this Component.").arg("One", "The first component.").arg("Two", "The second component.");
        this->addOperation("connectOperations", &DeploymentComponent::connectOperations, this, ClientThread).doc("Connect the matching provides/requires operations of two Components known to this Component.").arg("Requested", "The requested operation (dot-separated path).").arg("Provided", "The provided operation (dot-separated path).");

        cp = &DeploymentComponent::addPeer;
        this->addOperation("addPeer", cp, this, ClientThread).doc("Add a peer to a Component.").arg("From", "The first component.").arg("To", "The other component.");
        this->addOperation("aliasPeer", &DeploymentComponent::aliasPeer, this, ClientThread).doc("Add a peer to a Component with an alternative name.").arg("From", "The component which will see 'To' in its peer list.").arg("To", "The component which will be seen by 'From'.").arg("Alias","The name under which 'To' is known to 'From'");
        typedef void(DeploymentComponent::*RPFun)(const std::string&);
        RPFun rp = &RTT::TaskContext::removePeer;
        this->addOperation("removePeer", rp, this, ClientThread).doc("Remove a peer from this Component.").arg("PeerName", "The name of the peer to remove.");

        this->addOperation("setActivity", &DeploymentComponent::setActivity, this, ClientThread).doc("Attach an activity to a Component.").arg("CompName", "The name of the Component.").arg("Period", "The period of the activity (set to 0.0 for non periodic).").arg("Priority", "The priority of the activity.").arg("SchedType", "The scheduler type of the activity.");
        this->addOperation("setActivityOnCPU", &DeploymentComponent::setActivityOnCPU, this, ClientThread).doc("Attach an activity to a Component.").arg("CompName", "The name of the Component.").arg("Period", "The period of the activity (set to 0.0 for non periodic).").arg("Priority", "The priority of the activity.").arg("SchedType", "The scheduler type of the activity.").arg("CPU","The CPU to run on, starting from zero.");
        this->addOperation("setPeriodicActivity", &DeploymentComponent::setPeriodicActivity, this, ClientThread).doc("Attach a periodic activity to a Component.").arg("CompName", "The name of the Component.").arg("Period", "The period of the activity.").arg("Priority", "The priority of the activity.").arg("SchedType", "The scheduler type of the activity.");
        this->addOperation("setPeriodicActivityOnCPU", &DeploymentComponent::setPeriodicActivityOnCPU, this, ClientThread).doc("Attach a periodic activity to a Component on a CPU.").arg("CompName", "The name of the Component.").arg("Period", "The period of the activity.").arg("Priority", "The priority of the activity.").arg("SchedType", "The scheduler type of the activity.").arg("CPU","The CPU to run on, starting from zero.");
        this->addOperation("setSequentialActivity", &DeploymentComponent::setSequentialActivity, this, ClientThread).doc("Attach a 'stand alone' sequential activity to a Component.").arg("CompName", "The name of the Component.");
        this->addOperation("setSlaveActivity", &DeploymentComponent::setSlaveActivity, this, ClientThread).doc("Attach a 'stand alone' slave activity to a Component.").arg("CompName", "The name of the Component.").arg("Period", "The period of the activity (set to zero for non periodic).");
        this->addOperation("setMasterSlaveActivity", &DeploymentComponent::setMasterSlaveActivity, this, ClientThread).doc("Attach a slave activity with a master to a Component. The slave becomes a peer of the master as well.").arg("Master", "The name of the Component which is master of the Slave.").arg("Slave", "The name of the Component which gets the SlaveActivity.");
		this->addOperation("setFileDescriptorActivity", &DeploymentComponent::setFileDescriptorActivity, this, ClientThread)
			.doc("Attach a File Descriptor activity to a Component.")
			.arg("CompName", "The name of the Component.")
			.arg("Timeout", "The timeout of the activity (set to zero for no timeout).")
			.arg("Priority", "The priority of the activity.")
			.arg("SchedType", "The scheduler type of the activity.");

        this->addOperation("setWaitPeriodPolicy", &DeploymentComponent::setWaitPeriodPolicy, this, ClientThread).doc("Sets the wait period policy of an existing component thread.").arg("CompName", "The name of the Component.").arg("Policy", "The new policy (ORO_WAIT_ABS or ORO_WAIT_REL).");

        valid_names.insert("AutoUnload");
        valid_names.insert("UseNamingService");
        valid_names.insert("Server");
        valid_names.insert("AutoConf");
        valid_names.insert("AutoStart");
        valid_names.insert("AutoConnect");
        valid_names.insert("AutoSave");
        valid_names.insert("PropertyFile");
        valid_names.insert("UpdateProperties");
        valid_names.insert("LoadProperties");
        valid_names.insert("ProgramScript");
        valid_names.insert("StateMachineScript");
        valid_names.insert("Ports");
        valid_names.insert("Peers");
        valid_names.insert("Activity");
        valid_names.insert("Master");
        valid_names.insert("Properties");
        valid_names.insert("Service");
        valid_names.insert("Plugin"); // equivalent to Service.
        valid_names.insert("Provides"); // equivalent to Service.
        valid_names.insert("RunScript"); // runs a program script in a component.

        // Check for 'Deployer-site.cpf' XML file.
        if (siteFile.empty())
            siteFile = this->getName() + "-site.cpf";
        std::ifstream hassite(siteFile.c_str());
        if ( !hassite ) {
            // if not, just configure
            this->configure();

            // Backwards compatibility with < 2.3: import OCL by default
            Logger::log().logf(Logger::Info, "DeploymentComponent",
                               "No site file was found. Importing 'ocl' by default.");
            try {
                import("ocl");
            } catch (std::exception& e) {
                // ignore errors.
            }
            return;
        }

        // OK: kick-start it. Need to do import("ocl") and set AutoConf to configure self.
        Logger::log().logf(Logger::Info, "DeploymentComponent",
                           "Using site file '%s'.", siteFile.c_str());
        this->kickStart( siteFile );

    }

    bool DeploymentComponent::configureHook()
    {
        if (compPath.empty() )
        {
            compPath = ComponentLoader::Instance()->getComponentPath();
        } else {
            Logger::log().logf(Logger::Info, "DeploymentComponent::configure",
                               "RTT_COMPONENT_PATH was set to %s", compPath.c_str());
            Logger::log().logf(Logger::Info, "DeploymentComponent::configure",
                               "Re-scanning for plugins and components...");
            PluginLoader::Instance()->setPluginPath(compPath);
            ComponentLoader::Instance()->setComponentPath(compPath);
            ComponentLoader::Instance()->import(compPath);
        }
        return true;
    }

    bool DeploymentComponent::componentLoaded(RTT::TaskContext*) { return true; }

    bool DeploymentComponent::componentCanUnload(RTT::TaskContext*) { return true; }

    void DeploymentComponent::componentUnloaded(TaskContext*) { }

    std::unique_lock<std::recursive_mutex> DeploymentComponent::lockDeployment() const
    {
        return std::unique_lock<std::recursive_mutex>(deploymentMutex);
    }

    bool DeploymentComponent::deploymentShuttingDown() const noexcept
    {
        return deploymentClosing.load();
    }

    RTT::Service::shared_ptr DeploymentComponent::attachDeploymentService(
        const std::string& name,
        const std::function<RTT::Service::shared_ptr()>& create)
    {
        auto lock = lockDeployment();
        if (deploymentClosing.load() || provides()->hasService(name))
            return {};
        auto service = create();
        auto* lifecycle = dynamic_cast<DeploymentServiceLifecycle*>(service.get());
        if (!lifecycle || service->getName() != name || service->getOwner() != this)
            return {};

        std::vector<TaskContext*> seeded;
        seeded.reserve(compmap.size());
        try {
            for (const auto& entry : compmap) {
                TaskContext* component = entry.second.instance;
                if (!component)
                    continue;
                seeded.push_back(component);
                if (!lifecycle->componentLoaded(component)) {
                    for (auto* previous : seeded)
                        lifecycle->componentUnloaded(previous);
                    return {};
                }
            }
            std::lock_guard<std::mutex> registry_lock(deploymentServicesMutex);
            if (!deploymentClosing.load()) {
                deploymentServices.push_back(service);
                try {
                    if (provides()->addService(service))
                        return service;
                } catch (...) {
                    deploymentServices.pop_back();
                    throw;
                }
                deploymentServices.pop_back();
            }
        } catch (...) {
            for (auto* previous : seeded)
                lifecycle->componentUnloaded(previous);
            throw;
        }
        for (auto* previous : seeded)
            lifecycle->componentUnloaded(previous);
        return {};
    }

    bool DeploymentComponent::markManagedProxy(TaskContext* component)
    {
        auto lock = lockDeployment();
        for (auto& entry : compmap) {
            if (entry.second.instance == component) {
                entry.second.proxy = true;
                return true;
            }
        }
        return false;
    }

    bool DeploymentComponent::isManagedProxy(const TaskContext* component) const
    {
        auto lock = lockDeployment();
        for (const auto& entry : compmap) {
            if (entry.second.instance == component)
                return entry.second.proxy;
        }
        return false;
    }

    void DeploymentComponent::prepareDeploymentShutdown() noexcept
    {
        std::call_once(deploymentShutdownOnce, [this] {
            std::vector<RTT::Service::shared_ptr> participants;
            {
                std::lock_guard<std::mutex> lock(deploymentServicesMutex);
                deploymentClosing.store(true);
                participants = deploymentServices;
            }
            for (const auto& service : participants)
                dynamic_cast<DeploymentServiceLifecycle&>(*service).beginDeploymentShutdown();
            for (const auto& service : participants)
                dynamic_cast<DeploymentServiceLifecycle&>(*service).finishDeploymentShutdown();
            // An in-flight load must finish before ordinary component teardown.
            auto lock = lockDeployment();
            deploymentDrained.store(true);
        });
    }

    DeploymentComponent::~DeploymentComponent()
    {
      prepareDeploymentShutdown();
      // Should we unload all loaded components here ?
      if ( autoUnload.get() ) {
          kickOutAll();
      }
    }

    bool DeploymentComponent::waitForInterrupt() {
#ifdef USE_SIGNALS
        int sigs[] = { SIGINT, SIGTERM, SIGHUP };
        if ( !waitForSignals(sigs, 3) )
    		return false;
    	cout << "DeploymentComponent: Got interrupt !" <<endl;
    	return true;
#else
        cout << "DeploymentComponent: Failed to install interrupt handlers: Not supported by this Operating System. "<<endl;
        return false;
#endif
    }

    bool DeploymentComponent::waitForSignal(int sig) {
        return waitForSignals(&sig, 1);
    }

    bool DeploymentComponent::waitForSignals(int *sigs, std::size_t sig_count) {
#ifdef USE_SIGNALS
        struct sigaction sa = {};
        std::vector<struct sigaction> sold(sig_count);
        std::size_t index = 0;
        sa.sa_handler = ctrl_c_catcher;
        sigemptyset(&sa.sa_mask);
        for( ; index < sig_count; ++index) {
            if ( ::sigaction(sigs[index], &sa, &sold[index]) != 0) {
                cout << "DeploymentComponent: Failed to install signal handler for signal " << sigs[index] << endl;
                break;
            }
        }

        if (index == sig_count) {
            bool break_loop = false;
            while(!break_loop) {
                for(std::size_t check = 0; check < sig_count; ++check) {
                    if (got_signal == sigs[check]) break_loop = true;
                }
                TIME_SPEC ts;
                ts.tv_sec = 1;
                ts.tv_nsec = 0;
                rtos_nanosleep(&ts, 0);
            }
        }
        got_signal = -1;

        // reinstall previous handlers if present.
        while(index > 0) {
            index--;
            if (sold[index].sa_handler || sold[index].sa_sigaction) {
                ::sigaction(sigs[index], &sold[index], NULL);
            }
        }
        return true;
#else
        int first_signal = sig_count ? sigs[0] : 0;
        cout << "DeploymentComponent: Failed to install signal handler for signal " << first_signal << ": Not supported by this Operating System. "<<endl;
        return false;
#endif
    }

    bool DeploymentComponent::connectPeers(const std::string& one, const std::string& other)
    {
        RTT::TaskContext* t1 = (((one == this->getName()) || (one == "this")) ? this : this->getPeer(one));
        RTT::TaskContext* t2 = (((other == this->getName()) || (other == "this")) ? this : this->getPeer(other));
        if (!t1) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectPeers",
                               "No such peer: %s", one.c_str());
            return false;
        }
        if (!t2) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectPeers",
                               "No such peer: %s", other.c_str());
            return false;
        }
        return t1->connectPeers(t2);
    }

    bool DeploymentComponent::addPeer(const std::string& from, const std::string& to)
    {
        RTT::TaskContext* t1 = (((from == this->getName()) || (from == "this")) ? this : this->getPeer(from));
        RTT::TaskContext* t2 = (((to == this->getName()) || (to == "this")) ? this : this->getPeer(to));
        if (!t1) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::addPeer",
                               "No such peer: %s", from.c_str());
            return false;
        }
        if (!t2) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::addPeer",
                               "No such peer: %s", to.c_str());
            return false;
        }
        if ( t1->hasPeer(to) ) {
            Logger::log().logf(Logger::Info, "DeploymentComponent::addPeer",
                               "addPeer: %s is already a peer of %s", to.c_str(), from.c_str());
            return true;
        }
        return t1->addPeer(t2,to);
    }

    bool DeploymentComponent::aliasPeer(const std::string& from, const std::string& to, const std::string& alias)
    {
        RTT::TaskContext* t1 = (((from == this->getName()) || (from == "this")) ? this : this->getPeer(from));
        RTT::TaskContext* t2 = (((to == this->getName()) || (to == "this")) ? this : this->getPeer(to));
        if (!t1) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::aliasPeer",
                               "No such peer known to deployer '%s': %s",
                               this->getName().c_str(), from.c_str());
            return false;
        }
        if (!t2) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::aliasPeer",
                               "No such peer known to deployer '%s': %s",
                               this->getName().c_str(), to.c_str());
            return false;
        }
        return t1->addPeer(t2, alias);
    }

    Service::shared_ptr DeploymentComponent::stringToService(string const& names) {
    	std::vector<std::string> strs;
    	boost::split(strs, names, boost::is_any_of("."));

      // strs could be empty because of a bug in Boost 1.44 (see https://svn.boost.org/trac/boost/ticket/4751)
      if (strs.empty()) return Service::shared_ptr();

        string component = strs.front();
        RTT::TaskContext *tc = (((component == this->getName()) || (component == "this")) ? this : getPeer(component));
        if (!tc) {
            if ( names.find('.') != string::npos ) {
                Logger::log().logf(Logger::Error, "DeploymentComponent::stringToService",
                                   "No such component: '%s' when looking for service '%s '",
                                   component.c_str(), names.c_str());
            } else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::stringToService",
                                   "No such component: '%s'", component.c_str());
            }
    		return Service::shared_ptr();
    	}
    	// component is peer or self:
        Service::shared_ptr ret = tc->provides();

    	// remove component name:
    	strs.erase( strs.begin() );

    	// iterate over remainders:
    	while ( !strs.empty() && ret) {
    		ret = ret->getService( strs.front() );
    		if (ret)
    			strs.erase( strs.begin() );
    	}
    	if (!ret) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::stringToService",
                               "No such service: '%s' while looking for service '%s'",
                               strs.front().c_str(), names.c_str());
    	}
    	return ret;
    }

    ServiceRequester::shared_ptr DeploymentComponent::stringToServiceRequester(string const& names) {
        std::vector<std::string> strs;
        boost::split(strs, names, boost::is_any_of("."));

        string component = strs.front();
        RTT::TaskContext *tc = (((component == this->getName()) || (component == "this")) ? this : getPeer(component));
        if (!tc) {
            if ( names.find('.') != string::npos ) {
                Logger::log().logf(Logger::Error, "DeploymentComponent::stringToServiceRequester",
                                   "No such component: '%s' when looking for service '%s'",
                                   component.c_str(), names.c_str());
            } else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::stringToServiceRequester",
                                   "No such component: '%s'", component.c_str());
            }
            return ServiceRequester::shared_ptr();
        }
        // component is peer or self:
        ServiceRequester::shared_ptr ret = tc->requests();

        // remove component name:
        strs.erase( strs.begin() );

        // iterate over remainders:
        while ( !strs.empty() && ret) {
            ret = ret->requests( strs.front() );
            if (ret)
                strs.erase( strs.begin() );
        }
        if (!ret) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::stringToServiceRequester",
                               "No such service: '%s' while looking for service '%s'",
                               strs.front().c_str(), names.c_str());
        }
        return ret;
    }

    base::PortInterface* DeploymentComponent::stringToPort(string const& names) {
    	std::vector<std::string> strs;
    	boost::split(strs, names, boost::is_any_of("."));

      // strs could be empty because of a bug in Boost 1.44 (see https://svn.boost.org/trac/boost/ticket/4751)
      if (strs.size() < 2 || std::find(strs.begin(), strs.end(), std::string()) != strs.end()) return 0;

    	string component = strs.front();
        RTT::TaskContext *tc = (((component == this->getName()) || (component == "this")) ? this : getPeer(component));
        if (!tc) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::stringToPort",
                               "No such component: '%s' when looking for port '%s'",
                               component.c_str(), names.c_str());
    		return 0;
    	}
    	// component is peer or self:
        Service::shared_ptr serv = tc->provides();
    	base::PortInterface* ret = 0;

    	// remove component name:
    	strs.erase( strs.begin() );

    	// iterate over remainders:
    	while ( strs.size() != 1 && serv) {
    		serv = serv->getService( strs.front() );
    		if (serv)
    			strs.erase( strs.begin() );
    	}
    	if (!serv) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::stringToPort",
                               "No such service: '%s' while looking for port '%s'",
                               strs.front().c_str(), names.c_str());
    		return 0;
    	}
    	ret = serv->getPort(strs.front());
    	if (!ret) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::stringToPort",
                               "No such port: '%s' while looking for port '%s'",
                               strs.front().c_str(), names.c_str());
    	}

    	return ret;
    }

    bool DeploymentComponent::connectPort(const std::string& source, const std::string& destination)
    {
        return connectMember(source, "", destination, "");
    }

    bool DeploymentComponent::connectMember(const std::string& source, const std::string& sourceMember,
                                             const std::string& destination, const std::string& destinationMember)
    {
        base::OutputPortInterface* output = dynamic_cast<base::OutputPortInterface*>(stringToPort(source));
        base::InputPortInterface* input = dynamic_cast<base::InputPortInterface*>(stringToPort(destination));
        if (!output || !input) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectMember",
                              "Expected output '%s' and input '%s'", source.c_str(), destination.c_str());
            return false;
        }
        return RTT::connectMembers(*output, sourceMember, *input, destinationMember);
    }

    bool DeploymentComponent::finalizeConnections()
    {
        const PeerList peers = getPeerList();
        // Check all states first, so an active peer cannot cause partial preparation.
        if (isRunning()) return false;
        for (PeerList::const_iterator it = peers.begin(); it != peers.end(); ++it)
            if (getPeer(*it)->isRunning()) return false;
        bool result = TaskContext::finalizeConnections();
        for (PeerList::const_iterator it = peers.begin(); it != peers.end(); ++it)
            result = getPeer(*it)->finalizeConnections() && result;
        return result;
    }

    bool DeploymentComponent::connectPorts(const std::string& one, const std::string& other)
    {
        RTT::TaskContext* a, *b;
        a = getPeer(one);
        b = getPeer(other);
        if ( !a ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectPorts",
                               "%s could not be found.", one.c_str());
            return false;
        }
        if ( !b ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectPorts",
                               "%s could not be found.", other.c_str());
            return false;
        }

        return a->connectPorts(b);
    }

    bool DeploymentComponent::connectPorts(const std::string& one, const std::string& one_port,
                                           const std::string& other, const std::string& other_port)
    {
		Service::shared_ptr a,b;
		a = stringToService(one);
		b = stringToService(other);
		if (!a || !b)
			return false;
        base::PortInterface* ap, *bp;
        ap = a->getPort(one_port);
        bp = b->getPort(other_port);
        if ( !ap ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectPorts",
                               "%s does not have a port %s", one.c_str(), one_port.c_str());
            return false;
        }
        if ( !bp ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectPorts",
                               "%s does not have a port %s", other.c_str(), other_port.c_str());
            return false;
        }

        // Warn about already connected ports.
        if ( ap->connected() && bp->connected() ) {
            Logger::log().logf(Logger::Debug, "DeploymentComponent::connectPorts",
                               "Port '%s' of Component '%s' and port '%s' of Component '%s' are already connected but (probably) not to each other. Connecting them anyway.",
                               ap->getName().c_str(), a->getName().c_str(),
                               bp->getName().c_str(), b->getName().c_str());
        }

        // use the base::PortInterface implementation
        if ( ap->connectTo( bp ) ) {
            // all went fine.
            Logger::log().logf(Logger::Info, "DeploymentComponent::connectPorts",
                               "Connected Port %s.%s to  %s.%s.",
                               one.c_str(), one_port.c_str(), other.c_str(), other_port.c_str());
            return true;
        } else {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectPorts",
                               "Failed to connect Port %s.%s to  %s.%s.",
                               one.c_str(), one_port.c_str(), other.c_str(), other_port.c_str());
            return true;
        }
    }

    bool DeploymentComponent::createStream(const std::string& comp, const std::string& port, ConnPolicy policy)
    {
        Service::shared_ptr serv = stringToService(comp);
        if ( !serv )
            return false;
        PortInterface* porti = serv->getPort(port);
        if ( !porti ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::createStream",
                               "Service in component %s has no port %s.",
                               comp.c_str(), port.c_str());
            return false;
        }
        return porti->createStream( policy );
    }

    // New API:
    bool DeploymentComponent::connect(const std::string& one, const std::string& other, ConnPolicy cp)
    {
		base::PortInterface* ap, *bp;
		ap = stringToPort(one);
		bp = stringToPort(other);
		if (!ap || !bp)
			return false;

        // Warn about already connected ports.
        if ( ap->connected() && bp->connected() ) {
            Logger::log().logf(Logger::Debug, "DeploymentComponent::connect",
                               "Port '%s' of '%s' and port '%s' of '%s' are already connected but (probably) not to each other. Connecting them anyway.",
                               ap->getName().c_str(), one.c_str(),
                               bp->getName().c_str(), other.c_str());
        }

        // use the base::PortInterface implementation
        if ( ap->connectTo( bp, cp ) ) {
            // all went fine.
            Logger::log().logf(Logger::Info, "DeploymentComponent::connect",
                               "Connected Port %s to  %s.", one.c_str(), other.c_str());
            return true;
        } else {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connect",
                               "Failed to connect Port %s to  %s.", one.c_str(), other.c_str());
            return false;
        }
    }

    bool DeploymentComponent::stream(const std::string& port, ConnPolicy policy)
    {
        base::PortInterface* porti = stringToPort(port);
        if ( !porti ) {
            return false;
        }
        return porti->createStream( policy );
    }

    bool DeploymentComponent::connectServices(const std::string& one, const std::string& other)
    {
        RTT::TaskContext* a, *b;
        a = getPeer(one);
        b = getPeer(other);
        if ( !a ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectServices",
                               "%s could not be found.", one.c_str());
            return false;
        }
        if ( !b ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectServices",
                               "%s could not be found.", other.c_str());
            return false;
        }

        return a->connectServices(b);
    }

    bool DeploymentComponent::connectOperations(const std::string& required, const std::string& provided)
    {
        // Required service
        boost::iterator_range<std::string::const_iterator> reqs = boost::algorithm::find_last(required, ".");
        std::string reqs_name(required.begin(), reqs.begin());
        std::string rop_name(reqs.begin()+1, required.end());
        Logger::log().logf(Logger::Debug, "DeploymentComponent::connectOperations",
                           "Looking for required operation %s in service %s",
                           rop_name.c_str(), reqs_name.c_str());
        ServiceRequester::shared_ptr r = this->stringToServiceRequester(reqs_name);
        // Provided service
        boost::iterator_range<std::string::const_iterator> pros = boost::algorithm::find_last(provided, ".");
        std::string pros_name(provided.begin(), pros.begin());
        std::string pop_name(pros.begin()+1, provided.end());
        Logger::log().logf(Logger::Debug, "DeploymentComponent::connectOperations",
                           "Looking for provided operation %s in service %s",
                           pop_name.c_str(), pros_name.c_str());
        Service::shared_ptr p = this->stringToService(pros_name);
        // Requested operation
        RTT::base::OperationCallerBaseInvoker* rop = r->getOperationCaller(rop_name);
        if (! rop) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectOperations",
                               "No requested operation %s found in service %s",
                               rop_name.c_str(), reqs_name.c_str());
            return false;
        }
        if ( rop->ready() ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectOperations",
                               "Requested operation %s already connected to a provided operation!",
                               rop_name.c_str());
            return false;
        }
        // Provided operation
        if (! p->hasOperation(pop_name)) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::connectOperations",
                               "No provided operation %s found in service %s",
                               pop_name.c_str(), pros_name.c_str());
            return false;
        }
        // Connection
        rop->setImplementation(p->getLocalOperation( pop_name ), r->getServiceOwner()->engine());
        if ( rop->ready() )
            Logger::log().logf(Logger::Debug, "DeploymentComponent::connectOperations",
                               "Successfully set up OperationCaller for operation %s",
                               rop_name.c_str());
        return rop->ready();
    }

    int string_to_oro_sched(const std::string& sched) {
        if ( sched == "ORO_SCHED_OTHER" )
            return ORO_SCHED_OTHER;
        if (sched == "ORO_SCHED_RT" )
            return ORO_SCHED_RT;
        Logger::log().logf(Logger::Error, "DeploymentComponent::string_to_oro_sched",
                           "Unknown scheduler type: %s", sched.c_str());
        return -1;
    }

    bool DeploymentComponent::loadConfigurationString(const std::string& text)
    {
        const char* tmpfile = ".loadConfigurationString.cpf";
        std::ofstream file( tmpfile );
        file << text.c_str();
        file.close();
        return this->loadConfiguration( tmpfile );
    }

    bool DeploymentComponent::runScript(const std::string& file_name)
    {
#ifdef BUILD_LUA_RTT
        if (file_name.rfind(".lua") == file_name.length() - 4) {
            if (!this->provides()->hasService("Lua")) {
                // Load lua scripting service
                if(!RTT::plugin::PluginLoader::Instance()->loadService("Lua", this)) {
                  Logger::log().logf(Logger::Error, "DeploymentComponent::runScript",
                                     "Could not load lua service.");
                  return false;
                }

                // Get exec_str operation
                RTT::OperationCaller<bool(std::string)> exec_str =
                    this->provides("Lua")->getOperation("exec_str");

                // Load rttlib for first-class operation support
                exec_str("require(\"rttlib\")");
            }

            // Get exec_file operation
            RTT::OperationCaller<bool(std::string)> exec_file =
                this->provides("Lua")->getOperation("exec_file");

            return exec_file( file_name );
        }
#endif
        return this->getProvider<Scripting>("scripting")->runScript( file_name );
    }

    bool DeploymentComponent::kickStart(const std::string& configurationfile)
    {
        bool              loadOk        = true;
        bool              configureOk   = true;
        bool              startOk       = true;

        const bool rc = kickStart2(configurationfile, true, loadOk, configureOk, startOk);

        // avoid compiler warnings
        (void)loadOk;
        (void)configureOk;
        (void)startOk;

        return rc;
    }

    bool DeploymentComponent::kickStart2(const std::string& configurationfile,
                                         const bool         doStart,
                                         bool&              loadOk,
                                         bool&              configureOk,
                                         bool&              startOk)
    {
        // set defaults
        loadOk      = true;
        configureOk = true;
        startOk     = true;

        int thisGroup = nextGroup;
        ++nextGroup;    // whether succeed or fail
        if ( this->loadComponentsInGroup(configurationfile, thisGroup) ) {
            if ( root.empty() ) {
                Logger::log().logf(Logger::Warning, "DeploymentComponent::kickStart2",
                                   "No components loaded by DeploymentComponent from %s",
                                   configurationfile.c_str());
                return true;
            }
            if (this->configureComponentsGroup(thisGroup) ) {
                if (doStart) {
                    if ( this->startComponentsGroup(thisGroup) ) {
                        Logger::log().logf(Logger::Info, "DeploymentComponent::kickStart2",
                                           "Successfully loaded, configured and started components from %s",
                                           configurationfile.c_str());
                        return true;
                    } else {
                        Logger::log().logf(Logger::Error, "DeploymentComponent::kickStart2",
                                           "Failed to start a component: aborting kick-start.");
                        startOk = false;
                    }
                } else {
                    Logger::log().logf(Logger::Info, "DeploymentComponent::kickStart2",
                                       "Successfully loaded and configured (but did not start) components from %s",
                                       configurationfile.c_str());
                    return true;
                }
            } else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::kickStart2",
                                   "Failed to configure a component: aborting kick-start.");
                configureOk = false;
            }
        } else {
            Logger::log().logf(Logger::Error, "DeploymentComponent::kickStart2",
                               "Failed to load a component: aborting kick-start.");
            loadOk = false;
        }
        return false;
    }

    bool DeploymentComponent::kickOutAll()
    {
        bool    ok = true;
        while (nextGroup != -1 )
        {
            ok &= kickOutGroup(nextGroup);
            --nextGroup;
        }
        // reset group counter to zero
        nextGroup = 0;
        return ok;
    }

    bool DeploymentComponent::kickOutGroup(const int group)
    {
        bool sret = this->stopComponentsGroup(group);
        bool cret = this->cleanupComponentsGroup(group);
        bool uret = this->unloadComponentsGroup(group);
        if ( sret && cret && uret) {
            Logger::log().logf(Logger::Info, "DeploymentComponent::kickOutGroup",
                               "Kick-out of group %d successful.", group);
            return true;
        }
        // Diagnostics:
        Logger::log().logf(Logger::Critical, "DeploymentComponent::kickOutGroup",
                           "Kick-out of group %d failed:%s%s%s",
                           group,
                           !sret ? " stopComponents() failed." : "",
                           !cret ? " cleanupComponents() failed." : "",
                           !uret ? " unloadComponents() failed." : "");
        return false;
    }

    bool DeploymentComponent::createConnectionMapFromPortsTag(RTT::Property<RTT::PropertyBag>& comp,
                                                              RTT::TaskContext* c,
                                                              const bool ignoreNonexistentPorts)
    {
        assert(0 != c);

        bool valid = true;

        // connect ports 'Ports' tag is optional.
        RTT::Property<RTT::PropertyBag>* ports = comp.value().getPropertyType<PropertyBag>("Ports");
        if ( ports != 0 ) {
            for (RTT::PropertyBag::iterator pit = ports->value().begin(); pit != ports->value().end(); pit++) {
                Property<string> portcon = *pit;
                if ( !portcon.ready() ) {
                    Logger::log().logf(Logger::Error, "DeploymentComponent::createConnectionMapFromPortsTag",
                                       "RTT::Property '%s' is not of type 'string'.",
                                       (*pit)->getName().c_str());
                    valid = false;
                    continue;
                }
                base::PortInterface* p = c->ports()->getPort( portcon.getName() );
                if ( !p ) {
                    if (ignoreNonexistentPorts)
                    {
                        Logger::log().logf(Logger::Info, "DeploymentComponent::createConnectionMapFromPortsTag",
                                           "Component '%s' does not have a Port '%s'. Will try to connect again later.",
                                           c->getName().c_str(), portcon.getName().c_str());
                        continue;   // ignore this issue
                    } else {
                        Logger::log().logf(Logger::Error, "DeploymentComponent::createConnectionMapFromPortsTag",
                                           "Component '%s' does not have a Port '%s'.",
                                           c->getName().c_str(), portcon.getName().c_str());
                        valid = false;
                    }
                }
                // store the port
                if (valid){
                    string conn_name = portcon.value(); // reads field of property
                    bool to_add = true;
                    // go through the vector to avoid duplicate items.
                    // NOTE the sizes conmap[conn_name].ports.size() and conmap[conn_name].owners.size() are supposed to be equal
                    for(unsigned int a=0; a < conmap[conn_name].ports.size(); a++)
                    {
                        if(  conmap[conn_name].ports.at(a) == p && conmap[conn_name].owners.at(a) == c)
                        {
                            to_add = false;
                            continue;
                        }
                    }

                    if(to_add)
                    {
                        Logger::log().logf(Logger::Debug, "DeploymentComponent::createConnectionMapFromPortsTag",
                                           "storing Port: %s.%s in %s",
                                           c->getName().c_str(), portcon.getName().c_str(), conn_name.c_str());
                        conmap[conn_name].ports.push_back( p );
                        conmap[conn_name].owners.push_back( c );
                    }
                }
            }
        }
        return valid;
    }

    bool DeploymentComponent::loadConfiguration(const std::string& configurationfile)
    {
        return this->loadComponents(configurationfile);
    }

    bool DeploymentComponent::loadComponents(const std::string& configurationfile)
    {
        bool valid = loadComponentsInGroup(configurationfile, nextGroup);
        ++nextGroup;
        return valid;
    }

    bool DeploymentComponent::loadComponentsInGroup(const std::string& configurationfile,
                                                    const int group)
    {
        auto deployment_lock = lockDeployment();
        if (deploymentClosing.load())
            return false;
        RTT::PropertyBag from_file;
        Logger::log().logf(Logger::Info, "DeploymentComponent::loadComponents",
                           "Loading '%s' in group %d.", configurationfile.c_str(), group);
        // demarshalling failures:
        bool failure = false;
        // semantic failures:
        bool valid = validConfig.get();
        marsh::PropertyDemarshaller demarshaller(configurationfile);
        try {
            if ( demarshaller.deserialize( from_file ) )
                {
                    valid = true;
                    Logger::log().logf(Logger::Info, "DeploymentComponent::loadComponents",
                                       "Validating new configuration...");
                    if ( from_file.empty() ) {
                        Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                           "Configuration was empty !");
                        valid = false;
                    }

                    //for (RTT::PropertyBag::Names::iterator it= nams.begin();it != nams.end();it++) {
                    for (RTT::PropertyBag::iterator it= from_file.begin(); it!=from_file.end();it++) {
                        // Read in global options.
                        if ( (*it)->getName() == "Import" ) {
                            RTT::Property<std::string> importp = *it;
                            if ( !importp.ready() ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                   "Found 'Import' statement, but it is not of type='string'.");
                                valid = false;
                                continue;
                            }
                            if ( this->import( importp.get() ) == false )
                                valid = false;
                            continue;
                        }
                        if ( (*it)->getName() == "LoadLibrary" ) {
                            RTT::Property<std::string> importp = *it;
                            if ( !importp.ready() ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                   "Found 'LoadLibrary' statement, but it is not of type='string'.");
                                valid = false;
                                continue;
                            }
                            if ( this->loadLibrary( importp.get() ) == false )
                                valid = false;
                            continue;
                        }
                        if ( (*it)->getName() == "Path" ) {
                            RTT::Property<std::string> pathp = *it;
                            if ( !pathp.ready() ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                   "Found 'Path' statement, but it is not of type='string'.");
                                valid = false;
                                continue;
                            }
                            this->path( pathp.get() );
                            continue;
                        }
                        if ( (*it)->getName() == "Include" ) {
                            RTT::Property<std::string> includep = *it;
                            if ( !includep.ready() ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                   "Found 'Include' statement, but it is not of type='string'.");
                                valid = false;
                                continue;
                            }
                            // recursively call this function.
                            if ( this->loadComponentsInGroup( includep.get(), group ) == false )
                                valid = false;
                            continue;
                        }
                        if ( (*it)->getName() == "GlobalsRepository" ) {
                            RTT::Property<RTT::PropertyBag> global = *it;
                            if ( !global.ready() ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                   "Found 'GlobalsRepository' tag, but it is not a complex xml type");
                                valid = false;
                                continue;
                            }
                            // Check for default Global properties to be set.
                            for (RTT::PropertyBag::const_iterator pf = global.rvalue().begin(); pf != global.rvalue().end(); ++pf) {
                                if ( (*pf)->getName() == "Properties" ) {
                                    RTT::Property<RTT::PropertyBag> props = *pf;
                                    if ( !props.ready() ) {
                                        Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                           "Found 'Properties' in 'GlobalsRepository' tag, but it is not of type PropertyBag");
                                        valid = false;
                                        continue;
                                    }
                                    bool ret = updateProperties( *RTT::types::GlobalsRepository::Instance()->properties(), props );
                                    if (!ret) {
                                        Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                           "Failed to configure global properties from configuration file.");
                                        valid = false;
                                    } else {
                                        Logger::log().logf(Logger::Info, "DeploymentComponent::loadComponents",
                                                           "Configured global properties from configuration file.");
                                    }
                                }
                            }
                            continue;
                        }

                        // Check if it is a propertybag.
                        RTT::Property<RTT::PropertyBag> comp = *it;
                        if ( !comp.ready() ) {
                            Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                               "RTT::Property '%s' of type '%s' should be a struct, Include, Path or Import statement.",
                                               (*it)->getName().c_str(), (*it)->getType().c_str());
                            valid = false;
                            continue;
                        }

                        //Check if it is a ConnPolicy
                        // convert to Property<ConnPolicy>
                        Property<ConnPolicy> cp_prop((*it)->getName(),"");
                        assert( cp_prop.ready() );
                        if ( cp_prop.compose( comp ) ) {
                            //It's a connection policy.
#if defined(RTT_VERSION_GTE)
#if RTT_VERSION_GTE(2,8,99)
                            // Set default ConnPolicy
                            if (cp_prop.getName() == "Default") {
                                RTT::ConnPolicy::Default() = cp_prop.get();
                            } else {
#endif
#endif
                                conmap[cp_prop.getName()].policy = cp_prop.get();
#if defined(RTT_VERSION_GTE)
#if RTT_VERSION_GTE(2,8,99)
                            }
#endif
#endif
                            Logger::log().logf(Logger::Debug, "DeploymentComponent::loadComponents",
                                               "Saw connection policy %s", (*it)->getName().c_str());
                            continue;
                        }

                        // Parse the options before creating the component:
                        for (RTT::PropertyBag::const_iterator optit= comp.rvalue().begin(); optit != comp.rvalue().end();optit++) {
                            if ( valid_names.find( (*optit)->getName() ) == valid_names.end() ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                   "Unknown type syntax: '%s' in component struct %s",
                                                   (*optit)->getName().c_str(), comp.getName().c_str());
                                valid = false;
                                continue;
                            }
                            if ( (*optit)->getName() == "AutoConnect" ) {
                                RTT::Property<bool> ps = comp.rvalue().getProperty("AutoConnect");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "AutoConnect must be of type <boolean>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].autoconnect = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "AutoStart" ) {
                                RTT::Property<bool> ps = comp.rvalue().getProperty("AutoStart");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "AutoStart must be of type <boolean>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].autostart = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "AutoSave" ) {
                                RTT::Property<bool> ps = comp.rvalue().getProperty("AutoSave");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "AutoSave must be of type <boolean>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].autosave = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "AutoConf" ) {
                                RTT::Property<bool> ps = comp.rvalue().getProperty("AutoConf");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "AutoConf must be of type <boolean>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].autoconf = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "Server" ) {
                                RTT::Property<bool> ps = comp.rvalue().getProperty("Server");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "Server must be of type <boolean>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].server = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "Service" || (*optit)->getName() == "Plugin"  || (*optit)->getName() == "Provides") {
                                RTT::Property<string> ps = *optit;
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "%s must be of type <string>",
                                                       (*optit)->getName().c_str());
                                    valid = false;
                                } else {
                                    compmap[comp.getName()].plugins.push_back(ps.value());
                                }
                                continue;
                            }
                            if ( (*optit)->getName() == "UseNamingService" ) {
                                RTT::Property<bool> ps = comp.rvalue().getProperty("UseNamingService");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "UseNamingService must be of type <boolean>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].use_naming = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "PropertyFile" ) {
                                RTT::Property<string> ps = comp.rvalue().getProperty("PropertyFile");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "PropertyFile must be of type <string>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].configfile = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "UpdateProperties" ) {
                                RTT::Property<string> ps = comp.rvalue().getProperty("UpdateProperties");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "UpdateProperties must be of type <string>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].configfile = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "LoadProperties" ) {
                                RTT::Property<string> ps = comp.rvalue().getProperty("LoadProperties");
                                if (!ps.ready()) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "LoadProperties must be of type <string>");
                                    valid = false;
                                } else
                                    compmap[comp.getName()].configfile = ps.get();
                                continue;
                            }
                            if ( (*optit)->getName() == "Properties" ) {
                                base::PropertyBase* ps = comp.rvalue().getProperty("Properties");
                                if (!ps) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "Properties must be a <struct>");
                                    valid = false;
                                }
                                continue;
                            }
                            if ( (*optit)->getName() == "RunScript" ) {
                                base::PropertyBase* ps = comp.rvalue().getProperty("RunScript");
                                if (!ps) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "RunScript must be of type <string>");
                                    valid = false;
                                }
                                continue;
                            }
                            if ( (*optit)->getName() == "ProgramScript" ) {
                                base::PropertyBase* ps = comp.rvalue().getProperty("ProgramScript");
                                if (!ps) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "ProgramScript must be of type <string>");
                                    valid = false;
                                }
                                continue;
                            }
                            if ( (*optit)->getName() == "StateMachineScript" ) {
                                base::PropertyBase* ps = comp.rvalue().getProperty("StateMachineScript");
                                if (!ps) {
                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                       "StateMachineScript must be of type <string>");
                                    valid = false;
                                }
                                continue;
                            }
                        }

                        // Check if we know or are this component.
                        RTT::TaskContext* c = 0;
                        if ( (*it)->getName() == "this" || (*it)->getName() == this->getName() )
                            c = this;
                        else
                            c = this->getPeer( (*it)->getName() );
                        if ( !c ) {
                            // try to load it.
                            if (this->loadComponent( (*it)->getName(), comp.rvalue().getType() ) == false) {
                                Logger::log().logf(Logger::Warning, "DeploymentComponent::loadComponents",
                                                   "Could not configure '%s': No such peer.",
                                                   (*it)->getName().c_str());
                                valid = false;
                                continue;
                            }
                            c = compmap[(*it)->getName()].instance;

                            // The component is added to a group only when it is loaded, not when a service is added or changed.
                            compmap[(*it)->getName()].group = group;
                            Logger::log().logf(Logger::Info, "DeploymentComponent::loadComponents",
                                               "Component %s added to group %d.",
                                               (*it)->getName().c_str(), group);
                        } else {
                            // If the user added c as a peer (outside of Deployer) store the pointer
                            compmap[(*it)->getName()].instance = c;
                        }

                        assert(c);

                        // load plugins/services:
                        vector<string>& services = compmap[(*it)->getName()].plugins;
                        for (vector<string>::iterator svit = services.begin(); svit != services.end(); ++svit) {
                            if ( c->provides()->hasService( *svit ) == false) {
                                PluginLoader::Instance()->loadService(*svit, c);
                            }
                        }

                        // set PropFile name if present
                        if ( comp.value().getProperty("PropFile") )  // PropFile is deprecated
                            comp.value().getProperty("PropFile")->setName("PropertyFile");

                        // connect ports 'Ports' tag is optional.
                        valid &= createConnectionMapFromPortsTag(comp, c, true);

                        // Setup the connections from this
                        // component to the others.
                        if ( comp.value().find("Peers") != 0) {
                            RTT::Property<RTT::PropertyBag> nm = comp.value().find("Peers");
                            if ( !nm.ready() ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                   "RTT::Property 'Peers' must be a 'struct', was type %s",
                                                   comp.value().find("Peers")->getType().c_str());
                                valid = false;
                            } else {
                                for (RTT::PropertyBag::const_iterator it= nm.rvalue().begin(); it != nm.rvalue().end();it++) {
                                    RTT::Property<std::string> pr = *it;
                                    if ( !pr.ready() ) {
                                        Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                           "RTT::Property 'Peer' does not have type 'string'.");
                                        valid = false;
                                        continue;
                                    }
                                }
                            }
                        }

                        // Read the activity profile if present.
                        if ( comp.value().find("Activity") != 0) {
                            RTT::Property<RTT::PropertyBag> nm = comp.value().find("Activity");
                            if ( !nm.ready() ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                   "RTT::Property 'Activity' must be a 'struct'.");
                                valid = false;
                            } else {
                                if ( nm.rvalue().getType() == "PeriodicActivity" ) {
                                    RTT::Property<double> per = nm.rvalue().getProperty("Period"); // work around RTT 1.0.2 bug.
                                    if ( !per.ready() ) {
                                        Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                           "Please specify period <double> of PeriodicActivity.");
                                        valid = false;
                                    }
                                    RTT::Property<int> prio = nm.rvalue().getProperty("Priority"); // work around RTT 1.0.2 bug
                                    if ( !prio.ready() ) {
                                        Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                           "Please specify priority <short> of PeriodicActivity.");
                                        valid = false;
                                    }

                                    unsigned cpu_affinity = ~0; // default to all CPUs
                                    RTT::Property<unsigned> cpu_affinity_prop = nm.rvalue().getProperty("CpuAffinity");
                                    if(cpu_affinity_prop.ready()) {
                                        cpu_affinity = cpu_affinity_prop.get();
                                    }
                                    // else ignore as is optional

                                    RTT::Property<string> sched;
                                    if (nm.rvalue().getProperty("Scheduler") )
                                        sched = nm.rvalue().getProperty("Scheduler"); // work around RTT 1.0.2 bug
                                    int scheduler = ORO_SCHED_RT;
                                    if ( sched.ready() ) {
                                        scheduler = string_to_oro_sched( sched.get());
                                        if (scheduler == -1 )
                                            valid = false;
                                    }
                                    if (valid) {
                                        this->setNamedActivity(comp.getName(), nm.rvalue().getType(), per.get(), prio.get(), scheduler, cpu_affinity );
                                    }
                                } else
                                    if ( nm.rvalue().getType() == "Activity" || nm.rvalue().getType() == "NonPeriodicActivity" ) {
                                        RTT::Property<double> per = nm.rvalue().getProperty("Period");
                                        if ( !per.ready() ) {
                                            per = Property<double>("p","",0.0); // default to 0.0
                                        }
                                        RTT::Property<int> prio = nm.rvalue().getProperty("Priority");
                                        if ( !prio.ready() ) {
                                            Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                               "Please specify priority <short> of Activity.");
                                            valid = false;
                                        }

                                        unsigned int cpu_affinity = ~0; // default to all CPUs
                                        RTT::Property<unsigned int> cpu_affinity_prop = nm.rvalue().getProperty("CpuAffinity");
                                        if(cpu_affinity_prop.ready()) {
                                            cpu_affinity = cpu_affinity_prop.get();
                                        }
                                        // else ignore as is optional

                                        RTT::Property<string> sched = nm.rvalue().getProperty("Scheduler");
                                        int scheduler = ORO_SCHED_RT;
                                        if ( sched.ready() ) {
                                            scheduler = string_to_oro_sched( sched.get());
                                            if (scheduler == -1 )
                                                valid = false;
                                        }
                                        if (valid) {
                                            this->setNamedActivity(comp.getName(), nm.rvalue().getType(), per.get(), prio.get(), scheduler, cpu_affinity );
                                        }
                                    } else
                                        if ( nm.rvalue().getType() == "SlaveActivity" ) {
                                            double period = 0.0;
                                            string master;
                                            if ( nm.rvalue().getProperty("Master") ) {
                                                master = nm.rvalue().getPropertyType<string>("Master")->get();
                                                if (valid) {
                                                    this->setNamedActivity(comp.getName(), nm.rvalue().getType(), period, 0, 0, master );
                                                }
                                            } else {
                                                // No master given.
                                                if ( nm.rvalue().getProperty("Period") )
                                                    period = nm.rvalue().getPropertyType<double>("Period")->get();
                                                if (valid) {
                                                    this->setNamedActivity(comp.getName(), nm.rvalue().getType(), period, 0, 0 );
                                                }
                                            }
                                        } else
                                            if ( nm.rvalue().getType() == "SequentialActivity" ) {
                                                this->setNamedActivity(comp.getName(), nm.rvalue().getType(), 0, 0, 0 );
											} else
                                                if ( nm.rvalue().getType() == "FileDescriptorActivity" ) {
                                                    RTT::Property<double> per = nm.rvalue().getProperty("Period");
                                                    if ( !per.ready() ) {
                                                        per = Property<double>("p","",0.0); // default to 0.0
                                                    }
                                                    // else ignore as is optional

                                                    RTT::Property<int> prio = nm.rvalue().getProperty("Priority");
                                                    if ( !prio.ready() ) {
                                                        Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                                           "Please specify priority <short> of FileDescriptorActivity.");
                                                        valid = false;
                                                    }

                                                    unsigned int cpu_affinity = ~0; // default to all CPUs
                                                    RTT::Property<unsigned int> cpu_affinity_prop = nm.rvalue().getProperty("CpuAffinity");
                                                    if(cpu_affinity_prop.ready()) {
                                                        cpu_affinity = cpu_affinity_prop.get();
                                                    }
                                                    // else ignore as is optional

                                                    RTT::Property<string> sched = nm.rvalue().getProperty("Scheduler");
                                                    int scheduler = ORO_SCHED_RT;
                                                    if ( sched.ready() ) {
                                                        scheduler = string_to_oro_sched( sched.get());
                                                        if (scheduler == -1 )
                                                            valid = false;
                                                    }
                                                    if (valid) {
                                                        this->setNamedActivity(comp.getName(), nm.rvalue().getType(), per.get(), prio.get(), scheduler, cpu_affinity );
                                                    }
                                                } else {
                                                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                                                       "Unknown activity type: %s",
                                                                       nm.rvalue().getType().c_str());
                                                    valid = false;
                                                }
                            }
                        } else {
                            // no 'Activity' element, default to Slave:
                            //this->setNamedActivity(comp.getName(), "extras::SlaveActivity", 0.0, 0, 0 );
                        }
                        // put this component in the root config.
                        // existing component options are updated, new components are
                        // added to the back.
                        // great: a hack to allow 'CompName.ior' as property name.
                        string delimiter("@!#?<!");
                        bool ret = updateProperty( root, from_file, comp.getName(), delimiter );
                        if (!ret) {
                            Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                               "Failed to store deployment properties for component %s",
                                               comp.getName().c_str());
                            valid = false;
                        }
                    }

                    deletePropertyBag( from_file );
                }
            else
                {
                    Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                       "Some error occured while parsing %s",
                                       configurationfile.c_str());
                    failure = true;
                }
        } catch (...)
            {
                Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponents",
                                   "Uncaught exception in loadcomponents() !");
                failure = true;
            }
        validConfig.set(valid);
        return !failure && valid;
    }

    bool DeploymentComponent::createDataPortConnections(const bool skipUnconnected)
    {
        bool valid = true;

        for(ConMap::iterator it = conmap.begin(); it != conmap.end(); ++it) {
            ConnectionData *connection =  &(it->second);
            std::string connection_name = it->first;

            // Set the connection name as default name_id if none was given explicitly
            if (connection->policy.name_id.empty()) {
                connection->policy.name_id = connection_name;
            }

            if ( connection->ports.size() == 1 ){
                string owner = connection->owners[0]->getName();
                string portname = connection->ports.front()->getName();
                string porttype = dynamic_cast<InputPortInterface*>(connection->ports.front() ) ? "InputPort" : "OutputPort";

                // a connection that currently has only one port may end up with
                // two ports later on, so we skip this connection for now.
                if (skipUnconnected)
                {
                    Logger::log().logf(Logger::Info, "DeploymentComponent::createDataPortConnections",
                                       "Skipping connection with name %s with only one Port %s from %s",
                                       connection_name.c_str(), portname.c_str(), owner.c_str());
                }
                else if ( connection->ports.front()->createStream( connection->policy ) == false) {
                    Logger::log().logf(Logger::Warning, "DeploymentComponent::createDataPortConnections",
                                       "Creating stream with name %s with Port %s from %s failed.",
                                       connection_name.c_str(), portname.c_str(), owner.c_str());
                } else {
                    Logger::log().logf(Logger::Info, "DeploymentComponent::createDataPortConnections",
                                       "Component %s's %s %s will stream to %s",
                                       owner.c_str(), porttype.c_str(), portname.c_str(),
                                       connection->policy.name_id.c_str());
                }
                continue;
            }
            // first find all write ports.
            base::PortInterface* writer = 0;
            ConnectionData::Ports::iterator p = connection->ports.begin();

            // If one of the ports is connected, use that one as writer to connect to.
            vector<OutputPortInterface*> writers;
            while (p !=connection->ports.end() ) {
                if ( OutputPortInterface* out = dynamic_cast<base::OutputPortInterface*>( *p ) ) {
                    if ( writer ) {
                        Logger::log().logf(Logger::Info, "DeploymentComponent::createDataPortConnections",
                                           "Forming multi-output connections with additional OutputPort %s.",
                                           (*p)->getName().c_str());
                    } else
                    writer = *p;
                    writers.push_back( out );
                    std::string owner = it->second.owners[p - it->second.ports.begin()]->getName();
                    Logger::log().logf(Logger::Info, "DeploymentComponent::createDataPortConnections",
                                       "Component %s's OutputPort %s will write topic %s",
                                       owner.c_str(), writer->getName().c_str(), it->first.c_str());
                }
                ++p;
            }

            // Inform the user of non-optimal connections:
            if ( writer == 0 ) {
                Logger::log().logf(Logger::Error, "DeploymentComponent::createDataPortConnections",
                                   "No OutputPort listed that writes %s", it->first.c_str());
                valid = false;
                break;
            }

            // connect all ports to writer
            p = connection->ports.begin();
            vector<OutputPortInterface*>::iterator w = writers.begin();

            while (w != writers.end() ) {
                while (p != connection->ports.end() ) {
                    // connect all readers to the list of writers
                    if ( dynamic_cast<base::InputPortInterface*>( *p ) )
                    {
                        string owner = connection->owners[p - connection->ports.begin()]->getName();
                        // only try to connect p if it is not in the same connection of writer.
                        // OK. p is definately no part of writer's connection. Try to connect and flag errors if it fails.
                        if ( (*w)->connectTo( *p, connection->policy ) == false) {
                            Logger::log().logf(Logger::Error, "DeploymentComponent::createDataPortConnections",
                                               "Could not subscribe InputPort %s.%s to topic %s/%s",
                                               owner.c_str(), (*p)->getName().c_str(),
                                               (*w)->getName().c_str(), connection_name.c_str());
                            valid = false;
                        } else {
                            Logger::log().logf(Logger::Info, "DeploymentComponent::createDataPortConnections",
                                               "Subscribed InputPort %s.%s to topic %s/%s",
                                               owner.c_str(), (*p)->getName().c_str(),
                                               (*w)->getName().c_str(), connection_name.c_str());
                        }
                    }
                    ++p;
                }
                ++w;
                p = connection->ports.begin();
            }
        }
        return valid;
    }

    bool DeploymentComponent::configureComponents()
    {
        // do all groups
        bool valid = true;
        for (int group = 0; group <= nextGroup; ++group) {
            valid &= configureComponentsGroup(group);
        }
        return valid;
    }

    bool DeploymentComponent::configureComponentsGroup(const int group)
    {
        if ( root.empty() ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                               "No components loaded by DeploymentComponent !");
            return false;
        }

        bool valid = true;
        Logger::log().logf(Logger::Info, "DeploymentComponent::configureComponentsGroup",
                           "Configuring components in group %d", group);

        // Connect peers
        for (RTT::PropertyBag::iterator it= root.begin(); it!=root.end();it++) {

            RTT::Property<RTT::PropertyBag> comp = *it;

            // only components in this group
            if (group != compmap[comp.getName()].group) {
                continue;
            }

            RTT::TaskContext* peer = compmap[comp.getName()].instance;
            if ( !peer ) {
                Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                                   "Peer not found: %s", comp.getName().c_str());
                valid=false;
                continue;
            }

            compmap[comp.getName()].instance = peer;

            // Setup the connections from each component to the
            // others.
            RTT::Property<RTT::PropertyBag> peers = comp.rvalue().find("Peers");
            if ( peers.ready() )
                for (RTT::PropertyBag::const_iterator it= peers.rvalue().begin(); it != peers.rvalue().end();it++) {
                    RTT::Property<string> nm = (*it);
                    if ( nm.ready() )
                        {
                            if ( this->addPeer( compmap[comp.getName()].instance->getName(), nm.value() ) == false ) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                                                   "%s can't make %s a peer of %s",
                                                   this->getName().c_str(), nm.value().c_str(),
                                                   compmap[comp.getName()].instance->getName().c_str());
                                valid = false;
                            } else {
                                Logger::log().logf(Logger::Info, "DeploymentComponent::configureComponentsGroup",
                                                   "%s makes %s a peer of %s",
                                                   this->getName().c_str(), nm.value().c_str(),
                                                   compmap[comp.getName()].instance->getName().c_str());
                            }
                        }
                    else {
                        Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                                           "Wrong property type in Peers struct. Expected property of type 'string', got type %s",
                                           (*it)->getType().c_str());
                        valid = false;
                    }
                }
        }

        // Create data port connections:
        valid &= createDataPortConnections(true);

        // Autoconnect ports. The port name is the topic name.
        for (RTT::PropertyBag::iterator it= root.begin(); it!=root.end();it++) {
            RTT::Property<RTT::PropertyBag> comp = *it;
            if ( !comp.ready() )
                continue;

            // only components in this group
            if (group != compmap[ comp.getName() ].group) {
                continue;
            }

            RTT::TaskContext* peer = compmap[ comp.getName() ].instance;

            // only autoconnect if AutoConnect == 1 and peer has AutoConnect == 1
            // There should only be one writer; more than one will lead to undefined behaviour.
            // reader<->reader connections will silently fail and be retried once a writer is found.
            if ( compmap[comp.getName()].autoconnect ) {
                // XXX/TODO This is broken: we should not rely on the peers to implement AutoConnect!
                RTT::TaskContext::PeerList peers = peer->getPeerList();
                for(RTT::TaskContext::PeerList::iterator pit = peers.begin(); pit != peers.end(); ++pit) {
                    if ( compmap.count( *pit ) && compmap[*pit].autoconnect ) {
                        RTT::TaskContext* other = peer->getPeer( *pit );
                        valid = RTT::connectPorts( peer, other ) && valid;
                    }
                }
            }
        }

        // Main configuration
        for (RTT::PropertyBag::iterator it= root.begin(); it!=root.end();it++) {

            RTT::Property<RTT::PropertyBag> comp = *it;

            // only components in this group
            if (group != compmap[ comp.getName() ].group) {
                continue;
            }

            RTT::Property<string> dummy;
            RTT::TaskContext* peer = compmap[ comp.getName() ].instance;

            // do not configure when not stopped.
            if ( peer->getTaskState() > Stopped) {
                Logger::log().logf(Logger::Warning, "DeploymentComponent::configureComponentsGroup",
                                   "Component %s doesn't need to be configured (already Running).",
                                   peer->getName().c_str());
                continue;
            }

            // Check for default properties to set.
            for (RTT::PropertyBag::const_iterator pf = comp.rvalue().begin(); pf!= comp.rvalue().end(); ++pf) {
                // set PropFile name if present
                if ( (*pf)->getName() == "Properties"){
                    RTT::Property<RTT::PropertyBag> props = *pf; // convert to type.
                    bool ret = updateProperties( *peer->properties(), props);
                    if (!ret) {
                        Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                                           "Failed to configure properties from main configuration file for component %s",
                                           comp.getName().c_str());
                        valid = false;
                    } else {
                        Logger::log().logf(Logger::Info, "DeploymentComponent::configureComponentsGroup",
                                           "Configured Properties of %s from main configuration file.",
                                           comp.getName().c_str());
                    }
                }
            }
            // Load/update from property files.
            for (RTT::PropertyBag::const_iterator pf = comp.rvalue().begin(); pf!= comp.rvalue().end(); ++pf) {
                // set PropFile name if present
                if ( (*pf)->getName() == "PropertyFile" || (*pf)->getName() == "UpdateProperties" || (*pf)->getName() == "LoadProperties"){
                    dummy = *pf; // convert to type.
                    string filename = dummy.get();
                    marsh::PropertyLoader pl(peer);
                    bool strict = (*pf)->getName() == "PropertyFile" ? true : false;
                    bool load = (*pf)->getName() == "LoadProperties" ? true : false;
                    bool ret;
                    if (!load)
                        ret = pl.configure( filename, strict );
                    else
                        ret = pl.load(filename);
                    if (!ret) {
                        Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                                           "Failed to configure properties for component %s",
                                           comp.getName().c_str());
                        valid = false;
                    } else {
                        Logger::log().logf(Logger::Info, "DeploymentComponent::configureComponentsGroup",
                                           "Configured Properties of %s from %s",
                                           comp.getName().c_str(), filename.c_str());
                        compmap[ comp.getName() ].loadedProperties = true;
                    }
                }
            }

            // Attach activities
            if ( compmap[comp.getName()].act ) {
                if ( peer->getActivity() ) {
                    Logger::log().logf(Logger::Info, "DeploymentComponent::configureComponentsGroup",
                                       "Re-setting activity of %s", comp.getName().c_str());
                } else {
                    Logger::log().logf(Logger::Info, "DeploymentComponent::configureComponentsGroup",
                                       "Setting activity of %s", comp.getName().c_str());
                }
                if (peer->setActivity( compmap[comp.getName()].act ) == false ) {
                    valid = false;
                    Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                                       "Failed to set Activity of %s", comp.getName().c_str());
                } else {
                    assert( peer->engine()->getActivity() == compmap[comp.getName()].act );
                    compmap[comp.getName()].act = 0; // drops ownership.
                }
            }

            // Load scripts in order of appearance
            for (RTT::PropertyBag::const_iterator ps = comp.rvalue().begin(); ps!= comp.rvalue().end(); ++ps) {
                RTT::Property<string> script;
                if ( (*ps)->getName() == "RunScript" )
                    script = *ps;
                if ( script.ready() ) {
                    valid = valid && peer->getProvider<Scripting>("scripting")->runScript( script.get() );
                }
                // deprecated:
                RTT::Property<string> pscript;
                if ( (*ps)->getName() == "ProgramScript" )
                    pscript = *ps;
                if ( pscript.ready() ) {
                    valid = valid && peer->getProvider<Scripting>("scripting")->loadPrograms( pscript.get() );
                }
                RTT::Property<string> sscript;
                if ( (*ps)->getName() == "StateMachineScript" )
                    sscript = *ps;
                if ( sscript.ready() ) {
                    valid = valid && peer->getProvider<Scripting>("scripting")->loadStateMachines( sscript.get() );
                }
            }

            // AutoConf
            if (compmap[comp.getName()].autoconf )
                {
                    if( !peer->isRunning() )
                        {
                            OperationCaller<bool(void)> peerconfigure = peer->getOperation("configure");
                            if ( peerconfigure() == false) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                                                   "Component %s returns false in configure()",
                                                   peer->getName().c_str());
                                valid = false;
                            }
                        }
                    else
                        Logger::log().logf(Logger::Warning, "DeploymentComponent::configureComponentsGroup",
                                           "Apparently component %s don't need to be configured (already Running).",
                                           peer->getName().c_str());
                }

            // scan for connection changes due to ports created in configure()
            valid &= createConnectionMapFromPortsTag(comp, peer, false);

        }   // for root

        // Create data port connections for any newly created connections/ports.
        valid &= createDataPortConnections(false);

        // Finally, report success/failure (but ignore components that are actually running, as
        // they will have been configured/started previously)
        if (!valid) {
            for ( CompList::iterator cit = comps.begin(); cit != comps.end(); ++cit) {
                ComponentData* cd = &(compmap[*cit]);
                if ( group == cd->group && cd->loaded && cd->autoconf &&
                     (cd->instance->getTaskState() != TaskCore::Stopped) &&
                     (cd->instance->getTaskState() != TaskCore::Running))
                    Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponentsGroup",
                                       "Failed to configure component %s: state is %d",
                                       cd->instance->getName().c_str(),
                                       static_cast<int>(cd->instance->getTaskState()));
            }
        } else {
            Logger::log().logf(Logger::Info, "DeploymentComponent::configureComponentsGroup",
                               "Configuration successful for group %d.", group);
        }

        validConfig.set(valid);
        return valid;
    }

    bool DeploymentComponent::startComponents()
    {
        // do all groups
        bool valid = true;
        for (int group = 0; group <= nextGroup; ++group) {
            valid &= startComponentsGroup(group);
        }
        return valid;
    }

    bool DeploymentComponent::startComponentsGroup(const int group)
    {
        if (validConfig.get() == false) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::startComponentsGroup",
                               "Not starting components with invalid configuration.");
            return false;
        }
        bool valid = true;
        for (RTT::PropertyBag::iterator it= root.begin(); it!=root.end();it++) {

            // only components in this group
            if (group != compmap[(*it)->getName()].group) {
                continue;
            }

            TaskContext* peer = compmap[(*it)->getName()].instance;

            // only start if not already running (peer may have been previously
            // loaded/configured/started from the site deployer file)
            if (peer->isRunning())
            {
                continue;
            }

            // AutoStart
	    OperationCaller<bool(void)> peerstart = peer->getOperation("start");
            if (compmap[(*it)->getName()].autostart )
                if ( !peer || ( !peer->isRunning() && peerstart() == false) )
                    valid = false;
        }
        // Finally, report success/failure:
        if (!valid) {
            for ( CompList::iterator cit = comps.begin(); cit != comps.end(); ++cit) {
                ComponentData* it = &(compmap[*cit]);

                // only components in this group
                if (group != it->group) {
                    continue;
                }

                if ( it->instance == 0 ) {
                    Logger::log().logf(Logger::Error, "DeploymentComponent::startComponentsGroup",
                                       "Failed to start component %s: not found.", cit->c_str());
                    continue;
                }
                if ( it->autostart && it->instance->getTaskState() != base::TaskCore::Running )
                    Logger::log().logf(Logger::Error, "DeploymentComponent::startComponentsGroup",
                                       "Failed to start component %s", it->instance->getName().c_str());
            }
        } else {
                Logger::log().logf(Logger::Info, "DeploymentComponent::startComponentsGroup",
                                   "Startup of 'AutoStart' components successful for group %d.", group);
        }
        return valid;
    }

    bool DeploymentComponent::stopComponents()
    {
        // do all groups
        bool valid = true;
        for (int group = nextGroup ; group != -1; --group) {
            valid &= stopComponentsGroup(group);
        }
        return valid;
    }

    bool DeploymentComponent::stopComponentsGroup(const int group)
    {
        Logger::log().logf(Logger::Info, "DeploymentComponent::stopComponentsGroup",
                           "Stopping group %d", group);
        bool valid = true;
        // 1. Stop all activities, give components chance to cleanup.
        for ( CompList::reverse_iterator cit = comps.rbegin(); cit != comps.rend(); ++cit) {
            ComponentData* it = &(compmap[*cit]);
            if ( (group == it->group) && it->instance && !it->proxy ) {
                OperationCaller<bool(void)> instancestop = it->instance->getOperation("stop");
                if ( !it->instance->isRunning() ||
                     instancestop() ) {
                    Logger::log().logf(Logger::Info, "DeploymentComponent::stopComponentsGroup",
                                       "Stopped %s", it->instance->getName().c_str());
                } else {
                    Logger::log().logf(Logger::Error, "DeploymentComponent::stopComponentsGroup",
                                       "Could not stop loaded Component %s",
                                       it->instance->getName().c_str());
                    valid = false;
                }
            }
        }
        return valid;
    }

    bool DeploymentComponent::cleanupComponents()
    {
        // do all groups
        bool valid = true;
        for (int group = nextGroup ; group != -1; --group) {
            valid &= cleanupComponentsGroup(group);
        }
        return valid;
    }

    bool DeploymentComponent::cleanupComponentsGroup(const int group)
    {
        bool valid = true;
        Logger::log().logf(Logger::Info, "DeploymentComponent::cleanupComponentsGroup",
                           "Cleaning up group %d", group);
        // 1. Cleanup all activities, give components chance to cleanup.
        for ( CompList::reverse_iterator cit = comps.rbegin(); cit != comps.rend(); ++cit) {
            ComponentData* it = &(compmap[*cit]);

            // only components in this group
            if (group != it->group) {
                continue;
            }

            if (it->instance && !it->proxy) {
                if ( it->instance->getTaskState() <= base::TaskCore::Stopped ) {
                    if ( it->autosave && !it->configfile.empty()) {
                        if (it->loadedProperties) {
                            string file = it->configfile; // get file name
                            PropertyLoader pl(it->instance);
                            bool ret = pl.save( file, true ); // save all !
                            if (!ret) {
                                Logger::log().logf(Logger::Error, "DeploymentComponent::cleanupComponentsGroup",
                                                   "Failed to save properties for component %s",
                                                   it->instance->getName().c_str());
                                valid = false;
                            } else {
                                Logger::log().logf(Logger::Info, "DeploymentComponent::cleanupComponentsGroup",
                                                   "Refusing to save property file that was not loaded for %s",
                                                   it->instance->getName().c_str());
                            }
                        } else if (it->autosave) {
                            Logger::log().logf(Logger::Error, "DeploymentComponent::cleanupComponentsGroup",
                                               "AutoSave set but no property file specified. Specify one using the UpdateProperties simple element.");
                        }
                    } else if (it->autosave) {
                        Logger::log().logf(Logger::Error, "DeploymentComponent::cleanupComponentsGroup",
                                           "AutoSave set but no property file specified. Specify one using the UpdateProperties simple element.");
                    }
                    OperationCaller<bool(void)> instancecleanup = it->instance->getOperation("cleanup");
                    instancecleanup();
                    Logger::log().logf(Logger::Info, "DeploymentComponent::cleanupComponentsGroup",
                                       "Cleaned up %s", it->instance->getName().c_str());
                } else {
                    Logger::log().logf(Logger::Error, "DeploymentComponent::cleanupComponentsGroup",
                                       "Could not cleanup Component %s (not Stopped)",
                                       it->instance->getName().c_str());
                    valid = false;
                }
            }
        }
        return valid;
    }

    bool DeploymentComponent::unloadComponents()
    {
        // do all groups
        bool valid = true;
        for (int group = nextGroup ; group != -1; --group) {
            valid &= unloadComponentsGroup(group);
        }
        return valid;
    }

    bool DeploymentComponent::unloadComponentsGroup(const int group)
    {
        auto deployment_lock = lockDeployment();
        Logger::log().logf(Logger::Info, "DeploymentComponent::unloadComponentsGroup",
                           "Unloading group %d", group);
        // 2. Disconnect and destroy all components in group
        bool valid = true;
        CompList::reverse_iterator cit = comps.rbegin();
        while ( valid && cit != comps.rend())
            {
                ComponentData* it = &(compmap[*cit]);
                if (group == it->group)
                {
                    // this call modifies comps
                    valid &= this->unloadComponentImpl(compmap.find(*cit));
                    // so restart search
                    cit = comps.rbegin();
                }
                else
                {
                    ++cit;
                }
            }


        return valid;
    }

    void DeploymentComponent::clearConfiguration()
    {
        Logger::log().logf(Logger::Info, "DeploymentComponent::clearConfiguration",
                           "Clearing configuration options.");
        conmap.clear();
        deletePropertyBag( root );
    }

    bool DeploymentComponent::import(const std::string& package)
    {
        return ComponentLoader::Instance()->import( package, "" ); // search in existing search paths
    }

    void DeploymentComponent::path(const std::string& path)
    {
        ComponentLoader::Instance()->setComponentPath( ComponentLoader::Instance()->getComponentPath() + path );
        PluginLoader::Instance()->setPluginPath( PluginLoader::Instance()->getPluginPath() + path );
    }

    bool DeploymentComponent::loadLibrary(const std::string& name)
    {
        return PluginLoader::Instance()->loadLibrary(name) || ComponentLoader::Instance()->loadLibrary(name);
    }

    bool DeploymentComponent::reloadLibrary(const std::string& name)
    {
        return ComponentLoader::Instance()->reloadLibrary(name);
    }

    bool DeploymentComponent::loadService(const std::string& name, const std::string& type) {
        auto deployment_lock = lockDeployment();
        if (deploymentClosing.load())
            return false;
        TaskContext* peer = 0;
        if ((name == getName()) || (name == "this"))
            peer = this;
        else if ( (peer = getPeer(name)) == 0) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::loadService",
                               "No such peer: %s. Can not load service '%s'.",
                               name.c_str(), type.c_str());
            return false;
        }
        // note: in case the service is not exposed as a 'service' object with the same name,
        // we can not detect double loads. So this check is flaky.
        if (peer->provides()->hasService(type))
            return dynamic_cast<DeploymentServiceLifecycle*>(
                peer->provides()->getService(type).get()) == nullptr;
        return PluginLoader::Instance()->loadService(type, peer);
    }

    // or type is a shared library or it is a class type.
    bool DeploymentComponent::loadComponent(const std::string& name, const std::string& type)
    {
        auto deployment_lock = lockDeployment();
        if (deploymentClosing.load())
            return false;
        if ( type == "RTT::PropertyBag" )
            return false; // It should be present as peer.

        if ( this->getPeer(name) || ( compmap.find(name) != compmap.end() && compmap[name].instance != 0) ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponent",
                               "Failed to load component with name %s: already present as peer or loaded.",
                               name.c_str());
            return false;
        }

        TaskContext* instance = ComponentLoader::Instance()->loadComponent(name, type);

        if (!instance) {
            return false;
        }

        // we need to set instance such that componentLoaded can lookup 'instance' in 'comps'
        compmap[name].instance = instance;
        comps.push_back(name);

        std::vector<DeploymentServiceLifecycle*> notified;
        bool accepted = false;
        try {
            accepted = this->componentLoaded(instance);
            if (accepted) {
                for (const auto& service : deploymentServices) {
                    auto* lifecycle = dynamic_cast<DeploymentServiceLifecycle*>(service.get());
                    notified.push_back(lifecycle);
                    if (!lifecycle->componentLoaded(instance)) {
                        accepted = false;
                        break;
                    }
                }
            }
        } catch (...) {
            for (auto* lifecycle : notified)
                lifecycle->componentUnloaded(instance);
            compmap[name].instance = 0;
            comps.remove(name);
            ComponentLoader::Instance()->unloadComponent(instance);
            throw;
        }
        if (!accepted) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::loadComponent",
                               "This deployer type refused to connect to %s: aborting !",
                               instance->getName().c_str());
            compmap[name].instance = 0;
            comps.remove(name);
            for (auto* lifecycle : notified)
                lifecycle->componentUnloaded(instance);
            ComponentLoader::Instance()->unloadComponent( instance );
            return false;
        }

        // unlikely that this fails (checked at entry)!
        this->addPeer( instance, name );
        Logger::log().logf(Logger::Info, "DeploymentComponent::loadComponent",
                           "Adding %s as new peer:  OK.", name.c_str());

        compmap[name].loaded = true;

        return true;
    }

    /**
     * This method removes all references to the component hold in \a cit,
     * on the condition that it is not running.
     */
    bool DeploymentComponent::unloadComponentImpl( CompMap::iterator cit )
    {
        auto deployment_lock = lockDeployment();
        if (deploymentClosing.load() && !deploymentDrained.load())
            return false;
        bool valid = true;
        ComponentData* it = &(cit->second);
        std::string  name = cit->first;

        if ( it->loaded && it->instance ) {
            if ( !it->instance->isRunning() ) {
                if (!componentCanUnload(it->instance)) {
                    return false;
                }
                for (const auto& service : deploymentServices) {
                    if (!dynamic_cast<DeploymentServiceLifecycle&>(*service).componentCanUnload(it->instance))
                        return false;
                }
                for (const auto& service : deploymentServices)
                    dynamic_cast<DeploymentServiceLifecycle&>(*service).componentUnloaded(it->instance);
                if (!it->proxy ) {
                    // allow subclasses to do cleanup too.
                    componentUnloaded( it->instance );
                    Logger::log().logf(Logger::Debug, "DeploymentComponent::unloadComponentImpl",
                                       "Disconnecting %s", name.c_str());
                    it->instance->disconnect();
                    Logger::log().logf(Logger::Debug, "DeploymentComponent::unloadComponentImpl",
                                       "Terminating %s", name.c_str());
                } else
                    Logger::log().logf(Logger::Debug, "DeploymentComponent::unloadComponentImpl",
                                       "Removing proxy for %s", name.c_str());

                // Lookup and erase port+owner from conmap.
                for( ConMap::iterator cmit = conmap.begin(); cmit != conmap.end(); ++cmit) {
                    size_t n = 0;
                    while ( n != cmit->second.owners.size() ) {
                        if (cmit->second.owners[n] == it->instance ) {
                            cmit->second.owners.erase( cmit->second.owners.begin() + n );
                            cmit->second.ports.erase( cmit->second.ports.begin() + n );
                            n = 0;
                        } else
                            ++n;
                    }
                }
                // Lookup in the property configuration and remove:
                RTT::Property<RTT::PropertyBag>* pcomp = root.getPropertyType<PropertyBag>(name);
                if (pcomp) {
                    root.removeProperty(pcomp);
                }

                // Finally, delete the activity before the TC !
                delete it->act;
                it->act = 0;
                ComponentLoader::Instance()->unloadComponent( it->instance );
                it->instance = 0;
                Logger::log().logf(Logger::Info, "DeploymentComponent::unloadComponentImpl",
                                   "Disconnected and destroyed %s", name.c_str());
            } else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::unloadComponentImpl",
                                   "Could not unload Component %s: still running.", name.c_str());
                valid=false;
            }
        }
        if (valid) {
            // NOTE there is no reason to keep the ComponentData in the vector.
            // actually it may cause errors if we try to re-load the Component later.
            compmap.erase(cit);
            CompList::iterator it = comps.begin();
            while(it != comps.end()) {
                if (*it == name)
                    it = comps.erase(it);
                else
                    ++it;
            }
        }
        return valid;
    }

    bool DeploymentComponent::unloadComponent(const std::string& name)
    {
        auto deployment_lock = lockDeployment();
        CompMap::iterator it;
            // no such peer: try looking for the map name
            if ( compmap.count( name ) == 0 || compmap[name].loaded == false ) {
                Logger::log().logf(Logger::Error, "DeploymentComponent::unloadComponent",
                                   "Can't unload component '%s': not loaded by %s",
                                   name.c_str(), this->getName().c_str());
                return false;
                }

        // Ok. Go on with loaded component.
        it = compmap.find(name);

        if ( this->unloadComponentImpl( it ) == false )
            return false;

        Logger::log().logf(Logger::Info, "DeploymentComponent::unloadComponent",
                           "Successfully unloaded component %s.", name.c_str());
        return true;
    }

    void DeploymentComponent::displayComponentTypes() const
    {
        FactoryMap::const_iterator it;
        cout << "I can create the following component types: " <<endl;
        for(it = getFactories().begin(); it != getFactories().end(); ++it) {
            cout << "   " << it->first << endl;
        }
        if ( getFactories().size() == 0 )
            cout << "   (none)"<<endl;
    }

    std::vector<std::string> DeploymentComponent::getComponentTypes() const
    {
        std::vector<std::string> s;
        FactoryMap::const_iterator it;
        for(it = getFactories().begin(); it != getFactories().end(); ++it)
            s.push_back(it->first);

        return s;
    }

    bool DeploymentComponent::setActivity(const std::string& comp_name,
                                          double period, int priority,
                                          int scheduler)
    {
        if ( this->setNamedActivity(comp_name, "Activity", period, priority, scheduler) ) {
            assert( compmap[comp_name].instance );
            assert( compmap[comp_name].act );
            compmap[comp_name].instance->setActivity( compmap[comp_name].act );
            compmap[comp_name].act = 0;
            return true;
        }
        return false;
    }

	bool DeploymentComponent::setFileDescriptorActivity(const std::string& comp_name,
                                          double timeout, int priority,
                                          int scheduler)
    {
        if ( this->setNamedActivity(comp_name, "FileDescriptorActivity", timeout, priority, scheduler) ) {
            assert( compmap[comp_name].instance );
            assert( compmap[comp_name].act );
            compmap[comp_name].instance->setActivity( compmap[comp_name].act );
            compmap[comp_name].act = 0;
            return true;
        }
        return false;
    }

    bool DeploymentComponent::setActivityOnCPU(const std::string& comp_name,
                                          double period, int priority,
					       int scheduler, unsigned int cpu_nr)
    {
        unsigned int mask = 0x1 << cpu_nr;
        if ( this->setNamedActivity(comp_name, "Activity", period, priority, scheduler, mask) ) {
            assert( compmap[comp_name].instance );
            assert( compmap[comp_name].act );
            compmap[comp_name].instance->setActivity( compmap[comp_name].act );
            compmap[comp_name].act = 0;
            return true;
        }
        return false;
    }

    bool DeploymentComponent::setPeriodicActivity(const std::string& comp_name,
                                                  double period, int priority,
                                                  int scheduler)
    {
        if ( this->setNamedActivity(comp_name, "PeriodicActivity", period, priority, scheduler) ) {
            assert( compmap[comp_name].instance );
            assert( compmap[comp_name].act );
            compmap[comp_name].instance->setActivity( compmap[comp_name].act );
            compmap[comp_name].act = 0;
            return true;
        }
        return false;
    }

    bool DeploymentComponent::setPeriodicActivityOnCPU(const std::string& comp_name,
                                                       double period, int priority,
                                                       int scheduler,
                                                       unsigned int cpu_nr)
    {
        unsigned int mask = 0x1 << cpu_nr;
        if ( this->setNamedActivity(comp_name, "PeriodicActivity", period, priority, scheduler, mask) ) {
            assert( compmap[comp_name].instance );
            assert( compmap[comp_name].act );
            compmap[comp_name].instance->setActivity( compmap[comp_name].act );
            compmap[comp_name].act = 0;
            return true;
        }
        return false;
    }

    bool DeploymentComponent::setSlaveActivity(const std::string& comp_name,
                                               double period)
    {
        if ( this->setNamedActivity(comp_name, "SlaveActivity", period, 0, ORO_SCHED_OTHER ) ) {
            assert( compmap[comp_name].instance );
            assert( compmap[comp_name].act );
            compmap[comp_name].instance->setActivity( compmap[comp_name].act );
            compmap[comp_name].act = 0;
            return true;
        }
        return false;
    }

    bool DeploymentComponent::setSequentialActivity(const std::string& comp_name)
    {
        if ( this->setNamedActivity(comp_name, "SequentialActivity", 0, 0, 0 ) ) {
            assert( compmap[comp_name].instance );
            assert( compmap[comp_name].act );
            compmap[comp_name].instance->setActivity( compmap[comp_name].act );
            compmap[comp_name].act = 0;
            return true;
        }
        return false;
    }

    bool DeploymentComponent::setMasterSlaveActivity(const std::string& master,
                                                   const std::string& slave)
    {
        if ( this->setNamedActivity(slave, "SlaveActivity", 0, 0, ORO_SCHED_OTHER, master ) ) {
            assert( compmap[slave].instance );
            assert( compmap[slave].act );
            compmap[slave].instance->setActivity( compmap[slave].act );
            compmap[slave].act = 0;
            return true;
        }
        return false;
    }


    bool DeploymentComponent::setNamedActivity(const std::string& comp_name,
                                               const std::string& act_type,
                                               double period, int priority,
                                               int scheduler, const std::string& master_name)
    {
        return setNamedActivity(comp_name,
                                act_type,
                                period,
                                priority,
                                scheduler,
                                ~0,             // cpu_affinity == all CPUs
                                master_name);
    }

    bool DeploymentComponent::setNamedActivity(const std::string& comp_name,
                                               const std::string& act_type,
                                               double period, int priority,
                                               int scheduler, unsigned cpu_affinity,
                                               const std::string& master_name)
    {
        // This helper function does not actualy set the activity, it just creates it and
        // stores it in compmap[comp_name].act
        RTT::TaskContext* peer = 0;
        base::ActivityInterface* master_act = 0;
        if ( comp_name == "this" || comp_name == this->getName() )
            peer = this;
        else
            if ( compmap.count(comp_name) )
                peer = compmap[comp_name].instance;
            else
                peer = this->getPeer(comp_name); // last resort.
        if (!peer) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::setNamedActivity",
                               "Can't create Activity: component %s not found.",
                               comp_name.c_str());
            return false;
        }
        if ( !master_name.empty() ) {
            if ( master_name == "this" || master_name == this->getName() )
	        master_act = this->engine()->getActivity();
            else
                if ( compmap.count(master_name) && compmap[master_name].act )
            master_act = compmap[master_name].act;
                else
		    master_act = this->getPeer(master_name) ? getPeer(master_name)->engine()->getActivity() : 0; // last resort.

	    if ( !this->getPeer(master_name) ) {
                Logger::log().logf(Logger::Error, "DeploymentComponent::setNamedActivity",
                                   "Can't create SlaveActivity: Master component %s not known as peer.",
                                   master_name.c_str());
                return false;
            }

            if (!master_act) {
                Logger::log().logf(Logger::Error, "DeploymentComponent::setNamedActivity",
                                   "Can't create SlaveActivity: Master component %s has no activity set.",
                                   master_name.c_str());
                return false;
            }
        }
        // this is required for lateron attaching the engine()
        compmap[comp_name].instance = peer;
        if ( peer->isRunning() ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::setNamedActivity",
                               "Can't change activity of component %s since it is still running.",
                               comp_name.c_str());
            return false;
        }

        base::ActivityInterface* newact = 0;
        // standard case:
        if ( act_type == "Activity")
            newact = new RTT::Activity(scheduler, priority, period, cpu_affinity, 0, comp_name);
        else
            // special cases:
            if ( act_type == "PeriodicActivity" && period != 0.0)
                // WARNING RTT::PeriodicActivity does not support a name!
                newact = new RTT::extras::PeriodicActivity(scheduler, priority, period, cpu_affinity, 0);
            else
            if ( act_type == "NonPeriodicActivity" && period == 0.0)
                newact = new RTT::Activity(scheduler, priority, period, cpu_affinity, 0, comp_name);
            else
                if ( act_type == "SlaveActivity" ) {
                    if ( master_act == 0 )
                        newact = new extras::SlaveActivity(period);
                    else {
                        newact = new extras::SlaveActivity(master_act);
                        this->getPeer(master_name)->addPeer( peer );
                    }
                }
                else
                    if (act_type == "Activity") {
                        newact = new Activity(scheduler, priority, period, cpu_affinity, 0, comp_name);
                    }
                    else
                        if (act_type == "SequentialActivity") {
                            newact = new SequentialActivity();
                        }
			else if ( act_type == "FileDescriptorActivity") {
				using namespace RTT::extras;
                newact = new FileDescriptorActivity(scheduler, priority, period, cpu_affinity, 0, comp_name);
				FileDescriptorActivity* fdact = dynamic_cast< RTT::extras::FileDescriptorActivity* > (newact);
				if (fdact) fdact->setTimeout(period);
				else newact = 0;
			}
        if (newact == 0) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::setNamedActivity",
                               "Can't create '%s' for component %s: incorrect arguments.",
                               act_type.c_str(), comp_name.c_str());
            return false;
        }

        // assign default wait period policy to newly created activity
        newact->thread()->setWaitPeriodPolicy(defaultWaitPeriodPolicy);

        // this must never happen if component is running:
        assert( peer->isRunning() == false );
        delete compmap[comp_name].act;
        compmap[comp_name].act = newact;

        return true;
    }

    bool DeploymentComponent::setWaitPeriodPolicy(const std::string& comp_name, int policy)
    {
        if ( !compmap.count(comp_name) ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::setWaitPeriodPolicy",
                               "Can't setWaitPeriodPolicy: component %s not found.",
                               comp_name.c_str());
            return false;
        }

        RTT::base::ActivityInterface *activity = compmap[comp_name].instance->getActivity();
        if ( !activity ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::setWaitPeriodPolicy",
                               "Can't setWaitPeriodPolicy: component %s has no activity (yet).",
                               comp_name.c_str());
            return false;
        }

        activity->thread()->setWaitPeriodPolicy(policy);
        return true;
    }

    bool DeploymentComponent::configure(const std::string& name)
    {
        return configureFromFile( name,  name + ".cpf" );
    }

    bool DeploymentComponent::configureFromFile(const std::string& name, const std::string& filename)
    {
        RTT::TaskContext* c;
        if ( name == "this" || name == this->getName() )
            c = this;
        else
            c = this->getPeer(name);
        if (!c) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::configureFromFile",
                               "No such peer to configure: %s", name.c_str());
            return false;
        }

        marsh::PropertyLoader pl(c);
        return pl.configure( filename, true ); // strict:true
    }

    const FactoryMap& DeploymentComponent::getFactories() const
    {
        return RTT::ComponentLoader::Instance()->getFactories();
    }

    void DeploymentComponent::kickOut(const std::string& config_file)
    {
        RTT::PropertyBag from_file;
        RTT::Property<std::string>  import_file;
        std::vector<std::string> deleted_components_type;

        marsh::PropertyDemarshaller demarshaller(config_file);
        try {
            if ( demarshaller.deserialize( from_file ) ){
                for (RTT::PropertyBag::iterator it= from_file.begin(); it!=from_file.end();it++) {
                    if ( (*it)->getName() == "Import" ) continue;
                    if ( (*it)->getName() == "Include" ) continue;

                    kickOutComponent(  (*it)->getName() );
                }
                deletePropertyBag( from_file );
            }
            else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::kickOut",
                                   "Some error occured while parsing %s",
                                   config_file.c_str());
            }
        } catch (...)
            {
                Logger::log().logf(Logger::Error, "DeploymentComponent::kickOut",
                                   "Uncaught exception in kickOut() !");
            }
    }

    bool DeploymentComponent::cleanupComponent(RTT::TaskContext *instance)
    {
        bool valid = true;
        // 1. Cleanup a single activities, give components chance to cleanup.
        if (instance) {
            if ( instance->getTaskState() <= base::TaskCore::Stopped ) {
		OperationCaller<bool(void)> instancecleanup = instance->getOperation("cleanup");
		instancecleanup();
                Logger::log().logf(Logger::Info, "DeploymentComponent::cleanupComponent",
                                   "Cleaned up %s", instance->getName().c_str());
            } else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::cleanupComponent",
                                   "Could not cleanup Component %s (not Stopped)",
                                   instance->getName().c_str());
                valid = false;
            }
        }
        return valid;
    }

    bool DeploymentComponent::configureComponent(RTT::TaskContext *instance)
    {
        bool valid = false;

        if ( instance ) {
            OperationCaller<bool(void)> instanceconfigure = instance->getOperation("configure");
            if(instanceconfigure()) {
                Logger::log().logf(Logger::Info, "DeploymentComponent::configureComponent",
                                   "Configured %s", instance->getName().c_str());
                valid = true;
            }
            else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::configureComponent",
                                   "Could not configure loaded Component %s",
                                   instance->getName().c_str());
            }
        }
        return valid;
    }

    bool DeploymentComponent::startComponent(RTT::TaskContext *instance)
    {
        bool valid = false;

        if ( instance ) {
            OperationCaller<bool(void)> instancestart = instance->getOperation("start");
            if ( instance->isRunning() ||
                 instancestart() ) {
                Logger::log().logf(Logger::Info, "DeploymentComponent::startComponent",
                                   "Started %s", instance->getName().c_str());
                valid = true;
            }
            else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::startComponent",
                                   "Could not start loaded Component %s",
                                   instance->getName().c_str());
            }
        }
        return valid;
    }

    bool DeploymentComponent::stopComponent(RTT::TaskContext *instance)
    {
        bool valid = true;

        if ( instance ) {
	    OperationCaller<bool(void)> instancestop = instance->getOperation("stop");
            if ( !instance->isRunning() ||
                 instancestop() ) {
                Logger::log().logf(Logger::Info, "DeploymentComponent::stopComponent",
                                   "Stopped %s", instance->getName().c_str());
            }
            else {
                Logger::log().logf(Logger::Error, "DeploymentComponent::stopComponent",
                                   "Could not stop loaded Component %s",
                                   instance->getName().c_str());
                valid = false;
            }
        }
        return valid;
    }

    bool DeploymentComponent::kickOutComponent(const std::string& comp_name)
    {
        RTT::TaskContext* peer = compmap.count(comp_name) ? compmap[ comp_name ].instance : 0;

        if ( !peer ) {
            Logger::log().logf(Logger::Error, "DeploymentComponent::kickOutComponent",
                               "Component not loaded by this Deployer: %s",
                               comp_name.c_str());
            return false;
        }
        stopComponent( peer );
        cleanupComponent (peer );
        unloadComponent( comp_name);

        // also remove from XML if present:
        root.removeProperty( root.find( comp_name ) );

        return true;
    }

    void DeploymentComponent::shutdownDeployment()
    {
        static const char*	PEER="Application";
        static const char*	NAME="shutdownDeployment";

        // names of override properties
        static const char*	WAIT_PROP_NAME="shutdownWait_ms";
        static const char*	TOTAL_WAIT_PROP_NAME="shutdownTotalWait_ms";

        RTT::OperationCaller<void(void)>	ds;
        bool has_program = false;
        bool has_operation = false;
        // if have operation named NAME in peer PEER then use that one.
        RTT::TaskContext* peer = getPeer(PEER);
        if ( 0 != peer){
            has_operation = peer->provides()->hasOperation(NAME);
            if(has_operation)
                ds = peer->provides()->getOperation(NAME);
        } else {
            Logger::log().logf(Logger::Info, "DeploymentComponent::shutdownDeployment",
                               "Ignoring deployment shutdown function due to missing peer.");
            return;
        }
        //If no such operation is found, check if we have a shutdown program?
        if (!ds.ready()){
            has_operation = false;
            Logger::log().logf(Logger::Info, "DeploymentComponent::shutdownDeployment",
                               "Ignoring deployment shutdown function, looking for shutdown program script.");
            has_program = peer->getProvider<Scripting>("scripting")->hasProgram(NAME);
        }
        //Only continue if we have a shutdown operation or program script
        if (has_operation || has_program)
            {
                Logger::log().logf(Logger::Info, "DeploymentComponent::shutdownDeployment",
                                   "Shutting down deployment.");
                RTT::SendHandle<void(void)> handle;
                if(has_operation)
                    handle = ds.send();
                if (handle.ready() || peer->getProvider<Scripting>("scripting")->startProgram(NAME))
                {
                    // set defaults

                    // number milliseconds to wait in between completion checks
                    int wait		= 50;
                    // total number milliseconds to wait for completion
                    int totalWait	= 2000;

                    // any overrides?
                    RTT::Property<int> wait_prop =
                        this->properties()->getProperty(WAIT_PROP_NAME);
                    if (wait_prop.ready())
                    {
                        int w = wait_prop.rvalue();
                        if (0 < w)
                        {
                            wait = w;
                            Logger::log().logf(Logger::Debug, "DeploymentComponent::shutdownDeployment",
                                               "Using override value for %s", WAIT_PROP_NAME);
                        }
                        else
                        {
                            Logger::log().logf(Logger::Warning, "DeploymentComponent::shutdownDeployment",
                                               "Ignoring illegal value for %s", WAIT_PROP_NAME);
                        }
                    }
                    else
                    {
                        Logger::log().logf(Logger::Debug, "DeploymentComponent::shutdownDeployment",
                                           "Using default value for %s", WAIT_PROP_NAME);
                    }

                    RTT::Property<int> totalWait_prop =
                        this->properties()->getProperty(TOTAL_WAIT_PROP_NAME);
                    if (totalWait_prop.ready())
                    {
                        int w = totalWait_prop.rvalue();
                        if (0 < w)
                        {
                            totalWait = w;
                            Logger::log().logf(Logger::Debug, "DeploymentComponent::shutdownDeployment",
                                               "Using override value for %s", TOTAL_WAIT_PROP_NAME);
                        }
                        else
                        {
                            Logger::log().logf(Logger::Warning, "DeploymentComponent::shutdownDeployment",
                                               "Ignoring illegal value for %s", TOTAL_WAIT_PROP_NAME);
                        }
                    }
                    else
                    {
                        Logger::log().logf(Logger::Debug, "DeploymentComponent::shutdownDeployment",
                                           "Using default value for %s", TOTAL_WAIT_PROP_NAME);
                    }

                    // enforce constraints
                    if (wait > totalWait)
                    {
                        wait = totalWait;
                        Logger::log().logf(Logger::Warning, "DeploymentComponent::shutdownDeployment",
                                           "Setting wait == totalWait");
                    }

                    const long int wait_ns = wait * 1000000LL;
                    TIME_SPEC ts;
                    ts.tv_sec  = wait_ns / 1000000000LL;
                    ts.tv_nsec = wait_ns % 1000000000LL;

                    // wait till done or timed out
                    Logger::log().logf(Logger::Debug, "DeploymentComponent::shutdownDeployment",
                                       "Waiting for deployment shutdown to complete ...");
                    int waited = 0;
                    while ( ( (has_operation && RTT::SendNotReady == handle.collectIfDone() ) ||
                              (has_program && peer->getProvider<Scripting>("scripting")->isProgramRunning(NAME)) )
                            && (waited < totalWait) )
                    {
                        (void)rtos_nanosleep(&ts, NULL);
                        waited += wait;
                    }
                    if (waited >= totalWait)
                    {
                        Logger::log().logf(Logger::Error, "DeploymentComponent::shutdownDeployment",
                                           "Timed out waiting for deployment shutdown to complete.");
                    }
                    else
                    {
                        Logger::log().logf(Logger::Debug, "DeploymentComponent::shutdownDeployment",
                                           "Deployment shutdown completed.");
                    }
                }
                else
                {
                    Logger::log().logf(Logger::Error, "DeploymentComponent::shutdownDeployment",
                                       "Failed to start operation or scripting program: %s", NAME);
                }

            }
            else
            {
                Logger::log().logf(Logger::Info, "DeploymentComponent::shutdownDeployment",
                                   "No deployment shutdown function or program available.");
            }
    }

}
