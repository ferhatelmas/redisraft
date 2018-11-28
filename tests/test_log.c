#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>

#include "cmocka.h"

#include "../redisraft.h"

#define LOGNAME "test.log.db"
#define DBID "01234567890123456789012345678901"

static int setup_create_log(void **state)
{
    *state = RaftLogCreate(LOGNAME, DBID, 1, 0);
    assert_non_null(*state);
    return 0;
}

static int teardown_log(void **state)
{
    RaftLog *log = (RaftLog *) *state;
    RaftLogClose(log);
    unlink(LOGNAME);
    return 0;
}

static int log_entries_callback(void *arg, raft_entry_t *entry, raft_index_t idx)
{
    int ety_id = entry->id;
    const char *value = entry->data.buf;

    check_expected(ety_id);
    check_expected(value);

    return mock();
}

static void test_log_random_access(void **state)
{
    RaftLog *log = (RaftLog *) *state;

    char value1[] = "value1";
    raft_entry_t entry1 = {
        .term = 1, .type = 2, .id = 3, .data = { .buf = value1, .len = sizeof(value1)-1 }
    };
    char value2[] = "value2";
    raft_entry_t entry2 = {
        .term = 10, .type = 2, .id = 30, .data = { .buf = value2, .len = sizeof(value2)-1 }
    };

    /* Write entries */
    assert_int_equal(RaftLogAppend(log, &entry1), RR_OK);
    assert_int_equal(RaftLogAppend(log, &entry2), RR_OK);

    /* Invalid out of bound reads */
    assert_null(RaftLogGet(log, 0));
    assert_null(RaftLogGet(log, 3));

    raft_entry_t *e = RaftLogGet(log, 1);
    assert_int_equal(e->id, 3);
    raft_entry_release(e);

    e = RaftLogGet(log, 2);
    assert_int_equal(e->id, 30);
    raft_entry_release(e);
}

static void test_log_random_access_with_snapshot(void **state)
{
    RaftLog *log = (RaftLog *) *state;

    char value1[] = "value1";
    raft_entry_t entry1 = {
        .term = 1, .type = 2, .id = 3, .data = { .buf = value1, .len = sizeof(value1)-1 }
    };
    char value2[] = "value2";
    raft_entry_t entry2 = {
        .term = 10, .type = 2, .id = 30, .data = { .buf = value2, .len = sizeof(value2)-1 }
    };

    /* Reset log assuming last snapshot is 100 */
    RaftLogReset(log, 100, 1);

    /* Write entries */
    assert_int_equal(RaftLogAppend(log, &entry1), RR_OK);
    assert_int_equal(RaftLogAppend(log, &entry2), RR_OK);

    /* Invalid out of bound reads */
    assert_null(RaftLogGet(log, 99));
    assert_null(RaftLogGet(log, 100));
    assert_null(RaftLogGet(log, 103));

    raft_entry_t *e = RaftLogGet(log, 101);
    assert_non_null(e);
    assert_int_equal(e->id, 3);
    raft_entry_release(e);

    e = RaftLogGet(log, 102);
    assert_int_equal(e->id, 30);
    raft_entry_release(e);
}

static void test_log_load_entries(void **state)
{
    RaftLog *log = (RaftLog *) *state;

    char value1[] = "value1";
    raft_entry_t entry1 = {
        .term = 1, .type = 2, .id = 3, .data = { .buf = value1, .len = sizeof(value1)-1 }
    };
    char value2[] = "value2";
    raft_entry_t entry2 = {
        .term = 10, .type = 2, .id = 30, .data = { .buf = value2, .len = sizeof(value2)-1 }
    };

    /* Write entries */
    assert_int_equal(RaftLogAppend(log, &entry1), RR_OK);
    assert_int_equal(RaftLogAppend(log, &entry2), RR_OK);

    /* Load entries */
    will_return_always(log_entries_callback, 0);
    expect_value(log_entries_callback, ety_id, 3);
    expect_memory(log_entries_callback, value, "value1", 6);

    expect_value(log_entries_callback, ety_id, 30);
    expect_memory(log_entries_callback, value, "value2", 6);

    assert_int_equal(RaftLogLoadEntries(log, log_entries_callback, NULL), 2);
}

static void test_log_index_rebuild(void **state)
{
    RaftLog *log = (RaftLog *) *state;
    RaftLogReset(log, 100, 1);

    char value1[] = "value1";
    raft_entry_t entry1 = {
        .term = 1, .type = 2, .id = 3, .data = { .buf = value1, .len = sizeof(value1)-1 }
    };
    char value2[] = "value2";
    raft_entry_t entry2 = {
        .term = 10, .type = 2, .id = 30, .data = { .buf = value2, .len = sizeof(value2)-1 }
    };

    /* Write entries */
    assert_int_equal(RaftLogAppend(log, &entry1), RR_OK);
    assert_int_equal(RaftLogAppend(log, &entry2), RR_OK);

    /* Delete index file */
    unlink(LOGNAME ".idx");

    /* Reopen the log */
    RaftLog *log2 = RaftLogOpen(LOGNAME);
    RaftLogLoadEntries(log2, NULL, NULL);

    /* Invalid out of bound reads */
    assert_null(RaftLogGet(log, 99));
    assert_null(RaftLogGet(log, 100));
    assert_null(RaftLogGet(log, 103));

    raft_entry_t *e = RaftLogGet(log, 101);
    assert_int_equal(e->id, 3);
    raft_entry_release(e);

    e = RaftLogGet(log, 102);
    assert_int_equal(e->id, 30);
    raft_entry_release(e);

    /* Close the log */
    RaftLogClose(log2);
}

static void test_log_voting_persistence(void **state)
{
    RaftLog *log = (RaftLog *) *state;

    char value1[] = "value1";
    raft_entry_t entry1 = {
        .term = 1, .type = 2, .id = 3, .data = { .buf = value1, .len = sizeof(value1)-1 }
    };
    char value2[] = "value2";
    raft_entry_t entry2 = {
        .term = 10, .type = 2, .id = 30, .data = { .buf = value2, .len = sizeof(value2)-1 }
    };

    /* Write entries */
    assert_int_equal(RaftLogAppend(log, &entry1), RR_OK);
    assert_int_equal(RaftLogAppend(log, &entry2), RR_OK);

    /* Change voting */
    RaftLogSetTerm(log, 0xffffffff, INT32_MAX);

    /* Re-read first entry to verify no corruption */
    raft_entry_t *ety = RaftLogGet(log, 1);
    assert_int_equal(ety->id, 3);
    raft_entry_release(ety);

    RaftLog *templog = RaftLogOpen(LOGNAME);
    assert_int_equal(templog->term, 0xffffffff);
    assert_int_equal(templog->vote, INT32_MAX);
    RaftLogClose(templog);
}

static void mock_notify_func(void *arg, raft_entry_t *ety, raft_index_t idx)
{
    int ety_id = ety->id;
    check_expected(ety_id);
    check_expected(idx);
}

static void test_log_delete(void **state)
{
    RaftLog *log = (RaftLog *) *state;

    char value1[] = "value1";
    raft_entry_t entry1 = {
        .term = 1, .type = 2, .id = 3, .data = { .buf = value1, .len = sizeof(value1)-1 }
    };
    char value2[] = "value22222";
    raft_entry_t entry2 = {
        .term = 10, .type = 2, .id = 20, .data = { .buf = value2, .len = sizeof(value2)-1 }
    };
    char value3[] = "value33333333333";
    raft_entry_t entry3 = {
        .term = 10, .type = 2, .id = 30, .data = { .buf = value3, .len = sizeof(value3)-1 }
    };

    /* Simulate post snapshot log */
    RaftLogReset(log, 50, 1);

    /* Write entries */
    assert_int_equal(RaftLogAppend(log, &entry1), RR_OK);
    assert_int_equal(RaftLogAppend(log, &entry2), RR_OK);
    assert_int_equal(RaftLogAppend(log, &entry3), RR_OK);

    raft_entry_t *e = RaftLogGet(log, 51);
    assert_non_null(e);
    assert_int_equal(e->id, 3);
    raft_entry_release(e);

    /* Try delete with improper values */
    assert_int_equal(RaftLogDelete(log, 0, NULL, NULL), RR_ERROR);

    /* Delete last two elements */
    expect_value(mock_notify_func, ety_id, 20);
    expect_value(mock_notify_func, idx, 52);
    expect_value(mock_notify_func, ety_id, 30);
    expect_value(mock_notify_func, idx, 53);
    assert_int_equal(RaftLogDelete(log, 52, mock_notify_func, NULL), RR_OK);

    /* Check log sanity after delete */
    assert_int_equal(RaftLogCount(log), 1);
    assert_null(RaftLogGet(log, 52));
    e = RaftLogGet(log, 51);
    assert_non_null(e);
    assert_int_equal(e->id, 3);
    raft_entry_release(e);

    /* Re-add entries in reverse order, validate indexes are handled
     * properly.
     */

    assert_int_equal(RaftLogAppend(log, &entry3), RR_OK);
    e = RaftLogGet(log, 52);
    assert_non_null(e);
    assert_int_equal(e->id, 30);
    raft_entry_release(e);

    assert_int_equal(RaftLogAppend(log, &entry2), RR_OK);
    e = RaftLogGet(log, 53);
    assert_non_null(e);
    assert_int_equal(e->id, 20);
    raft_entry_release(e);
}

static void test_entry_cache_sanity(void **state)
{
    EntryCache *cache = EntryCacheNew(8);
    raft_entry_t *ety;
    int i;

    /* Insert 64 entries (cache grows) */
    for (i = 1; i <= 64; i++) {
        ety = raft_entry_new();
        ety->id = i;
        EntryCacheAppend(cache, ety, i);
        raft_entry_release(ety);
    }

    assert_int_equal(cache->size, 64);
    assert_int_equal(cache->len, 64);

    /* Get 64 entries */
    for (i = 1; i <= 64; i++) {
        ety = EntryCacheGet(cache, i);
        assert_non_null(ety);
        assert_int_equal(ety->id, i);
        raft_entry_release(ety);
    }

    EntryCacheFree(cache);
}

static void test_entry_cache_start_index_change(void **state)
{
    EntryCache *cache = EntryCacheNew(8);
    raft_entry_t *ety;

    /* Establish start_idx 1 */
    ety = raft_entry_new();
    ety->id = 1;
    EntryCacheAppend(cache, ety, 1);
    raft_entry_release(ety);

    assert_int_equal(cache->start_idx, 1);
    EntryCacheDeleteTail(cache, 1);
    assert_int_equal(cache->start_idx, 0);

    ety = raft_entry_new();
    ety->id = 10;
    EntryCacheAppend(cache, ety, 10);
    raft_entry_release(ety);

    assert_int_equal(cache->start_idx, 10);

    EntryCacheFree(cache);
}

static void test_entry_cache_delete_head(void **state)
{
    EntryCache *cache = EntryCacheNew(4);
    raft_entry_t *ety;
    int i;

    /* Fill up 5 entries */
    for (i = 1; i <= 5; i++) {
        ety = raft_entry_new();
        ety->id = i;
        EntryCacheAppend(cache, ety, i);
        raft_entry_release(ety);
    }

    assert_int_equal(cache->size, 8);
    assert_int_equal(cache->start, 0);
    assert_int_equal(cache->start_idx, 1);

    /* Test invalid deletes */
    assert_int_equal(EntryCacheDeleteHead(cache, 0), -1);

    /* Delete first entry */
    assert_int_equal(EntryCacheDeleteHead(cache, 2), 1);
    assert_null(EntryCacheGet(cache, 1));
    ety = EntryCacheGet(cache, 2);
    assert_int_equal(ety->id, 2);
    raft_entry_release(ety);

    assert_int_equal(cache->start, 1);
    assert_int_equal(cache->len, 4);
    assert_int_equal(cache->start_idx, 2);

    /* Delete and add 5 entries (6, 7, 8, 9, 10)*/
    for (i = 0; i < 5; i++) {
        assert_int_equal(EntryCacheDeleteHead(cache, 3 + i), 1);
        ety = raft_entry_new();
        ety->id = 6 + i;
        EntryCacheAppend(cache, ety, ety->id);
        raft_entry_release(ety);
    }

    assert_int_equal(cache->start_idx, 7);
    assert_int_equal(cache->start, 6);
    assert_int_equal(cache->size, 8);
    assert_int_equal(cache->len, 4);

    /* Add another 3 (11, 12, 13) */
    for (i = 11; i <= 13; i++) {
        ety = raft_entry_new();
        ety->id = i;
        EntryCacheAppend(cache, ety, i);
        raft_entry_release(ety);
    }

    assert_int_equal(cache->start, 6);
    assert_int_equal(cache->size, 8);
    assert_int_equal(cache->len, 7);

    /* Validate contents */
    for (i = 7; i <= 13; i++) {
        ety = EntryCacheGet(cache, i);
        assert_non_null(ety);
        assert_int_equal(ety->id, i);
        raft_entry_release(ety);
    }

    /* Delete multiple with an overlap */
    assert_int_equal(EntryCacheDeleteHead(cache, 10), 3);
    assert_int_equal(cache->len, 4);
    assert_int_equal(cache->start, 1);

    /* Validate contents after deletion */
    for (i = 10; i <= 13; i++) {
        ety = EntryCacheGet(cache, i);
        assert_non_null(ety);
        assert_int_equal(ety->id, i);
        raft_entry_release(ety);
    }

    EntryCacheFree(cache);
}

static void test_entry_cache_delete_tail(void **state)
{
    EntryCache *cache = EntryCacheNew(4);
    raft_entry_t *ety;
    int i;

    for (i = 100; i <= 103; i++) {
        ety = raft_entry_new();
        ety->id = i;
        EntryCacheAppend(cache, ety, i);
        raft_entry_release(ety);
    }

    assert_int_equal(cache->size, 4);
    assert_int_equal(cache->len, 4);

    /* Try invalid indexes */
    assert_int_equal(EntryCacheDeleteTail(cache, 104), -1);
    assert_int_equal(EntryCacheDeleteTail(cache, 99), -1);

    /* Delete last entry */
    assert_int_equal(EntryCacheDeleteTail(cache, 103), 1);
    assert_int_equal(cache->len, 3);
    assert_null(EntryCacheGet(cache, 103));
    ety = EntryCacheGet(cache, 102);
    assert_int_equal(ety->id, 102);
    raft_entry_release(ety);

    /* Delete all entries */
    assert_int_equal(EntryCacheDeleteTail(cache, 100), 3);
    assert_int_equal(cache->len, 0);

    EntryCacheFree(cache);
}

static void test_entry_cache_fuzzer(void **state)
{
    EntryCache *cache = EntryCacheNew(4);
    raft_entry_t *ety;
    int i, j;
    raft_index_t first_index = 1;
    raft_index_t index = 0;

    srandom(time(NULL));
    for (i = 0; i < 100000; i++) {
        int new_entries = random() % 50;

        for (j = 0; j < new_entries; j++) {
            ety = raft_entry_new();
            ety->id = ++index;
            EntryCacheAppend(cache, ety, index);
            raft_entry_release(ety);
        }

        if (index > 5) {
            int del_head = random() % ((index + 1) / 2);
            int removed = EntryCacheDeleteHead(cache, del_head);
            if (removed > 0) {
                first_index += removed;
            }
        }

        if (index - first_index > 10) {
            int del_tail = random() % ((index - first_index) / 10);
            if (del_tail) {
                int removed = EntryCacheDeleteTail(cache, index - del_tail + 1);
                assert_int_equal(removed, del_tail);
                index -= removed;
            }
        }
    }

    /* verify */
    for (i = 1; i < first_index; i++) {
        assert_null(EntryCacheGet(cache, i));
    }
    for (i = first_index; i <= index; i++) {
        ety = EntryCacheGet(cache, i);
        assert_non_null(ety);
        assert_int_equal(i, ety->id);
        raft_entry_release(ety);
    }

    EntryCacheFree(cache);
}

const struct CMUnitTest log_tests[] = {
    cmocka_unit_test_setup_teardown(
            test_log_load_entries, setup_create_log, teardown_log),
    cmocka_unit_test_setup_teardown(
            test_log_random_access, setup_create_log, teardown_log),
    cmocka_unit_test_setup_teardown(
            test_log_random_access_with_snapshot, setup_create_log, teardown_log),
    cmocka_unit_test_setup_teardown(
            test_log_index_rebuild, setup_create_log, teardown_log),
    cmocka_unit_test_setup_teardown(
            test_log_delete, setup_create_log, teardown_log),
    cmocka_unit_test_setup_teardown(
            test_log_voting_persistence, setup_create_log, teardown_log),
    cmocka_unit_test_setup_teardown(
            test_entry_cache_sanity, NULL, NULL),
    cmocka_unit_test_setup_teardown(
            test_entry_cache_start_index_change, NULL, NULL),
    cmocka_unit_test_setup_teardown(
            test_entry_cache_delete_head, NULL, NULL),
    cmocka_unit_test_setup_teardown(
            test_entry_cache_delete_tail, NULL, NULL),
    cmocka_unit_test_setup_teardown(
            test_entry_cache_fuzzer, NULL, NULL),
    { .test_func = NULL }
};
