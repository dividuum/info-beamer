gl.setup(320, 200)

remote = require "remote"
remote.install_remote_input("player1")
remote.install_remote_input("player2")

node.event("keydown", function(...)
    print(...)
end)

node.event("mousedown", function(...)
    print(...)
end)

local mice = {}
node.event("mousemotion", function(x, y, source)
    mice[source] = { x = x, y = y }
end)

util.auto_loader(_G)

function node.render()
    gl.clear(1,1,1,1)
    for player, pos in pairs(mice) do
        mouse:draw(pos.x, pos.y, pos.x+12, pos.y+21, 1.0)
    end
end
