#include "VersionSet.h"

#include "VersionEdit.h"
#include "coding.h"
#include "comparator.h"
#include "env.h"
#include "filename.h"
#include "format.h"
#include "iterator.h"
#include "log_writer.h"
#include "options.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>

namespace LSMKV {

Version::~Version() {
  assert(refs_ == 0);

  prev_->next_ = next_;
  next_->prev_ = prev_;

  for (int level = 0; level < config::kNumLevels; level++) {
    for (size_t i = 0; i < files_[level].size(); i++) {
      FileMetaData *f = files_[level][i];

      assert(f->refs > 0);
      f->refs--;
      if (f->refs <= 0) {
        delete f;
      }
    }
  }
}

int FindFile(const InternalKeyComparator &icmp,
             const std::vector<FileMetaData *> &files, const Slice &key) {
  uint32_t left = 0;
  uint32_t right = files.size();

  while (left < right) {
    uint32_t mid = (left + right) / 2;
    const FileMetaData *f = files[mid];
    if (icmp.Compare(f->largest.Encode(), key) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }

  return right;
}

static bool AfterFile(const Comparator *ucmp, const Slice *user_key,
                      const FileMetaData *f) {
  return (user_key != nullptr &&
          ucmp->Compare(*user_key, f->largest.user_key()) > 0);
}
static bool BeforeFile(const Comparator *ucmp, const Slice *user_key,
                       const FileMetaData *f) {
  return (user_key != nullptr &&
          ucmp->Compare(*user_key, f->smallest.user_key()) < 0);
}

bool SomeFileOverlapsRange(const InternalKeyComparator &icmp,
                           bool disjoint_sorted_files,
                           const std::vector<FileMetaData *> &files,
                           const Slice *smallest_user_key,
                           const Slice *largest_user_key) {
  const Comparator *ucmp = icmp.user_comparator();
  if (!disjoint_sorted_files) {
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData *f = files[i];
      if (AfterFile(ucmp, smallest_user_key, f) ||
          BeforeFile(ucmp, largest_user_key, f)) {

      } else {
        return true;
      }
    }

    return false;
  }

  uint32_t index = 0;

  // overlap条件：query_smallest <= file_largest && query_largest >=
  // file_smallest

  // 保证 file_largest >= query_smallest
  if (smallest_user_key != nullptr) {
    InternalKey small_key(*smallest_user_key, kMaxSequenceNumber,
                          kValueTypeForSeek);
    index = FindFile(icmp, files, small_key.Encode());
  }

  if (index >= files.size()) {
    return false;
  }

  // 检查query_largest >= file_smallest
  return !BeforeFile(ucmp, largest_user_key, files[index]);
}

class Version::LevelFileNumIterator : public Iterator {
public:
  LevelFileNumIterator(const InternalKeyComparator &icmp,
                       const std::vector<FileMetaData *> *flist)
      : icmp_(icmp), flist_(flist), index_(flist->size()) {}

  bool Valid() const override { return index_ < flist_->size(); }
  void Seek(const Slice &target) override {
    index_ = FindFile(icmp_, *flist_, target);
  }
  void SeekToFirst() override { index_ = 0; }
  void SeekToLast() override {
    index_ = flist_->empty() ? 0 : flist_->size() - 1;
  }
  void Next() override {
    assert(Valid());
    index_++;
  }

  void Prev() override {
    assert(Valid());
    if (index_ == 0) {
      index_ = flist_->size();
    } else {
      index_--;
    }
  }

  Slice key() const override {
    assert(Valid());
    return (*flist_)[index_]->largest.Encode();
  }

  Slice value() const override {
    assert(Valid());
    EncodeFixed64(value_buf_, (*flist_)[index_]->number);
    EncodeFixed64(value_buf_, (*flist_)[index_]->file_size);
    return Slice(value_buf_, sizeof(value_buf_));
  }

  Status status() const override { return Status::OK(); }

private:
  const InternalKeyComparator icmp_;
  const std::vector<FileMetaData *> *flist_;
  uint32_t index_;

  mutable char value_buf_[16];
};

static Iterator *GetFileIterator(void *arg, const ReadOptions &options,
                                 const Slice &file_value) {}

namespace {}

static bool NewestFirst(FileMetaData *a, FileMetaData *b) {
  return a->number > b->number;
}

void Version::ForEachOverlapping(Slice user_key, Slice internal_key, void *arg,
                                 bool (*func)(void *, int, FileMetaData *)) {
  const Comparator *ucmp = vset_->icmp_.user_comparator();

  std::vector<FileMetaData *> tmp;
  tmp.reserve(files_[0].size());
  for (uint32_t i = 0; i < files_[0].size(); i++) {
    FileMetaData *f = files_[0][i];
    if (ucmp->Compare(user_key, f->smallest.user_key()) >= 0 &&
        ucmp->Compare(user_key, f->largest.user_key()) <= 0) {
      tmp.push_back(files_[0][i]);
    }
  }

  if (!tmp.empty()) {
    std::sort(tmp.begin(), tmp.end(), NewestFirst);
    for (uint32_t i = 0; i < tmp.size(); i++) {
      if (!(*func)(arg, 0, tmp[i])) {
        return;
      }
    }
  }

  for (int level = 1; level < config::kNumLevels; level++) {
    size_t num_files = files_[level].size();
    if (num_files == 0)
      continue;

    uint32_t index = FindFile(vset_->icmp_, files_[level], internal_key);
    if (index < num_files) {
      FileMetaData *f = files_[level][index];
      if (ucmp->Compare(user_key, f->smallest.user_key()) < 0) {

      } else {
        if (!(*func)(arg, level, f)) {
          return;
        }
      }
    }
  }
}

/*
Status Version::Get(const ReadOptions& options, const LookupKey& k, std::string*
value, GetStats* stats) { stats->seek_file = nullptr; stats->seek_file_level =
-1;

    struct State {
    //Saver saver;
    GetStats* stats;
    const ReadOptions* options;
    Slice ikey;
    FileMetaData* last_file_read;
    int last_file_read_level;

    VersionSet* vset;
    Status s;
    bool found;

    static bool Match(void* arg, int level, FileMetaData* f) {
        State* state = reinterpret_cast<State*>(arg);

        if (state->stats->seek_file == nullptr && state->last_file_read !=
nullptr) { state->stats->seek_file = state
        }
    }
    };
*/

void Version::Ref() { ++refs_; }

void Version::Unref() {
  assert(this != &vset_->dummy_versions_);
  assert(refs_ >= 1);
  --refs_;
  if (refs_ == 0) {
    delete this;
  }
}

bool Version::OverlapInLevel(int level, const Slice *smallest_user_key,
                             const Slice *largest_user_key) {
  return SomeFileOverlapsRange(vset_->icmp_, (level > 0), files_[level],
                               smallest_user_key, largest_user_key);
}

int Version::PickLevelForMemtableOutput(const Slice &smallest_user_key,
                                        const Slice &largest_user_key) {
                                          //todo
                                          }

void Version::GetOverlappingInputs(int level, const InternalKey *begin,
                                   const InternalKey *end,
                                   std::vector<FileMetaData *> *inputs) {
  assert(level >= 0);
  assert(level < config::kNumLevels);

  inputs->clear();
  Slice user_begin, user_end;
  if (begin != nullptr) {
    user_begin = begin->user_key();
  }
  if (end != nullptr) {
    user_end = end->user_key();
  }

  const Comparator *user_cmp = vset_->icmp_.user_comparator();
  for (size_t i = 0; i < files_[level].size();) {
    FileMetaData *f = files_[level][i++];
    const Slice file_start = f->smallest.user_key();
    const Slice file_limit = f->largest.user_key();
    if (begin != nullptr && user_cmp->Compare(file_limit, user_begin) < 0) {

    } else if (end != nullptr && user_cmp->Compare(file_start, user_end) > 0) {

    } else {
      inputs->push_back(f);
      if (level == 0) {
        if (begin != nullptr && user_cmp->Compare(file_start, user_begin) < 0) {
          user_begin = file_start;
          inputs->clear();
          i = 0;
        } else if (end != nullptr &&
                   user_cmp->Compare(file_limit, user_end) > 0) {
          user_end = file_limit;
          inputs->clear();
          i = 0;
        }
      }
    }
  }
}

class VersionSet::Builder {
private:
  struct BySmallestKet {
    const InternalKeyComparator *internalkey_comparator;

    bool operator()(FileMetaData *a, FileMetaData *b) const {
      int r = internalkey_comparator->Compare(a->smallest, b->smallest);
      if (r != 0) {
        return (r < 0);
      } else {
        return (a->number < b->number);
      }
    }
  };

  typedef std::set<FileMetaData *, BySmallestKet> FileSet;
  struct LevelState {
    std::set<uint64_t> deleted_files;
    FileSet *added_files;
  };

  VersionSet *vset_;
  Version *base_;
  LevelState levels_[config::kNumLevels];

public:
  Builder(VersionSet *vset, Version *base) : vset_(vset), base_(base) {
    base_->Ref();
    BySmallestKet cmp;
    cmp.internalkey_comparator = &vset->icmp_;
    for (int i = 0; i < config::kNumLevels; i++) {
      levels_[i].added_files = new FileSet(cmp);
    }
  }

  ~Builder() {
    for (int level = 0; level < config::kNumLevels; level++) {
      const FileSet *added = levels_[level].added_files;
      std::vector<FileMetaData *> to_unref;
      to_unref.reserve(added->size());
      for (auto *x : *added) {
        to_unref.push_back(x);
      }
      delete added;
      for (uint32_t i = 0; i < to_unref.size(); i++) {
        FileMetaData *f = to_unref[i];
        f->refs--;
        if (f->refs <= 0) {
          delete f;
        }
      }
    }

    base_->Unref();
  }

  void Apply(const VersionEdit *edit) {
    for (size_t i = 0; i < edit->compact_pointers_.size(); i++) {
      const int level = edit->compact_pointers_[i].first;
      vset_->compact_pointer_[level] = edit->compact_pointers_[i].second.Encode().ToString();
    }

    // delete files
    for (const auto &deleted_file_set_kvp : edit->deleted_files_) {
      const int level = deleted_file_set_kvp.first;
      const uint64_t number = deleted_file_set_kvp.second;

      levels_[level].deleted_files.insert(number);
    }

    // added files
    for (size_t i = 0; i < edit->new_files_.size(); i++) {
      const int level = edit->new_files_[i].first;
      FileMetaData *f = new FileMetaData(edit->new_files_[i].second);
      f->refs = 1;

      // seek compaction
      f->allowed_seeks = static_cast<int>(f->file_size / 16384U);
      if (f->allowed_seeks < 100)
        f->allowed_seeks = 100;

      levels_[level].deleted_files.erase(f->number);
      levels_[level].added_files->insert(f);
    }
  }

  void SaveTo(Version *v) {
    BySmallestKet cmp;
    cmp.internalkey_comparator = &vset_->icmp_;
    for (int level = 0; level < config::kNumLevels; level++) {
      const std::vector<FileMetaData *> &base_files = base_->files_[level];
      std::vector<FileMetaData *>::const_iterator base_iter =
          base_files.begin();
      std::vector<FileMetaData *>::const_iterator base_end = base_files.end();
      const FileSet *added_files = levels_[level].added_files;
      v->files_[level].reserve(base_files.size() + added_files->size());
      for (const auto &f : *added_files) {
        for (std::vector<FileMetaData *>::const_iterator bpos =
                 std::upper_bound(base_iter, base_end, f, cmp);
             base_iter != bpos; base_iter++) {
          MaybeAddFile(v, level, *base_iter);
        }
        MaybeAddFile(v, level, f);
      }

      for (; base_iter != base_end; base_iter++) {
        MaybeAddFile(v, level, *base_iter);
      }

#ifndef NDEBUG
      if (level > 0) {
        for (uint32_t i = 1; i < v->files_[level].size(); i++) {
          const InternalKey &prev_end = v->files_[level][i - 1]->largest;
          const InternalKey &this_begin = v->files_[level][i]->smallest;
          if (vset_->icmp_.Compare(prev_end, this_begin) >= 0) {
            std::fprintf(stderr, "overlapping ranges in same level %s vs. %s\n",
                         prev_end.DebugString().c_str(),
                         this_begin.DebugString().c_str());
            std::abort();
          }
        }
      }
#endif
    }
  }

  void MaybeAddFile(Version *v, int level, FileMetaData *f) {
    if (levels_[level].deleted_files.count(f->number) > 0) {

    } else {
      std::vector<FileMetaData *> *files = &v->files_[level];
      if (level > 0 && !files->empty()) {
        assert(vset_->icmp_.Compare((*files)[files->size() - 1]->largest,
                                    f->smallest) < 0);
      }
      f->refs++;
      files->push_back(f);
    }
  }
};

VersionSet::VersionSet(const std::string& dbname, const Options* options,
                       TableCache* table_cache,
                       const InternalKeyComparator* cmp)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      table_cache_(table_cache),
      icmp_(*cmp),
      next_file_number_(2),
      manifest_file_number_(0),  // Filled by Recover()
      last_sequence_(0),
      log_number_(0),
      prev_log_number_(0),
      descriptor_file_(nullptr),
      descriptor_log_(nullptr),
      dummy_versions_(this),
      current_(nullptr) {
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);
  delete descriptor_file_;
  delete descriptor_log_;
}

void VersionSet::AppendVersion(Version* v) {
  assert(v->refs_ == 0);
  assert(v != current_);

  if(current_ != nullptr) {
    current_->Unref();
  }

  current_ = v;
  v->Ref();

  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

uint64_t VersionSet::NewFileNumber() {
  assert(next_file_number_ < std::numeric_limits<uint64_t>::max());
  return next_file_number_++;
}

void VersionSet::ReuseFileNumber(uint64_t file_number) {
  // 用减法判断，避免非法的 UINT64_MAX 参数在 file_number + 1 时回绕。
  if (next_file_number_ > 0 && file_number == next_file_number_ - 1) {
    next_file_number_ = file_number;
  }
}

SequenceNumber VersionSet::LastSequence() const { return last_sequence_; }

void VersionSet::SetLastSequence(SequenceNumber sequence) {
  assert(sequence >= last_sequence_);
  assert(sequence <= kMaxSequenceNumber);
  last_sequence_ = sequence;
}

uint64_t VersionSet::LogNumber() const { return log_number_; }

uint64_t VersionSet::PrevLogNumber() const { return prev_log_number_; }

uint64_t VersionSet::ManifestFileNumber() const { return manifest_file_number_; }

void VersionSet::MarkFileNumberUsed(uint64_t number) {
  assert(number < std::numeric_limits<uint64_t>::max());
  if (next_file_number_ <= number) {
    next_file_number_ = number + 1;
  }
}

int VersionSet::NumLevelFiles(int level) const{
  assert(level >= 0);
  assert(level < config::kNumLevels);
  return current_->files_[level].size();
}

int64_t VersionSet::NumLevelBytes(int level) const {
  assert(level >= 0);
  assert(level < config::kNumLevels);
  uint64_t total = 0;
  for (const FileMetaData* file : current_->files_[level]) {
    assert(file->file_size <=
           static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - total);
    total += file->file_size;
  }
  return static_cast<int64_t>(total);
}

void VersionSet::AddLiveFiles(std::set<uint64_t>* live) {
  assert(live != nullptr);
  for (Version* v = dummy_versions_.next_; v != &dummy_versions_; v = v->next_) {
    for (int level = 0; level < config::kNumLevels; level++) {
      const std::vector<FileMetaData*>& files = v->files_[level];
      for (size_t i = 0; i < files.size(); i++) {
        live->insert(files[i]->number);
      }
    }
  }
}

Status VersionSet::WriteSnapshot(log::Writer* log) {
  VersionEdit edit;

  edit.SetComparatorName(icmp_.user_comparator()->Name());

  for (int level = 0; level < config::kNumLevels; level++) {
    if (!compact_pointer_[level].empty()) {
      InternalKey key;
      key.DecodeFrom(compact_pointer_[level]);
      edit.SetCompactPointer(level, key);
    }
  }

  for (int level = 0; level < config::kNumLevels; level++) {
    const std::vector<FileMetaData*>& files = current_->files_[level];
    for (size_t i = 0; i < files.size(); i++) {
      const FileMetaData* f = files[i];
      edit.AddFile(level, f->number, f->file_size, f->smallest, f->largest);
    }
  }

  std::string record;
  edit.EncodeTo(&record);
  return log->AddRecord(record);
}

Status VersionSet::LogAndApply(VersionEdit* edit, std::mutex* mu)
{
  if (edit->has_log_number_) {
    assert(edit->log_number_ >= log_number_);
    assert(edit->log_number_ < next_file_number_);
  } else {
    edit->SetLogNumber(log_number_);
  }

  if (!edit->has_prev_log_number_) {
    edit->SetPrevLogNumber(prev_log_number_);
  }

  edit->SetNextFile(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  Version* v = new Version(this);
  {
    Builder builder(this, current_);
    builder.Apply(edit);
    builder.SaveTo(v);
  }
  Finalize(v);

  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == nullptr) {
    assert(descriptor_file_ == nullptr);
    new_manifest_file = DescriptorFileName(dbname_, manifest_file_number_);
    s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
    if (s.ok()) {
      descriptor_log_ = new log::Writer(descriptor_file_);
      s = WriteSnapshot(descriptor_log_);
    }
  }

  {
    mu->unlock();

    if (s.ok()) {
      std::string record;
      edit->EncodeTo(&record);
      s = descriptor_log_->AddRecord(record);
      if (s.ok()) {
        s = descriptor_file_->Sync();
      }
      if (!s.ok()) {
        Log(options_->info_log, "MANIFEST write: %s\n", s.ToString().c_str());
      }
    }

    if (s.ok() && !new_manifest_file.empty()) {
      s = SetCurrentFile(env_, dbname_, manifest_file_number_);
    }

    mu->lock();
  }

  if (s.ok()) {
    AppendVersion(v);
    log_number_ = edit->log_number_;
    prev_log_number_ = edit->prev_log_number_;
  } else {
    delete v;
    if (!new_manifest_file.empty()) {
      delete descriptor_file_;
      delete descriptor_log_;
      descriptor_file_ = nullptr;
      descriptor_log_ = nullptr;
      env_->RemoveFile(new_manifest_file);
    }
  }

  return s;
}
} // namespace LSMKV
