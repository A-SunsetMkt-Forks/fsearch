#include "fsearch_database_include_manager.h"

struct include_ctx {
    const char *path;
    gboolean one_file_system;
    gboolean monitor;
    gboolean scan_after_load;
    gint id;
};

static struct include_ctx includes[] = {
    {.path = "/home/user_1", .one_file_system = TRUE, .monitor = TRUE, .scan_after_load = FALSE, .id = 1},
    {.path = "/home/user_2", .one_file_system = FALSE, .monitor = FALSE, .scan_after_load = TRUE, .id = 2},
};

static void
test_database_include() {
    for (guint i = 0; i < G_N_ELEMENTS(includes); ++i) {
        g_autoptr(FsearchDatabaseInclude) include = fsearch_database_include_new(includes[i].path,
                                                                                 includes[i].one_file_system,
                                                                                 includes[i].monitor,
                                                                                 includes[i].scan_after_load,
                                                                                 includes[i].id);
        g_assert_cmpstr(fsearch_database_include_get_path(include), ==, includes[i].path);
        g_assert_cmpint(fsearch_database_include_get_id(include), ==, includes[i].id);
        g_assert_cmpint(fsearch_database_include_get_one_file_system(include), ==, includes[i].one_file_system);
        g_assert_cmpint(fsearch_database_include_get_monitored(include), ==, includes[i].monitor);
        g_assert_cmpint(fsearch_database_include_get_scan_after_launch(include), ==, includes[i].scan_after_load);
    }

    g_autoptr(FsearchDatabaseInclude) i1 = fsearch_database_include_new(includes[0].path,
                                                                        includes[0].one_file_system,
                                                                        includes[0].monitor,
                                                                        includes[0].scan_after_load,
                                                                        includes[0].id);
    g_autoptr(FsearchDatabaseInclude) i2 = fsearch_database_include_new(includes[1].path,
                                                                        includes[1].one_file_system,
                                                                        includes[1].monitor,
                                                                        includes[1].scan_after_load,
                                                                        includes[1].id);
    g_assert_false(fsearch_database_include_equal(i1, i2));
    g_assert_true(fsearch_database_include_equal(i1, i1));
    g_assert_true(fsearch_database_include_equal(i2, i2));
    g_assert_cmpint(fsearch_database_include_compare(&i1, &i1), ==, 0);
    g_assert_cmpint(fsearch_database_include_compare(&i2, &i2), ==, 0);
    g_assert_cmpint(fsearch_database_include_compare(&i1, &i2), ==, -1);
    g_assert_cmpint(fsearch_database_include_compare(&i2, &i1), ==, 1);

    g_autoptr(FsearchDatabaseInclude) i1_copy = fsearch_database_include_copy(i1);
    g_assert_true(fsearch_database_include_equal(i1, i1_copy));
    g_assert_cmpint(fsearch_database_include_compare(&i1, &i1_copy), ==, 0);
}

static void
test_database_include_manager() {
    g_autoptr(FsearchDatabaseIncludeManager) include_manager = fsearch_database_include_manager_new();
    for (guint i = 0; i < G_N_ELEMENTS(includes); ++i) {
        g_autoptr(FsearchDatabaseInclude) include = fsearch_database_include_new(includes[i].path,
                                                                                 includes[i].one_file_system,
                                                                                 includes[i].monitor,
                                                                                 includes[i].scan_after_load,
                                                                                 includes[i].id);
        fsearch_database_include_manager_add(include_manager, include);
    }
    g_autoptr(GPtrArray) i = fsearch_database_include_manager_get_includes(include_manager);
    g_assert_true(i != NULL);
    g_assert_cmpint(i->len, ==, G_N_ELEMENTS(includes));

    g_autoptr(FsearchDatabaseIncludeManager)
        include_manager_copy = fsearch_database_include_manager_copy(include_manager);
    g_assert_true(fsearch_database_include_manager_equal(include_manager, include_manager_copy));

    fsearch_database_include_manager_remove(include_manager, g_ptr_array_index(i, 0));
    g_assert_cmpint(i->len, ==, 1);

    g_assert_false(fsearch_database_include_manager_equal(include_manager, include_manager_copy));

    g_autoptr(FsearchDatabaseInclude) include = fsearch_database_include_copy(g_ptr_array_index(i, 0));
    fsearch_database_include_manager_add(include_manager, include);
    g_assert_cmpint(i->len, ==, 1);
}

int
main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/database/include", test_database_include);
    g_test_add_func("/FSearch/database/include_manager", test_database_include_manager);
    return g_test_run();
}