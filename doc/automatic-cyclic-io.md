# Automatic cyclic data ports

OCL 3 uses the RTT 3 cyclic port model. A component reads `input.data()` and
modifies `output.data()` in its `updateHook()`. RTT refreshes inputs before the
hook and publishes outputs only after a successful hook. Input images retain
their previous/default values when no new sample arrives. `input.status()`
observes freshness without transferring data.

Declare deployment mappings with service-qualified port names:

```text
connectPort("source.motion.state", "sink.state")
connectPort("source.motion.state.axes[2].position", "sink.command.position")
connectPort("scalar.value", "sink.command.velocity")
finalizeConnections()
```

Use the same dot/index path when inspecting a value and connecting it. The path
resolves the component, real nested services, port, then reflected members and
fixed-array elements. Omit members to select the whole value. Legacy `::`
selectors, malformed paths and out-of-range indices are rejected. Selected types
and fixed-array shapes must match exactly. Destination writers must not
overlap. Declarations and finalization must complete while all involved
components are stopped. The separate `connectMember` deployment operation and
C++ method have been removed.

Register every data port with `addPort`. Data publication does not schedule a
consumer: configure its periodic activity or execute it through an explicit
external scheduler. Each scheduled cycle refreshes inputs, runs `updateHook`,
and publishes outputs. `addEventPort` has been removed, including the Lua binding;
Lua `rttlib.create_if` accepts only `in` and `out` port specifications and rejects
the removed `in+event` form.

Data ports deliver the latest published value. FIFO and circular-buffer modes
are removed; XML and script policies using numeric types 1 or 2 fail instead of
changing delivery semantics. Use `DATA` (0) for port connections. Transport
capacity and internal operation/network queues do not create a data-port history.

Lua provides `input:data()`, `input:status()`, `output:data(value)`, and
`output:data()`. Image getters return detached copies; input copies cannot modify
the actual input image. A running component's images are accessible only in its
own execution context, including native lifecycle hooks such as `startHook`.
`output:snapshot()` safely observes a committed output
from another context. Image assignment never publishes immediately. The manual
`read` and `write` methods have been removed.

Lua's `source:connect(sink)` uses the default policy. To supply one explicitly,
create `policy = rtt.Variable.new("ConnPolicy")`, set `policy.type = 0`, and call
`source:connect(sink, policy)`. Unsupported policy argument types and removed
delivery modes raise an error.

TaskBrowser evaluates ports directly:

```text
source.motion.state
source.motion.state.axes[2].position
sink.command.velocity
isPortConnected("sink.command")
disconnectPort("sink.command")
```

Inputs show the last component-acquired image; outputs show the last committed
value. Observation is read-only, does not consume samples or schedule a cycle,
and retains the last value while stopped. Inputs initially show their default
image. Outputs show `(unavailable)` until the first commit; an unavailable value
cannot supply an operation argument. Ports do not create services or methods;
fields named `data`, `snapshot`, `connected`, `name` or `status` are ordinary data.
Use `ls component` or `ls component.service` for port types, directions, values and
input source mappings. Real service operations remain callable.
`isPortConnected` and `disconnectPort` accept whole-port paths only, rejecting
member selectors. Disconnect removes all whole/member connections of that port
and requires the affected component graph to be stopped.

HTTP and OPC UA publication observes both port directions without adding
connections. Configure external input writers explicitly while the affected graph
is stopped, after publishing the component:

```text
http.enableInputWrite("sink.command.velocity")
opcua.enableInputWrite("sink.command.position")
```

Each enabled endpoint reserves one whole/member writer region. Disjoint writers
may coexist; overlapping regions, output endpoints and incompatible selected types
are rejected. Network acknowledgements mean the value was staged; the component
acquires it at its next input boundary. Reads continue to show the acquired input
image. Release a source with the matching service's `disableInputWrite(endpoint)`
while stopped. Whole structured writes require a complete validated value rather
than merging partial JSON or member updates into component storage.

Reporting also observes committed
snapshots independently of component input subscriptions. Configure a periodic
reporter activity or request its `snapshot()` operation explicitly. Each report
samples each output once; samples between reports may coalesce. `ReportOnlyNewData`
uses independent observer freshness. The former queue-oriented `ReportPolicy`
property has been removed. Structured fields are decomposed from reporter-owned
copies, so reporting cannot modify the source snapshot. Register or remove report
sources and marshallers while the reporter is stopped.

TimerComponent's `timer_0` through `timer_31` ports now expose `UInt64` cumulative
expiration counts. Its `timeout` port holds their sum. Timer callbacks increment
atomic counters and request a component cycle; the component copies the counts
into its output images during `updateHook()`. Multiple expirations between cycles
therefore remain observable through counter differences. Counters persist across
stop/start and wrap modulo 2^64. Consumers must account for wrap and cannot recover
the order or timestamps of individual expirations. `setMaxTimers` accepts at most
32 timers and rejects changes while running.

These boundaries belong to each component. They do not synchronize independently
scheduled components into a common input/output barrier.
