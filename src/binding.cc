#define NAPI_VERSION 3

#include "napi-macros.h"
#include <node_api.h>

#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>

#include <map>
#include <vector>

/**
 * Forward declarations.
 */
struct Database;
struct Iterator;
struct EndWorker;
static void iterator_end_do (napi_env env, Iterator* iterator, napi_value cb);

/**
 * Macros.
 */

#define NAPI_DB_CONTEXT() \
  Database* database = NULL; \
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], (void**)&database));

#define NAPI_ITERATOR_CONTEXT() \
  Iterator* iterator = NULL; \
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], (void**)&iterator));

#define NAPI_BATCH_CONTEXT() \
  Batch* batch = NULL; \
  NAPI_STATUS_THROWS(napi_get_value_external(env, argv[0], (void**)&batch));

#define NAPI_RETURN_UNDEFINED() \
  return 0;

#define NAPI_UTF8_NEW(name, val)                \
  size_t name##_size = 0;                                               \
  NAPI_STATUS_THROWS(napi_get_value_string_utf8(env, val, NULL, 0, &name##_size)) \
  char* name = new char[name##_size + 1];                               \
  NAPI_STATUS_THROWS(napi_get_value_string_utf8(env, val, name, name##_size + 1, &name##_size)) \
  name[name##_size] = '\0';

#define NAPI_ARGV_UTF8_NEW(name, i) \
  NAPI_UTF8_NEW(name, argv[i])

#define LD_STRING_OR_BUFFER_TO_COPY(env, from, to)                      \
  char* to##Ch_ = 0;                                                    \
  size_t to##Sz_ = 0;                                                   \
  if (IsString(env, from)) {                                            \
    napi_get_value_string_utf8(env, from, NULL, 0, &to##Sz_);           \
    to##Ch_ = new char[to##Sz_ + 1];                                    \
    napi_get_value_string_utf8(env, from, to##Ch_, to##Sz_ + 1, &to##Sz_); \
    to##Ch_[to##Sz_] = '\0';                                            \
  } else if (IsBuffer(env, from)) {                                     \
    char* buf = 0;                                                      \
    napi_get_buffer_info(env, from, (void **)&buf, &to##Sz_);           \
    to##Ch_ = new char[to##Sz_];                                        \
    memcpy(to##Ch_, buf, to##Sz_);                                      \
  }

/*********************************************************************
 * Helpers.
 ********************************************************************/

/**
 * Returns true if 'value' is a string.
 */
static bool IsString (napi_env env, napi_value value) {
  napi_valuetype type;
  napi_typeof(env, value, &type);
  return type == napi_string;
}

/**
 * Returns true if 'value' is a buffer.
 */
static bool IsBuffer (napi_env env, napi_value value) {
  bool isBuffer;
  napi_is_buffer(env, value, &isBuffer);
  return isBuffer;
}

/**
 * Returns true if 'value' is an object.
 */
static bool IsObject (napi_env env, napi_value value) {
  napi_valuetype type;
  napi_typeof(env, value, &type);
  return type == napi_object;
}

/**
 * Create an error object.
 */
static napi_value CreateError (napi_env env, const char* str) {
  napi_value msg;
  napi_create_string_utf8(env, str, strlen(str), &msg);
  napi_value error;
  napi_create_error(env, NULL, msg, &error);
  return error;
}

/**
 * Returns true if 'obj' has a property 'key'.
 */
static bool HasProperty (napi_env env, napi_value obj, const char* key) {
  bool has = false;
  napi_has_named_property(env, obj, key, &has);
  return has;
}

/**
 * Returns a property in napi_value form.
 */
static napi_value GetProperty (napi_env env, napi_value obj, const char* key) {
  napi_value value;
  napi_get_named_property(env, obj, key, &value);
  return value;
}

/**
 * Returns a boolean property 'key' from 'obj'.
 * Returns 'DEFAULT' if the property doesn't exist.
 */
static bool BooleanProperty (napi_env env, napi_value obj, const char* key,
                             bool DEFAULT) {
  if (HasProperty(env, obj, key)) {
    napi_value value = GetProperty(env, obj, key);
    bool result;
    napi_get_value_bool(env, value, &result);
    return result;
  }

  return DEFAULT;
}

/**
 * Returns a uint32 property 'key' from 'obj'.
 * Returns 'DEFAULT' if the property doesn't exist.
 */
static uint32_t Uint32Property (napi_env env, napi_value obj, const char* key,
                                uint32_t DEFAULT) {
  if (HasProperty(env, obj, key)) {
    napi_value value = GetProperty(env, obj, key);
    uint32_t result;
    napi_get_value_uint32(env, value, &result);
    return result;
  }

  return DEFAULT;
}

/**
 * Returns a uint32 property 'key' from 'obj'.
 * Returns 'DEFAULT' if the property doesn't exist.
 */
static int Int32Property (napi_env env, napi_value obj, const char* key,
                          int DEFAULT) {
  if (HasProperty(env, obj, key)) {
    napi_value value = GetProperty(env, obj, key);
    int result;
    napi_get_value_int32(env, value, &result);
    return result;
  }

  return DEFAULT;
}

/**
 * Returns a string property 'key' from 'obj'.
 * Returns empty string if the property doesn't exist.
 */
static std::string StringProperty (napi_env env, napi_value obj, const char* key) {
  if (HasProperty(env, obj, key)) {
    napi_value value = GetProperty(env, obj, key);
    if (IsString(env, value)) {
      size_t size = 0;
      napi_get_value_string_utf8(env, value, NULL, 0, &size);

      char* buf = new char[size + 1];
      napi_get_value_string_utf8(env, value, buf, size + 1, &size);
      buf[size] = '\0';

      std::string result = buf;
      delete [] buf;
      return result;
    }
  }

  return "";
}

static void DisposeSliceBuffer (leveldb::Slice slice) {
  if (!slice.empty()) delete [] slice.data();
}

/**
 * Convert a napi_value to a leveldb::Slice.
 */
static leveldb::Slice ToSlice (napi_env env, napi_value from) {
  LD_STRING_OR_BUFFER_TO_COPY(env, from, to);
  return leveldb::Slice(toCh_, toSz_);
}

/**
 * Returns length of string or buffer
 */
static size_t StringOrBufferLength (napi_env env, napi_value value) {
  size_t size = 0;

  if (IsString(env, value)) {
    napi_get_value_string_utf8(env, value, NULL, 0, &size);
  } else if (IsBuffer(env, value)) {
    char* buf;
    napi_get_buffer_info(env, value, (void **)&buf, &size);
  }

  return size;
}

/**
 * Calls a function.
 */
static napi_status CallFunction (napi_env env,
                                 napi_value callback,
                                 const int argc,
                                 napi_value* argv) {
  napi_value global;
  napi_get_global(env, &global);
  return napi_call_function(env, global, callback, argc, argv, NULL);
}

/**
 * Base worker class. Handles the async work.
 */
struct BaseWorker {
  BaseWorker (napi_env env,
              Database* database,
              napi_value callback,
              const char* resourceName)
    : env_(env), database_(database), errMsg_(NULL) {
    NAPI_STATUS_THROWS(napi_create_reference(env_, callback, 1, &callbackRef_));
    napi_value asyncResourceName;
    NAPI_STATUS_THROWS(napi_create_string_utf8(env_, resourceName,
                                               NAPI_AUTO_LENGTH,
                                               &asyncResourceName));
    NAPI_STATUS_THROWS(napi_create_async_work(env_, callback,
                                              asyncResourceName,
                                              BaseWorker::Execute,
                                              BaseWorker::Complete,
                                              this, &asyncWork_));
  }

  virtual ~BaseWorker () {
    delete [] errMsg_;
    napi_delete_reference(env_, callbackRef_);
    napi_delete_async_work(env_, asyncWork_);
  }

  static void Execute (napi_env env, void* data) {
    BaseWorker* self = (BaseWorker*)data;
    self->DoExecute();
  }

  void SetStatus (leveldb::Status status) {
    status_ = status;
    if (!status.ok()) {
      SetErrorMessage(status.ToString().c_str());
    }
  }

  void SetErrorMessage(const char *msg) {
    delete [] errMsg_;
    size_t size = strlen(msg) + 1;
    errMsg_ = new char[size];
    memcpy(errMsg_, msg, size);
  }

  virtual void DoExecute () = 0;

  static void Complete (napi_env env, napi_status status, void* data) {
    BaseWorker* self = (BaseWorker*)data;
    self->DoComplete();
    delete self;
  }

  void DoComplete () {
    if (status_.ok()) {
      return HandleOKCallback();
    }

    napi_value argv = CreateError(env_, errMsg_);
    napi_value callback;
    napi_get_reference_value(env_, callbackRef_, &callback);
    CallFunction(env_, callback, 1, &argv);
  }

  virtual void HandleOKCallback () {
    napi_value argv;
    napi_get_null(env_, &argv);
    napi_value callback;
    napi_get_reference_value(env_, callbackRef_, &callback);
    CallFunction(env_, callback, 1, &argv);
  }

  void Queue () {
    napi_queue_async_work(env_, asyncWork_);
  }

  napi_env env_;
  napi_ref callbackRef_;
  napi_async_work asyncWork_;
  Database* database_;

private:
  leveldb::Status status_;
  char *errMsg_;
};

/**
 * Owns the LevelDB storage, cache, filter policy and iterators.
 */
struct Database {
  Database (napi_env env)
    : env_(env),
      db_(NULL),
      blockCache_(NULL),
      filterPolicy_(leveldb::NewBloomFilterPolicy(10)),
      currentIteratorId_(0),
      pendingCloseWorker_(NULL) {}

  ~Database () {
    if (db_ != NULL) {
      delete db_;
      db_ = NULL;
    }
  }

  leveldb::Status Open (const leveldb::Options& options,
                        const char* location) {
    return leveldb::DB::Open(options, location, &db_);
  }

  void CloseDatabase () {
    delete db_;
    db_ = NULL;
    if (blockCache_) {
      delete blockCache_;
      blockCache_ = NULL;
    }
  }

  leveldb::Status Put (const leveldb::WriteOptions& options,
                       leveldb::Slice key,
                       leveldb::Slice value) {
    return db_->Put(options, key, value);
  }

  leveldb::Status Get (const leveldb::ReadOptions& options,
                       leveldb::Slice key,
                       std::string& value) {
    return db_->Get(options, key, &value);
  }

  leveldb::Status Del (const leveldb::WriteOptions& options,
                       leveldb::Slice key) {
    return db_->Delete(options, key);
  }

  leveldb::Status WriteBatch (const leveldb::WriteOptions& options,
                              leveldb::WriteBatch* batch) {
    return db_->Write(options, batch);
  }

  uint64_t ApproximateSize (const leveldb::Range* range) {
    uint64_t size = 0;
    db_->GetApproximateSizes(range, 1, &size);
    return size;
  }

  void CompactRange (const leveldb::Slice* start,
                     const leveldb::Slice* end) {
    db_->CompactRange(start, end);
  }

  void GetProperty (const leveldb::Slice& property, std::string* value) {
    db_->GetProperty(property, value);
  }

  const leveldb::Snapshot* NewSnapshot () {
    return db_->GetSnapshot();
  }

  leveldb::Iterator* NewIterator (leveldb::ReadOptions* options) {
    return db_->NewIterator(*options);
  }

  void ReleaseSnapshot (const leveldb::Snapshot* snapshot) {
    return db_->ReleaseSnapshot(snapshot);
  }

  void ReleaseIterator (uint32_t id) {
    iterators_.erase(id);
    if (iterators_.empty() && pendingCloseWorker_ != NULL) {
      pendingCloseWorker_->Queue();
      pendingCloseWorker_ = NULL;
    }
  }

  napi_env env_;
  leveldb::DB* db_;
  leveldb::Cache* blockCache_;
  const leveldb::FilterPolicy* filterPolicy_;
  uint32_t currentIteratorId_;
  BaseWorker *pendingCloseWorker_;
  std::map< uint32_t, Iterator * > iterators_;
};

/**
 * Runs when a Database is garbage collected.
 */
static void FinalizeDatabase (napi_env env, void* data, void* hint) {
  if (data) {
    delete (Database*)data;
  }
}

/**
 * Owns a leveldb iterator.
 */
struct Iterator {
  Iterator (Database* database,
            uint32_t id,
            leveldb::Slice* start,
            std::string* end,
            bool reverse,
            bool keys,
            bool values,
            int limit,
            std::string* lt,
            std::string* lte,
            std::string* gt,
            std::string* gte,
            bool fillCache,
            bool keyAsBuffer,
            bool valueAsBuffer,
            uint32_t highWaterMark)
    : database_(database),
      id_(id),
      start_(start),
      end_(end),
      reverse_(reverse),
      keys_(keys),
      values_(values),
      limit_(limit),
      lt_(lt),
      lte_(lte),
      gt_(gt),
      gte_(gte),
      keyAsBuffer_(keyAsBuffer),
      valueAsBuffer_(valueAsBuffer),
      highWaterMark_(highWaterMark),
      dbIterator_(NULL),
      count_(0),
      target_(NULL),
      seeking_(false),
      landed_(false),
      nexting_(false),
      ended_(false),
      endWorker_(NULL) {
    options_ = new leveldb::ReadOptions();
    options_->fill_cache = fillCache;
    options_->snapshot = database->NewSnapshot();
  }

  ~Iterator () {
    ReleaseTarget();
    if (start_ != NULL) {
      // Special case for `start` option: it won't be
      // freed up by any of the delete calls below.
      if (!((lt_ != NULL && reverse_)
            || (lte_ != NULL && reverse_)
            || (gt_ != NULL && !reverse_)
            || (gte_ != NULL && !reverse_))) {
        delete [] start_->data();
      }
      delete start_;
    }
    if (end_ != NULL) {
      delete end_;
    }
    if (lt_ != NULL) {
      delete lt_;
    }
    if (gt_ != NULL) {
      delete gt_;
    }
    if (lte_ != NULL) {
      delete lte_;
    }
    if (gte_ != NULL) {
      delete gte_;
    }
  }

  void ReleaseTarget () {
    if (target_ != NULL) {
      if (!target_->empty()) {
        delete [] target_->data();
      }
      delete target_;
      target_ = NULL;
    }
  }

  void Release () {
    database_->ReleaseIterator(id_);
  }

  leveldb::Status IteratorStatus () {
    return dbIterator_->status();
  }

  void IteratorEnd () {
    delete dbIterator_;
    dbIterator_ = NULL;
    database_->ReleaseSnapshot(options_->snapshot);
  }

  bool GetIterator () {
    if (dbIterator_ != NULL) return false;

    dbIterator_ = database_->NewIterator(options_);

    if (start_ != NULL) {
      dbIterator_->Seek(*start_);

      if (reverse_) {
        if (!dbIterator_->Valid()) {
          dbIterator_->SeekToLast();
        } else {
          std::string keyStr = dbIterator_->key().ToString();

          if (lt_ != NULL) {
            if (lt_->compare(keyStr) <= 0)
              dbIterator_->Prev();
          } else if (lte_ != NULL) {
            if (lte_->compare(keyStr) < 0)
              dbIterator_->Prev();
          } else if (start_ != NULL) {
            if (start_->compare(keyStr))
              dbIterator_->Prev();
          }
        }

        if (dbIterator_->Valid() && lt_ != NULL) {
          if (lt_->compare(dbIterator_->key().ToString()) <= 0)
            dbIterator_->Prev();
        }
      } else {
        if (dbIterator_->Valid() && gt_ != NULL
            && gt_->compare(dbIterator_->key().ToString()) == 0)
          dbIterator_->Next();
      }
    } else if (reverse_) {
      dbIterator_->SeekToLast();
    } else {
      dbIterator_->SeekToFirst();
    }

    return true;
  }

  bool Read (std::string& key, std::string& value) {
    if (!GetIterator() && !seeking_) {
      if (reverse_) {
        dbIterator_->Prev();
      }
      else {
        dbIterator_->Next();
      }
    }

    seeking_ = false;

    if (dbIterator_->Valid()) {
      std::string keyStr = dbIterator_->key().ToString();
      const int isEnd = end_ == NULL ? 1 : end_->compare(keyStr);

      if ((limit_ < 0 || ++count_ <= limit_)
          && (end_ == NULL
              || (reverse_ && (isEnd <= 0))
              || (!reverse_ && (isEnd >= 0)))
          && ( lt_  != NULL ? (lt_->compare(keyStr) > 0)
               : lte_ != NULL ? (lte_->compare(keyStr) >= 0)
               : true )
          && ( gt_  != NULL ? (gt_->compare(keyStr) < 0)
               : gte_ != NULL ? (gte_->compare(keyStr) <= 0)
               : true )
          ) {
        if (keys_) {
          key.assign(dbIterator_->key().data(), dbIterator_->key().size());
        }
        if (values_) {
          value.assign(dbIterator_->value().data(), dbIterator_->value().size());
        }
        return true;
      }
    }

    return false;
  }

  bool OutOfRange (leveldb::Slice* target) {
    if ((lt_ != NULL && target->compare(*lt_) >= 0) ||
        (lte_ != NULL && target->compare(*lte_) > 0) ||
        (start_ != NULL && reverse_ && target->compare(*start_) > 0)) {
      return true;
    }

    if (end_ != NULL) {
      int d = target->compare(*end_);
      if (reverse_ ? d < 0 : d > 0) return true;
    }

    return ((gt_ != NULL && target->compare(*gt_) <= 0) ||
            (gte_ != NULL && target->compare(*gte_) < 0) ||
            (start_ != NULL && !reverse_ && target->compare(*start_) < 0));
  }

  bool IteratorNext (std::vector<std::pair<std::string, std::string> >& result) {
    size_t size = 0;
    while (true) {
      std::string key, value;
      bool ok = Read(key, value);

      if (ok) {
        result.push_back(std::make_pair(key, value));

        if (!landed_) {
          landed_ = true;
          return true;
        }

        size = size + key.size() + value.size();
        if (size > highWaterMark_) return true;

      } else {
        return false;
      }
    }
  }

  Database* database_;
  uint32_t id_;
  leveldb::Slice* start_;
  std::string* end_;
  bool reverse_;
  bool keys_;
  bool values_;
  int limit_;
  std::string* lt_;
  std::string* lte_;
  std::string* gt_;
  std::string* gte_;
  bool keyAsBuffer_;
  bool valueAsBuffer_;
  uint32_t highWaterMark_;
  leveldb::Iterator* dbIterator_;
  int count_;
  leveldb::Slice* target_;
  bool seeking_;
  bool landed_;
  bool nexting_;
  bool ended_;

  leveldb::ReadOptions* options_;
  EndWorker* endWorker_;
};

/**
 * Returns a context object for a database.
 */
NAPI_METHOD(db_init) {
  Database* database = new Database(env);

  napi_value result;
  NAPI_STATUS_THROWS(napi_create_external(env, database,
                                          FinalizeDatabase,
                                          NULL, &result));
  return result;
}

/**
 * Worker class for opening a database.
 */
struct OpenWorker : public BaseWorker {
  OpenWorker (napi_env env,
              Database* database,
              napi_value callback,
              const std::string& location,
              bool createIfMissing,
              bool errorIfExists,
              bool compression,
              uint32_t writeBufferSize,
              uint32_t blockSize,
              uint32_t maxOpenFiles,
              uint32_t blockRestartInterval,
              uint32_t maxFileSize)
    : BaseWorker(env, database, callback, "leveldown.db.open"),
      location_(location) {
    options_.block_cache = database->blockCache_;
    options_.filter_policy = database->filterPolicy_;
    options_.create_if_missing = createIfMissing;
    options_.error_if_exists = errorIfExists;
    options_.compression = compression
      ? leveldb::kSnappyCompression
      : leveldb::kNoCompression;
    options_.write_buffer_size = writeBufferSize;
    options_.block_size = blockSize;
    options_.max_open_files = maxOpenFiles;
    options_.block_restart_interval = blockRestartInterval;
    options_.max_file_size = maxFileSize;
  }

  virtual ~OpenWorker () {}

  virtual void DoExecute () {
    SetStatus(database_->Open(options_, location_.c_str()));
  }

  leveldb::Options options_;
  std::string location_;
};

/**
 * Open a database.
 */
NAPI_METHOD(db_open) {
  NAPI_ARGV(4);
  NAPI_DB_CONTEXT();
  NAPI_ARGV_UTF8_NEW(location, 1);

  napi_value options = argv[2];
  bool createIfMissing = BooleanProperty(env, options, "createIfMissing", true);
  bool errorIfExists = BooleanProperty(env, options, "errorIfExists", false);
  bool compression = BooleanProperty(env, options, "compression", true);

  uint32_t cacheSize = Uint32Property(env, options, "cacheSize", 8 << 20);
  uint32_t writeBufferSize = Uint32Property(env, options , "writeBufferSize" , 4 << 20);
  uint32_t blockSize = Uint32Property(env, options, "blockSize", 4096);
  uint32_t maxOpenFiles = Uint32Property(env, options, "maxOpenFiles", 1000);
  uint32_t blockRestartInterval = Uint32Property(env, options,
                                                 "blockRestartInterval", 16);
  uint32_t maxFileSize = Uint32Property(env, options, "maxFileSize", 2 << 20);

  database->blockCache_ = leveldb::NewLRUCache(cacheSize);

  napi_value callback = argv[3];
  OpenWorker* worker = new OpenWorker(env, database, callback, location,
                                      createIfMissing, errorIfExists,
                                      compression, writeBufferSize, blockSize,
                                      maxOpenFiles, blockRestartInterval,
                                      maxFileSize);
  worker->Queue();
  delete [] location;

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for closing a database
 */
struct CloseWorker : public BaseWorker {
  CloseWorker (napi_env env,
               Database* database,
               napi_value callback)
    : BaseWorker(env, database, callback, "leveldown.db.close") {}

  virtual ~CloseWorker () {}

  virtual void DoExecute () {
    database_->CloseDatabase();
  }
};

napi_value noop_callback (napi_env env, napi_callback_info info) {
  return 0;
}

/**
 * Close a database.
 */
NAPI_METHOD(db_close) {
  NAPI_ARGV(2);
  NAPI_DB_CONTEXT();

  napi_value callback = argv[1];
  CloseWorker* worker = new CloseWorker(env, database, callback);

  if (database->iterators_.empty()) {
    worker->Queue();
    NAPI_RETURN_UNDEFINED();
  }

  database->pendingCloseWorker_ = worker;

  napi_value noop;
  napi_create_function(env, NULL, 0, noop_callback, NULL, &noop);

  std::map< uint32_t, Iterator * >::iterator it;
  for (it = database->iterators_.begin();
       it != database->iterators_.end(); ++it) {
    Iterator *iterator = it->second;
    if (!iterator->ended_) {
      iterator_end_do(env, iterator, noop);
    }
  }

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for putting key/value to the database
 */
struct PutWorker : public BaseWorker {
  PutWorker (napi_env env,
             Database* database,
             napi_value callback,
             leveldb::Slice key,
             leveldb::Slice value,
             bool sync)
    : BaseWorker(env, database, callback, "leveldown.db.put"),
      key_(key), value_(value) {
    options_.sync = sync;
  }

  virtual ~PutWorker () {
    DisposeSliceBuffer(key_);
    DisposeSliceBuffer(value_);
  }

  virtual void DoExecute () {
    SetStatus(database_->Put(options_, key_, value_));
  }

  leveldb::WriteOptions options_;
  leveldb::Slice key_;
  leveldb::Slice value_;
};

/**
 * Puts a key and a value to a database.
 */
NAPI_METHOD(db_put) {
  NAPI_ARGV(5);
  NAPI_DB_CONTEXT();

  leveldb::Slice key = ToSlice(env, argv[1]);
  leveldb::Slice value = ToSlice(env, argv[2]);
  bool sync = BooleanProperty(env, argv[3], "sync", false);
  napi_value callback = argv[4];

  PutWorker* worker = new PutWorker(env, database, callback, key, value, sync);
  worker->Queue();

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for getting a value from a database.
 */
struct GetWorker : public BaseWorker {
  GetWorker (napi_env env,
             Database* database,
             napi_value callback,
             leveldb::Slice key,
             bool asBuffer,
             bool fillCache)
    : BaseWorker(env, database, callback, "leveldown.db.get"),
      key_(key),
      asBuffer_(asBuffer) {
    options_.fill_cache = fillCache;
  }

  virtual ~GetWorker () {
    DisposeSliceBuffer(key_);
  }

  virtual void DoExecute () {
    SetStatus(database_->Get(options_, key_, value_));
  }

  virtual void HandleOKCallback() {
    napi_value argv[2];
    napi_get_null(env_, &argv[0]);

    if (asBuffer_) {
      napi_create_buffer_copy(env_, value_.size(), value_.data(), NULL, &argv[1]);
    } else {
      napi_create_string_utf8(env_, value_.data(), value_.size(), &argv[1]);
    }

    napi_value callback;
    napi_get_reference_value(env_, callbackRef_, &callback);
    CallFunction(env_, callback, 2, argv);
  }

  leveldb::ReadOptions options_;
  leveldb::Slice key_;
  std::string value_;
  bool asBuffer_;
};

/**
 * Gets a value from a database.
 */
NAPI_METHOD(db_get) {
  NAPI_ARGV(4);
  NAPI_DB_CONTEXT();

  leveldb::Slice key = ToSlice(env, argv[1]);
  napi_value options = argv[2];
  bool asBuffer = BooleanProperty(env, options, "asBuffer", true);
  bool fillCache = BooleanProperty(env, options, "fillCache", true);
  napi_value callback = argv[3];

  GetWorker* worker = new GetWorker(env, database, callback, key, asBuffer,
                                    fillCache);
  worker->Queue();

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for deleting a value from a database.
 */
struct DelWorker : public BaseWorker {
  DelWorker (napi_env env,
             Database* database,
             napi_value callback,
             leveldb::Slice key,
             bool sync)
    : BaseWorker(env, database, callback, "leveldown.db.del"),
      key_(key) {
    options_.sync = sync;
  }

  virtual ~DelWorker () {
    DisposeSliceBuffer(key_);
  }

  virtual void DoExecute () {
    SetStatus(database_->Del(options_, key_));
  }

  leveldb::WriteOptions options_;
  leveldb::Slice key_;
};

/**
 * Delete a value from a database.
 */
NAPI_METHOD(db_del) {
  NAPI_ARGV(4);
  NAPI_DB_CONTEXT();

  leveldb::Slice key = ToSlice(env, argv[1]);
  bool sync = BooleanProperty(env, argv[2], "sync", false);
  napi_value callback = argv[3];

  DelWorker* worker = new DelWorker(env, database, callback, key, sync);
  worker->Queue();

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for calculating the size of a range.
 */
struct ApproximateSizeWorker : public BaseWorker {
  ApproximateSizeWorker (napi_env env,
                         Database* database,
                         napi_value callback,
                         leveldb::Slice start,
                         leveldb::Slice end)
    : BaseWorker(env, database, callback, "leveldown.db.approximate_size"),
      start_(start), end_(end) {}

  virtual ~ApproximateSizeWorker () {
    DisposeSliceBuffer(start_);
    DisposeSliceBuffer(end_);
  }

  virtual void DoExecute () {
    leveldb::Range range(start_, end_);
    size_ = database_->ApproximateSize(&range);
  }

  virtual void HandleOKCallback() {
    napi_value argv[2];
    napi_get_null(env_, &argv[0]);
    napi_create_uint32(env_, (uint32_t)size_, &argv[1]);
    napi_value callback;
    napi_get_reference_value(env_, callbackRef_, &callback);
    CallFunction(env_, callback, 2, argv);
  }

  leveldb::Slice start_;
  leveldb::Slice end_;
  uint64_t size_;
};

/**
 * Calculates the approximate size of a range in a database.
 */
NAPI_METHOD(db_approximate_size) {
  NAPI_ARGV(4);
  NAPI_DB_CONTEXT();

  leveldb::Slice start = ToSlice(env, argv[1]);
  leveldb::Slice end = ToSlice(env, argv[2]);

  napi_value callback = argv[3];

  ApproximateSizeWorker* worker  = new ApproximateSizeWorker(env, database,
                                                             callback, start,
                                                             end);
  worker->Queue();

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for compacting a range in a database.
 */
struct CompactRangeWorker : public BaseWorker {
  CompactRangeWorker (napi_env env,
                      Database* database,
                      napi_value callback,
                      leveldb::Slice start,
                      leveldb::Slice end)
    : BaseWorker(env, database, callback, "leveldown.db.compact_range"),
      start_(start), end_(end) {}

  virtual ~CompactRangeWorker () {
    DisposeSliceBuffer(start_);
    DisposeSliceBuffer(end_);
  }

  virtual void DoExecute () {
    database_->CompactRange(&start_, &end_);
  }

  leveldb::Slice start_;
  leveldb::Slice end_;
};

/**
 * Compacts a range in a database.
 */
NAPI_METHOD(db_compact_range) {
  NAPI_ARGV(4);
  NAPI_DB_CONTEXT();

  leveldb::Slice start = ToSlice(env, argv[1]);
  leveldb::Slice end = ToSlice(env, argv[2]);
  napi_value callback = argv[3];

  CompactRangeWorker* worker  = new CompactRangeWorker(env, database, callback,
                                                       start, end);
  worker->Queue();

  NAPI_RETURN_UNDEFINED();
}

/**
 * Get a property from a database.
 */
NAPI_METHOD(db_get_property) {
  NAPI_ARGV(2);
  NAPI_DB_CONTEXT();

  leveldb::Slice property = ToSlice(env, argv[1]);

  std::string value;
  database->GetProperty(property, &value);

  napi_value result;
  napi_create_string_utf8(env, value.data(), value.size(), &result);

  DisposeSliceBuffer(property);

  return result;
}

/**
 * Worker class for destroying a database.
 */
struct DestroyWorker : public BaseWorker {
  DestroyWorker (napi_env env,
                 const std::string& location,
                 napi_value callback)
    : BaseWorker(env, NULL, callback, "leveldown.destroy_db"),
      location_(location) {}

  virtual ~DestroyWorker () {}

  virtual void DoExecute () {
    leveldb::Options options;
    SetStatus(leveldb::DestroyDB(location_, options));
  }

  std::string location_;
};

/**
 * Destroys a database.
 */
NAPI_METHOD(destroy_db) {
  NAPI_ARGV(2);
  NAPI_ARGV_UTF8_NEW(location, 0);
  napi_value callback = argv[1];

  DestroyWorker* worker = new DestroyWorker(env, location, callback);
  worker->Queue();

  delete [] location;

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for repairing a database.
 */
struct RepairWorker : public BaseWorker {
  RepairWorker (napi_env env,
                const std::string& location,
                napi_value callback)
    : BaseWorker(env, NULL, callback, "leveldown.repair_db"),
      location_(location) {}

  virtual ~RepairWorker () {}

  virtual void DoExecute () {
    leveldb::Options options;
    SetStatus(leveldb::RepairDB(location_, options));
  }

  std::string location_;
};

/**
 * Repairs a database.
 */
NAPI_METHOD(repair_db) {
  NAPI_ARGV(2);
  NAPI_ARGV_UTF8_NEW(location, 0);
  napi_value callback = argv[1];

  RepairWorker* worker = new RepairWorker(env, location, callback);
  worker->Queue();

  delete [] location;

  NAPI_RETURN_UNDEFINED();
}

/**
 * Runs when an Iterator is garbage collected.
 */
static void FinalizeIterator (napi_env env, void* data, void* hint) {
  if (data) {
    delete (Iterator*)data;
  }
}

#define CHECK_PROPERTY(name, code)                                      \
  if (HasProperty(env, options, #name)) {                               \
    napi_value value = GetProperty(env, options, #name);                \
    if (IsString(env, value) || IsBuffer(env, value)) {                 \
      if (StringOrBufferLength(env, value) > 0) {                       \
        LD_STRING_OR_BUFFER_TO_COPY(env, value, _##name);               \
        code;                                                           \
      }                                                                 \
    }                                                                   \
  }                                                                     \

/**
 * Create an iterator.
 */
NAPI_METHOD(iterator_init) {
  NAPI_ARGV(2);
  NAPI_DB_CONTEXT();

  napi_value options = argv[1];
  bool reverse = BooleanProperty(env, options, "reverse", false);
  bool keys = BooleanProperty(env, options, "keys", true);
  bool values = BooleanProperty(env, options, "values", true);
  bool fillCache = BooleanProperty(env, options, "fillCache", false);
  bool keyAsBuffer = BooleanProperty(env, options, "keyAsBuffer", true);
  bool valueAsBuffer = BooleanProperty(env, options, "valueAsBuffer", true);
  int limit = Int32Property(env, options, "limit", -1);
  uint32_t highWaterMark = Uint32Property(env, options, "highWaterMark",
                                          16 * 1024);

  // TODO simplify and refactor the hideous code below

  leveldb::Slice* start = NULL;
  char *startStr = NULL;
  CHECK_PROPERTY(start, {
    start = new leveldb::Slice(_startCh_, _startSz_);
    startStr = _startCh_;
  });

  std::string* end = NULL;
  CHECK_PROPERTY(end, {
    end = new std::string(_endCh_, _endSz_);
    delete [] _endCh_;
  });

  std::string* lt = NULL;
  CHECK_PROPERTY(lt, {
    lt = new std::string(_ltCh_, _ltSz_);
    delete [] _ltCh_;
    if (reverse) {
      if (startStr != NULL) {
        delete [] startStr;
        startStr = NULL;
      }
      if (start != NULL) {
        delete start;
      }
      start = new leveldb::Slice(lt->data(), lt->size());
    }
  });

  std::string* lte = NULL;
  CHECK_PROPERTY(lte, {
    lte = new std::string(_lteCh_, _lteSz_);
    delete [] _lteCh_;
    if (reverse) {
      if (startStr != NULL) {
        delete [] startStr;
        startStr = NULL;
      }
      if (start != NULL) {
        delete start;
      }
      start = new leveldb::Slice(lte->data(), lte->size());
    }
  });

  std::string* gt = NULL;
  CHECK_PROPERTY(gt, {
    gt = new std::string(_gtCh_, _gtSz_);
    delete [] _gtCh_;
    if (!reverse) {
      if (startStr != NULL) {
        delete [] startStr;
        startStr = NULL;
      }
      if (start != NULL) {
        delete start;
      }
      start = new leveldb::Slice(gt->data(), gt->size());
    }
  });

  std::string* gte = NULL;
  CHECK_PROPERTY(gte, {
    gte = new std::string(_gteCh_, _gteSz_);
    delete [] _gteCh_;
    if (!reverse) {
      if (startStr != NULL) {
        delete [] startStr;
        startStr = NULL;
      }
      if (start != NULL) {
        delete start;
      }
      start = new leveldb::Slice(gte->data(), gte->size());
    }
  });

  uint32_t id = database->currentIteratorId_++;
  Iterator* iterator = new Iterator(database, id, start, end, reverse, keys,
                                    values, limit, lt, lte, gt, gte, fillCache,
                                    keyAsBuffer, valueAsBuffer, highWaterMark);
  database->iterators_[id] = iterator;

  napi_value result;
  NAPI_STATUS_THROWS(napi_create_external(env, iterator,
                                          FinalizeIterator,
                                          NULL, &result));
  return result;
}

/**
 * Seeks an iterator.
 */
NAPI_METHOD(iterator_seek) {
  NAPI_ARGV(2);
  NAPI_ITERATOR_CONTEXT();

  iterator->ReleaseTarget();
  iterator->target_ = new leveldb::Slice(ToSlice(env, argv[1]));
  iterator->GetIterator();

  leveldb::Iterator* dbIterator = iterator->dbIterator_;
  dbIterator->Seek(*iterator->target_);

  iterator->seeking_ = true;
  iterator->landed_ = false;

  if (iterator->OutOfRange(iterator->target_)) {
    if (iterator->reverse_) {
      dbIterator->SeekToFirst();
      dbIterator->Prev();
    } else {
      dbIterator->SeekToLast();
      dbIterator->Next();
    }
  }
  else if (dbIterator->Valid()) {
    int cmp = dbIterator->key().compare(*iterator->target_);
    if (cmp > 0 && iterator->reverse_) {
      dbIterator->Prev();
    } else if (cmp < 0 && !iterator->reverse_) {
      dbIterator->Next();
    }
  } else {
    if (iterator->reverse_) {
      dbIterator->SeekToLast();
    } else {
      dbIterator->SeekToFirst();
    }
    if (dbIterator->Valid()) {
      int cmp = dbIterator->key().compare(*iterator->target_);
      if (cmp > 0 && iterator->reverse_) {
        dbIterator->SeekToFirst();
        dbIterator->Prev();
      } else if (cmp < 0 && !iterator->reverse_) {
        dbIterator->SeekToLast();
        dbIterator->Next();
      }
    }
  }

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for ending an iterator
 */
struct EndWorker : public BaseWorker {
  EndWorker (napi_env env,
             Iterator* iterator,
             napi_value callback)
    : BaseWorker(env, iterator->database_, callback, "leveldown.iterator.end"),
      iterator_(iterator) {}

  virtual ~EndWorker () {}

  virtual void DoExecute () {
    iterator_->IteratorEnd();
  }

  virtual void HandleOKCallback () {
    iterator_->Release();
    BaseWorker::HandleOKCallback();
  }

  Iterator* iterator_;
};

/**
 * Called by NAPI_METHOD(iterator_end) and also when closing
 * open iterators during NAPI_METHOD(db_close).
 */
static void iterator_end_do (napi_env env, Iterator* iterator, napi_value cb) {
  if (!iterator->ended_) {
    EndWorker* worker = new EndWorker(env, iterator, cb);
    iterator->ended_ = true;

    if (iterator->nexting_) {
      iterator->endWorker_ = worker;
    } else {
      worker->Queue();
    }
  }
}

/**
 * Ends an iterator.
 */
NAPI_METHOD(iterator_end) {
  NAPI_ARGV(2);
  NAPI_ITERATOR_CONTEXT();

  iterator_end_do(env, iterator, argv[1]);

  NAPI_RETURN_UNDEFINED();
}

/**
 * TODO Move this to Iterator. There isn't any reason
 * for this function being a separate function pointer.
 */
void CheckEndCallback (Iterator* iterator) {
  iterator->ReleaseTarget();
  iterator->nexting_ = false;
  if (iterator->endWorker_ != NULL) {
    iterator->endWorker_->Queue();
    iterator->endWorker_ = NULL;
  }
}

/**
 * Worker class for nexting an iterator.
 */
struct NextWorker : public BaseWorker {
  NextWorker (napi_env env,
              Iterator* iterator,
              napi_value callback,
              void (*localCallback)(Iterator*))
    : BaseWorker(env, iterator->database_, callback,
                 "leveldown.iterator.next"),
      iterator_(iterator),
      localCallback_(localCallback) {}

  virtual ~NextWorker () {}

  virtual void DoExecute () {
    ok_ = iterator_->IteratorNext(result_);
    if (!ok_) {
      SetStatus(iterator_->IteratorStatus());
    }
  }

  virtual void HandleOKCallback () {
    size_t arraySize = result_.size() * 2;
    napi_value jsArray;
    napi_create_array_with_length(env_, arraySize, &jsArray);

    for (size_t idx = 0; idx < result_.size(); ++idx) {
      std::pair<std::string, std::string> row = result_[idx];
      std::string key = row.first;
      std::string value = row.second;

      napi_value returnKey;
      if (iterator_->keyAsBuffer_) {
        napi_create_buffer_copy(env_, key.size(), key.data(), NULL, &returnKey);
      } else {
        napi_create_string_utf8(env_, key.data(), key.size(), &returnKey);
      }

      napi_value returnValue;
      if (iterator_->valueAsBuffer_) {
        napi_create_buffer_copy(env_, value.size(), value.data(), NULL, &returnValue);
      } else {
        napi_create_string_utf8(env_, value.data(), value.size(), &returnValue);
      }

      // put the key & value in a descending order, so that they can be .pop:ed in javascript-land
      napi_set_element(env_, jsArray, static_cast<int>(arraySize - idx * 2 - 1), returnKey);
      napi_set_element(env_, jsArray, static_cast<int>(arraySize - idx * 2 - 2), returnValue);
    }

    // clean up & handle the next/end state
    // TODO this should just do iterator_->CheckEndCallback();
    localCallback_(iterator_);

    napi_value argv[3];
    napi_get_null(env_, &argv[0]);
    argv[1] = jsArray;
    napi_get_boolean(env_, !ok_, &argv[2]);
    napi_value callback;
    napi_get_reference_value(env_, callbackRef_, &callback);
    CallFunction(env_, callback, 3, argv);
  }

  Iterator* iterator_;
  // TODO why do we need a function pointer for this?
  void (*localCallback_)(Iterator*);
  std::vector<std::pair<std::string, std::string> > result_;
  bool ok_;
};

/**
 * Moves an iterator to next element.
 */
NAPI_METHOD(iterator_next) {
  NAPI_ARGV(2);
  NAPI_ITERATOR_CONTEXT();

  napi_value callback = argv[1];

  if (iterator->ended_) {
    napi_value argv = CreateError(env, "iterator has ended");
    CallFunction(env, callback, 1, &argv);

    NAPI_RETURN_UNDEFINED();
  }

  NextWorker* worker = new NextWorker(env, iterator, callback,
                                      CheckEndCallback);
  iterator->nexting_ = true;
  worker->Queue();

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for batch write operation.
 */
struct BatchWorker : public BaseWorker {
  BatchWorker (napi_env env,
               Database* database,
               napi_value callback,
               leveldb::WriteBatch* batch,
               bool sync)
    : BaseWorker(env, database, callback, "leveldown.batch.do"),
      batch_(batch) {
    options_.sync = sync;
  }

  virtual ~BatchWorker () {
    delete batch_;
  }

  virtual void DoExecute () {
    SetStatus(database_->WriteBatch(options_, batch_));
  }

  leveldb::WriteOptions options_;
  leveldb::WriteBatch* batch_;
};

/**
 * Does a batch write operation on a database.
 */
NAPI_METHOD(batch_do) {
  NAPI_ARGV(4);
  NAPI_DB_CONTEXT();

  napi_value array = argv[1];
  bool sync = BooleanProperty(env, argv[2], "sync", false);
  napi_value callback = argv[3];

  uint32_t length;
  napi_get_array_length(env, array, &length);

  leveldb::WriteBatch* batch = new leveldb::WriteBatch();
  bool hasData = false;

  for (uint32_t i = 0; i < length; i++) {
    napi_value element;
    napi_get_element(env, array, i, &element);

    if (!IsObject(env, element)) continue;

    std::string type = StringProperty(env, element, "type");

    if (type == "del") {
      if (!HasProperty(env, element, "key")) continue;
      leveldb::Slice key = ToSlice(env, GetProperty(env, element, "key"));

      batch->Delete(key);
      if (!hasData) hasData = true;

      DisposeSliceBuffer(key);
    } else if (type == "put") {
      if (!HasProperty(env, element, "key")) continue;
      if (!HasProperty(env, element, "value")) continue;

      leveldb::Slice key = ToSlice(env, GetProperty(env, element, "key"));
      leveldb::Slice value = ToSlice(env, GetProperty(env, element, "value"));

      batch->Put(key, value);
      if (!hasData) hasData = true;

      DisposeSliceBuffer(key);
      DisposeSliceBuffer(value);
    }
  }

  if (hasData) {
    BatchWorker* worker = new BatchWorker(env, database, callback, batch, sync);
    worker->Queue();
  } else {
    delete batch;
    napi_value argv;
    napi_get_null(env, &argv);
    CallFunction(env, callback, 1, &argv);
  }

  NAPI_RETURN_UNDEFINED();
}

/**
 * Owns a WriteBatch.
 */
struct Batch {
  Batch (Database* database)
    : database_(database),
      batch_(new leveldb::WriteBatch()),
      hasData_(false) {}

  ~Batch () {
    delete batch_;
  }

  void Put (leveldb::Slice key, leveldb::Slice value) {
    batch_->Put(key, value);
    hasData_ = true;
  }

  void Del (leveldb::Slice key) {
    batch_->Delete(key);
    hasData_ = true;
  }

  void Clear () {
    batch_->Clear();
    hasData_ = false;
  }

  leveldb::Status Write (bool sync) {
    leveldb::WriteOptions options;
    options.sync = sync;
    return database_->WriteBatch(options, batch_);
  }

  Database* database_;
  leveldb::WriteBatch* batch_;
  bool hasData_;
};

/**
 * Runs when a Batch is garbage collected.
 */
static void FinalizeBatch (napi_env env, void* data, void* hint) {
  if (data) {
    delete (Batch*)data;
  }
}

/**
 * Return a batch object.
 */
NAPI_METHOD(batch_init) {
  NAPI_ARGV(1);
  NAPI_DB_CONTEXT();

  Batch* batch = new Batch(database);

  napi_value result;
  NAPI_STATUS_THROWS(napi_create_external(env, batch,
                                          FinalizeBatch,
                                          NULL, &result));
  return result;
}

/**
 * Adds a put instruction to a batch object.
 */
NAPI_METHOD(batch_put) {
  NAPI_ARGV(3);
  NAPI_BATCH_CONTEXT();

  leveldb::Slice key = ToSlice(env, argv[1]);
  leveldb::Slice value = ToSlice(env, argv[2]);
  batch->Put(key, value);
  DisposeSliceBuffer(key);
  DisposeSliceBuffer(value);

  NAPI_RETURN_UNDEFINED();
}

/**
 * Adds a delete instruction to a batch object.
 */
NAPI_METHOD(batch_del) {
  NAPI_ARGV(2);
  NAPI_BATCH_CONTEXT();

  leveldb::Slice key = ToSlice(env, argv[1]);
  batch->Del(key);
  DisposeSliceBuffer(key);

  NAPI_RETURN_UNDEFINED();
}

/**
 * Clears a batch object.
 */
NAPI_METHOD(batch_clear) {
  NAPI_ARGV(1);
  NAPI_BATCH_CONTEXT();

  batch->Clear();

  NAPI_RETURN_UNDEFINED();
}

/**
 * Worker class for batch write operation.
 */
struct BatchWriteWorker : public BaseWorker {
  BatchWriteWorker (napi_env env,
                    Batch* batch,
                    napi_value callback,
                    bool sync)
    : BaseWorker(env, batch->database_, callback, "leveldown.batch.write"),
      batch_(batch),
      sync_(sync) {}

  virtual ~BatchWriteWorker () {}

  virtual void DoExecute () {
    SetStatus(batch_->Write(sync_));
  }

  Batch* batch_;
  bool sync_;
};

/**
 * Writes a batch object.
 */
NAPI_METHOD(batch_write) {
  NAPI_ARGV(3);
  NAPI_BATCH_CONTEXT();

  napi_value options = argv[1];
  bool sync = BooleanProperty(env, options, "sync", false);
  napi_value callback = argv[2];

  BatchWriteWorker* worker  = new BatchWriteWorker(env, batch, callback, sync);
  worker->Queue();

  NAPI_RETURN_UNDEFINED();
}

/**
 * All exported functions.
 */
NAPI_INIT() {
  NAPI_EXPORT_FUNCTION(db_init);
  NAPI_EXPORT_FUNCTION(db_open);
  NAPI_EXPORT_FUNCTION(db_close);
  NAPI_EXPORT_FUNCTION(db_put);
  NAPI_EXPORT_FUNCTION(db_get);
  NAPI_EXPORT_FUNCTION(db_del);
  NAPI_EXPORT_FUNCTION(db_approximate_size);
  NAPI_EXPORT_FUNCTION(db_compact_range);
  NAPI_EXPORT_FUNCTION(db_get_property);

  NAPI_EXPORT_FUNCTION(destroy_db);
  NAPI_EXPORT_FUNCTION(repair_db);

  NAPI_EXPORT_FUNCTION(iterator_init);
  NAPI_EXPORT_FUNCTION(iterator_seek);
  NAPI_EXPORT_FUNCTION(iterator_end);
  NAPI_EXPORT_FUNCTION(iterator_next);

  NAPI_EXPORT_FUNCTION(batch_do);
  NAPI_EXPORT_FUNCTION(batch_init);
  NAPI_EXPORT_FUNCTION(batch_put);
  NAPI_EXPORT_FUNCTION(batch_del);
  NAPI_EXPORT_FUNCTION(batch_clear);
  NAPI_EXPORT_FUNCTION(batch_write);
}
