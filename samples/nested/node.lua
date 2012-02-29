gl.setup(1024, 768)

function node.render()
    gl.clear(1,1,1,1)

    local child = resource.render_child("child")
    child:draw(100, 100, 400, 400)
end
