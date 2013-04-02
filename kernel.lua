-- See Copyright Notice in LICENSE.txt

function kprint(msg)
    print("kernel: " .. msg)
end

function safe_loadstring(code, chunkname, allow_precompiled)
    if not allow_precompiled and string.byte(code, 1) == 27 then
        return nil, string.format(
            "precompiled code not allowed for chunk '%s'",
            chunkname
        )
    else
        return loadstring(code, chunkname)
    end
end

--=============
-- Sandboxing
--=============

-- list of childs/contents for this node.
local CHILDS = {}
local CONTENTS = {}

-- "persistent" table for this node. survives reloads
local N = {}

function create_sandbox()
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
        getmetatable = getmetatable;

        module = function(name, ...)
            local module = sandbox.package.loaded[name]
            if not module then
                module = sandbox._G[name]
            end
            if not module then
                module = {
                    _NAME = name;
                    _PACKAGE = name;
                }
                module._M = module
            end
            -- Make sure setfenv won't change the outer
            -- environment.
            if getfenv(2) == _G then
                error("cannot modify outer environment")
            end
            setfenv(2, module)
            for _, func in ipairs({...}) do
                module = func(module)
            end
            sandbox._G[name] = module
            sandbox.package.loaded[name] = module
            return module
        end;

        struct = {
            unpack = struct.unpack;
        };

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
            find = function(s, pattern, init, plain)
                if #s > 32768 then
                    error("s too large")
                elseif #pattern > 4096 then
                    error("pattern too large")
                end
                return string.find(s, pattern, init, plain)
            end;
            format = string.format;
            gmatch = string.gmatch;
            gsub = string.gsub;
            len = string.len;
            lower = string.lower;
            match = string.match;
            rep = function(s, n)
                if n > 8192 then
                    error("n too large")
                elseif n < 0 then
                    error("n cannot be negative")
                end
                return string.rep(s, n)
            end;
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
        };

        print = print;

        loadstring = function(code, chunkname)
            local func, err = safe_loadstring(code, chunkname, false)
            if func then
                return setfenv(func, sandbox)
            else
                return nil, err
            end
        end;

        resource = {
            render_child = render_child;
            load_image = load_image;
            load_video = load_video;
            load_font = load_font;
            load_file = load_file;
            create_shader = create_shader;
            create_vnc = create_vnc;
            create_snapshot = create_snapshot;
        };

        gl = {
            setup = function(width, height)
                setup(width, height)
                sandbox.WIDTH = width
                sandbox.HEIGHT = height
            end;
            clear = glClear;
            pushMatrix = glPushMatrix;
            popMatrix = glPopMatrix;
            rotate = glRotate;
            translate = glTranslate;
            scale = glScale;
            ortho = glOrtho;
            perspective = glPerspective;
        };

        sys = {
            now = now;
        };

        events = {
            child_add = {};
            child_remove = {};
            content_update = {};
            content_remove = {};

            osc = {};
            data = {};

            connect = {};
            input = {};
            disconnect = {};

            raw_data = {
                function(data, is_osc, suffix)
                    if is_osc then
                        if string.byte(data, 1, 1) ~= 44 then
                            kprint("no osc type tag string")
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
                                kprint("no blob support")
                                return
                            else
                                kprint("unknown type tag " .. string.char(tag))
                                return
                            end
                        end
                        local unpacked = {struct.unpack(fmt, data)}
                        table.remove(unpacked, 1) -- remove typetags
                        table.remove(unpacked, #unpacked) -- remove trailing offset
                        sandbox.node.dispatch("osc", suffix, unpack(unpacked))
                    else
                        sandbox.node.dispatch("data", data, suffix)
                    end
                end;
            };

            render = {
                function()
                    sandbox.node.render()
                end
            };
        };

        node = {
            alias = set_alias;

            event = function(event, handler)
                table.insert(sandbox.events[event], handler)
            end;

            dispatch = function(event, ...)
                for _, handler in ipairs(sandbox.events[event]) do
                    handler(...)
                end
            end;

            render = function()
            end;
        };

        NAME = NAME;
        PATH = PATH;

        CHILDS = CHILDS;
        CONTENTS = CONTENTS;

        N = N;
    }

    -- There is only one metatable for strings. Reset it
    -- to the sandbox controlled version.
    local string_mt = getmetatable("")
    for k, v in pairs(string_mt) do
        string_mt[k] = nil
    end
    string_mt.__index = sandbox.string

    sandbox._G = sandbox
    return sandbox
end

function load_into_sandbox(code, chunkname, allow_precompiled)
    setfenv(
        assert(safe_loadstring(code, chunkname, allow_precompiled)),
        sandbox
    )()
end

function reload(...)
    sandbox = create_sandbox()

    reset_error()

    -- load userlib
    load_into_sandbox(
        USERLIB,
        "=userlib.lua",
        true
    )

    -- load all given files into the sandbox
    for _, usercode_file in ipairs({...}) do
        load_into_sandbox(
            load_file(usercode_file),
            "=" .. PATH .. "/" .. usercode_file,
            os.getenv("INFOBEAMER_PRECOMPILED")
        )
    end

    -- send child / content events
    for name, added in pairs(CHILDS) do
        sandbox.node.dispatch("child_add", name)
    end
    for name, added in pairs(CONTENTS) do
        sandbox.node.dispatch("content_update", name)
    end
end

-- Einige Funktionen in der registry speichern, 
-- so dass der C Teil dran kommt.
do
    local registry = debug.getregistry()
    local full_scale = os.getenv("INFOBEAMER_FULLSCALE")

    registry.traceback = debug.traceback

    registry.execute = function(cmd, ...)
        if cmd == "boot" then
            kprint("booting node")
            reload(NODE_CODE_FILE)
        elseif cmd == "event" then
            sandbox.node.dispatch(...)
        elseif cmd == "child_update" then
            local name, added = ...
            if added then
                CHILDS[name] = now()
                sandbox.node.dispatch("child_add", name)
            else
                CHILDS[name] = nil
                sandbox.node.dispatch("child_remove", name)
            end
        elseif cmd == "content_update" then
            local name, added = ...
            if name == NODE_CODE_FILE then
                if added then
                    kprint("node code updated. reloading...")
                    reload(NODE_CODE_FILE)
                else
                    kprint("node code removed. resetting...")
                    reload()
                end
            else
                if added then
                    CONTENTS[name] = now()
                    sandbox.node.dispatch("content_update", name)
                else
                    CONTENTS[name] = nil
                    sandbox.node.dispatch("content_remove", name)
                end
            end
        elseif cmd == "render_self" then
            local screen_width, screen_height = ...
            if full_scale then
                render_self():draw(0, 0, screen_width, screen_height)
            else
                sandbox.util.draw_correct(
                    render_self(),
                    0, 0, screen_width, screen_height
                )
            end
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
os = {
    getenv = os.getenv;
}
dofile = nil
debug = {
    traceback = debug.traceback;
    getinfo = debug.getinfo;
}

reload()
