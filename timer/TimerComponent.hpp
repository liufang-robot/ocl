#ifndef ORO_TIMER_COMPONENT_HPP
#define ORO_TIMER_COMPONENT_HPP


#include <rtt/os/TimeService.hpp>
#include <rtt/TaskContext.hpp>
#include <rtt/os/Timer.hpp>
#include <rtt/OutputPort.hpp>

#include <rtt/RTT.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <ocl/OCL.hpp>

namespace OCL
{
    /**
     * @brief A Component interface to the Real-Time types::Toolkit's timer.
     * It must be configured with a Activity which will emit
     * the timeout event of this component.
     *
     */
    class TimerComponent
        : public RTT::TaskContext
    {
    protected:
        /**
         * Helper class for catching the virtual timeout function of Timer.
         */
        // Expiration counts are state, modulo 2^64. Consumers compute counter
        // differences to recover multiplicity even when component cycles coalesce.
        static const unsigned TimerCount = 32;
        typedef std::uint64_t ExpirationCount;
        struct TimeoutCatcher : public os::Timer {
            TimerComponent& owner;
            std::array<std::atomic<ExpirationCount>, TimerCount> counts;
            TimeoutCatcher(TimerComponent& component, const std::string& name)
                : os::Timer(TimerCount, ORO_SCHED_RT, os::HighestPriority, name + ".Timer"), owner(component)
            {
                for (unsigned i = 0; i != TimerCount; ++i) counts[i].store(0);
            }
            ~TimeoutCatcher() override {
                if (getThread()) getThread()->stop();
            }
            void timeout(os::Timer::TimerId id) override {
                if (id < 0 || static_cast<unsigned>(id) >= TimerCount) return;
                counts[id].fetch_add(1, std::memory_order_relaxed);
                owner.trigger();
            }
        };

        std::vector<OutputPort<ExpirationCount>* > port_timers;
        OutputPort<ExpirationCount> mtimeoutEvent;
        TimeoutCatcher mtimer;
        bool setMaxTimers(unsigned int count);

        /**
         * This hook will check if a Activity has been properly
         * setup.
         */
        bool startHook();
        void updateHook();
        void stopHook();

        /**
         * Command: wait until a timer expires.
         */
        RTT::Operation<bool(RTT::os::Timer::TimerId)> waitForCommand;

        /**
         * Command: arm and wait until a timer expires.
         */
        RTT::Operation<bool(RTT::os::Timer::TimerId, double)> waitCommand;

        /**
         * Command Implementation: wait until a timer expires.
         */
        bool waitFor(RTT::os::Timer::TimerId id);

        /**
         * Command Implementation: \b arm and wait until a timer expires.
         */
        bool wait(RTT::os::Timer::TimerId id, double seconds);

        /**
         * Command Condition: return true if \a id expired.
         */
        bool isTimerExpired(RTT::os::Timer::TimerId id) const;
    public:
        /**
         * Set up a component for timing events.
         */
        TimerComponent( std::string name = "os::Timer" );

        virtual ~TimerComponent();
    };

}

#endif
