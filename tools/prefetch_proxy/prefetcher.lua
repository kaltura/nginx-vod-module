local http = require 'resty.http'

local ngx_ERR = ngx.ERR
local ngx_log = ngx.log
local ngx_req = ngx.req
local ngx_req_get_headers = ngx_req.get_headers
local ngx_timer = ngx.timer
local ngx_var = ngx.var

local base_url = '127.0.0.1'

local _M = {}

local function fetch_url(premature, uri, headers)
    local httpc = http.new()
    local res, err = httpc:request_uri(uri, {
        headers = headers,
        ssl_verify = false
    })
end

function _M.prefetch_segments(prefetch_segment_count, url_prefix, req_segment, url_suffix)
    -- prevent request loops
    if ngx_var.http_x_kaltura_proxy == 'prefetch' then
        return
    end
    local headers = ngx_req_get_headers()
    headers['X-Kaltura-Proxy'] = 'prefetch'

    -- prefetch segments
    local full_prefix = ngx_var.scheme .. '://' .. base_url .. ':' .. ngx_var.server_port .. url_prefix
    req_segment = tonumber(req_segment)
    for segment = req_segment + 1, req_segment + prefetch_segment_count do
        local url = full_prefix .. segment .. url_suffix

        -- set a timer of 0 sec to perform the request in the background
        local ok, err = ngx_timer.at(0, fetch_url, url, headers)
        if not ok then
            ngx_log(ngx_ERR, 'failed to create timer: ', err)
            break
        end
    end
end

return _M
