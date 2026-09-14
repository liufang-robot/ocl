/**
 * @file testcomp.cpp
 * Simple testcomponent used for Lua unit testing
 */

#include <rtt/TaskContext.hpp>
#include <rtt/Logger.hpp>
#include <rtt/Property.hpp>
#include <rtt/Attribute.hpp>
#include <rtt/Operation.hpp>
#include <rtt/Port.hpp>
#include <rtt/Activity.hpp>

#include <ocl/OCL.hpp>

using namespace std;
using namespace RTT;
using namespace RTT::detail; // workaround in 2.0 transition phase.
using namespace Orocos;

namespace OCL
{
	class Testcomp : public RTT::TaskContext
	{
	protected:
		RTT::Property<std::string> property;
		std::string attribute;
		std::string constant;

		RTT::OutputPort<std::string> outport;
		RTT::InputPort<std::string> inport;

		void null_0() {
			Logger::log().logf(Logger::Warning, "Testcomp::null_0",
			                   "in: void null_0");
		}

		std::string op_0() {
			Logger::log().logf(Logger::Warning, "Testcomp::op_0",
			                   "in: std::string op_0");
			return "inside operation_0";
		}

		bool op_1(std::string s) {
			Logger::log().logf(Logger::Warning, "Testcomp::op_1",
			                   "in: bool op_1(std::string s) %s", s.c_str());
			return true;
		}

		double op_2(std::string s, double d) {
			Logger::log().logf(Logger::Warning, "Testcomp::op_2",
			                   "in: double op_2(std::string s, double d) %s%f",
			                   s.c_str(), d);
			return d*2;
		}

		void op_1_out(int &i) {
			Logger::log().logf(Logger::Warning, "Testcomp::op_1_out",
			                   "in: void op_1_out(int &i) %d", i);
			i = i+1;
			return;
		}

		void op_3_out(std::string &s, double &d, int &i) {
			Logger::log().logf(Logger::Warning, "Testcomp::op_3_out",
			                   "in: void op_3_out(std::string &s, double &d, int &i) %s%f%d",
			                   s.c_str(), d, i);
			s = s + "-this-string-has-a-tail";
			d = d * 2;
			i = 4711;
			return;
		}

		bool op_1_out_retval(int &i) {
			Logger::log().logf(Logger::Warning, "Testcomp::op_1_out_retval",
			                   "in: bool op_1_out_retval(int &i) %d", i);
			i = i + 1;
			return i%2;
		}

		void throw_exception()
		{
			throw std::runtime_error("Alas, its time to go.");
		}

		bool op1_uint8(unsigned char x)
		{
			if(x == 'x')
				return true;
			else
				return false;
		}


		void updateHook() {
			// Logger::log().logf(Logger::Info, "Testcomp::updateHook",
			//                    "inside update_hook");
		}
	public:
		OperationCaller<bool(std::string)> print;

		/**
		 * This example sets the interface up in the Constructor
		 * of the component.
		 */
		Testcomp(std::string name) : RTT::TaskContext(name),
			  // Name, description, value
			  property("the_property", "the_property Description", "Hello World"),
			  attribute("Hello World"),
			  constant("Hello World"),
			  // Name, initial value
			  outport("the_results"),
			  // Name, policy
			  inport("the_input", ConnPolicy::data())
		{

#if 0
			// New activity with period 0.01s and priority 0.
			this->setActivity( new Activity(0, 0.01) );
#endif
			// Check if all initialisation was ok:
			assert( property.ready() );

			// Now add it to the interface:
			this->properties()->addProperty( property);

			this->addAttribute("the_attribute", attribute);
			this->addConstant("the_constant", constant);

			this->ports()->addPort( outport ).doc("dummy test port");
			this->ports()->addPort( inport );

			this->addOperation( "null_0", &Testcomp::null_0, this, OwnThread ).doc("'null_0' Description");
			this->addOperation( "op_0_ct", &Testcomp::op_0, this, ClientThread ).doc("'op_0_ct', ClientThread variant");
			this->addOperation( "op_0_ot", &Testcomp::op_0, this, ClientThread ).doc("'op_0_ot', OwnThread variant");
			this->addOperation( "op_1", &Testcomp::op_1, this, OwnThread).doc("'op_1' Description").arg("mes", "just any string.");
			this->addOperation( "op_2", &Testcomp::op_2, this, OwnThread).doc("'op_2' Description").arg("mes", "just any string.").arg("double", "just any double");
			this->addOperation( "op_1_out", &Testcomp::op_1_out, this, OwnThread).doc("'op_1_out' Description").arg("i", "any int");
			this->addOperation( "op_3_out", &Testcomp::op_3_out, this, OwnThread).doc("'op_3_out' Description").arg("mes", "just any string.").arg("double", "just any double").arg("i", "just any int");
			this->addOperation( "op_1_out_retval", &Testcomp::op_1_out_retval, this, OwnThread).doc("'op_1_out' Description").arg("i", "any int");

			this->addOperation( "op1_uint8", &Testcomp::op1_uint8, this, OwnThread).doc("'op1_uint8' Description").arg("x", "any char, try 'x'");

			this->addOperation( "throw", &Testcomp::throw_exception, this, ClientThread).doc("This operation throws an exception");
			this->provides("printing")
				->addOperation( "print", &Testcomp::op_1,
						this, OwnThread).doc("'op_1' Description").arg("mes", "just any string.");

			this->requests("print_str")->addOperationCaller(print);

#if 0
			Logger::log().logf(Logger::Info, "Testcomp",
			                   "**** Starting the 'Testcomp' component ****");
			this->start();
#endif
		}
	};
}

#include "ocl/Component.hpp"
ORO_CREATE_COMPONENT( OCL::Testcomp )
