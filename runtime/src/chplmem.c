#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "chplmem.h"
#include "chplrt.h"
#include "chpltypes.h"
#include "error.h"

#undef malloc
#undef calloc
#undef free
#undef realloc

#define HASHSIZE 1019

typedef struct _memTableEntry { /* table entry */
  size_t number;
  size_t size;
  char* description;
  void* memAlloc;

  struct _memTableEntry* nextInBucket;
  struct _memTableEntry* prevInstalled;
  struct _memTableEntry* nextInstalled;

} memTableEntry;

_chpl_meminfo_t chpl_meminfo = {0};
int broadcastingGlobalsStarted = 0;
int whichMalloc = 0;

/* hash table */
static memTableEntry* memTable[HASHSIZE];
static memTableEntry* first = NULL;
static memTableEntry* last = NULL;


/* hashing function */
static unsigned hash(void* memAlloc) {
  unsigned hashValue = 0;
  char* fakeCharPtr = (char*)&memAlloc;
  size_t i;
  for (i = 0; i < sizeof(void*); i++) {
    hashValue = *fakeCharPtr + 31 * hashValue;
    fakeCharPtr++;
  }
  return hashValue % HASHSIZE;
}


static int memstat = 0;
static int memstatSet = 0;
static int memtrace = 0;
static int memtraceSet = 0;
static int memtrack = 0;
static int memtrackSet = 0;
static int64_t memmaxValue = 0;
static int64_t memthresholdValue = 0;
static FILE* memlog = NULL;
static size_t totalMem = 0;  /* total memory currently allocated */
static size_t maxMem = 0;    /* maximum total memory during run  */


chpl_mutex_t _memtrack_lock;
chpl_mutex_t _memstat_lock;
chpl_mutex_t _memtrace_lock;
chpl_mutex_t _malloc_lock;


void initMemTable(void) {
  if (memtrack) {
    int i;
    for (i = 0; i < HASHSIZE; i++) {
      memTable[i] = NULL;
    }
  }
}


void setMemmax(int64_t value) {
  memmaxValue = value;
  setMemstat();
}


void setMemstat(void) {
  memstatSet = 1;
  memtrackSet = 1;
}


void setMemtrack(void) {
  memtrackSet = 1;
}


void setMemthreshold(int64_t value) {
  if (!memlog) {
    chpl_error("--memthreshold useless when used without --memtrace", 0, 0);
  }
  memthresholdValue = value;
}


void setMemtrace(char* memlogname) {
  memtraceSet = 1;
  if (memlogname) {
    memlog = fopen(memlogname, "w");
    if (!memlog) {
      char* message = _glom_strings(3, "Unable to open \"", memlogname, 
                                    "\"");
      chpl_error(message, 0, 0);
    }
  } 
}


static void updateMaxMem(void) {
  if (totalMem > maxMem) {
    maxMem = totalMem;
  }
}


static void increaseMemStat(size_t chunk, int32_t lineno, _string filename) {
  totalMem += chunk;
  if (memmaxValue && (totalMem > memmaxValue)) {
    const char* message = "Exceeded memory limit";
    chpl_mutex_unlock(&_memstat_lock);
    chpl_error(message, lineno, filename);
  }
  updateMaxMem();
}


static void decreaseMemStat(size_t chunk) {
  totalMem -= chunk;
  updateMaxMem();
}


void resetMemStat(void) {
  totalMem = 0;
  maxMem = 0;
}


void startTrackingMem(void) {
    memstat = memstatSet;
    memtrack = memtrackSet;
    memtrace = memtraceSet;
}


static int alreadyPrintingStat = 0;

uint64_t _mem_used(int32_t lineno, _string filename) {
  uint64_t u;
  alreadyPrintingStat = 1; /* hack: don't want to print final stats */
  if (!memstat)
    chpl_error("memoryUsed() only works with the --memstat flag", lineno, filename);
  u = (uint64_t)totalMem;
  return u;
}

void printMemStat(int32_t lineno, _string filename) {
  if (memstat) {
    chpl_mutex_lock(&_memstat_lock);
    fprintf(stdout, "totalMem=%u, maxMem=%u\n", 
            (unsigned)totalMem, (unsigned)maxMem);
    alreadyPrintingStat = 1;
    chpl_mutex_unlock(&_memstat_lock);
  } else {
    const char* message = "printMemStat() only works with the --memstat flag";
    chpl_error(message, lineno, filename);
  }
}


void printFinalMemStat(int32_t lineno, _string filename) {
  if (!alreadyPrintingStat && memstat) {
    fprintf(stdout, "Final Memory Statistics:  ");
    printMemStat(lineno, filename);
  }
}


void printMemTable(int64_t threshold, int32_t lineno, _string filename) {
  memTableEntry* memEntry = NULL;

  int numberWidth   = 9;
  int addressWidth  = 12;
  int precision     = 8;

  const char* size        = "Size:";
  const char* bytes       = "(bytes)"; 
  const char* number      = "Number:";
  const char* total       = "Total:";
  const char* address     = "Address:";
  const char* description = "Description:";
  const char* line40      = "========================================";   

  if (!memtrack) {
    const char* message = "The printMemTable function only works with the "
      "--memtrack flag";
    chpl_error(message, lineno, filename);
  }

  fprintf(stdout, "\n");

  fprintf(stdout, "%s%s\n", line40, line40);

  fprintf(stdout, "----------------------\n");
  fprintf(stdout, "***Allocated Memory***\n");
  fprintf(stdout, "----------------------\n");
  
  fprintf(stdout, "%-*s%-*s%-*s%-*s%-s\n", 
          numberWidth, size, 
          numberWidth, number, 
          numberWidth, total, 
          addressWidth, address, 
          description);
  
  fprintf(stdout, "%-*s%-*s%-*s\n", 
          numberWidth, bytes, 
          numberWidth, "", 
          numberWidth, bytes);
  
  fprintf(stdout, "%s%s\n", line40, line40);
  
  for (memEntry = first;
       memEntry != NULL;
       memEntry = memEntry->nextInstalled) {
    
    size_t chunk = memEntry->number * memEntry->size;
    if (chunk > threshold) {      
      fprintf(stdout, "%-*u%-*u%-*u%#-*.*x%-s\n", 
              numberWidth, (unsigned)memEntry->size, 
              numberWidth, (unsigned)memEntry->number, 
              numberWidth, (unsigned)chunk, 
              addressWidth, precision, (unsigned)(intptr_t)memEntry->memAlloc, 
              memEntry->description);
    }
  }
  fprintf(stdout, "\n");
}


static memTableEntry* lookupMemory(void* memAlloc) {
  memTableEntry* memEntry = NULL;
  unsigned hashValue = hash(memAlloc);

  for (memEntry = memTable[hashValue]; 
       memEntry != NULL; 
       memEntry = memEntry->nextInBucket) {

    if (memEntry->memAlloc == memAlloc) {
      return memEntry;
    }
  }
  return NULL;
}


static void installMemory(void* memAlloc, size_t number, size_t size, 
                          const char* description) {
  unsigned hashValue;
  memTableEntry* memEntry = lookupMemory(memAlloc);

  if (!memEntry) { 
    memEntry = (memTableEntry*) calloc(1, sizeof(memTableEntry));
    if (!memEntry) {
      char* message = _glom_strings(3, "Out of memory allocating table entry "
                                    "for \"", description, "\"");
      chpl_error(message, 0, 0);
    }

    hashValue = hash(memAlloc);
    memEntry->nextInBucket = memTable[hashValue];
    memTable[hashValue] = memEntry;

    if (first == NULL) {
      first = memEntry;
    } else {
      last->nextInstalled = memEntry;
      memEntry->prevInstalled = last;
    }
    last = memEntry;

    memEntry->description = (char*) malloc((strlen(description) + 1)
                                           * sizeof(char));
    if (!memEntry->description) {
      char* message = _glom_strings(3, "Out of memory allocating table entry "
                                    "for \"", description, "\"");
      chpl_error(message, 0, 0);
    }
    strcpy(memEntry->description, description);
    memEntry->memAlloc = memAlloc;
  }  
  memEntry->number = number;
  memEntry->size = size;
}


static memTableEntry* removeBucketEntry(void* address) {
  unsigned hashValue = hash(address);    
  memTableEntry* thisBucketEntry = memTable[hashValue];
  memTableEntry* deletedBucket = NULL;
    
  if (thisBucketEntry->memAlloc == address) {
    memTable[hashValue] = thisBucketEntry->nextInBucket;
    deletedBucket = thisBucketEntry;
  } else {
    for (thisBucketEntry = memTable[hashValue]; 
         thisBucketEntry != NULL; 
         thisBucketEntry = thisBucketEntry->nextInBucket) {

      memTableEntry* nextBucketEntry = thisBucketEntry->nextInBucket;
        
      if (nextBucketEntry && nextBucketEntry->memAlloc == address) {
        thisBucketEntry->nextInBucket = nextBucketEntry->nextInBucket;
        deletedBucket = nextBucketEntry;
      }
    }
  }
  if (deletedBucket == NULL) {
    chpl_internal_error("Hash table entry has disappeared unexpectedly!");
  }
  return deletedBucket;
}


static void updateMemory(memTableEntry* memEntry, void* oldAddress, 
                         void* newAddress, size_t number, size_t size) {
  unsigned newHashValue = hash(newAddress);

  /* Rehash on the new memory location.  */
  removeBucketEntry(oldAddress);
  memEntry->nextInBucket = memTable[newHashValue];
  memTable[newHashValue] = memEntry;

  memEntry->memAlloc = newAddress;
  memEntry->number = number;
  memEntry->size = size;
}


static void removeMemory(void* memAlloc, int32_t lineno, _string filename) {
  memTableEntry* memEntry = lookupMemory(memAlloc);
  memTableEntry* thisBucketEntry;

  if (memEntry) {
    /* Remove the entry from the first-to-last list. */
    if (memEntry == first) {
      first = memEntry->nextInstalled;
      if (memEntry->nextInstalled) {
        memEntry->nextInstalled->prevInstalled = NULL;
      }
    } else {
      memEntry->prevInstalled->nextInstalled = memEntry->nextInstalled;
      if (memEntry->nextInstalled) {
        memEntry->nextInstalled->prevInstalled = memEntry->prevInstalled;
      } else {
        last = memEntry->prevInstalled;
      } 
    }

    /* Remove the entry from the bucket list. */
    thisBucketEntry = removeBucketEntry(memAlloc);
    free(thisBucketEntry->description);
    free(thisBucketEntry);
  } else {
    const char* message = "Attempting to free memory that wasn't allocated";
    chpl_error(message, lineno, filename);
  }
}


static void
confirm(void* memAlloc, const char* description, int32_t lineno, 
        _string filename) {
  if (!memAlloc) {
    char message[1024];
    sprintf(message, "Out of memory allocating \"%s\"", description);
    chpl_error(message, lineno, filename);
  }
}


static void printToMemLog(size_t number, size_t size, const char* description, 
                          const char* memType, void* memAlloc, void* moreMemAlloc) {
  size_t chunk = number * size;
  
  if (chunk >= memthresholdValue) {
    if (moreMemAlloc && (moreMemAlloc != memAlloc)) {
      fprintf(memlog, "%s called for %u items of size %u"
              " for %s:  0x%x -> 0x%x\n", memType, (unsigned)number, 
              (unsigned)size, description, (unsigned)(intptr_t)memAlloc, 
              (unsigned)(intptr_t)moreMemAlloc);     
    } else {
      fprintf(memlog, "%s called for %u items of size %u"
              " for %s:  0x%x\n", memType, (unsigned)number, (unsigned)size, 
              description, (unsigned)(intptr_t)memAlloc);       
    }
  }
}


static size_t computeChunkSize(size_t number, size_t size, int zeroOK, 
                               int32_t lineno, _string filename) {
  size_t chunk = number * size;
  long long bigChunk = (long long)number * (long long)size;
  if (bigChunk != chunk) {
    chpl_error("Attempting to allocate > max(size_t) bytes of memory", lineno,
               filename);
  }
  if (chunk == 0 && zeroOK == 0) {
    chpl_internal_error("Attempting to allocate 0 bytes of memory");
  }
  return chunk;
}


static void* _chpl_malloc1(size_t number, size_t size,
                           const char* description,
                           int32_t lineno, _string filename) {
  size_t chunk = computeChunkSize(number, size, 0, lineno, filename);
  void* memAlloc = (chunk) ? malloc(chunk) : 0x0;
  if (chunk != 0) {
    confirm(memAlloc, description, lineno, filename);
  }

  if (memtrace) {
    chpl_mutex_lock(&_memtrace_lock);
    printToMemLog(number, size, description, "malloc", memAlloc, NULL);
    chpl_mutex_unlock(&_memtrace_lock);
  }
  if (memtrack) {
    chpl_mutex_lock(&_memtrack_lock);
    installMemory(memAlloc, number, size, description);
    chpl_mutex_unlock(&_memtrack_lock);
    if (memstat) {
      chpl_mutex_lock(&_memstat_lock);
      increaseMemStat(chunk, lineno, filename);
      chpl_mutex_unlock(&_memstat_lock);
    }
  }
  return memAlloc;
}


static void* _chpl_malloc2(size_t number, size_t size, const char* description,
                           int32_t lineno, _string filename) {
  size_t chunk = computeChunkSize(number, size, 0, lineno, filename);
  void* memAlloc;
  chunk = chunk + (16 - (chunk % 16)); // 16 byte aligned.
  if (broadcastingGlobalsStarted == 0 && chpl_meminfo.head == NULL) {
    //
    // This branch is taken by the original process that launches the
    // sub-processes in a multi-locale program. It should never be taken
    // by a process that has a localeID.
    //
    memAlloc = _chpl_malloc1(number, size, description, lineno, filename);
    confirm(memAlloc, description, lineno, filename);
  } else {
    chpl_mutex_lock(&_malloc_lock);
    if (broadcastingGlobalsStarted == 0 || chpl_meminfo.head == NULL) {
      chpl_error("_chpl_malloc called before properly initialized", lineno, filename);
    }
    memAlloc = chpl_meminfo.current;
    chpl_meminfo.current += chunk;
    if (chpl_meminfo.current > chpl_meminfo.tail) {
      chpl_error("_chpl_malloc out of memory", lineno, filename);
    }
    chpl_mutex_unlock(&_malloc_lock);
  }
  return memAlloc;
}


void* _chpl_malloc(size_t number, size_t size, const char* description,
                   int32_t lineno, _string filename) {
  void* memAlloc;
  switch (whichMalloc) {
    case 1:
      memAlloc = _chpl_malloc1(number, size, description, lineno, filename);
      break;
    case 2:
      memAlloc = _chpl_malloc2(number, size, description, lineno, filename);
      break;
    default:
      memAlloc = NULL;
      chpl_error("bad malloc type", lineno, filename);
  }
  return memAlloc;
}


void* _chpl_calloc(size_t number, size_t size, const char* description, 
                   int32_t lineno, _string filename) {
  void* memAlloc;
  if (whichMalloc == 1) {
    memAlloc = calloc(number, size);
    confirm(memAlloc, description, lineno, filename);
  } else if (whichMalloc == 2) {
    memAlloc = _chpl_malloc(number, size, description, lineno, filename);
    confirm(memAlloc, description, lineno, filename);
    memset(memAlloc, 0, number*size);
  } else {
    chpl_error("bad malloc type", lineno, filename);
    memAlloc = NULL; // to avoid warnings
  }

  if (memtrace) {
    chpl_mutex_lock(&_memtrace_lock);
    printToMemLog(number, size, description, "calloc", memAlloc, NULL);
    chpl_mutex_unlock(&_memtrace_lock);
  }

  if (memtrack) {
    chpl_mutex_lock(&_memtrack_lock);
    installMemory(memAlloc, number, size, description);
    chpl_mutex_unlock(&_memtrack_lock);
    if (memstat) {
      size_t chunk;
      chpl_mutex_lock(&_memstat_lock);
      chunk = number * size;
      increaseMemStat(chunk, lineno, filename);
      chpl_mutex_unlock(&_memstat_lock);
    }
  }
  return memAlloc;
}


void _chpl_free(void* memAlloc, int32_t lineno, _string filename) {
  if (whichMalloc == 2)
    return;

  if (memAlloc != (void*)0x0) {
  if (memtrace) {
    chpl_mutex_lock(&_memtrace_lock);
    if (memtrack) {
      memTableEntry* memEntry;
      chpl_mutex_lock(&_memtrack_lock);
      memEntry = lookupMemory(memAlloc);
      chpl_mutex_unlock(&_memtrack_lock);
      printToMemLog(memEntry->number, memEntry->size, memEntry->description, "free", memAlloc, NULL);
    } else
      printToMemLog(0, 0, "", "free", memAlloc, NULL);
    chpl_mutex_unlock(&_memtrace_lock);
  }

  if (memtrack) {
    chpl_mutex_lock(&_memtrack_lock);
    if (memstat) {
      memTableEntry* memEntry;
      size_t chunk;
      memEntry = lookupMemory(memAlloc);
      if (memEntry) {
        chunk = memEntry->number * memEntry->size;
        chpl_mutex_lock(&_memstat_lock);
        decreaseMemStat(chunk);
        chpl_mutex_unlock(&_memstat_lock);
      }
    }
    removeMemory(memAlloc, lineno, filename);
    chpl_mutex_unlock(&_memtrack_lock);
  }
  }

  free(memAlloc);
}


void* _chpl_realloc(void* memAlloc, size_t number, size_t size, 
                    const char* description, int32_t lineno, _string filename) {
  size_t newChunk = computeChunkSize(number, size, 1, lineno, filename);
  memTableEntry* memEntry = NULL;
  void* moreMemAlloc;
  if (whichMalloc == 2) {
    chpl_error("realloc is not compatible with this malloc", lineno, filename);
  }
  if (!newChunk) {
    _chpl_free(memAlloc, lineno, filename);
    return NULL;
  }
  if (memtrack) {
    chpl_mutex_lock(&_memtrack_lock);
    memEntry = lookupMemory(memAlloc);
    chpl_mutex_unlock(&_memtrack_lock);
    if (!memEntry && (memAlloc != NULL)) {
      char* message;
      message = _glom_strings(3, "Attempting to realloc memory for ",
                              description, " that wasn't allocated");
      chpl_error(message, lineno, filename);
    }
  }
  moreMemAlloc = realloc(memAlloc, newChunk);
  confirm(moreMemAlloc, description, lineno, filename);

  if (memtrack) { 
    chpl_mutex_lock(&_memtrack_lock);
    if (memAlloc != NULL) {
      if (memEntry) {
        if (memstat) {
          size_t oldChunk;
          chpl_mutex_lock(&_memstat_lock);
          oldChunk = memEntry->number * memEntry->size;
          decreaseMemStat(oldChunk);
          chpl_mutex_unlock(&_memstat_lock);
        }
        updateMemory(memEntry, memAlloc, moreMemAlloc, number, size);
      }
    } else {
      installMemory(moreMemAlloc, number, size, description);
    }
    if (memstat) {
      chpl_mutex_lock(&_memstat_lock);
      increaseMemStat(newChunk, lineno, filename);
      chpl_mutex_unlock(&_memstat_lock);
    }
    chpl_mutex_unlock(&_memtrack_lock);
  }
  if (memtrace) {
    chpl_mutex_lock(&_memtrace_lock);
    printToMemLog(number, size, description, "realloc", memAlloc, 
                  moreMemAlloc);
    chpl_mutex_unlock(&_memtrace_lock);
  }
  return moreMemAlloc;
}
