-- GET /v1/items?category=cat-N&limit=20: an indexed range read of 20 rows.
request = function()
    return wrk.format("GET", "/v1/items?category=cat-" .. math.random(0, 19) .. "&limit=20")
end
