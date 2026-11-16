/*
 * Sanitizer regression harness for DtMmdb and dtinfo hardening fixes:
 *  - DtMmdb dstr/memory_pool.C:
 *      - memory_pool::alloc / free for > MAX_CHUNK_SZ (previously read
 *        chunk_manage_record_t before the start of new char[sz] and called
 *        delete[] on a non-header or mismatched pointer)
 *      - memory_pool::alloc / free for non-multiple-of-sizeof(void*) sizes
 *        such as 5 bytes (previously stored in vm_pool_vector[5] while
 *        chunk_carrier rounded chunk_sz to ll4(5) == 8, causing free() to
 *        throw "fix_chunk_pool missing")
 *  - DtMmdb utility/buffer.C:
 *      - buffer::expand_chunk alignment offset calculation when the new
 *        allocation is already aligned to sizeof(void*)
 *  - dtinfo UAS/DtSR/DtSR_SearchResultsEntry.C:
 *      - overlay_proximity operator precedence bug `f_zone & (0x01<<uas_zone == 0)`
 *        and negative zone guard (`uas_inv = -1`)
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <vector>

using namespace std;

typedef void *voidPtr;
#define MAX_CHUNK_SZ 128
#define NUM_CHUNKS 50
#define INRANGE(x, low, high) (((x) >= (low)) && ((x) <= (high)))
#define debug(s, x) ((void)0)

struct stringException {
    const char *msg;
    explicit stringException(const char *m) : msg(m) {}
};

static int
ll4(int x)
{
    int rem = x % (int)sizeof(void *);
    return (rem == 0) ? x : (x + (int)sizeof(void *) - rem);
}

class dlist_cell {
public:
    dlist_cell *next_ptr = nullptr;
    virtual ~dlist_cell() = default;
};

class dlist_void_ptr_cell : public dlist_cell {
    void *v_ptr;
public:
    explicit dlist_void_ptr_cell(void *p) : v_ptr(p) {}
    void *void_ptr() const { return v_ptr; }
};

class dlist {
    vector<dlist_cell *> items;
public:
    void insert_as_tail(dlist_cell *c) { items.push_back(c); }
    void append(dlist *other) {
        for (auto *c : other->items)
            items.push_back(c);
    }
    void empty_list() { items.clear(); }
    int count() const { return (int)items.size(); }
    dlist_cell *get_head() { return items.empty() ? nullptr : items.front(); }
    void delete_cell(dlist_cell *c) {
        for (auto it = items.begin(); it != items.end(); ++it) {
            if (*it == c) {
                items.erase(it);
                return;
            }
        }
    }
    long first() const { return items.empty() ? 0 : 1; }
    void next(long &idx) const {
        if (idx <= 0 || (size_t)idx >= items.size())
            idx = 0;
        else
            idx++;
    }
    dlist_cell *at_index(long idx) const { return items[(size_t)(idx - 1)]; }
};

class void_ptr_array {
    vector<void *> arr;
public:
    explicit void_ptr_array(int n) : arr((size_t)(n + 2), nullptr) {}
    void *operator[](size_t i) const { return (i < arr.size()) ? arr[i] : nullptr; }
    void insert(void *p, int i) {
        if ((size_t)i >= arr.size())
            arr.resize((size_t)i + 1, nullptr);
        arr[(size_t)i] = p;
    }
};

class chunk_carrier;

class chunk_manage_record_t : public dlist_cell {
public:
    chunk_carrier *chunk_carrier_ptr;
    explicit chunk_manage_record_t(chunk_carrier *chk_carr)
        : chunk_carrier_ptr(chk_carr) {}
    ~chunk_manage_record_t() override = default;
};

class chunk_carrier {
public:
    chunk_carrier(int chk_sz, int chks)
        : chunk_sz(ll4(chk_sz)), max_chunks(chks)
    {
        alloc_sz = max_chunks * (chunk_sz + (int)sizeof(chunk_manage_record_t));
        carrier_ptr = new char[alloc_sz];
    }
    virtual ~chunk_carrier() { delete[] carrier_ptr; }
    dlist *init_ptrs() {
        dlist *w = new dlist;
        int offset = 0;
        for (int i = 0; i < max_chunks; i++) {
            chunk_manage_record_t *x =
                (chunk_manage_record_t *)(carrier_ptr + offset);
            x->chunk_carrier_ptr = this;
            w->insert_as_tail(x);
            offset += (chunk_sz + (int)sizeof(chunk_manage_record_t));
        }
        return w;
    }
    int alloc_sz;
    int chunk_sz;
    int max_chunks;
    char *carrier_ptr;
};

class fix_chunk_pool {
public:
    explicit fix_chunk_pool(int x) : chunk_sz(x), chunks(1) {
        init_one_chunk_carrier();
    }
    virtual ~fix_chunk_pool() {
        free_chunk_list.empty_list();
        for (auto *c : carriers)
            delete c;
    }
    virtual char *alloc() {
        if (free_chunk_list.count() == 0)
            init_one_chunk_carrier();
        chunk_manage_record_t *x =
            (chunk_manage_record_t *)free_chunk_list.get_head();
        free_chunk_list.delete_cell(x);
        return (char *)(((char *)x) + sizeof(chunk_manage_record_t));
    }
    virtual void free(char *x) {
        free_chunk_list.insert_as_tail(
            (chunk_manage_record_t *)(x - sizeof(chunk_manage_record_t)));
    }
protected:
    int chunk_sz;
    int chunks;
    vector<chunk_carrier *> carriers;
    dlist free_chunk_list;
    void init_one_chunk_carrier() {
        chunk_carrier *x = new chunk_carrier(chunk_sz, NUM_CHUNKS);
        carriers.push_back(x);
        dlist *z = x->init_ptrs();
        free_chunk_list.append(z);
        delete z;
    }
};

class memory_pool {
public:
    explicit memory_pool(int x = MAX_CHUNK_SZ);
    virtual ~memory_pool();
    char *alloc(size_t sz);
    void free(char *str);
protected:
    int max_alloc_size_from_pool;
    void_ptr_array vm_pool_vector;
};

class buffer {
public:
    explicit buffer(const int sz = 16) { _alloc(sz); }
    virtual ~buffer();
    void _alloc(const int sz);
    int expand_chunk(const int newsz);
    int buf_sz() const { return v_bufsz; }
    char *get_base() const { return v_base; }
protected:
    int v_bufsz = 0;
    char *v_base = nullptr;
    char *v_aptr = nullptr;
    char *v_eptr = nullptr;
    int v_align_offset = 0;
    int v_allocated = 0;
};

struct UAS_SearchZones {
    enum uas_zones {
        uas_inv = -1,
        uas_default = 0,
        uas_title = 1,
        uas_body = 2,
        uas_all = 6
    };
};
typedef UAS_SearchZones DtSR_SearchZones;

class DtSR_SearchResultsEntry {
public:
    enum {
        Unspecified = -1,
        Conflict = -2,
        Beyond_Range = -3
    };
    unsigned int f_zone = 0;
    int f_proximity[UAS_SearchZones::uas_all + 1] = {0};

    int unset_proximity(DtSR_SearchZones::uas_zones uas_zone);
    int set_proximity(DtSR_SearchZones::uas_zones uas_zone, int proximity);
    int overlay_proximity(DtSR_SearchZones::uas_zones uas_zone, int proximity);
    int get_proximity(DtSR_SearchZones::uas_zones uas_zone) const;
};

/* @@EXTRACTED_FUNCTIONS@@ */

static void
test_memory_pool_small_unaligned_and_large(void)
{
    memory_pool pool;

    /* Unaligned pool size (5 -> ll4(5) == 8) */
    char *p5 = pool.alloc(5);
    assert(p5 != nullptr);
    memcpy(p5, "abcd", 5);
    pool.free(p5);

    /* Exact boundary size MAX_CHUNK_SZ (128) */
    char *p128 = pool.alloc(MAX_CHUNK_SZ);
    assert(p128 != nullptr);
    memset(p128, 'A', MAX_CHUNK_SZ);
    pool.free(p128);

    /* Large allocation > MAX_CHUNK_SZ (previously read before new char[sz]!) */
    char *p512 = pool.alloc(512);
    assert(p512 != nullptr);
    memset(p512, 'Z', 512);
    pool.free(p512);
}

static void
test_buffer_expand_chunk(void)
{
    buffer buf(16);
    memcpy(buf.get_base(), "0123456789abcdef", 16);
    assert(buf.expand_chunk(64) == 0);
    assert(buf.buf_sz() == 64);
    assert(memcmp(buf.get_base(), "0123456789abcdef", 16) == 0);
    /* Write up to the end of the 64-byte buffer to verify no OOB write */
    memset(buf.get_base() + 16, 'X', 48);
}

static void
test_dtsr_search_results_entry_proximity(void)
{
    DtSR_SearchResultsEntry entry;

    /* Negative zone (uas_inv = -1) must be rejected as Beyond_Range */
    assert(entry.get_proximity(UAS_SearchZones::uas_inv) ==
           DtSR_SearchResultsEntry::Beyond_Range);
    assert(entry.set_proximity(UAS_SearchZones::uas_inv, 10) ==
           DtSR_SearchResultsEntry::Beyond_Range);
    assert(entry.overlay_proximity(UAS_SearchZones::uas_inv, 10) ==
           DtSR_SearchResultsEntry::Beyond_Range);

    /* First set zone 0 to 100 */
    assert(entry.set_proximity(UAS_SearchZones::uas_default, 100) == 100);

    /* Now overlay_proximity on unset zone 1 (uas_title) while f_zone != 0:
     * previously `if (f_zone & (0x01<<uas_zone == 0))` evaluated `0x01<<1 == 0`
     * (false -> 0), `f_zone & 0` (false), and skipped set_proximity(), leaving
     * zone 1 unset in f_zone and averaging 0 with 80 = 40!
     */
    int res = entry.overlay_proximity(UAS_SearchZones::uas_title, 80);
    assert(res == 80);
    assert(entry.get_proximity(UAS_SearchZones::uas_title) == 80);

    /* Second overlay_proximity on zone 1 averages (80 + 40) / 2 = 60 */
    assert(entry.overlay_proximity(UAS_SearchZones::uas_title, 40) == 60);
    assert(entry.get_proximity(UAS_SearchZones::uas_title) == 60);
}

int
main(void)
{
    test_memory_pool_small_unaligned_and_large();
    test_buffer_expand_chunk();
    test_dtsr_search_results_entry_proximity();
    printf("check-dtmmdb-hardening: all tests passed\n");
    return 0;
}
