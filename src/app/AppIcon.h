#pragma once

// Resource id of the application icon that cmake/ExplosionSim.rc.in compiles
// into the executable. Windows uses the lowest icon id for the shell icon of
// the binary, so this stays 1; the window class loads the same resource.
#define EXPLOSION_APP_ICON_ID 1
