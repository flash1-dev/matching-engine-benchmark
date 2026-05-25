/* test_sha256.c — verifies the vendored SHA-256 against NIST FIPS 180-4 vectors. */
#include "../third_party/sha256.h"
#include <stdio.h>
#include <string.h>

static int check(const char* label, const char* msg, size_t len, const char* expect) {
    char got[65];
    sha256_hex(msg, len, got);
    int ok = strcmp(got, expect) == 0;
    printf("  %-8s %s  %s\n", label, ok ? "PASS" : "FAIL", got);
    if (!ok) printf("           expected %s\n", expect);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= check("abc",   "abc", 3,
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    ok &= check("empty", "", 0,
                "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const char* m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ok &= check("56byte", m, strlen(m),
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    printf("SHA-256: %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
