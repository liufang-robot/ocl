
/**
 * @file HelloWorld.cpp
 * This file demonstrate each Orocos primitive with
 * a 'hello world' example.
 */

#include <rtt/os/main.h>

#include <rtt/TaskContext.hpp>
#include <taskbrowser/TaskBrowser.hpp>
#include <rtt/Logger.hpp>
#include <rtt/Property.hpp>
#include <rtt/Attribute.hpp>
#include <rtt/OperationCaller.hpp>
#include <rtt/Port.hpp>
#include <rtt/Activity.hpp>

#include <ocl/OCL.hpp>

using namespace std;
using namespace RTT;
using namespace RTT::detail; // workaround in 2.0 transition phase.
using namespace Orocos;

namespace OCL
{

    /**
     * Every component inherits from the 'RTT::TaskContext' class.  This base
     * class allow a user to add a primitive to the interface and contain
     * an RTT::ExecutionEngine which executes application code.
     */
    class HelloWorld
        : public RTT::TaskContext
    {
    protected:
        /**
         * @name Name-Value parameters
         * @{
         */
        /**
         * Properties take a name, a value and a description
         * and are suitable for XML.
         */
        std::string property;

        /**
         * Attribute that you can toggle to influence what is printed
         * in updateHook()
         */
        bool flag;
        /**
         * Attributes are aliased to class variables.
         */
        std::string attribute;
        /**
         * Constants are aliased, but can only be changed
         * from the component itself.
         */
        std::string constant;
        /** @} */

        /**
         * @name Input-Output ports
         * @{
         */
        /**
         * We publish our data through this RTT::OutputPort
         *
         */
        RTT::OutputPort<std::string> outport;
        /**
         * This RTT::InputPort exposes the latest cyclic input image.
         */
        RTT::InputPort<std::string> bufferport;
        /** @} */

        /**
         * An operation we want to add to our interface.
         */
        std::string mymethod() {
            return "Hello World";
        }

        /**
         * This one is executed in our own thread.
         */
        bool sayWorld( const std::string& word) {
            cout <<"Saying Hello '"<<word<<"' in own thread." <<endl;
            if (word == "World")
                return true;
            return false;
        }

        void updateHook() {
        	if (flag) {
        		cout <<"flag: " << flag <<endl;
        		cout <<"the_property: "<< property <<endl;
        		cout <<"the_attribute: "<< attribute <<endl;
        		cout <<"the_constant: "<< constant <<endl;
        		cout <<"Setting 'flag' back to false."<<endl;
        		flag = false;
        	}

            outport.data() = "Hello World!";

            if (bufferport.status() == NewData) {
                Logger::log().logf(Logger::Debug, "HelloWorld::updateHook",
                                   "Received %s", bufferport.data().c_str());
            }
        }
    public:
        /**
         * This example sets the interface up in the Constructor
         * of the component.
         */
        HelloWorld(std::string name)
            : RTT::TaskContext(name,PreOperational),
              // Name, description, value
              property("Hello Property"), flag(false),
              attribute("Hello Attribute"),
              constant("Hello Constant"),
              // Name, initial value
              outport("the_results"),
              // Name, policy
              bufferport("the_buffer_port")
        {
            // New activity with period 0.1s and priority 0.
            this->setActivity( new Activity(0, 0.1) );

            // Now add member variables to the interface:
            this->properties()->addProperty("the_property", property).doc("A friendly property.");

            this->addAttribute("flag", flag);
            this->addAttribute("the_attribute", attribute);
            this->addConstant("the_constant", constant);

            this->ports()->addPort( outport );
            this->ports()->addPort( bufferport );

            this->addOperation( "the_method", &HelloWorld::mymethod, this, ClientThread ).doc("'the_method' Description");

            this->addOperation( "the_command", &HelloWorld::sayWorld, this, OwnThread).doc("'the_command' Description").arg("the_arg", "Use 'World' as argument to make the command succeed.");

            // Logger::log().logf(Logger::Info, "HelloWorld",
            //                    "**** Starting the 'Hello' component is cancelled ****");
            // Start the component's activity:
            //this->start();
        }
    };
}


// This define allows to compile the hello world component as a library
// liborocos-helloworld.so or as a program (helloworld). Your component
// should only be compiled as a library.
#ifndef OCL_COMPONENT_ONLY

int ORO_main(int, char** argv)
{
    // Set log level more verbose than default,
    // such that we can see output :
    if ( Logger::log().getLogLevel() < RTT::Logger::Info ) {
        Logger::log().setLogLevel( RTT::Logger::Info );
        Logger::log().logf(Logger::Info, "HelloWorld::main",
                           "%s manually raises LogLevel to 'Info' (5). See also file 'orocos.log'.",
                           argv[0]);
    }

    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "**** Creating the 'Hello' component ****");
    // Create the task:
    HelloWorld hello("Hello");

    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "**** Using the 'Hello' component    ****");

    // Do some 'client' calls :
    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "**** Reading a RTT::Property:            ****");
    RTT::Property<std::string> p = hello.properties()->getProperty("the_property");
    assert( p.ready() );
    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "     %s = %s", p.getName().c_str(), p.value().c_str());
#if 0
    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "**** Sending a RTT::OperationCaller:             ****");
    RTT::OperationCaller<bool(std::string)> c = hello.getOperation<bool(std::string)>("the_command");
    assert( c.ready() );
    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "     Sending RTT::OperationCaller : %d", c.send("World").ready() ? 1 : 0);

    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "**** Calling a RTT::OperationCaller:              ****");
    RTT::OperationCaller<std::string(void)> m = hello.getOperation<std::string(void)>("the_method");
    assert( m.ready() );
    const std::string methodResult = m();
    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "     Calling RTT::OperationCaller : %s", methodResult.c_str());
#endif
    Logger::log().logf(Logger::Info, "HelloWorld::main",
                       "**** Starting the TaskBrowser       ****");
    // Switch to user-interactive mode.
    TaskBrowser browser( &hello );

    // Accept user commands from console.
    browser.loop();

    return 0;
}

#else

#include "ocl/Component.hpp"
ORO_CREATE_COMPONENT( OCL::HelloWorld )

#endif
