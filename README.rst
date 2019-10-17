Python Multithreading without GIL
====================================

Overview
-------------------

This is a proof-of-concept implementation of CPython that supports multithreading without the global interpreter lock (GIL). An overview of the  design is described in the `Python Multithreading without GIL <https://docs.google.com/document/d/18CXhDb1ygxg-YXNBJNzfzZsDFosB5e6BfnXLlejd9l0/edit>`__ Google doc.


Installation from source
-------------------

The proof-of-concept works best on Linux x86-64. It also builds on Linux ARM64, Windows (64-bit), and macOS, but you will have to recompile extension modules yourself for these platforms.

The build process has not changed from upstream CPython. See https://devguide.python.org/ for instructions on how to build from source, or follow the steps below.

Install::

    ./configure [--prefix=PREFIX] [--enable-optimizations]
    make -j
    make install
    
The optional ``--prefix=PREFIX`` specifies the destination directory for the Python installation. The optional ``--enable-optimizations`` enables profile guided optimizations (PGO). This slows down the build process, but makes the compiled Python a bit faster.

Docker
-------------------

A pre-built Docker image `colesbury/python-nogil <https://hub.docker.com/r/colesbury/python-nogil>`_ is available on Docker Hub.

Packages
-------------------

Use ``pip install <package>`` as usual to install packages. Please file an issue if you are unable to install a pip package you would like to use.

The proof-of-concept comes with a modified bundled "pip" that includes an `alternative package index <https://d1yxz45j0ypngg.cloudfront.net/>`_. The alternative package index includes C extensions that are either slow to build from source or require some modifications for compatibility.


GIL control
-------------------

The GIL is disabled by default, but if you wish, you can enable it at runtime using the environment variable ``PYTHONGIL=1``. You can check if the GIL is disabled from Python by accessing ``sys.flags.nogil``::

    python3 -c "import sys; print(sys.flags.nogil)"  # True
    PYTHONGIL=1 python3 -c "import sys; print(sys.flags.nogil)"  # False

Example
-------------------

You can use the existing Python APIs, such as the `threading <https://docs.python.org/3/library/threading.html>`_ module and the  `ThreadPoolExecutor <https://docs.python.org/3/library/concurrent.futures.html#concurrent.futures.ThreadPoolExecutor>`_ class.

Here is an example based on Larry Hastings's `Gilectomy benchmark <https://github.com/larryhastings/gilectomy/blob/gilectomy/x.py>`_::

    import sys
    from concurrent.futures import ThreadPoolExecutor

    print(f"nogil={getattr(sys.flags, 'nogil', False)}")

    def fib(n):
        if n < 2: return 1
        return fib(n-1) + fib(n-2)

    threads = 8
    if len(sys.argv) > 1:
        threads = int(sys.argv[1])

    with ThreadPoolExecutor(max_workers=threads) as executor:
        for _ in range(threads):
            executor.submit(lambda: print(fib(34)))

Run it with, e.g.::

    time python3 fib.py 1   # 1 thread, 1x work
    time python3 fib.py 20  # 20 threads, 20x work
    
The program parallelizes well up to the number of available cores. On a 20 core Intel Xeon E5-2698 v4  one thread takes 1.50 seconds and 20 threads take 1.52 seconds [1].

[1] Turbo boost was `disabled <https://askubuntu.com/questions/619875/disabling-intel-turbo-boost-in-ubuntu>`_ to measure the scaling of the program without the effects of CPU frequency scaling. Additionally, you may get more reliable measurements by using `taskset <https://man7.org/linux/man-pages/man1/taskset.1.html>`_ to avoid virtual "hyperthreading" cores.


This is Python version 3.9.7
============================

.. image:: https://travis-ci.org/python/cpython.svg?branch=3.9
   :alt: CPython build status on Travis CI
   :target: https://travis-ci.org/python/cpython

.. image:: https://github.com/python/cpython/workflows/Tests/badge.svg
   :alt: CPython build status on GitHub Actions
   :target: https://github.com/python/cpython/actions

.. image:: https://dev.azure.com/python/cpython/_apis/build/status/Azure%20Pipelines%20CI?branchName=3.9
   :alt: CPython build status on Azure DevOps
   :target: https://dev.azure.com/python/cpython/_build/latest?definitionId=4&branchName=3.9

.. image:: https://codecov.io/gh/python/cpython/branch/3.9/graph/badge.svg
   :alt: CPython code coverage on Codecov
   :target: https://codecov.io/gh/python/cpython

.. image:: https://img.shields.io/badge/discourse-join_chat-brightgreen.svg
   :alt: Python Discourse chat
   :target: https://discuss.python.org/


Copyright (c) 2001-2021 Python Software Foundation.  All rights reserved.

See the end of this file for further copyright and license information.

.. contents::

General Information
-------------------

- Website: https://www.python.org
- Source code: https://github.com/python/cpython
- Issue tracker: https://bugs.python.org
- Documentation: https://docs.python.org
- Developer's Guide: https://devguide.python.org/

Contributing to CPython
-----------------------

For more complete instructions on contributing to CPython development,
see the `Developer Guide`_.

.. _Developer Guide: https://devguide.python.org/

Using Python
------------

Installable Python kits, and information about using Python, are available at
`python.org`_.

.. _python.org: https://www.python.org/

Build Instructions
------------------

On Unix, Linux, BSD, macOS, and Cygwin::

    ./configure
    make
    make test
    sudo make install

This will install Python as ``python3``.

You can pass many options to the configure script; run ``./configure --help``
to find out more.  On macOS case-insensitive file systems and on Cygwin,
the executable is called ``python.exe``; elsewhere it's just ``python``.

Building a complete Python installation requires the use of various
additional third-party libraries, depending on your build platform and
configure options.  Not all standard library modules are buildable or
useable on all platforms.  Refer to the
`Install dependencies <https://devguide.python.org/setup/#install-dependencies>`_
section of the `Developer Guide`_ for current detailed information on
dependencies for various Linux distributions and macOS.

On macOS, there are additional configure and build options related
to macOS framework and universal builds.  Refer to `Mac/README.rst
<https://github.com/python/cpython/blob/3.9/Mac/README.rst>`_.

On Windows, see `PCbuild/readme.txt
<https://github.com/python/cpython/blob/3.9/PCbuild/readme.txt>`_.

If you wish, you can create a subdirectory and invoke configure from there.
For example::

    mkdir debug
    cd debug
    ../configure --with-pydebug
    make
    make test

(This will fail if you *also* built at the top-level directory.  You should do
a ``make clean`` at the top-level first.)

To get an optimized build of Python, ``configure --enable-optimizations``
before you run ``make``.  This sets the default make targets up to enable
Profile Guided Optimization (PGO) and may be used to auto-enable Link Time
Optimization (LTO) on some platforms.  For more details, see the sections
below.

Profile Guided Optimization
^^^^^^^^^^^^^^^^^^^^^^^^^^^

PGO takes advantage of recent versions of the GCC or Clang compilers.  If used,
either via ``configure --enable-optimizations`` or by manually running
``make profile-opt`` regardless of configure flags, the optimized build
process will perform the following steps:

The entire Python directory is cleaned of temporary files that may have
resulted from a previous compilation.

An instrumented version of the interpreter is built, using suitable compiler
flags for each flavor. Note that this is just an intermediary step.  The
binary resulting from this step is not good for real-life workloads as it has
profiling instructions embedded inside.

After the instrumented interpreter is built, the Makefile will run a training
workload.  This is necessary in order to profile the interpreter's execution.
Note also that any output, both stdout and stderr, that may appear at this step
is suppressed.

The final step is to build the actual interpreter, using the information
collected from the instrumented one.  The end result will be a Python binary
that is optimized; suitable for distribution or production installation.


Link Time Optimization
^^^^^^^^^^^^^^^^^^^^^^

Enabled via configure's ``--with-lto`` flag.  LTO takes advantage of the
ability of recent compiler toolchains to optimize across the otherwise
arbitrary ``.o`` file boundary when building final executables or shared
libraries for additional performance gains.


What's New
----------

We have a comprehensive overview of the changes in the `What's New in Python
3.9 <https://docs.python.org/3.9/whatsnew/3.9.html>`_ document.  For a more
detailed change log, read `Misc/NEWS
<https://github.com/python/cpython/blob/3.9/Misc/NEWS.d>`_, but a full
accounting of changes can only be gleaned from the `commit history
<https://github.com/python/cpython/commits/3.9>`_.

If you want to install multiple versions of Python, see the section below
entitled "Installing multiple versions".


Documentation
-------------

`Documentation for Python 3.9 <https://docs.python.org/3.9/>`_ is online,
updated daily.

It can also be downloaded in many formats for faster access.  The documentation
is downloadable in HTML, PDF, and reStructuredText formats; the latter version
is primarily for documentation authors, translators, and people with special
formatting requirements.

For information about building Python's documentation, refer to `Doc/README.rst
<https://github.com/python/cpython/blob/3.9/Doc/README.rst>`_.


Converting From Python 2.x to 3.x
---------------------------------

Significant backward incompatible changes were made for the release of Python
3.0, which may cause programs written for Python 2 to fail when run with Python
3.  For more information about porting your code from Python 2 to Python 3, see
the `Porting HOWTO <https://docs.python.org/3/howto/pyporting.html>`_.


Testing
-------

To test the interpreter, type ``make test`` in the top-level directory.  The
test set produces some output.  You can generally ignore the messages about
skipped tests due to optional features which can't be imported.  If a message
is printed about a failed test or a traceback or core dump is produced,
something is wrong.

By default, tests are prevented from overusing resources like disk space and
memory.  To enable these tests, run ``make testall``.

If any tests fail, you can re-run the failing test(s) in verbose mode.  For
example, if ``test_os`` and ``test_gdb`` failed, you can run::

    make test TESTOPTS="-v test_os test_gdb"

If the failure persists and appears to be a problem with Python rather than
your environment, you can `file a bug report <https://bugs.python.org>`_ and
include relevant output from that command to show the issue.

See `Running & Writing Tests <https://devguide.python.org/runtests/>`_
for more on running tests.

Installing multiple versions
----------------------------

On Unix and Mac systems if you intend to install multiple versions of Python
using the same installation prefix (``--prefix`` argument to the configure
script) you must take care that your primary python executable is not
overwritten by the installation of a different version.  All files and
directories installed using ``make altinstall`` contain the major and minor
version and can thus live side-by-side.  ``make install`` also creates
``${prefix}/bin/python3`` which refers to ``${prefix}/bin/pythonX.Y``.  If you
intend to install multiple versions using the same prefix you must decide which
version (if any) is your "primary" version.  Install that version using ``make
install``.  Install all other versions using ``make altinstall``.

For example, if you want to install Python 2.7, 3.6, and 3.9 with 3.9 being the
primary version, you would execute ``make install`` in your 3.9 build directory
and ``make altinstall`` in the others.


Issue Tracker and Mailing List
------------------------------

Bug reports are welcome!  You can use the `issue tracker
<https://bugs.python.org>`_ to report bugs, and/or submit pull requests `on
GitHub <https://github.com/python/cpython>`_.

You can also follow development discussion on the `python-dev mailing list
<https://mail.python.org/mailman/listinfo/python-dev/>`_.


Proposals for enhancement
-------------------------

If you have a proposal to change Python, you may want to send an email to the
`comp.lang.python`_ or `python-ideas`_ mailing lists for initial feedback.  A
Python Enhancement Proposal (PEP) may be submitted if your idea gains ground.
All current PEPs, as well as guidelines for submitting a new PEP, are listed at
`python.org/dev/peps/ <https://www.python.org/dev/peps/>`_.

.. _python-ideas: https://mail.python.org/mailman/listinfo/python-ideas/
.. _comp.lang.python: https://mail.python.org/mailman/listinfo/python-list


Release Schedule
----------------

See :pep:`596` for Python 3.9 release details.


Copyright and License Information
---------------------------------

Copyright (c) 2001-2021 Python Software Foundation.  All rights reserved.

Copyright (c) 2000 BeOpen.com.  All rights reserved.

Copyright (c) 1995-2001 Corporation for National Research Initiatives.  All
rights reserved.

Copyright (c) 1991-1995 Stichting Mathematisch Centrum.  All rights reserved.

See the file "LICENSE" for information on the history of this software, terms &
conditions for usage, and a DISCLAIMER OF ALL WARRANTIES.

This Python distribution contains *no* GNU General Public License (GPL) code,
so it may be used in proprietary projects.  There are interfaces to some GNU
code but these are entirely optional.

All trademarks referenced herein are property of their respective holders.
