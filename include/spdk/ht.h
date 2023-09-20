/*   SPDX-License-Identifier: BSD-3-Clause
 *   All rights reserved.
 */

#ifndef SPDK_HT_H
#define SPDK_HT_H

#include "spdk/stdinc.h"
#include "string.h"

#define INITIAL_CAPACITY 16
#define FNV_OFFSET 14695981039346656037UL
#define FNV_PRIME 1099511628211UL

typedef struct {
    const char *key;
    void *value;
} ht_entry;

typedef struct {
    ht_entry *entries;
    size_t capacity;
    size_t length;
} ht;

typedef struct {
    const char *key;
    void *value;

    ht *_table;
    size_t _index;
} hti;

ht *
ht_create(void)
{
    ht *table = (ht *)malloc(sizeof(ht));

    if (table == NULL) {
        return NULL;
    }

    table->length = 0;
    table->capacity = INITIAL_CAPACITY;
    table->entries = (ht_entry *)calloc(table->capacity, sizeof(ht_entry));

    if (table->entries == NULL) {
        free(table);
        return NULL;
    }

    return table;
}

void
ht_destroy(ht *table)
{
    for (size_t i = 0; i < table->capacity; i++) {
        free((void *)table->entries[i].key);
    }

    free(table->entries);
    free(table);
}

static uint64_t
hash_key(const char *key)
{
    uint64_t hash = FNV_OFFSET;

    for (const char *p = key; *p; p++) {
        hash ^= (uint64_t)(unsigned char)(*p);
        hash *= FNV_PRIME;
    }

    return hash;
}

void *
ht_get(ht *table, const char *key)
{
    uint64_t hash = hash_key(key);
    size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

    while (table->entries[index].key != NULL) {

        if (strcmp(key, table->entries[index].key) == 0) return table->entries[index].value;

        index++;
        if (index >= table->capacity) {
            index = 0;
        }

    }

    return NULL;
}

static const char *
ht_set_entry(ht_entry *entries, size_t capacity, const char *key, void *value, size_t *plength)
{
    uint64_t hash = hash_key(key);
    size_t index = (size_t)(hash & (uint64_t)(capacity - 1));

    while (entries[index].key != NULL) {

        if (strcmp(key, entries[index].key) == 0) {
            entries[index].value = value;
            return entries[index].key;
        }

        index++;
        if (index >= capacity) {
            index = 0;
        }
    }

    if (plength != NULL) {
        key = strdup(key);
        if (key == NULL) return NULL;
        (*plength)++;
    }

    entries[index].key = (char *)key;
    entries[index].value = value;
    return key;
}

static bool
ht_expand(ht *table)
{
    size_t new_capacity = table->capacity * 2;
    if (new_capacity < table->capacity) {
        return false;
    }

    ht_entry *new_entries = (ht_entry *)calloc(new_capacity, sizeof(ht_entry));
    if (new_entries == NULL) {
        return false;
    }

    for (size_t i = 0; i < table->capacity; i++) {
        ht_entry entry = table->entries[i];
        if (entry.key != NULL) ht_set_entry(new_entries, new_capacity, entry.key, entry.value, NULL);
    }

    free(table->entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
    return true;
}

const char *
ht_set(ht *table, const char *key, void *value)
{
    assert(value != NULL);
    if (value == NULL) {
        return NULL;
    }

    if (table->length >= table->capacity / 2) {
        if (!ht_expand(table)) return NULL;
    }

    return ht_set_entry(table->entries, table->capacity, key, value, &table->length);
}

void *
ht_remove(ht *table, const char *key)
{
    uint64_t hash = hash_key(key);
    size_t index = (size_t)(hash & (uint64_t)(table->capacity - 1));

    while (table->entries[index].key != NULL) {

        if (strcmp(key, table->entries[index].key) == 0) {
            table->entries[index].value = NULL;
            table->length--;
            return table->entries[index].value;
        }

        index++;

        if (index >= table->capacity) index = 0;
    }

    return NULL;
}

size_t
ht_length(ht *table)
{
    return table->length;
}

hti
ht_iterator(ht *table)
{
    hti it;
    it._table = table;
    it._index = 0;
    return it;
}

bool
ht_next(hti *it)
{
    ht *table = it->_table;
    while (it->_index < table->capacity) {

        size_t i = it->_index;
        it->_index++;

        if (table->entries[i].key != NULL) {
            ht_entry entry = table->entries[i];
            it->key = entry.key;
            it->value = entry.value;
            return true;
        }

    }
    return false;
}

#endif
