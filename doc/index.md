User Documentation
==================

About
-----

XXX is an interactive multimedia presentation framework. It is somewhat
similar to fluxus or processing. XXX allows you to create impressive
realtime visualisations using the Lua programming language.

XXX uses directories as presentable units. A minimal example consists of a
single directory (called a node) containing a font file and a control file
**node.lua**. Let's look at the example code in `samples/hello`:

    gl.setup(1024, 768)

    font = resource.load_font("silkscreen.ttf")

    function node.render()
        font:write(120, 320, "Hello World", 100, 1,1,1,1)
    end

Let's look at each line:

    gl.setup(1024, 768)

This call will initialize a virtual screen of width 1024 and height 768.
The virtual screen is the node's window to the world.

    font = resource.load_font("silkscreen.ttf")

This line will read the Truetype font `silkscreen.ttf` and create a font
object `font`. This object can then be used to write output using the font.

    function node.render()
        font:write(120, 320, "Hello World", 100, 1,1,1,1)
    end

XXX will call the function `node.render` for each frame it will display on
the screen. Inside of `node.render` it's up to you to decide what do show
on each frame. In this example we use the previously create `font` object
to write `"Hello World"` to the virtual screen.

The first two parameters of `write` are the x and y Position on the virtual
screen. `x` is the horizontal position. A value of 0 is the leftmost
position. `y` is the vertical position. The value 0 is the topmost
position. Therefore `x=0, y=0` is in the topleft corner of the virtual
screen.

`"Hello World"` is obviously the value we want to put on the virtual
screen.  `100` is the size of the output in screen units. `1,1,1,1` is
color in RGBA format.

To display this example using XXX, switch into the `samples` directory and
type 

    samples$ ../info-beamer hello

This will start XXX. It will open the directory `hello` (called the `hello`
node) and look for the file `node.lua`. This should get you a new
window showing the "Hello World" text in the center to the screen. 

XXX is a rapid development environment. If you update the code for your
example and save it, XXX will pickup the changes and show them to you
immediatelly. Let's try this: While XXX is still running and displaying the
"Hello World", change the string "Hello World" to "Updated World" in
`node.lua` and save the file `node.lua`. XXX will notice that the file
changed and reload it. The output window will now show the text `"Updated
World"` to you!

Resource loading
----------------

### Images

XXX can load more than just font files. It will load JPG and PNG files
using the `resource.load_image` function. Code using this function might
look like this:

    gl.setup(1024, 768)

    background = resource.load_image("background.jpg")

    function node.render()
        background:draw(0, 0, WIDTH, HEIGHT)
    end

The image `background.jpg` will be loaded into the image object
`background`. Inside `node.render` this background image is the drawn from
coordinates `0, 0` (top left corner of the virtualscreen) to `WIDTH,
HEIGHT` (bottom right corner of the screen). WIDTH and HEIGHT will be
initializes with the values from the `gl.setup` call.

### Videos

You can load videos and display them. Doing so is quite similar to image
loading:

    gl.setup(1024, 768)

    video = resource.load_video("video.mp4")

    function node.render()
        video:next()
        video:draw(0, 0, WIDTH, HEIGHT)
    end

`video` now contains a video object. `video:next` will read the next frame
from video. `video:draw` will then display this frame. You'll notice that
video playback will be too fast. Since `node.render` is called for each
frame XXX wants to display, it's most likely that this function will be
called 60 times per seconds (the refresh rate of your monitor). Likewise
your video might have 25 frames per seconds. So you'll have to slow down
decoding to the actual framerate of the video. `util.videoplayer` will do
all of this for you:

    gl.setup(1024, 768)

    video = util.videoplayer("video.mp4")

    function node.render()
        video:draw(0, 0, WIDTH, HEIGHT)
    end

`util.videoplayer` is a helper function that provides a small wrapper
around video resources. It will automatically decode the video using the
correct framerate.

### Rendering Child Nodes

A directory (called a node) can contain subdirectories. Each subdirectory
is then loaded as a child node. The parent node can render child nodes like
any other resource. Let's look at `samples/nested`:

    gl.setup(1024, 768)

    function node.render()
        gl.clear(1,1,1,1)

        local child = resource.render_child("child")
        child:draw(100, 100, 400, 400)
    end

It creates a new virtual screen sized 1024x768. For each frame it clears
the screen using `gl.clear`. `1,1,1,1` is the color white.

Then is renders the child node `child` (aka the subdirectory `child`) into
a new object called `child` and draws it into the square from `100,100` to
`400,400`. The sourcecode for `child/node.lua` looks like this:

    gl.setup(100, 100)

    function node.render()
        if sys.now() % 2 < 1 then
            gl.clear(1,0,0,1)
        else
            gl.clear(0,1,0,1)
        end
    end

It starts by setting up a 100x100 screen. Each time the child is rendered
(which happens if `resource.render_child` is called), the child clears the
screen either in red or in green, depending in the current time. You can
start the example like this:

    samples$ ../info-beamer nested

You can also start the child on its own:

    samples$ cd nested
    samples/nested$ ../../info-beamer child

This is a great feature: You can develop nodes independently and later
include them in other nodes. 

### Rendering from VNC

XXX contains another useful feature for including content. It can act as a
VNC client. VNC is a cross platform desktop sharing protocol. XXX
implements the client side of this protocol. If you setup a server on a
remote machine, XXX can connect to this machine and create a VNC
object you can use to render the remote desktop in your presentation. Here
is an example:

    gl.setup(1024, 768)

    vnc = resource.create_vnc("192.168.1.1")

    function node.render()
        vnc:draw(0, 0, WIDTH, HEIGHT)
    end

This will try to create a connection to a running VNC server on
`192.168.1.1`. The server must not be password protected (since XXX doesn't
support any kind of authentication). If you create the server, be sure that
you are in a secure environment. Inside `node.render` the content of the
remote desktop is drawn onto the screen.

### Using GLSL Shaders

XXX supports GLSL shaders. Shaders are small programs that run on the GPU.
They enable various realtime effects using the raw power of your GPU.
Shaders come in pairs: A vertex shader and a fragment shader. Vertex
shaders are responsible for transforming 3D positions. Fragment shaders
then calculate the color displayed on each visible pixel of the transformed
object. Fragment shaders can be used to create stunning realtime effects.
XXX enables you to pass numeric values and additional textures into the
shader. This allows you to do all kinds of crazy stuff like for example
blending videos with static textures.

`samples/shader` contains a small basic shader example:

    gl.setup(640, 480)

    util.resource_loader{
        "lua.png",
        "shader.vert",
        "shader.frag",
    }

    function node.render()
        gl.clear(1,1,1,1)
        shader:use{
            Effect = math.cos(sys.now()*2)*3
        }
        lua:draw(120, 40, 520, 440)
    end

`util.resource_loader` is a utility function that makes resource loading
very easy. You just give it a number of filenames. It will then detect
which loader is responsible for the given fileformat and load the file into
a global variable whose name is derived from the filename. The above code
will load the image `lua.png` into the global variable `lua`. It will also
load the shader pair `shader.vert` and `shader.frag` into the global
variable `shader`. The resource loader will also make sure, that changes
files will be reloaded. So if you edit and save for example `shader.frag`,
XXX will instantly reload the shader. You can see changes to your effect
immediatelly. This is great for rapidly development of effects.

Inside of `node.render` we first clear the screen. Then we activate the
shader, which was automatically created from the files `shader.vert` and
`shader.frag` by `util.resource_loader`. We pass in a variable `Effect` which
depends on a time value. This will create a dynamic effect.

Reference
---------

### image = resource.load\_image(filename)

Loads the JPG or PNG file specified by `filename` into a texture objects.

The returned `image` objects supports the following methods:

#### image:draw(x1, y1, x2, y2)

Draws the image into a rectangle specified by the give coordinates.

#### width, height = image:size()

Returns the size of the image.

### video = resource.load\_video(filename)

Loads any supported video file and returns a 
`video` object. The `video` objects supports the following methods:

#### video:draw(x1, y1, x2, y2)

Draws the current video frame into a rectangle specified by the give coordinates.

#### has\_next\_frame = video:next()

Decodes the next frame of the video. Returns true, if a frame was decoded
or false if there was no next frame.

#### width, height = video:size()

Returns the size of the video.

#### fps = video:fps()

Returns the frame per seconds as given by the video file.

### font = resource.load\_font(filename)

Loads the given Truetype font file and returns a `font` objects. It
supports the following methods:

#### width = font:write(x, y, text, size, r, g, b, a)

Writes the provided `text` to the coordinates given in `x` and `y`. The
color is given by `r`, `g`, `b` and `a`, the red, green, blue and alpha
values. The call will return the width of the rendered text in screen
space.

#### width = font:write(x, y, text, size, texturelike)

Mostly identical to font:write but will not use a solid color but the
texturelike object. `texturelike` can be an image, a video or any other
`texturelike` objects. The texture will be used for each character
individually.

### string = resource.load\_file(filename)

Will load the content of the specified file into a string value.
At most 16kb (16384 bytes) are readable.

### shader = resource.create\_shader(vertex\_shader, fragment\_shader)

Will create a new `shader` object. `vertex_shader` and `fragment_shader`
are strings containing the shaders in the GLSL language.

#### shader:use(table\_of\_shader\_variables)

Will activate the shader and pass in the given variables. A call might look
like this:

    shader:use{
        float_value = 123.45,
        some_texture = image,
    }

You can pass in numerical values. Inside the shader, they will be available
as a float uniform values:

    uniform Float float_value;

Textures (which can be images, videos or any other `texturelike` object)
will be available as a 2d sampler:

    uniform sampler2d some_texture;

The shader will be active for the rest of execution of the `node.render`
call. Currently there is now way do deativate an activated shader.

### vnc = resource.create\_vnc(hostname, [port])

This call will create VNC client. It will connect to the specified address.
The remote desktop will be available as a `texturelike` object. If no port
is given, the default value of 5900 will be used.

#### vnc:draw(x1, y1, x2, y2)

Draws the current remote desktop to the screen.

#### width, height = vnc:size()

Returns the size of the remote desktop. Will return 0x0 is the client is
not yet connected to the VNC server.

#### alive = vnc:alive()

Returns a boolean value that indicates if the client is still connected to
the server. If the client was disconnected (which can have various reasons
like an invalid hostname, a closed connection or invalid VNC packets) it
will not automatically reconnect. It's up to you to create a new client if
you need a persistent connection.

### obj = resource.render\_child(name)

Renders a child node into a texture-like object. Rendering will call
`node.render` within the child.

The returned objects supports the same methods like image objects created
by `resource.load_image`.

### gl.setup(width, height)

Initializes the virtual screen of the current node. Will also set the
global variables `WIDTH` and `HEIGHT`.

### gl.clear(r, g, b, a)

Clears the virtual screen using the given color.

### gl.pushMatrix()

Like the native glPushMatrix call, this will save the current ModelView
matrix. This function can only be called inside of `node.render` (or any
functions called from there). To avoid errors, you can only push 20
matrices. This should be more than enough for normal visualisations.

### gl.popMatrix()

Restores a previously saved matrix. Can only be called inside
`node.render`.

### gl.rotate(angle, x, y, z)

Produces a rotation of `angle` degrees around the vector `x`, `y`, `z`. 

### gl.translate(x, y, z)

Produces a translation by `x`, `y`, `z`.

### gl.ortho()

Resets the view to an orthogonal projection. It will create a projection
where the topleft pixel of the virtualscreen is at `0`, `0` and the
bottomright pixel is at `WIDTH`, `HEIGHT`. This is the default mode.

### gl.perspective(fov, eyex, eyey, eyez, centerx, centery, centerz)

This will create a perspective projection. The field of view is given by
`fov`. The camera (or eye) will be at the coordinates `eyex`, `eyey`,
`eyez`. It will look at `centerx`, `centery`, `centerz`. The up vector of
the camera will always be `0`, `-1`, `0`.

### sys.now()

Returns a timestamp as a floating point number that will increment by 1 for
each passing second. The timestamp is relative to the start of XXX.

Node functions
--------------

### node.render()

You should overwrite this function with your own code. This function will
be called by XXX (or by a parent node using `resource.render_child`). It
should create the current scene.

### node.alias(new_alias)

Nodes always have a name and a path. The `name` is the directory name of the
node. The `path` is the full path to the node from the toplevel node.

If you send data to a node using TCP (see the `input` event) or UDP (see
`osc` and `data` events), you address the node using its full path. Using
`node.alias`, you can give your node an alias name. This name must be
unique in a running XXX instance.

Global variables
----------------

### WIDTH

Current width of the virtual screen as set by `gl.setup`.

### HEIGHT

Current height of the virtual screen as set by `gl.setup`.

### NAME

Name of the current node (its directory name)

### PATH

Complete path of the node

Event Listeners
---------------

XXX allows you to listen to various events. All events must be registered
using `node.event`

### node.event(event\_name, event\_handler)

Registers a new `event_handler` function for the event specified by
`event_name`. The following events are available:

#### node.event("child\_add", function(child\_name) ... end)

Registers an event handler that is called if XXX detects that a child node
was added to the current node. The name of the new child node is provided
in `child_name`.

#### node.event("child\_remove", function(child\_name) ... end)

Registers an event handler that is called if XXX detects that a child node
was removed from the current node. The child name is provided in
`child_name`.

#### node.event("content\_update", function(filename) ... end)

Registers an event handler that is called if XXX detects that a file was
created of modified in the current node. This allows you to detect updated
resources. 

#### node.event("content\_remove", function(filename) ... end)

Registers an event handler that is called if XXX detects that a file was
removed from the current node.

#### node.event("data", function(data, suffix) ... end)

Registers a new event handler that will be called if UDP data is sent to
the node. You can send udp data like this:

    $ echo -n "path:data" | netcat -u localhost 4444

Where `path` is the complete path to the node (in case of nested nodes) and
maybe a suffx. `data` is the data you want to send. XXX listens for
incoming UDP packets on port 4444.

XXX will dispatch packets to the node that best matches the specified path.
Let's say you have two nodes:

    nested
    nested/child

If you send a packet to `nested`, the `data` callback will be called in the
node `nested`. If you send a packet to `nested/child/foobar` the `data`
callback will be called in `nested/child`. `suffix` will have the value
`foobar`. See `util.data_mapper` for an easier way to receive udp data
packets. You can give your node a unique alias name using `node.alias`.
This might be useful if you use OSC clients that don't support changing the
paths they create.

#### node.event("osc", function(suffix, ...) ... end)

XXX also supports OSC (open sound control) packets via UDP. If you send an
OSC packet containing a float to `node/slider/1`, the `osc` callback will
be called with the suffix `slider/1` and the decoded osc values. See
`util.osc_mapper` for an easier way to receive osc packets.

#### node.event("input", function(line) ... end)

XXX allows incoming TCP connections to port 4444. You'll be greeted by a
welcome line and are expected to provide a node name. XXX will return `ok!`
if you provide a valid node name. From this moment on, XXX will feed you
the output of the node. This can be used for debugging a node from remote.

Any text you type while connected will trigger the `input` event. The
`input` event will be given the provided line. This can be used to feed a
node with input from outside sources.

Utility functions
-----------------

### util.resource\_loader(table\_of\_filenames)

Creates a magic resource loader that will load the resources from the given
filenames and put them in global variables. Example usage:

    util.resource_loader{
        "font.ttf",
        "image.jpg",
        "video.mp4",
        "shader.vert",
        "shader.frag",
    }

This will load the font `font.ttf` and put the font object into the global
variable `font`. The global variable `image` will contain the image object.
And so on.

The `util.resource_loader` will also detect changes to the files and reload
them.

### util.shaderpair\_loader(basename)

Loads the vertex and fragment shader from two files called `basename.vert`
and `basename.frag` and returns a shader objects.

### util.osc\_mapper(routing\_table)

Will create a OSC mapper that makes if simple to dispatch OSC messages to
different functions. Think of it as Url-routing for OSC messages:

    util.osc_mapper{
        ["slider/(.*)"] = function(slider_num, slider_args)
            ...
        end;
        ["fader/1"] = function(fader_args)
            ...
        end;
    }

The example will allow the node to receive two different type of OSC
messages. If the node is called `example`, the following OSC path will
trigger the slider callback function:
    
    /example/slider/123

`slider_num` will be a string containing the value `123`. `slider_args`
will contain the OSC arguments.

### util.data\_mapper(routing\_table)

Provides the same functionality as the `osc_mapper`, but handles simple UDP
packets.

### video = util.videoplayer(filename, [opt\_table])

Provides a small wrapper around `resource.load_video`. Provides simplified
playback of videos by handling framerate issues. `opt_table` is an optional
table containing the key `loop`. It is a boolean value that indicates if
the videoplayer should loop the video.

`util.videoplayer` will return a `video` object that has the following method:

#### video:draw(x1, y1, x2, y2)

Draws the current video frame into a rectangle specified by the give coordinates.

### util.draw\_correct(obj, x1, y1, x2, y2, alpha)

It is often necessary to display images or videos using the correct aspect
ratio (so they don't look stretched). `util.draw_correct` does this for
you.

    -- maybe wrong, stretches the image
    image:draw(0, 0, WIDTH, HEIGHT)

    -- keeps aspect ratio
    util.draw_correct(image, 0, 0, WIDTH, HEIGHT)
