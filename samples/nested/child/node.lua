gl.setup(100, 100)

function node.render()
    if sys.now() % 2 < 1 then
        gl.clear(1,0,0,1)
    else
        gl.clear(0,1,0,1)
    end
end
