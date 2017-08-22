/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright 2014 Jason King.
 * Copyright 2017 Joyent, Inc.
 */

#ifndef _IKEV2_SA_H
#define	_IKEV2_SA_H

#include <sys/types.h>
#include <sys/socket.h>
#include <assert.h>
#include <synch.h>
#include <stddef.h>
#include <security/cryptoki.h>
#include <atomic.h>
#include <pthread.h>
#include <libuutil.h>

#include "ikev2.h"
#include "defs.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct ikev2_sa;
struct ikev2_child_sa;
struct i2sa_bucket;
struct pkt;
struct buf;

#ifndef IKEV2_SA_T
#define	IKEV2_SA_T
typedef struct ikev2_sa ikev2_sa_t;
typedef struct ikev2_child_sa ikev2_child_sa_t;
typedef struct i2sa_bucket i2sa_bucket_t;
#endif /* IKEV2_SA_T */

struct config_s;
struct config_rule_s;

#define	I2SA_NUM_HASH	2	/* The number of IKEv2 SA hashes we have */

#define	I2SA_SALT_LEN	(32)	/* Maximum size of salt */

/*
 * The IKEv2 SA.
 *
 * This is the central data structure to the IKEv2 daemon.  It is a
 * reference-counted node, where the lookup key is either the local
 * SPI/cookie, or a hash based on the remote address and remote SPI.  (See
 * ikev2_pkt.h for the _SPI() macros.)  It should be allocated with a umem
 * cache.  It contains a mutex to lock certain fields if need be.
 *
 * Because of the distinct sets of lookup keys, it requires two linkages.
 */
struct ikev2_sa {
	/*
	 * Fields that should not be zeroed out between trips before
	 * returning to the umem_cache should go at the top of this struct.
	 */
	pthread_mutex_t lock;

	uu_list_node_t	lspi_node;
	uu_list_node_t	rhash_node;

	bunyan_logger_t	*i2sa_log;

			/* Link to the bucket we are in for each hash */
	i2sa_bucket_t	*bucket[I2SA_NUM_HASH];

	struct config_s		*i2sa_cfg;
	struct config_rule_s	*i2sa_rule;

	uint64_t		i_spi;	  /* Initiator SPI. */
	uint64_t		r_spi;	  /* Responder SPI. */
	uint32_t		flags;
	volatile uint32_t	refcnt;

	struct sockaddr_storage laddr;  /* Local address & port. */
	struct sockaddr_storage raddr;  /* Remote address & port. */

	vendor_t		vendor;

	/* Current number of outstanding messages prior to outmsgid. */
	int		msgwin;

	int		encr;		/* Encryption algorithm */
	size_t		encr_key_len;	/* Key length (bytes) for encr */
	int		auth;		/* Authentication algorithm */
	int		prf;		/* PRF algorithm */
	int		dhgrp;		/* Diffie-Hellman group. */

	uint32_t outmsgid;		/* Next msgid for outbound packets. */
	uint32_t inmsgid;		/* Next expected inbound msgid. */

	struct pkt	*init;  	/* IKE_SA_INIT packet. */
	struct pkt	*last_resp_sent;
	struct pkt	*last_sent;
	struct pkt	*last_recvd;

	time_t		birth;		/* When was AUTH completed */
	hrtime_t	softexpire;
	hrtime_t	hardexpire;

	ikev2_child_sa_t	*child_sas;

	CK_OBJECT_HANDLE dh_pubkey;
	CK_OBJECT_HANDLE dh_privkey;
	CK_OBJECT_HANDLE dh_key;
	CK_OBJECT_HANDLE sk_d;
	CK_OBJECT_HANDLE sk_ai;
	CK_OBJECT_HANDLE sk_ar;
	CK_OBJECT_HANDLE sk_ei;
	CK_OBJECT_HANDLE sk_er;
	CK_OBJECT_HANDLE sk_pi;
	CK_OBJECT_HANDLE sk_pr;

	uint8_t		salt[I2SA_SALT_LEN];
	size_t		saltlen;
};

struct ikev2_child_sa {
	ikev2_child_sa_t	*next;
	ikev2_spi_proto_t	satype;
	uint32_t		spi;
	/* XXX: more to come probably */
};

/* SA flags */
#define	I2SA_INITIATOR		0x1	/* Am I the initiator of this IKE SA? */
#define	I2SA_NAT_LOCAL		0x2	/* I am behind a NAT. */
#define	I2SA_NAT_REMOTE		0x4	/* My peer is behind a NAT. */
#define	I2SA_CONDEMNED		0x8	/* SA is unlinked from a tree. */
#define	I2SA_AUTHENTICATED	0x10	/* SA has been authenticated */

#define	I2SA_LOCAL_SPI(i2sa) \
	(((i2sa)->flags & I2SA_INITIATOR) ? (i2sa)->i_spi : \
	    (i2sa)->r_spi)

#define	I2SA_REMOTE_SPI(i2sa) \
	(((i2sa)->flags & I2SA_INITIATOR) ? (i2sa)->r_spi : \
	    (i2sa)->i_spi)

#define	I2SA_IS_NAT(i2sa) \
	(!!((i2sa)->flags && (I2SA_NAT_LOCAL|I2SA_NAT_REMOTE)))

#define	I2SA_REFHOLD(i2sa) \
	atomic_inc_32(&(i2sa)->refcnt)

/* Stupid C tricks stolen from <assert.h>. */
#define	I2SA_REFRELE(i2sa) \
	(void) ((atomic_dec_32_nv(&(i2sa)->refcnt) != 0) || \
	    (ikev2_sa_free(i2sa), 0))

extern ikev2_sa_t *ikev2_sa_get(uint64_t, uint64_t,
    const struct sockaddr_storage *restrict,
    const struct sockaddr_storage *restrict,
    const struct pkt *restrict);
extern ikev2_sa_t *ikev2_sa_alloc(boolean_t, struct pkt *restrict,
    const struct sockaddr_storage *restrict,
    const struct sockaddr_storage *restrict);

extern void	ikev2_sa_free(ikev2_sa_t *);
extern void	ikev2_sa_condemn(ikev2_sa_t *);

extern void	ikev2_sa_flush(void);
extern void	ikev2_sa_set_hashsize(uint_t);

#ifdef  __cplusplus
}
#endif

#endif  /* _IKEV2_SA_H */
