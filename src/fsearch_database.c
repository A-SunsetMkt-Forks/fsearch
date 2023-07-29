#define G_LOG_DOMAIN "fsearch-database"

#include "fsearch_database.h"

#include <config.h>
#ifdef HAVE_MALLOC_TRIM
#include <malloc.h>
#endif

#include <glib/gi18n.h>
#include <sys/file.h>

#include "fsearch_array.h"
#include "fsearch_database_entries_container.h"
#include "fsearch_database_entry.h"
#include "fsearch_database_entry_info.h"
#include "fsearch_database_index.h"
#include "fsearch_database_info.h"
#include "fsearch_database_search.h"
#include "fsearch_database_search_info.h"
#include "fsearch_database_sort.h"
#include "fsearch_database_work.h"
#include "fsearch_enums.h"
#include "fsearch_selection.h"
#include "fsearch_thread_pool.h"

#define DATABASE_MAJOR_VERSION 1
#define DATABASE_MINOR_VERSION 0
#define DATABASE_MAGIC_NUMBER "FSDB"

#define NUM_DB_ENTRIES_FOR_POOL_BLOCK 10000

typedef struct {
    GPtrArray *indices;

    FsearchDatabaseEntriesContainer *file_container[NUM_DATABASE_INDEX_PROPERTIES];
    FsearchDatabaseEntriesContainer *folder_container[NUM_DATABASE_INDEX_PROPERTIES];

    FsearchDatabaseIncludeManager *include_manager;
    FsearchDatabaseExcludeManager *exclude_manager;

    FsearchDatabaseIndexEventFunc event_func;
    gpointer event_func_data;

    FsearchDatabaseIndexPropertyFlags flags;

    struct {
        GThread *thread;
        GMainLoop *loop;
        GMainContext *ctx;
    } monitor;

    struct {
        GThread *thread;
        GMainLoop *loop;
        GMainContext *ctx;
    } worker;

    bool is_sorted;
    bool running;

    volatile gint ref_count;
} FsearchDatabaseIndexStore;

struct _FsearchDatabase {
    GObject parent_instance;

    // The file the database will be loaded from and saved to
    GFile *file;

    GThread *work_queue_thread;
    GAsyncQueue *work_queue;

    GHashTable *search_results;

    FsearchThreadPool *thread_pool;

    FsearchDatabaseIndexStore *store;

    FsearchDatabaseIndexPropertyFlags flags;

    GMutex mutex;
};

typedef struct FsearchDatabaseSearchView {
    FsearchQuery *query;
    FsearchDatabaseEntriesContainer *file_container;
    FsearchDatabaseEntriesContainer *folder_container;
    GtkSortType sort_type;
    FsearchDatabaseIndexProperty sort_order;
    FsearchDatabaseIndexProperty secondardy_sort_order;
    GHashTable *file_selection;
    GHashTable *folder_selection;
} FsearchDatabaseSearchView;

typedef enum FsearchDatabaseEventType {
    EVENT_LOAD_STARTED,
    EVENT_LOAD_FINISHED,
    EVENT_ITEM_INFO_READY,
    EVENT_SAVE_STARTED,
    EVENT_SAVE_FINISHED,
    EVENT_SCAN_STARTED,
    EVENT_SCAN_FINISHED,
    EVENT_SEARCH_STARTED,
    EVENT_SEARCH_FINISHED,
    EVENT_SORT_STARTED,
    EVENT_SORT_FINISHED,
    EVENT_SELECTION_CHANGED,
    EVENT_DATABASE_CHANGED,
    NUM_EVENTS,
} FsearchDatabaseEventType;

G_DEFINE_TYPE(FsearchDatabase, fsearch_database, G_TYPE_OBJECT)

enum { PROP_0, PROP_FILE, NUM_PROPERTIES };

static GParamSpec *properties[NUM_PROPERTIES];
static guint signals[NUM_EVENTS];

typedef struct FsearchSignalEmitContext {
    FsearchDatabase *db;
    FsearchDatabaseEventType type;
    gpointer arg1;
    gpointer arg2;
    GDestroyNotify arg1_free_func;
    GDestroyNotify arg2_free_func;
    guint n_args;
} FsearchSignalEmitContext;

static void
index_event_cb(FsearchDatabaseIndex *index, FsearchDatabaseIndexEvent *event, gpointer user_data);

static void
index_store_unref(FsearchDatabaseIndexStore *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(FsearchDatabaseIndexStore, index_store_unref)

static void
unlock_all_indices(FsearchDatabaseIndexStore *self) {
    g_return_if_fail(self);

    for (uint32_t i = 0; i < self->indices->len; ++i) {
        FsearchDatabaseIndex *index_stored = g_ptr_array_index(self->indices, i);
        fsearch_database_index_unlock(index_stored);
    }
}

static void
lock_all_indices(FsearchDatabaseIndexStore *self) {
    g_return_if_fail(self);

    for (uint32_t i = 0; i < self->indices->len; ++i) {
        FsearchDatabaseIndex *index_stored = g_ptr_array_index(self->indices, i);
        fsearch_database_index_lock(index_stored);
    }
}

static void
sorted_entries_free(FsearchDatabaseIndexStore *self) {
    g_return_if_fail(self);

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; ++i) {
        if (self->file_container[i]) {
            g_clear_pointer(&self->file_container[i], fsearch_database_entries_container_unref);
        }
        if (self->folder_container[i]) {
            g_clear_pointer(&self->folder_container[i], fsearch_database_entries_container_unref);
        }
    }
}

static bool
has_flag(FsearchDatabaseIndexStore *self, FsearchDatabaseIndex *index) {
    g_assert(self);
    g_assert(index);

    const FsearchDatabaseIndexPropertyFlags store_flags = self->flags;
    const FsearchDatabaseIndexPropertyFlags index_flags = fsearch_database_index_get_flags(index);

    return (store_flags & index_flags) == store_flags;
}

static bool
index_store_has_index_with_same_id(FsearchDatabaseIndexStore *self, FsearchDatabaseIndex *index) {
    g_assert(self);
    g_assert(index);

    for (uint32_t i = 0; i < self->indices->len; ++i) {
        FsearchDatabaseIndex *index_stored = g_ptr_array_index(self->indices, i);
        if (fsearch_database_index_get_id(index_stored) == fsearch_database_index_get_id(index)) {
            return true;
        }
    }
    return false;
}

static void
index_store_remove_entry(FsearchDatabaseIndexStore *self, FsearchDatabaseEntry *entry, FsearchDatabaseIndex *index) {
    g_return_if_fail(self);
    g_return_if_fail(index);

    if (!entry) {
        return;
    }

    if (!g_ptr_array_find(self->indices, index, NULL)) {
        g_debug("[index_store_remove] index does not belong to index store; must be a bug");
        g_assert_not_reached();
    }

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; ++i) {
        FsearchDatabaseEntriesContainer *container = NULL;
        if (db_entry_is_folder(entry)) {
            container = self->folder_container[i];
        }
        else {
            container = self->file_container[i];
        }

        if (!container) {
            continue;
        }

        if (!fsearch_database_entries_container_steal(container, entry)) {
            g_debug("store: failed to remove entry: %s", db_entry_get_name_raw_for_display(entry));
        }
    }
}

static void
index_store_remove_folders(FsearchDatabaseIndexStore *self, DynamicArray *folders, FsearchDatabaseIndex *index) {
    g_return_if_fail(self);
    g_return_if_fail(index);

    if (!folders || darray_get_num_items(folders) == 0) {
        return;
    }

    if (!g_ptr_array_find(self->indices, index, NULL)) {
        g_debug("[index_store_remove] index does not belong to index store; must be a bug");
        g_assert_not_reached();
    }

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; ++i) {
        FsearchDatabaseEntriesContainer *container = self->folder_container[i];
        if (!container) {
            continue;
        }
        for (uint32_t j = 0; j < darray_get_num_items(folders); ++j) {
            FsearchDatabaseEntry *entry = darray_get_item(folders, j);
            if (!fsearch_database_entries_container_steal(container, entry)) {
                g_debug("store: failed to remove entry: %s", db_entry_get_name_raw_for_display(entry));
            }
        }
    }
}

static void
index_store_remove_files(FsearchDatabaseIndexStore *self, DynamicArray *files, FsearchDatabaseIndex *index) {
    g_return_if_fail(self);
    g_return_if_fail(index);

    if (!files || darray_get_num_items(files) == 0) {
        return;
    }

    if (!g_ptr_array_find(self->indices, index, NULL)) {
        g_debug("[index_store_remove] index does not belong to index store; must be a bug");
        g_assert_not_reached();
    }

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; ++i) {
        FsearchDatabaseEntriesContainer *container = self->file_container[i];
        if (!container) {
            continue;
        }
        for (uint32_t j = 0; j < darray_get_num_items(files); ++j) {
            FsearchDatabaseEntry *entry = darray_get_item(files, j);
            if (!fsearch_database_entries_container_steal(container, entry)) {
                g_debug("store: failed to remove entry: %s", db_entry_get_name_raw_for_display(entry));
            }
        }
    }
}

static void
index_store_add_entries(FsearchDatabaseIndexStore *self, DynamicArray *entries, bool is_dir) {
    g_return_if_fail(self);

    if (!entries || darray_get_num_items(entries) == 0) {
        return;
    }

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; ++i) {
        FsearchDatabaseEntriesContainer *container = NULL;
        if (is_dir) {
            container = self->folder_container[i];
        }
        else {
            container = self->file_container[i];
        }

        if (!container) {
            continue;
        }

        for (uint32_t j = 0; j < darray_get_num_items(entries); ++j) {
            FsearchDatabaseEntry *entry = darray_get_item(entries, j);
            fsearch_database_entries_container_insert(container, entry);
        }
    }
}

static gboolean
thread_quit(GMainLoop *loop) {
    g_return_val_if_fail(loop, G_SOURCE_REMOVE);

    g_main_loop_quit(loop);

    return G_SOURCE_REMOVE;
}

static void
thread_func(GMainContext *ctx, GMainLoop *loop) {
    g_main_context_push_thread_default(ctx);
    g_main_loop_run(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_loop_unref(loop);
}

static gpointer
worker_thread_func(gpointer user_data) {
    FsearchDatabaseIndexStore *self = user_data;
    g_return_val_if_fail(self, NULL);

    thread_func(self->worker.ctx, self->worker.loop);

    return NULL;
}

static gpointer
monitor_thread_func(gpointer user_data) {
    FsearchDatabaseIndexStore *self = user_data;
    g_return_val_if_fail(self, NULL);

    thread_func(self->monitor.ctx, self->monitor.loop);

    return NULL;
}

static void
index_store_free(FsearchDatabaseIndexStore *self) {
    g_return_if_fail(self);

    if (self->monitor.loop) {
        g_main_context_invoke_full(self->monitor.ctx, G_PRIORITY_HIGH, (GSourceFunc)thread_quit, self->monitor.loop, NULL);
    }
    if (self->monitor.thread) {
        g_thread_join(self->monitor.thread);
    }
    g_clear_pointer(&self->monitor.ctx, g_main_context_unref);

    if (self->worker.loop) {
        g_main_context_invoke_full(self->worker.ctx, G_PRIORITY_HIGH, (GSourceFunc)thread_quit, self->worker.loop, NULL);
    }
    if (self->worker.thread) {
        g_thread_join(self->worker.thread);
    }
    g_clear_pointer(&self->worker.ctx, g_main_context_unref);

    sorted_entries_free(self);
    g_clear_pointer(&self->indices, g_ptr_array_unref);
    g_clear_object(&self->include_manager);
    g_clear_object(&self->exclude_manager);

    g_slice_free(FsearchDatabaseIndexStore, self);
}

static FsearchDatabaseIndexStore *
index_store_new(FsearchDatabaseIncludeManager *include_manager,
                FsearchDatabaseExcludeManager *exclude_manager,
                FsearchDatabaseIndexPropertyFlags flags,
                FsearchDatabaseIndexEventFunc event_func,
                gpointer event_func_data) {
    FsearchDatabaseIndexStore *self;
    self = g_slice_new0(FsearchDatabaseIndexStore);

    self->indices = g_ptr_array_new_with_free_func((GDestroyNotify)fsearch_database_index_unref);
    self->flags = flags;
    self->is_sorted = false;
    self->running = false;

    self->include_manager = g_object_ref(include_manager);
    self->exclude_manager = g_object_ref(exclude_manager);

    self->event_func = event_func;
    self->event_func_data = event_func_data;

    self->monitor.ctx = g_main_context_new();
    self->monitor.loop = g_main_loop_new(self->monitor.ctx, FALSE);
    self->monitor.thread = g_thread_new("FsearchDatabaseIndexStoreMonitor", monitor_thread_func, self);

    self->worker.ctx = g_main_context_new();
    self->worker.loop = g_main_loop_new(self->worker.ctx, FALSE);
    self->worker.thread = g_thread_new("FsearchDatabaseIndexStoreWorker", worker_thread_func, self);

    self->ref_count = 1;

    return self;
}

static FsearchDatabaseIndexStore *
index_store_ref(FsearchDatabaseIndexStore *self) {
    g_return_val_if_fail(self != NULL, NULL);
    g_return_val_if_fail(self->ref_count > 0, NULL);

    g_atomic_int_inc(&self->ref_count);

    return self;
}

static void
index_store_unref(FsearchDatabaseIndexStore *self) {
    g_return_if_fail(self != NULL);
    g_return_if_fail(self->ref_count > 0);

    if (g_atomic_int_dec_and_test(&self->ref_count)) {
        g_clear_pointer(&self, index_store_free);
    }
}

static bool
index_store_has_container(FsearchDatabaseIndexStore *self, FsearchDatabaseEntriesContainer *container) {
    g_return_val_if_fail(self, false);
    g_return_val_if_fail(container, false);

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; ++i) {
        FsearchDatabaseEntriesContainer *c = self->folder_container[i];
        if (c == container) {
            return true;
        }
        c = self->file_container[i];
        if (c == container) {
            return true;
        }
    }
    return false;
}

static FsearchDatabaseEntriesContainer *
index_store_get_files(FsearchDatabaseIndexStore *self, FsearchDatabaseIndexProperty sort_order) {
    g_return_val_if_fail(self, NULL);
    g_return_val_if_fail(self->is_sorted, NULL);

    return self->file_container[sort_order] ? fsearch_database_entries_container_ref(self->file_container[sort_order])
                                            : NULL;
}

static FsearchDatabaseEntriesContainer *
index_store_get_folders(FsearchDatabaseIndexStore *self, FsearchDatabaseIndexProperty sort_order) {
    g_return_val_if_fail(self, NULL);
    g_return_val_if_fail(self->is_sorted, NULL);

    return self->folder_container[sort_order]
             ? fsearch_database_entries_container_ref(self->folder_container[sort_order])
             : NULL;
}

static uint32_t
index_store_get_num_fast_sort_indices(FsearchDatabaseIndexStore *self) {
    g_return_val_if_fail(self, 0);

    uint32_t num_fast_sort_indices = 0;
    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; ++i) {
        if (self->folder_container[i] && self->file_container[i]) {
            num_fast_sort_indices++;
        }
    }

    return num_fast_sort_indices;
}

static uint32_t
index_store_get_num_files(FsearchDatabaseIndexStore *self) {
    g_return_val_if_fail(self, 0);

    return self->file_container[DATABASE_INDEX_PROPERTY_NAME]
             ? fsearch_database_entries_container_get_num_entries(self->file_container[DATABASE_INDEX_PROPERTY_NAME])
             : 0;
}

static uint32_t
index_store_get_num_folders(FsearchDatabaseIndexStore *self) {
    g_return_val_if_fail(self, 0);

    return self->folder_container[DATABASE_INDEX_PROPERTY_NAME]
             ? fsearch_database_entries_container_get_num_entries(self->folder_container[DATABASE_INDEX_PROPERTY_NAME])
             : 0;
}

static void
index_store_start(FsearchDatabaseIndexStore *self, GCancellable *cancellable) {
    g_return_if_fail(self);
    if (self->running) {
        return;
    }

    g_autoptr(GPtrArray) indices = g_ptr_array_new_with_free_func((GDestroyNotify)fsearch_database_index_unref);
    g_autoptr(GPtrArray) includes = fsearch_database_include_manager_get_includes(self->include_manager);
    for (uint32_t i = 0; i < includes->len; ++i) {
        FsearchDatabaseInclude *include = g_ptr_array_index(includes, i);
        g_autoptr(FsearchDatabaseIndex) index = fsearch_database_index_new(fsearch_database_include_get_id(include),
                                                                           include,
                                                                           self->exclude_manager,
                                                                           self->flags,
                                                                           self->worker.ctx,
                                                                           self->monitor.ctx,
                                                                           index_event_cb,
                                                                           self->event_func_data);
        if (index && fsearch_database_index_scan(index, cancellable)) {
            g_ptr_array_add(indices, g_steal_pointer(&index));
        }
    }
    if (g_cancellable_is_cancelled(cancellable)) {
        return;
    }

    g_autoptr(DynamicArray) store_files = darray_new(1024);
    g_autoptr(DynamicArray) store_folders = darray_new(1024);
    for (uint32_t i = 0; i < indices->len; ++i) {
        FsearchDatabaseIndex *index = g_ptr_array_index(indices, i);

        if (index_store_has_index_with_same_id(self, index) || !has_flag(self, index)) {
            continue;
        }
        g_ptr_array_add(self->indices, fsearch_database_index_ref(index));
        fsearch_database_index_lock(index);
        g_autoptr(DynamicArray) files = fsearch_database_index_get_files(index);
        g_autoptr(DynamicArray) folders = fsearch_database_index_get_folders(index);
        darray_add_array(store_files, files);
        darray_add_array(store_folders, folders);

        fsearch_database_index_unlock(index);

        self->is_sorted = false;
    }

    lock_all_indices(self);
    self->folder_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(store_folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               cancellable);
    self->file_container[DATABASE_INDEX_PROPERTY_NAME] =
        fsearch_database_entries_container_new(store_files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_NAME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               cancellable);
    self->folder_container[DATABASE_INDEX_PROPERTY_PATH] =
        fsearch_database_entries_container_new(store_folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_PATH,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               cancellable);
    self->file_container[DATABASE_INDEX_PROPERTY_PATH] =
        fsearch_database_entries_container_new(store_files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_PATH,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               cancellable);
    self->folder_container[DATABASE_INDEX_PROPERTY_SIZE] =
        fsearch_database_entries_container_new(store_folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_SIZE,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               cancellable);
    self->file_container[DATABASE_INDEX_PROPERTY_SIZE] =
        fsearch_database_entries_container_new(store_files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_SIZE,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               cancellable);
    self->folder_container[DATABASE_INDEX_PROPERTY_MODIFICATION_TIME] =
        fsearch_database_entries_container_new(store_folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_MODIFICATION_TIME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               cancellable);
    self->file_container[DATABASE_INDEX_PROPERTY_MODIFICATION_TIME] =
        fsearch_database_entries_container_new(store_files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_MODIFICATION_TIME,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               cancellable);
    self->folder_container[DATABASE_INDEX_PROPERTY_EXTENSION] =
        fsearch_database_entries_container_new(store_folders,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_EXTENSION,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FOLDER,
                                               cancellable);
    self->file_container[DATABASE_INDEX_PROPERTY_EXTENSION] =
        fsearch_database_entries_container_new(store_files,
                                               FALSE,
                                               DATABASE_INDEX_PROPERTY_EXTENSION,
                                               DATABASE_INDEX_PROPERTY_NONE,
                                               DATABASE_ENTRY_TYPE_FILE,
                                               cancellable);
    self->is_sorted = true;
    unlock_all_indices(self);

    if (g_cancellable_is_cancelled(cancellable)) {
        sorted_entries_free(self);
        g_ptr_array_remove_range(self->indices, 0, self->indices->len);
        return;
    }

    self->running = true;

    return;
}

static void
index_store_start_monitoring(FsearchDatabaseIndexStore *self) {
    g_return_if_fail(self);
    lock_all_indices(self);

    for (uint32_t i = 0; i < self->indices->len; ++i) {
        FsearchDatabaseIndex *index = g_ptr_array_index(self->indices, i);
        fsearch_database_index_start_monitoring(index, true);
    }

    unlock_all_indices(self);
}

// Database File

typedef struct {
    FsearchMemoryPool *file_pool;
    FsearchMemoryPool *folder_pool;
    DynamicArray *files[NUM_DATABASE_INDEX_PROPERTIES];
    DynamicArray *folders[NUM_DATABASE_INDEX_PROPERTIES];
    FsearchDatabaseIndexPropertyFlags flags;
} LoadSaveContext;

static void
update_folder_indices(DynamicArray *folders) {
    g_assert(folders);
    const uint32_t num_folders = darray_get_num_items(folders);
    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = darray_get_item(folders, i);
        if (!folder) {
            continue;
        }
        db_entry_set_idx((FsearchDatabaseEntry *)folder, i);
    }
}

static uint8_t
get_name_offset(const char *old, const char *new) {
    if (!old || !new) {
        return 0;
    }

    uint8_t offset = 0;
    while (old[offset] == new[offset] && old[offset] != '\0' && new[offset] != '\0' && offset < 255) {
        offset++;
    }
    return offset;
}

static FILE *
file_open_locked(const char *file_path, const char *mode) {
    FILE *file_pointer = fopen(file_path, mode);
    if (!file_pointer) {
        g_debug("[db_file] can't open database file: %s", file_path);
        return NULL;
    }

    int file_descriptor = fileno(file_pointer);
    if (flock(file_descriptor, LOCK_EX | LOCK_NB) == -1) {
        g_debug("[db_file] database file is already locked by a different process: %s", file_path);

        g_clear_pointer(&file_pointer, fclose);
    }

    return file_pointer;
}

static const uint8_t *
copy_bytes_and_return_new_src(void *dest, const uint8_t *src, size_t len) {
    memcpy(dest, src, len);
    return src + len;
}

static const uint8_t *
load_entry_super_elements_from_memory(const uint8_t *data_block,
                                      FsearchDatabaseIndexPropertyFlags index_flags,
                                      FsearchDatabaseEntry *entry,
                                      GString *previous_entry_name) {
    // name_offset: character position after which previous_entry_name and entry_name differ
    uint8_t name_offset = *data_block++;

    // name_len: length of the new name characters
    uint8_t name_len = *data_block++;

    // erase previous name starting at name_offset
    g_string_erase(previous_entry_name, name_offset, -1);

    char name[256] = "";
    // name: new characters to be appended to previous_entry_name
    if (name_len > 0) {
        data_block = copy_bytes_and_return_new_src(name, data_block, name_len);
        name[name_len] = '\0';
    }

    // now we can build the new full file name
    g_string_append(previous_entry_name, name);
    db_entry_set_name(entry, previous_entry_name->str);

    if ((index_flags & DATABASE_INDEX_PROPERTY_FLAG_SIZE) != 0) {
        // size: size of file/folder
        off_t size = 0;
        data_block = copy_bytes_and_return_new_src(&size, data_block, 8);

        db_entry_set_size(entry, size);
    }

    if ((index_flags & DATABASE_INDEX_PROPERTY_FLAG_MODIFICATION_TIME) != 0) {
        // mtime: modification time file/folder
        time_t mtime = 0;
        data_block = copy_bytes_and_return_new_src(&mtime, data_block, 8);

        db_entry_set_mtime(entry, mtime);
    }

    return data_block;
}

static bool
read_element_from_file(void *restrict ptr, size_t size, FILE *restrict stream) {
    return fread(ptr, size, 1, stream) == 1 ? true : false;
}

static bool
load_entry_super_elements(FILE *fp, FsearchDatabaseEntry *entry, GString *previous_entry_name) {
    // name_offset: character position after which previous_entry_name and entry_name differ
    uint8_t name_offset = 0;
    if (!read_element_from_file(&name_offset, 1, fp)) {
        g_debug("[db_load] failed to load name offset");
        return false;
    }

    // name_len: length of the new name characters
    uint8_t name_len = 0;
    if (!read_element_from_file(&name_len, 1, fp)) {
        g_debug("[db_load] failed to load name length");
        return false;
    }

    // erase previous name starting at name_offset
    g_string_erase(previous_entry_name, name_offset, -1);

    char name[256] = "";
    // name: new characters to be appended to previous_entry_name
    if (name_len > 0) {
        if (!read_element_from_file(name, name_len, fp)) {
            g_debug("[db_load] failed to load name");
            return false;
        }
        name[name_len] = '\0';
    }

    // now we can build the new full file name
    g_string_append(previous_entry_name, name);
    db_entry_set_name(entry, previous_entry_name->str);

    // size: size of file/folder
    uint64_t size = 0;
    if (!read_element_from_file(&size, 8, fp)) {
        g_debug("[db_load] failed to load size");
        return false;
    }
    db_entry_set_size(entry, (off_t)size);

    return true;
}

static bool
load_header(FILE *fp) {
    char magic[5] = "";
    if (!read_element_from_file(magic, strlen(DATABASE_MAGIC_NUMBER), fp)) {
        return false;
    }
    magic[4] = '\0';
    if (strcmp(magic, DATABASE_MAGIC_NUMBER) != 0) {
        g_debug("[db_load] invalid magic number: %s", magic);
        return false;
    }

    uint8_t majorver = 0;
    if (!read_element_from_file(&majorver, 1, fp)) {
        return false;
    }
    if (majorver != DATABASE_MAJOR_VERSION) {
        g_debug("[db_load] invalid major version: %d", majorver);
        g_debug("[db_load] expected major version: %d", DATABASE_MAJOR_VERSION);
        return false;
    }

    uint8_t minorver = 0;
    if (!read_element_from_file(&minorver, 1, fp)) {
        return false;
    }
    if (minorver > DATABASE_MINOR_VERSION) {
        g_debug("[db_load] invalid minor version: %d", minorver);
        g_debug("[db_load] expected minor version: <= %d", DATABASE_MINOR_VERSION);
        return false;
    }

    return true;
}

static bool
load_parent_idx(FILE *fp, uint32_t *parent_idx) {
    if (!read_element_from_file(parent_idx, 4, fp)) {
        g_debug("[db_load] failed to load parent_idx");
        return false;
    }
    return true;
}

static bool
load_folders(FILE *fp,
             FsearchDatabaseIndexPropertyFlags index_flags,
             DynamicArray *folders,
             uint32_t num_folders,
             uint64_t folder_block_size) {
    g_autoptr(GString) previous_entry_name = g_string_sized_new(256);

    g_autofree uint8_t *folder_block = calloc(folder_block_size + 1, sizeof(uint8_t));
    g_assert(folder_block);

    if (fread(folder_block, sizeof(uint8_t), folder_block_size, fp) != folder_block_size) {
        g_debug("[db_load] failed to read file block");
        return false;
    }

    const uint8_t *fb = folder_block;
    // load folders
    uint32_t idx = 0;
    for (idx = 0; idx < num_folders; idx++) {
        FsearchDatabaseEntryFolder *folder = darray_get_item(folders, idx);
        FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)folder;

        // TODO: db_index is currently unused
        // db_index: the database index this folder belongs to
        uint16_t db_index = 0;
        fb = copy_bytes_and_return_new_src(&db_index, fb, 2);

        fb = load_entry_super_elements_from_memory(fb, index_flags, entry, previous_entry_name);

        // parent_idx: index of parent folder
        uint32_t parent_idx = 0;
        fb = copy_bytes_and_return_new_src(&parent_idx, fb, 4);

        if (parent_idx != db_entry_get_idx(entry)) {
            FsearchDatabaseEntryFolder *parent = darray_get_item(folders, parent_idx);
            db_entry_set_parent(entry, parent);
        }
        else {
            // parent_idx and idx are the same (i.e. folder is a root index) so it has no parent
            db_entry_set_parent(entry, NULL);
        }
    }

    // fail if we didn't read the correct number of bytes
    if (fb - folder_block != folder_block_size) {
        g_debug("[db_load] wrong amount of memory read: %lu != %lu", fb - folder_block, folder_block_size);
        return false;
    }

    // fail if we didn't read the correct number of folders
    if (idx != num_folders) {
        g_debug("[db_load] failed to read folders (read %d of %d)", idx, num_folders);
        return false;
    }

    return true;
}

static bool
load_files(FILE *fp,
           FsearchDatabaseIndexPropertyFlags index_flags,
           FsearchMemoryPool *pool,
           DynamicArray *folders,
           DynamicArray *files,
           uint32_t num_files,
           uint64_t file_block_size) {
    g_autoptr(GString) previous_entry_name = g_string_sized_new(256);
    g_autofree uint8_t *file_block = calloc(file_block_size + 1, sizeof(uint8_t));
    g_assert(file_block);

    if (fread(file_block, sizeof(uint8_t), file_block_size, fp) != file_block_size) {
        g_debug("[db_load] failed to read file block");
        return false;
    }

    const uint8_t *fb = file_block;
    // load folders
    uint32_t idx = 0;
    for (idx = 0; idx < num_files; idx++) {
        FsearchDatabaseEntry *entry = fsearch_memory_pool_malloc(pool);
        db_entry_set_type(entry, DATABASE_ENTRY_TYPE_FILE);
        db_entry_set_idx(entry, idx);

        fb = load_entry_super_elements_from_memory(fb, index_flags, entry, previous_entry_name);

        // parent_idx: index of parent folder
        uint32_t parent_idx = 0;
        fb = copy_bytes_and_return_new_src(&parent_idx, fb, 4);

        FsearchDatabaseEntryFolder *parent = darray_get_item(folders, parent_idx);
        db_entry_set_parent(entry, parent);

        darray_add_item(files, entry);
    }
    if (fb - file_block != file_block_size) {
        g_debug("[db_load] wrong amount of memory read: %lu != %lu", fb - file_block, file_block_size);
        return false;
    }

    // fail if we didn't read the correct number of files
    if (idx != num_files) {
        g_debug("[db_load] failed to read files (read %d of %d)", idx, num_files);
        return false;
    }

    return true;
}

static bool
load_sorted_entries(FILE *fp, DynamicArray *src, uint32_t num_src_entries, DynamicArray *dest) {

    g_autofree uint32_t *indexes = calloc(num_src_entries + 1, sizeof(uint32_t));
    g_assert(indexes);

    if (fread(indexes, 4, num_src_entries, fp) != num_src_entries) {
        return false;
    }
    else {
        for (uint32_t i = 0; i < num_src_entries; i++) {
            uint32_t idx = indexes[i];
            void *entry = darray_get_item(src, idx);
            if (!entry) {
                return false;
            }
            darray_add_item(dest, entry);
        }
    }
    return true;
}

static bool
load_sorted_arrays(FILE *fp, DynamicArray **sorted_folders, DynamicArray **sorted_files) {
    uint32_t num_sorted_arrays = 0;

    DynamicArray *files = sorted_files[0];
    DynamicArray *folders = sorted_folders[0];

    if (!read_element_from_file(&num_sorted_arrays, 4, fp)) {
        g_debug("[db_load] failed to load number of sorted arrays");
        return false;
    }

    for (uint32_t i = 0; i < num_sorted_arrays; i++) {
        uint32_t sorted_array_id = 0;
        if (!read_element_from_file(&sorted_array_id, 4, fp)) {
            g_debug("[db_load] failed to load sorted array id");
            return false;
        }

        if (sorted_array_id < 1 || sorted_array_id >= NUM_DATABASE_INDEX_PROPERTIES) {
            g_debug("[db_load] sorted array id is not supported: %d", sorted_array_id);
            return false;
        }

        const uint32_t num_folders = darray_get_num_items(folders);
        sorted_folders[sorted_array_id] = darray_new(num_folders);
        if (!load_sorted_entries(fp, folders, num_folders, sorted_folders[sorted_array_id])) {
            g_debug("[db_load] failed to load sorted folder indexes: %d", sorted_array_id);
            return false;
        }

        const uint32_t num_files = darray_get_num_items(files);
        sorted_files[sorted_array_id] = darray_new(num_files);
        if (!load_sorted_entries(fp, files, num_files, sorted_files[sorted_array_id])) {
            g_debug("[db_load] failed to load sorted file indexes: %d", sorted_array_id);
            return false;
        }
    }

    return true;
}

static size_t
write_data_to_file(FILE *fp, const void *data, size_t data_size, size_t num_elements, bool *write_failed) {
    if (data_size == 0 || num_elements == 0) {
        return 0;
    }
    if (fwrite(data, data_size, num_elements, fp) != num_elements) {
        *write_failed = true;
        return 0;
    }
    return data_size * num_elements;
}

static size_t
save_entry_super_elements(FILE *fp,
                          FsearchDatabaseIndexPropertyFlags index_flags,
                          FsearchDatabaseEntry *entry,
                          uint32_t parent_idx,
                          GString *previous_entry_name,
                          GString *new_entry_name,
                          bool *write_failed) {
    // init new_entry_name with the name of the current entry
    g_string_erase(new_entry_name, 0, -1);
    g_string_append(new_entry_name, db_entry_get_name_raw(entry));

    size_t bytes_written = 0;
    // name_offset: character position after which previous_entry_name and new_entry_name differ
    const uint8_t name_offset = get_name_offset(previous_entry_name->str, new_entry_name->str);
    bytes_written += write_data_to_file(fp, &name_offset, 1, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save name offset");
        goto out;
    }

    // name_len: length of the new name characters
    const uint8_t name_len = new_entry_name->len - name_offset;
    bytes_written += write_data_to_file(fp, &name_len, 1, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save name length");
        goto out;
    }

    // append new unique characters to previous_entry_name starting at name_offset
    g_string_erase(previous_entry_name, name_offset, -1);
    g_string_append(previous_entry_name, new_entry_name->str + name_offset);

    if (name_len > 0) {
        // name: new characters to be written to file
        const char *name = previous_entry_name->str + name_offset;
        bytes_written += write_data_to_file(fp, name, name_len, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save name");
            goto out;
        }
    }

    if ((index_flags & DATABASE_INDEX_PROPERTY_FLAG_SIZE) != 0) {
        // size: file or folder size (folder size: sum of all children sizes)
        const uint64_t size = db_entry_get_size(entry);
        bytes_written += write_data_to_file(fp, &size, 8, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save size");
            goto out;
        }
    }

    if ((index_flags & DATABASE_INDEX_PROPERTY_FLAG_MODIFICATION_TIME) != 0) {
        // mtime: modification time of file/folder
        const uint64_t mtime = db_entry_get_mtime(entry);
        bytes_written += write_data_to_file(fp, &mtime, 8, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save modification time");
            goto out;
        }
    }

    // parent_idx: index of parent folder
    bytes_written += write_data_to_file(fp, &parent_idx, 4, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save parent_idx");
        goto out;
    }

out:
    return bytes_written;
}

static size_t
save_header(FILE *fp, bool *write_failed) {
    size_t bytes_written = 0;

    const char magic[] = DATABASE_MAGIC_NUMBER;
    bytes_written += write_data_to_file(fp, magic, strlen(magic), 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save magic number");
        goto out;
    }

    const uint8_t majorver = DATABASE_MAJOR_VERSION;
    bytes_written += write_data_to_file(fp, &majorver, 1, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save major version number");
        goto out;
    }

    const uint8_t minorver = DATABASE_MINOR_VERSION;
    bytes_written += write_data_to_file(fp, &minorver, 1, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save minor version number");
        goto out;
    }

out:
    return bytes_written;
}

static size_t
save_files(FILE *fp,
           FsearchDatabaseIndexPropertyFlags index_flags,
           DynamicArray *files,
           uint32_t num_files,
           bool *write_failed) {
    size_t bytes_written = 0;

    g_autoptr(GString) name_prev = g_string_sized_new(256);
    g_autoptr(GString) name_new = g_string_sized_new(256);

    for (uint32_t i = 0; i < num_files; i++) {
        FsearchDatabaseEntry *entry = darray_get_item(files, i);

        // let's also update the idx of the file here while we're at it to make sure we have the correct
        // idx set when we store the fast sort indexes
        db_entry_set_idx(entry, i);

        FsearchDatabaseEntryFolder *parent = db_entry_get_parent(entry);
        const uint32_t parent_idx = db_entry_get_idx((FsearchDatabaseEntry *)parent);
        bytes_written += save_entry_super_elements(fp, index_flags, entry, parent_idx, name_prev, name_new, write_failed);
        if (*write_failed == true)
            return bytes_written;
    }
    return bytes_written;
}

static uint32_t *
build_sorted_entry_index_list(DynamicArray *entries, uint32_t num_entries) {
    if (num_entries < 1) {
        return NULL;
    }
    uint32_t *indexes = calloc(num_entries + 1, sizeof(uint32_t));
    g_assert(indexes);

    for (int i = 0; i < num_entries; i++) {
        FsearchDatabaseEntry *entry = darray_get_item(entries, i);
        indexes[i] = db_entry_get_idx(entry);
    }
    return indexes;
}

static size_t
save_sorted_entries(FILE *fp, DynamicArray *entries, uint32_t num_entries, bool *write_failed) {
    if (num_entries < 1) {
        // nothing to write, we're done here
        return 0;
    }

    g_autofree uint32_t *sorted_entry_index_list = build_sorted_entry_index_list(entries, num_entries);
    if (!sorted_entry_index_list) {
        *write_failed = true;
        g_debug("[db_save] failed to create sorted index list");
        return 0;
    }

    size_t bytes_written = write_data_to_file(fp, sorted_entry_index_list, 4, num_entries, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save sorted index list");
    }

    return bytes_written;
}

static size_t
save_sorted_arrays(FILE *fp, FsearchDatabaseIndexStore *store, uint32_t num_files, uint32_t num_folders, bool *write_failed) {
    size_t bytes_written = 0;
    uint32_t num_sorted_arrays = index_store_get_num_fast_sort_indices(store);

    bytes_written += write_data_to_file(fp, &num_sorted_arrays, 4, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save number of sorted arrays: %d", num_sorted_arrays);
        goto out;
    }

    if (num_sorted_arrays < 1) {
        goto out;
    }

    for (uint32_t id = 1; id < NUM_DATABASE_INDEX_PROPERTIES; id++) {
        g_autoptr(FsearchDatabaseEntriesContainer) folder_container = index_store_get_folders(store, id);
        g_autoptr(FsearchDatabaseEntriesContainer) file_container = index_store_get_files(store, id);
        if (!folder_container || !file_container) {
            continue;
        }
        g_autoptr(DynamicArray) folders = fsearch_database_entries_container_get_joined(folder_container);
        g_autoptr(DynamicArray) files = fsearch_database_entries_container_get_joined(file_container);
        if (!files || !folders) {
            continue;
        }

        // id: this is the id of the sorted files
        bytes_written += write_data_to_file(fp, &id, 4, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save sorted arrays id: %d", id);
            goto out;
        }

        bytes_written += save_sorted_entries(fp, folders, num_folders, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save sorted folders");
            goto out;
        }
        bytes_written += save_sorted_entries(fp, files, num_files, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save sorted files");
            goto out;
        }
    }

out:
    return bytes_written;
}

static size_t
save_folders(FILE *fp,
             FsearchDatabaseIndexPropertyFlags index_flags,
             DynamicArray *folders,
             uint32_t num_folders,
             bool *write_failed) {
    size_t bytes_written = 0;

    g_autoptr(GString) name_prev = g_string_sized_new(256);
    g_autoptr(GString) name_new = g_string_sized_new(256);

    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntry *entry = darray_get_item(folders, i);

        const uint16_t db_index = db_entry_get_db_index(entry);
        bytes_written += write_data_to_file(fp, &db_index, 2, 1, write_failed);
        if (*write_failed == true) {
            g_debug("[db_save] failed to save folder's database index: %d", db_index);
            return bytes_written;
        }

        FsearchDatabaseEntryFolder *parent = db_entry_get_parent(entry);
        const uint32_t parent_idx = parent ? db_entry_get_idx((FsearchDatabaseEntry *)parent) : db_entry_get_idx(entry);
        bytes_written += save_entry_super_elements(fp, index_flags, entry, parent_idx, name_prev, name_new, write_failed);
        if (*write_failed == true) {
            return bytes_written;
        }
    }

    return bytes_written;
}

static size_t
save_indexes(FILE *fp, FsearchDatabaseIndexStore *store, bool *write_failed) {
    size_t bytes_written = 0;

    // TODO: actually implement storing all index information
    const uint32_t num_indexes = 0;
    bytes_written += write_data_to_file(fp, &num_indexes, 4, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save number of indexes: %d", num_indexes);
        goto out;
    }
out:
    return bytes_written;
}

static size_t
save_excludes(FILE *fp, FsearchDatabaseIndexStore *store, bool *write_failed) {
    size_t bytes_written = 0;

    // TODO: actually implement storing all exclude information
    const uint32_t num_excludes = 0;
    bytes_written += write_data_to_file(fp, &num_excludes, 4, 1, write_failed);
    if (*write_failed == true) {
        g_debug("[db_save] failed to save number of indexes: %d", num_excludes);
        goto out;
    }
out:
    return bytes_written;
}

static size_t
save_exclude_pattern(FILE *fp, FsearchDatabaseIndexStore *store, bool *write_failed) {
    // TODO
    return 0;
}

bool
db_file_save(FsearchDatabaseIndexStore *store, const char *file_path) {
    g_return_val_if_fail(file_path, false);
    g_return_val_if_fail(store, false);

    g_debug("[db_save] saving database to file...");

    if (!g_file_test(file_path, G_FILE_TEST_IS_DIR)) {
        g_debug("[db_save] database file_path doesn't exist: %s", file_path);
        return false;
    }

    g_autoptr(GTimer) timer = g_timer_new();
    g_timer_start(timer);

    g_autoptr(GString) path_full = g_string_new(file_path);
    g_string_append_c(path_full, G_DIR_SEPARATOR);
    g_string_append(path_full, "fsearch.db");

    g_autoptr(GString) path_full_temp = g_string_new(path_full->str);
    g_string_append(path_full_temp, ".tmp");

    g_autoptr(FsearchDatabaseEntriesContainer) folder_container = NULL;
    g_autoptr(FsearchDatabaseEntriesContainer) file_container = NULL;

    g_autoptr(DynamicArray) files = NULL;
    g_autoptr(DynamicArray) folders = NULL;

    g_debug("[db_save] trying to open temporary database file: %s", path_full_temp->str);

    FILE *fp = file_open_locked(path_full_temp->str, "wb");
    if (!fp) {
        g_debug("[db_save] failed to open temporary database file: %s", path_full_temp->str);
        goto save_fail;
    }

    bool write_failed = false;

    size_t bytes_written = 0;

    g_debug("[db_save] saving database header...");
    bytes_written += save_header(fp, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    g_debug("[db_save] saving database index flags...");
    const uint64_t index_flags = store->flags;
    bytes_written += write_data_to_file(fp, &index_flags, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    g_debug("[db_save] updating folder indices...");
    folder_container = index_store_get_folders(store, DATABASE_INDEX_PROPERTY_NAME);
    folders = fsearch_database_entries_container_get_joined(folder_container);
    update_folder_indices(folders);

    const uint32_t num_folders = darray_get_num_items(folders);
    g_debug("[db_save] saving number of folders: %d", num_folders);
    bytes_written += write_data_to_file(fp, &num_folders, 4, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    file_container = index_store_get_files(store, DATABASE_INDEX_PROPERTY_NAME);
    files = fsearch_database_entries_container_get_joined(file_container);
    const uint32_t num_files = darray_get_num_items(files);
    g_debug("[db_save] saving number of files: %d", num_files);
    bytes_written += write_data_to_file(fp, &num_files, 4, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    uint64_t folder_block_size = 0;
    const uint64_t folder_block_size_offset = bytes_written;
    g_debug("[db_save] saving folder block size...");
    bytes_written += write_data_to_file(fp, &folder_block_size, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    uint64_t file_block_size = 0;
    const uint64_t file_block_size_offset = bytes_written;
    g_debug("[db_save] saving file block size...");
    bytes_written += write_data_to_file(fp, &file_block_size, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    g_debug("[db_save] saving indices...");
    bytes_written += save_indexes(fp, store, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }
    g_debug("[db_save] saving excludes...");
    bytes_written += save_excludes(fp, store, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }
    g_debug("[db_save] saving exclude pattern...");
    bytes_written += save_exclude_pattern(fp, store, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }
    g_debug("[db_save] saving folders...");
    folder_block_size = save_folders(fp, index_flags, folders, num_folders, &write_failed);
    bytes_written += folder_block_size;
    if (write_failed == true) {
        goto save_fail;
    }
    g_debug("[db_save] saving files...");
    file_block_size = save_files(fp, index_flags, files, num_files, &write_failed);
    bytes_written += file_block_size;
    if (write_failed == true) {
        goto save_fail;
    }
    g_debug("[db_save] saving sorted arrays...");
    bytes_written += save_sorted_arrays(fp, store, num_files, num_folders, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    // now that we know the size of the file/folder block we've written, store it in the file header
    if (fseek(fp, (long int)folder_block_size_offset, SEEK_SET) != 0) {
        goto save_fail;
    }
    g_debug("[db_save] updating file and folder block size: %lu, %lu", folder_block_size, file_block_size);
    bytes_written += write_data_to_file(fp, &folder_block_size, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }
    bytes_written += write_data_to_file(fp, &file_block_size, 8, 1, &write_failed);
    if (write_failed == true) {
        goto save_fail;
    }

    g_debug("[db_save] removing current database file...");
    // remove current database file
    unlink(path_full->str);

    g_clear_pointer(&fp, fclose);

    g_debug("[db_save] renaming temporary database file: %s -> %s", path_full_temp->str, path_full->str);
    // rename temporary fsearch.db.tmp to fsearch.db
    if (rename(path_full_temp->str, path_full->str) != 0) {
        goto save_fail;
    }

    const double seconds = g_timer_elapsed(timer, NULL);
    g_timer_stop(timer);

    g_debug("[db_save] database file saved in: %f ms", seconds * 1000);

    return true;

save_fail:
    g_warning("[db_save] saving failed");

    g_clear_pointer(&fp, fclose);

    // remove temporary fsearch.db.tmp file
    unlink(path_full_temp->str);

    return false;
}

bool
db_file_load(const char *file_path,
             void (*status_cb)(const char *),
             FsearchDatabaseIndexStore **store_out,
             FsearchDatabaseIncludeManager **include_manager_out,
             FsearchDatabaseExcludeManager **exclude_manager_out) {
    g_return_val_if_fail(file_path, false);
    g_return_val_if_fail(store_out, false);

    FILE *fp = file_open_locked(file_path, "rb");
    if (!fp) {
        return false;
    }

    DynamicArray *folders = NULL;
    DynamicArray *files = NULL;
    DynamicArray *sorted_folders[NUM_DATABASE_INDEX_PROPERTIES] = {NULL};
    DynamicArray *sorted_files[NUM_DATABASE_INDEX_PROPERTIES] = {NULL};
    FsearchMemoryPool *file_pool = fsearch_memory_pool_new(NUM_DB_ENTRIES_FOR_POOL_BLOCK,
                                                           db_entry_get_sizeof_file_entry(),
                                                           (GDestroyNotify)db_entry_destroy);
    FsearchMemoryPool *folder_pool = fsearch_memory_pool_new(NUM_DB_ENTRIES_FOR_POOL_BLOCK,
                                                             db_entry_get_sizeof_folder_entry(),
                                                             (GDestroyNotify)db_entry_destroy);

    if (!load_header(fp)) {
        goto load_fail;
    }

    uint64_t index_flags = 0;
    if (!read_element_from_file(&index_flags, 8, fp)) {
        goto load_fail;
    }

    uint32_t num_folders = 0;
    if (!read_element_from_file(&num_folders, 4, fp)) {
        goto load_fail;
    }

    uint32_t num_files = 0;
    if (!read_element_from_file(&num_files, 4, fp)) {
        goto load_fail;
    }
    g_debug("[db_load] load %d folders, %d files", num_folders, num_files);

    uint64_t folder_block_size = 0;
    if (!read_element_from_file(&folder_block_size, 8, fp)) {
        goto load_fail;
    }

    uint64_t file_block_size = 0;
    if (!read_element_from_file(&file_block_size, 8, fp)) {
        goto load_fail;
    }
    g_debug("[db_load] folder size: %lu, file size: %lu", folder_block_size, file_block_size);

    // TODO: implement index loading
    uint32_t num_indexes = 0;
    if (!read_element_from_file(&num_indexes, 4, fp)) {
        goto load_fail;
    }

    // TODO: implement exclude loading
    uint32_t num_excludes = 0;
    if (!read_element_from_file(&num_excludes, 4, fp)) {
        goto load_fail;
    }

    // pre-allocate the folders array, so we can later map parent indices to the corresponding pointers
    sorted_folders[DATABASE_INDEX_PROPERTY_NAME] = darray_new(num_folders);
    folders = sorted_folders[DATABASE_INDEX_PROPERTY_NAME];

    for (uint32_t i = 0; i < num_folders; i++) {
        FsearchDatabaseEntryFolder *folder = fsearch_memory_pool_malloc(folder_pool);
        FsearchDatabaseEntry *entry = (FsearchDatabaseEntry *)folder;
        db_entry_set_idx(entry, i);
        db_entry_set_type(entry, DATABASE_ENTRY_TYPE_FOLDER);
        db_entry_set_parent(entry, NULL);
        darray_add_item(folders, folder);
    }

    if (status_cb) {
        status_cb(_("Loading folders…"));
    }
    // load folders
    if (!load_folders(fp, index_flags, folders, num_folders, folder_block_size)) {
        goto load_fail;
    }

    if (status_cb) {
        status_cb(_("Loading files…"));
    }
    // load files
    sorted_files[DATABASE_INDEX_PROPERTY_NAME] = darray_new(num_files);
    files = sorted_files[DATABASE_INDEX_PROPERTY_NAME];
    if (!load_files(fp, index_flags, file_pool, folders, files, num_files, file_block_size)) {
        goto load_fail;
    }

    if (!load_sorted_arrays(fp, sorted_folders, sorted_files)) {
        goto load_fail;
    }

    // FsearchDatabaseIndexStore *store = fsearch_database_index_store_new(index_flags);
    //  FsearchDatabaseIndex *index = calloc(1, sizeof(FsearchDatabaseIndex));
    // g_assert(index);

    // for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; i++) {
    //     index->files[i] = g_steal_pointer(&sorted_files[i]);
    //     index->folders[i] = g_steal_pointer(&sorted_folders[i]);
    // }
    // index->file_pool = g_steal_pointer(&file_pool);
    // index->folder_pool = g_steal_pointer(&folder_pool);

    // index->flags = index_flags;

    //*index_out = index;

    g_clear_pointer(&fp, fclose);

    return true;

load_fail:
    g_debug("[db_load] load failed");

    g_clear_pointer(&fp, fclose);

    for (uint32_t i = 0; i < NUM_DATABASE_INDEX_PROPERTIES; i++) {
        g_clear_pointer(&sorted_folders[i], darray_unref);
        g_clear_pointer(&sorted_files[i], darray_unref);
    }
    g_clear_pointer(&file_pool, fsearch_memory_pool_free_pool);
    g_clear_pointer(&folder_pool, fsearch_memory_pool_free_pool);

    return false;
}

static void
search_view_free(FsearchDatabaseSearchView *view) {
    g_return_if_fail(view);
    g_clear_pointer(&view->query, fsearch_query_unref);
    g_clear_pointer(&view->file_container, fsearch_database_entries_container_unref);
    g_clear_pointer(&view->folder_container, fsearch_database_entries_container_unref);
    g_clear_pointer(&view->file_selection, fsearch_selection_free);
    g_clear_pointer(&view->folder_selection, fsearch_selection_free);
    g_clear_pointer(&view, free);
}

static FsearchDatabaseSearchView *
search_view_new(FsearchQuery *query,
                DynamicArray *files,
                DynamicArray *folders,
                GHashTable *old_selection,
                FsearchDatabaseIndexProperty sort_order,
                FsearchDatabaseIndexProperty secondary_sort_order,
                GtkSortType sort_type) {
    FsearchDatabaseSearchView *view = calloc(1, sizeof(FsearchDatabaseSearchView));
    g_assert(view);
    view->query = fsearch_query_ref(query);
    view->folder_container = fsearch_database_entries_container_new(folders,
                                                                    TRUE,
                                                                    sort_order,
                                                                    secondary_sort_order,
                                                                    DATABASE_ENTRY_TYPE_FOLDER,
                                                                    NULL);
    view->file_container = fsearch_database_entries_container_new(files,
                                                                  TRUE,
                                                                  sort_order,
                                                                  secondary_sort_order,
                                                                  DATABASE_ENTRY_TYPE_FILE,
                                                                  NULL);
    view->sort_order = sort_order;
    view->secondardy_sort_order = secondary_sort_order;
    view->sort_type = sort_type;
    view->file_selection = fsearch_selection_new();
    view->folder_selection = fsearch_selection_new();
    return view;
}

static void
quit_work_queue(FsearchDatabase *self) {
    g_async_queue_push(self->work_queue, fsearch_database_work_new_quit());
}

static void
signal_emit_context_free(FsearchSignalEmitContext *ctx) {
    if (ctx->arg1_free_func) {
        g_clear_pointer(&ctx->arg1, ctx->arg1_free_func);
    }
    if (ctx->arg2_free_func) {
        g_clear_pointer(&ctx->arg2, ctx->arg2_free_func);
    }
    g_clear_object(&ctx->db);
    g_clear_pointer(&ctx, free);
}

static FsearchSignalEmitContext *
signal_emit_context_new(FsearchDatabase *db,
                        FsearchDatabaseEventType type,
                        gpointer arg1,
                        gpointer arg2,
                        guint n_args,
                        GDestroyNotify arg1_free_func,
                        GDestroyNotify arg2_free_func) {
    FsearchSignalEmitContext *ctx = calloc(1, sizeof(FsearchSignalEmitContext));
    g_assert(ctx != NULL);

    ctx->db = g_object_ref(db);
    ctx->type = type;
    ctx->arg1 = arg1;
    ctx->arg2 = arg2;
    ctx->n_args = n_args;
    ctx->arg1_free_func = arg1_free_func;
    ctx->arg2_free_func = arg2_free_func;
    return ctx;
}

static gboolean
emit_signal_cb(gpointer user_data) {
    FsearchSignalEmitContext *ctx = user_data;

    switch (ctx->n_args) {
    case 0:
        g_signal_emit(ctx->db, signals[ctx->type], 0);
        break;
    case 1:
        g_signal_emit(ctx->db, signals[ctx->type], 0, ctx->arg1);
        break;
    case 2:
        g_signal_emit(ctx->db, signals[ctx->type], 0, ctx->arg1, ctx->arg2);
        break;
    default:
        g_assert_not_reached();
    }

    g_clear_pointer(&ctx, signal_emit_context_free);

    return G_SOURCE_REMOVE;
}

static void
emit_signal0(FsearchDatabase *self, FsearchDatabaseEventType type) {
    g_idle_add(emit_signal_cb, signal_emit_context_new(self, type, NULL, NULL, 0, NULL, NULL));
}

static void
emit_signal(FsearchDatabase *self,
            FsearchDatabaseEventType type,
            gpointer arg1,
            gpointer arg2,
            guint n_args,
            GDestroyNotify arg1_free_func,
            GDestroyNotify arg2_free_func) {
    g_idle_add(emit_signal_cb, signal_emit_context_new(self, type, arg1, arg2, n_args, arg1_free_func, arg2_free_func));
}

static void
emit_item_info_ready_signal(FsearchDatabase *self, guint id, FsearchDatabaseEntryInfo *info) {
    emit_signal(self,
                EVENT_ITEM_INFO_READY,
                GUINT_TO_POINTER(id),
                info,
                2,
                NULL,
                (GDestroyNotify)fsearch_database_entry_info_unref);
}

static void
emit_search_finished_signal(FsearchDatabase *self, guint id, FsearchDatabaseSearchInfo *info) {
    emit_signal(self,
                EVENT_SEARCH_FINISHED,
                GUINT_TO_POINTER(id),
                info,
                2,
                NULL,
                (GDestroyNotify)fsearch_database_search_info_unref);
}

static void
emit_sort_finished_signal(FsearchDatabase *self, guint id, FsearchDatabaseSearchInfo *info) {
    emit_signal(self,
                EVENT_SORT_FINISHED,
                GUINT_TO_POINTER(id),
                info,
                2,
                NULL,
                (GDestroyNotify)fsearch_database_search_info_unref);
}

static void
emit_selection_changed_signal(FsearchDatabase *self, guint id, FsearchDatabaseSearchInfo *info) {
    emit_signal(self,
                EVENT_SELECTION_CHANGED,
                GUINT_TO_POINTER(id),
                info,
                2,
                NULL,
                (GDestroyNotify)fsearch_database_search_info_unref);
}

static void
emit_database_changed_signal(FsearchDatabase *self, FsearchDatabaseInfo *info) {
    emit_signal(self, EVENT_DATABASE_CHANGED, info, NULL, 1, NULL, (GDestroyNotify)fsearch_database_info_unref);
}

static void
database_unlock(FsearchDatabase *self) {
    g_assert(FSEARCH_IS_DATABASE(self));
    g_mutex_unlock(&self->mutex);
}

static void
database_lock(FsearchDatabase *self) {
    g_assert(FSEARCH_IS_DATABASE(self));
    g_mutex_lock(&self->mutex);
}

static uint32_t
get_num_search_file_results(FsearchDatabaseSearchView *view) {
    return view && view->file_container ? fsearch_database_entries_container_get_num_entries(view->file_container) : 0;
}

static uint32_t
get_num_search_folder_results(FsearchDatabaseSearchView *view) {
    return view && view->file_container ? fsearch_database_entries_container_get_num_entries(view->folder_container) : 0;
}

FsearchDatabaseExcludeManager *
get_exclude_manager(FsearchDatabase *self) {
    return self->store ? self->store->exclude_manager : NULL;
}

FsearchDatabaseIncludeManager *
get_include_manager(FsearchDatabase *self) {
    return self->store ? self->store->include_manager : NULL;
}

static uint32_t
get_num_database_files(FsearchDatabase *self) {
    return self->store ? index_store_get_num_files(self->store) : 0;
}

static uint32_t
get_num_database_folders(FsearchDatabase *self) {
    return self->store ? index_store_get_num_folders(self->store) : 0;
}

static FsearchDatabaseInfo *
get_database_info(FsearchDatabase *self) {
    g_return_val_if_fail(self, NULL);
    return fsearch_database_info_new(get_include_manager(self),
                                     get_exclude_manager(self),
                                     get_num_database_files(self),
                                     get_num_database_folders(self));
}

static GFile *
get_default_database_file() {
    return g_file_new_build_filename(g_get_user_data_dir(), "fsearch", "fsearch.db", NULL);
}

static uint32_t
get_idx_for_sort_type(uint32_t idx, uint32_t num_files, uint32_t num_folders, GtkSortType sort_type) {
    if (sort_type == GTK_SORT_DESCENDING) {
        return num_folders + num_files - (idx + 1);
    }
    return idx;
}

static FsearchDatabaseEntry *
get_entry_for_idx(FsearchDatabaseSearchView *view, uint32_t idx) {
    if (!view->folder_container) {
        return NULL;
    }
    if (!view->file_container) {
        return NULL;
    }
    const uint32_t num_folders = get_num_search_folder_results(view);
    const uint32_t num_files = get_num_search_file_results(view);

    idx = get_idx_for_sort_type(idx, num_files, num_folders, view->sort_type);

    if (idx < num_folders) {
        return fsearch_database_entries_container_get_entry(view->folder_container, idx);
    }
    idx -= num_folders;
    if (idx < num_files) {
        return fsearch_database_entries_container_get_entry(view->file_container, idx);
    }
    return NULL;
}

static bool
is_selected(FsearchDatabaseSearchView *view, FsearchDatabaseEntry *entry) {
    if (db_entry_get_type(entry) == DATABASE_ENTRY_TYPE_FILE) {
        return fsearch_selection_is_selected(view->file_selection, entry);
    }
    else {
        return fsearch_selection_is_selected(view->folder_selection, entry);
    }
}

static FsearchDatabaseSearchView *
get_search_view(FsearchDatabase *self, uint32_t view_id) {
    return g_hash_table_lookup(self->search_results, GUINT_TO_POINTER(view_id));
}

static FsearchResult
get_entry_info_non_blocking(FsearchDatabase *self, FsearchDatabaseWork *work, FsearchDatabaseEntryInfo **info_out) {
    g_return_val_if_fail(self, FSEARCH_RESULT_FAILED);
    g_return_val_if_fail(work, FSEARCH_RESULT_FAILED);
    g_return_val_if_fail(info_out, FSEARCH_RESULT_FAILED);

    const uint32_t idx = fsearch_database_work_item_info_get_index(work);
    const uint32_t id = fsearch_database_work_get_view_id(work);

    FsearchDatabaseSearchView *view = get_search_view(self, id);
    if (!view) {
        return FSEARCH_RESULT_DB_UNKOWN_SEARCH_VIEW;
    }

    const FsearchDatabaseEntryInfoFlags flags = fsearch_database_work_item_info_get_flags(work);

    FsearchDatabaseEntry *entry = get_entry_for_idx(view, idx);
    if (!entry) {
        return FSEARCH_RESULT_DB_ENTRY_NOT_FOUND;
    }

    *info_out = fsearch_database_entry_info_new(entry, view->query, idx, is_selected(view, entry), flags);
    return FSEARCH_RESULT_SUCCESS;
}

static FsearchResult
get_entry_info(FsearchDatabase *self, FsearchDatabaseWork *work, FsearchDatabaseEntryInfo **info_out) {
    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);
    return get_entry_info_non_blocking(self, work, info_out);
}

static void
sort_database(FsearchDatabase *self, FsearchDatabaseWork *work) {
    g_return_if_fail(self);

    const uint32_t id = fsearch_database_work_get_view_id(work);
    const FsearchDatabaseIndexProperty sort_order = fsearch_database_work_sort_get_sort_order(work);
    const GtkSortType sort_type = fsearch_database_work_sort_get_sort_type(work);
    g_autoptr(GCancellable) cancellable = fsearch_database_work_get_cancellable(work);

    emit_signal(self, EVENT_SORT_STARTED, GUINT_TO_POINTER(id), NULL, 1, NULL, NULL);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    FsearchDatabaseSearchView *view = get_search_view(self, id);
    if (!view) {
        return;
    }

    g_autoptr(DynamicArray) files_new = NULL;
    g_autoptr(DynamicArray) folders_new = NULL;

    g_autoptr(FsearchDatabaseEntriesContainer) files_fast_sort_index = index_store_get_files(self->store, sort_order);
    g_autoptr(FsearchDatabaseEntriesContainer) folders_fast_sort_index = index_store_get_folders(self->store, sort_order);

    g_autoptr(DynamicArray) files_fast_sorted = NULL;
    g_autoptr(DynamicArray) folders_fast_sorted = NULL;
    if (files_fast_sort_index && folders_fast_sort_index) {
        files_fast_sorted = fsearch_database_entries_container_get_joined(files_fast_sort_index);
        folders_fast_sorted = fsearch_database_entries_container_get_joined(folders_fast_sort_index);
    }
    g_autoptr(DynamicArray) files_in = fsearch_database_entries_container_get_joined(view->file_container);
    g_autoptr(DynamicArray) folders_in = fsearch_database_entries_container_get_joined(view->folder_container);

    fsearch_database_sort_results(view->sort_order,
                                  view->secondardy_sort_order,
                                  sort_order,
                                  files_in,
                                  folders_in,
                                  files_fast_sorted,
                                  folders_fast_sorted,
                                  &files_new,
                                  &folders_new,
                                  &view->sort_order,
                                  &view->secondardy_sort_order,
                                  cancellable);

    if (files_new) {
        g_clear_pointer(&view->file_container, fsearch_database_entries_container_unref);
        view->file_container = fsearch_database_entries_container_new(files_new,
                                                                      TRUE,
                                                                      view->sort_order,
                                                                      view->secondardy_sort_order,
                                                                      DATABASE_ENTRY_TYPE_FILE,
                                                                      NULL);
        view->sort_type = sort_type;
    }
    if (folders_new) {
        g_clear_pointer(&view->folder_container, fsearch_database_entries_container_unref);
        view->folder_container = fsearch_database_entries_container_new(folders_new,
                                                                        TRUE,
                                                                        view->sort_order,
                                                                        view->secondardy_sort_order,
                                                                        DATABASE_ENTRY_TYPE_FOLDER,
                                                                        NULL);
        view->sort_type = sort_type;
    }

    emit_sort_finished_signal(self,
                              id,
                              fsearch_database_search_info_new(fsearch_query_ref(view->query),
                                                               get_num_search_file_results(view),
                                                               get_num_search_folder_results(view),
                                                               fsearch_selection_get_num_selected(view->file_selection),
                                                               fsearch_selection_get_num_selected(view->folder_selection),
                                                               view->sort_order,
                                                               view->sort_type));
}

static bool
search_database(FsearchDatabase *self, FsearchDatabaseWork *work) {
    g_return_val_if_fail(self, false);

    const uint32_t id = fsearch_database_work_get_view_id(work);

    g_autoptr(FsearchQuery) query = fsearch_database_work_search_get_query(work);
    FsearchDatabaseIndexProperty sort_order = fsearch_database_work_search_get_sort_order(work);
    const GtkSortType sort_type = fsearch_database_work_search_get_sort_type(work);
    g_autoptr(GCancellable) cancellable = fsearch_database_work_get_cancellable(work);
    uint32_t num_files = 0;
    uint32_t num_folders = 0;

    database_lock(self);

    if (!self->store) {
        database_unlock(self);
        return false;
    }

    emit_signal(self, EVENT_SEARCH_STARTED, GUINT_TO_POINTER(id), NULL, 1, NULL, NULL);

    bool result = false;

    g_autoptr(FsearchDatabaseEntriesContainer) file_container = index_store_get_files(self->store, sort_order);
    g_autoptr(FsearchDatabaseEntriesContainer) folder_container = index_store_get_folders(self->store, sort_order);

    if (!file_container && !folder_container) {
        sort_order = DATABASE_INDEX_PROPERTY_NAME;
        file_container = index_store_get_files(self->store, sort_order);
        folder_container = index_store_get_folders(self->store, sort_order);
    }

    g_autoptr(DynamicArray) files = fsearch_database_entries_container_get_joined(file_container);
    g_autoptr(DynamicArray) folders = fsearch_database_entries_container_get_joined(folder_container);

    DatabaseSearchResult *search_result = fsearch_query_matches_everything(query)
                                            ? db_search_empty(folders, files)
                                            : db_search(query, self->thread_pool, folders, files, cancellable);
    if (search_result) {
        num_files = search_result->files ? darray_get_num_items(search_result->files) : 0;
        num_folders = search_result->folders ? darray_get_num_items(search_result->folders) : 0;

        // After searching the secondary sort order will always be NONE, because we only search in pre-sorted indexes
        FsearchDatabaseSearchView *view = search_view_new(query,
                                                          search_result->files,
                                                          search_result->folders,
                                                          NULL,
                                                          sort_order,
                                                          DATABASE_INDEX_PROPERTY_NONE,
                                                          sort_type);
        g_hash_table_insert(self->search_results, GUINT_TO_POINTER(id), view);

        g_clear_pointer(&search_result->files, darray_unref);
        g_clear_pointer(&search_result->folders, darray_unref);
        g_clear_pointer(&search_result, free);
        result = true;
    }

    database_unlock(self);

    emit_search_finished_signal(
        self,
        id,
        fsearch_database_search_info_new(query, num_files, num_folders, 0, 0, sort_order, sort_type));

    return result;
}

static void
toggle_range(FsearchDatabaseSearchView *view, int32_t start_idx, int32_t end_idx) {
    int32_t tmp = start_idx;
    if (start_idx > end_idx) {
        start_idx = end_idx;
        end_idx = tmp;
    }
    for (int32_t i = start_idx; i <= end_idx; ++i) {
        FsearchDatabaseEntry *entry = get_entry_for_idx(view, i);
        if (!entry) {
            continue;
        }
        FsearchDatabaseEntryType type = db_entry_get_type(entry);
        if (type == DATABASE_ENTRY_TYPE_FILE) {
            fsearch_selection_select_toggle(view->file_selection, entry);
        }
        else {
            fsearch_selection_select_toggle(view->folder_selection, entry);
        }
    }
}

static void
select_range(FsearchDatabaseSearchView *view, int32_t start_idx, int32_t end_idx) {
    int32_t tmp = start_idx;
    if (start_idx > end_idx) {
        start_idx = end_idx;
        end_idx = tmp;
    }
    for (int32_t i = start_idx; i <= end_idx; ++i) {
        FsearchDatabaseEntry *entry = get_entry_for_idx(view, i);
        if (!entry) {
            continue;
        }
        FsearchDatabaseEntryType type = db_entry_get_type(entry);
        if (type == DATABASE_ENTRY_TYPE_FILE) {
            fsearch_selection_select(view->file_selection, entry);
        }
        else {
            fsearch_selection_select(view->folder_selection, entry);
        }
    }
}

static void
search_views_updated_cb(gpointer key, gpointer value, gpointer user_data) {
    FsearchDatabase *self = user_data;
    g_return_if_fail(self);
    g_return_if_fail(FSEARCH_IS_DATABASE(self));

    FsearchDatabaseSearchView *view = value;
    g_return_if_fail(view);

    emit_selection_changed_signal(
        self,
        GPOINTER_TO_INT(key),
        fsearch_database_search_info_new(fsearch_query_ref(view->query),
                                         get_num_search_file_results(view),
                                         get_num_search_folder_results(view),
                                         fsearch_selection_get_num_selected(view->file_selection),
                                         fsearch_selection_get_num_selected(view->folder_selection),
                                         view->sort_order,
                                         view->sort_type));
}

static bool
entry_matches_query(FsearchDatabaseSearchView *view, FsearchDatabaseEntry *entry) {
    FsearchQueryMatchData *match_data = fsearch_query_match_data_new();
    fsearch_query_match_data_set_entry(match_data, entry);

    const bool found = fsearch_query_match(view->query, match_data);
    g_clear_pointer(&match_data, fsearch_query_match_data_free);
    return found;
}

typedef struct {
    FsearchDatabase *db;
    FsearchDatabaseIndex *index;
    FsearchDatabaseEntry *entry;
    DynamicArray *folders;
    DynamicArray *files;
} FsearchDatabaseAddRemoveContext;

static bool
search_view_result_add(FsearchDatabaseEntry *entry, FsearchDatabaseSearchView *view) {
    if (!entry || !entry_matches_query(view, entry)) {
        return true;
    }

    fsearch_database_entries_container_insert(db_entry_is_folder(entry) ? view->folder_container : view->file_container,
                                              entry);
    return true;
}

static void
search_view_results_add_cb(gpointer key, gpointer value, gpointer user_data) {
    FsearchDatabaseAddRemoveContext *ctx = user_data;
    g_return_if_fail(ctx);

    FsearchDatabaseSearchView *view = value;
    g_return_if_fail(view);

    if (ctx->files && view->file_container && !index_store_has_container(ctx->db->store, view->file_container)) {
        darray_for_each(ctx->files, (DynamicArrayForEachFunc)search_view_result_add, view);
    }
    if (ctx->folders && view->folder_container && !index_store_has_container(ctx->db->store, view->folder_container)) {
        darray_for_each(ctx->folders, (DynamicArrayForEachFunc)search_view_result_add, view);
    }
}

static bool
search_view_result_remove(FsearchDatabaseEntry *entry, FsearchDatabaseSearchView *view) {
    if (!entry || !entry_matches_query(view, entry)) {
        return true;
    }

    FsearchDatabaseEntriesContainer *container = NULL;
    GHashTable *selection = NULL;
    if (db_entry_is_folder(entry)) {
        container = view->folder_container;
        selection = view->folder_selection;
    }
    else {
        container = view->file_container;
        selection = view->file_selection;
    }

    // Remove it from search results
    fsearch_database_entries_container_steal(container, entry);

    // Remove it from the selection
    fsearch_selection_unselect(selection, entry);

    return true;
}

static void
search_view_results_remove_cb(gpointer key, gpointer value, gpointer user_data) {
    FsearchDatabaseAddRemoveContext *ctx = user_data;
    g_return_if_fail(ctx);

    FsearchDatabaseSearchView *view = value;
    g_return_if_fail(view);

    if (ctx->files && view->file_container && !index_store_has_container(ctx->db->store, view->file_container)) {
        darray_for_each(ctx->files, (DynamicArrayForEachFunc)search_view_result_remove, view);
    }
    if (ctx->folders && view->folder_container && !index_store_has_container(ctx->db->store, view->folder_container)) {
        darray_for_each(ctx->folders, (DynamicArrayForEachFunc)search_view_result_remove, view);
    }
}

static void
index_event_cb(FsearchDatabaseIndex *index, FsearchDatabaseIndexEvent *event, gpointer user_data) {
    FsearchDatabase *self = FSEARCH_DATABASE(user_data);

    FsearchDatabaseAddRemoveContext ctx = {
        .db = self,
        .index = index,
        .folders = event->folders,
        .files = event->files,
    };

    switch (event->kind) {
    case FSEARCH_DATABASE_INDEX_EVENT_START_MODIFYING:
        database_lock(self);
        break;
    case FSEARCH_DATABASE_INDEX_EVENT_END_MODIFYING:
        g_hash_table_foreach(self->search_results, search_views_updated_cb, self);
        emit_database_changed_signal(self, get_database_info(self));
        database_unlock(self);
        break;
    case FSEARCH_DATABASE_INDEX_EVENT_SCAN_STARTED:
        break;
    case FSEARCH_DATABASE_INDEX_EVENT_SCAN_FINISHED:
        break;
    case FSEARCH_DATABASE_INDEX_EVENT_MONITORING_STARTED:
        break;
    case FSEARCH_DATABASE_INDEX_EVENT_MONITORING_FINISHED:
        break;
    case FSEARCH_DATABASE_INDEX_EVENT_ENTRY_CREATED:
        g_hash_table_foreach(self->search_results, search_view_results_add_cb, &ctx);
        index_store_add_entries(self->store, event->folders, true);
        index_store_add_entries(self->store, event->files, false);
        break;
    case FSEARCH_DATABASE_INDEX_EVENT_ENTRY_DELETED:
        g_hash_table_foreach(self->search_results, search_view_results_remove_cb, &ctx);
        index_store_remove_folders(self->store, event->folders, index);
        index_store_remove_files(self->store, event->files, index);
        break;
    case NUM_FSEARCH_DATABASE_INDEX_EVENTS:
        break;
    }
}

static void
modify_selection(FsearchDatabase *self, FsearchDatabaseWork *work) {
    g_return_if_fail(self);
    g_return_if_fail(work);
    const uint32_t view_id = fsearch_database_work_get_view_id(work);
    const FsearchSelectionType type = fsearch_database_work_modify_selection_get_type(work);
    const int32_t start_idx = fsearch_database_work_modify_selection_get_start_idx(work);
    const int32_t end_idx = fsearch_database_work_modify_selection_get_end_idx(work);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    FsearchDatabaseSearchView *view = get_search_view(self, view_id);
    if (!view) {
        return;
    }

    FsearchDatabaseEntry *entry = get_entry_for_idx(view, start_idx);
    if (!entry) {
        return;
    }

    g_autoptr(DynamicArray) file_container = fsearch_database_entries_container_get_containers(view->file_container);
    g_autoptr(DynamicArray) folder_container = fsearch_database_entries_container_get_containers(view->folder_container);

    switch (type) {
    case FSEARCH_SELECTION_TYPE_CLEAR:
        fsearch_selection_unselect_all(view->file_selection);
        fsearch_selection_unselect_all(view->folder_selection);
        break;
    case FSEARCH_SELECTION_TYPE_ALL:
        for (uint32_t i = 0; i < darray_get_num_items(file_container); ++i) {
            fsearch_selection_select_all(view->file_selection, darray_get_item(file_container, i));
        }
        for (uint32_t i = 0; i < darray_get_num_items(folder_container); ++i) {
            fsearch_selection_select_all(view->folder_selection, darray_get_item(folder_container, i));
        }
        break;
    case FSEARCH_SELECTION_TYPE_INVERT:
        for (uint32_t i = 0; i < darray_get_num_items(file_container); ++i) {
            fsearch_selection_invert(view->file_selection, darray_get_item(file_container, i));
        }
        for (uint32_t i = 0; i < darray_get_num_items(folder_container); ++i) {
            fsearch_selection_invert(view->folder_selection, darray_get_item(folder_container, i));
        }
        break;
    case FSEARCH_SELECTION_TYPE_SELECT:
        if (db_entry_get_type(entry) == DATABASE_ENTRY_TYPE_FILE) {
            fsearch_selection_select(view->file_selection, entry);
        }
        else {
            fsearch_selection_select(view->folder_selection, entry);
        }
        break;
    case FSEARCH_SELECTION_TYPE_TOGGLE:
        if (db_entry_get_type(entry) == DATABASE_ENTRY_TYPE_FILE) {
            fsearch_selection_select_toggle(view->file_selection, entry);
        }
        else {
            fsearch_selection_select_toggle(view->folder_selection, entry);
        }
        break;
    case FSEARCH_SELECTION_TYPE_SELECT_RANGE:
        select_range(view, start_idx, end_idx);
        break;
    case FSEARCH_SELECTION_TYPE_TOGGLE_RANGE:
        toggle_range(view, start_idx, end_idx);
        break;
    case NUM_FSEARCH_SELECTION_TYPES:
        g_assert_not_reached();
    }

    emit_selection_changed_signal(
        self,
        view_id,
        fsearch_database_search_info_new(fsearch_query_ref(view->query),
                                         get_num_search_file_results(view),
                                         get_num_search_folder_results(view),
                                         fsearch_selection_get_num_selected(view->file_selection),
                                         fsearch_selection_get_num_selected(view->folder_selection),
                                         view->sort_order,
                                         view->sort_type));
}

static void
save_database_to_file(FsearchDatabase *self) {
    g_return_if_fail(self);
    g_return_if_fail(self->file);

    // g_autoptr(GFile) db_directory = g_file_get_parent(self->file);
    // g_autofree gchar *db_directory_path = g_file_get_path(db_directory);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);
    // db_file_save(self->store, NULL);
}

static void
rescan_database(FsearchDatabase *self) {
    g_return_if_fail(self);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    emit_signal0(self, EVENT_SCAN_STARTED);

    const FsearchDatabaseIndexPropertyFlags flags = self->flags;

    g_clear_pointer(&locker, g_mutex_locker_free);

    g_autoptr(FsearchDatabaseIndexStore)
        store = index_store_new(get_include_manager(self), get_exclude_manager(self), flags, index_event_cb, self);
    g_return_if_fail(store);
    index_store_start(store, NULL);

    locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    self->flags = flags;
    g_clear_pointer(&self->store, index_store_unref);
    self->store = g_steal_pointer(&store);

    index_store_start_monitoring(self->store);

    g_hash_table_remove_all(self->search_results);

#ifdef HAVE_MALLOC_TRIM
    malloc_trim(0);
#endif

    emit_signal(self, EVENT_SCAN_FINISHED, get_database_info(self), NULL, 1, (GDestroyNotify)fsearch_database_info_unref, NULL);
}

static void
scan_database(FsearchDatabase *self, FsearchDatabaseWork *work) {
    g_return_if_fail(self);
    g_return_if_fail(work);

    g_autoptr(FsearchDatabaseIncludeManager) include_manager = fsearch_database_work_scan_get_include_manager(work);
    g_autoptr(FsearchDatabaseExcludeManager) exclude_manager = fsearch_database_work_scan_get_exclude_manager(work);
    const FsearchDatabaseIndexPropertyFlags flags = fsearch_database_work_scan_get_flags(work);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    if (self->store && fsearch_database_include_manager_equal(get_include_manager(self), include_manager)
        && fsearch_database_exclude_manager_equal(get_exclude_manager(self), exclude_manager)) {
        g_debug("[scan] new config is identical to the current one. No scan necessary.");
        return;
    }

    g_clear_pointer(&locker, g_mutex_locker_free);

    emit_signal0(self, EVENT_SCAN_STARTED);

    g_autoptr(FsearchDatabaseIndexStore)
        store = index_store_new(include_manager, exclude_manager, flags, index_event_cb, self);
    g_return_if_fail(store);
    index_store_start(store, NULL);

    locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    self->flags = flags;
    g_clear_pointer(&self->store, index_store_unref);
    self->store = g_steal_pointer(&store);

    index_store_start_monitoring(self->store);

    g_hash_table_remove_all(self->search_results);

#ifdef HAVE_MALLOC_TRIM
    malloc_trim(0);
#endif

    emit_signal(self, EVENT_SCAN_FINISHED, get_database_info(self), NULL, 1, (GDestroyNotify)fsearch_database_info_unref, NULL);
}

static void
load_database_from_file(FsearchDatabase *self) {
    g_return_if_fail(self);
    g_return_if_fail(self->file);

    emit_signal0(self, EVENT_LOAD_STARTED);

    g_autofree gchar *file_path = g_file_get_path(self->file);
    g_return_if_fail(file_path);

    g_autoptr(FsearchDatabaseIndexStore) store = NULL;
    bool res = false;
    // bool res = db_file_load(file_path, NULL, &store, &include_manager, &exclude_manager);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    if (res) {
        g_clear_pointer(&self->store, index_store_unref);
        self->store = g_steal_pointer(&store);
        // g_set_object(&self->include_manager, include_manager);
        // g_set_object(&self->exclude_manager, exclude_manager);
    }
    else {
        // g_set_object(&self->include_manager, fsearch_database_include_manager_new_with_defaults());
        // g_set_object(&self->exclude_manager, fsearch_database_exclude_manager_new_with_defaults());
    }

    emit_signal(self, EVENT_LOAD_FINISHED, get_database_info(self), NULL, 1, (GDestroyNotify)fsearch_database_info_unref, NULL);
}

static gpointer
work_queue_thread(gpointer data) {
    g_debug("manager thread started");
    FsearchDatabase *self = data;

    while (TRUE) {
        g_autoptr(FsearchDatabaseWork) work = g_async_queue_pop(self->work_queue);
        if (!work) {
            break;
        }

        g_autoptr(GTimer) timer = g_timer_new();
        g_timer_start(timer);

        bool quit = false;

        switch (fsearch_database_work_get_kind(work)) {
        case FSEARCH_DATABASE_WORK_QUIT:
            quit = true;
            break;
        case FSEARCH_DATABASE_WORK_LOAD_FROM_FILE:
            load_database_from_file(self);
            break;
        case FSEARCH_DATABASE_WORK_GET_ITEM_INFO: {
            FsearchDatabaseEntryInfo *info = NULL;
            get_entry_info(self, work, &info);
            if (info) {
                emit_item_info_ready_signal(self, fsearch_database_work_get_view_id(work), g_steal_pointer(&info));
            }
            break;
        }
        case FSEARCH_DATABASE_WORK_RESCAN:
            rescan_database(self);
            break;
        case FSEARCH_DATABASE_WORK_SAVE_TO_FILE:
            emit_signal0(self, EVENT_SAVE_STARTED);
            save_database_to_file(self);
            emit_signal0(self, EVENT_SAVE_FINISHED);
            break;
        case FSEARCH_DATABASE_WORK_SCAN:
            scan_database(self, work);
            break;
        case FSEARCH_DATABASE_WORK_SEARCH:
            search_database(self, work);
            break;
        case FSEARCH_DATABASE_WORK_SORT:
            sort_database(self, work);
            break;
        case FSEARCH_DATABASE_WORK_MODIFY_SELECTION:
            modify_selection(self, work);
            break;
        default:
            g_assert_not_reached();
        }

        g_debug("finished work '%s' in: %fs.", fsearch_database_work_to_string(work), g_timer_elapsed(timer, NULL));

        if (quit) {
            break;
        }
    }

    g_debug("manager thread returning");
    return NULL;
}

static void
fsearch_database_constructed(GObject *object) {
    FsearchDatabase *self = (FsearchDatabase *)object;

    g_assert(FSEARCH_IS_DATABASE(self));

    G_OBJECT_CLASS(fsearch_database_parent_class)->constructed(object);

    if (self->file == NULL) {
        self->file = get_default_database_file();
    }

    g_async_queue_push(self->work_queue, fsearch_database_work_new_load());
}

static void
fsearch_database_dispose(GObject *object) {
    FsearchDatabase *self = (FsearchDatabase *)object;

    // Notify work queue thread to exit itself
    quit_work_queue(self);
    g_thread_join(self->work_queue_thread);

    G_OBJECT_CLASS(fsearch_database_parent_class)->dispose(object);
}

static void
fsearch_database_finalize(GObject *object) {
    FsearchDatabase *self = (FsearchDatabase *)object;

    database_lock(self);
    g_clear_pointer(&self->work_queue, g_async_queue_unref);
    g_clear_pointer(&self->thread_pool, fsearch_thread_pool_free);

    g_clear_object(&self->file);

    g_clear_pointer(&self->search_results, g_hash_table_unref);
    g_clear_pointer(&self->store, index_store_unref);
    database_unlock(self);

    g_mutex_clear(&self->mutex);

    G_OBJECT_CLASS(fsearch_database_parent_class)->finalize(object);
}

static void
fsearch_database_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec) {
    FsearchDatabase *self = FSEARCH_DATABASE(object);

    switch (prop_id) {
    case PROP_FILE:
        g_value_set_object(value, self->file);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
fsearch_database_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec) {
    FsearchDatabase *self = FSEARCH_DATABASE(object);

    switch (prop_id) {
    case PROP_FILE:
        self->file = g_value_get_object(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void
fsearch_database_class_init(FsearchDatabaseClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->constructed = fsearch_database_constructed;
    object_class->dispose = fsearch_database_dispose;
    object_class->finalize = fsearch_database_finalize;
    object_class->set_property = fsearch_database_set_property;
    object_class->get_property = fsearch_database_get_property;

    properties[PROP_FILE] = g_param_spec_object("file",
                                                "File",
                                                "The file where the database will be loaded from or saved to by "
                                                "default",
                                                G_TYPE_FILE,
                                                (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

    g_object_class_install_properties(object_class, NUM_PROPERTIES, properties);

    signals[EVENT_LOAD_STARTED] =
        g_signal_new("load-started", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[EVENT_LOAD_FINISHED] = g_signal_new("load-finished",
                                                G_TYPE_FROM_CLASS(klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL,
                                                NULL,
                                                NULL,
                                                G_TYPE_NONE,
                                                1,
                                                FSEARCH_TYPE_DATABASE_INFO);
    signals[EVENT_SAVE_STARTED] = g_signal_new("save-started",
                                               G_TYPE_FROM_CLASS(klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               NULL,
                                               G_TYPE_NONE,
                                               1,
                                               G_TYPE_POINTER);
    signals[EVENT_SAVE_FINISHED] = g_signal_new("save-finished",
                                                G_TYPE_FROM_CLASS(klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL,
                                                NULL,
                                                NULL,
                                                G_TYPE_NONE,
                                                1,
                                                G_TYPE_POINTER);
    signals[EVENT_SCAN_STARTED] =
        g_signal_new("scan-started", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    signals[EVENT_SCAN_FINISHED] = g_signal_new("scan-finished",
                                                G_TYPE_FROM_CLASS(klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL,
                                                NULL,
                                                NULL,
                                                G_TYPE_NONE,
                                                1,
                                                FSEARCH_TYPE_DATABASE_INFO);
    signals[EVENT_SEARCH_STARTED] = g_signal_new("search-started",
                                                 G_TYPE_FROM_CLASS(klass),
                                                 G_SIGNAL_RUN_LAST,
                                                 0,
                                                 NULL,
                                                 NULL,
                                                 NULL,
                                                 G_TYPE_NONE,
                                                 1,
                                                 G_TYPE_UINT);
    signals[EVENT_SEARCH_FINISHED] = g_signal_new("search-finished",
                                                  G_TYPE_FROM_CLASS(klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  G_TYPE_NONE,
                                                  2,
                                                  G_TYPE_UINT,
                                                  FSEARCH_TYPE_DATABASE_SEARCH_INFO);
    signals[EVENT_SORT_STARTED] = g_signal_new("sort-started",
                                               G_TYPE_FROM_CLASS(klass),
                                               G_SIGNAL_RUN_LAST,
                                               0,
                                               NULL,
                                               NULL,
                                               NULL,
                                               G_TYPE_NONE,
                                               1,
                                               G_TYPE_UINT);
    signals[EVENT_SORT_FINISHED] = g_signal_new("sort-finished",
                                                G_TYPE_FROM_CLASS(klass),
                                                G_SIGNAL_RUN_LAST,
                                                0,
                                                NULL,
                                                NULL,
                                                NULL,
                                                G_TYPE_NONE,
                                                2,
                                                G_TYPE_UINT,
                                                FSEARCH_TYPE_DATABASE_SEARCH_INFO);
    signals[EVENT_DATABASE_CHANGED] = g_signal_new("database-changed",
                                                   G_TYPE_FROM_CLASS(klass),
                                                   G_SIGNAL_RUN_LAST,
                                                   0,
                                                   NULL,
                                                   NULL,
                                                   NULL,
                                                   G_TYPE_NONE,
                                                   1,
                                                   FSEARCH_TYPE_DATABASE_INFO);
    signals[EVENT_SELECTION_CHANGED] = g_signal_new("selection-changed",
                                                    G_TYPE_FROM_CLASS(klass),
                                                    G_SIGNAL_RUN_LAST,
                                                    0,
                                                    NULL,
                                                    NULL,
                                                    NULL,
                                                    G_TYPE_NONE,
                                                    2,
                                                    G_TYPE_UINT,
                                                    FSEARCH_TYPE_DATABASE_SEARCH_INFO);
    signals[EVENT_ITEM_INFO_READY] = g_signal_new("item-info-ready",
                                                  G_TYPE_FROM_CLASS(klass),
                                                  G_SIGNAL_RUN_LAST,
                                                  0,
                                                  NULL,
                                                  NULL,
                                                  NULL,
                                                  G_TYPE_NONE,
                                                  2,
                                                  G_TYPE_UINT,
                                                  FSEARCH_TYPE_DATABASE_ENTRY_INFO);
}

static void
fsearch_database_init(FsearchDatabase *self) {
    g_mutex_init((&self->mutex));
    self->thread_pool = fsearch_thread_pool_init();
    self->search_results = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)search_view_free);
    self->work_queue = g_async_queue_new();
    self->work_queue_thread = g_thread_new("FsearchDatabaseWorkQueue", work_queue_thread, self);
}

void
fsearch_database_queue_work(FsearchDatabase *self, FsearchDatabaseWork *work) {
    g_return_if_fail(self);
    g_return_if_fail(work);

    g_async_queue_push(self->work_queue, fsearch_database_work_ref(work));
}

FsearchResult
fsearch_database_try_get_search_info(FsearchDatabase *self, uint32_t view_id, FsearchDatabaseSearchInfo **info_out) {
    g_return_val_if_fail(self, FSEARCH_RESULT_FAILED);
    g_return_val_if_fail(info_out, FSEARCH_RESULT_FAILED);

    if (!g_mutex_trylock(&self->mutex)) {
        return FSEARCH_RESULT_DB_BUSY;
    }

    FsearchResult res = FSEARCH_RESULT_FAILED;
    FsearchDatabaseSearchView *view = get_search_view(self, view_id);
    if (!view) {
        res = FSEARCH_RESULT_DB_UNKOWN_SEARCH_VIEW;
    }
    else {
        *info_out = fsearch_database_search_info_new(fsearch_query_ref(view->query),
                                                     get_num_search_file_results(view),
                                                     get_num_search_folder_results(view),
                                                     fsearch_selection_get_num_selected(view->file_selection),
                                                     fsearch_selection_get_num_selected(view->folder_selection),
                                                     view->sort_order,
                                                     view->sort_type);
        res = FSEARCH_RESULT_SUCCESS;
    }

    database_unlock(self);

    return res;
}

FsearchResult
fsearch_database_try_get_item_info(FsearchDatabase *self,
                                   uint32_t view_id,
                                   uint32_t idx,
                                   FsearchDatabaseEntryInfoFlags flags,
                                   FsearchDatabaseEntryInfo **info_out) {
    g_return_val_if_fail(self, FSEARCH_RESULT_FAILED);
    g_return_val_if_fail(info_out, FSEARCH_RESULT_FAILED);

    if (!g_mutex_trylock(&self->mutex)) {
        return FSEARCH_RESULT_DB_BUSY;
    }
    g_autoptr(FsearchDatabaseWork) work = fsearch_database_work_new_get_item_info(view_id, idx, flags);
    FsearchResult res = get_entry_info_non_blocking(self, work, info_out);

    database_unlock(self);

    return res;
}

FsearchResult
fsearch_database_try_get_database_info(FsearchDatabase *self, FsearchDatabaseInfo **info_out) {
    g_return_val_if_fail(self, FSEARCH_RESULT_FAILED);
    g_return_val_if_fail(info_out, FSEARCH_RESULT_FAILED);

    if (!g_mutex_trylock(&self->mutex)) {
        return FSEARCH_RESULT_DB_BUSY;
    }

    *info_out = get_database_info(self);

    database_unlock(self);

    return FSEARCH_RESULT_SUCCESS;
}

typedef struct {
    FsearchDatabaseForeachFunc func;
    gpointer user_data;
} FsearchDatabaseSelectionForeachContext;

static void
selection_foreach_cb(gpointer key, gpointer value, gpointer user_data) {
    FsearchDatabaseEntry *entry = value;
    if (G_UNLIKELY(!entry)) {
        return;
    }
    FsearchDatabaseSelectionForeachContext *ctx = user_data;
    ctx->func(entry, ctx->user_data);
}

void
fsearch_database_selection_foreach(FsearchDatabase *self,
                                   uint32_t view_id,
                                   FsearchDatabaseForeachFunc func,
                                   gpointer user_data) {
    g_return_if_fail(FSEARCH_IS_DATABASE(self));
    g_return_if_fail(func);

    g_autoptr(GMutexLocker) locker = g_mutex_locker_new(&self->mutex);
    g_assert_nonnull(locker);

    FsearchDatabaseSearchView *view = get_search_view(self, view_id);
    if (!view) {
        return;
    }

    FsearchDatabaseSelectionForeachContext ctx = {.func = func, .user_data = user_data};

    g_hash_table_foreach(view->folder_selection, selection_foreach_cb, &ctx);
    g_hash_table_foreach(view->file_selection, selection_foreach_cb, &ctx);
}

FsearchDatabase *
fsearch_database_new(GFile *file) {
    return g_object_new(FSEARCH_TYPE_DATABASE, "file", file, NULL);
}
