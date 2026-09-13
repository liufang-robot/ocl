#include <timer/TimerComponent.hpp>
#include <taskbrowser/TaskBrowser.hpp>

#include <rtt/Activity.hpp>
#include <rtt/InputPort.hpp>
#include <iostream>
#include <rtt/os/main.h>

using namespace std;
using namespace Orocos;
using namespace RTT;

class TestTaskContext
    : public RTT::TaskContext
{
    InputPort<std::uint64_t> receiver;
public:
    TestTaskContext(std::string name)
        : RTT::TaskContext(name, PreOperational),
          receiver("TimerIn")
    {
        ports()->addEventPort( receiver );
    }

    bool configureHook()
    {
        if ( receiver.connected() )
            Logger::log().logf(Logger::Info, "timer tests",
                               "%s starts listening for timeout events.",
                               this->getName().c_str());
        return receiver.connected();
    }

    void updateHook()
    {
        if (receiver.status() == NewData)
            Logger::log().logf(Logger::Info, "timer tests",
                               "%s observes cumulative expiration count %llu",
                               this->getName().c_str(), static_cast<unsigned long long>(receiver.data()));
    }
};

int ORO_main( int, char** argv)
{
    // Set log level more verbose than default,
    // such that we can see output :
    if ( RTT::Logger::log().getLogLevel() < RTT::Logger::Info ) {
        RTT::Logger::log().setLogLevel( RTT::Logger::Info );
        Logger::log().logf(Logger::Info, "timer tests",
                           "%s manually raises LogLevel to 'Info' (5). See also file 'orocos.log'.",
                           argv[0]);
    }


    TimerComponent tcomp("Timer");
    tcomp.setActivity( new RTT::Activity(ORO_SCHED_RT, os::HighestPriority, 0.0) );

    TestTaskContext gtc("Peer");
    gtc.setActivity( new RTT::Activity(ORO_SCHED_RT, os::HighestPriority, 0.1) );

    gtc.ports()->getPort("TimerIn")->connectTo( tcomp.ports()->getPort("timeout"));

    TaskBrowser tb( &gtc );

    gtc.configure();
    gtc.start();
    gtc.addPeer( &tcomp );
    tcomp.configure();
    tcomp.start();

    cout <<endl<< "  This demo allows testing the TimerComponent." << endl;
    cout << "  Use 'Timer.arm(0, 1.5)' to arm timer '0' to end over 1.5 seconds. " <<endl;
    cout << "  32 timers are initially available (0..31)." <<endl;
    cout << "  Other methods (type 'this') are available as well."<<endl;

    tb.loop();

    tcomp.stop();
    gtc.stop();

    return 0;
}
