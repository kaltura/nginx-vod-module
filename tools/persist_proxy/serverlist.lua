
--[[
    a list of host + port pairs, pulled from remote resource
--]]

local sync = require('sync')
local cjson_safe = require('cjson.safe')

local log = ngx.log
local ERR = ngx.ERR
local INFO = ngx.INFO
local crc32 = ngx.crc32_short
local gmatch = ngx.re.gmatch

local type = type
local random = math.random
local json_decode = cjson_safe.decode
local json_encode = cjson_safe.encode

local _M = { _VERSION = '0.1' }

local mt = { __index = _M }


local function _server_key(host, port)
    return 'srv_' .. host .. '_' .. port
end

local function _server_new(host, port)
    return {
        key = _server_key(host, port),
        host = host,
        port = port,
    }
end


local function _parse_k8s_pod_list(json)
    if not json then
        return
    end

    local regex = [["podIP"\s*:\s*"([^"]+)"]]
    local it, err = gmatch(json, regex)
    if not it then
        log(ERR, 'gmatch failed: ', err)
        return
    end

    local list = {}
    local idx = 0

    while true do
        local m, err = it()
        if err then
            log(ERR, 'iterator failed: ', err)
            return
        end

        if not m then
            break
        end

        idx = idx + 1
        list[idx] = {
            host = m[1],
            port = 80
        }
    end

    local res, err = json_encode(list)
    if not res then
        log(ERR, 'json_encode failed: ', err)
        return
    end

    return res
end

local function _is_server_valid(srv)
    if not srv.host then
        log(ERR, 'server missing host')
        return false
    end

    if not srv.port then
        log(ERR, 'server missing port')
        return false
    end

    if type(srv.host) ~= 'string' then
        log(ERR, 'invalid host type: ', type(srv.host))
        return false
    end

    if type(srv.port) ~= 'string' and type(srv.port) ~= 'number' then
        log(ERR, 'invalid port type: ', type(srv.port))
        return false
    end

    return true
end


local function _update(self, json)
    local list, err = json_decode(json)
    if not list then
        log(ERR, 'json_decode failed: ', err)
        self._errors = self._errors + 1
        return
    end

    if type(list) ~= 'table' then
        log(ERR, 'invalid list type: ', type(list))
        self._errors = self._errors + 1
        return
    end

    local count = 0
    local bykey = {}
    local byidx = {}

    for _, cur in pairs(list) do
        if _is_server_valid(cur) then
            local key = _server_key(cur.host, cur.port)
            local srv

            if self._bykey[key] then
                srv = self._bykey[key]
            else
                srv = _server_new(cur.host, cur.port)
            end

            count = count + 1
            bykey[key] = srv
            byidx[count] = srv
        end
    end

    if count == 0 then
        log(ERR, 'no servers were parsed')
        self._errors = self._errors + 1
        return
    end

    log(INFO, 'list updated, new_count: ', count, ', old_count: ', self._count)

    self._count = count
    self._bykey = bykey
    self._byidx = byidx
end

function _M.empty(self)
    if self._count > 0 then
        return false
    end

    self._sync:pull_shared()

    return self._count <= 0
end

function _M.get(self)
    return self._bykey
end

function _M.get_by_key(self, key)
    return self._bykey[key]
end

function _M.get_errors(self)
    return self._errors
end

function _M.get_random_server_pair(self)
    local count = self._count
    if count <= 1 then
        return self._byidx[1]
    end

    local idx1 = random(count)
    local idx2 = random(count - 1)
    if idx2 >= idx1 then
        idx2 = idx2 + 1
    end

    return self._byidx[idx1], self._byidx[idx2]
end

function _M.add_servers(self, left, list, persist_key)
    local count = self._count
    if count <= 0 then
        return
    end

    local ignore = {}
    local out_idx = 0

    for _, srv in pairs(list) do
        out_idx = out_idx + 1
        ignore[srv.key] = true
    end

    local start
    if persist_key then
        start = crc32(persist_key)
    else
        start = random(count)
    end

    for i = 1, count do
        local idx = (start + i) % count + 1
        local srv = self._byidx[idx]

        if not ignore[srv.key] then
            out_idx = out_idx + 1
            list[out_idx] = srv

            left = left - 1
            if left <= 0 then
                break
            end
        end
    end

    return list
end

function _M.new(_, pull, dict, options)
    local self = setmetatable({
        _count = 0,
        _bykey = {},
        _byidx = {},
        _errors = 0,
    }, mt)

    local update = function (json) _update(self, json) end

    local do_pull
    if options.format == 'k8s_pod_list' then
        do_pull = function () return _parse_k8s_pod_list(pull()) end
    else
        do_pull = pull
    end

    self._sync = sync:new(do_pull, update, dict, options.prefix or 'server_list')

    return self
end

return _M
