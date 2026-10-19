#include "test.h"
#include "renderer/r_gl_dispatch.h"

typedef struct { glFunction_t first, second; } dispatchTest_t;
static dispatchTest_t dispatch_table;
static uint32_t dispatch_calls, dispatch_resolved;
static bool dispatch_missing;
static glFunctionDef_t const dispatch_entries[] = {
    { "glFirst", offsetof(dispatchTest_t, first) },
    { "glSecond", offsetof(dispatchTest_t, second) },
};

static void dispatch_first(void) { dispatch_calls += 1; }
static void dispatch_second(void) { dispatch_calls += 10; }
static glFunction_t dispatch_resolve(cstring_t name) {
    dispatch_resolved++;
    if (!strcmp(name, "glFirst")) return dispatch_first;
    return dispatch_missing ? NULL : dispatch_second;
}

TEST(renderer_dispatch, selected_resolver_supplies_every_function) {
    dispatch_calls = dispatch_resolved = 0; dispatch_missing = false;
    T_ASSERT(R_GLLoadFunctions(&(glFunctionLoad_t){
        .table = &dispatch_table, .size = sizeof(dispatch_table),
        .entries = dispatch_entries, .entries_count = 2, .resolve = dispatch_resolve,
    }));
    T_EQ(dispatch_resolved, 2);
    dispatch_table.first(); dispatch_table.second();
    T_EQ(dispatch_calls, 11);
}

TEST(renderer_dispatch, missing_entry_clears_prior_context_and_partial_load) {
    dispatch_table.first = dispatch_first; dispatch_table.second = dispatch_second;
    dispatch_resolved = 0; dispatch_missing = true;
    T_ASSERT(!R_GLLoadFunctions(&(glFunctionLoad_t){
        .table = &dispatch_table, .size = sizeof(dispatch_table),
        .entries = dispatch_entries, .entries_count = 2, .resolve = dispatch_resolve,
    }));
    T_EQ(dispatch_resolved, 2);
    T_ASSERT(!dispatch_table.first && !dispatch_table.second);
    dispatch_missing = false;
}
