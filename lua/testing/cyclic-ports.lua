local rtt = require('rtt')
assert(rtt.InputPort.read == nil, 'manual input transfer must not be exposed')
assert(rtt.OutputPort.write == nil, 'manual output transfer must not be exposed')
local source = rtt.OutputPort.new('Int32', 'source')
local sink = rtt.InputPort.new('Int32', 'sink')
for _, removed_type in ipairs({1, 2}) do
   local policy = rtt.Variable.new('ConnPolicy')
   policy.type = removed_type
   policy.size = 12
   local ok, result = pcall(function() return source:connect(sink, policy) end)
   assert(not ok or not result, 'removed numeric policy must not become a DATA connection')
   assert(not sink:info().connected and not source:info().connected)
end
local ok = pcall(function() return source:connect(sink, {type=1, size=12}) end)
assert(not ok, 'an unsupported policy argument must not be ignored')
local policy = rtt.Variable.new('ConnPolicy')
policy.type = 0
assert(source:connect(sink, policy))
assert(source:disconnect(sink))
assert(source:connect(sink))
source:data(37)
assert(source:data() == 37)
assert(sink:data() == 0, 'editing an output image must not publish')
assert(sink:status() == 'NoData')
assert(source:snapshot() == 0, 'uncommitted image must not be visible to observers')
sink:delete()
source:delete()
print('cyclic port image test passed')
