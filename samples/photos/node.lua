gl.setup(1024, 768)

PictureSource = function()
    local queue = {}

    node.event("content_remove", function(name)
        queue[name] = nil
    end)
    
    return {
        get_next_image = function()
            local next_image = next(queue)
            if not next_image then
                for name, date in pairs(CONTENTS) do
                    queue[name] = 1
                end
                next_image = next(queue)
            end
            queue[next_image] = nil
            return next_image
        end
    }
end

local effect = 0
local ps = PictureSource()

COUNTDOWN = 10

local current_image = resource.load_image(ps.get_next_image())
local next_image
local next_image_time = sys.now() + COUNTDOWN
math.randomseed(sys.now())

function node.render()
    gl.clear(0,0,0,1)
    gl.perspective(60,
       WIDTH/2, HEIGHT/2, -WIDTH/1.6,
       -- WIDTH/2, HEIGHT/2, -WIDTH/1.4,
       WIDTH/2, HEIGHT/2, 0
    )
    -- gl.perspective(60,
    --    WIDTH/2+math.cos(sys.now()) * 100, HEIGHT/2+math.sin(sys.now()) * 100, -WIDTH/1.9,
    --    -- WIDTH/2, HEIGHT/2, -WIDTH/1.4,
    --    WIDTH/2, HEIGHT/2, 0
    -- )
    local time_to_next = next_image_time - sys.now()
    if time_to_next < 0 then
        current_image = next_image
        next_image = nil
        next_image_time = sys.now() + COUNTDOWN
        util.draw_correct(current_image, 0,0,WIDTH,HEIGHT)
        effect = effect + math.floor(math.random() * 10)
    elseif time_to_next < 1 then
        local xoff = (1 - time_to_next) * WIDTH

        gl.pushMatrix()
            local current_effect = effect % 3
            if current_effect < 0.5 then
                gl.rotate(200 * (1-time_to_next), 0,1,0)
                util.draw_correct(current_image, 0 + xoff, 0, WIDTH + xoff, HEIGHT, time_to_next)
            elseif current_effect < 1.5 then
                gl.rotate(60 * (1-time_to_next), 0,0,1)
                util.draw_correct(current_image, 0 + xoff, 0, WIDTH + xoff, HEIGHT, time_to_next)
            else
                gl.rotate(300 * (1-time_to_next), -1,0.2,0.4)
                util.draw_correct(current_image, 0 + xoff, 0, WIDTH + xoff, HEIGHT, time_to_next)
            end
        gl.popMatrix()

        gl.pushMatrix()
            current_effect = effect % 3
            xoff = time_to_next * -WIDTH
            if current_effect < 0.5 then
                gl.rotate(100 * (time_to_next), 1,-1,0)
                util.draw_correct(next_image, 0 + xoff, 0,WIDTH + xoff, HEIGHT, 1-time_to_next)
            elseif current_effect < 1.5 then 
                gl.rotate(100 * (time_to_next), 0,0,-1)
                util.draw_correct(next_image, 0 + xoff, 0,WIDTH + xoff, HEIGHT, 1-time_to_next)
            else
                local half_width = WIDTH/2
                local half_height = HEIGHT/2
                local percent = 1 - time_to_next
                gl.translate(half_width, half_height)
                gl.rotate(100 * time_to_next, 0,0,-1)
                gl.translate(-half_width, -half_height)
                util.draw_correct(next_image,
                    half_width - half_width*percent, half_height - half_height*percent, 
                    half_width + half_width*percent, half_height + half_height*percent, 
                    1-time_to_next
                )
            end
        gl.popMatrix()
    elseif time_to_next < 2 then
        if not next_image then
            next_image = resource.load_image(
                ps.get_next_image()
            )
        end
        util.draw_correct(current_image, 0,0,WIDTH,HEIGHT)
    else
        util.draw_correct(current_image, 0,0,WIDTH,HEIGHT)
    end
end
