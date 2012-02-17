-- See Copyright Notice in LICENSE.txt

util = {}

function util.videoplayer(name, opt)
    local stream = resource.load_video(name)
    local start = sys.now()
    local fps = stream:fps()
    local frame = 0
    local width, height = stream:size()

    opt = opt or {}
    local speed = opt.speed or 1
    fps = fps * speed

    return {
        play = function(_, x1, y1, x2, y2, alpha)
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
                        -- stream completed
                        return false
                    end
                    frame = frame + 1
                end
            end
            local sx1, sy1, sx2, sy2 = util.scale_into(
                width, height, x2 - x1, y2 - y1
            )
            stream:draw(x1 + sx1, y1 + sy1, x1 + sx2, y1 + sy2, alpha)
            return true
        end
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

function util.scale_into(source_width, source_height, target_width, target_height)
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

