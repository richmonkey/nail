#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NON_CONFORMING_SWPRINTFS

#define WIN32_LEAN_AND_MEAN
#define _WTL_USE_CSTRING

#include <atlbase.h>       // base ATL classes
#include <atlapp.h>        // base WTL classes
extern CAppModule _Module; // WTL version of CComModule
#include <atlwin.h>        // ATL GUI classes
#include <atlframe.h>      // WTL frame window classes
#include <atlmisc.h>       // WTL utility classes like CString
#include <atlcrack.h>      // WTL enhanced msg map macros
#include <atlctrls.h>
#if (ATL_VER < 0x0700)
#undef BEGIN_MSG_MAP
#define BEGIN_MSG_MAP(x) BEGIN_MSG_MAP_EX(x)
#endif

#include <windows.h>
#include <winioctl.h>
#include <tchar.h>
#include <stdio.h>
#include <conio.h>
#include <process.h>   
#include <string>
#include <vector>
#include <map>
#include <assert.h>
#include <iostream>
using namespace std;

#include "nail_file.h"

#define USN_REASON_DATA_CHANGED (USN_REASON_DATA_OVERWRITE|USN_REASON_DATA_EXTEND|USN_REASON_DATA_TRUNCATION)
#define NAIL_PAGE_SIZE (4*1024*1024)
#define NAIL_MAGIC 'nail'
#define MAX_VOLUME_COUNT ('Z'-'A'+1)
#define NAIL_DB_NAME "nail.db"
#define NAIL_DB_VERSION (1<<16|0) //1.0
typedef struct {
  char drive_index;//0-25
  DWORDLONG FRN;
  USN  next_usn;
  DWORDLONG usn_journal_id;
  HANDLE handle;
  HANDLE event;
  OVERLAPPED overlapped;
  char buffer[USN_PAGE_SIZE];
}Volume;

static Volume volumes[MAX_VOLUME_COUNT];
static bool is_db_readed;


static std::string UnicodeToUTF8(std::wstring& str);
static std::wstring UTF8ToUnicode(std::string &str);
static std::wstring GBKToUnicode(std::string &str);
static std::string UnicodeToGBK(std::wstring& str);
static std::string file_extension(std::string& path);
std::wstring format_filetime(FILETIME ft);

std::wstring file_name(USN_RECORD * record) {
  wchar_t szFile[MAX_PATH];
  LPWSTR pszFileName = (LPWSTR) ((PBYTE) record  + record->FileNameOffset);
  int cFileName = record->FileNameLength / sizeof(wchar_t);
  wcsncpy(szFile, pszFileName, cFileName);
  szFile[cFileName] = 0;
  return szFile;
}
void print_reason(DWORD dwReason) {
  TCHAR pszReason[1024] = {0};
  int cchReason = 1024;
  static LPCTSTR szCJReason[] = {
    TEXT("DataOverwrite"),         // 0x00000001
    TEXT("DataExtend"),            // 0x00000002
    TEXT("DataTruncation"),        // 0x00000004
    TEXT("0x00000008"),            // 0x00000008
    TEXT("NamedDataOverwrite"),    // 0x00000010
    TEXT("NamedDataExtend"),       // 0x00000020
    TEXT("NamedDataTruncation"),   // 0x00000040
    TEXT("0x00000080"),            // 0x00000080
    TEXT("FileCreate"),            // 0x00000100
    TEXT("FileDelete"),            // 0x00000200
    TEXT("PropertyChange"),        // 0x00000400
    TEXT("SecurityChange"),        // 0x00000800
    TEXT("RenameOldName"),         // 0x00001000
    TEXT("RenameNewName"),         // 0x00002000
    TEXT("IndexableChange"),       // 0x00004000
    TEXT("BasicInfoChange"),       // 0x00008000
    TEXT("HardLinkChange"),        // 0x00010000
    TEXT("CompressionChange"),     // 0x00020000
    TEXT("EncryptionChange"),      // 0x00040000
    TEXT("ObjectIdChange"),        // 0x00080000
    TEXT("ReparsePointChange"),    // 0x00100000
    TEXT("StreamChange"),          // 0x00200000
    TEXT("0x00400000"),            // 0x00400000
    TEXT("0x00800000"),            // 0x00800000
    TEXT("0x01000000"),            // 0x01000000
    TEXT("0x02000000"),            // 0x02000000
    TEXT("0x04000000"),            // 0x04000000
    TEXT("0x08000000"),            // 0x08000000
    TEXT("0x10000000"),            // 0x10000000
    TEXT("0x20000000"),            // 0x20000000
    TEXT("0x40000000"),            // 0x40000000
    TEXT("*Close*")                // 0x80000000
  };
  TCHAR sz[1024];
  sz[0] = sz[1] = sz[2] = 0;
  for (int i = 0; dwReason != 0; dwReason >>= 1, i++) {
    if ((dwReason & 1) == 1) {
      lstrcat(sz, TEXT(", "));
      lstrcat(sz, szCJReason[i]);
    }
  }
  if (cchReason > lstrlen(&sz[2])) {
    lstrcpy(pszReason, &sz[2]);
  }
}

static DWORDLONG DriveFRN(char drive_letter) {
  wchar_t szVolumePath[10];
  swprintf(szVolumePath, L"%c:\\", (wchar_t)drive_letter);
  HANDLE hDir = CreateFile(szVolumePath, 0, FILE_SHARE_READ|FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  if (INVALID_HANDLE_VALUE == hDir) return 0;
  BY_HANDLE_FILE_INFORMATION fi;
  GetFileInformationByHandle(hDir, &fi);
  CloseHandle(hDir);
  return ((((DWORDLONG) fi.nFileIndexHigh) << 32) | fi.nFileIndexLow);
}

static void OpenDrive(wchar_t *p) {
  wchar_t fileSysBuf[8];
  wchar_t szRootName[40];
  wchar_t szVolumeName[32];
  wchar_t szVolumePath[10];
  DWORD dwMaxComLen,dwFileSysFlag;
  int index = *p - L'A';
  if(DRIVE_FIXED != GetDriveTypeW(p))
    goto NOT_SUPPORTED;

  GetVolumeInformationW(p,szVolumeName,32,NULL,&dwMaxComLen,&dwFileSysFlag,fileSysBuf,8);
  if (wcsncmp(fileSysBuf, L"NTFS", 4) != 0) 
    goto NOT_SUPPORTED;

  swprintf(szRootName,L"%s (%c:)",szVolumeName,*p);
  swprintf(szVolumePath,L"\\\\.\\%c:",*p);
  volumes[index].drive_index = index;
  DWORDLONG FRN = DriveFRN(char(*p));
  if (volumes[index].FRN != 0 && FRN != volumes[index].FRN) {
    //formated or init first time    
    free_drive_file(volumes[index].drive_index);
    volumes[index].usn_journal_id = 0;
  }

  volumes[index].FRN = FRN;
  HANDLE drive = CreateFile(szVolumePath, GENERIC_READ, 
                            FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 
                            NULL, OPEN_ALWAYS, FILE_FLAG_OVERLAPPED|FILE_FLAG_NO_BUFFERING, NULL);
  if (drive == INVALID_HANDLE_VALUE){
    printf("CreateFile: %u\n", GetLastError());
    exit(1);
  }
  volumes[index].handle = drive;
  volumes[index].event = CreateEvent(NULL, FALSE, FALSE, L"");

  nail_file_s *f = malloc_file(2);//"c:", "d:"
  _snprintf(f->filename, 3, "%c:", index+'A');
  f->FRN = volumes[index].FRN;
  f->drive_index = index;
  f->parent = -1;
  if (0 == volumes[index].FRN) exit(1);
  return;

 NOT_SUPPORTED:
  if (volumes[index].drive_index != -1) {
    free_drive_file(volumes[index].drive_index);
    volumes[index].drive_index = -1;
  }
}

static void OpenNTFSVolume(){
  int flag[MAX_VOLUME_COUNT] = {0};
  wchar_t tDrivers[26*4+1] = {};
  GetLogicalDriveStrings(26*4+1,tDrivers);
  for (wchar_t *p=tDrivers;*p!='\0';p+=4) {  
    if(*p>=L'a') 
      *p-=32;
    int index = *p - L'A';
    OpenDrive(p);
    flag[index] = 1;
  }
  for (int i = 0; i < MAX_VOLUME_COUNT; i++) {
    if (!flag[i]) {
      if (volumes[i].drive_index) {
        //deleted
        free_drive_file(volumes[i].drive_index);
        volumes[i].drive_index = -1;
      }
    }
  }
}

static void read_volume(Volume *vol) {
  std::map<DWORDLONG, int> frns;
  std::vector<int> no_parents;
  std::vector<DWORDLONG> no_parent_parents;
  MFT_ENUM_DATA mft_enum_data;
  USN_JOURNAL_DATA * journal;
  PBYTE buffer = (PBYTE)vol->buffer;
  DWORD bytecount;

  frns[vol->FRN] = find_nail_file_position(vol->drive_index, vol->FRN);

  if (!DeviceIoControl(vol->handle, FSCTL_QUERY_USN_JOURNAL,
                       NULL, 0, buffer, USN_PAGE_SIZE, &bytecount, NULL)) {
    DWORD err = GetLastError();
    if (err == ERROR_JOURNAL_NOT_ACTIVE) {
      CREATE_USN_JOURNAL_DATA create_data = {0, 0};
      if (!DeviceIoControl(vol->handle, FSCTL_CREATE_USN_JOURNAL, 
                           &create_data, sizeof(create_data), NULL, NULL, &bytecount, NULL)) {
        printf("FSCTL_CREATE_USN_JOURNAL: %u\n", GetLastError());
        vol->drive_index = -1;
        return;
      }
      if (!DeviceIoControl(vol->handle, FSCTL_QUERY_USN_JOURNAL, 
                           NULL, 0, buffer, USN_PAGE_SIZE, &bytecount, NULL)) {
        printf("FSCTL_QUERY_USN_JOURNAL: %u\n", GetLastError());
        vol->drive_index = -1;
        return;
      }
    }
  }
  
  journal = (USN_JOURNAL_DATA *)buffer;

  if (vol->usn_journal_id == journal->UsnJournalID) {
    printf("drive:%d %I64d %I64d %I64d\n", vol->drive_index, journal->FirstUsn, vol->next_usn, journal->NextUsn - vol->next_usn);
    //run again
    return;
  } else if (vol->usn_journal_id != 0) {
    free_drive_file(vol->drive_index);
  }

  mft_enum_data.StartFileReferenceNumber = 0;
  mft_enum_data.LowUsn = 0;
  mft_enum_data.HighUsn = journal->NextUsn;
  vol->next_usn = journal->NextUsn;
  vol->usn_journal_id = journal->UsnJournalID;
  DWORDLONG usnjournalid = journal->UsnJournalID;
  DWORDLONG nextid;

  //read all file name
  for (;;) {
    BOOL res;
    res = DeviceIoControl(vol->handle, FSCTL_ENUM_USN_DATA, 
                          &mft_enum_data, sizeof(mft_enum_data), buffer, 
                          USN_PAGE_SIZE, &bytecount, NULL);
    if (!res) {
      break;
    }
    nextid = *((DWORDLONG *)buffer);
    USN_RECORD *pUsnRecord = (USN_RECORD *)((USN *)buffer + 1);
    USN_RECORD *recordend = (USN_RECORD *)(((BYTE *)buffer) + bytecount);
    while (pUsnRecord < recordend) {
      std::string name = UnicodeToUTF8(file_name(pUsnRecord));
      nail_file_t *f = malloc_file(name.length());
      strcpy(f->filename, name.c_str());
      f->FRN = pUsnRecord->FileReferenceNumber;
      f->drive_index = vol->drive_index;
      if (pUsnRecord->FileAttributes&FILE_ATTRIBUTE_DIRECTORY)
        f->is_file = 0;
      else
        f->is_file = 1;

      std::map<DWORDLONG, int>::iterator  iter;
      iter = frns.find(pUsnRecord->ParentFileReferenceNumber);
      if (iter != frns.end()) {
        f->parent = iter->second;
      } else {
        f->parent = -1;
        no_parents.push_back(file_position(f));
        no_parent_parents.push_back(pUsnRecord->ParentFileReferenceNumber);
      }
      frns[f->FRN] = file_position(f);
      pUsnRecord = (USN_RECORD *)(((BYTE *)pUsnRecord) + pUsnRecord->RecordLength);
    }
    mft_enum_data.StartFileReferenceNumber = nextid;
  }
  for (unsigned int i = 0; i < no_parents.size(); i++) {
    nail_file_s *f = file_object(no_parents[i]);
    std::map<DWORDLONG, int>::iterator  iter;
    iter = frns.find(no_parent_parents[i]);
    if (iter != frns.end()) {
      f->parent = iter->second;
    } else {
      printf("%s no parent\n", f->filename);
    }
  }
  return;
}

static void start_read_notification(Volume *volume);

static void on_volume_notification(Volume *volume) {
  if (NULL == volume) return;

  DWORD size;
  if (!GetOverlappedResult(volume->handle, &volume->overlapped, &size, FALSE)) {
    printf("error\n");
    return;
  }
  PBYTE buffer = (PBYTE)volume->buffer;
  if(size <= sizeof(USN)) {
    printf("error\n");
    return;
  }

  USN next_usn = * (USN*) buffer;
  PUSN_RECORD pUsnRecord = (PUSN_RECORD) &buffer[sizeof(USN)];

  while ((PBYTE) pUsnRecord < (buffer + size)) { 
    TCHAR desc[1024] = {0};
    if (!(pUsnRecord->Reason&USN_REASON_CLOSE)) {
      goto NEXT;
    }
    if (pUsnRecord->Reason&USN_REASON_FILE_DELETE) {
      int pos = find_nail_file_position(volume->drive_index, pUsnRecord->FileReferenceNumber);
      if (pos < 0) {
        goto NEXT;
      }
      nail_file_s *file = file_object(pos);
      free_file(file);
    } else  if (pUsnRecord->Reason&(USN_REASON_FILE_CREATE|USN_REASON_RENAME_NEW_NAME)) {
      std::string name = UnicodeToUTF8(file_name(pUsnRecord));
      int pos = find_nail_file_position(volume->drive_index, pUsnRecord->FileReferenceNumber);
      nail_file_s *f = NULL;
      if (pos < 0) {
        f = malloc_file(name.length());
        strcpy(f->filename, name.c_str());
        f->drive_index = volume->drive_index;
        f->FRN = pUsnRecord->FileReferenceNumber;
        f->parent = find_nail_file_position(volume->drive_index, pUsnRecord->ParentFileReferenceNumber);
        if (pUsnRecord->FileAttributes&FILE_ATTRIBUTE_DIRECTORY)
          f->is_file = 0;
        else
          f->is_file = 1;
      } else {
        f = file_object(pos);
        if (strcmp(f->filename, "tttt") == 0) {
          printf("old filename:%s new filename:%s\n", nail_file_name(f).c_str(), name.c_str());
        }
        rename_nail_file(f, name);
      }
    } 
  NEXT:
    pUsnRecord = (PUSN_RECORD) ((PBYTE) pUsnRecord + pUsnRecord->RecordLength);
  }
  volume->next_usn = next_usn;
  start_read_notification(volume);
  return ;
}

static void start_read_notification(Volume *volume) {
  BOOL retval = TRUE;
  DWORD cb = 0;

  READ_USN_JOURNAL_DATA rujd;
  ZeroMemory(&rujd, sizeof(rujd));
  rujd.StartUsn = volume->next_usn;	
  rujd.ReasonMask = USN_REASON_CLOSE;
  rujd.UsnJournalID = volume->usn_journal_id;
  rujd.Timeout = 0;
  rujd.BytesToWaitFor = sizeof(USN_RECORD) - 2;
  ZeroMemory(&volume->overlapped, sizeof(volume->overlapped));
  volume->overlapped.hEvent = volume->event;
  retval = DeviceIoControl(volume->handle, FSCTL_READ_USN_JOURNAL, &rujd, sizeof(rujd), 
                           volume->buffer, USN_PAGE_SIZE, &cb, &volume->overlapped);

  if (retval) {
    //impossible?, todo
    //   printf("io success immediate:%d\n", cb);
    //   on_volume_notification(volume, FALSE);
  } else if (retval == 0 && GetLastError() == ERROR_IO_PENDING) {
    return;
  } else {
    printf("error:%d\n", GetLastError());
  }
}

void write_db() {
  int version = NAIL_DB_VERSION;
  int r;
  FILE *f = fopen(NAIL_DB_NAME, "wb");
  int32_t magic = NAIL_MAGIC;
  r = fwrite(&magic, 1, sizeof(int32_t), f);
  if (r != sizeof(int32_t)) exit(1);
  r = fwrite((void*)&version, 1, sizeof(int), f);
  if (r != sizeof(int)) exit(1);
  r = fwrite((void*)volumes, 1, sizeof(volumes), f);
  if (r != sizeof(volumes)) exit(1);
  r = fwrite(mem_pool, 1, next_free_pos, f);
  if (r != next_free_pos) exit(1);
  fclose(f);
}

void read_db() {
  int version = 0;
  int r;
  FILE *f = fopen(NAIL_DB_NAME, "rb");
  if (!f) return;
  int32_t magic = 0;
  r = fread((void*)&magic, 1, sizeof(int32_t), f);
  if (r != sizeof(int32_t)) exit(1);
  if (magic != NAIL_MAGIC) return;
  r = fread((void*)&version, 1, sizeof(int), f);
  if (r != sizeof(int)) exit(1);
  if (version != NAIL_DB_VERSION) return;
  r = fread((void*)volumes, 1, sizeof(volumes), f);
  if (r != sizeof(volumes)) exit(1);
  int b = ftell(f);
  fseek(f, 0, SEEK_END);
  int e = ftell(f);
  fseek(f, b, SEEK_SET);
  pool_size = e - b;
  next_free_pos = e - b;
  pool_size = (next_free_pos + NAIL_PAGE_SIZE)/(NAIL_PAGE_SIZE)*(NAIL_PAGE_SIZE);
  if (mem_pool) free(mem_pool);//malloc in init
  mem_pool = (char*)zmalloc(pool_size);
  r = fread(mem_pool, 1, next_free_pos, f);
  if (r != next_free_pos) exit(1);
  fclose(f);

  for (int i = 0; i < MAX_VOLUME_COUNT; i++) {
    volumes[i].handle = INVALID_HANDLE_VALUE;
  }
}

static unsigned int __stdcall init_db(void* ) {
  read_db();
  OpenNTFSVolume();

  for (int i = 0; i < MAX_VOLUME_COUNT; i++) {
    Volume *vol = volumes + i;
    if (vol->drive_index != -1) {
      read_volume(vol);
    }
  }
  return 0;
}

static void init() {
  mem_pool = (char*)zmalloc(NAIL_PAGE_SIZE);
  pool_size = NAIL_PAGE_SIZE;
  next_free_pos = 0;

  ZeroMemory(volumes, sizeof(volumes));
  for (int i = 0; i < MAX_VOLUME_COUNT; i++) {
    volumes[i].drive_index = -1;
    volumes[i].handle = INVALID_HANDLE_VALUE;
  }
  is_db_readed = false;
}

class CNailWindow : public CFrameWindowImpl<CNailWindow>
{
public:
  DECLARE_FRAME_WND_CLASS(_T("nail main window"), 0);
  BEGIN_MSG_MAP(CNailWindow)
  MSG_WM_CREATE(OnCreate)
  COMMAND_CODE_HANDLER(EN_CHANGE, OnEdit)
  CHAIN_MSG_MAP(CFrameWindowImpl<CNailWindow>)
  END_MSG_MAP()
  private:
  CListViewCtrl m_listview;
  CEdit m_edit;
public:
  LRESULT OnCreate(LPCREATESTRUCT lpcs);
  LRESULT OnEdit(UINT, UINT, HWND hwnd, BOOL);
};

CAppModule _Module;

int main(int argc, char ** argv) {
  init();

  unsigned int threadID;
  HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0, &init_db, NULL, 0, &threadID);

  DWORD nCmdShow = SW_SHOW;
  HMODULE hInstance = GetModuleHandle(NULL);

  _Module.Init(NULL, hInstance);

  CNailWindow wndMain;
  MSG msg;

  if (NULL == wndMain.CreateEx())
    return 1;
  wndMain.SetWindowText(L"Nail");
  wndMain.ShowWindow(nCmdShow);
  wndMain.UpdateWindow();

  while (TRUE) { 
    if (is_pool_need_compact()) {
      compact_pool();
    }

    HANDLE events[MAX_VOLUME_COUNT+1];
    Volume *v[MAX_VOLUME_COUNT];
    int count = 0;
    if (!is_db_readed) {
      events[count] = hThread;
      count++;
    } else {
      for (int i = 0; i < MAX_VOLUME_COUNT; i++) {
        if (volumes[i].drive_index != -1) {
          events[count] = volumes[i].event;
          v[count] = &volumes[i];
          count++;
        }
      }
    }

    DWORD result ; 
    result = MsgWaitForMultipleObjectsEx(count, events, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE); 

    if (result < (WAIT_OBJECT_0 + count)) { 
      if (!is_db_readed) {
        CloseHandle(hThread);
        is_db_readed = true;
        for (int i = 0; i < MAX_VOLUME_COUNT; i++) {
          Volume *vol = volumes + i;
          if (vol->drive_index != -1) {
            start_read_notification(vol);
          }
        }
      } else {
        on_volume_notification(v[result - WAIT_OBJECT_0]);
      }
    } else if(result == (WAIT_OBJECT_0 + count)) { 
      if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) break;
        TranslateMessage ( &msg );
        DispatchMessage(&msg); 
      }
    } else {
      break;
    } 
  }
  _Module.Term();
  if (is_db_readed) {
    write_db();
  }
  return 0;
}


LRESULT CNailWindow::OnCreate(LPCREATESTRUCT lpcs) {
  CRect rc(10,10,400,30);
  CEdit edit;
  edit.Create(m_hWnd, rc, NULL, WS_CHILD|WS_VISIBLE|WS_BORDER);
  m_edit = edit;

  CListViewCtrl listview;
  rc = CRect(10,40,400,230);
  listview.Create(m_hWnd, rc, NULL, WS_CHILD|WS_VISIBLE|WS_BORDER|LVS_REPORT);
  listview.InsertColumn(0, L"Name", LVCFMT_LEFT, 100);
  listview.InsertColumn(1, L"Path", LVCFMT_LEFT, 100);
  listview.InsertColumn(2, L"Size", LVCFMT_LEFT, 100);
  listview.InsertColumn(3, L"Date Modified", LVCFMT_LEFT, 100);
  m_listview = listview;
  return 0;
}

LRESULT CNailWindow::OnEdit(UINT, UINT, HWND hwnd, BOOL) {
  if (!is_db_readed) return 0;
  TCHAR text[1024] = {0};
  m_edit.GetWindowText(text, 1024);
  m_listview.DeleteAllItems();
  if (wcslen(text) == 0) {
    m_listview.UpdateWindow();
    return 0;
  }
  DWORD begin = GetTickCount();
  std::string s = UnicodeToUTF8(std::wstring(text));
  nail_file_t * f = find_first_nail_file(s.c_str());
  while (f) {
    std::wstring r = UTF8ToUnicode(std::string(f->filename));
    std::wstring pathname = UTF8ToUnicode(nail_file_name(f));

    WIN32_FILE_ATTRIBUTE_DATA file_info;
    GetFileAttributesEx(pathname.c_str(), GetFileExInfoStandard, (void*)&file_info);
    wchar_t size_buff[512];
    UINT64 size = file_info.nFileSizeHigh;
    size <<= 32;
    size |= file_info.nFileSizeLow;
    swprintf(size_buff, L"%I64d\n", size);
    std::wstring m = format_filetime(file_info.ftLastWriteTime);
    int index = m_listview.InsertItem(0, r.c_str());
    m_listview.SetItemText(index, 1, pathname.c_str());
    if (f->is_file)
      m_listview.SetItemText(index, 2, size_buff);
    m_listview.SetItemText(index, 3, m.c_str());
    f = find_next_nail_file(f);
  }
  m_listview.UpdateWindow();
  DWORD end = GetTickCount();
  printf("search ended:%d\n", end - begin);
  return 0;
}



static std::string UnicodeToUTF8(std::wstring& str) {
  CHAR buf[256] = {0};
  WideCharToMultiByte(CP_UTF8, 0, str.c_str(), -1, 
                      buf, 256, NULL, NULL);
  return buf;
}

static std::wstring UTF8ToUnicode(std::string &str) {
  wchar_t buff[256] = {0};
  MultiByteToWideChar( CP_UTF8, 0, str.c_str(), -1, buff, 256);
  return buff;
}
static std::wstring GBKToUnicode(std::string &str) {
  wchar_t buff[256] = {0};
  MultiByteToWideChar( CP_ACP, 0, str.c_str(), -1, buff, 256);
  return buff;
}

static std::string UnicodeToGBK(std::wstring& str) {
  CHAR buf[256] = {0};
  WideCharToMultiByte(CP_ACP, 0, str.c_str(), -1, 
                      buf, 256, NULL, NULL);
  return buf;
}

static std::string file_extension(std::string& path) {
  int pos = path.rfind(".");
  if (pos != -1) {
    std::string ext = path.substr(pos + 1, path.length() - pos - 1);
    if (ext.find("\\") != -1) {
      return "";
    }
    return ext;
  }
  return "";
}

std::wstring format_filetime(FILETIME ft) {
  SYSTEMTIME st;
  TCHAR szLocalDate[255] = {0};
  TCHAR szLocalTime[255] = {0};

  FileTimeToLocalFileTime( &ft, &ft );
  FileTimeToSystemTime( &ft, &st );
  GetDateFormat( LOCALE_USER_DEFAULT, 0, &st, L"yyyy-M-d",
                 szLocalDate, 255 );
  GetTimeFormat( LOCALE_USER_DEFAULT, 0, &st, L"H:m", szLocalTime, 255 );
  return std::wstring(szLocalDate) + L" " + szLocalTime;
}

