Knurbs
======

Knurbs is a fork of `OBS Studio <https://github.com/obsproject/obs-studio>`_
for capturing, compositing, encoding, recording and streaming video. It aims
to be the most stable and fastest way to stream and record, with a minimal
set of features that work on every supported platform. It focuses on three
things:

- **Stability**: fix the common pitfalls in how libobs works, and back every
  fix with a regression test. Development is test and coverage driven, so
  bugs are caught early, often before they reach a release.
- **Performance**: fewer copies, fewer locks, less work per frame.
- **Unification across platforms**: the same features, behaving the same
  way, on every supported platform.

Knurbs is not OBS Studio
------------------------

Knurbs is an independent project. It is not OBS Studio, is not made by or
affiliated with the OBS Project, and is not supported by it. Please do not
report Knurbs issues to the OBS Project.

Knurbs is **not compatible** with anything built for OBS Studio: plugins,
scripts, themes, profiles, scene collections and other setups are not
expected to work. The initial release will not load OBS Studio plugins,
because Knurbs' core and API differ too much. A compatibility layer may
follow later, without guarantees (see Direction). Knurbs is under heavy
development and not ready for production use.

Platforms
---------

Knurbs targets Windows, Linux and macOS. The first goal is a fully working
Windows and Linux build; macOS follows after that.

Community
---------

Questions, help and discussion happen on the
`Knurbs Discord server <https://discord.gg/X2cMgYeA9q>`_. Bugs and feature
requests go to the `issue tracker <https://github.com/Xaymar/knurbs/issues>`_.

Knurbs is free. It is funded entirely by backers and sponsors on
`Patreon <https://patreon.com/xaymar/join>`_.

Direction
---------

These are goals, not promises, and none of them has a date:

- **Vulkan-only graphics.** The D3D11 and OpenGL backends are to be replaced
  by a single Vulkan backend, so every platform renders the same way.
- **Wayland only on Linux.** X11 support is retired. Screen capture goes
  through PipeWire; video devices through the PipeWire camera portal and
  V4L2.
- **Unified capture sources.** One source per capture job (display, window,
  video device, ...) with per-platform backends behind it, instead of a
  different source on every platform.
- **A new Qt 6 frontend.** A new application shell built with Qt Widgets,
  developed alongside the old frontend until it reaches parity. Features
  live in plugins wherever possible, so alternatives can be swapped in.
- **A full test suite by default.** Everything is covered by tests, to
  guarantee functionality and stability.
- **A cleaner API.** The C API is to be renamed to a ``knurbs_`` prefix and
  every object made reference counted. Installed headers already live under
  ``<prefix>/include/knurbs/``.
- **Legacy OBS plugins, later.** Plugins built for OBS Studio may run again
  one day through a compatibility layer that isolates them in their own
  process (see `datapath <https://github.com/Xaymar/datapath>`_), rather
  than through source or binary compatibility. There is no guarantee that
  this happens.

Naming
------

Knurbs' own plugins and source types use a ``knurbs`` prefix and descriptive
names. Identifiers such as plugin IDs, source type IDs and setting names are
lowercase and use only ``-`` or ``.`` as separators (no uppercase, no ``_``),
so they never need Shift to type. C identifiers are the only exception.
Where a source replaces an OBS one, the old OBS ID is registered as an alias
and its settings are migrated.

Building
--------

Build instructions for Knurbs have not been written yet. The project uses
CMake; the presets in ``CMakePresets.json`` are the starting point.

Contributing
------------

Issues and pull requests go to
`Xaymar/knurbs <https://github.com/Xaymar/knurbs>`_, against the ``root``
branch. Code style is defined by ``.clang-format``, ``.swift-format`` and
``.gersemirc``; see ``CONTRIBUTING.md``. Bug fixes come in their own commit
with a regression test.

Knurbs is an AI assisted project and welcomes contributions made with AI
tools, as long as they are tested; see ``CONTRIBUTING.md``.

Licence and attribution
-----------------------

Knurbs is based on OBS Studio, copyright the OBS Project and its
contributors (see ``AUTHORS``). Like OBS Studio, it is distributed under the
GNU General Public License v2 (or any later version); see the accompanying
``LICENSE`` file for details. Bundled third-party code under ``deps/`` and
elsewhere keeps its own licence.
