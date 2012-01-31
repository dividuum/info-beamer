-----
-- Pretty Printer
-----

function table.show(t, name, indent)
   local cart     -- a container
   local autoref  -- for self references

   -- (RiciLake) returns true if the table is empty
   local function isemptytable(t) return next(t) == nil end

   local function basicSerialize (o)
      local so = tostring(o)
      if type(o) == "function" then
         return so
      elseif type(o) == "number" then
         return so
      elseif o == nil then
          return "nil"
      else
         return string.format("%q", so)
      end
   end

   local function addtocart (value, name, indent, saved, field)
      indent = indent or ""
      saved = saved or {}
      field = field or name

      cart = cart .. indent .. field

      if type(value) ~= "table" then
         cart = cart .. " = " .. basicSerialize(value) .. ";\n"
      else
         if saved[value] then
            cart = cart .. " = {..}; -- " .. saved[value]
                        .. " (self reference)\n"
            autoref = autoref ..  name .. " = " .. saved[value] .. ";\n"
         else
            saved[value] = name
            if isemptytable(value) then
               cart = cart .. " = {};\n"
            else
               cart = cart .. " = {\n"
               for k, v in pairs(value) do
                  k = basicSerialize(k)
                  local fname = string.format("%s[%s]", name, k)
                  field = string.format("[%s]", k)
                  -- three spaces between levels
                  addtocart(v, fname, indent .. "   ", saved, field)
               end
               cart = cart .. indent .. "};\n"
            end
         end
      end
   end

   name = name or "__unnamed__"
   if type(t) ~= "table" then
      return name .. " = " .. basicSerialize(t)
   end
   cart, autoref = "", ""
   addtocart(t, name, indent)
   return cart .. autoref
end

function pp(t)
    print(table.show(t))
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
        getmetatable = getmetatable;
        setmetatable = setmetatable;

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

        gfx = {
            setup = setup;
            render_child = render_child;
            clear = clear;
            load_image = load_image;
            load_font = load_font;
        };

        sys = {
            now = now;
        };

        news = {
            on_content_update = function(name) 
                print("}}} lua: content update " .. name)
            end;

            on_content_remove = function(name)
                print("{{{ lua: content remove " .. name)
            end;

            on_render = function()

            end;

            on_data = function(data)
                print(data)
            end;
        };
    }
    sandbox._G = sandbox
    return sandbox
end

function render_into_screen(screen_width, screen_height)
    image = render_self()
    if not image then
        print("node cannot render itself. gfx.setup called?")
        return
    end
    root_width, root_height = image:dims()

    clear(0.05, 0.05, 0.05, 1)

    local prop_height = root_height * screen_width / root_width
    local prop_width  = root_width * screen_height / root_height
    local x1, y1, x2, y2
    if prop_height > screen_height then
        local x_center = screen_width / 2
        local half_width = prop_width / 2
        x1 = x_center - half_width
        y1 = 0
        x2 = x_center + half_width
        y2 = screen_height
    else
        local y_center = screen_height / 2
        local half_height = prop_height / 2
        x1 = 0
        y1 = y_center - half_height
        x2 = screen_width
        y2 = y_center + half_height
    end

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



-- Globales unsicheres Zeugs zur Sicherheit entfernen.
-- Sollte jemand aus der sandbox ausbrechen, bliebt hier
-- auch nichts gefaehrliches uebrig.
io = nil
require = nil
loadfile = nil
load = nil
package = nil
module = nil
os = nil
dofile = nil
ustring = nil
getfenv = nil
debug = { traceback = debug.traceback }
