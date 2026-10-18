/* Ed25519 signature check for the self-update (adapters/azimuth_update.c).
   A release carries azimuth-tls.dll.sig: the 64-byte raw Ed25519 signature, by the project's
   release key, of the 84-byte ASCII line
       "fh2online-update-v1 " + the DLL's SHA-256 in lowercase hex
   (no newline, nothing else). The release tooling makes it. Freestanding: no Windows headers,
   so tests/sign_test.c builds it for the host too. */
#ifndef FH2_AZIMUTH_SIGN_H
#define FH2_AZIMUTH_SIGN_H
#include <stddef.h>
#include <stdint.h>

#define AZIMUTH_SIGN_PREFIX "fh2online-update-v1 "
#define AZIMUTH_SIGN_LINE 84

/* The release public key (raw 32 bytes). */
extern const unsigned char azimuth_sign_release_key[32];

/* 1 when `signature` is a valid Ed25519 signature of `message` (`size` bytes) by `key`
   (RFC 8032 Ed25519, S below the group order), 0 otherwise. */
int azimuth_sign_verify(const unsigned char signature[64], const unsigned char *message, size_t size,
                        const unsigned char key[32]);

/* 1 when `signature` signs the update line of `digest` (the DLL's SHA-256) by `key`. */
int azimuth_sign_update_ok(const unsigned char signature[64], const unsigned char digest[32],
                           const unsigned char key[32]);
#endif
