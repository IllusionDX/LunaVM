import re

with open('value.c', 'r') as f:
    code = f.read()

code = code.replace('int gc_threshold = 1000;', 'size_t bytes_allocated = 0;\nsize_t next_gc_threshold = 64 * 1024 * 1024;')
code = code.replace('static void init_object(Object *obj, ObjType type) {', 'static void init_object(Object *obj, ObjType type, size_t size) {')

code = code.replace('    obj->is_marked = false;', '    obj->is_marked = false;\n    obj->size = size;\n    bytes_allocated += size;')
code = code.replace('    allocated_objects--;', '    allocated_objects--;\n    bytes_allocated -= obj->size;')

# Constructors
code = code.replace('init_object((Object*)s, OBJ_STRING);', 'init_object((Object*)s, OBJ_STRING, sizeof(ObjString) + length + 1);')
code = code.replace('init_object((Object*)l, OBJ_LIST);', 'init_object((Object*)l, OBJ_LIST, sizeof(ObjList) + (capacity > 4 ? capacity * sizeof(Value) : 0));')
code = code.replace('init_object((Object*)d, OBJ_DICT);', 'init_object((Object*)d, OBJ_DICT, sizeof(ObjDict));')
code = code.replace('init_object((Object*)i, OBJ_INSTANCE);', 'init_object((Object*)i, OBJ_INSTANCE, sizeof(ObjInstance) + sizeof(Value) * field_count + sizeof(ObjFunction*) * method_count);')
code = code.replace('init_object((Object*)f, OBJ_FUNCTION);', 'init_object((Object*)f, OBJ_FUNCTION, sizeof(ObjFunction));')
code = code.replace('init_object((Object*)e, OBJ_EXCEPTION);', 'init_object((Object*)e, OBJ_EXCEPTION, sizeof(ObjException) + length + 1);')
code = code.replace('init_object((Object*)uv, OBJ_UPVALUE);', 'init_object((Object*)uv, OBJ_UPVALUE, sizeof(ObjUpvalue));')
code = code.replace('init_object((Object*)cl, OBJ_CLOSURE);', 'init_object((Object*)cl, OBJ_CLOSURE, sizeof(ObjClosure) + sizeof(ObjUpvalue*) * function->upvalue_count);')

# list_add
code = code.replace('''            list->capacity = old_cap < 8 ? 8 : old_cap * 2;
            list->items = realloc(list->items, sizeof(Value) * list->capacity);''', '''            list->capacity = old_cap < 8 ? 8 : old_cap * 2;
            list->items = realloc(list->items, sizeof(Value) * list->capacity);
            size_t diff = (list->capacity - old_cap) * sizeof(Value);
            ((Object*)list)->size += diff;
            bytes_allocated += diff;''')

# resize_dict
code = code.replace('''    d->entries = new_entries;
    d->indices = new_indices;
    d->capacity = new_cap;''', '''    d->entries = new_entries;
    d->indices = new_indices;
    size_t old_size = d->indices ? d->capacity * (sizeof(int) + sizeof(DictEntry)) : 0;
    size_t new_size = new_cap * (sizeof(int) + sizeof(DictEntry));
    ((Object*)d)->size += (new_size - old_size);
    bytes_allocated += (new_size - old_size);
    d->capacity = new_cap;''')

# resize intern table
code = code.replace('''    intern_table = new_table;
    intern_capacity = new_cap;''', '''    intern_table = new_table;
    bytes_allocated += (new_cap - intern_capacity) * sizeof(ObjString*);
    intern_capacity = new_cap;''')

with open('value.c', 'w') as f:
    f.write(code)
print("Updated value.c")
