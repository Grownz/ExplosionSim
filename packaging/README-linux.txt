Explosion Simulator - Linux
===========================

This archive is self-contained. Run it from this directory:

    ./ExplosionSim

The renderer needs a Vulkan 1.2 capable driver. The assets (shaders are
embedded, but the splash frames and scene models are loaded from ./assets)
must stay next to the executable.

Command-line examples
---------------------

Headless screenshot:

    ./ExplosionSim --headless --width 1600 --height 900 \
        --frames 1 --time 0.12 --screenshot capture.png

High-fidelity Euler solver capture:

    ./ExplosionSim --high-fidelity --solver-resolution 32 \
        --no-solver-auto-scale --headless --width 320 --height 180 \
        --frames 1 --time 0.01 --charge 0.2 --distance-scale 0.55 \
        --screenshot hf.png

Solver benchmark with GPU timings:

    ./ExplosionSim --solver-benchmark --solver-resolution 32 \
        --no-solver-auto-scale --solver-step-count 64 --solver-steps 16

Settings are read from ./settings.ini when present, otherwise from
$XDG_CONFIG_HOME/explosionsim/settings.ini (or ~/.config/explosionsim).
Use --settings <path> to load a specific profile and --write-settings <path>
to dump the active profile.

The desktop entry (explosionsim.desktop) and icon (explosionsim.png) are
provided for system integration. Copy them to the appropriate XDG directories
and adjust Exec if the binary is installed elsewhere.

Updates: Linux v1 uses the GitHub release archive; there is no in-app
self-updater. Replace this directory with the new release to update.
