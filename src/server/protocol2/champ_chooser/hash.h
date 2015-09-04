#ifndef _CHAMP_CHOOSER_HASH_H
#define _CHAMP_CHOOSER_HASH_H

#include <uthash.h>

typedef struct hash_strong hash_strong_t;

struct hash_strong
{
	uint8_t md5sum[MD5_DIGEST_LENGTH];
	hash_strong_t *next;
	uint64_t savepath;
};

struct hash_weak
{
	uint64_t weak;
	struct hash_strong *strong;
	UT_hash_handle hh;
};

extern struct hash_weak *hash_table;

extern struct hash_weak *hash_weak_find(uint64_t weak);
extern struct hash_strong *hash_strong_find(struct hash_weak *hash_weak,
	uint8_t *md5sum);
extern struct hash_weak *hash_weak_add(uint64_t weakint);

extern void hash_delete_all(void);
extern int hash_load(const char *champ, const char *directory);

#endif