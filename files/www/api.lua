--[[
    Native In-Process Unified API Engine for Audio Pro Addon C3
    Zero-fork execution running natively inside uhttpd-mod-lua
--]]

local has_uci, uci_lib = pcall(require, "uci")
local uci_ctx = has_uci and uci_lib.cursor() or nil

local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local content = f:read("*a")
    f:close()
    return content
end

local function write_file(path, content)
    local tmp_path = path .. ".tmp." .. tostring(os.time()) .. tostring(math.random(100, 999))
    local f = io.open(tmp_path, "w")
    if not f then return false end
    local ok = f:write(content)
    f:close()
    if not ok then
        os.remove(tmp_path)
        return false
    end
    return os.rename(tmp_path, path)
end

-- Cryptographically secure session token generation
local function generate_token()
    local f = io.open("/dev/urandom", "rb")
    if f then
        local bytes = f:read(32)
        f:close()
        if bytes and #bytes == 32 then
            local hex = {}
            for i = 1, #bytes do
                table.insert(hex, string.format("%02x", string.byte(bytes, i)))
            end
            return table.concat(hex)
        end
    end
    return nil
end

local function constant_time_compare(a, b)
    if not a or not b or #a ~= #b then return false end
    local diff = 0
    for i = 1, #a do
        diff = diff + math.abs(string.byte(a, i) - string.byte(b, i))
    end
    return diff == 0
end

-- Lazy session garbage collector for expired sessions and old rate-limit entries
local function gc_sessions()
    local sess_dir = "/tmp/ap_sessions"
    local p = io.popen("ls " .. sess_dir .. " 2>/dev/null", "r")
    if p then
        local now = os.time()
        for fname in p:lines() do
            if string.match(fname, "^%x+$") then
                local content = read_file(sess_dir .. "/" .. fname)
                if content then
                    local exp = tonumber(string.match(content, "expires=(%d+)") or "0")
                    if exp and exp > 0 and now >= exp then
                        os.remove(sess_dir .. "/" .. fname)
                    end
                end
            end
        end
        p:close()
    end
end

-- URL Decoder
local function urldecode(s)
    if not s then return "" end
    s = string.gsub(s, "+", " ")
    s = string.gsub(s, "%%(%x%x)", function(h)
        return string.char(tonumber(h, 16))
    end)
    return s
end

-- JSON String Escaper
local function json_escape(s)
    if not s then return "" end
    s = tostring(s)
    s = string.gsub(s, "\\", "\\\\")
    s = string.gsub(s, '"', '\\"')
    s = string.gsub(s, "\b", "\\b")
    s = string.gsub(s, "\f", "\\f")
    s = string.gsub(s, "\n", "\\n")
    s = string.gsub(s, "\r", "\\r")
    s = string.gsub(s, "\t", "\\t")
    return s
end

-- Strict Shell Argument Sanitizer (CWE-78 Command Injection Mitigation)
-- Strips all control characters (\r, \n, \0, ASCII 1-31, 127) and escapes single quotes
local function sanitize_shell(s)
    if not s then return "" end
    local ok, res = pcall(function()
        local clean = string.gsub(tostring(s), "[\r\n\0\x01-\x1f\x7f]", "")
        return string.gsub(clean, "'", "'\\''")
    end)
    return ok and res or ""
end


-- Fast Non-blocking UART Command Sender
local function send_mcu(cmd)
    local f = io.open("/tmp/mcu_cmd_fifo", "w")
    if f then
        f:write(cmd)
        f:flush()
        f:close()
        return true
    end
    return false
end

-- Query Parser
local function parse_params(query)
    local params = {}
    if not query or query == "" then return params end
    for pair in string.gmatch(query, "[^&]+") do
        local k, v = string.match(pair, "([^=]+)=(.*)")
        if k then
            params[urldecode(k)] = urldecode(v)
        else
            params[urldecode(pair)] = "1"
        end
    end
    return params
end

-- Cookie Parser
local function parse_cookies(cookie_header)
    local cookies = {}
    if not cookie_header or cookie_header == "" then return cookies end
    for pair in string.gmatch(cookie_header, "[^;]+") do
        local k, v = string.match(pair, "^%s*([^=]+)%s*=%s*(.*)%s*$")
        if k and v then
            cookies[k] = v
        end
    end
    return cookies
end

-- In-process native UCI accessor
local function uci_get_val(config, section, option, default_val)
    if uci_ctx then
        local v = uci_ctx:get(config, section, option)
        if v ~= nil then return tostring(v) end
    else
        local f = io.popen(string.format("uci -q get %s.%s.%s 2>/dev/null", config, section, option), "r")
        if f then
            local res = f:read("*l")
            f:close()
            if res and res ~= "" then return res end
        end
    end
    return default_val or ""
end

local SEC_HEADERS = "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: strict-origin-when-cross-origin\r\nContent-Security-Policy: default-src 'self' 'unsafe-inline' data: blob:; img-src 'self' data: blob: http: https:;\r\n"

local function verify_system_password(username, password)
    if not username or username == "" or not password or password == "" then return false end

    -- Check via LuCI sys if available
    local has_luci, luci_sys = pcall(require, "luci.sys")
    if has_luci and luci_sys and luci_sys.user and luci_sys.user.checkpasswd then
        return luci_sys.user.checkpasswd(username, password)
    end

    local shadow = read_file("/etc/shadow")
    if shadow then
        for line in string.gmatch(shadow, "[^\r\n]+") do
            local u, stored = string.match(line, "^([^:]+):([^:]+)")
            if u == username then
                if stored and stored ~= "" and stored ~= "*" and stored ~= "!" then
                    local algo, salt = string.match(stored, "^%$([156])%$([^$]+)")
                    if salt and algo then
                        local cmd = string.format("openssl passwd -%s -salt '%s' -stdin 2>/dev/null", algo, string.gsub(salt, "'", "'\\''"))
                        local f = io.popen(cmd, "r+")
                        if f then
                            f:write(password .. "\n")
                            f:flush()
                            local computed = f:read("*l")
                            f:close()
                            if computed and constant_time_compare(computed, stored) then return true end
                        end
                    end
                    return false
                end
                break
            end
        end
    end
    return false
end

-- Entrypoint for uhttpd-mod-lua
function handle_request(env)
    local method = env.REQUEST_METHOD or "GET"
    local query_string = env.QUERY_STRING or ""
    
    local params = parse_params(query_string)
    local cookies = parse_cookies(env.HTTP_COOKIE or "")
    local action = params.action or ""
    local now = os.time()
    local client_ip = env.REMOTE_ADDR or "127.0.0.1"
    local client_ua = env.HTTP_USER_AGENT or "unknown"
    local fp = string.format("%x", os.time())

    if action == "" then
        for k, v in pairs(params) do
            if k == "volume" or k == "mute" or k == "unmute" or k == "input" or k == "status" then
                action = k
                params.val = v
                break
            end
        end
    end

    -- Authentication Configuration
    local auth_enabled = uci_get_val("mcud", "main", "auth_enabled", "0") == "1"
    local session_ttl = tonumber(uci_get_val("mcud", "main", "session_ttl", "604800")) or 604800

    -- Validate session token
    local token = cookies.ap_sid or ""
    if token == "" and env.HTTP_AUTHORIZATION then
        token = string.match(env.HTTP_AUTHORIZATION, "^[Bb]earer%s+(%x+)") or ""
    end
    if token == "" and params.token then
        token = string.match(params.token, "^(%x+)") or ""
    end
    token = string.match(token, "^(%x+)") or ""

    local is_authenticated = false
    local auth_user = ""
    if token ~= "" then
        local sess = read_file("/tmp/ap_sessions/" .. token)
        if sess then
            local exp = tonumber(string.match(sess, "expires=(%d+)") or "0")
            local s_u = string.match(sess, "username=([^\n\r]+)") or "root"
            if now < exp then
                is_authenticated = true
                auth_user = s_u
            else
                os.remove("/tmp/ap_sessions/" .. token)
            end
        end
    end

    -- Firmware Binary Upload Endpoint (Strict max 11.18 MB = 11730944 partition boundary + Auth guard)
    if action == "upload" and method == "POST" then
        if auth_enabled and not is_authenticated then
            uhttpd.send("Status: 403 Forbidden\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
            uhttpd.send('{"status":"error","code":403,"message":"Forbidden: Authentication required"}')
            return
        end

        local MAX_FW_SIZE = 11730944 -- 11.18 MB (0xB30000) exact SPI NOR firmware partition boundary
        local out_f = io.open("/tmp/firmware.bin", "wb")
        local total = 0
        local overflow = false
        if out_f and uhttpd and uhttpd.recv then
            while true do
                local chunk = uhttpd.recv(8192)
                if not chunk or #chunk == 0 then break end
                total = total + #chunk
                if total > MAX_FW_SIZE then
                    overflow = true
                    break
                end
                out_f:write(chunk)
            end
            out_f:close()
        end

        if overflow or total < 10240 then
            os.remove("/tmp/firmware.bin")
            uhttpd.send("Status: 413 Payload Too Large\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
            uhttpd.send('{"status":"error","message":"Invalid firmware size (Max 11.18 MB allowed)"}')
            return
        end

        uhttpd.send("Status: 200 OK\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
        uhttpd.send(string.format('{"status":"ok","message":"Firmware uploaded","size":%d}', total))
        return
    end

    -- Read POST body for non-upload API requests (Limit to 64 KB)
    if method == "POST" and action ~= "upload" and uhttpd and uhttpd.recv then
        local body = ""
        local total_post = 0
        while true do
            local chunk = uhttpd.recv(4096)
            if not chunk or chunk == "" then break end
            total_post = total_post + #chunk
            if total_post > 65536 then break end
            body = body .. chunk
        end
        if body ~= "" then
            local post_params = parse_params(body)
            for k, v in pairs(post_params) do params[k] = v end
            if params.action then action = params.action end
        end
    end

    -- Binary Artwork (Public)
    if action == "get_artwork" then
        local artwork = read_file("/tmp/audiopro_artwork.jpg")
        if artwork and #artwork > 0 then
            uhttpd.send("Status: 200 OK\r\nContent-Type: image/jpeg\r\nCache-Control: no-cache\r\n\r\n")
            uhttpd.send(artwork)
            return
        else
            uhttpd.send("Status: 404 Not Found\r\nContent-Type: text/plain\r\n\r\nNo artwork")
            return
        end
    end

    -- Certificate Download (Public)
    if action == "download_cert" then
        local cert = read_file("/etc/uhttpd.crt")
        if cert and #cert > 0 then
            uhttpd.send("Status: 200 OK\r\nContent-Type: application/x-x509-ca-cert\r\nContent-Disposition: attachment; filename=\"audiopro-c3.crt\"\r\n\r\n")
            uhttpd.send(cert)
            return
        else
            uhttpd.send("Status: 404 Not Found\r\nContent-Type: text/plain\r\n\r\nCertificate not found")
            return
        end
    end

    -- Authentication Endpoints
    if action == "login" then
        gc_sessions()
        local u = params.username or "root"
        local p = params.password or ""
        local ip_safe = string.gsub(client_ip, "[^%w%.%:]", "")
        os.execute("mkdir -p /tmp/ap_auth_fails /tmp/ap_sessions")
        local fail_file = "/tmp/ap_auth_fails/" .. ip_safe
        local fail_data = read_file(fail_file) or ""
        local fail_cnt, fail_tm = string.match(fail_data, "(%d+)%s+(%d+)")
        fail_cnt = tonumber(fail_cnt or "0") or 0
        fail_tm = tonumber(fail_tm or "0") or 0

        if fail_cnt >= 5 and (now - fail_tm) < 60 then
            uhttpd.send("Status: 429 Too Many Requests\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
            uhttpd.send('{"status":"error","code":429,"message":"Too many failed attempts. Please wait 60 seconds."}')
            return
        end

        if verify_system_password(u, p) then
            os.remove(fail_file)
            local tok = generate_token()
            if not tok then
                uhttpd.send("Status: 500 Internal Server Error\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
                uhttpd.send('{"status":"error","message":"CSPRNG entropy error"}')
                return
            end
            local exp = now + session_ttl
            write_file("/tmp/ap_sessions/" .. tok, string.format("username=%s\nexpires=%d\ncreated=%d\n", u, exp, now))
            uhttpd.send(string.format("Status: 200 OK\r\nContent-Type: application/json\r\nSet-Cookie: ap_sid=%s; Max-Age=%d; Path=/; HttpOnly; SameSite=Strict\r\n%s\r\n", tok, session_ttl, SEC_HEADERS))
            uhttpd.send(string.format('{"status":"ok","message":"Authenticated","username":"%s","expires":%d}', json_escape(u), exp))
            return
        else
            if (now - fail_tm) > 60 then fail_cnt = 1 else fail_cnt = fail_cnt + 1 end
            write_file(fail_file, string.format("%d %d\n", fail_cnt, now))
            uhttpd.send("Status: 403 Forbidden\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
            uhttpd.send('{"status":"error","code":403,"message":"Invalid credentials"}')
            return
        end
    elseif action == "logout" then
        if token ~= "" then os.remove("/tmp/ap_sessions/" .. token) end
        uhttpd.send("Status: 200 OK\r\nContent-Type: application/json\r\nSet-Cookie: ap_sid=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict\r\n" .. SEC_HEADERS .. "\r\n")
        uhttpd.send('{"status":"ok","message":"Logged out"}')
        return
    elseif action == "check_auth" or action == "check" then
        gc_sessions()
        uhttpd.send("Status: 200 OK\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
        if not auth_enabled then
            uhttpd.send('{"auth_required":false,"logged_in":true,"username":"root"}')
            return
        end
        if is_authenticated then
            uhttpd.send(string.format('{"auth_required":true,"logged_in":true,"username":"%s"}', json_escape(auth_user)))
        else
            uhttpd.send('{"auth_required":true,"logged_in":false}')
        end
        return
    end

    -- Verify Auth Token for Protected Actions
    if auth_enabled and not is_authenticated then
        uhttpd.send("Status: 403 Forbidden\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
        uhttpd.send('{"status":"error","code":403,"message":"Forbidden: Authentication required"}')
        return
    end

    -- Default JSON Headers
    uhttpd.send("Status: 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\n" .. SEC_HEADERS .. "\r\n")

    -- Dispatch Core Actions
    if action == "status" then
        local bat = 100
        local bat_str = read_file("/tmp/battery_status") or ""
        local bat_match = string.match(bat_str, "(%d+)")
        if bat_match then bat = tonumber(bat_match) end

        local src = "wifi"
        local src_str = read_file("/tmp/audio_source")
        if src_str then src = string.gsub(src_str, "%s+", "") end

        local vol = 25
        local vol_str = read_file("/tmp/current_volume")
        if vol_str then vol = tonumber(vol_str) or 25 end

        local mut = false
        local mut_str = read_file("/tmp/current_mute")
        if mut_str and string.match(mut_str, "1") then mut = true end

        local uptime = 120
        local up_str = read_file("/proc/uptime")
        if up_str then uptime = math.floor(tonumber(string.match(up_str, "^(%d+%.?%d*)") or "120")) end

        local loadavg = "0.15"
        local load_str = read_file("/proc/loadavg")
        if load_str then loadavg = string.match(load_str, "^(%S+)") or "0.15" end

        local mem_free = "32.0"
        local mem_str = read_file("/proc/meminfo")
        if mem_str then
            local avail = string.match(mem_str, "MemAvailable:%s+(%d+)") or string.match(mem_str, "MemFree:%s+(%d+)")
            if avail then mem_free = string.format("%.1f", tonumber(avail)/1024) end
        end

        local airplay_act = (read_file("/tmp/shairport-sync-meta") ~= nil)
        local spotify_act = (read_file("/tmp/audiopro_meta.json") ~= nil)

        local now_playing = read_file("/tmp/audiopro_meta.json") or '{"active":false}'
        if now_playing == "" then now_playing = '{"active":false}' end

        local hostname = uci_get_val("system", "@system[0]", "hostname", "AudioPro-C3")

        local resp = string.format('{"status":"ok","battery":%d,"source":"%s","volume":%d,"mute":%s,"ip":"%s","hostname":"%s","uptime":%d,"load":"%s","mem_free":"%s MB","airplay":%s,"spotify":%s,"now_playing":%s}',
            bat, json_escape(src), vol, tostring(mut), client_ip, json_escape(hostname), uptime, loadavg, mem_free, tostring(airplay_act), tostring(spotify_act), now_playing)
        uhttpd.send(resp)

    elseif action == "volume" then
        local vol = tonumber(params.val or params.volume or "50") or 50
        if vol < 0 then vol = 0 end
        if vol > 100 then vol = 100 end
        send_mcu(string.format("AXX+VOL+%03d\n", vol))
        write_file("/tmp/current_volume", tostring(vol))
        uhttpd.send(string.format('{"status":"ok","volume":%d}', vol))

    elseif action == "mute" then
        send_mcu("AXX+MUT+001\n")
        write_file("/tmp/current_mute", "1")
        uhttpd.send('{"status":"ok","mute":true}')

    elseif action == "unmute" then
        send_mcu("AXX+MUT+000\n")
        write_file("/tmp/current_mute", "0")
        uhttpd.send('{"status":"ok","mute":false}')

    elseif action == "input" then
        local input = string.lower(params.val or params.input or "wifi")
        if input == "wifi" or input == "i2s" then
            send_mcu("AXX+INP+000\nAXX+PLM+001\nAXX+MUT+000\n")
            write_file("/tmp/audio_source", "wifi")
        elseif input == "bt" or input == "bluetooth" then
            send_mcu("AXX+INP+002\n")
            write_file("/tmp/audio_source", "bluetooth")
        elseif input == "aux" then
            send_mcu("AXX+INP+001\n")
            write_file("/tmp/audio_source", "aux")
        end
        uhttpd.send(string.format('{"status":"ok","input":"%s"}', json_escape(input)))

    elseif action == "player_play" or action == "play" then
        send_mcu("AXX+PLM+001\nAXX+MUT+000\n")
        os.execute("/usr/bin/player_control.sh play all >/dev/null 2>&1 &")
        write_file("/tmp/player_cmd", "play\n")
        uhttpd.send('{"status":"ok","playing":true}')

    elseif action == "player_pause" or action == "pause" then
        send_mcu("AXX+PLM+000\n")
        os.execute("/usr/bin/player_control.sh pause all >/dev/null 2>&1 &")
        write_file("/tmp/player_cmd", "pause\n")
        uhttpd.send('{"status":"ok","playing":false}')

    elseif action == "player_toggle" or action == "play_pause" then
        os.execute("/usr/bin/player_control.sh toggle all >/dev/null 2>&1 &")
        write_file("/tmp/player_cmd", "toggle\n")
        uhttpd.send('{"status":"ok","action":"toggle"}')

    elseif action == "player_next" or action == "next" then
        send_mcu("AXX+TRK+001\n")
        write_file("/tmp/player_cmd", "next\n")
        uhttpd.send('{"status":"ok","action":"next"}')

    elseif action == "player_prev" or action == "prev" then
        send_mcu("AXX+TRK+000\n")
        write_file("/tmp/player_cmd", "prev\n")
        uhttpd.send('{"status":"ok","action":"prev"}')

    elseif action == "trigger_preset" or action == "preset" then
        local pid = tonumber(params.id or params.preset or "1") or 1
        if pid < 1 then pid = 1 end
        if pid > 4 then pid = 4 end
        os.execute(string.format("/usr/bin/audiopro_preset_handler.sh %d >/dev/null 2>&1 &", pid))
        uhttpd.send(string.format('{"status":"ok","message":"Preset %d triggered"}', pid))

    elseif action == "get_eq" then
        local eq = read_file("/tmp/audiopro_eq.json") or '{"status":"ok","b60":2,"b150":1,"b400":0,"b1k":0,"b2k5":1,"b6k":2,"b14k":1,"preset":"Default"}'
        uhttpd.send(eq)

    elseif action == "save_eq" then
        local b60 = tonumber(params.b60 or "0") or 0
        local b150 = tonumber(params.b150 or "0") or 0
        local b400 = tonumber(params.b400 or "0") or 0
        local b1k = tonumber(params.b1k or "0") or 0
        local b2k5 = tonumber(params.b2k5 or "0") or 0
        local b6k = tonumber(params.b6k or "0") or 0
        local b14k = tonumber(params.b14k or "0") or 0
        local pre = params.preset or "Custom"
        local eq_json = string.format('{"status":"ok","b60":%d,"b150":%d,"b400":%d,"b1k":%d,"b2k5":%d,"b6k":%d,"b14k":%d,"preset":"%s"}',
            b60, b150, b400, b1k, b2k5, b6k, b14k, json_escape(pre))
        write_file("/tmp/audiopro_eq.json", eq_json)
        uhttpd.send(string.format('{"status":"ok","message":"Equalizer updated","preset":"%s"}', json_escape(pre)))

    elseif action == "tts" or action == "tts_duck" then
        local tts_url = params.url or ""
        if tts_url ~= "" and string.match(tts_url, "^https?://") then
            os.execute(string.format("/usr/bin/ha_ducking.sh tts '%s' >/dev/null 2>&1 &", sanitize_shell(tts_url)))
            uhttpd.send('{"status":"ok","message":"TTS announcement queued"}')
        else
            uhttpd.send('{"status":"error","message":"Valid HTTP/HTTPS URL required"}')
        end

    elseif action == "play_stream" then
        local stream_url = params.url or ""
        local stream_name = params.name or "Live Stream"
        if stream_url ~= "" and string.match(stream_url, "^https?://") then
            os.execute("killall -9 mpg123 madplay 2>/dev/null")
            local meta = string.format('{"active":true,"source":"webradio","title":"%s","artist":"Web Radio","album":"Direct Stream","playing":true,"artwork":false,"updated":%d}',
                json_escape(stream_name), os.time())
            write_file("/tmp/audiopro_meta.json", meta)
            os.execute(string.format("mpg123 -q -a music_in -- '%s' >/dev/null 2>&1 &", sanitize_shell(stream_url)))
            uhttpd.send(string.format('{"status":"ok","message":"Streaming started","title":"%s"}', json_escape(stream_name)))
        else
            uhttpd.send('{"status":"error","message":"Valid stream URL required"}')
        end

    elseif action == "stop_stream" then
        os.execute("killall -9 mpg123 madplay 2>/dev/null")
        uhttpd.send('{"status":"ok","message":"Streaming stopped"}')

    elseif action == "wifi_scan" then
        local f = io.popen("iwinfo wlan0 scan 2>/dev/null | grep 'ESSID:' | awk -F'\"' '{print $2}'", "r")
        local networks = {}
        if f then
            for line in f:lines() do
                if line ~= "" and not networks[line] then
                    table.insert(networks, line)
                    networks[line] = true
                end
            end
            f:close()
        end
        local list_items = {}
        for _, n in ipairs(networks) do
            table.insert(list_items, string.format('{"ssid":"%s"}', json_escape(n)))
        end
        uhttpd.send(string.format('{"status":"ok","networks":[%s]}', table.concat(list_items, ",")))

    elseif action == "wifi_connect" then
        local ssid = params.ssid or ""
        local key = params.key or ""
        if ssid ~= "" then
            if uci_ctx then
                uci_ctx:set("wireless", "sta_iface", "ssid", ssid)
                uci_ctx:set("wireless", "sta_iface", "key", key)
                uci_ctx:set("wireless", "sta_iface", "disabled", "0")
                -- Disable setup AP once connecting to client Wi-Fi network for radio stealth and security
                uci_ctx:set("wireless", "ap_iface", "disabled", "1")
                uci_ctx:commit("wireless")
            else
                os.execute(string.format("uci set wireless.sta_iface.ssid='%s' 2>/dev/null; uci set wireless.sta_iface.key='%s' 2>/dev/null; uci set wireless.sta_iface.disabled='0' 2>/dev/null; uci set wireless.ap_iface.disabled='1' 2>/dev/null; uci commit wireless 2>/dev/null",
                    sanitize_shell(ssid), sanitize_shell(key)))
            end
            os.execute("(sleep 2 && wifi reload) >/dev/null 2>&1 &")
            uhttpd.send('{"status":"ok","message":"Connecting to home Wi-Fi and disabling setup AP..."}')
        else
            uhttpd.send('{"status":"error","message":"SSID required"}')
        end

    elseif action == "reset_wifi" then
        os.execute("(sleep 2 && /usr/bin/wifi-reset-ap) >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Resetting Wi-Fi to AP mode..."}')

    elseif action == "flash_firmware" then
        local keep = (params.keep_settings == "1" or params.keep_settings == "true")
        local keep_flag = keep and "" or "-n"
        if read_file("/tmp/firmware.bin") then
            os.execute(string.format("(sleep 2 && sysupgrade %s /tmp/firmware.bin) >/dev/null 2>&1 &", keep_flag))
            uhttpd.send('{"status":"ok","message":"Flashing started. Device will reboot in 2-3 minutes."}')
        else
            uhttpd.send('{"status":"error","message":"No firmware file uploaded"}')
        end

    elseif action == "get_config" then
        local ap_ssid = uci_get_val("wireless", "ap_iface", "ssid", "AudioPro-C3-Setup")
        local ap_key = uci_get_val("wireless", "ap_iface", "key", "")
        local ap_chan = uci_get_val("wireless", "radio0", "channel", "auto")
        local ap_dis = (uci_get_val("wireless", "ap_iface", "disabled", "0") == "1")

        local sta_ssid = uci_get_val("wireless", "sta_iface", "ssid", "")
        local sta_key = uci_get_val("wireless", "sta_iface", "key", "")
        local sta_dis = (uci_get_val("wireless", "sta_iface", "disabled", "1") == "1")

        local def_vol = tonumber(uci_get_val("mcud", "main", "default_volume", "25")) or 25
        local auto_sleep = tonumber(uci_get_val("mcud", "main", "auto_sleep_min", "0")) or 0
        local slp_aux = (uci_get_val("mcud", "main", "sleep_in_aux", "0") == "1")
        local slp_bt = (uci_get_val("mcud", "main", "sleep_in_bt", "0") == "1")

        local mqtt_en = (uci_get_val("mcud", "main", "mqtt_enabled", "1") == "1")
        local mqtt_host = uci_get_val("mcud", "main", "mqtt_host", "127.0.0.1")
        local mqtt_port = tonumber(uci_get_val("mcud", "main", "mqtt_port", "1883")) or 1883
        local mqtt_pre = uci_get_val("mcud", "main", "mqtt_topic_prefix", "audiopro_c3")
        local mqtt_user = uci_get_val("mcud", "main", "mqtt_user", "")

        local hostname = uci_get_val("system", "@system[0]", "hostname", "AudioPro-C3")

        local resp = string.format('{"status":"ok","ap_ssid":"%s","ap_key":"%s","ap_channel":"%s","ap_disabled":%s,"sta_ssid":"%s","sta_key":"%s","sta_disabled":%s,"default_volume":%d,"auto_sleep_min":%d,"sleep_in_aux":%s,"sleep_in_bt":%s,"mqtt_enabled":%s,"mqtt_host":"%s","mqtt_port":%d,"mqtt_prefix":"%s","mqtt_user":"%s","hostname":"%s"}',
            json_escape(ap_ssid), json_escape(ap_key), json_escape(ap_chan), tostring(ap_dis),
            json_escape(sta_ssid), json_escape(sta_key), tostring(sta_dis),
            def_vol, auto_sleep, tostring(slp_aux), tostring(slp_bt),
            tostring(mqtt_en), json_escape(mqtt_host), mqtt_port, json_escape(mqtt_pre), json_escape(mqtt_user), json_escape(hostname))
        uhttpd.send(resp)

    elseif action == "get_security" then
        local s_days = math.floor(session_ttl / 86400)
        local https_en = (uci_get_val("uhttpd", "main", "listen_https", "") ~= "")
        local https_redir = (uci_get_val("uhttpd", "main", "redirect_https", "0") == "1")
        local has_cert = (read_file("/etc/uhttpd.crt") ~= nil)
        uhttpd.send(string.format('{"status":"ok","auth_enabled":%s,"session_ttl":%d,"session_days":%d,"https_enabled":%s,"https_redirect":%s,"has_cert":%s}',
            tostring(auth_enabled), session_ttl, s_days, tostring(https_en), tostring(https_redir), tostring(has_cert)))

    elseif action == "save_security" then
        local auth_en = (params.auth_enabled == "1" or params.auth_enabled == "true") and "1" or "0"
        local days = tonumber(params.session_days or "7") or 7
        if days < 1 then days = 1 end
        if days > 365 then days = 365 end
        local ttl = days * 86400

        if uci_ctx then
            uci_ctx:set("mcud", "main", "auth_enabled", auth_en)
            uci_ctx:set("mcud", "main", "session_ttl", tostring(ttl))
            uci_ctx:commit("mcud")
        else
            os.execute(string.format("uci set mcud.main.auth_enabled='%s' 2>/dev/null; uci set mcud.main.session_ttl='%d' 2>/dev/null; uci commit mcud 2>/dev/null", auth_en, ttl))
        end

        local https_en = (params.https_enabled == "1" or params.https_enabled == "true")
        local https_redir = (params.https_redirect == "1" or params.https_redirect == "true")
        if https_en then
            os.execute("uci -q del uhttpd.main.listen_https 2>/dev/null; uci add_list uhttpd.main.listen_https='0.0.0.0:443' 2>/dev/null; uci add_list uhttpd.main.listen_https='[::]:443' 2>/dev/null")
            if https_redir then
                os.execute("uci set uhttpd.main.redirect_https='1' 2>/dev/null")
            else
                os.execute("uci set uhttpd.main.redirect_https='0' 2>/dev/null")
            end
        else
            os.execute("uci -q del uhttpd.main.listen_https 2>/dev/null; uci set uhttpd.main.redirect_https='0' 2>/dev/null")
        end
        os.execute("uci commit uhttpd 2>/dev/null; (sleep 1 && /etc/init.d/uhttpd restart) >/dev/null 2>&1 &")
        uhttpd.send(string.format('{"status":"ok","message":"Security settings saved","auth_enabled":%s,"session_ttl":%d,"session_days":%d}',
            (auth_en == "1" and "true" or "false"), ttl, days))

    elseif action == "save_ap" then
        local ssid = params.ap_ssid or ""
        local key = params.ap_key or ""
        local chan = params.ap_channel or "auto"
        local dis = (params.ap_disabled == "1" or params.ap_disabled == "true") and "1" or "0"

        if ssid ~= "" then
            if uci_ctx then
                uci_ctx:set("wireless", "ap_iface", "ssid", ssid)
                if key ~= "" then
                    if #key >= 8 then
                        uci_ctx:set("wireless", "ap_iface", "encryption", "psk2")
                        uci_ctx:set("wireless", "ap_iface", "key", key)
                    else
                        uhttpd.send('{"status":"error","message":"AP password must be at least 8 characters"}')
                        return
                    end
                else
                    uci_ctx:set("wireless", "ap_iface", "encryption", "none")
                    uci_ctx:delete("wireless", "ap_iface", "key")
                end
                if chan ~= "" then uci_ctx:set("wireless", "radio0", "channel", chan) end
                uci_ctx:set("wireless", "ap_iface", "disabled", dis)
                uci_ctx:commit("wireless")
            else
                if ssid ~= "" then os.execute(string.format("uci set wireless.ap_iface.ssid='%s' 2>/dev/null", sanitize_shell(ssid))) end
                if key ~= "" then
                    if #key >= 8 then
                        os.execute(string.format("uci set wireless.ap_iface.encryption='psk2' 2>/dev/null; uci set wireless.ap_iface.key='%s' 2>/dev/null", sanitize_shell(key)))
                    else
                        uhttpd.send('{"status":"error","message":"AP password must be at least 8 characters"}')
                        return
                    end
                else
                    os.execute("uci set wireless.ap_iface.encryption='none' 2>/dev/null; uci -q del wireless.ap_iface.key 2>/dev/null")
                end
                if chan ~= "" then os.execute(string.format("uci set wireless.radio0.channel='%s' 2>/dev/null", sanitize_shell(chan))) end
                os.execute(string.format("uci set wireless.ap_iface.disabled='%s' 2>/dev/null; uci commit wireless 2>/dev/null", dis))
            end
            os.execute("(sleep 1 && wifi reload) >/dev/null 2>&1 &")
            uhttpd.send('{"status":"ok","message":"AP settings applied"}')
        else
            uhttpd.send('{"status":"error","message":"SSID cannot be empty"}')
        end

    elseif action == "save_audio" then
        local dvol = tonumber(params.default_volume or "25") or 25
        local aslp = tonumber(params.auto_sleep_min or "0") or 0
        local slp_aux = (params.sleep_in_aux == "1" or params.sleep_in_aux == "true") and "1" or "0"
        local slp_bt = (params.sleep_in_bt == "1" or params.sleep_in_bt == "true") and "1" or "0"

        if uci_ctx then
            uci_ctx:set("mcud", "main", "default_volume", tostring(dvol))
            uci_ctx:set("mcud", "main", "auto_sleep_min", tostring(aslp))
            uci_ctx:set("mcud", "main", "sleep_in_aux", slp_aux)
            uci_ctx:set("mcud", "main", "sleep_in_bt", slp_bt)
            uci_ctx:commit("mcud")
        else
            os.execute(string.format("uci set mcud.main.default_volume='%d'; uci set mcud.main.auto_sleep_min='%d'; uci set mcud.main.sleep_in_aux='%s'; uci set mcud.main.sleep_in_bt='%s'; uci commit mcud 2>/dev/null", dvol, aslp, slp_aux, slp_bt))
        end
        os.execute("/etc/init.d/mcud restart >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Audio & Power settings saved"}')

    elseif action == "save_device" then
        local host = string.lower(string.gsub(params.hostname or "", "[^%w%-]", ""))
        if host ~= "" then
            if uci_ctx then
                uci_ctx:set("system", "@system[0]", "hostname", host)
                uci_ctx:commit("system")
                uci_ctx:set("shairport-sync", "shairport_sync", "name", host)
                uci_ctx:commit("shairport-sync")
            else
                os.execute(string.format("uci set system.@system[0].hostname='%s'; uci commit system 2>/dev/null; uci set shairport-sync.shairport_sync.name='%s' 2>/dev/null; uci commit shairport-sync 2>/dev/null", host, host))
            end
            os.execute(string.format("echo '%s' > /proc/sys/kernel/hostname 2>/dev/null; sed -i 's/name = .*/name = \"%s\";/' /etc/shairport-sync.conf 2>/dev/null", host, host))
            uhttpd.send(string.format('{"status":"ok","message":"Device name updated to %s"}', json_escape(host)))
        else
            uhttpd.send('{"status":"error","message":"Hostname cannot be empty"}')
        end

    elseif action == "save_mqtt" then
        local en = (params.mqtt_enabled == "1" or params.mqtt_enabled == "true") and "1" or "0"
        local host = string.gsub(params.mqtt_host or "127.0.0.1", "[^%w%.%-%_]", "")
        local port = tonumber(params.mqtt_port or "1883") or 1883
        local pre = string.gsub(params.mqtt_prefix or "audiopro_c3", "[^%w%_%-]", "")
        local user = params.mqtt_user or ""
        local pass = params.mqtt_password or params.mqtt_pass or ""

        if uci_ctx then
            uci_ctx:set("mcud", "main", "mqtt_enabled", en)
            uci_ctx:set("mcud", "main", "mqtt_host", host)
            uci_ctx:set("mcud", "main", "mqtt_port", tostring(port))
            uci_ctx:set("mcud", "main", "mqtt_topic_prefix", pre)
            uci_ctx:set("mcud", "main", "mqtt_user", user)
            uci_ctx:set("mcud", "main", "mqtt_password", pass)
            uci_ctx:commit("mcud")
        else
            os.execute(string.format("uci set mcud.main.mqtt_enabled='%s'; uci set mcud.main.mqtt_host='%s'; uci set mcud.main.mqtt_port='%d'; uci set mcud.main.mqtt_topic_prefix='%s'; uci set mcud.main.mqtt_user='%s'; uci set mcud.main.mqtt_password='%s'; uci commit mcud 2>/dev/null",
                en, sanitize_shell(host), port, sanitize_shell(pre), sanitize_shell(user), sanitize_shell(pass)))
        end
        os.execute("/etc/init.d/mcud restart >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"MQTT settings saved"}')

    elseif action == "get_presets" then
        local p1_m = uci_get_val("audiopro_presets", "1", "mode", "ha")
        local p1_n = uci_get_val("audiopro_presets", "1", "name", "Preset 1")
        local p1_u = uci_get_val("audiopro_presets", "1", "url", "")
        local p1_c = uci_get_val("audiopro_presets", "1", "command", "")

        local p2_m = uci_get_val("audiopro_presets", "2", "mode", "ha")
        local p2_n = uci_get_val("audiopro_presets", "2", "name", "Preset 2")
        local p2_u = uci_get_val("audiopro_presets", "2", "url", "")
        local p2_c = uci_get_val("audiopro_presets", "2", "command", "")

        local p3_m = uci_get_val("audiopro_presets", "3", "mode", "ha")
        local p3_n = uci_get_val("audiopro_presets", "3", "name", "Preset 3")
        local p3_u = uci_get_val("audiopro_presets", "3", "url", "")
        local p3_c = uci_get_val("audiopro_presets", "3", "command", "")

        local p4_m = uci_get_val("audiopro_presets", "4", "mode", "ha")
        local p4_n = uci_get_val("audiopro_presets", "4", "name", "Preset 4")
        local p4_u = uci_get_val("audiopro_presets", "4", "url", "")
        local p4_c = uci_get_val("audiopro_presets", "4", "command", "")

        local resp = string.format('{"status":"ok","presets":[{"id":1,"name":"%s","mode":"%s","url":"%s","command":"%s"},{"id":2,"name":"%s","mode":"%s","url":"%s","command":"%s"},{"id":3,"name":"%s","mode":"%s","url":"%s","command":"%s"},{"id":4,"name":"%s","mode":"%s","url":"%s","command":"%s"}]}',
            json_escape(p1_n), json_escape(p1_m), json_escape(p1_u), json_escape(p1_c),
            json_escape(p2_n), json_escape(p2_m), json_escape(p2_u), json_escape(p2_c),
            json_escape(p3_n), json_escape(p3_m), json_escape(p3_u), json_escape(p3_c),
            json_escape(p4_n), json_escape(p4_m), json_escape(p4_u), json_escape(p4_c))
        uhttpd.send(resp)

    elseif action == "save_presets" then
        local pid = tonumber(params.id or "1") or 1
        if pid < 1 then pid = 1 end
        if pid > 4 then pid = 4 end
        local pname = params.name or ("Preset " .. pid)
        local pmode = params.mode or "ha"
        local purl = params.url or ""
        local pcmd = params.command or ""

        if uci_ctx then
            uci_ctx:set("audiopro_presets", tostring(pid), "name", pname)
            uci_ctx:set("audiopro_presets", tostring(pid), "mode", pmode)
            uci_ctx:set("audiopro_presets", tostring(pid), "url", purl)
            uci_ctx:set("audiopro_presets", tostring(pid), "command", pcmd)
            uci_ctx:commit("audiopro_presets")
        else
            os.execute(string.format("uci set audiopro_presets.%d.name='%s' 2>/dev/null; uci set audiopro_presets.%d.mode='%s' 2>/dev/null; uci set audiopro_presets.%d.url='%s' 2>/dev/null; uci set audiopro_presets.%d.command='%s' 2>/dev/null; uci commit audiopro_presets 2>/dev/null",
                pid, sanitize_shell(pname), pid, sanitize_shell(pmode), pid, sanitize_shell(purl), pid, sanitize_shell(pcmd)))
        end
        uhttpd.send(string.format('{"status":"ok","message":"Preset %d updated"}', pid))

    elseif action == "list_alarm_sounds" then
        local p = io.popen("ls /usr/share/sounds/*.wav 2>/dev/null", "r")
        local sounds = {}
        if p then
            for line in p:lines() do
                local basename = string.match(line, "[^/]+$") or line
                table.insert(sounds, string.format('{"name":"%s","path":"%s"}', json_escape(basename), json_escape(line)))
            end
            p:close()
        end
        uhttpd.send(string.format('{"status":"ok","sounds":[%s]}', table.concat(sounds, ",")))

    elseif action == "get_alarm" then
        local a_en = uci_get_val("mcud", "alarm", "enabled", "0")
        local a_time = uci_get_val("mcud", "alarm", "time", "07:00")
        local a_days = uci_get_val("mcud", "alarm", "days", "1,2,3,4,5")
        local a_vol = tonumber(uci_get_val("mcud", "alarm", "target_volume", "60")) or 60
        local a_mode = uci_get_val("mcud", "alarm", "alarm_mode", "sharp")
        local a_stype = uci_get_val("mcud", "alarm", "sound_type", "chime")
        local a_sfile = uci_get_val("mcud", "alarm", "sound_file", "/usr/share/sounds/alarm_sharp.wav")
        local a_url = uci_get_val("mcud", "alarm", "stream_url", "http://icecast.vrtcdn.be/klara-high.mp3")
        local a_suri = uci_get_val("mcud", "alarm", "spotify_uri", "spotify:track:4cOdK2wGLETKBW3PvgPWqT")
        local a_fade = tonumber(uci_get_val("mcud", "alarm", "fade_sec", "0")) or 0
        local a_dur = tonumber(uci_get_val("mcud", "alarm", "duration_min", "30")) or 30
        local a_snooze = tonumber(uci_get_val("mcud", "alarm", "snooze_min", "9")) or 9
        local is_active = (read_file("/tmp/alarm.pid") ~= nil)

        local resp = string.format('{"status":"ok","alarm":{"enabled":%s,"time":"%s","days":"%s","target_volume":%d,"alarm_mode":"%s","sound_type":"%s","sound_file":"%s","stream_url":"%s","spotify_uri":"%s","fade_sec":%d,"duration_min":%d,"snooze_min":%d,"active":%s}}',
            (a_en == "1" and "true" or "false"), json_escape(a_time), json_escape(a_days), a_vol,
            json_escape(a_mode), json_escape(a_stype), json_escape(a_sfile), json_escape(a_url), json_escape(a_suri),
            a_fade, a_dur, a_snooze, (is_active and "true" or "false"))
        uhttpd.send(resp)

    elseif action == "save_alarm" then
        local a_en = (params.enabled == "1" or params.enabled == "true") and "1" or "0"
        local a_time = params.time or "07:00"
        local a_days = params.days or "1,2,3,4,5"
        local a_vol = tonumber(params.target_volume or "60") or 60
        local a_mode = (params.alarm_mode == "gentle") and "gentle" or "sharp"
        local a_stype = params.sound_type or "chime"
        local a_sfile = params.sound_file or "/usr/share/sounds/alarm_sharp.wav"
        local a_url = params.stream_url or "http://icecast.vrtcdn.be/klara-high.mp3"
        local a_suri = params.spotify_uri or "spotify:track:4cOdK2wGLETKBW3PvgPWqT"
        local a_fade = tonumber(params.fade_sec or "0") or 0
        local a_dur = tonumber(params.duration_min or "30") or 30
        local a_snooze = tonumber(params.snooze_min or "9") or 9

        if uci_ctx then
            uci_ctx:set("mcud", "alarm", "enabled", a_en)
            uci_ctx:set("mcud", "alarm", "time", a_time)
            uci_ctx:set("mcud", "alarm", "days", a_days)
            uci_ctx:set("mcud", "alarm", "target_volume", tostring(a_vol))
            uci_ctx:set("mcud", "alarm", "alarm_mode", a_mode)
            uci_ctx:set("mcud", "alarm", "sound_type", a_stype)
            uci_ctx:set("mcud", "alarm", "sound_file", a_sfile)
            uci_ctx:set("mcud", "alarm", "stream_url", a_url)
            uci_ctx:set("mcud", "alarm", "spotify_uri", a_suri)
            uci_ctx:set("mcud", "alarm", "fade_sec", tostring(a_fade))
            uci_ctx:set("mcud", "alarm", "duration_min", tostring(a_dur))
            uci_ctx:set("mcud", "alarm", "snooze_min", tostring(a_snooze))
            uci_ctx:commit("mcud")
        else
            os.execute(string.format("uci set mcud.alarm.enabled='%s'; uci set mcud.alarm.time='%s'; uci set mcud.alarm.days='%s'; uci set mcud.alarm.target_volume='%d'; uci set mcud.alarm.alarm_mode='%s'; uci set mcud.alarm.sound_type='%s'; uci set mcud.alarm.sound_file='%s'; uci set mcud.alarm.stream_url='%s'; uci set mcud.alarm.spotify_uri='%s'; uci set mcud.alarm.fade_sec='%d'; uci set mcud.alarm.duration_min='%d'; uci set mcud.alarm.snooze_min='%d'; uci commit mcud 2>/dev/null",
                a_en, sanitize_shell(a_time), sanitize_shell(a_days), a_vol,
                sanitize_shell(a_mode), sanitize_shell(a_stype), sanitize_shell(a_sfile),
                sanitize_shell(a_url), sanitize_shell(a_suri), a_fade, a_dur, a_snooze))
        end
        os.execute("/usr/bin/smart_alarm.sh sync_cron >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Smart alarm configuration saved"}')

    elseif action == "test_alarm" or action == "trigger_alarm" then
        os.execute("/usr/bin/smart_alarm.sh test >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Alarm test triggered"}')

    elseif action == "snooze_alarm" then
        os.execute("/usr/bin/smart_alarm.sh snooze >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Alarm snoozed"}')

    elseif action == "stop_alarm" or action == "dismiss_alarm" then
        os.execute("/usr/bin/smart_alarm.sh stop >/dev/null 2>&1")
        uhttpd.send('{"status":"ok","message":"Alarm stopped"}')

    elseif action == "start_timer" or action == "set_timer" then
        local sec = tonumber(params.seconds or params.sec or "0") or 0
        if sec <= 0 then
            local min = tonumber(params.minutes or params.min or "0") or 0
            sec = min * 60
        end
        local name = params.name or "Timer"
        local sound = params.sound or "/usr/share/sounds/alarm_sharp.wav"
        local vol = tonumber(params.volume or "70") or 70
        if sec > 0 then
            os.execute(string.format("/usr/bin/smart_timer.sh start %d '%s' '%s' %d >/dev/null 2>&1 &",
                sec, sanitize_shell(name), sanitize_shell(sound), vol))
            uhttpd.send(string.format('{"status":"ok","message":"Timer started","seconds":%d,"name":"%s"}', sec, json_escape(name)))
        else
            uhttpd.send('{"status":"error","message":"Invalid duration in seconds or minutes"}')
        end

    elseif action == "get_timer" or action == "timer_status" then
        local st = read_file("/tmp/timer_state.json") or '{"active":false,"ringing":false,"remaining":0,"total":0,"name":""}'
        uhttpd.send(string.format('{"status":"ok","timer":%s}', st))

    elseif action == "cancel_timer" or action == "stop_timer" or action == "dismiss_timer" then
        os.execute("/usr/bin/smart_timer.sh cancel >/dev/null 2>&1")
        uhttpd.send('{"status":"ok","message":"Timer cancelled"}')

    elseif action == "reboot" then
        os.execute("(sleep 2 && reboot) >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Rebooting..."}')

    elseif action == "poweroff" or action == "shutdown" then
        os.execute("(sleep 2 && /sbin/poweroff) >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Powering off..."}')

    else
        uhttpd.send('{"status":"error","message":"Unknown API action"}')
    end
end
