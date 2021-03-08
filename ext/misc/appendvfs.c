/*
** 2017-10-20
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
******************************************************************************
**
** This file implements a VFS shim that allows an SQLite database to be
** appended onto the end of some other file, such as an executable.
**
** A special record must appear at the end of the file that identifies the
** file as an appended database and provides the offset to the first page
** of the exposed content. (Or, it is the length of the content prefix.)
** For best performance page 1 should be located at a disk page boundary,
** though that is not required.
**
** When opening a database using this VFS, the connection might treat
** the file as an ordinary SQLite database, or it might treat it as a
** database appended onto some other file.  The decision is made by
** applying the following rules in order:
**
**  (1)  An empty file is an ordinary database.
**
**  (2)  If the file ends with the appendvfs trailer string
**       "Start-Of-SQLite3-NNNNNNNN" that file is an appended database.
**
**  (3)  If the file begins with the standard SQLite prefix string
**       "SQLite format 3", that file is an ordinary database.
**
**  (4)  If none of the above apply and the SQLITE_OPEN_CREATE flag is
**       set, then a new database is appended to the already existing file.
**
**  (5)  Otherwise, SQLITE_CANTOPEN is returned.
**
** To avoid unnecessary complications with the PENDING_BYTE, the size of
** the file containing the database is limited to 1GB. (1000013824 bytes)
** This VFS will not read or write past the 1GB mark.  This restriction
** might be lifted in future versions.  For now, if you need a larger
** database, then keep it in a separate file.
**
** If the file being opened is a plain database (not an appended one), then
** this shim is a pass-through into the default underlying VFS. (rule 3)
**/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <string.h>
#include <assert.h>

/* The append mark at the end of the database is:
**
**     Start-Of-SQLite3-NNNNNNNN
**     123456789 123456789 12345
**
** The NNNNNNNN represents a 64-bit big-endian unsigned integer which is
** the offset to page 1, and also the length of the prefix content.
*/
#define APND_MARK_PREFIX     "Start-Of-SQLite3-"
#define APND_MARK_PREFIX_SZ  17
#define APND_MARK_FOS_SZ      8
#define APND_MARK_SIZE       (APND_MARK_PREFIX_SZ+APND_MARK_FOS_SZ)

/*
** Maximum size of the combined prefix + database + append-mark.  This
** must be less than 0x40000000 to avoid locking issues on Windows.
*/
#define APND_MAX_SIZE  (65536*15259)

/*
** Size of storage page upon which to align appendvfs portion.
*/
#ifndef APND_ROUNDUP_BITS
#define APND_ROUNDUP_BITS 12
#endif

/*
** Forward declaration of objects used by this utility
*/
typedef struct sqlite3_vfs ApndVfs;
typedef struct ApndFile ApndFile;

/* Access to a lower-level VFS that (might) implement dynamic loading,
** access to randomness, etc.
*/
#define ORIGVFS(p)  ((sqlite3_vfs*)((p)->pAppData))
#define ORIGFILE(p) ((sqlite3_file*)(((ApndFile*)(p))+1))

/* Invariants for an open appendvfs file:
 * Once an appendvfs file is opened, it will be in one of three states:
 * State 0: Never written. Underlying file (if any) is unaltered.
 * State 1: Append mark is persisted, content write is in progress.
 * State 2: Append mark is persisted, content writes are complete.
 * 
 * State 0 is persistent in the sense that nothing will have been done
 * to the underlying file, including any attempt to convert it to an
 * appendvfs file.
 *
 * State 1 is normally transitory. However, if a write operation ends
 * abnormally (disk full, power loss, process kill, etc.), then State 1
 * may be persistent on disk with an incomplete content write-out. This
 * is logically equivalent to an interrupted write to an ordinary file,
 * where some unknown portion of to-be-written data is persisted while
 * the remainder is not. Database integrity in such cases is maintained
 * (or not) by the same measures available for ordinary file access.
 *
 * State 2 is persistent under normal circumstances (when there is no
 * abnormal termination of a write operation such that data provided
 * to the underlying VFS write method has not yet reached storage.)
 *
 * In order to maintain the state invariant, the append mark is written
 * in advance of content writes where any part of such content would
 * overwrite an existing (or yet to be written) append mark.
 */
struct ApndFile {
  /* Access to IO methods of the underlying file */
  sqlite3_file base;
  /* File offset to beginning of appended content (unchanging) */
  sqlite3_int64 iPgOne;
  /* File offset of written append-mark, or -1 if unwritten */
  sqlite3_int64 iMark;
};

/*
** Methods for ApndFile
*/
static int apndClose(sqlite3_file*);
static int apndRead(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
static int apndWrite(sqlite3_file*,const void*,int iAmt, sqlite3_int64 iOfst);
static int apndTruncate(sqlite3_file*, sqlite3_int64 size);
static int apndSync(sqlite3_file*, int flags);
static int apndFileSize(sqlite3_file*, sqlite3_int64 *pSize);
static int apndLock(sqlite3_file*, int);
static int apndUnlock(sqlite3_file*, int);
static int apndCheckReservedLock(sqlite3_file*, int *pResOut);
static int apndFileControl(sqlite3_file*, int op, void *pArg);
static int apndSectorSize(sqlite3_file*);
static int apndDeviceCharacteristics(sqlite3_file*);
static int apndShmMap(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
static int apndShmLock(sqlite3_file*, int offset, int n, int flags);
static void apndShmBarrier(sqlite3_file*);
static int apndShmUnmap(sqlite3_file*, int deleteFlag);
static int apndFetch(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
static int apndUnfetch(sqlite3_file*, sqlite3_int64 iOfst, void *p);

/*
** Methods for ApndVfs
*/
static int apndOpen(sqlite3_vfs*, const char *, sqlite3_file*, int , int *);
static int apndDelete(sqlite3_vfs*, const char *zName, int syncDir);
static int apndAccess(sqlite3_vfs*, const char *zName, int flags, int *);
static int apndFullPathname(sqlite3_vfs*, const char *zName, int, char *zOut);
static void *apndDlOpen(sqlite3_vfs*, const char *zFilename);
static void apndDlError(sqlite3_vfs*, int nByte, char *zErrMsg);
static void (*apndDlSym(sqlite3_vfs *pVfs, void *p, const char*zSym))(void);
static void apndDlClose(sqlite3_vfs*, void*);
static int apndRandomness(sqlite3_vfs*, int nByte, char *zOut);
static int apndSleep(sqlite3_vfs*, int microseconds);
static int apndCurrentTime(sqlite3_vfs*, double*);
static int apndGetLastError(sqlite3_vfs*, int, char *);
static int apndCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);
static int apndSetSystemCall(sqlite3_vfs*, const char*,sqlite3_syscall_ptr);
static sqlite3_syscall_ptr apndGetSystemCall(sqlite3_vfs*, const char *z);
static const char *apndNextSystemCall(sqlite3_vfs*, const char *zName);

static sqlite3_vfs apnd_vfs = {
  3,                            /* iVersion (set when registered) */
  0,                            /* szOsFile (set when registered) */
  1024,                         /* mxPathname */
  0,                            /* pNext */
  "apndvfs",                    /* zName */
  0,                            /* pAppData (set when registered) */ 
  apndOpen,                     /* xOpen */
  apndDelete,                   /* xDelete */
  apndAccess,                   /* xAccess */
  apndFullPathname,             /* xFullPathname */
  apndDlOpen,                   /* xDlOpen */
  apndDlError,                  /* xDlError */
  apndDlSym,                    /* xDlSym */
  apndDlClose,                  /* xDlClose */
  apndRandomness,               /* xRandomness */
  apndSleep,                    /* xSleep */
  apndCurrentTime,              /* xCurrentTime */
  apndGetLastError,             /* xGetLastError */
  apndCurrentTimeInt64,         /* xCurrentTimeInt64 */
  apndSetSystemCall,            /* xSetSystemCall */
  apndGetSystemCall,            /* xGetSystemCall */
  apndNextSystemCall            /* xNextSystemCall */
};

static const sqlite3_io_methods apnd_io_methods = {
  3,                              /* iVersion */
  apndClose,                      /* xClose */
  apndRead,                       /* xRead */
  apndWrite,                      /* xWrite */
  apndTruncate,                   /* xTruncate */
  apndSync,                       /* xSync */
  apndFileSize,                   /* xFileSize */
  apndLock,                       /* xLock */
  apndUnlock,                     /* xUnlock */
  apndCheckReservedLock,          /* xCheckReservedLock */
  apndFileControl,                /* xFileControl */
  apndSectorSize,                 /* xSectorSize */
  apndDeviceCharacteristics,      /* xDeviceCharacteristics */
  apndShmMap,                     /* xShmMap */
  apndShmLock,                    /* xShmLock */
  apndShmBarrier,                 /* xShmBarrier */
  apndShmUnmap,                   /* xShmUnmap */
  apndFetch,                      /* xFetch */
  apndUnfetch                     /* xUnfetch */
};

/*
** Close an apnd-file.
*/
static int apndClose(sqlite3_file *pFile){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xClose(pFile);
}

/*
** Read data from an apnd-file.
*/
static int apndRead(
  sqlite3_file *pFile, 
  void *zBuf, 
  int iAmt, 
  sqlite_int64 iOfst
){
  ApndFile *paf = (ApndFile *)pFile;
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xRead(pFile, zBuf, iAmt, paf->iPgOne+iOfst);
}

/*
** Add the append-mark onto what should become the end of the file.
*  If and only if this succeeds, internal ApndFile.iMark is updated.
*  Parameter iWriteEnd is the appendvfs-relative offset of the new mark.
*/
static int apndWriteMark(
  ApndFile *paf,
  sqlite3_file *pFile,
  sqlite_int64 iWriteEnd
){
  sqlite_int64 iPgOne = paf->iPgOne;
  unsigned char a[APND_MARK_SIZE];
  int i = APND_MARK_FOS_SZ;
  int rc;
  assert(pFile == ORIGFILE(paf));
  memcpy(a, APND_MARK_PREFIX, APND_MARK_PREFIX_SZ);
  while (--i >= 0) {
    a[APND_MARK_PREFIX_SZ+i] = (unsigned char)(iPgOne & 0xff);
    iPgOne >>= 8;
  }
  iWriteEnd += paf->iPgOne;
  if( SQLITE_OK==(rc = pFile->pMethods->xWrite
                  (pFile, a, APND_MARK_SIZE, iWriteEnd)) ){
    paf->iMark = iWriteEnd;
  }
  return rc;
}

/*
** Write data to an apnd-file.
*/
static int apndWrite(
  sqlite3_file *pFile,
  const void *zBuf,
  int iAmt,
  sqlite_int64 iOfst
){
  ApndFile *paf = (ApndFile *)pFile;
  sqlite_int64 iWriteEnd = iOfst + iAmt;
  if( iWriteEnd>=APND_MAX_SIZE ) return SQLITE_FULL;
  pFile = ORIGFILE(pFile);
  /* If append-mark is absent or will be overwritten, write it. */
  if( paf->iMark < 0 || paf->iPgOne + iWriteEnd > paf->iMark ){
    int rc = apndWriteMark(paf, pFile, iWriteEnd);
    if( SQLITE_OK!=rc )
      return rc;
  }
  return pFile->pMethods->xWrite(pFile, zBuf, iAmt, paf->iPgOne+iOfst);
}

/*
** Truncate an apnd-file.
*/
static int apndTruncate(sqlite3_file *pFile, sqlite_int64 size){
  ApndFile *paf = (ApndFile *)pFile;
  pFile = ORIGFILE(pFile);
  /* The append mark goes out first so truncate failure does not lose it. */
  if( SQLITE_OK!=apndWriteMark(paf, pFile, size) )
    return SQLITE_IOERR;
  /* Truncate underlying file just past append mark */
  return pFile->pMethods->xTruncate(pFile, paf->iMark+APND_MARK_SIZE);
}

/*
** Sync an apnd-file.
*/
static int apndSync(sqlite3_file *pFile, int flags){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xSync(pFile, flags);
}

/*
** Return the current file-size of an apnd-file.
** If the append mark is not yet there, the file-size is 0.
*/
static int apndFileSize(sqlite3_file *pFile, sqlite_int64 *pSize){
  ApndFile *paf = (ApndFile *)pFile;
  *pSize = ( paf->iMark >= 0 )? (paf->iMark - paf->iPgOne) : 0;
  return SQLITE_OK;
}

/*
** Lock an apnd-file.
*/
static int apndLock(sqlite3_file *pFile, int eLock){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xLock(pFile, eLock);
}

/*
** Unlock an apnd-file.
*/
static int apndUnlock(sqlite3_file *pFile, int eLock){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xUnlock(pFile, eLock);
}

/*
** Check if another file-handle holds a RESERVED lock on an apnd-file.
*/
static int apndCheckReservedLock(sqlite3_file *pFile, int *pResOut){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xCheckReservedLock(pFile, pResOut);
}

/*
** File control method. For custom operations on an apnd-file.
*/
static int apndFileControl(sqlite3_file *pFile, int op, void *pArg){
  ApndFile *paf = (ApndFile *)pFile;
  int rc;
  pFile = ORIGFILE(pFile);
  rc = pFile->pMethods->xFileControl(pFile, op, pArg);
  if( rc==SQLITE_OK && op==SQLITE_FCNTL_VFSNAME ){
    *(char**)pArg = sqlite3_mprintf("apnd(%lld)/%z", paf->iPgOne,*(char**)pArg);
  }
  return rc;
}

/*
** Return the sector-size in bytes for an apnd-file.
*/
static int apndSectorSize(sqlite3_file *pFile){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xSectorSize(pFile);
}

/*
** Return the device characteristic flags supported by an apnd-file.
*/
static int apndDeviceCharacteristics(sqlite3_file *pFile){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xDeviceCharacteristics(pFile);
}

/* Create a shared memory file mapping */
static int apndShmMap(
  sqlite3_file *pFile,
  int iPg,
  int pgsz,
  int bExtend,
  void volatile **pp
){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xShmMap(pFile,iPg,pgsz,bExtend,pp);
}

/* Perform locking on a shared-memory segment */
static int apndShmLock(sqlite3_file *pFile, int offset, int n, int flags){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xShmLock(pFile,offset,n,flags);
}

/* Memory barrier operation on shared memory */
static void apndShmBarrier(sqlite3_file *pFile){
  pFile = ORIGFILE(pFile);
  pFile->pMethods->xShmBarrier(pFile);
}

/* Unmap a shared memory segment */
static int apndShmUnmap(sqlite3_file *pFile, int deleteFlag){
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xShmUnmap(pFile,deleteFlag);
}

/* Fetch a page of a memory-mapped file */
static int apndFetch(
  sqlite3_file *pFile,
  sqlite3_int64 iOfst,
  int iAmt,
  void **pp
){
  ApndFile *p = (ApndFile *)pFile;
  if( p->iMark < 0 || iOfst+iAmt > p->iMark)
    return SQLITE_IOERR; /* Cannot read what is not yet there. */
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xFetch(pFile, iOfst+p->iPgOne, iAmt, pp);
}

/* Release a memory-mapped page */
static int apndUnfetch(sqlite3_file *pFile, sqlite3_int64 iOfst, void *pPage){
  ApndFile *p = (ApndFile *)pFile;
  pFile = ORIGFILE(pFile);
  return pFile->pMethods->xUnfetch(pFile, iOfst+p->iPgOne, pPage);
}

/*
** Try to read the append-mark off the end of a file.  Return the
** start of the appended database if the append-mark is present.
** If there is no valid append-mark, return -1;
*/
static sqlite3_int64 apndReadMark(sqlite3_int64 sz, sqlite3_file *pFile){
  int rc, i;
  sqlite3_int64 iMark;
  int msbs = 8 * (APND_MARK_FOS_SZ-1);
  unsigned char a[APND_MARK_SIZE];

  if( APND_MARK_SIZE!=(sz & 0x1ff) ) return -1;
  rc = pFile->pMethods->xRead(pFile, a, APND_MARK_SIZE, sz-APND_MARK_SIZE);
  if( rc ) return -1;
  if( memcmp(a, APND_MARK_PREFIX, APND_MARK_PREFIX_SZ)!=0 ) return -1;
  iMark = ((sqlite3_int64)(a[APND_MARK_PREFIX_SZ] & 0x7f)) << msbs;
  for(i=1; i<8; i++){
    msbs -= 8;
    iMark |= (sqlite3_int64)a[APND_MARK_PREFIX_SZ+i]<<msbs;
  }
  return iMark;
}

static const char apvfsSqliteHdr[] = "SQLite format 3";
/*
** Check to see if the file is an appendvfs SQLite database file.
** Return true iff it is such. Parameter sz is the file's size.
*/
static int apndIsAppendvfsDatabase(sqlite3_int64 sz, sqlite3_file *pFile){
  int rc;
  char zHdr[16];
  sqlite3_int64 iMark = apndReadMark(sz, pFile);
  if( iMark>=0 ){
    /* If file has right end-marker, the expected odd size, and the
     * SQLite DB type marker where the end-marker puts it, then it
     * is an appendvfs database (to be treated as such.)
     */
    rc = pFile->pMethods->xRead(pFile, zHdr, sizeof(zHdr), iMark);
    if( SQLITE_OK==rc && memcmp(zHdr, apvfsSqliteHdr, sizeof(zHdr))==0
        && (sz & 0x1ff)== APND_MARK_SIZE && sz>=512+APND_MARK_SIZE )
      return 1; /* It's an appendvfs database */
  }
  return 0;
}

/*
** Check to see if the file is an ordinary SQLite database file.
** Return true iff so. Parameter sz is the file's size.
*/
static int apndIsOrdinaryDatabaseFile(sqlite3_int64 sz, sqlite3_file *pFile){
  char zHdr[16];
  if( apndIsAppendvfsDatabase(sz, pFile) /* rule 2 */
   || (sz & 0x1ff) != 0
   || SQLITE_OK!=pFile->pMethods->xRead(pFile, zHdr, sizeof(zHdr), 0)
   || memcmp(zHdr, apvfsSqliteHdr, sizeof(zHdr))!=0
  ){
    return 0;
  }else{
    return 1;
  }
}

/* Round-up used to get appendvfs portion to begin at a page boundary. */
#define APND_START_ROUNDUP(fsz, nPageBits) \
  ((fsz) + ((1<<nPageBits)-1) & ~(sqlite3_int64)((1<<nPageBits)-1))

/*
** Open an apnd file handle.
*/
static int apndOpen(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_file *pFile,
  int flags,
  int *pOutFlags
){
  ApndFile *p;
  sqlite3_file *pSubFile;
  sqlite3_vfs *pSubVfs;
  int rc;
  sqlite3_int64 sz;
  pSubVfs = ORIGVFS(pVfs);
  if( (flags & SQLITE_OPEN_MAIN_DB)==0 ){
    /* The appendvfs is not to be used for transient or temporary databases. */
    return pSubVfs->xOpen(pSubVfs, zName, pFile, flags, pOutFlags);
  }
  p = (ApndFile*)pFile;
  memset(p, 0, sizeof(*p));
  pSubFile = ORIGFILE(pFile);
  pFile->pMethods = &apnd_io_methods;
  rc = pSubVfs->xOpen(pSubVfs, zName, pSubFile, flags, pOutFlags);
  if( rc ) goto apnd_open_done;
  rc = pSubFile->pMethods->xFileSize(pSubFile, &sz);
  if( rc ){
    pSubFile->pMethods->xClose(pSubFile);
    goto apnd_open_done;
  }
  if( apndIsOrdinaryDatabaseFile(sz, pSubFile) ){
    memmove(pFile, pSubFile, pSubVfs->szOsFile);
    return SQLITE_OK;
  }
  /* Record that append mark has not been written until seen otherwise. */
  p->iMark = -1;
  p->iPgOne = apndReadMark(sz, pFile);
  if( p->iPgOne>=0 ){
    /* Append mark was found, infer its offset */
    p->iMark = sz - p->iPgOne - APND_MARK_SIZE;
    return SQLITE_OK;
  }
  if( (flags & SQLITE_OPEN_CREATE)==0 ){
    pSubFile->pMethods->xClose(pSubFile);
    rc = SQLITE_CANTOPEN;
  }
  /* Round newly added appendvfs location to #define'd page boundary. 
   * Note that nothing has yet been written to the underlying file.
   * The append mark will be written along with first content write.
   * Until then, the p->iMark value indicates it is not yet written.
   */
  p->iPgOne = APND_START_ROUNDUP(sz, APND_ROUNDUP_BITS);
apnd_open_done:
  if( rc ) pFile->pMethods = 0;
  return rc;
}

/*
** Delete an apnd file.
** For an appendvfs, this could mean delete the appendvfs portion,
** leaving the appendee as it was before it gained an appendvfs.
** For now, this code deletes the underlying file too.
*/
static int apndDelete(sqlite3_vfs *pVfs, const char *zPath, int dirSync){
  return ORIGVFS(pVfs)->xDelete(ORIGVFS(pVfs), zPath, dirSync);
}

/*
** All other VFS methods are pass-thrus.
*/
static int apndAccess(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int flags, 
  int *pResOut
){
  return ORIGVFS(pVfs)->xAccess(ORIGVFS(pVfs), zPath, flags, pResOut);
}
static int apndFullPathname(
  sqlite3_vfs *pVfs, 
  const char *zPath, 
  int nOut, 
  char *zOut
){
  return ORIGVFS(pVfs)->xFullPathname(ORIGVFS(pVfs),zPath,nOut,zOut);
}
static void *apndDlOpen(sqlite3_vfs *pVfs, const char *zPath){
  return ORIGVFS(pVfs)->xDlOpen(ORIGVFS(pVfs), zPath);
}
static void apndDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg){
  ORIGVFS(pVfs)->xDlError(ORIGVFS(pVfs), nByte, zErrMsg);
}
static void (*apndDlSym(sqlite3_vfs *pVfs, void *p, const char *zSym))(void){
  return ORIGVFS(pVfs)->xDlSym(ORIGVFS(pVfs), p, zSym);
}
static void apndDlClose(sqlite3_vfs *pVfs, void *pHandle){
  ORIGVFS(pVfs)->xDlClose(ORIGVFS(pVfs), pHandle);
}
static int apndRandomness(sqlite3_vfs *pVfs, int nByte, char *zBufOut){
  return ORIGVFS(pVfs)->xRandomness(ORIGVFS(pVfs), nByte, zBufOut);
}
static int apndSleep(sqlite3_vfs *pVfs, int nMicro){
  return ORIGVFS(pVfs)->xSleep(ORIGVFS(pVfs), nMicro);
}
static int apndCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut){
  return ORIGVFS(pVfs)->xCurrentTime(ORIGVFS(pVfs), pTimeOut);
}
static int apndGetLastError(sqlite3_vfs *pVfs, int a, char *b){
  return ORIGVFS(pVfs)->xGetLastError(ORIGVFS(pVfs), a, b);
}
static int apndCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *p){
  return ORIGVFS(pVfs)->xCurrentTimeInt64(ORIGVFS(pVfs), p);
}
static int apndSetSystemCall(
  sqlite3_vfs *pVfs,
  const char *zName,
  sqlite3_syscall_ptr pCall
){
  return ORIGVFS(pVfs)->xSetSystemCall(ORIGVFS(pVfs),zName,pCall);
}
static sqlite3_syscall_ptr apndGetSystemCall(
  sqlite3_vfs *pVfs,
  const char *zName
){
  return ORIGVFS(pVfs)->xGetSystemCall(ORIGVFS(pVfs),zName);
}
static const char *apndNextSystemCall(sqlite3_vfs *pVfs, const char *zName){
  return ORIGVFS(pVfs)->xNextSystemCall(ORIGVFS(pVfs), zName);
}

  
#ifdef _WIN32
__declspec(dllexport)
#endif
/* 
** This routine is called when the extension is loaded.
** Register the new VFS.
*/
int sqlite3_appendvfs_init(
  sqlite3 *db, 
  char **pzErrMsg, 
  const sqlite3_api_routines *pApi
){
  int rc = SQLITE_OK;
  sqlite3_vfs *pOrig;
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErrMsg;
  (void)db;
  pOrig = sqlite3_vfs_find(0);
  apnd_vfs.iVersion = pOrig->iVersion;
  apnd_vfs.pAppData = pOrig;
  apnd_vfs.szOsFile = pOrig->szOsFile + sizeof(ApndFile);
  rc = sqlite3_vfs_register(&apnd_vfs, 0);
#ifdef APPENDVFS_TEST
  if( rc==SQLITE_OK ){
    rc = sqlite3_auto_extension((void(*)(void))apndvfsRegister);
  }
#endif
  if( rc==SQLITE_OK ) rc = SQLITE_OK_LOAD_PERMANENTLY;
  return rc;
}
