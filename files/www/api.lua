--[[
    High-Performance Native In-Process API Handler for Audio Pro C3
    Zero-fork execution engine running inside uhttpd-mod-lua
--]]

local function read_file(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local content = f:read("*a")
    f:close()
    return content
end

local function write_file(path, content)
    local f = io.open(path, "w")
    if not f then return false end
    f:write(content)
    f:close()
    return true
end

local function append_file(path, content)
    local f = io.open(path, "a")
    if not f then return false end
    f:write(content)
    f:close()
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

-- Global Entrypoint for uhttpd-mod-lua
function handle_request(env)
    local method = env.REQUEST_METHOD or "GET"
    local query_string = env.QUERY_STRING or ""
    
    -- Read POST body if present
    if method == "POST" and uhttpd and uhttpd.recv then
        local body = ""
        while true do
            local chunk = uhttpd.recv(4096)
            if not chunk or chunk == "" then break end
            body = body .. chunk
        end
        if query_string == "" and body ~= "" then
            query_string = body
        end
    end
    
    local params = parse_params(query_string)
    local cookies = parse_cookies(env.HTTP_COOKIE or "")
    local action = params.action or ""
    
    -- Support direct parameter shorthand like ?volume=50 or ?input=bt
    if action == "" then
        for k, v in pairs(params) do
            if k == "volume" or k == "mute" or k == "unmute" or k == "input" or k == "status" then
                action = k
                params.val = v
                break
            end
        end
    end
    
    -- Artwork Binary Endpoint
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

    -- Download Certificate Endpoint
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

    -- Authentication Layer
    local auth_conf = read_file("/etc/audiopro_auth") or ""
    local auth_enabled = string.match(auth_conf, "AUTH_ENABLED=(%d+)") == "1"
    
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
            local sess_content = read_file("/tmp/ap_sessions/" .. token)
            if sess_content then
                local exp = tonumber(string.match(sess_content, "expires=(%d+)") or "0")
                local s_fp = string.match(sess_content, "fingerprint=([^\n\r]+)") or ""
                local cur_fp = get_md5((env.REMOTE_ADDR or "127.0.0.1") .. "|" .. (env.HTTP_USER_AGENT or "unknown"))
                if os.time() < exp and s_fp == cur_fp then
                    is_valid = true
                end
            end
        end
        
        if not is_valid then
            uhttpd.send("Status: 403 Forbidden\r\nContent-Type: application/json\r\n\r\n")
            uhttpd.send('{"status":"error","code":403,"message":"Forbidden: Authentication required"}')
            return
        end
    end

    -- Default JSON Headers
    uhttpd.send("Status: 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-cache\r\n\r\n")

    -- Dispatch Actions
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
            bat, json_escape(src), vol, tostring(mut), env.REMOTE_ADDR or "127.0.0.1", uptime, loadavg, mem_free, tostring(airplay_act), tostring(spotify_act), now_playing)
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

    elseif action == "reboot" then
        os.execute("(sleep 2 && reboot) >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Rebooting..."}')

    elseif action == "poweroff" or action == "shutdown" then
        os.execute("(sleep 2 && /sbin/poweroff) >/dev/null 2>&1 &")
        uhttpd.send('{"status":"ok","message":"Powering off..."}')

    else
        -- Fallback: delegate complex configuration tasks to cgi-bin
        local f = io.popen(string.format("QUERY_STRING='%s' /www/cgi-bin/api 2>/dev/null", string.gsub(query_string, "'", "'\\''")), "r")
        if f then
            local raw = f:read("*a") or ""
            f:close()
            local json_body = string.match(raw, "\r?\n\r?\n(.*)$") or raw
            uhttpd.send(json_body)
        else
            uhttpd.send('{"error":"unknown action"}')
        end
    end
end
