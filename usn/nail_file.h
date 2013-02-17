#ifndef NAIL_FILE_H
#define NAIL_FILE_H

typedef __int32 int32_t;
typedef unsigned __int32 uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;

#pragma warning(disable:4200)

#pragma pack(push,1) 
typedef struct nail_file_s {
  DWORDLONG FRN;
  union{
    int parent;
    int new_position;
    int new_pool_position;
  };
  unsigned short is_deleted:1;
  unsigned short is_moved:1;//1:new_position 0:parent
  unsigned short is_compacted:1;//1:new_pool_position 0:other
  unsigned short is_file:1;
  unsigned short drive_index:5;//0~25
  unsigned char name_len;//readonly
  char filename[0];
}nail_file_t;
#pragma pack(pop)

extern char *mem_pool;
extern int pool_size;
extern int next_free_pos;

void *zmalloc(int size) ;
//return pointer temporary, can not be saved
nail_file_t *malloc_file(int nl) ;
int file_position(nail_file_t *f) ;
nail_file_t* file_object(int pos) ;

void free_file(nail_file_t *f);
void free_drive_file(int di) ;

int find_nail_file_position(int drive, DWORDLONG FRN) ;
nail_file_t *find_first_nail_file(const char *name) ;
nail_file_t *find_next_nail_file(nail_file_t *last) ;

std::string nail_file_name(nail_file_t *f);
void rename_nail_file(nail_file_t *file, std::string &new_name);

int is_pool_need_compact() ;
void compact_pool() ;

#endif
