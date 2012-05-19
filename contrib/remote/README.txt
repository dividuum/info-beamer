Remote input example
====================

This directory contains a remote input example.
The lua module remote.lua installs new event sources
for mouse/keyboard input. All inputs will be sent to
info-beamer using OSC.

The client program remote.py uses python, pygame and
pyOSC to send mouse/keyboard input to info-beamer.

You can have multiple clients sending to the same 
info-beamer node. To tell them apart, you can use
prefix them.

node.lua is a minimal example. `install_remote_input`
adds new OSC listeners for the given prefix and
dispatches incoming input events to 5 new event
handlers: keydown, keyup, mousedown, mouseup and
mousemotion.
