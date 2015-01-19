interceptty V0.6
----------------
Copyright 2000-2004 by Scott Gifford <sgifford@suspectclass.com>
This software is licensed under the GNU Public License.  See the file
COPYING included with this distribution for details

interceptty is designed to sit between a serial port (or other
terminal device, or program, or socket, or something connected to a
file descriptor) and a program which is communicating with that
device, recording everything that goes back and forth between the two.
It does this by opening the real device, creating a pseudo-tty, then
forwarding everything between the two, recording whatever it sees.  It
has a number of options that let you fine-tune the devices it uses and
the terminal options for the real device.

See the included manpage for more information.

Compiling
---------

The configuration of interceptty is now managed by autoconf.  That
means that you run

	./configure
	make
	make install

to go from just-unwrapped-the-tarfile to installed-on-your-computer.

For more details on using the configure script, run:

   ./configure --help

or see the INSTALL file included with this distribution.

There is a test script that is useful for making sure everything's
basically worked.  After compiling and before installing, it's a good
idea to run:

    ./test

The script takes about 30 seconds to run, and will run interceptty in
several configurations, making sure they all work as expected.

$Id: README,v 7.4 2004/09/05 23:01:34 gifford Exp $
