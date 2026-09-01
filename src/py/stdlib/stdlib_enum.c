/* stdlib_enum.c — Python-style Enum support for Luna Python subset.
 *
 * Usage:
 *   class Color(Enum):
 *       RED = auto()    -> Color.RED.name  == "RED", .value == 0
 *       GREEN = 5       -> Color.GREEN.name  == "GREEN", .value == 5
 *       BLUE = auto()   -> Color.BLUE.name   == "BLUE", .value == 6
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vm.h"
#include "value.h"
#include "py/object.h"
#include "py/frontend_state.h"

/* Sequential counter used by auto(). Reset per Enum subclass at compile time. */
static int auto_counter = 0;

Value enum_get_auto_value(void) {
    return make_int(auto_counter++);
}

void enum_reset_auto(void) {
    auto_counter = 0;
}

void enum_register_members(struct ObjClass *cls, const char **names,
                           Value *values, int count) {
    if (!cls || !names || !values || count <= 0) return;
    if (!cls->fields) cls->fields = new_dict();
    for (int i = 0; i < count; i++) {
        dict_set(cls->fields,
                 make_obj((Object*)new_string(names[i], (int)strlen(names[i]))),
                 values[i]);
    }
}

void vm_register_enum_module(VM *vm) {
    ObjClass *enum_class = new_class("Enum", NULL);
    vm_set_global(vm, "Enum", make_obj((Object*)enum_class), false);
}
