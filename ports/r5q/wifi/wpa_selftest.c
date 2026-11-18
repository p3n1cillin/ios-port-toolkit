/* PC check of bsd/dev/redwood_wpa.h against published vectors: gcc -I. wpa_selftest.c && ./a.out */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "../kernel-build/r5q-overlay/bsd/dev/redwood_wpa.h"

static int
check(const char *what, const uint8_t *got, const char *hex)
{
	uint8_t want[128];
	size_t n = strlen(hex) / 2;

	for (size_t i = 0; i < n; i++) {
		sscanf(hex + 2 * i, "%2hhx", &want[i]);
	}
	printf("%-28s %s\n", what, memcmp(got, want, n) == 0 ? "ok" : "WRONG");
	return memcmp(got, want, n) != 0;
}

int
main(void)
{
	int bad = 0;
	uint8_t out[64], key[20];
	struct wpa_sha1 s;

	wpa_sha1_init(&s);
	wpa_sha1_add(&s, (const uint8_t *)"abc", 3);
	wpa_sha1_end(&s, out);
	bad += check("SHA-1 abc", out, "a9993e364706816aba3e25717850c26c9cd0d89d");
	wpa_sha1_init(&s);
	wpa_sha1_add(&s, (const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
	wpa_sha1_end(&s, out);
	bad += check("SHA-1 448 bits", out, "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
	memset(key, 0x0b, 20);
	{
		const uint8_t *d[1] = { (const uint8_t *)"Hi There" };
		size_t dl[1] = { 8 };

		wpa_hmac_sha1(key, 20, d, dl, 1, out);
		bad += check("HMAC-SHA1 RFC 2202 #1", out, "b617318655057264e28bc0b6fb378c8ef146be00");
	}
	{
		const uint8_t *d[1] = { (const uint8_t *)"what do ya want for nothing?" };
		size_t dl[1] = { 28 };

		wpa_hmac_sha1((const uint8_t *)"Jefe", 4, d, dl, 1, out);
		bad += check("HMAC-SHA1 RFC 2202 #2", out, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
	}
	/* IEEE 802.11-2016 J.3.2 (PRF-512, test case 1) */
	wpa_prf(key, 20, "prefix", (const uint8_t *)"Hi There", 8, out, 64);
	bad += check("802.11 PRF-512 #1", out, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
	    "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a");
	{
		uint8_t k[16], c[16], rk[176];

		for (int i = 0; i < 16; i++) {
			k[i] = (uint8_t)i;
		}
		for (int i = 0; i < 16; i++) {
			sscanf("69c4e0d86a7b0430d8cdb78070b4c55a" + 2 * i, "%2hhx", &c[i]);
		}
		wpa_aes_expand(k, rk);
		wpa_aes_decrypt(rk, c, out);
		bad += check("AES-128 decrypt FIPS-197", out, "00112233445566778899aabbccddeeff");
	}
	{
		uint8_t kek[16], in[24];

		for (int i = 0; i < 16; i++) {
			kek[i] = (uint8_t)i;
		}
		for (int i = 0; i < 24; i++) {
			sscanf("1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5" + 2 * i, "%2hhx", &in[i]);
		}
		printf("%-28s %s\n", "RFC 3394 integrity", wpa_aes_unwrap(kek, in, 2, out) == 0 ? "ok" : "WRONG");
		bad += check("RFC 3394 4.1 unwrap", out, "00112233445566778899aabbccddeeff");
	}
	/* IEEE 802.11-2016 J.4.2: passphrase "password", SSID "IEEE" */
	wpa_pbkdf2_sha1((const uint8_t *)"password", 8, (const uint8_t *)"IEEE", 4, 4096, out, 32);
	bad += check("PSK J.4.2 #1 (PBKDF2)", out, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e");
	wpa_pbkdf2_sha1((const uint8_t *)"ThisIsAPassword", 15, (const uint8_t *)"ThisIsASSID", 11, 4096, out, 32);
	bad += check("PSK J.4.2 #2 (PBKDF2)", out, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af");
	printf(bad ? "FAILED\n" : "all good\n");
	return bad;
}
