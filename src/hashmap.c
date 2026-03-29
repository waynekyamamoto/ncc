// hashmap.c — Simple hash map implementation
#include "cc.h"

// FNV-1a hash
static uint32_t fnv_hash(const char *s, int len) {
  uint32_t hash = 2166136261u;
  for (int i = 0; i < len; i++) {
    hash ^= (unsigned char)s[i];
    hash *= 16777619;
  }
  return hash;
}

#define TOMBSTONE ((void *)-1)
#define INITIAL_CAP 16

static void rehash(HashMap *map) {
  int newcap = map->capacity ? map->capacity * 2 : INITIAL_CAP;
  HashEntry *newbuckets = calloc(newcap, sizeof(HashEntry));

  for (int i = 0; i < map->capacity; i++) {
    HashEntry *e = &map->buckets[i];
    if (!e->key || e->key == TOMBSTONE)
      continue;

    uint32_t h = fnv_hash(e->key, e->keylen) % newcap;
    for (;;) {
      if (!newbuckets[h].key) {
        newbuckets[h] = *e;
        break;
      }
      h = (h + 1) % newcap;
    }
  }

  free(map->buckets);
  map->buckets = newbuckets;
  map->capacity = newcap;
}

static HashEntry *get_entry(HashMap *map, const char *key, int keylen) {
  if (!map->buckets)
    return NULL;

  uint32_t h = fnv_hash(key, keylen) % map->capacity;
  for (;;) {
    HashEntry *e = &map->buckets[h];
    if (!e->key)
      return NULL;
    if (e->key != TOMBSTONE && e->keylen == keylen && !memcmp(e->key, key, keylen))
      return e;
    h = (h + 1) % map->capacity;
  }
}

static HashEntry *get_or_insert_entry(HashMap *map, const char *key, int keylen) {
  if (!map->buckets)
    rehash(map);
  else if (map->used * 4 >= map->capacity * 3) // 75% load
    rehash(map);

  uint32_t h = fnv_hash(key, keylen) % map->capacity;
  for (;;) {
    HashEntry *e = &map->buckets[h];
    if (!e->key || e->key == TOMBSTONE) {
      e->key = strndup_checked(key, keylen);
      e->keylen = keylen;
      map->used++;
      return e;
    }
    if (e->keylen == keylen && !memcmp(e->key, key, keylen))
      return e;
    h = (h + 1) % map->capacity;
  }
}

void *hashmap_get(HashMap *map, const char *key) {
  return hashmap_get2(map, key, strlen(key));
}

void *hashmap_get2(HashMap *map, const char *key, int keylen) {
  HashEntry *e = get_entry(map, key, keylen);
  return e ? e->val : NULL;
}

void hashmap_put(HashMap *map, const char *key, void *val) {
  hashmap_put2(map, key, strlen(key), val);
}

void hashmap_put2(HashMap *map, const char *key, int keylen, void *val) {
  HashEntry *e = get_or_insert_entry(map, key, keylen);
  e->val = val;
}

void hashmap_delete(HashMap *map, const char *key) {
  hashmap_delete2(map, key, strlen(key));
}

void hashmap_delete2(HashMap *map, const char *key, int keylen) {
  HashEntry *e = get_entry(map, key, keylen);
  if (e) {
    free(e->key);
    e->key = TOMBSTONE;
  }
}
