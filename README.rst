
fawm
****

fawm is similar to a fawn. Both of them are small and light. But there is single
difference. A fawn is a baby deer, but fawm is an window manager.

Screenshots
===========

.. image:: http://neko-daisuki.ddo.jp/~SumiTomohiko/fawm-thumbnail.png

Features
========

* Popup menu
* Taskbar
* MIT Lisence
* (fawm is not a tiling window manager)

Requirements
============

* FreeBSD 9.1/amd64

Install
=======

fawm needs Python 3.x to compile. You will get ``src/fawm`` with the following
command::

  $ ./configure && make

How to Use
==========

.xinitrc
--------

Write in your ``~/.xinitrc``::

  exec fawm

Popup Menu
----------

Clicking the root window show you the popup menu. The popup menu appears by
clicking the most left box of the taskbar.

You can define items of this menu in ``~/.fawm.conf`` (below).

Wallpaper
---------

fawm cannot set a wallpaper. Please use ``xloadimage -onroot``.

``~/.fawm.conf``
================

fawm reads ``~/.fawm.conf`` at starting to define items of the popup menu.
Syntax of this file likes::

  # A line starting with "#" is a comment.
  menu
    exec "Firefox" "firefox"
    exec "mlterm" "mlterm"
    :
    exec <caption> <command>
    :
    exit
  end

Known Bugs
==========

* Close/maximize/minimize buttons have no image. They are only three dark boxes.
* Maximize button does not work.

Author
======

The author is `Tomohiko Sumi <http://neko-daisuki.ddo.jp/~SumiTomohiko/>`_.

.. vim: tabstop=2 shiftwidth=2 expandtab softtabstop=2 filetype=rst
