--[[
    Native In-Process Unified API Engine for Audio Pro Addon C3
    Zero-fork execution running natively inside uhttpd-mod-lua
--]]

-- Global Initialization (Runs once when uhttpd loads the module)
local function seed_prng()
    local f = io.open("/dev/urandom", "r")
    if f then
        local bytes = f:read(4)
        f:close()
        if bytes then
            local seed = 0
            for i = 1, 4 do seed = seed * 256 + string.byte(bytes, i) end
            math.randomseed(seed)
            return
        end
    end
    math.randomseed(os.time())
end
seed_prng()

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
    f:write(content)
    f:close()
    os.rename(tmp_path, path)
    return true
end

local function send_mcu(cmd)
    local f = io.open("/tmp/mcu_cmd_fifo", "w")
    if f then
        f:write(cmd)
        f:close()
        return true
    end
    f = io.open("/dev/ttyS0", "w")
    if f then
        f:write(cmd)
        f:close()
        return true
    end
    return false
end

local function urldecode(str)
    if not str then return "" end
    str = string.gsub(str, "+", " ")
    str = string.gsub(str, "%%(%x%x)", function(h)
        return string.char(tonumber(h, 16))
    end)
    return str
end

local function parse_params(query_string)
    local params = {}
    if not query_string or query_string == "" then return params end
    for pair in string.gmatch(query_string, "[^&]+") do
        local k, v = string.match(pair, "^([^=]+)=(.*)$")
        if k then
            params[urldecode(k)] = urldecode(v or "")
        else
            params[urldecode(pair)] = ""
        end
    end
    return params
end

local function parse_cookies(cookie_str)
    local cookies = {}
    if not cookie_str or cookie_str == "" then return cookies end
    for pair in string.gmatch(cookie_str, "[^;]+") do
        local k, v = string.match(pair, "^%s*([^=]+)=(.*)$")
        if k then
            cookies[k] = v:match("^%s*(.-)%s*$")
        end
    end
    return cookies
end

local function json_escape(str)
    if not str then return "" end
    str = string.gsub(str, "\\", "\\\\")
    str = string.gsub(str, '"', '\\"')
    str = string.gsub(str, "\n", "\\n")
    str = string.gsub(str, "\r", "\\r")
    str = string.gsub(str, "\t", "\\t")
    return str
end

local function get_md5(str)
    local f = io.popen("echo -n '" .. string.gsub(str, "'", "'\\''") .. "' | md5sum", "r")
    if f then
        local res = f:read("*l") or ""
        f:close()
        return string.match(res, "^(%x+)") or ""
    end
    return ""
end

local function uci_get(key, default_val)
    local f = io.popen("uci -q get " .. key .. " 2>/dev/null", "r")
    if f then
        local res = f:read("*l")
        f:close()
        if res and res ~= "" then return res end
    end
    return default_val or ""
end

local SEC_HEADERS = "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: strict-origin-when-cross-origin\r\nContent-Security-Policy: default-src 'self' 'unsafe-inline' data: blob:;\r\n"

local function verify_system_password(username, password)
    if not username or username == "" or not password then return false end
    local shadow = read_file("/etc/shadow")
    if shadow then
        for line in string.gmatch(shadow, "[^\r\n]+") do
            local u, stored = string.match(line, "^([^:]+):([^:]+)")
            if u == username then
                if stored and stored ~= "" and stored ~= "*" and stored ~= "!" then
                    local algo, salt = string.match(stored, "^%$([156])%$([^$]+)")
                    if salt and algo then
                        local cmd = string.format("openssl passwd -%s -salt '%s' '%s' 2>/dev/null", algo, string.gsub(salt, "'", "'\\''"), string.gsub(password, "'", "'\\''"))
                        local f = io.popen(cmd, "r")
                        if f then
                            local computed = f:read("*l")
                            f:close()
                            if computed == stored then return true end
                        end
                    end
                    return false
                end
                break
            end
        end
    end
    -- Initial setup fallback if no password is set in /etc/shadow
    if username == "root" and password == "admin" then return true end
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
    local fp = get_md5(client_ip .. "|" .. client_ua)

    if action == "" then
        for k, v in pairs(params) do
            if k == "volume" or k == "mute" or k == "unmute" or k == "input" or k == "status" then
                action = k
                params.val = v
                break
            end
        end
    end

    -- Firmware Binary Upload Endpoint
    if action == "upload" and method == "POST" then
        local out_f = io.open("/tmp/firmware.bin", "wb")
        local total = 0
        if out_f and uhttpd and uhttpd.recv then
            while true do
                local chunk = uhttpd.recv(8192)
                if not chunk or #chunk == 0 then break end
                out_f:write(chunk)
                total = total + #chunk
                if total > 33554432 then break end
            end
            out_f:close()
        end
        uhttpd.send("Status: 200 OK\r\nContent-Type: application/json\r\n\r\n")
        if total > 10240 and total <= 33554432 then
            local md5_pipe = io.popen("md5sum /tmp/firmware.bin 2>/dev/null", "r")
            local f_md5 = "valid"
            if md5_pipe then
                local l = md5_pipe:read("*l") or ""
                md5_pipe:close()
                f_md5 = string.match(l, "^(%x+)") or "valid"
            end
            uhttpd.send(string.format('{"status":"ok","message":"Firmware uploaded","size":%d,"md5":"%s"}', total, f_md5))
        else
            os.remove("/tmp/firmware.bin")
            uhttpd.send('{"status":"error","message":"Invalid firmware binary size"}')
        end
        return
    end

    -- Read POST body for non-upload API requests
    if method == "POST" and action ~= "upload" and uhttpd and uhttpd.recv then
        local body = ""
        while true do
            local chunk = uhttpd.recv(4096)
            if not chunk or chunk == "" then break end
            body = body .. chunk
        end
        if body ~= "" then
            local post_params = parse_params(body)
            for k, v in pairs(post_params) do params[k] = v end
            if params.action then action = params.action end
        end
    end

    -- Binary Artwork
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

    -- Certificate Download
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
    local auth_enabled = uci_get("mcud.main.auth_enabled", "0") == "1"
    local session_ttl = tonumber(uci_get("mcud.main.session_ttl", "604800")) or 604800

    if action == "login" then
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
            uhttpd.send('{"status":"error","code":429,"message":"Too many failed attempts. Please wait 30 seconds."}')
            return
        end

        if verify_system_password(u, p) then
            os.remove(fail_file)
            local token = get_md5(tostring(now) .. "_" .. fp .. "_" .. tostring(math.random(100000, 999999)))
            local exp = now + session_ttl
            write_file("/tmp/ap_sessions/" .. token, string.format("username=%s\nexpires=%d\nfingerprint=%s\ncreated=%d\n", u, exp, fp, now))
            uhttpd.send(string.format("Status: 200 OK\r\nContent-Type: application/json\r\nSet-Cookie: ap_sid=%s; Max-Age=%d; Path=/; HttpOnly; SameSite=Strict\r\n%s\r\n", token, session_ttl, SEC_HEADERS))
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
        local token = cookies.ap_sid or ""
        if token ~= "" then os.remove("/tmp/ap_sessions/" .. token) end
        uhttpd.send("Status: 200 OK\r\nContent-Type: application/json\r\nSet-Cookie: ap_sid=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict\r\n" .. SEC_HEADERS .. "\r\n")
        uhttpd.send('{"status":"ok","message":"Logged out"}')
        return
    elseif action == "check_auth" or action == "check" then
        uhttpd.send("Status: 200 OK\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
        if not auth_enabled then
            uhttpd.send(string.format('{"auth_required":false,"logged_in":true,"username":"root","fingerprint":"%s"}', fp))
            return
        end
        local token = cookies.ap_sid or ""
        local is_logged_in = false
        local user = ""
        if token ~= "" then
            local sess = read_file("/tmp/ap_sessions/" .. token)
            if sess then
                local exp = tonumber(string.match(sess, "expires=(%d+)") or "0")
                local s_fp = string.match(sess, "fingerprint=([^\n\r]+)") or ""
                local s_u = string.match(sess, "username=([^\n\r]+)") or "root"
                if now < exp and s_fp == fp then
                    is_logged_in = true
                    user = s_u
                else
                    os.remove("/tmp/ap_sessions/" .. token)
                end
            end
        end
        if is_logged_in then
            uhttpd.send(string.format('{"auth_required":true,"logged_in":true,"username":"%s","fingerprint":"%s"}', json_escape(user), fp))
        else
            uhttpd.send(string.format('{"auth_required":true,"logged_in":false,"fingerprint":"%s"}', fp))
        end
        return
    end

    -- Verify Auth Token for Protected Actions
    if auth_enabled then
        local token = cookies.ap_sid or ""
        if token == "" and env.HTTP_AUTHORIZATION then
            token = string.match(env.HTTP_AUTHORIZATION, "^[Bb]earer%s+(%x+)") or ""
        end
        if token == "" and params.token then
            token = string.match(params.token, "^(%x+)") or ""
        end
        
        token = string.match(token, "^(%x+)") or ""
        local is_valid = false
        if token ~= "" then
            local sess = read_file("/tmp/ap_sessions/" .. token)
            if sess then
                local exp = tonumber(string.match(sess, "expires=(%d+)") or "0")
                local s_fp = string.match(sess, "fingerprint=([^\n\r]+)") or ""
                if now < exp and s_fp == fp then
                    is_valid = true
                end
            end
        end
        
        if not is_valid then
            uhttpd.send("Status: 403 Forbidden\r\nContent-Type: application/json\r\n" .. SEC_HEADERS .. "\r\n")
            uhttpd.send('{"status":"error","code":403,"message":"Forbidden: Authentication required"}')
            return
        end
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

        local airplay_act = (os.execute("pidof shairport-sync >/dev/null 2>&1") == 0)
        local spotify_act = (os.execute("pidof librespot >/dev/null 2>&1") == 0)

        local now_playing = read_file("/tmp/audiopro_meta.json") or '{"active":false}'
        if now_playing == "" then now_playing = '{"active":false}' end

        local resp = string.format('{"status":"ok","battery":%d,"source":"%s","volume":%d,"mute":%s,"ip":"%s","hostname":"AudioPro-C3","uptime":%d,"load":"%s","mem_free":"%s MB","airplay":%s,"spotify":%s,"now_playing":%s}',
            bat, json_escape(src), vol, tostring(mut), client_ip, uptime, loadavg, mem_free, tostring(airplay_act), tostring(spotify_act), now_playing)
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
        os.execute("pidof librespot >/dev/null 2>&1 && kill -CONT $(pidof librespot) 2>/dev/null")
        uhttpd.send('{"status":"ok","playing":true}')

    elseif action == "player_pause" or action == "pause" then
        send_mcu("AXX+PLM+000\n")
        os.execute("pidof librespot >/dev/null 2>&1 && kill -STOP $(pidof librespot) 2>/dev/null")
        uhttpd.send('{"status":"ok","playing":false}')

    elseif action == "player_toggle" then
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
        if tts_url ~= "" then
            os.execute(string.format("/usr/bin/ha_ducking.sh tts '%s' >/dev/null 2>&1 &", string.gsub(tts_url, "'", "'\\''")))
            uhttpd.send('{"status":"ok","message":"TTS announcement queued"}')
        else
            uhttpd.send('{"status":"error","message":"URL required"}')
        end

    elseif action == "play_stream" then
        local stream_url = params.url or ""
        local stream_name = params.name or "Live Stream"
        if stream_url ~= "" then
            os.execute("killall -9 mpg123 madplay 2>/dev/null")
            local meta = string.format('{"active":true,"source":"webradio","title":"%s","artist":"Web Radio","album":"Direct Stream","playing":true,"artwork":false,"updated":%d}',
                json_escape(stream_name), os.time())
            write_file("/tmp/audiopro_meta.json", meta)
            os.execute(string.format("mpg123 -q -a music_in -- '%s' >/dev/null 2>&1 &", string.gsub(stream_url, "'", "'\\''")))
            uhttpd.send(string.format('{"status":"ok","message":"Streaming started","title":"%s"}', json_escape(stream_name)))
        else
            uhttpd.send('{"status":"error","message":"URL required"}')
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
            os.execute(string.format("uci set wireless.sta_iface.ssid='%s' 2>/dev/null; uci set wireless.sta_iface.key='%s' 2>/dev/null; uci set wireless.sta_iface.disabled='0' 2>/dev/null; uci commit wireless 2>/dev/null; (sleep 2 && wifi reload) >/dev/null 2>&1 &",
                string.gsub(ssid, "'", "'\\''"), string.gsub(key, "'", "'\\''")))
            uhttpd.send('{"status":"ok","message":"Connecting to home Wi-Fi..."}')
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
        local ap_ssid = uci_get("wireless.ap_iface.ssid", "AudioPro-C3-Setup")
        local ap_key = uci_get("wireless.ap_iface.key", "")
        local ap_chan = uci_get("wireless.radio0.channel", "auto")
        local ap_dis = (uci_get("wireless.ap_iface.disabled", "0") == "1")

        local sta_ssid = uci_get("wireless.sta_iface.ssid", "")
        local sta_key = uci_get("wireless.sta_iface.key", "")
        local sta_dis = (uci_get("wireless.sta_iface.disabled", "1") == "1")

        local def_vol = tonumber(uci_get("mcud.main.default_volume", "25")) or 25
        local auto_sleep = tonumber(uci_get("mcud.main.auto_sleep_min", "0")) or 0
        local slp_aux = (uci_get("mcud.main.sleep_in_aux", "0") == "1")
        local slp_bt = (uci_get("mcud.main.sleep_in_bt", "0") == "1")

        local mqtt_en = (uci_get("mcud.main.mqtt_enabled", "1") == "1")
        local mqtt_host = uci_get("mcud.main.mqtt_host", "127.0.0.1")
        local mqtt_port = tonumber(uci_get("mcud.main.mqtt_port", "1883")) or 1883
        local mqtt_pre = uci_get("mcud.main.mqtt_topic_prefix", "audiopro_c3")
        local mqtt_user = uci_get("mcud.main.mqtt_user", "")

        local hostname = uci_get("system.@system[0].hostname", "AudioPro-C3")

        local resp = string.format('{"status":"ok","ap_ssid":"%s","ap_key":"%s","ap_channel":"%s","ap_disabled":%s,"sta_ssid":"%s","sta_key":"%s","sta_disabled":%s,"default_volume":%d,"auto_sleep_min":%d,"sleep_in_aux":%s,"sleep_in_bt":%s,"mqtt_enabled":%s,"mqtt_host":"%s","mqtt_port":%d,"mqtt_prefix":"%s","mqtt_user":"%s","hostname":"%s"}',
            json_escape(ap_ssid), json_escape(ap_key), json_escape(ap_chan), tostring(ap_dis),
            json_escape(sta_ssid), json_escape(sta_key), tostring(sta_dis),
            def_vol, auto_sleep, tostring(slp_aux), tostring(slp_bt),
            tostring(mqtt_en), json_escape(mqtt_host), mqtt_port, json_escape(mqtt_pre), json_escape(mqtt_user), json_escape(hostname))
        uhttpd.send(resp)

    elseif action == "get_security" then
        local s_days = math.floor(session_ttl / 86400)
        local https_en = (uci_get("uhttpd.main.listen_https", "") ~= "")
        local https_redir = (uci_get("uhttpd.main.redirect_https", "0") == "1")
        local has_cert = (read_file("/etc/uhttpd.crt") ~= nil)
        uhttpd.send(string.format('{"status":"ok","auth_enabled":%s,"session_ttl":%d,"session_days":%d,"https_enabled":%s,"https_redirect":%s,"has_cert":%s}',
            tostring(auth_enabled), session_ttl, s_days, tostring(https_en), tostring(https_redir), tostring(has_cert)))

    elseif action == "save_security" then
        local auth_en = (params.auth_enabled == "1" or params.auth_enabled == "true") and 1 or 0
        local days = tonumber(params.session_days or "7") or 7
        if days < 1 then days = 1 end
        if days > 365 then days = 365 end
        local ttl = days * 86400
        write_file("/etc/audiopro_auth", string.format("AUTH_ENABLED=%d\nSESSION_TTL=%d\n", auth_en, ttl))

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
        uhttpd.send(string.format('{"status":"ok","message":"Security settings saved","auth_enabled":%d,"session_ttl":%d,"session_days":%d}', auth_en, ttl, days))

    elseif action == "save_ap" then
        local ssid = params.ap_ssid or ""
        local key = params.ap_key or ""
        local chan = params.ap_channel or "auto"
        local dis = (params.ap_disabled == "1" or params.ap_disabled == "true") and "1" or "0"

        if ssid ~= "" then os.execute(string.format("uci set wireless.ap_iface.ssid='%s' 2>/dev/null", string.gsub(ssid, "'", "'\\''"))) end
        if key ~= "" then
            if #key >= 8 then
                os.execute(string.format("uci set wireless.ap_iface.encryption='psk2' 2>/dev/null; uci set wireless.ap_iface.key='%s' 2>/dev/null", string.gsub(key, "'", "'\\''")))
            else
                uhttpd.send('{"status":"error","message":"AP password must be at least 8 characters"}')
                return
            end
        else
            os.execute("uci set wireless.ap_iface.encryption='none' 2>/dev/null; uci -q del wireless.ap_iface.key 2>/dev/null")
        end
        if chan ~= "" then os.execute(string.format("uci set wireless.radio0.channel='%s' 2>/dev/null", string.gsub(chan, "'", "'\\''"))) end
        os.execute(string.format("uci set wireless.ap_iface.disabled='%s' 2>/dev/null; uci commit wireless 2>/dev/null; (sleep 1 && wifi reload) >/dev/null 2>&1 &", dis))
        uhttpd.send('{"status":"ok","message":"AP settings applied"}')

    elseif action == "save_audio" then
        local dvol = tonumber(params.default_volume or "25") or 25
        local aslp = tonumber(params.auto_sleep_min or "0") or 0
        local slp_aux = (params.sleep_in_aux == "1" or params.sleep_in_aux == "true") and "1" or "0"
        local slp_bt = (params.sleep_in_bt == "1" or params.sleep_in_bt == "true") and "1" or "0"

        os.execute(string.format("uci set mcud.main.default_volume='%d'; uci set mcud.main.auto_sleep_min='%d'; uci set mcud.main.sleep_in_aux='%s'; uci set mcud.main.sleep_in_bt='%s'; uci commit mcud; /etc/init.d/mcud restart >/dev/null 2>&1 &",
            dvol, aslp, slp_aux, slp_bt))
        uhttpd.send('{"status":"ok","message":"Audio & Power settings saved"}')

    elseif action == "save_device" then
        local host = string.lower(string.gsub(params.hostname or "", "[^%w%-]", ""))
        if host ~= "" then
            os.execute(string.format("uci set system.@system[0].hostname='%s'; uci commit system; echo '%s' > /proc/sys/kernel/hostname 2>/dev/null; sed -i 's/name = .*/name = \"%s\";/' /etc/shairport-sync.conf 2>/dev/null; uci set shairport-sync.shairport_sync.name='%s' 2>/dev/null; uci commit shairport-sync 2>/dev/null",
                host, host, host, host))
            uhttpd.send(string.format('{"status":"ok","message":"Device name updated to %s"}', host))
        else
            uhttpd.send('{"status":"error","message":"Hostname cannot be empty"}')
        end

    elseif action == "save_mqtt" then
        local en = (params.mqtt_enabled == "1" or params.mqtt_enabled == "true") and "1" or "0"
        local host = params.mqtt_host or "127.0.0.1"
        local port = tonumber(params.mqtt_port or "1883") or 1883
        local pre = params.mqtt_prefix or "audiopro_c3"
        local user = params.mqtt_user or ""
        local pass = params.mqtt_pass or ""

        os.execute(string.format("uci set mcud.main.mqtt_enabled='%s'; uci set mcud.main.mqtt_host='%s'; uci set mcud.main.mqtt_port='%d'; uci set mcud.main.mqtt_topic_prefix='%s'; uci set mcud.main.mqtt_user='%s'; uci set mcud.main.mqtt_password='%s'; uci commit mcud; /etc/init.d/mcud restart >/dev/null 2>&1 &",
            en, string.gsub(host, "'", "'\\''"), port, string.gsub(pre, "'", "'\\''"), string.gsub(user, "'", "'\\''"), string.gsub(pass, "'", "'\\''")))
        uhttpd.send('{"status":"ok","message":"MQTT settings saved"}')

    elseif action == "get_presets" then
        local p1_m = uci_get("audiopro_presets.1.mode", "ha")
        local p1_n = uci_get("audiopro_presets.1.name", "Preset 1")
        local p1_u = uci_get("audiopro_presets.1.url", "")
        local p1_c = uci_get("audiopro_presets.1.command", "")

        local p2_m = uci_get("audiopro_presets.2.mode", "ha")
        local p2_n = uci_get("audiopro_presets.2.name", "Preset 2")
        local p2_u = uci_get("audiopro_presets.2.url", "")
        local p2_c = uci_get("audiopro_presets.2.command", "")

        local p3_m = uci_get("audiopro_presets.3.mode", "ha")
        local p3_n = uci_get("audiopro_presets.3.name", "Preset 3")
        local p3_u = uci_get("audiopro_presets.3.url", "")
        local p3_c = uci_get("audiopro_presets.3.command", "")

        local p4_m = uci_get("audiopro_presets.4.mode", "ha")
        local p4_n = uci_get("audiopro_presets.4.name", "Preset 4")
        local p4_u = uci_get("audiopro_presets.4.url", "")
        local p4_c = uci_get("audiopro_presets.4.command", "")

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

        os.execute(string.format("uci set audiopro_presets.%d.name='%s' 2>/dev/null; uci set audiopro_presets.%d.mode='%s' 2>/dev/null; uci set audiopro_presets.%d.url='%s' 2>/dev/null; uci set audiopro_presets.%d.command='%s' 2>/dev/null; uci commit audiopro_presets 2>/dev/null",
            pid, string.gsub(pname, "'", "'\\''"), pid, string.gsub(pmode, "'", "'\\''"), pid, string.gsub(purl, "'", "'\\''"), pid, string.gsub(pcmd, "'", "'\\''")))
        uhttpd.send(string.format('{"status":"ok","message":"Preset %d updated"}', pid))

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
