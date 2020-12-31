
--[[
    outputs nginx stub status metrics in prom format
--]]

local log = ngx.log
local ERR = ngx.ERR
local capture = ngx.location.capture
local gmatch = ngx.re.gmatch
local say = ngx.say

local _M = { _VERSION = '0.1' }

-- order matching nginx stub status output
local metrics = {
    'nginx_connections_current{state="active"} ',
    'nginx_connections_processed_total{stage="accepted"} ',
    'nginx_connections_processed_total{stage="handled"} ',
    'nginx_connections_processed_total{stage="any"} ',
    'nginx_connections_current{state="reading"} ',
    'nginx_connections_current{state="writing"} ',
    'nginx_connections_current{state="waiting"} ',
}

function _M.format_status(uri)

    local res = capture(uri)
    if res.status ~= 200 then
        log(ERR, 'bad status: ', res.status)
        return
    end

    local iter, err = gmatch(res.body, '(\\d+)')
    if not iter then
        log(ERR, 'gmatch failed: ', err)
        return
    end

    local body = ''
    for _, cur in pairs(metrics) do
        local m, err = iter()
        if err then
            log(ERR, 'gmatch iterator failed: ', err)
            return
        end

        if not m then
            log(ERR, 'missing matches')
            return
        end

        body = body .. cur .. m[1] .. '\n'
    end

    return body
end

return _M
