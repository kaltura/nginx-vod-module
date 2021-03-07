
--[[
    - exposes metrics on server performance in prom format
--]]

local log = ngx.log
local ERR = ngx.ERR
local ngx_var = ngx.var
local ngx_now = ngx.now
local start_time = ngx.req.start_time

local pairs = pairs
local concat = table.concat

local HTTP_STATUS_KEY_PREFIX = 'http_status_'
local HTTP_TIME_KEY_PREFIX = 'http_time_'

local _M = { _VERSION = '0.1' }

local mt = { __index = _M }

function _M.get_status(group_name)
    local self = _M.self
    local dict = self._dict

    local res = {}
    local count = 0

    for group, _ in pairs(self._groups) do
        local key = HTTP_TIME_KEY_PREFIX .. group
        local cur = dict:get(key) or 0
        count = count + 1
        res[count] = 'upstream_http_time{' .. group_name .. '="' .. group .. '"} ' .. cur .. '\n'

        for status, _ in pairs(self._statuses) do
            local key = HTTP_STATUS_KEY_PREFIX .. status .. '_' .. group
            local cur = dict:get(key) or 0

            count = count + 1
            res[count] = 'upstream_http_status{status="' .. status .. '",' .. group_name .. '="' .. group .. '"} ' .. cur .. '\n'
        end
    end

    return concat(res)
end

function _M.log(group)
    local self = _M.self
    local dict = self._dict
    local status = ngx_var.status

    if not group then
        group = ''
    end

    local key = HTTP_STATUS_KEY_PREFIX .. status .. '_' .. group
    local res, err = dict:incr(key, 1, 0)
    if not res then
        log(ERR, 'dict:incr failed: ', err, ', key: ', key)
    end

    local request_time = ngx_now() - start_time()
    local key = HTTP_TIME_KEY_PREFIX .. group
    local res, err = dict:incr(key, request_time, 0)
    if not res then
        log(ERR, 'dict:incr failed: ', err, ', key: ', key)
    end

    self._statuses[status] = 1
    self._groups[group] = 1
end

function _M.new(_, options)
    if _M.self then
        return _M.self
    end

    local dict = options.dict
    if not dict then
        log(ERR, 'missing required params for metrics')
        return
    end

    local self = setmetatable({
        _dict         = dict,
        _statuses     = {},
        _groups       = {},
    }, mt)

    _M.self = self        -- singleton
    return self
end

return _M
