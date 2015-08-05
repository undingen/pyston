#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <cstring>
#include <cassert>
#include "jit-reader.h"

GDB_DECLARE_GPL_COMPATIBLE_READER

struct MyHeader {
   char name[512];
   void* start;
   int size;
};

enum gdb_status my_read_debug_info(struct gdb_reader_funcs *self,
                                   struct gdb_symbol_callbacks *cb,
                                   void *memory, long memory_sz) {
  if (memcmp(memory, "bjit", 4) == 0 && memory_sz == sizeof(MyHeader)) {
    MyHeader* header = (MyHeader*)memory;
    gdb_object* obj = cb->object_open(cb);
    gdb_symtab* sym_tab = cb->symtab_open(cb, obj, NULL);
    cb->block_open(cb, sym_tab, NULL, (GDB_CORE_ADDR)header->start, (GDB_CORE_ADDR)&((char*)(header->start))[header->size], header->name);
    cb->symtab_close(cb, sym_tab);
    cb->object_close(cb, obj);
  }
  return GDB_SUCCESS;
}

enum gdb_status my_unwind_frame(struct gdb_reader_funcs *self,
                                struct gdb_unwind_callbacks *cb) {
  printf("my_unwind_frame\n");
  return GDB_SUCCESS;
}

struct gdb_frame_id my_get_frame_id(struct gdb_reader_funcs *self,
                                    struct gdb_unwind_callbacks *c) {
  printf("my_get_frame_id\n");
  return gdb_frame_id();
}

void my_destroy_reader(struct gdb_reader_funcs *self) {
}

gdb_reader_funcs myreader = {
    GDB_READER_INTERFACE_VERSION,
    NULL,
    my_read_debug_info,
    my_unwind_frame,
    my_get_frame_id,
    my_destroy_reader
};

struct gdb_reader_funcs *gdb_init_reader(void) {
  return &myreader;
}

