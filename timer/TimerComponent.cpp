
#include "TimerComponent.hpp"
#include <rtt/Logger.hpp>
#include "ocl/Component.hpp"

ORO_CREATE_COMPONENT_TYPE()
ORO_LIST_COMPONENT_TYPE( OCL::TimerComponent )

namespace OCL
{
    using namespace std;
    using namespace RTT;

    const unsigned TimerComponent::TimerCount;

    TimerComponent::TimerComponent( std::string name /*= "os::Timer" */ )
        : TaskContext( name, PreOperational ), port_timers(TimerCount), mtimeoutEvent("timeout"),
          mtimer( *this, name ),
          waitForCommand( "waitFor", &TimerComponent::waitFor, this), //, &TimerComponent::isTimerExpired, this),
          waitCommand( "wait", &TimerComponent::wait, this) //&TimerComponent::isTimerExpired, this)
    {

        // Add the methods, methods make sure that they are
        // executed in the context of the (non realtime) caller.

        this->addOperation("arm", &os::Timer::arm , &mtimer, RTT::ClientThread).doc("Arm a single shot timer.").arg("timerId", "A numeric id of the timer to arm.").arg("delay", "The delay in seconds before it fires.");
        this->addOperation("startTimer", &os::Timer::startTimer , &mtimer, RTT::ClientThread).doc("Start a periodic timer.").arg("timerId", "A numeric id of the timer to start.").arg("period", "The period in seconds.");
        this->addOperation("killTimer", &os::Timer::killTimer , &mtimer, RTT::ClientThread).doc("Kill (disable) an armed or started timer.").arg("timerId", "A numeric id of the timer to kill.");
        this->addOperation("isArmed", &os::Timer::isArmed , &mtimer, RTT::ClientThread).doc("Check if a given timer is armed or started.").arg("timerId", "A numeric id of the timer to check.");
        this->addOperation("setMaxTimers", &TimerComponent::setMaxTimers , this, RTT::ClientThread).doc("Set the number of enabled timers while stopped, up to 32.").arg("timers", "The largest amount of timers. The highest timerId is max-1.");
        this->addOperation( waitForCommand ).doc("Wait until a timer expires.").arg("timerId", "A numeric id of the timer to wait for.");
        this->addOperation( waitCommand ).doc("Arm and wait until that timer expires.").arg("timerId", "A numeric id of the timer to arm and to wait for.").arg("delay", "The delay in seconds before the timer expires.");
        this->addPort(mtimeoutEvent).doc("Cumulative count of all timer expirations, modulo 2^64, sampled at component cycle boundaries.");
        for(unsigned int i=0;i<port_timers.size();i++){
            ostringstream port_name;
            port_name<<"timer_"<<i;
            port_timers[i] = new RTT::OutputPort<ExpirationCount>(port_name.str());
            this->addPort(*(port_timers[i])).doc(string("Cumulative expiration count, modulo 2^64, for ")+port_name.str());
        }
    }

    TimerComponent::~TimerComponent() {
        this->stop();
        // os::Timer starts its worker during construction, including when this
        // component was never started. Join callbacks before releasing ports.
        if (mtimer.getThread()) mtimer.getThread()->stop();
        for(unsigned int i=0;i<port_timers.size();i++)
            delete port_timers[i];
    }

    bool TimerComponent::setMaxTimers(unsigned int count)
    {
        if (isRunning() || count > TimerCount) return false;
        mtimer.setMaxTimers(count);
        return true;
    }

    bool TimerComponent::startHook()
    {
        return mtimer.getThread() && mtimer.getThread()->start();
    }

    void TimerComponent::updateHook()
    {
        ExpirationCount total = 0;
        for (unsigned i = 0; i != TimerCount; ++i) {
            const ExpirationCount count = mtimer.counts[i].load(std::memory_order_relaxed);
            port_timers[i]->data() = count;
            total += count;
        }
        mtimeoutEvent.data() = total;
    }

    void TimerComponent::stopHook()
    {
        mtimer.getThread()->stop();
    }

    bool TimerComponent::wait(RTT::os::Timer::TimerId id, double seconds)
    {
        return mtimer.arm(id, seconds) && mtimer.waitFor(id);
    }

    bool TimerComponent::waitFor(RTT::os::Timer::TimerId id)
    {
        return mtimer.waitFor(id);
    }

    bool TimerComponent::isTimerExpired(RTT::os::Timer::TimerId id) const
    {
        return !mtimer.isArmed(id);
    }
}
