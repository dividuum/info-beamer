-- See Copyright Notice in LICENSE.txt

util = {}

function util.videoplayer(name)
    local stream = resource.load_video(name)
    local start = sys.now()
    local fps = stream:fps()
    local frame = 0
    local width, height = stream:size()

    return {
        play = function(_, x1, y1, x2, y2, alpha)
            local now = sys.now()
            local target_frame = (now - start) * fps
            if target_frame > frame + 10 then
                error(string.format(
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
            local sx1, sy1, sx2, sy2 = sys.scale_into(
                width, height, x2 - x1, y2 - y1
            )
            stream:draw(x1 + sx1, y1 + sy1, x1 + sx2, y1 + sy2, alpha)
            return true
        end
    }
end
