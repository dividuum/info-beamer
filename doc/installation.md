Installation Guide
==================

Dependencies
------------

XXX tries to have minimal dependencies. It shouldn't ne necessary to
install obscure packages before compiling XXX. Here are the required 3rd
party packages:

    lua (5.1)           - for scripting
    libevent (>2.0)     - io multiplexing
    glfw                - opengl initialization
    GL & GLU            - opengl & utility functions
    GLEW                - accessing opengl extensions
    ftgl                - truetype font rendering for opengl
    libpng              - reading png files
    libjpeg             - reading jpeg files
    libavformat         - video decoding
    libavcodec
    libavutil
    libswscale
    libz 

### Prerequisites on Ubuntu

Ubuntu provides all required packages. Just execute the following command:

	apt-get install liblua5.1-dev libevent-dev libglfw-dev \
		libglew1.5-dev libftgl-dev libavcodec-dev libswscale-dev \
		libavformat-dev libpng-dev libjpeg-dev


Building From Source
--------------------

XXX is provided as a source release on http://github.com/dividuum/XXX. The
best way to install XXX is to clone the github repository and type `make`
inside the root directory:

    src$ git clone https://github.com/dividuum/XXX.git
    src$ cd XXX
    src/XXX$ make
             [...]
    src/XXX$ ./info-beamer 
    Info Beamer rev-0cbca3 (http://dividuum.de/info-beamer)
    Copyright (c) 2012, Florian Wesch <fw@dividuum.de>

    usage: ./info-beamer <root_name>

Installation
------------

There is nothing special to do. XXX consists of only a single binary called
XXX. You can move it to any directory you like (e.g. /usr/local/bin).
