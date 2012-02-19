gl.setup(640, 480)

logo = resource.load_image("lua.png")

alert = resource.create_shader(
[[
    void main() {
        // gl_TexCoord[0] = gl_MultiTexCoord0;
        gl_TexCoord[0] = gl_MultiTexCoord0;
        gl_Position = ftransform();
    }
]], [[
    uniform sampler2D tex;
    uniform float Scale, InvScale;

    void main() {
        float absScale = 0.5 - abs(Scale) / 2;
        float x = sin(Scale*90) * 0.02 * absScale;
        float y = cos(Scale*90) * 0.02 * absScale;
        vec4 color = texture2D(tex, gl_TexCoord[0].st + vec4(
            x, y, 0, 0
        ));
        gl_FragColor = vec4(
            color[0] + absScale, 
            color[1] - absScale/2, 
            color[2] - absScale/2, 
            color[3]
        );
    }
]])

function event.render()
    gl.clear(1,1,1,1)
    logo:draw(120, 40, 520, 440)
    alert:use{
        Scale = math.cos(sys.now()*2)
    }
end
