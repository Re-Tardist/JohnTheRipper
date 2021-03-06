/* Mac OS X Keychain cracker patch for JtR. Hacked together during Summer of
 * 2012 by Dhiru Kholia <dhiru.kholia at gmail.com>.
 *
 * This software is Copyright (c) 2012, Dhiru Kholia <dhiru.kholia at gmail.com>,
 * and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted.
 *
 * * (c) 2004 Matt Johnston <matt @ ucc asn au>
 * This code may be freely used and modified for any purpose. */

#include <string.h>
#include <assert.h>
#include <errno.h>
#include "arch.h"
#include "misc.h"
#include "common.h"
#include "formats.h"
#include "params.h"
#include "options.h"
#include "pbkdf2_hmac_sha1.h"
#include <openssl/des.h>
#ifdef _OPENMP
#include <omp.h>
#define OMP_SCALE               64
#endif

#define FORMAT_LABEL		"keychain"
#define FORMAT_NAME		"Mac OS X Keychain"
#ifdef MMX_COEF
#define ALGORITHM_NAME		"PBKDF2-SHA1 3DES " SHA1_N_STR MMX_TYPE
#else
#define ALGORITHM_NAME		"PBKDF2-SHA1 3DES 32/" ARCH_BITS_STR
#endif
#define BENCHMARK_COMMENT	""
#define BENCHMARK_LENGTH	-1
#define BINARY_SIZE		0
#define PLAINTEXT_LENGTH	125
#define SALT_SIZE		sizeof(*salt_struct)
#ifdef MMX_COEF
#define MIN_KEYS_PER_CRYPT	SSE_GROUP_SZ_SHA1
#define MAX_KEYS_PER_CRYPT	SSE_GROUP_SZ_SHA1
#else
#define MIN_KEYS_PER_CRYPT	1
#define MAX_KEYS_PER_CRYPT	1
#endif

#define SALTLEN 20
#define IVLEN 8
#define CTLEN 48

static struct fmt_tests keychain_tests[] = {
	{"$keychain$*10f7445c8510fa40d9ef6b4e0f8c772a9d37e449*f3d19b2a45cdcccb*8c3c3b1c7d48a24dad4ccbd4fd794ca9b0b3f1386a0a4527f3548bfe6e2f1001804b082076641bbedbc9f3a7c33c084b", "password"},
	{NULL}
};

#if defined (_OPENMP)
static int omp_t = 1;
#endif
static char (*saved_key)[PLAINTEXT_LENGTH + 1];
static int *cracked;

static struct custom_salt {
	unsigned char salt[SALTLEN];
	unsigned char iv[IVLEN];
	unsigned char ct[CTLEN];
} *salt_struct;

static void init(struct fmt_main *self)
{

#if defined (_OPENMP)
	omp_t = omp_get_max_threads();
	self->params.min_keys_per_crypt *= omp_t;
	omp_t *= OMP_SCALE;
	self->params.max_keys_per_crypt *= omp_t;
#endif
	saved_key = mem_calloc_tiny(sizeof(*saved_key) *
			self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
	cracked = mem_calloc_tiny(sizeof(*cracked) *
			self->params.max_keys_per_crypt, MEM_ALIGN_WORD);
}

static int valid(char *ciphertext, struct fmt_main *self)
{
	char *ctcopy, *keeptr, *p;
	if (strncmp(ciphertext,  "$keychain$*", 11) != 0)
		return 0;
	ctcopy = strdup(ciphertext);
	keeptr = ctcopy;
	ctcopy += 11;
	if ((p = strtok(ctcopy, "*")) == NULL)	/* salt */
		goto err;
	if(strlen(p) != SALTLEN * 2)
		goto err;
	if ((p = strtok(NULL, "*")) == NULL)	/* iv */
		goto err;
	if(strlen(p) != IVLEN * 2)
		goto err;
	if ((p = strtok(NULL, "*")) == NULL)	/* ciphertext */
		goto err;
	if(strlen(p) != CTLEN * 2)
		goto err;

	MEM_FREE(keeptr);
	return 1;

err:
	MEM_FREE(keeptr);
	return 0;
}

static void *get_salt(char *ciphertext)
{
	char *ctcopy = strdup(ciphertext);
	char *keeptr = ctcopy;
	int i;
	char *p;
	ctcopy += 11;	/* skip over "$keychain$*" */
	salt_struct = mem_alloc_tiny(sizeof(struct custom_salt), MEM_ALIGN_WORD);
	p = strtok(ctcopy, "*");
	for (i = 0; i < SALTLEN; i++)
		salt_struct->salt[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
	p = strtok(NULL, "*");
	for (i = 0; i < IVLEN; i++)
		salt_struct->iv[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];
	p = strtok(NULL, "*");
	for (i = 0; i < CTLEN; i++)
		salt_struct->ct[i] = atoi16[ARCH_INDEX(p[i * 2])] * 16
			+ atoi16[ARCH_INDEX(p[i * 2 + 1])];

	MEM_FREE(keeptr);
	return (void *)salt_struct;
}


static void set_salt(void *salt)
{
	salt_struct = (struct custom_salt *)salt;
}

static int kcdecrypt(unsigned char *key, unsigned char *iv, unsigned char *data)
{
	unsigned char out[CTLEN];
	int pad, n, i;
	DES_cblock key1, key2, key3;
	DES_cblock ivec;
	DES_key_schedule ks1, ks2, ks3;
	memset(out, 0, sizeof(out));
	memcpy(key1, key, 8);
	memcpy(key2, key + 8, 8);
	memcpy(key3, key + 16, 8);
	DES_set_key((C_Block *) key1, &ks1);
	DES_set_key((C_Block *) key2, &ks2);
	DES_set_key((C_Block *) key3, &ks3);
	memcpy(ivec, iv, 8);
	DES_ede3_cbc_encrypt(data, out, CTLEN, &ks1, &ks2, &ks3, &ivec,  DES_DECRYPT);

	// now check padding
	pad = out[47];
	if(pad > 8)
		// "Bad padding byte. You probably have a wrong password"
		return -1;
	if(pad != 4) /* possible bug here, is this assumption always valid? */
		return -1;
	n = CTLEN - pad;
	for(i = n; i < CTLEN; i++)
		if(out[i] != pad)
			// "Bad padding. You probably have a wrong password"
			return -1;
	return 0;
}


static int crypt_all(int *pcount, struct db_salt *salt)
{
	int count = *pcount;
	int index = 0;
#ifdef _OPENMP
#pragma omp parallel for
	for (index = 0; index < count; index += MAX_KEYS_PER_CRYPT)
#endif
	{
		unsigned char master[MAX_KEYS_PER_CRYPT][32];
		int i;
#ifdef MMX_COEF
		int lens[MAX_KEYS_PER_CRYPT];
		unsigned char *pin[MAX_KEYS_PER_CRYPT], *pout[MAX_KEYS_PER_CRYPT];
		for (i = 0; i < MAX_KEYS_PER_CRYPT; ++i) {
			lens[i] = strlen(saved_key[index+i]);
			pin[i] = (unsigned char*)saved_key[index+i];
			pout[i] = master[i];
		}
		pbkdf2_sha1_sse((const unsigned char**)pin, lens, salt_struct->salt, SALTLEN, 1000, pout, 24, 0);
#else
		pbkdf2_sha1((unsigned char *)saved_key[index],  strlen(saved_key[index]), salt_struct->salt, SALTLEN, 1000, master[0], 24, 0);
#endif
		for (i = 0; i < MAX_KEYS_PER_CRYPT; ++i) {
			if(kcdecrypt(master[i], salt_struct->iv, salt_struct->ct) == 0)
				cracked[index+i] = 1;
			else
				cracked[index+i] = 0;
		}
	}
	return count;
}

static int cmp_all(void *binary, int count)
{
	int index;
	for (index = 0; index < count; index++)
		if (cracked[index])
			return 1;
	return 0;
}

static int cmp_one(void *binary, int index)
{
	return cracked[index];
}

static int cmp_exact(char *source, int index)
{
    return 1;
}

static void keychain_set_key(char *key, int index)
{
	int saved_key_length = strlen(key);
	if (saved_key_length > PLAINTEXT_LENGTH)
		saved_key_length = PLAINTEXT_LENGTH;
	memcpy(saved_key[index], key, saved_key_length);
	saved_key[index][saved_key_length] = 0;
}

static char *get_key(int index)
{
	return saved_key[index];
}

struct fmt_main fmt_keychain = {
	{
		FORMAT_LABEL,
		FORMAT_NAME,
		ALGORITHM_NAME,
		BENCHMARK_COMMENT,
		BENCHMARK_LENGTH,
		PLAINTEXT_LENGTH,
		BINARY_SIZE,
		DEFAULT_ALIGN,
		SALT_SIZE,
		DEFAULT_ALIGN,
		MIN_KEYS_PER_CRYPT,
		MAX_KEYS_PER_CRYPT,
		FMT_CASE | FMT_8_BIT | FMT_OMP | FMT_NOT_EXACT,
		keychain_tests
	}, {
		init,
		fmt_default_done,
		fmt_default_reset,
		fmt_default_prepare,
		valid,
		fmt_default_split,
		fmt_default_binary,
		get_salt,
		fmt_default_source,
		{
			fmt_default_binary_hash
		},
		fmt_default_salt_hash,
		set_salt,
		keychain_set_key,
		get_key,
		fmt_default_clear_keys,
		crypt_all,
		{
			fmt_default_get_hash
		},
		cmp_all,
		cmp_one,
		cmp_exact
	}
};
