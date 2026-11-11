#define DEFINE_AWS_LC_RS 1
#include "rustls.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const struct rustls_str version = rustls_version();
    const char expected[] = "rustls-ffi/0.15.7/rustls/0.23.46/aws-lc-rs";
    assert(version.len == sizeof(expected) - 1);
    assert(memcmp(version.data, expected, version.len) == 0);
    const struct rustls_crypto_provider *provider = rustls_aws_lc_rs_crypto_provider();
    assert(provider != NULL);
    assert(!rustls_crypto_provider_fips(provider));
    assert(rustls_crypto_provider_ciphersuites_len(provider) > 0);
    unsigned char random_bytes[32];
    assert(rustls_crypto_provider_random(provider, random_bytes, sizeof(random_bytes)) == RUSTLS_RESULT_OK);
    rustls_crypto_provider_free(provider);
    fwrite(version.data, 1, version.len, stdout);
    fputc('\n', stdout);
    return 0;
}
