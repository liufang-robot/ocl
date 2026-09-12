#include "lua/LuaComponent.hpp"
#include <rtt/InputPort.hpp>
#include <rtt/OutputPort.hpp>
#include <rtt/extras/SlaveActivity.hpp>
#include <rtt/os/main.h>
#include <iostream>
#include <stdexcept>
namespace {
// Models a transport proxy which has neither a local process image nor a snapshot codec.
class UnsupportedPort : public RTT::OutputPort<int> {
    RTT::base::DataSourceBase::shared_ptr imageSource() override { return {}; }
public:
    UnsupportedPort() : RTT::OutputPort<int>("unsupported") {}
    RTT::base::DataSourceBase::shared_ptr getDataSource() const override { return {}; }
};
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}
int ORO_main(int, char**) {
    try {
        OCL::LuaComponent component("lua");
        component.setActivity(new RTT::extras::SlaveActivity(0.01));
        UnsupportedPort unsupported;
        component.addPort(unsupported);
        require(component.exec_str(R"(
            local port = rtt.getTC():getPort('unsupported')
            assert(not pcall(function() port:data() end))
            assert(not pcall(function() port:snapshot() end))
        )"), "unsupported proxy image and snapshot return Lua errors");
        component.ports()->removePort("unsupported");
        RTT::OutputPort<int> output("output");
        component.addPort(output);
        require(component.exec_str(R"(
            local rtt = require('rtt')
            output = rtt.getTC():getPort('output')
            function configureHook() return true end
            function startHook() return true end
            function updateHook() output:data(37) end
        )"), "install Lua cyclic hook");
        require(component.configure() && component.start(), "start Lua component");
        require(component.getActivity()->execute(), "execute Lua hook");
        require(output.snapshot() == 37, "successful Lua hook publishes automatically");
        require(component.exec_str("function updateHook() output:data(99); error('intentional failure') end"), "install failing hook");
        component.getActivity()->execute();
        require(output.snapshot() == 37, "failed Lua hook must not publish its partial output image");
        require(component.inRunTimeError(), "failed Lua hook enters runtime error");
        component.stop();
        RTT::TaskContext peer("peer");
        peer.setActivity(new RTT::extras::SlaveActivity(0.01));
        RTT::OutputPort<int> foreign("foreign");
        peer.addPort(foreign); component.addPeer(&peer);
        require(peer.start(), "start peer on the same execution thread");
        require(component.exec_str(R"(
            function updateHook()
                local peer = rtt.getTC():getPeer('peer')
                local ok = pcall(function() peer:getPort('foreign'):data(123) end)
                assert(not ok, 'another owner on the same thread must reject image access')
                output:data(41)
            end
        )"), "install owner isolation hook");
        require(component.start(), "restart Lua component");
        component.getActivity()->execute();
        require(output.snapshot() == 41 && foreign.data() == 0, "Lua image ownership must match component context");
        component.stop(); peer.stop();
        peer.ports()->removePort("foreign");
        component.ports()->removePort("output");
        std::cout << "Lua cyclic component test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
