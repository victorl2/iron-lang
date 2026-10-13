-- GET /v1/items/{random id}: the database read path by primary key.
local n = tonumber(os.getenv("BENCH_ITEMS") or "10000")
local suffix = os.getenv("BENCH_QUERY") or ""
request = function()
    return wrk.format("GET", "/v1/items/" .. math.random(1, n) .. suffix)
end
