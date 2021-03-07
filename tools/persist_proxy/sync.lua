
--[[
    periodically loads a remote resource and saves it to a shared dict.
    changes to the resource are reported to all worker processes via callback.
--]]

local log = ngx.log
local ERR = ngx.ERR
local INFO = ngx.INFO
local ngx_time = ngx.time
local timer_at = ngx.timer.at
local timer_every = ngx.timer.every
local setmetatable = setmetatable

local _M = { _VERSION = '0.1' }

local mt = { __index = _M }

local _sync


local function _pull_remote(self)
    local dict = self._dict
    if not dict:add(self._lock_key, true, self._lock_exp) then
        return
    end

    local new_value, err = self._pull()
    if not new_value then
        log(ERR, 'remote pull failed: ', err)
        return
    end

    local value_key = self._value_key

    local cur_value = dict:get(value_key)
    if cur_value == new_value then
        return
    end

    log(INFO, 'pulled new value from remote')

    local success, err = dict:set(value_key, new_value)
    if not success then
        log(ERR, 'dict:set: ', err, ', key: ', value_key)
        return
    end

    local version, err = dict:incr(self._version_key, 1, 0)
    if not version then
        log(ERR, 'dict:incr: ', err, ', key: ', self._version_key)
        return
    end

    log(INFO, 'updated from remote, new version: ', version)

    self._version = version
    self._update(new_value)
end

local function _pull_shared(self)
    local dict = self._dict

    local version = dict:get(self._version_key)
    if version == self._version then
        return
    end

    log(INFO, 'pulling from shared, version: ', version)

    local new_value = dict:get(self._value_key)
    if not new_value then
        log(ERR, 'dict:get failed, key: ', self._value_key)
        return
    end

    self._version = version
    self._update(new_value)
end
_M.pull_shared = _pull_shared

function _sync(premature, self)
    if premature then
        return
    end

    local now = ngx_time()
    local dict = self._dict

    local updated = dict:get(self._updated_key)
    if not updated or now >= updated + self._remote_period then
        local success, err = dict:set(self._updated_key, now)
        if not success then
            log(ERR, 'dict:set: ', err, ', key: ', self._updated_key)
        end

        _pull_remote(self)
    end

    _pull_shared(self)
end

function _M.new(_, pull, update, dict, prefix, options)
    if not options then
        options = {}
    end

    local res = setmetatable({
        _pull          = pull,
        _update        = update,
        _dict          = dict,
        _value_key     = prefix .. '_value',
        _version_key   = prefix .. '_version',
        _updated_key   = prefix .. '_updated',
        _lock_key      = prefix .. '_lock',
        _lock_exp      = options.lock_exp or 1,
        _shared_period = options.shared_period or 10,
        _remote_period = options.remote_period or 30,
    }, mt)

    local hdl, err = timer_at(0, _sync, res)
    if not hdl then
        log(ERR, 'ngx.timer.at failed: ', err)
    end

    local hdl, err = timer_every(res._shared_period, _sync, res)
    if not hdl then
        log(ERR, 'ngx.timer.every failed: ', err)
    end

    return res
end

return _M
