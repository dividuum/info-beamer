gl.setup(1024, 768)

util.resource_loader{
    "beamer.png";
}

function node.render()
    gl.clear(1,1,1,1)
    util.draw_correct(beamer, 0, 0, WIDTH, HEIGHT)
end

