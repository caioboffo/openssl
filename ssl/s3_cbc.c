/* ssl/s3_cbc.c */
/* ====================================================================
 * Copyright (c) 2012 The OpenSSL Project.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. All advertising materials mentioning features or use of this
 *    software must display the following acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit. (http://www.openssl.org/)"
 *
 * 4. The names "OpenSSL Toolkit" and "OpenSSL Project" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For written permission, please contact
 *    openssl-core@openssl.org.
 *
 * 5. Products derived from this software may not be called "OpenSSL"
 *    nor may "OpenSSL" appear in their names without prior written
 *    permission of the OpenSSL Project.
 *
 * 6. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by the OpenSSL Project
 *    for use in the OpenSSL Toolkit (http://www.openssl.org/)"
 *
 * THIS SOFTWARE IS PROVIDED BY THE OpenSSL PROJECT ``AS IS'' AND ANY
 * EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OpenSSL PROJECT OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 * ====================================================================
 *
 * This product includes cryptographic software written by Eric Young
 * (eay@cryptsoft.com).  This product includes software written by Tim
 * Hudson (tjh@cryptsoft.com).
 *
 */

#include <stdint.h>

#include "ssl_locl.h"

#include <openssl/md5.h>
#include <openssl/sha.h>

/* MAX_HASH_BIT_COUNT_BYTES is the maximum number of bytes in the hash's length
 * field. (SHA-384/512 have 128-bit length.) */
#define MAX_HASH_BIT_COUNT_BYTES 16

/* MAX_HASH_BLOCK_SIZE is the maximum hash block size that we'll support.
 * Currently SHA-384/512 has a 128-byte block size and that's the largest
 * supported by TLS.) */
#define MAX_HASH_BLOCK_SIZE 128

/* Some utility functions are needed:
 *
 * These macros return the given value with the MSB copied to all the other
 * bits. They use the fact that arithmetic shift shifts-in the sign bit.
 * However, this is not ensured by the C standard so you may need to replace
 * them with something else on odd CPUs. */
#define DUPLICATE_MSB_TO_ALL(x) ( (unsigned)( (int)(x) >> (sizeof(int)*8-1) ) )
#define DUPLICATE_MSB_TO_ALL_8(x) ((unsigned char)(DUPLICATE_MSB_TO_ALL(x)))

/* constant_time_ge returns 0xff if a>=b and 0x00 otherwise. */
static unsigned constant_time_ge(unsigned a, unsigned b)
	{
	a -= b;
	return DUPLICATE_MSB_TO_ALL(~a);
	}

/* constant_time_eq_8 returns 0xff if a==b and 0x00 otherwise. */
static unsigned char constant_time_eq_8(unsigned char a, unsigned char b)
	{
	unsigned c = a ^ b;
	c--;
	return DUPLICATE_MSB_TO_ALL_8(c);
	}

/* ssl3_cbc_remove_padding removes padding from the decrypted, SSLv3, CBC
 * record in |rec| by updating |rec->length| in constant time.
 *
 * block_size: the block size of the cipher used to encrypt the record.
 * returns:
 *   0: (in non-constant time) if the record is publicly invalid.
 *   1: if the padding was valid
 *  -1: otherwise. */
int ssl3_cbc_remove_padding(const SSL* s,
			    SSL3_RECORD *rec,
			    unsigned block_size,
			    unsigned mac_size)
	{
	unsigned padding_length, good;
	const unsigned overhead = 1 /* padding length byte */ + mac_size;

	/* These lengths are all public so we can test them in non-constant
	 * time. */
	if (overhead > rec->length)
		return 0;

	padding_length = rec->data[rec->length-1];
	good = constant_time_ge(rec->length, padding_length+overhead);
	/* SSLv3 requires that the padding is minimal. */
	good &= constant_time_ge(block_size, padding_length+1);
	rec->length -= good & (padding_length+1);
	return (int)((good & 1) | (~good & -1));
}

/* tls1_cbc_remove_padding removes the CBC padding from the decrypted, TLS, CBC
 * record in |rec| in constant time and returns 1 if the padding is valid and
 * -1 otherwise. It also removes any explicit IV from the start of the record
 * without leaking any timing about whether there was enough space after the
 * padding was removed.
 *
 * block_size: the block size of the cipher used to encrypt the record.
 * returns:
 *   0: (in non-constant time) if the record is publicly invalid.
 *   1: if the padding was valid
 *  -1: otherwise. */
int tls1_cbc_remove_padding(const SSL* s,
			    SSL3_RECORD *rec,
			    unsigned block_size,
			    unsigned mac_size)
	{
	unsigned padding_length, good, to_check, i;
	const char has_explicit_iv = s->version == DTLS1_VERSION;
	const unsigned overhead = 1 /* padding length byte */ +
				  mac_size +
				  (has_explicit_iv ? block_size : 0);

	/* These lengths are all public so we can test them in non-constant
	 * time. */
	if (overhead > rec->length)
		return 0;

	padding_length = rec->data[rec->length-1];

	/* NB: if compression is in operation the first packet may not be of
	 * even length so the padding bug check cannot be performed. This bug
	 * workaround has been around since SSLeay so hopefully it is either
	 * fixed now or no buggy implementation supports compression [steve]
	 */
	if ( (s->options&SSL_OP_TLS_BLOCK_PADDING_BUG) && !s->expand)
		{
		/* First packet is even in size, so check */
		if ((memcmp(s->s3->read_sequence, "\0\0\0\0\0\0\0\0",8) == 0) &&
		    !(padding_length & 1))
			{
			s->s3->flags|=TLS1_FLAGS_TLS_PADDING_BUG;
			}
		if ((s->s3->flags & TLS1_FLAGS_TLS_PADDING_BUG) &&
		    padding_length > 0)
			{
			padding_length--;
			}
		}

	good = constant_time_ge(rec->length, overhead+padding_length);
	/* The padding consists of a length byte at the end of the record and
	 * then that many bytes of padding, all with the same value as the
	 * length byte. Thus, with the length byte included, there are i+1
	 * bytes of padding.
	 *
	 * We can't check just |padding_length+1| bytes because that leaks
	 * decrypted information. Therefore we always have to check the maximum
	 * amount of padding possible. (Again, the length of the record is
	 * public information so we can use it.) */
	to_check = 255; /* maximum amount of padding. */
	if (to_check > rec->length-1)
		to_check = rec->length-1;

	for (i = 0; i < to_check; i++)
		{
		unsigned char mask = constant_time_ge(padding_length, i);
		unsigned char b = rec->data[rec->length-1-i];
		/* The final |padding_length+1| bytes should all have the value
		 * |padding_length|. Therefore the XOR should be zero. */
		good &= ~(mask&(padding_length ^ b));
		}

	/* If any of the final |padding_length+1| bytes had the wrong value,
	 * one or more of the lower eight bits of |good| will be cleared. We
	 * AND the bottom 8 bits together and duplicate the result to all the
	 * bits. */
	good &= good >> 4;
	good &= good >> 2;
	good &= good >> 1;
	good <<= sizeof(good)*8-1;
	good = DUPLICATE_MSB_TO_ALL(good);

	rec->length -= good & (padding_length+1);

	/* We can always safely skip the explicit IV. We check at the beginning
	 * of this function that the record has at least enough space for the
	 * IV, MAC and padding length byte. (These can be checked in
	 * non-constant time because it's all public information.) So, if the
	 * padding was invalid, then we didn't change |rec->length| and this is
	 * safe. If the padding was valid then we know that we have at least
	 * overhead+padding_length bytes of space and so this is still safe
	 * because overhead accounts for the explicit IV. */
	if (has_explicit_iv)
		{
		rec->data += block_size;
		rec->input += block_size;
		rec->length -= block_size;
		rec->orig_len -= block_size;
		}

	return (int)((good & 1) | (~good & -1));
	}

#if defined(_M_AMD64) || defined(__x86_64__)
#define CBC_MAC_ROTATE_IN_PLACE
#endif

/* ssl3_cbc_copy_mac copies |md_size| bytes from the end of |rec| to |out| in
 * constant time (independent of the concrete value of rec->length, which may
 * vary within a 256-byte window).
 *
 * ssl3_cbc_remove_padding or tls1_cbc_remove_padding must be called prior to
 * this function.
 *
 * On entry:
 *   rec->orig_len >= md_size
 *   md_size <= EVP_MAX_MD_SIZE
 *
 * If CBC_MAC_ROTATE_IN_PLACE is defined then the rotation is performed with
 * variable accesses in a 64-byte-aligned buffer. Assuming that this fits into
 * a single cache-line, then the variable memory accesses don't actually affect
 * the timing. This has been tested to be true on Intel amd64 chips.
 */
void ssl3_cbc_copy_mac(unsigned char* out,
		       const SSL3_RECORD *rec,
		       unsigned md_size)
	{
#if defined(CBC_MAC_ROTATE_IN_PLACE)
	unsigned char rotated_mac_buf[EVP_MAX_MD_SIZE*2];
	unsigned char *rotated_mac;
#else
	unsigned char rotated_mac[EVP_MAX_MD_SIZE];
#endif

	/* mac_end is the index of |rec->data| just after the end of the MAC. */
	unsigned mac_end = rec->length;
	unsigned mac_start = mac_end - md_size;
	/* scan_start contains the number of bytes that we can ignore because
	 * the MAC's position can only vary by 255 bytes. */
	unsigned scan_start = 0;
	unsigned i, j;
	unsigned div_spoiler;
	unsigned rotate_offset;

	OPENSSL_assert(rec->orig_len >= md_size);
	OPENSSL_assert(md_size <= EVP_MAX_MD_SIZE);

#if defined(CBC_MAC_ROTATE_IN_PLACE)
	rotated_mac = (unsigned char*) (((intptr_t)(rotated_mac_buf + 64)) & ~63);
#endif

	/* This information is public so it's safe to branch based on it. */
	if (rec->orig_len > md_size + 255 + 1)
		scan_start = rec->orig_len - (md_size + 255 + 1);
	/* div_spoiler contains a multiple of md_size that is used to cause the
	 * modulo operation to be constant time. Without this, the time varies
	 * based on the amount of padding when running on Intel chips at least.
	 *
	 * The aim of right-shifting md_size is so that the compiler doesn't
	 * figure out that it can remove div_spoiler as that would require it
	 * to prove that md_size is always even, which I hope is beyond it. */
	div_spoiler = md_size >> 1;
	div_spoiler <<= (sizeof(div_spoiler)-1)*8;
	rotate_offset = (div_spoiler + mac_start - scan_start) % md_size;

	memset(rotated_mac, 0, md_size);
	for (i = scan_start; i < rec->orig_len;)
		{
		for (j = 0; j < md_size && i < rec->orig_len; i++, j++)
			{
			unsigned char mac_started = constant_time_ge(i, mac_start);
			unsigned char mac_ended = constant_time_ge(i, mac_end);
			unsigned char b = 0;
			b = rec->data[i];
			rotated_mac[j] |= b & mac_started & ~mac_ended;
			}
		}

	/* Now rotate the MAC */
#if defined(CBC_MAC_ROTATE_IN_PLACE)
	j = 0;
	for (i = 0; i < md_size; i++)
		{
		unsigned char offset = (div_spoiler + rotate_offset + i) % md_size;
		out[j++] = rotated_mac[offset];
		}
#else
	memset(out, 0, md_size);
	for (i = 0; i < md_size; i++)
		{
		unsigned char offset = (div_spoiler + md_size - rotate_offset + i) % md_size;
		for (j = 0; j < md_size; j++)
			out[j] |= rotated_mac[i] & constant_time_eq_8(j, offset);
		}
#endif
	}

/* These functions serialize the state of a hash and thus perform the standard
 * "final" operation without adding the padding and length that such a function
 * typically does. */
static void tls1_md5_final_raw(void* ctx, unsigned char *md_out)
	{
	MD5_CTX *md5 = ctx;
	l2n(md5->A, md_out);
	l2n(md5->B, md_out);
	l2n(md5->C, md_out);
	l2n(md5->D, md_out);
	}

static void tls1_sha1_final_raw(void* ctx, unsigned char *md_out)
	{
	SHA_CTX *sha1 = ctx;
	l2n(sha1->h0, md_out);
	l2n(sha1->h1, md_out);
	l2n(sha1->h2, md_out);
	l2n(sha1->h3, md_out);
	l2n(sha1->h4, md_out);
	}

static void tls1_sha256_final_raw(void* ctx, unsigned char *md_out)
	{
	SHA256_CTX *sha256 = ctx;
	unsigned i;

	for (i = 0; i < 8; i++)
		{
		l2n(sha256->h[i], md_out);
		}
	}

static void tls1_sha512_final_raw(void* ctx, unsigned char *md_out)
	{
	SHA512_CTX *sha512 = ctx;
	unsigned i;

	for (i = 0; i < 8; i++)
		{
		l2n8(sha512->h[i], md_out);
		}
	}

/* ssl3_cbc_record_digest_supported returns 1 iff |ctx| uses a hash function
 * which ssl3_cbc_digest_record supports. */
char ssl3_cbc_record_digest_supported(const EVP_MD_CTX *ctx)
	{
	switch (ctx->digest->type)
		{
		case NID_md5:
		case NID_sha1:
		case NID_sha224:
		case NID_sha256:
		case NID_sha384:
		case NID_sha512:
			return 1;
		default:
			return 0;
		}
	}

/* ssl3_cbc_digest_record computes the MAC of a decrypted, padded SSLv3/TLS
 * record.
 *
 *   ctx: the EVP_MD_CTX from which we take the hash function.
 *     ssl3_cbc_record_digest_supported must return true for this EVP_MD_CTX.
 *   md_out: the digest output. At most EVP_MAX_MD_SIZE bytes will be written.
 *   md_out_size: if non-NULL, the number of output bytes is written here.
 *   header: the 13-byte, TLS record header.
 *   data: the record data itself, less any preceeding explicit IV.
 *   data_plus_mac_size: the secret, reported length of the data and MAC
 *     once the padding has been removed.
 *   data_plus_mac_plus_padding_size: the public length of the whole
 *     record, including padding.
 *   is_sslv3: non-zero if we are to use SSLv3. Otherwise, TLS.
 *
 * On entry: by virtue of having been through one of the remove_padding
 * functions, above, we know that data_plus_mac_size is large enough to contain
 * a padding byte and MAC. (If the padding was invalid, it might contain the
 * padding too. ) */
void ssl3_cbc_digest_record(
	const EVP_MD_CTX *ctx,
	unsigned char* md_out,
	size_t* md_out_size,
	const unsigned char header[13],
	const unsigned char *data,
	size_t data_plus_mac_size,
	size_t data_plus_mac_plus_padding_size,
	const unsigned char *mac_secret,
	unsigned mac_secret_length,
	char is_sslv3)
	{
	unsigned char md_state[sizeof(SHA512_CTX)];
	void (*md_final_raw)(void *ctx, unsigned char *md_out);
	void (*md_transform)(void *ctx, const unsigned char *block);
	unsigned md_size, md_block_size = 64;
	unsigned sslv3_pad_length = 40, header_length, variance_blocks,
		 len, max_mac_bytes, num_blocks,
		 num_starting_blocks, k, mac_end_offset, c, index_a, index_b;
	uint64_t bits;
	unsigned char length_bytes[MAX_HASH_BIT_COUNT_BYTES];
	/* hmac_pad is the masked HMAC key. */
	unsigned char hmac_pad[MAX_HASH_BLOCK_SIZE];
	unsigned char first_block[MAX_HASH_BLOCK_SIZE];
	unsigned char mac_out[EVP_MAX_MD_SIZE];
	unsigned i, j, md_out_size_u;
	EVP_MD_CTX md_ctx;
	/* mdLengthSize is the number of bytes in the length field that terminates
	* the hash. */
	unsigned md_length_size = 8;

	/* This is a, hopefully redundant, check that allows us to forget about
	 * many possible overflows later in this function. */
	OPENSSL_assert(data_plus_mac_plus_padding_size < 1024*1024);

	switch (ctx->digest->type)
		{
		case NID_md5:
			MD5_Init((MD5_CTX*)md_state);
			md_final_raw = tls1_md5_final_raw;
			md_transform = (void(*)(void *ctx, const unsigned char *block)) MD5_Transform;
			md_size = 16;
			sslv3_pad_length = 48;
			break;
		case NID_sha1:
			SHA1_Init((SHA_CTX*)md_state);
			md_final_raw = tls1_sha1_final_raw;
			md_transform = (void(*)(void *ctx, const unsigned char *block)) SHA1_Transform;
			md_size = 20;
			break;
		case NID_sha224:
			SHA224_Init((SHA256_CTX*)md_state);
			md_final_raw = tls1_sha256_final_raw;
			md_transform = (void(*)(void *ctx, const unsigned char *block)) SHA256_Transform;
			md_size = 224/8;
			break;
		case NID_sha256:
			SHA256_Init((SHA256_CTX*)md_state);
			md_final_raw = tls1_sha256_final_raw;
			md_transform = (void(*)(void *ctx, const unsigned char *block)) SHA256_Transform;
			md_size = 32;
			break;
		case NID_sha384:
			SHA384_Init((SHA512_CTX*)md_state);
			md_final_raw = tls1_sha512_final_raw;
			md_transform = (void(*)(void *ctx, const unsigned char *block)) SHA512_Transform;
			md_size = 384/8;
			md_block_size = 128;
			md_length_size = 16;
			break;
		case NID_sha512:
			SHA512_Init((SHA512_CTX*)md_state);
			md_final_raw = tls1_sha512_final_raw;
			md_transform = (void(*)(void *ctx, const unsigned char *block)) SHA512_Transform;
			md_size = 64;
			md_block_size = 128;
			md_length_size = 16;
			break;
		default:
			/* ssl3_cbc_record_digest_supported should have been
			 * called first to check that the hash function is
			 * supported. */
			OPENSSL_assert(0);
			if (md_out_size)
				*md_out_size = -1;
			return;
		}

	OPENSSL_assert(md_length_size <= MAX_HASH_BIT_COUNT_BYTES);
	OPENSSL_assert(md_block_size <= MAX_HASH_BLOCK_SIZE);
	OPENSSL_assert(md_size <= EVP_MAX_MD_SIZE);

	header_length = 13;
	if (is_sslv3)
		{
		header_length =
			mac_secret_length +
			sslv3_pad_length +
			8 /* sequence number */ +
			1 /* record type */ +
			2 /* record length */;
		}

	/* variance_blocks is the number of blocks of the hash that we have to
	 * calculate in constant time because they could be altered by the
	 * padding value.
	 *
	 * In SSLv3, the padding must be minimal so the end of the plaintext
	 * varies by, at most, 15+20 = 35 bytes. (We conservatively assume that
	 * the MAC size varies from 0..20 bytes.) In case the 9 bytes of hash
	 * termination (0x80 + 64-bit length) don't fit in the final block, we
	 * say that the final two blocks can vary based on the padding.
	 *
	 * TLSv1 has MACs up to 48 bytes long (SHA-384) and the padding is not
	 * required to be minimal. Therefore we say that the final six blocks
	 * can vary based on the padding.
	 *
	 * Later in the function, if the message is short and there obviously
	 * cannot be this many blocks then variance_blocks can be reduced. */
	variance_blocks = is_sslv3 ? 2 : 6;
	/* From now on we're dealing with the MAC, which conceptually has 13
	 * bytes of `header' before the start of the data (TLS) or 71/75 bytes
	 * (SSLv3) */
	len = data_plus_mac_plus_padding_size + header_length;
	/* max_mac_bytes contains the maximum bytes of bytes in the MAC, including
	* |header|, assuming that there's no padding. */
	max_mac_bytes = len - md_size - 1;
	/* num_blocks is the maximum number of hash blocks. */
	num_blocks = (max_mac_bytes + 1 + md_length_size + md_block_size - 1) / md_block_size;
	/* In order to calculate the MAC in constant time we have to handle
	 * the final blocks specially because the padding value could cause the
	 * end to appear somewhere in the final |variance_blocks| blocks and we
	 * can't leak where. However, |num_starting_blocks| worth of data can
	 * be hashed right away because no padding value can affect whether
	 * they are plaintext. */
	num_starting_blocks = 0;
	/* k is the starting byte offset into the conceptual header||data where
	 * we start processing. */
	k = 0;
	/* mac_end_offset is the index just past the end of the data to be
	 * MACed. */
	mac_end_offset = data_plus_mac_size + header_length - md_size;
	/* c is the index of the 0x80 byte in the final hash block that
	 * contains application data. */
	c = mac_end_offset % md_block_size;
	/* index_a is the hash block number that contains the 0x80 terminating
	 * value. */
	index_a = mac_end_offset / md_block_size;
	/* index_b is the hash block number that contains the 64-bit hash
	 * length, in bits. */
	index_b = (mac_end_offset + md_length_size) / md_block_size;
	/* bits is the hash-length in bits. It includes the additional hash
	 * block for the masked HMAC key, or whole of |header| in the case of
	 * SSLv3. */

	/* For SSLv3, if we're going to have any starting blocks then we need
	 * at least two because the header is larger than a single block. */
	if (num_blocks > variance_blocks + (is_sslv3 ? 1 : 0))
		{
		num_starting_blocks = num_blocks - variance_blocks;
		k = md_block_size*num_starting_blocks;
		}

	bits = 8*mac_end_offset;
	if (!is_sslv3)
		{
		/* Compute the initial HMAC block. For SSLv3, the padding and
		 * secret bytes are included in |header| because they take more
		 * than a single block. */
		bits += 8*md_block_size;
		memset(hmac_pad, 0, md_block_size);
		OPENSSL_assert(mac_secret_length <= sizeof(hmac_pad));
		memcpy(hmac_pad, mac_secret, mac_secret_length);
		for (i = 0; i < md_block_size; i++)
			hmac_pad[i] ^= 0x36;

		md_transform(md_state, hmac_pad);
		}

	j = 0;
	if (md_length_size == 16)
		{
		memset(length_bytes, 0, 8);
		j = 8;
		}
	for (i = 0; i < 8; i++)
		length_bytes[i+j] = bits >> (8*(7-i));

	if (k > 0)
		{
		if (is_sslv3)
			{
			/* The SSLv3 header is larger than a single block.
			 * overhang is the number of bytes beyond a single
			 * block that the header consumes: either 7 bytes
			 * (SHA1) or 11 bytes (MD5). */
			unsigned overhang = header_length-md_block_size;
			md_transform(md_state, header);
			memcpy(first_block, header + md_block_size, overhang);
			memcpy(first_block + overhang, data, md_block_size-overhang);
			md_transform(md_state, first_block);
			for (i = 1; i < k/md_block_size - 1; i++)
				md_transform(md_state, data + md_block_size*i - overhang);
			}
		else
			{
			/* k is a multiple of md_block_size. */
			memcpy(first_block, header, 13);
			memcpy(first_block+13, data, md_block_size-13);
			md_transform(md_state, first_block);
			for (i = 1; i < k/md_block_size; i++)
				md_transform(md_state, data + md_block_size*i - 13);
			}
		}

	memset(mac_out, 0, sizeof(mac_out));

	/* We now process the final hash blocks. For each block, we construct
	 * it in constant time. If the |i==index_a| then we'll include the 0x80
	 * bytes and zero pad etc. For each block we selectively copy it, in
	 * constant time, to |mac_out|. */
	for (i = num_starting_blocks; i <= num_starting_blocks+variance_blocks; i++)
		{
		unsigned char block[MAX_HASH_BLOCK_SIZE];
		unsigned char is_block_a = constant_time_eq_8(i, index_a);
		unsigned char is_block_b = constant_time_eq_8(i, index_b);
		for (j = 0; j < md_block_size; j++)
			{
			unsigned char b = 0, is_past_c, is_past_cp1;
			if (k < header_length)
				b = header[k];
			else if (k < data_plus_mac_plus_padding_size + header_length)
				b = data[k-header_length];
			k++;

			is_past_c = is_block_a & constant_time_ge(j, c);
			is_past_cp1 = is_block_a & constant_time_ge(j, c+1);
			/* If this is the block containing the end of the
			 * application data, and we are at the offset for the
			 * 0x80 value, then overwrite b with 0x80. */
			b = (b&~is_past_c) | (0x80&is_past_c);
			/* If this the the block containing the end of the
			 * application data and we're past the 0x80 value then
			 * just write zero. */
			b = b&~is_past_cp1;
			/* If this is index_b (the final block), but not
			 * index_a (the end of the data), then the 64-bit
			 * length didn't fit into index_a and we're having to
			 * add an extra block of zeros. */
			b &= ~is_block_b | is_block_a;

			/* The final bytes of one of the blocks contains the
			 * length. */
			if (j >= md_block_size - md_length_size)
				{
				/* If this is index_b, write a length byte. */
				b = (b&~is_block_b) | (is_block_b&length_bytes[j-(md_block_size-md_length_size)]);
				}
			block[j] = b;
			}

		md_transform(md_state, block);
		md_final_raw(md_state, block);
		/* If this is index_b, copy the hash value to |mac_out|. */
		for (j = 0; j < md_size; j++)
			mac_out[j] |= block[j]&is_block_b;
		}

	EVP_MD_CTX_init(&md_ctx);
	EVP_DigestInit_ex(&md_ctx, ctx->digest, NULL /* engine */);
	if (is_sslv3)
		{
		/* We repurpose |hmac_pad| to contain the SSLv3 pad2 block. */
		memset(hmac_pad, 0x5c, sslv3_pad_length);

		EVP_DigestUpdate(&md_ctx, mac_secret, mac_secret_length);
		EVP_DigestUpdate(&md_ctx, hmac_pad, sslv3_pad_length);
		EVP_DigestUpdate(&md_ctx, mac_out, md_size);
		}
	else
		{
		/* Complete the HMAC in the standard manner. */
		for (i = 0; i < md_block_size; i++)
			hmac_pad[i] ^= 0x6a;

		EVP_DigestUpdate(&md_ctx, hmac_pad, md_block_size);
		EVP_DigestUpdate(&md_ctx, mac_out, md_size);
		}
	EVP_DigestFinal(&md_ctx, md_out, &md_out_size_u);
	if (md_out_size)
		*md_out_size = md_out_size_u;
	EVP_MD_CTX_cleanup(&md_ctx);
	}
