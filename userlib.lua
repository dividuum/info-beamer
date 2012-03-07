-- See Copyright Notice in LICENSE.txt

util = {}

function util.resource_loader(resources)
    local loaders = {
        png  = resource.load_image;
        jpg  = resource.load_image;
        jpeg = resource.load_image;
        gif  = resource.load_image;
        bmp  = resource.load_image;
        ttf  = resource.load_font;
        avi  = util.videoplayer;
        mpg  = util.videoplayer;
        ogg  = util.videoplayer;
        flv  = util.videoplayer;
        mkv  = util.videoplayer;
        mp4  = util.videoplayer;
        mov  = util.videoplayer;
        frag = util.shaderpair_loader;
        vert = util.shaderpair_loader;
    }
    local resource_handlers = {}
    node.event("content_update", function(name)
        local handler = resource_handlers[name]
        if handler then
            local target, loader = unpack(handler)
            _G[target] = loader(name)
            print("resource_loader: updated _G." .. target .. " (triggered by " .. name .. ")")
        end
    end)
    node.event("content_remove", function(name)
        local handler = resource_handlers[name]
        if handler then
            local target, loader = unpack(handler)
            print("resource_loader: unloaded _G." .. target)
            _G[target] = nil
        end
    end)
    for idx, resource in ipairs(resources) do
        local name, suffix = resource:match("(.*)[.]([^.]+)$")
        if not name then
            error("invalid resource name " .. resource)
        end
        local loader = loaders[suffix]
        if not loader then
            error("no resource loader for suffix " .. suffix)
        end
        resource_handlers[resource] = {
            name, loader
        }
    end
end

function util.file_watch(filename, handler)
    node.event("content_update", function(name)
        if name == filename then
            handler(resource.load_file(name))
        end
    end)
end

function util.shaderpair_loader(any_name)
    local name, suffix = any_name:match("(.*)[.]([^.]+)$")
    return resource.create_shader(
        resource.load_file(name .. ".vert"),
        resource.load_file(name .. ".frag")
    )
end

function util.osc_mapper(routes)
    node.event("osc", function(suffix, ...)
        for pattern, callback in pairs(routes) do
            local match = {suffix:match(pattern)}
            if #match > 0 then
                if match[1] == suffix then
                    return callback(...)
                else
                    return callback(unpack(match), ...)
                end
            end
        end
    end)
end

function util.data_mapper(routes)
    node.event("data", function(data, suffix)
        for pattern, callback in pairs(routes) do
            local match = {suffix:match(pattern)}
            if #match > 0 then
                if match[1] == suffix then
                    return callback(data)
                else
                    return callback(unpack(match), data)
                end
            end
        end
    end)
end

function util.videoplayer(name, opt)
    local stream, start, fps, frame, width, height

    function open_stream()
        stream = resource.load_video(name)
        start = sys.now()
        fps = stream:fps()
        frame = 0
        width, height = stream:size()
    end

    open_stream()

    opt = opt or {}
    local speed = opt.speed or 1
    fps = fps * speed

    local loop = opt.loop or true

    local done = false

    return {
        draw = function(_, x1, y1, x2, y2, alpha)
            if done then return end
            local now = sys.now()
            local target_frame = (now - start) * fps
            if target_frame > frame + 10 then
                print(string.format(
                    "slow player for '%s'. missed %d frames since last call",
                    name,
                    target_frame - frame
                ))
                -- too slow to decode. rebase time
                start = now - frame * 1/fps
            else
                while frame < target_frame do
                    if not stream:next() then
                        if loop then
                            open_stream()
                            break
                        else
                            -- stream completed
                            done = true
                            return false
                        end
                    end
                    frame = frame + 1
                end
            end
            stream:draw(x1, y1, x2, y2, alpha)
            return true
        end;
        texid = function()
            return stream:texid()
        end;
        size = function()
            return stream:size()
        end;
    }
end

function util.running_text(opt)
    local current_idx = 1
    local current_left = 0
    local last = sys.now()

    local texts = opt.texts
    local y = opt.y
    local font = opt.font
    local size = opt.size or 10
    local speed = opt.speed or 10

    return {
        draw = function()
            local now = sys.now()
            local xoff = current_left
            local idx = current_idx
            while true do
                local width = font:write(xoff, y, texts[idx] .. "   -   ", size, 1, 1, 1, 1)
                xoff = xoff + width
                if xoff < 0 then
                    current_left = xoff
                    current_idx = current_idx + 1
                    if current_idx > #texts then
                        current_idx = 1
                    end
                end
                idx = idx + 1
                if idx > #texts then idx = 1 end
                if xoff > WIDTH then
                    break
                end
            end
            local delta = now - last
            last = now
            current_left = current_left - delta * speed
        end
    }
end

function util.scale_into(target_width, target_height, source_width, source_height)
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

function util.draw_correct(obj, x1, y1, x2, y2, ...)
    local ox1, oy1, ox2, oy2 = util.scale_into(
        x2 - x1, y2 - y1, obj:size()
    )
    obj:draw(x1 + ox1, y1 + oy1, x1 + ox2, y1 + oy2, ...)
end


function table.filter(t, predicate)
    local j = 1

    for i, v in ipairs(t) do
        if predicate(v) then
            t[j] = v
            j = j + 1
        end
    end

    while t[j] ~= nil do
        t[j] = nil
        j = j + 1
    end

    return t
end

-- Based on http://lua-users.org/wiki/TableSerialization
-- Modified to *not* use debug.getinfo

--[[
   Author: Julio Manuel Fernandez-Diaz
   Date:   January 12, 2007
   (For Lua 5.1)
   
   Modified slightly by RiciLake to avoid the unnecessary table traversal in tablecount()

   Formats tables with cycles recursively to any depth.
   The output is returned as a string.
   References to other tables are shown as values.
   Self references are indicated.

   The string returned is "Lua code", which can be procesed
   (in the case in which indent is composed by spaces or "--").
   Userdata and function keys and values are shown as strings,
   which logically are exactly not equivalent to the original code.

   This routine can serve for pretty formating tables with
   proper indentations, apart from printing them:

      print(table.show(t, "t"))   -- a typical use
   
   Heavily based on "Saving tables with cycles", PIL2, p. 113.

   Arguments:
      t is the table.
      name is the name of the table (optional)
      indent is a first indentation (optional).
--]]
function table.show(t, name, indent)
   local cart     -- a container
   local autoref  -- for self references

   -- (RiciLake) returns true if the table is empty
   local function isemptytable(t) return next(t) == nil end

   local function basicSerialize (o)
      local so = tostring(o)
      if type(o) == "function" or type(o) == "number" or type(o) == "boolean" then
         return so
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
            cart = cart .. " = {...}; -- " .. saved[value] 
                        .. " (self reference)\n"
            autoref = autoref ..  name .. " = " .. saved[value] .. ";\n"
         else
            saved[value] = name
            --if tablecount(value) == 0 then
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

-- Sandboxed package loader
package = {
    loadlib = function(libname, funcname)
        error("no native linking")
    end;

    seeall = function(module)
        return setmetatable(module, {
            __index = _G
        })
    end;

    loaded = {
        string = string;
        math = math;
        table = table;
        coroutine = coroutine;
        struct = struct;
        util = util;
    };

    loaders = {
        function(modname)
            return function(modname)
                local filename = modname .. ".lua"
                local modulename = PATH .. "/" .. filename
                return loadstring(resource.load_file(filename), "=" .. modulename)()
            end
        end
    };
}

function require(modname)
    local loaded = package.loaded[modname]
    if loaded then
        return loaded
    end

    -- find loader
    local loader
    local errors = {}
    for _, searcher in ipairs(package.loaders) do
        local searcher_val = searcher(modname)
        if type(searcher_val) == "function" then
            loader = searcher_val
            break
        elseif type(searcher_val) == "string" then
            errors[#errors + 1] = searcher_val
        end
    end
    if not loader then
        error(table.concat(errors, "\n"))
    end

    -- load module
    local value = loader(modname)
    if value then
        package.loaded[modname] = value
    elseif not sandbox.package.loaded[modname] then
        package.loaded[modname] = true
    end
    return package.loaded[modname]
end
