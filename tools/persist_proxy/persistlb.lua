
--[[
    load balancer supporting:
    - persistence using redis + shared dict
    - fallback to another server in case of error
    - server selection according to 'power of two choices',
        using fails and % of fast replies for prioritization
    - exposes metrics in prom format
--]]

local http = require('resty.http')
local redis = require('resty.redis')
local balancer = require('ngx.balancer')
local serverlist = require('serverlist')

local log = ngx.log
local ERR = ngx.ERR
local INFO = ngx.INFO
local ERROR = ngx.ERROR
local HTTP_SERVICE_UNAVAILABLE = ngx.HTTP_SERVICE_UNAVAILABLE
local ngx_exit = ngx.exit
local ngx_time = ngx.time
local ngx_var = ngx.var
local ngx_null = ngx.null

local get_last_failure = balancer.get_last_failure
local set_more_tries = balancer.set_more_tries
local set_current_peer = balancer.set_current_peer

local pairs = pairs
local floor = math.floor
local concat = table.concat

local BUSY = -3
local SERVER_LIST_KEY = 'server_list'
local SERVER_LIST_PULL_OK_KEY = 'server_list_pull_ok'
local SERVER_LIST_PULL_ERR_KEY = 'server_list_pull_err'
local FAILS_KEY_SUFFIX = '_fails'
local FAILED_KEY_SUFFIX = '_failed'
local CHECKED_KEY_SUFFIX = '_checked'
local FAST_REQ_KEY_SUFFIX = '_fast_'
local SLOW_REQ_KEY_SUFFIX = '_slow_'
local PERSIST_STATUS_KEY_PREFIX = 'persist_status_'

local PERSIST_STATUSES = { 'LOCALHIT', 'REMOTEHIT', 'MISS', 'ERROR' }

local _M = { _VERSION = '0.1' }

local mt = { __index = _M }


local function _k8s_get_pod_list(self)
    local options = self._k8s_options
    local host = 'https://' .. options.host .. ':' .. options.port
    local url = host .. '/api/v1/namespaces/' .. options.namespace .. '/pods/'

    local query = {
        fieldSelector = 'status.phase=Running'
    }
    if options.label_selector then
        query.labelSelector = options.label_selector
    end

    local httpc = http.new()

    local res, err = httpc:request_uri(url, {
        query = query,
        headers = {
          ['Accept'] = 'application/json',
          ['Authorization'] = 'Bearer ' .. options.token,
        },
        keepalive_timeout = 60000,
        keepalive_pool = 10,
        ssl_verify = false,
    })

    local body, key

    if not res then
        log(ERR, 'request_uri failed: ', err)
        key = SERVER_LIST_PULL_ERR_KEY
    elseif res.status ~= 200 then
        log(ERR, 'bad status: ', res.status)
        key = SERVER_LIST_PULL_ERR_KEY
    else
        body = res.body
        key = SERVER_LIST_PULL_OK_KEY
    end

    local res, err = self._srv_dict:incr(key, 1, 0)
    if not res then
        log(ERR, 'srv_dict:incr failed: ', err)
    end

    return body
end


local function _redis_connect(self)
    local red, err = redis:new()
    if not red then
        log(ERR, 'redis:new failed: ', err)
        return
    end

    red:set_timeout(1000)

    local params = self._redis
    local res, err = red:connect(params._host, params._port)
    if not res then
        log(ERR, 'redis:connect failed: ', err, ', host: ', params._host,
            ', port: ', params._port)
        return
    end

    return red
end

local function _redis_set_keepalive(self, red)
    local params = self._redis
    local res, err = red:set_keepalive(params._max_idle_timeout,
        params._pool_size)
    if not res then
        log(ERR, 'redis:set_keepalive failed: ', err)
    end
end


local function _redis_get_server_list(self)
    local red = _redis_connect(self)
    if not red then
        return
    end

    local data, err = red:get(SERVER_LIST_KEY)
    if data == ngx_null then
        data = nil
        err = 'not found'
    end

    local key

    if not data then
        log(ERR, 'redis:get failed: ', err)
        key = SERVER_LIST_PULL_ERR_KEY
    else
        key = SERVER_LIST_PULL_OK_KEY
    end

    local res, err = self._srv_dict:incr(key, 1, 0)
    if not res then
        log(ERR, 'srv_dict:incr failed: ', err)
    end

    _redis_set_keepalive(self, red)

    return data, err
end


local function _server_update_stats(self, srv_key)
    local params = self._stats

    -- check response size within limits
    local bytes_sent = tonumber(ngx_var.bytes_sent)
    if bytes_sent < params._min_resp_size or
        bytes_sent > params._max_resp_size then
        return
    end

    -- determine whether the response was fast/slow
    local suffix
    local request_time = tonumber(ngx_var.request_time)
    if request_time < bytes_sent / params._fast_req_rate then
        suffix = FAST_REQ_KEY_SUFFIX
    else
        suffix = SLOW_REQ_KEY_SUFFIX
    end

    -- increment the counter
    local bucket_id = floor(ngx_time() / params._interval) % 2
    local key = srv_key .. suffix .. bucket_id

    -- Note: the ttl ensures the counter will be reset by the next cycle
    local res, err = self._srv_dict:incr(key, 1, 0, params._interval - 1)
    if not res then
        log(ERR, 'srv_dict:incr failed: ', err, ', key: ', key)
    end
end

local function _server_get_stats(self, srv_key)
    local srv_dict = self._srv_dict
    local fast, slow = 0, 0

    for bucket_id = 0, 1 do
        local cur_key = srv_key .. FAST_REQ_KEY_SUFFIX .. bucket_id
        fast = fast + (srv_dict:get(cur_key) or 0)

        local cur_key = srv_key .. SLOW_REQ_KEY_SUFFIX .. bucket_id
        slow = slow + (srv_dict:get(cur_key) or 0)
    end

    return fast, slow
end

local function _compare_server_perf(self, srv_key1, srv_key2)
    --[[
        true = srv2 is better
        false = srv1 is better
        nil = not enough requests on one of the servers
    --]]
    local min_stat_count = self._stats._min_stat_count

    local fast1, slow1 = _server_get_stats(self, srv_key1)
    local total1 = fast1 + slow1
    if total1 < min_stat_count then
        return
    end

    local fast2, slow2 = _server_get_stats(self, srv_key2)
    local total2 = fast2 + slow2
    if total2 < min_stat_count then
        return
    end

    return fast1 * total2 < fast2 * total1
end


local function _choose_server(self)
    local srv1, srv2 = self._srv_list:get_random_server_pair()
    if not srv2 then
        return srv1
    end

    local now = ngx_time()
    local srv_dict = self._srv_dict

    -- compare the number of fails
    local fails1 = srv_dict:get(srv1.key .. FAILS_KEY_SUFFIX) or 0
    local reset1
    if fails1 > 0 then
        local checked = srv_dict:get(srv1.key .. CHECKED_KEY_SUFFIX)
        if not checked or now > checked + self._fail_timeout then
            -- fail_timeout passed since the last time the server was checked
            --  assume fail count is zero, and set the last check time to now
            fails1 = 0
            reset1 = true
        end
    end

    local fails2 = srv_dict:get(srv2.key .. FAILS_KEY_SUFFIX) or 0
    local reset2
    if fails2 > 0 then
        local checked = srv_dict:get(srv2.key .. CHECKED_KEY_SUFFIX)
        if not checked or now > checked + self._fail_timeout then
            fails2 = 0
            reset2 = true
        end
    end

    -- Note: in case performance can't be compared, using srv1
    local srv, reset
    if fails1 < fails2 or (fails1 == fails2 and
        not _compare_server_perf(self, srv1.key, srv2.key)) then
        srv = srv1
        reset = reset1
    else
        srv = srv2
        reset = reset2
    end

    if reset then
        -- avoid checking this server until it succeeds/fails time out
        local cur_key = srv.key .. CHECKED_KEY_SUFFIX
        local res, err = srv_dict:set(cur_key, now)
        if not res then
            log(ERR, 'srv_dict:set: ', err, ', key: ', cur_key)
        end
    end

    return srv
end

local function _server_fail(self, srv_key)
    local srv_dict = self._srv_dict
    local now = ngx_time()

    local cur_key = srv_key .. FAILS_KEY_SUFFIX
    local res, err = srv_dict:incr(cur_key, 1, 0)
    if not res then
        log(ERR, 'srv_dict:incr failed: ', err, ', key: ', cur_key)
    end

    local cur_key = srv_key .. FAILED_KEY_SUFFIX
    local res, err = srv_dict:set(cur_key, now)
    if not res then
        log(ERR, 'srv_dict:set failed: ', err, ', key: ', cur_key)
    end

    local cur_key = srv_key .. CHECKED_KEY_SUFFIX
    local res, err = srv_dict:set(cur_key, now)
    if not res then
        log(ERR, 'srv_dict:set failed: ', err, ', key: ', cur_key)
    end
end

local function _server_success(self, srv_key)
    local srv_dict = self._srv_dict

    local checked = srv_dict:get(srv_key .. CHECKED_KEY_SUFFIX)
    if not checked then
        return
    end

    local failed = srv_dict:get(srv_key .. FAILED_KEY_SUFFIX)
    if not failed or failed < checked then
        srv_dict:delete(srv_key .. CHECKED_KEY_SUFFIX)
        srv_dict:delete(srv_key .. FAILED_KEY_SUFFIX)
        srv_dict:delete(srv_key .. FAILS_KEY_SUFFIX)
    end
end

local function _get_server(self, persist_key)
    local srv, srv_key

    local cache_dict = self._cache_dict
    local srv_list = self._srv_list

    -- try to get from shared dict
    srv_key = cache_dict:get(persist_key)
    if srv_key then
        srv = srv_list:get_by_key(srv_key)
        if srv then
            return srv, 'LOCALHIT'
        end
    end

    -- try to get from redis
    local red = _redis_connect(self)
    if not red then
        return nil, 'ERROR'
    end

    local try = 0

    srv_key = red:get(persist_key)

    repeat
        if srv_key and srv_key ~= ngx_null then
            srv = srv_list:get_by_key(srv_key)
            if srv then
                -- update shared dict
                local res, err = cache_dict:set(persist_key, srv.key,
                    self._dict_ttl)
                if not res then
                    log(ERR, 'cache_dict:set failed: ', err,
                        ', key: ', persist_key)
                end

                _redis_set_keepalive(self, red)
                return srv, 'REMOTEHIT'
            end
        end

        -- no valid mapping in redis, choose a server
        srv = _choose_server(self)

        try = try + 1

        -- check redis again to minimize the chances of race conditions
        --    if some other server already allocated a server - try to use it
        local prev_srv_key = srv_key
        srv_key = red:get(persist_key)
    until srv_key == prev_srv_key or try > 5

    -- update redis
    local res, err = red:setex(persist_key, self._redis_ttl, srv.key)
    if not res then
        log(ERR, 'redis:set failed: ', err, ', key: ', persist_key)
    end

    _redis_set_keepalive(self, red)

    -- update shared dict
    local res, err = cache_dict:set(persist_key, srv.key, self._dict_ttl)
    if not res then
        log(ERR, 'cache_dict:set failed: ', err, ', key: ', persist_key)
    end

    return srv, 'MISS'
end


local _RBMT = {}

local rbmt = { __index = _RBMT }

function _RBMT.balance(self)
    local list = self._list
    local idx = self._idx

    if idx > 0 then
        local srv_key = list[idx].key
        local state, code, err = get_last_failure()

        log(ERR, 'upstream failed, key: ', srv_key, ', state: ', state,
            ', code: ', code, ', err: ', err)
        _server_fail(self._balancer, srv_key)
    end

    idx = idx + 1
    self._idx = idx

    local srv = list[idx]
    if not srv then
        return ngx_exit(BUSY)
    end

    set_more_tries(1)

    local ok, err = set_current_peer(srv.host, srv.port)
    if not ok then
        log(ERR, 'set_current_peer failed, host: ', srv.host,
            ', port: ', srv.port, ', err: ', err)
        return ngx_exit(ERROR)
    end
end

function _RBMT.header_filter(self)
    local srv = self._list[self._idx]
    if srv then
        _server_success(self._balancer, srv.key)
    end
end

function _RBMT.log(self)
    local srv = self._list[self._idx]
    if srv then
        _server_update_stats(self._balancer, srv.key)
    end
end

local function _req_balancer_new(balancer, list)
    return setmetatable({
        _balancer = balancer,
        _list = list,
        _idx = 0,
    }, rbmt)
end


function _M.get_status()
    local self = _M.self
    local srv_dict = self._srv_dict

    local res = {}
    local count = 0

    for srv_key, srv in pairs(self._srv_list:get()) do
        local fails = srv_dict:get(srv_key .. FAILS_KEY_SUFFIX) or 0
        local failed = srv_dict:get(srv_key .. FAILED_KEY_SUFFIX) or 0
        local checked = srv_dict:get(srv_key .. CHECKED_KEY_SUFFIX) or 0
        local fast, slow = _server_get_stats(self, srv_key)

        local tags = '{host="' .. srv.host .. '",port="' .. srv.port .. '"} '

        count = count + 1
        res[count] = 'upstream_fails' .. tags .. fails ..
            '\nupstream_failed' .. tags .. failed ..
            '\nupstream_checked' .. tags .. checked ..
            '\nupstream_fast_reqs' .. tags .. fast ..
            '\nupstream_slow_reqs' .. tags .. slow .. '\n'
    end

    local upstream_count = count

    for _, status in pairs(PERSIST_STATUSES) do
        local cur = srv_dict:get(PERSIST_STATUS_KEY_PREFIX .. status) or 0

        count = count + 1
        res[count] = 'upstream_persist{status="' .. status .. '"} ' .. cur .. '\n'
    end

    local pull_ok = srv_dict:get(SERVER_LIST_PULL_OK_KEY) or 0
    local pull_errs = srv_dict:get(SERVER_LIST_PULL_ERR_KEY) or 0
    local parse_errs = self._srv_list:get_errors()

    res[count + 1] = 'upstream_count ' .. upstream_count ..
        '\nupstream_list_pull_ok ' .. pull_ok ..
        '\nupstream_list_pull_errs ' .. pull_errs ..
        '\nupstream_list_parse_errs ' .. parse_errs .. '\n'

    return concat(res)
end

function _M.get_req_balancer(persist_key)
    local self = _M.self
    local srv_list = self._srv_list

    if srv_list:empty() then
        log(ERR, 'no upstream servers')
        return ngx_exit(HTTP_SERVICE_UNAVAILABLE)
    end

    local srv

    if persist_key and persist_key ~= '' then
        persist_key = self._persist_prefix .. persist_key

        local status
        srv, status = _get_server(self, persist_key)

        local key = PERSIST_STATUS_KEY_PREFIX .. status
        local res, err = self._srv_dict:incr(key, 1, 0)
        if not res then
            log(ERR, 'srv_dict:incr failed: ', err, ', key: ', key)
        end

        ngx_var.persist_status = status
    end

    if not srv then
        srv = _choose_server(self)
    end

    local list = { srv }
    list = srv_list:add_servers(self._max_tries - 1, list, persist_key)

    return _req_balancer_new(self, list)
end

local function _print_table(t, prefix)
    for key, value in pairs(t) do
        if type(value) == 'table' then
            _print_table(value, prefix .. '.' .. key)
        elseif type(value) == 'string' or type(value) == 'number' then
            log(INFO, prefix .. '.' .. key, '=', value)
        end
    end
end

function _M.new(_, options)
    if _M.self then
        return _M.self
    end

    _print_table(options, 'options')

    local srv_dict = options.srv_dict
    local cache_dict = options.cache_dict
    if not srv_dict or not cache_dict then
        log(ERR, 'missing required params for balancer')
        return
    end

    local redis = {
        _host             = options.redis_host or '127.0.0.1',
        _port             = options.redis_port or 6379,
        _max_idle_timeout = options.redis_max_idle_timeout or 30000,
        _pool_size        = options.redis_pool_size or 128,
    }

    local stats = {
        _interval         = options.interval or 30,
        _min_resp_size    = options.min_resp_size or 100 * 1024,
        _max_resp_size    = options.max_resp_size or 5 * 1024 * 1024,
        _fast_req_rate    = options.fast_req_rate or 2 * 1024 * 1024,
        _min_stat_count   = options.min_stat_count or 20,
    }

    local self = setmetatable({
        _srv_dict         = srv_dict,
        _cache_dict       = cache_dict,
        _redis            = redis,
        _stats            = stats,
        _max_tries        = options.max_tries or 5,
        _redis_ttl        = options.redis_ttl or 3600,
        _dict_ttl         = options.dict_ttl or 600,
        _fail_timeout     = options.fail_timeout or 30,
        _persist_prefix   = options.persist_prefix or 'persist_',
        _k8s_options      = options.k8s,
    }, mt)

    local srvlist_options = {}
    local srvlist_pull

    if options.k8s then
        srvlist_options.format = 'k8s_pod_list'
        srvlist_pull = function()
            return _k8s_get_pod_list(self)
        end
    else
        srvlist_pull = function()
            return _redis_get_server_list(self)
        end
    end

    self._srv_list = serverlist:new(srvlist_pull, srv_dict, srvlist_options)

    _M.self = self        -- singleton
    return self
end

return _M
