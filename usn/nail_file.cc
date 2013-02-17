#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <winioctl.h>
#include <assert.h>

#include <string>
#include "nail_file.h"

#define NAIL_FILE_OBJECT_SIZE(f) (sizeof(nail_file_t) + f->name_len)

char *mem_pool = NULL;
int pool_size = 0;
int next_free_pos = 0;

static int zobie_file_count = 0;
static int zobie_mem = 0;

void *zmalloc(int size) {
  char *p = (char*)malloc(size);
  memset(p, 0, size);
  return p;
}

//return pointer temporary, can not be saved
nail_file_t *malloc_file(int nl) {
  int need_size = sizeof(nail_file_s) + nl+1;
  if (next_free_pos + need_size > pool_size) {
    pool_size += 4*1024*1024;
    mem_pool = (char*)realloc(mem_pool, pool_size);
    memset(mem_pool + next_free_pos, 0, pool_size-next_free_pos);
  }
  assert(next_free_pos <= pool_size - need_size);
  nail_file_t *f = (nail_file_t*)&mem_pool[next_free_pos];
  f->name_len = nl + 1;
  next_free_pos += need_size;
  return f;
}

int file_position(nail_file_t *f) {
  return int((char*)f - mem_pool);
}

nail_file_t* file_object(int pos) {
  if (pos < 0 || pos >= next_free_pos) return NULL;
  nail_file_t *f =  (nail_file_t*)&mem_pool[pos];
  while (f->is_moved) {
    f = (nail_file_t*)&mem_pool[f->new_position];
  }
  return f;
}

void free_file(nail_file_t *f) {
  f->is_deleted = 1;
  zobie_mem += NAIL_FILE_OBJECT_SIZE(f);
  zobie_file_count += 1;
}

void free_drive_file(int di) {
  int p = 0;
  while(p < next_free_pos) {
    nail_file_t *f = (nail_file_t*)&mem_pool[p];
    if (f->is_deleted) goto NEXT;
    if (f->drive_index == di) free_file(f);
NEXT:
    p += sizeof(nail_file_t) + f->name_len;
  }
}

int find_nail_file_position(int drive, DWORDLONG FRN) {
  int p = 0;
  while(p < next_free_pos) {
    nail_file_t *f = (nail_file_t*)&mem_pool[p];
    if (f->is_deleted) goto NEXT;
    if (f->drive_index == drive && f->FRN == FRN) return p;
NEXT:
    p += sizeof(nail_file_t) + f->name_len;
  }
  return -1;
}

static nail_file_t* find_nail_file(const char *name) {
  int p = 0;
  while(p < next_free_pos) {
    nail_file_t *f = (nail_file_t*)&mem_pool[p];
    if (f->is_deleted) goto NEXT;
    if (strcmp(f->filename, name) == 0) return f;
NEXT:
    p += NAIL_FILE_OBJECT_SIZE(f);
  }
  return NULL;
}

nail_file_t *find_first_nail_file(const char *name) {
  int p = 0;
  while(p < next_free_pos) {
    nail_file_t *f = (nail_file_t*)&mem_pool[p];
    if (f->is_deleted) goto NEXT;
    if (f->is_moved) goto NEXT;
    if (strcmp(f->filename, name) == 0) return f;
NEXT:
    p += NAIL_FILE_OBJECT_SIZE(f);
  }
  return NULL;
}
nail_file_t *find_next_nail_file(nail_file_t *last) {
  const char *name = last->filename;
  int p = file_position(last);
  p += NAIL_FILE_OBJECT_SIZE(last);
  while(p < next_free_pos) {
    nail_file_t *f = (nail_file_t*)&mem_pool[p];
    if (f->is_deleted) goto NEXT;
    if (f->is_moved) goto NEXT;
    if (strcmp(f->filename, name) == 0) return f;
NEXT:
    p += NAIL_FILE_OBJECT_SIZE(f);
  }
  return NULL;
}

std::string nail_file_name(nail_file_t *f)  {
  std::string name = f->filename;
  while (f->parent != -1) {
    nail_file_t *parent = file_object(f->parent);
    name = std::string(parent->filename) + "\\" + name;
    f = parent;
  }
  return name;
}

int is_pool_need_compact() {
  if (zobie_mem > 4*1024*1024)
    return 1;
  return 0;
}

static void copy_file(nail_file_t *f, char *new_pool, int *next_pos) {
  assert(!f->is_deleted);
  if (f->is_compacted) return;
  assert (!f->is_moved);
  int new_position = *next_pos;
  nail_file_t *new_file = (nail_file_t*)&new_pool[*next_pos];
  new_file->name_len = f->name_len;
  strcpy(new_file->filename, f->filename);
  new_file->drive_index = f->drive_index;
  new_file->FRN = f->FRN;
  new_file->is_file = f->is_file;
  *next_pos += NAIL_FILE_OBJECT_SIZE(f);
  if (f->parent != -1) {
    nail_file_t *parent = file_object(f->parent);
    copy_file(parent, new_pool, next_pos);
    new_file->parent = parent->new_pool_position;
  } else {
    new_file->parent = -1;
  }
  f->new_pool_position = new_position;
  f->is_compacted = 1;
}

void compact_pool() {
  char *new_pool = (char*)zmalloc(pool_size);
  int next_pos = 0;
  int p = 0;
  while(p < next_free_pos) {
    nail_file_t *f = (nail_file_t*)&mem_pool[p];
    if (f->is_deleted) goto NEXT;
    if (f->is_compacted) goto NEXT;
    if (f->is_moved) goto NEXT;
    copy_file(f, new_pool, &next_pos);
NEXT:
    p += NAIL_FILE_OBJECT_SIZE(f);
  }
  free(mem_pool);
  mem_pool = new_pool;
  next_free_pos = next_pos;
  zobie_mem = 0;
  zobie_file_count = 0;
}

void rename_nail_file(nail_file_t *file, std::string &new_name) {
  if (new_name.length() >= file->name_len) {
    nail_file_t *new_file = malloc_file(new_name.length());
    new_file->drive_index = file->drive_index;
    new_file->FRN = file->FRN;
    new_file->is_file = file->is_file;
    new_file->parent = file->parent;
    strcpy(new_file->filename, new_name.c_str());
    file->is_moved = 1;
    file->new_position = file_position(new_file);
  } else {
    strcpy(file->filename, new_name.c_str());
  }
}


