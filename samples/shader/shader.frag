uniform sampler2D tex;
uniform float Scale, InvScale;

void main() {
    float dist = 0.5 - min(0.5, sqrt(pow(0.5 - gl_TexCoord[0].s, 2) + pow(0.5 - gl_TexCoord[0].t, 2)));

    float absScale = abs(Scale) / 2 * dist;
    float x = sin(Scale*10) * 0.25 * absScale;
    float y = cos(Scale*10) * 0.26 * absScale;
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
