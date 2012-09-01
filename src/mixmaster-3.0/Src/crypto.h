/* Mixmaster version 3.0  --  (C) 1999 - 2006 Anonymizer Inc. and others.

   Mixmaster may be redistributed and modified under certain conditions.
   This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
   ANY KIND, either express or implied. See the file COPYRIGHT for
   details.

   Interface to cryptographic library
   $Id: crypto.h 934 2006-06-24 13:40:39Z rabbi $ */


#ifndef _CRYPTO_H
#define _CRYPTO_H
#include "mix3.h"

#ifdef USE_OPENSSL
#include <openssl/opensslv.h>
#if (OPENSSL_VERSION_NUMBER < 0x0903100)
#error "This version of OpenSSL is not supported. Please get a more current version from http://www.openssl.org"
#endif /* version check */
#include <openssl/des.h>
#include <openssl/blowfish.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/ripemd.h>
#include <openssl/bn.h>
#include <openssl/dh.h>
#include <openssl/dsa.h>
#include <openssl/rsa.h>
#ifdef USE_IDEA
#include <openssl/idea.h>
#endif /* USE_IDEA */
#ifdef USE_AES
#include <openssl/aes.h>
#endif /* USE_AES */
#include <openssl/cast.h>
#include <openssl/rand.h>

typedef RSA PUBKEY;
typedef RSA SECKEY;

#else /* end of USE_OPENSSL */
/* #error "No crypto library." */
typedef void PUBKEY;
typedef void SECKEY;
#endif /* else not USE_OPENSSL */

#endif /* ifndef _CRYPTO_H */
