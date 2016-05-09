#include "multiverso/allocator.h"

#include "multiverso/util/log.h"
#include "multiverso/util/configure.h"

namespace multiverso {

inline FreeList::FreeList(size_t size) :
size_(size) {
  free_ = new MemoryBlock(size, this);
}

#ifdef _MSC_VER 
#define ALIGN_MALLOC(data, size)      \
  data = (char*)_aligned_malloc(size, \
    MV_CONFIG_allocator_alignment);     
#define ALIGN_FREE(data)              \
  _aligned_free(data);
#else   
#define ALIGN_MALLOC(data, size)               \
  CHECK(posix_memalign(&data,                  \
    MV_CONFIG_allocator_alignment, size) == 0);
#define ALIGN_FREE(data)    free(data);
#endif

FreeList::~FreeList() {
  MemoryBlock*move = free_, *next;
  while (move) {
    next = move->next;
    delete move;
    move = next;
  }
}

inline char* FreeList::Pop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_ == nullptr) {
    free_ = new MemoryBlock(size_, this);
  }
  char* data = free_->data();
  free_ = free_->next;
  return data;
}

inline void FreeList::Push(MemoryBlock*block) {
  std::lock_guard<std::mutex> lock(mutex_);
  block->next = free_;
  free_ = block;
}

MV_DEFINE_int(allocator_alignment, 16, "alignment for align malloc");

inline MemoryBlock::MemoryBlock(size_t size, FreeList* list) :
next(nullptr) {
  ALIGN_MALLOC(data_, size + header_size_);
  *(FreeList**)(data_) = list;
  *(MemoryBlock**)(data_ + g_pointer_size) = this;
}

MemoryBlock::~MemoryBlock() {
  ALIGN_FREE(data_);
}

inline void MemoryBlock::Unlink() {
  if ((--ref_) == 0) {
    (*(FreeList**)data_)->Push(this);
  }
}

inline char* MemoryBlock::data() {
  ++ref_;
  return data_ + header_size_;
}

inline void MemoryBlock::Link() {
  ++ref_;
}

char* SmartAllocator::Malloc(size_t size) {
  const static size_t t = ((size_t)(-1)) << 5;
  size = ((size & 31) ? ((size & t) + 32) : size);
  std::unique_lock<std::mutex> lock(mutex_);
  if (pools_[size] == nullptr) {
    pools_[size] = new FreeList(size);
  }
  lock.unlock();

  return pools_[size]->Pop();
}

void SmartAllocator::Free(char *data) {
  (*(MemoryBlock**)(data - g_pointer_size))->Unlink();
}

void SmartAllocator::Refer(char *data) {
  (*(MemoryBlock**)(data - g_pointer_size))->Link();
}

SmartAllocator::~SmartAllocator() {
  Log::Debug("~SmartAllocator, final pool size: %d\n", pools_.size());
  for (auto i : pools_) {
    delete i.second;
  }
}

char* Allocator::Malloc(size_t size) {
  char* data;
  ALIGN_MALLOC(data, size + header_size_);
  // record ref
  *(std::atomic<int>**)data = new std::atomic<int>(1);
  return data + header_size_;
}

void Allocator::Free(char* data) {
  data -= header_size_;
  if (--(**(std::atomic<int>**)data) == 0) {
    delete *(std::atomic<int>**)data;
    ALIGN_FREE(data);
  }
}

void Allocator::Refer(char* data) {
  ++(**(std::atomic<int>**)(data - header_size_));
}

MV_DEFINE_string(allocator_type, "smart", "use smart allocator by default");
Allocator* Allocator::Get() {
  if (MV_CONFIG_allocator_type == "smart") {
    static SmartAllocator allocator_;
    return &allocator_;
  }
  static Allocator allocator_;
  return &allocator_;
}

} // namespace multiverso 