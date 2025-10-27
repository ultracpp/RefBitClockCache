/*
 * Implementation of the ESP-IDF C-based Thread-Safe Cache with Clock and Reference Bit Eviction Policy
 * Copyright (c) 2025 Eungsuk Jeon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "refbit_clock_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_random.h>

typedef struct
{
    char* key;
    int cache_index;
    int state;
} HashEntry;

static void cache_lock(RefBitClockCache* c)
{
    xSemaphoreTake(c->lock, portMAX_DELAY);
}

static void cache_unlock(RefBitClockCache* c)
{
    xSemaphoreGive(c->lock);
}

static unsigned int next_prime(unsigned int n)
{
    while (1)
    {
        int prime = 1;
        for (unsigned int i = 2; i * i <= n; i++)
        {
            if (n % i == 0)
            {
                prime = 0;
                break;
            }
        }
        if (prime)
        {
            return n;
        }
        n++;
    }
}

static unsigned int hash(const char* key, int hash_size)
{
    unsigned int h = 2166136261u;
    while (*key)
    {
        h ^= (unsigned int)*key++;
        h *= 16777619u;
    }
    return h % hash_size;
}

RefBitClockCache* createCache(int cache_size, void (*value_free)(void*))
{
    RefBitClockCache* cache = (RefBitClockCache*)malloc(sizeof(RefBitClockCache));
    if (!cache)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate cache");
        return NULL;
    }

    cache->cache_size = cache_size;
    cache->cache.keys = (char**)malloc(cache_size * sizeof(char*));
    if (cache->cache.keys)
    {
        memset(cache->cache.keys, 0, cache_size * sizeof(char*));
    }
    cache->cache.values = (CacheValue**)malloc(cache_size * sizeof(CacheValue*));
    if (cache->cache.values)
    {
        memset(cache->cache.values, 0, cache_size * sizeof(CacheValue*));
    }
    cache->clock_hand = 0;
    cache->value_free = value_free;

    // Hash table setup
    int hash_size = next_prime(cache_size * 2);
    HashEntry* hash_table = (HashEntry*)malloc(hash_size * sizeof(HashEntry));
    if (hash_table)
    {
        memset(hash_table, 0, hash_size * sizeof(HashEntry));
    }

    cache->lock = xSemaphoreCreateMutex();
    if (!cache->lock || !cache->cache.keys || !cache->cache.values || !hash_table)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate cache resources");
        free(cache->cache.keys);
        free(cache->cache.values);
        free(hash_table);
        if (cache->lock)
        {
            vSemaphoreDelete(cache->lock);
        }
        free(cache);
        return NULL;
    }

    return cache;
}

void printCacheState(RefBitClockCache* cache)
{
    char state[256] = {0};
    int offset = 0;
    for (int i = 0; i < cache->cache_size; i++)
    {
        if (cache->cache.keys[i])
        {
            CacheValue* cv = cache->cache.values[i];
            offset += snprintf(state + offset, sizeof(state) - offset, "[%d: %s, ref=%d, bit=%d] ",
                               i, cache->cache.keys[i],
                               cv ? cv->refcount : 0,
                               cv ? cv->ref_bit : 0);
        }
    }
    ESP_LOGI(CACHE_TAG, "Cache state (hand=%d): %s", cache->clock_hand, state);
}

static int findClockVictim(RefBitClockCache* cache)
{
    int start_hand = cache->clock_hand;
    int attempts = 0;
    int max_attempts = cache->cache_size * 2;

    while (attempts < max_attempts)
    {
        int idx = cache->clock_hand;
        CacheValue* cv = cache->cache.values[idx];

        if (!cv || !cache->cache.keys[idx])
        {
            cache->clock_hand = (idx + 1) % cache->cache_size;
            return idx;
        }

        if (cv->refcount == 0 && cv->ref_bit == 0)
        {
            cache->clock_hand = (idx + 1) % cache->cache_size;
            return idx;
        }

        cv->ref_bit = 0;
        cache->clock_hand = (idx + 1) % cache->cache_size;
        attempts++;
    }

    for (int i = 0; i < cache->cache_size; i++)
    {
        if (!cache->cache.keys[i])
        {
            return i;
        }
    }

    ESP_LOGW(CACHE_TAG, "No suitable victim found, forcing eviction of start_hand=%d", start_hand);
    return start_hand;
}

static void rehash(RefBitClockCache* cache)
{
    int old_size = cache->hash_size;
    HashEntry* old_table = cache->hash_table;
    cache->hash_size = next_prime(old_size * 2);
    cache->hash_table = (HashEntry*)malloc(cache->hash_size * sizeof(HashEntry));
    if (!cache->hash_table)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate new hash table");
        cache->hash_table = old_table;
        cache->hash_size = old_size;
        return;
    }
    memset(cache->hash_table, 0, cache->hash_size * sizeof(HashEntry));
    cache->hash_used = 0;

    for (int i = 0; i < old_size; i++)
    {
        if (old_table[i].state == STATE_OCCUPIED)
        {
            unsigned int h = hash(old_table[i].key, cache->hash_size);
            while (cache->hash_table[h].state == STATE_OCCUPIED)
            {
                h = (h + 1) % cache->hash_size;
            }
            cache->hash_table[h] = old_table[i];
            cache->hash_used++;
        }
    }

    free(old_table);
}

static void insertHash(RefBitClockCache* cache, char* key, int idx)
{
    if ((cache->hash_used * 10) / cache->hash_size >= 7)
    {
        rehash(cache);
    }

    unsigned int h = hash(key, cache->hash_size);
    int tombstone_idx = -1;

    while (1)
    {
        if (cache->hash_table[h].state == STATE_EMPTY)
        {
            if (tombstone_idx != -1)
            {
                h = tombstone_idx;
            }

            cache->hash_table[h].key = key;
            cache->hash_table[h].cache_index = idx;
            cache->hash_table[h].state = STATE_OCCUPIED;
            cache->hash_used++;
            return;
        }
        else if (cache->hash_table[h].state == STATE_TOMBSTONE)
        {
            if (tombstone_idx == -1)
            {
                tombstone_idx = h;
            }
        }
        else if (cache->hash_table[h].state == STATE_OCCUPIED && strcmp(cache->hash_table[h].key, key) == 0)
        {
            cache->hash_table[h].cache_index = idx;
            return;
        }

        h = (h + 1) % cache->hash_size;
    }
}

static void eraseHash(RefBitClockCache* cache, const char* key)
{
    unsigned int h = hash(key, cache->hash_size);

    while (cache->hash_table[h].state != STATE_EMPTY)
    {
        if (cache->hash_table[h].state == STATE_OCCUPIED && strcmp(cache->hash_table[h].key, key) == 0)
        {
            cache->hash_table[h].state = STATE_TOMBSTONE;
            cache->hash_used--;
            return;
        }

        h = (h + 1) % cache->hash_size;
    }
}

static int getCacheIndex(RefBitClockCache* cache, const char* key)
{
    unsigned int h = hash(key, cache->hash_size);

    while (cache->hash_table[h].state != STATE_EMPTY)
    {
        if (cache->hash_table[h].state == STATE_OCCUPIED && strcmp(cache->hash_table[h].key, key) == 0)
        {
            return cache->hash_table[h].cache_index;
        }

        h = (h + 1) % cache->hash_size;
    }

    return -1;
}

CacheValue* accessCache(RefBitClockCache* cache, const char* key, void* value, size_t value_size)
{
    cache_lock(cache);

    int index = getCacheIndex(cache, key);

    if (index != -1)
    {
        CacheValue* cv = cache->cache.values[index];

        if (cv)
        {
            cv->refcount++;
            cv->ref_bit = 1;
            ESP_LOGI(CACHE_TAG, "Cache hit → key: %s in line %d ref=%d, bit=%d", key, index, cv->refcount, cv->ref_bit);
            printCacheState(cache);
            cache_unlock(cache);
            return cv;
        }
    }

    int victim_idx = findClockVictim(cache);

    if (cache->cache.keys[victim_idx])
    {
        eraseHash(cache, cache->cache.keys[victim_idx]);
        free(cache->cache.keys[victim_idx]);
        cache->cache.keys[victim_idx] = NULL;
    }

    CacheValue* old = cache->cache.values[victim_idx];

    if (old)
    {
        if (old->refcount == 0)
        {
            cache->value_free(old->data);
            free(old);
        }
        else
        {
            old->index = -1;
        }
        cache->cache.values[victim_idx] = NULL;
    }

    cache->cache.keys[victim_idx] = strdup(key);
    if (!cache->cache.keys[victim_idx])
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate key");
        cache_unlock(cache);
        return NULL;
    }
    CacheValue* cv = (CacheValue*)malloc(sizeof(CacheValue));
    if (!cv)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate CacheValue");
        free(cache->cache.keys[victim_idx]);
        cache->cache.keys[victim_idx] = NULL;
        cache_unlock(cache);
        return NULL;
    }
    cv->data = malloc(value_size);
    if (!cv->data)
    {
        ESP_LOGE(CACHE_TAG, "Failed to allocate data");
        free(cv);
        free(cache->cache.keys[victim_idx]);
        cache->cache.keys[victim_idx] = NULL;
        cache_unlock(cache);
        return NULL;
    }
    memcpy(cv->data, value, value_size);
    cv->refcount = 1;
    cv->index = victim_idx;
    cv->ref_bit = 1;
    cache->cache.values[victim_idx] = cv;

    insertHash(cache, cache->cache.keys[victim_idx], victim_idx);

    ESP_LOGI(CACHE_TAG, "Cache miss → stored key: %s in line %d ref=1, bit=1 (victim was %d)", key, victim_idx, victim_idx);
    printCacheState(cache);

    cache_unlock(cache);
    return cv;
}

void releaseValue(RefBitClockCache* cache, CacheValue* cv)
{
    if (!cv)
    {
        return;
    }

    cache_lock(cache);
    cv->refcount--;

    if (cv->refcount == 0 && cv->index == -1)
    {
        cache->value_free(cv->data);
        free(cv);
    }

    cache_unlock(cache);
}

void freeCache(RefBitClockCache* cache)
{
    for (int i = 0; i < cache->cache_size; i++)
    {
        if (cache->cache.keys[i])
        {
            eraseHash(cache, cache->cache.keys[i]);
            free(cache->cache.keys[i]);
            cache->cache.keys[i] = NULL;
        }

        CacheValue* cv = cache->cache.values[i];

        if (cv)
        {
            if (cv->refcount > 0)
            {
                ESP_LOGW(CACHE_TAG, "Warning: freeing held CacheValue at %d (ref=%d)", i, cv->refcount);
                cache->value_free(cv->data);
                free(cv);
            }
            else if (cv->refcount == 0)
            {
                cache->value_free(cv->data);
                free(cv);
            }
            cache->cache.values[i] = NULL;
        }
    }

    free(cache->cache.keys);
    free(cache->cache.values);
    free(cache->hash_table);

    vSemaphoreDelete(cache->lock);
    free(cache);
}

void freeValue(void* ptr)
{
    free(ptr);
}
