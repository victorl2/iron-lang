-- POST /v1/items: the insert path (a transaction with two inserts, then a
-- read back). SKUs are unique per run, thread and request.
local threads = 0
local counter = 0
local run = ""
local headers = { ["Content-Type"] = "application/json" }
setup = function(thread)
    threads = threads + 1
    thread:set("tid", threads)
end
init = function(args)
    run = tostring(os.time()) .. "-" .. tostring(tid)
end
request = function()
    counter = counter + 1
    local body = '{"sku":"B-' .. run .. '-' .. counter .. '","name":"Bench item","category":"bench","price_cents":1234,"quantity":5}'
    return wrk.format("POST", "/v1/items", headers, body)
end
