# Automatic cyclic data ports

OCL 3 uses the RTT 3 cyclic port model. A component reads `input.data()` and
modifies `output.data()` in its `updateHook()`. RTT refreshes inputs before the
hook and publishes outputs only after a successful hook. Input images retain
their previous/default values when no new sample arrives. `input.status()`
observes freshness without transferring data.

Declare deployment mappings with service-qualified port names:

```text
connectPort("source.motion.state", "sink.state")
connectMember("source.motion.state", "axes[2].position", "sink.command", "position")
connectMember("scalar.value", "", "sink.command", "velocity")
finalizeConnections()
```

An empty member path selects the whole value. Selected types and fixed-array
shapes must match exactly. Destination writers must not overlap. Declarations
and finalization must complete while all involved components are stopped.

Lua provides `input:data()`, `input:status()`, `output:data(value)`, and
`output:data()`. Image getters return detached copies; input copies cannot modify
the actual input image. A running component's images are accessible only in its
own execution context. `output:snapshot()` safely observes a committed output
from another context. Image assignment never publishes immediately. The manual
`read` and `write` methods have been removed.

TaskBrowser displays synchronized snapshots. Reporting also observes committed
snapshots independently of component input subscriptions. Configure a periodic
reporter activity or request its `snapshot()` operation explicitly. Each report
samples each output once; samples between reports may coalesce. `ReportOnlyNewData`
uses independent observer freshness. The former queue-oriented `ReportPolicy`
property has been removed.

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
