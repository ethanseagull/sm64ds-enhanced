// Out-of-line copies of inline destructors that the ROM and the vtable fills
// call by their Itanium names (Vector3::~Vector3 and four classes with
// destructors in the class body).
//
// Their src/ files only define those names directly under #ifdef _MSC_VER, and
// the GCC side forces a copy with a call that -O1 inlines away. This file is
// built with -fkeep-inline-functions so GCC emits them as normal weak
// definitions.
#include "types.h"
#include "daObjSwdoor_c.h"
#include "daObjUkiyuka_c.h"
#include "daObjGuragura_c.h"
#include "daObjFloatBoard_c.h"
// destructors the title scene's actors and the HAL's dtor faces reach
#include "daOts_c.h"
#include "daObjKuruma_c.h"
#include "daObjKurumajiku_c.h"
#include "daObjKaitendai_c.h"
#include "daObjDorifu_c.h"
#include "SpikeBomb.h"
#include "daPgMthr_c.h"
#include "daObj_volcanoCannon_c.h"
#include "daMky_c.h"
