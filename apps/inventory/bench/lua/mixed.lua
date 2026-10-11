-- A mixed workload: reads with and without conversion, lists, the report,
-- rates, stock changes and inserts, in roughly the proportions a service
-- like this sees.
local n = tonumber(os.getenv("BENCH_ITEMS") or "10000")
local threads = 0
local counter = 0
local run = ""
local json = { ["Content-Type"] = "application/json" }
setup = function(thread)
    threads = threads + 1
    thread:set("tid", threads)
end
init = function(args)
    run = tostring(os.time()) .. "-" .. tostring(tid)
end
request = function()
    local r = math.random(1, 100)
    if r <= 40 then
        return wrk.format("GET", "/v1/items/" .. math.random(1, n))
    elseif r <= 55 then
        return wrk.format("GET", "/v1/items/" .. math.random(1, n) .. "?currency=EUR")
    elseif r <= 70 then
        return wrk.format("GET", "/v1/items?category=cat-" .. math.random(0, 19) .. "&limit=20&currency=GBP")
    elseif r <= 72 then
        return wrk.format("GET", "/v1/reports/categories")
    elseif r <= 80 then
        return wrk.format("GET", "/v1/rates/JPY")
    elseif r <= 95 then
        return wrk.format("POST", "/v1/items/" .. math.random(1, n) .. "/stock", json, '{"delta":-1,"reason":"sale"}')
    else
        counter = counter + 1
        return wrk.format("POST", "/v1/items", json,
            '{"sku":"M-' .. run .. '-' .. counter .. '","name":"Mixed item","category":"mixed","price_cents":999,"quantity":3}')
    end
end
