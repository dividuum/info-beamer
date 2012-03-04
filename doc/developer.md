Developer Guide
===============

Thanks for your interest in XXX. XXX tries to be a simple framework for
building interactive realtime presentations. Lets keep it that way.

Project Philosophy
------------------

 * Keep it simple. XXX avoids unnecessary clutter. If a simple external 
   Script could solve a problem, there is no need to do it in XXX. For
   example a GIF file can be converted to PNG very easily. There is no need
   to include a gif format reader. If a problem is solvable within Lua,
   where is no need to include a C-version (unless speed prohibits a
   Lua solution).

 * Keep it small. XXX is a simple self-contained binary. It shouldn't
   depend on files somewhere in the filesystem. Keep XXX portable.

 * Keep it robust. XXX tries to provide a crash free environment. It
   shouldn't be possible to crash XXX by using the provided API.

 * Keep it safe. XXX tries to provide a secure environment. Usercode 
   is sandboxed and memory and processing time is limited. It should be
   safe to execute random node code without worrying about malicious
   behaviour.

 * Keep it statefree. It should be possible to develop a node independently
   from other nodes. Composing them shouldn't change their behaviour.
   OpenGL or other state should not leak into or from child nodes.

 * Keep it scriptable. XXX embraces scripting. Not just from the inside but
   also from the outside. It provides several ways to control a running
   instance: Changing files, sending UDP/OSC packets or using a TCP
   connection. Make it easy to script things.

 * Keep it readable. The core XXX code tries to be simple. It should be
   possible to read and hopefully understand the complete sourcecode
   in one evening.

 * Keep it compilable. XXX tries to use libraries that are widely 
   available. It shouldn't be necessary to build obscure dependencies.
   XXX uses a simple GNU Makefile. It shouldn't be necessary to 
   include a buildsystem that creates files larger than all of XXXs
   sourcecode combined.

Contributing
------------

Feel free to fork and enhance XXX. Please judge your changes against the
philosophy. If your contribution conflicts with one of them, I will
probably not merge then into the official version. Let's keep XXX clean and
simple.
