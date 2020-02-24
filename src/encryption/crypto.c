/*
 * MeshVPN - A open source peer-to-peer VPN (forked from PeerVPN)
 *
 * Copyright (C) 2012-2016  Tobias Volk <mail@tobiasvolk.de>
 * Copyright (C) 2016       Hideman Developer <company@hideman.net>
 * Copyright (C) 2017       Benjamin Kübler <b.kuebler@kuebler-it.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef F_CRYPTO_C
#define F_CRYPTO_C

#include "crypto.h"
#include "util.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int cryptoRandFD = -1;

// return EVP cipher key size
int cryptoGetEVPCipherSize(struct s_crypto_cipher *st_cipher) {
	return EVP_CIPHER_key_length(st_cipher->cipher);
}


// return EVP cipher
struct s_crypto_cipher cryptoGetEVPCipher(const EVP_CIPHER *cipher) {
	struct s_crypto_cipher ret = { .cipher = cipher };
	return ret;
}


// return EVP md
struct s_crypto_md cryptoGetEVPMD(const EVP_MD *md) {
	struct s_crypto_md ret = { .md = md };
	return ret;
}

int cryptoRandInit() {
	int fd;
	unsigned char randbuf[64];
	if(!(cryptoRandFD < 0)) { return 1; }
	if(!((fd = open("/dev/urandom", O_RDONLY)) < 0)) {
		if(read(fd, randbuf, 64) > 0) {
			RAND_seed(randbuf, 64);
			cryptoRandFD = fd;
			return 1;
		}
		close(fd);
	}
	if(!((fd = open("/dev/random", O_RDONLY)) < 0)) {
		if(read(fd, randbuf, 64) > 0) {
			RAND_seed(randbuf, 64);
			cryptoRandFD = fd;
			return 1;
		}
		close(fd);
	}
	if(RAND_bytes(randbuf, 64) == 1) {
		return 1;
	}
	return 0;
}


// generate random bytes
int cryptoRand(unsigned char *buf, const int buf_size) {
	int len;
	unsigned char randbuf[64];
	len = 0;
	if(buf_size > 0) {
		// prefer OpenSSL RNG
		if(RAND_bytes(buf, buf_size) == 1) {
			return 1;
		}

		// fallback to /dev/urandom
		if(!(cryptoRandFD < 0)) {
			len = read(cryptoRandFD, randbuf, 64);
			if(len > 0) {
				RAND_seed(randbuf, len); // re-seed OpenSSL RNG
			}
			len = read(cryptoRandFD, buf, buf_size);
			if(len == buf_size) {
				return 1;
			}
		}
	}
	return 0;
}


// generate random int64 number
int64_t cryptoRand64() {
	int64_t n;
	unsigned char *buf = (unsigned char *)&n;
	int len = sizeof(int64_t);
	cryptoRand(buf, len);
	return n;
}


// generate random int number
int cryptoRandInt() {
	int n;
	n = cryptoRand64();
	return n;
}


// generate keys
int cryptoSetKeys(struct s_crypto *ctxs, const int count, const unsigned char *secret_buf, const int secret_len, const unsigned char *nonce_buf, const int nonce_len) {
	int cur_key_len;
	unsigned char cur_key[EVP_MAX_MD_SIZE];
	int seed_key_len;
	unsigned char seed_key[EVP_MAX_MD_SIZE];
	const EVP_MD *keygen_md = EVP_sha512();
	const EVP_MD *out_md = EVP_sha256();
	const EVP_CIPHER *out_cipher = EVP_aes_256_cbc();
	const int key_size = EVP_CIPHER_key_length(out_cipher);
	HMAC_CTX *hmac_ctx;
	int16_t i;
	unsigned char in[2];
	int j,k;

	// setup hmac as the pseudorandom function
	hmac_ctx = HMAC_CTX_new();

	// calculate seed key
	HMAC_Init_ex(hmac_ctx, nonce_buf, nonce_len, keygen_md, NULL);
	HMAC_Update(hmac_ctx, secret_buf, secret_len);
	HMAC_Final(hmac_ctx, seed_key, (unsigned int *)&seed_key_len);

	// calculate derived keys
	HMAC_Init_ex(hmac_ctx, seed_key, seed_key_len, keygen_md, NULL);
	HMAC_Update(hmac_ctx, nonce_buf, nonce_len);
	HMAC_Final(hmac_ctx, cur_key, (unsigned int *)&cur_key_len);
	i = 0;
	j = 0;
	k = 0;
	while(k < count) {
		// calculate next key
		utilWriteInt16(in, i);
		HMAC_Init_ex(hmac_ctx, NULL, -1, NULL, NULL);
		HMAC_Update(hmac_ctx, cur_key, cur_key_len);
		HMAC_Update(hmac_ctx, nonce_buf, nonce_len);
		HMAC_Update(hmac_ctx, in, 2);
		HMAC_Final(hmac_ctx, cur_key, (unsigned int *)&cur_key_len);
		if(cur_key_len < key_size) return 0; // check if key is long enough
		switch(j) {
			case 1:
				// save this key as the decryption and encryption key
				if(!EVP_EncryptInit_ex(ctxs[k].enc_ctx, out_cipher, NULL, cur_key, NULL)) return 0;
				if(!EVP_DecryptInit_ex(ctxs[k].dec_ctx, out_cipher, NULL, cur_key, NULL)) return 0;
				break;
			case 2:
				// save this key as the hmac key
				HMAC_Init_ex(ctxs[k].hmac_ctx, cur_key, cur_key_len, out_md, NULL);
				break;
			default:
				// throw this key away
				break;
		}
		if(j > 3) {
			j = 0;
			k++;
		}
		j++;
		i++;
	}

	// clean up
	HMAC_CTX_free(hmac_ctx);
	return 1;
}


// generate random keys
int cryptoSetKeysRandom(struct s_crypto *ctxs, const int count) {
	unsigned char buf_a[256];
	unsigned char buf_b[256];
	cryptoRand(buf_a, 256);
	cryptoRand(buf_b, 256);
	return cryptoSetKeys(ctxs, count, buf_a, 256, buf_b, 256);
}


// destroy cipher contexts
void cryptoDestroy(struct s_crypto *ctxs, const int count) {
	int i;
	cryptoSetKeysRandom(ctxs, count);
	for(i=0; i<count; i++) {
		HMAC_CTX_free(ctxs[i].hmac_ctx);
		EVP_CIPHER_CTX_free(ctxs[i].dec_ctx);
		EVP_CIPHER_CTX_free(ctxs[i].enc_ctx);
	}
}


// create cipher contexts
int cryptoCreate(struct s_crypto *ctxs, const int count) {
	int i;
	for(i=0; i<count; i++) {
		ctxs[i].enc_ctx = EVP_CIPHER_CTX_new();
		ctxs[i].dec_ctx = EVP_CIPHER_CTX_new();
		ctxs[i].hmac_ctx = HMAC_CTX_new();
	}
	if(cryptoSetKeysRandom(ctxs, count)) {
		return 1;
	}
	else {
		cryptoDestroy(ctxs, count);
		return 0;
	}
}


// generate HMAC tag
int cryptoHMAC(struct s_crypto *ctx, unsigned char *hmac_buf, const int hmac_len, const unsigned char *in_buf, const int in_len) {
	unsigned char hmac[EVP_MAX_MD_SIZE];
	int len;
	HMAC_Init_ex(ctx->hmac_ctx, NULL, -1, NULL, NULL);
	HMAC_Update(ctx->hmac_ctx, in_buf, in_len);
	HMAC_Final(ctx->hmac_ctx, hmac, (unsigned int *)&len);
	if(len < hmac_len) return 0;
	memcpy(hmac_buf, hmac, hmac_len);
	return 1;
}


// generate session keys
int cryptoSetSessionKeys(struct s_crypto *session_ctx, struct s_crypto *cipher_keygen_ctx, struct s_crypto *md_keygen_ctx, const unsigned char *nonce, const int nonce_len, const int cipher_algorithm, const int hmac_algorithm) {
	struct s_crypto_cipher st_cipher;
	struct s_crypto_md st_md;

	// select algorithms
	switch(cipher_algorithm) {
		case crypto_AES256: st_cipher = cryptoGetEVPCipher(EVP_aes_256_cbc()); break;
		default: return 0;
	}
	switch(hmac_algorithm) {
		case crypto_SHA256: st_md = cryptoGetEVPMD(EVP_sha256()); break;
		default: return 0;
	}

	// calculate the keys
	const int key_size = cryptoGetEVPCipherSize(&st_cipher);
	unsigned char cipher_key[key_size];
	unsigned char hmac_key[key_size];
	if(!cryptoHMAC(cipher_keygen_ctx, cipher_key, key_size, nonce, nonce_len)) return 0;
	if(!cryptoHMAC(md_keygen_ctx, hmac_key, key_size, nonce, nonce_len)) return 0;

	// set the keys
	if(!EVP_EncryptInit_ex(session_ctx->enc_ctx, st_cipher.cipher, NULL, cipher_key, NULL)) return 0;
	if(!EVP_DecryptInit_ex(session_ctx->dec_ctx, st_cipher.cipher, NULL, cipher_key, NULL)) return 0;
	HMAC_Init_ex(session_ctx->hmac_ctx, hmac_key, key_size, st_md.md, NULL);

	return 1;
}


// encrypt buffer
int cryptoEnc(struct s_crypto *ctx, unsigned char *enc_buf, const int enc_len, const unsigned char *dec_buf, const int dec_len, const int hmac_len, const int iv_len) {
	if(!((enc_len > 0) && (dec_len > 0) && (dec_len < enc_len) && (hmac_len > 0) && (hmac_len <= crypto_MAXHMACSIZE) && (iv_len > 0) && (iv_len <= crypto_MAXIVSIZE))) { return 0; }

	unsigned char iv[crypto_MAXIVSIZE];
	unsigned char hmac[hmac_len];
	const int hdr_len = (hmac_len + iv_len);
	int cr_len;
	int len;

	if(enc_len < (hdr_len + crypto_MAXIVSIZE + dec_len)) { return 0; }

	memset(iv, 0, crypto_MAXIVSIZE);
	cryptoRand(iv, iv_len);
	memcpy(&enc_buf[hmac_len], iv, iv_len);

	if(!EVP_EncryptInit_ex(ctx->enc_ctx, NULL, NULL, NULL, iv)) { return 0; }
	if(!EVP_EncryptUpdate(ctx->enc_ctx, &enc_buf[(hdr_len)], &len, dec_buf, dec_len)) { return 0; }
	cr_len = len;
	if(!EVP_EncryptFinal(ctx->enc_ctx, &enc_buf[(hdr_len + cr_len)], &len)) { return 0; }
	cr_len += len;

	if(!cryptoHMAC(ctx, hmac, hmac_len, &enc_buf[hmac_len], (iv_len + cr_len))) { return 0; }
	memcpy(enc_buf, hmac, hmac_len);

	return (hdr_len + cr_len);
}


// decrypt buffer
int cryptoDec(struct s_crypto *ctx, unsigned char *dec_buf, const int dec_len, const unsigned char *enc_buf, const int enc_len, const int hmac_len, const int iv_len) {
	if(!((enc_len > 0) && (dec_len > 0) && (enc_len < dec_len) && (hmac_len > 0) && (hmac_len <= crypto_MAXHMACSIZE) && (iv_len > 0) && (iv_len <= crypto_MAXIVSIZE))) { return 0; }

	unsigned char iv[crypto_MAXIVSIZE];
	unsigned char hmac[hmac_len];
	const int hdr_len = (hmac_len + iv_len);
	int cr_len;
	int len;

	if(enc_len < hdr_len) { return 0; }

	if(!cryptoHMAC(ctx, hmac, hmac_len, &enc_buf[hmac_len], (enc_len - hmac_len))) { return 0; }
	if(memcmp(hmac, enc_buf, hmac_len) != 0) { return 0; }

	memset(iv, 0, crypto_MAXIVSIZE);
	memcpy(iv, &enc_buf[hmac_len], iv_len);

	if(!EVP_DecryptInit_ex(ctx->dec_ctx, NULL, NULL, NULL, iv)) { return 0; }
	if(!EVP_DecryptUpdate(ctx->dec_ctx, dec_buf, &len, &enc_buf[hdr_len], (enc_len - hdr_len))) { return 0; }
	cr_len = len;
	if(!EVP_DecryptFinal(ctx->dec_ctx, &dec_buf[cr_len], &len)) { return 0; }
	cr_len += len;

	return cr_len;
}


// calculate hash
int cryptoCalculateHash(unsigned char *hash_buf, const int hash_len, const unsigned char *in_buf, const int in_len, const EVP_MD *hash_func) {
	unsigned char hash[EVP_MAX_MD_SIZE];
	int len;
	EVP_MD_CTX *ctx;
	ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(ctx, hash_func, NULL);
	EVP_DigestUpdate(ctx, in_buf, in_len);
	EVP_DigestFinal(ctx, hash, (unsigned int *)&len);
	EVP_MD_CTX_free(ctx);
	if(len < hash_len) return 0;
	memcpy(hash_buf, hash, hash_len);
	return 1;
}


// calculate SHA-256 hash
int cryptoCalculateSHA256(unsigned char *hash_buf, const int hash_len, const unsigned char *in_buf, const int in_len) {
	return cryptoCalculateHash(hash_buf, hash_len, in_buf, in_len, EVP_sha256());
}


// calculate SHA-512 hash
int cryptoCalculateSHA512(unsigned char *hash_buf, const int hash_len, const unsigned char *in_buf, const int in_len) {
	return cryptoCalculateHash(hash_buf, hash_len, in_buf, in_len, EVP_sha512());
}


// generate session keys from password
int cryptoSetSessionKeysFromPassword(struct s_crypto *session_ctx, const unsigned char *password, const int password_len, const int cipher_algorithm, const int hmac_algorithm) {
	unsigned char key_a[64];
	unsigned char key_b[64];
	struct s_crypto ctx[2];
	int i;
	int ret_a, ret_b;
	ret_b = 0;
	if(cryptoCreate(ctx, 2)) {
		if(cryptoCalculateSHA512(key_a, 64, password, password_len)) {
			ret_a = 1;
			for(i=0; i<31337; i++) { // hash the password multiple times
				if(!cryptoCalculateSHA512(key_b, 64, key_a, 64)) { ret_a = 0; break; }
				if(!cryptoCalculateSHA512(key_a, 64, key_b, 64)) { ret_a = 0; break; }
			}
			if(ret_a) {
				if(cryptoSetKeys(ctx, 2, key_a, 32, &key_a[32], 32)) {
					ret_b = cryptoSetSessionKeys(session_ctx, &ctx[0], &ctx[1], key_b, 64, cipher_algorithm, hmac_algorithm);
				}
			}
		}
		cryptoDestroy(ctx, 2);
	}
	return ret_b;
}

#if OPENSSL_VERSION_NUMBER < 0x10100000L
void DH_get0_key(const DH *dh, const BIGNUM **pub_key, const BIGNUM **priv_key) {
	if (pub_key != NULL)
		*pub_key = dh->pub_key;
	if (priv_key != NULL)
		*priv_key = dh->priv_key;
}

HMAC_CTX *HMAC_CTX_new(void) {
	HMAC_CTX *ctx = OPENSSL_malloc(sizeof(*ctx));
	if (ctx != NULL) {
		if (!HMAC_CTX_reset(ctx)) {
			HMAC_CTX_free(ctx);
			return NULL;
		}
	}
	return ctx;
}

void HMAC_CTX_free(HMAC_CTX *ctx) {
	if (ctx != NULL) {
		hmac_ctx_cleanup(ctx);
		EVP_MD_CTX_free(ctx->i_ctx);
		EVP_MD_CTX_free(ctx->o_ctx);
		EVP_MD_CTX_free(ctx->md_ctx);
		OPENSSL_free(ctx);
	}
}

EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void) {
	EVP_CIPHER_CTX *ctx = OPENSSL_malloc(sizeof(*ctx));
	if (ctx != NULL) [
		if (!EVP_CIPHER_CTX_reset(ctx== {
			EVP_CIPHER_CTX_free(ctx);
			return NULL;
		}
	}
	return ctx;
}

void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *ctx) {
	if (ctx != NULL) {
		EVP_CIPHER_CTX_reset(ctx);
		OPENSSL_free(ctx);
	}
}

EVP_MD_CTX *EVP_MD_CTX_new(void) {
	return OPENSSL_zalloc(sizeof(EVP_MD_CTX));
}

void EVP_MD_CTX_free(EVP_MD_CTX *ctx) {
	EVP_MD_CTX_cleanup(ctx);
	OPENSSL_free(ctx);
}

int OPENSSL_init_crypto(uint64_t opts, const OPENSSL_INIT_SETTINGS *settings) {
	switch (opts) {
		case OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS | OPENSSL_INIT_LOAD_CONFIG:
			OPENSSL_add_all_algorithms_conf();
			break;
		case OPENSSL_INIT_ADD_ALL_CIPHERS | OPENSSL_INIT_ADD_ALL_DIGESTS:
			OPENSSL_add_all_algorithms_noconf();
			break;
		case OPENSSL_INIT_ADD_ALL_CIPHERS:
			OpenSSL_add_all_ciphers();
			break;
		case OPENSSL_INIT_ADD_ALL_DIGESTS:
			OpenSSL_add_all_digests();
			break;
		default:
			return 0;
	}
	return 1;
}
#endif

#endif // F_CRYPTO_C
