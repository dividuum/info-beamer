--======================
-- Wrap unsafe functions
--======================

do
    -- Disable Precompiled
    local old_loadstring = loadstring
    local string_byte = string.byte
    local error = error

    function loadstring(code, chunkname)
        if string_byte(code, 1) == 27 then
            error("no precompiled code")
        else
            return old_loadstring(code, chunkname)
        end
    end

    -- Rep can take too much memory/cpu
    local old_rep = string.rep
    string.rep = function(s, n)
        if n > 8192 then
            error("n too large")
        elseif n < 0 then
            error("n cannot be negative")
        end
        return old_rep(s, n)
    end
end

--=================
-- Helper functions
--=================

function scale_into(source_width, source_height, target_width, target_height)
    local prop_height = source_height * target_width / source_width
    local prop_width  = source_width * target_height / source_height
    local x1, y1, x2, y2
    if prop_height > target_height then
        local x_center = target_width / 2
        local half_width = prop_width / 2
        x1 = x_center - half_width
        y1 = 0
        x2 = x_center + half_width
        y2 = target_height
    else
        local y_center = target_height / 2
        local half_height = prop_height / 2
        x1 = 0
        y1 = y_center - half_height
        x2 = target_width
        y2 = y_center + half_height
    end
    return x1, y1, x2, y2
end

--=============
-- Sandboxing
--=============

function init_sandbox()
    local sandbox = {
        error = error;
        assert = assert;
        ipairs = ipairs;
        next = next;
        pairs = pairs;
        pcall = pcall;
        rawequal = rawequal;
        rawget = rawget;
        rawset = rawset;
        select = select;
        tonumber = tonumber;
        tostring = tostring;
        type = type;
        unpack = unpack;
        xpcall = xpcall;
        setmetatable = setmetatable;
        struct = struct;

        coroutine = {
            create = coroutine.create;
            resume = coroutine.resume;
            running = coroutine.running;
            status = coroutine.status;
            wrap = coroutine.wrap;
            yield = coroutine.yield;
        };

        debug = {
            traceback = function(message, level)
                local message = tostring(message or "")
                local level = tonumber(level) or 1
                assert(level >= 0, "level is negative")
                assert(level < 256, "level too large")
                return debug.traceback(message, level)
            end;
        };

        math = {
            abs = math.abs;
            acos = math.acos;
            asin = math.asin;
            atan = math.atan;
            atan2 = math.atan2;
            ceil = math.ceil;
            cos = math.cos;
            cosh = math.cosh;
            deg = math.deg;
            exp = math.exp;
            floor = math.floor;
            fmod = math.fmod;
            frexp= math.frexp;
            ldexp = math.ldexp;
            log = math.log;
            log10 = math.log10;
            max = math.max;
            min = math.min;
            modf = math.modf;
            pi = math.pi;
            pow = math.pow;
            rad = math.rad;
            sin = math.sin;
            sinh = math.sinh;
            sqrt = math.sqrt;
            tan = math.tan;
            tanh = math.tanh;
            random = math.random;
            randomseed = math.randomseed;
        };

        string = {
            byte = string.byte;
            char = string.char;
            find = string.find;
            format = string.format;
            gmatch = string.gmatch;
            gsub = string.gsub;
            len = string.len;
            lower = string.lower;
            match = string.match;
            rep = string.rep;
            reverse = string.reverse;
            sub = string.sub;
            upper = string.upper;
        };

        table = {
            insert = table.insert;
            concat = table.concat;
            maxn = table.maxn;
            remove = table.remove;
            sort = table.sort;
            show = table.show;
        };

        print = print;

        player = {
            setup = setup;
            render_child = render_child;
            clear = clear;
            load_image = load_image;
            load_video = load_video;
            load_font = load_font;
            load_file = load_file;
        };

        sys = {
            now = now;
            list_childs = list_childs;
            send_child = send_child;
            scale_into = scale_into;
        };

        news = {
            on_content_update = function(name) 
                -- print("}}} lua: content update " .. name)
            end;

            on_content_remove = function(name)
                print("{{{ lua: content remove " .. name)
            end;

            on_render = function()

            end;

            on_raw_data = function(data, is_osc)
                -- print(PATH, "on_data", is_osc)
                if is_osc then
                    if string.byte(data, 1, 1) ~= 44 then
                        print("no osc type tag string")
                        return
                    end
                    local typetags, offset = struct.unpack(">!4s", data)
                    local tags = {string.byte(typetags, 1, offset)}
                    local fmt = ">!4"
                    for idx, tag in ipairs(tags) do
                        if tag == 44 then -- ,
                            fmt = fmt .. "s"
                        elseif tag == 105 then -- i
                            fmt = fmt .. "i4"
                        elseif tag == 102 then -- f
                            fmt = fmt .. "f"
                        elseif tag == 98 then -- b
                            print("no blob support")
                            return
                        else
                            print("unknown type tag " .. string.char(tag))
                            return
                        end
                    end
                    local unpacked = {struct.unpack(fmt, data)}
                    table.remove(unpacked, 1) -- remove typetags
                    table.remove(unpacked, #unpacked) -- remove trailing offset
                    return sandbox.news.on_osc(unpack(unpacked))
                else
                    return sandbox.news.on_data(data)
                end
            end;

            on_data = function(data)
                print(PATH, "on_data")
            end;

            on_osc = function(...)
                print(PATH, "on_osc", ...)
            end;

            on_msg = function(data)
                print(PATH, "on_msg")
            end;
        };

        NAME = NAME;
        PATH = PATH;
    }
    sandbox._G = sandbox
    sandbox.loadstring = function(...)
        return setfenv(assert(loadstring(...)), sandbox)
    end
    return sandbox
end

function render_into_screen(screen_width, screen_height)
    local image = render_self()
    root_width, root_height = image:size()
    clear(0.05, 0.05, 0.05, 1)
    local x1, y1, x2, y2 = scale_into(root_width, root_height, screen_width, screen_height)
    image:draw(x1, y1, x2, y2)
end

-- Einige Funktionen in der registry speichern, 
-- so dass der C Teil dran kommt.
do
    local registry = debug.getregistry()

    registry.traceback = debug.traceback

    registry.execute = function(cmd, ...)
        if cmd == "code" then
            setfenv(
                assert(loadstring(..., "usercode: " .. PATH)),
                sandbox
            )()
        elseif cmd == "callback" then
            setfenv(
                function(callback, ...)
                    news[callback](...)
                end,
                sandbox
            )(...)
        elseif cmd == "init_sandbox" then
            sandbox = init_sandbox()
        elseif cmd == "render_self" then
            local width, height = ...
            render_into_screen(width, height)
        end
    end

    registry.alarm = function()
        error("CPU usage too high")
    end
end

io = nil
require = nil
loadfile = nil
load = nil
package = nil
module = nil
os = nil
dofile = nil
getfenv = nil
debug = { traceback = debug.traceback }
