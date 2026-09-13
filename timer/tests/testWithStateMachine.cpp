#include <string>

#include <ocl/HMIConsoleOutput.hpp>
#include <timer/TimerComponent.hpp>
#include <taskbrowser/TaskBrowser.hpp>

#include <rtt/scripting/Scripting.hpp>
#include <rtt/Activity.hpp>
#include <rtt/scripting/StateMachine.hpp>
#include <iostream>
#include <rtt/os/main.h>

using namespace std;
using namespace Orocos;
using namespace RTT;
using namespace boost;

// test TimerComponent when used by state machine (ie via Orocos interface)
class TestStateMachine
    : public TaskContext
{
    Handle h;
	// log a message
	RTT::Operation<void(std::string)>					log_mtd;
    InputPort<std::uint64_t> extra_expirations;
    InputPort<std::uint64_t> timeout_expirations;

public:
    TestStateMachine(std::string name) :
            TaskContext(name, PreOperational),
            log_mtd("log", &TestStateMachine::doLog, this)
    {
        addOperation( log_mtd ).doc("Log a message").arg("message", "Message to log");
        addPort("extra_expirations", extra_expirations);
        addPort("timeout_expirations", timeout_expirations);
        addOperation("extraCount", &TestStateMachine::extraCount, this, OwnThread);
        addOperation("timeoutCount", &TestStateMachine::timeoutCount, this, OwnThread);
    }

    std::uint64_t extraCount() const { return extra_expirations.data(); }
    std::uint64_t timeoutCount() const { return timeout_expirations.data(); }

    bool startHook()
    {
        bool 				rc = false;		// prove otherwise
        scripting::StateMachinePtr 	p;
        boost::shared_ptr<Scripting> scripting = getProvider<Scripting>("scripting");
        if (!scripting)
            return false;
        std::string         machineName = this->getName();
        if ( scripting->hasStateMachine(machineName))
        {
            if (scripting->activateStateMachine(machineName))
            {
                if (scripting->startStateMachine(machineName))
                {
                    rc = true;
                }
                else
                {
                    Logger::log().logf(Logger::Error, "testWithStateMachine",
                                       "Unable to start state machine: %s",
                                       machineName.c_str());
                }
            }
            else
            {
                Logger::log().logf(Logger::Error, "testWithStateMachine",
                                   "Unable to activate state machine: %s",
                                   machineName.c_str());
            }
        }
        else
        {
            Logger::log().logf(Logger::Error, "testWithStateMachine",
                               "Unable to find state machine: %s",
                               machineName.c_str());
        }
        return rc;
    }

    void doLog(std::string message)
    {
        Logger::log().logf(Logger::Info, "testWithStateMachine",
                           "%s", message.c_str());
    }
};

int ORO_main( int, char** argv)
{
    // Set log level more verbose than default,
    // such that we can see output :
    if ( Logger::log().getLogLevel() < Logger::Info ) {
        Logger::log().setLogLevel( Logger::Info );
        Logger::log().logf(Logger::Info, "testWithStateMachine",
                           "%s manually raises LogLevel to 'Info' (5). See also file 'orocos.log'.",
                           argv[0]);
    }

    HMIConsoleOutput hmi("hmi");
    hmi.setActivity( new Activity(ORO_SCHED_RT, os::HighestPriority, 0.1) );

    TimerComponent tcomp("Timer");
    tcomp.setActivity( new Activity(ORO_SCHED_RT, os::HighestPriority ) );

    TestStateMachine peer("testWithStateMachine");  // match filename
    peer.setActivity( new Activity(ORO_SCHED_RT, os::HighestPriority, 0.1 ) );

    peer.addPeer(&tcomp);
    peer.addPeer(&hmi);

    peer.ports()->getPort("extra_expirations")->connectTo(tcomp.ports()->getPort("timer_5"));
    peer.ports()->getPort("timeout_expirations")->connectTo(tcomp.ports()->getPort("timer_4"));

    std::string name = "testWithStateMachine.osd";
    assert (peer.getProvider<Scripting>("scripting"));
	if ( !peer.getProvider<Scripting>("scripting")->loadStateMachines(name) )
    {
        Logger::log().logf(Logger::Error, "testWithStateMachine",
                           "Unable to load state machine: '%s'", name.c_str());
        tcomp.getActivity()->stop();
        return -1;
    }

    TaskBrowser tb( &peer );

    peer.configure();
    peer.start();
    tcomp.configure();
    tcomp.start();
    hmi.start();

    tb.loop();

    tcomp.stop();
    peer.stop();

    return 0;
}
