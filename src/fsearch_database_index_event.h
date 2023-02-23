#pragma once

typedef enum {
    FSEARCH_DATABASE_INDEX_EVENT_SCAN_STARTED,
    FSEARCH_DATABASE_INDEX_EVENT_SCAN_FINISHED,
    FSEARCH_DATABASE_INDEX_EVENT_MONITORING_STARTED,
    FSEARCH_DATABASE_INDEX_EVENT_MONITORING_FINISHED,
    FSEARCH_DATABASE_INDEX_EVENT_ENTRY_CREATED,
    FSEARCH_DATABASE_INDEX_EVENT_ENTRY_DELETED,
    FSEARCH_DATABASE_INDEX_EVENT_ENTRY_RENAMED,
    FSEARCH_DATABASE_INDEX_EVENT_ENTRY_MOVED,
    FSEARCH_DATABASE_INDEX_EVENT_ENTRY_CHANGED,
    FSEARCH_DATABASE_INDEX_EVENT_ENTRY_ATTRIBUTE_CHANGED,
    NUM_FSEARCH_DATABASE_INDEX_EVENTS,
} FsearchDatabaseIndexEventKind;

typedef struct _FsearchDatabaseIndexEvent FsearchDatabaseIndexEvent;
