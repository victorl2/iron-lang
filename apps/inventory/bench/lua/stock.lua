-- POST /v1/items/{random id}/stock: the write path (a transaction that
-- locks the row, updates it and inserts a movement).
local n = tonumber(os.getenv("BENCH_ITEMS") or "10000")
local body = '{"delta":1,"reason":"bench"}'
local headers = { ["Content-Type"] = "application/json" }
request = function()
    return wrk.format("POST", "/v1/items/" .. math.random(1, n) .. "/stock", headers, body)
end
