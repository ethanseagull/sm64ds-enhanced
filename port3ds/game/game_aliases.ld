/* Hand aliases for the game link (an implicit linker script, like
 * build3ds/game3ds_aliases.ld that tools/linkfix.py writes). Each is a name
 * the GCC build asks for that the decomp spells on a different symbol. */

/* include/Bully.h declares Render and CleanupResources as Bully's own, so
 * GCC's _ZTV5Bully names them; the ROM's slots 3 and 9 are the base class's
 * bodies (hal/actor_classes_ov064.cpp: daOts_c's). */
PROVIDE(_ZN5Bully6RenderEv = _ZN7daOts_c6RenderEv);
PROVIDE(_ZN5Bully16CleanupResourcesEv = _ZN7daOts_c16CleanupResourcesEv);
