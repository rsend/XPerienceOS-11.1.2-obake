/*
 * Compatibility aliases for the pre-BoringSSL OpenSSL ABI used by
 * Motorola's proprietary IMS data-plane library.
 */

#include <openssl/evp.h>

int EVP_EncryptFinal(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len)
{
    return EVP_EncryptFinal_ex(ctx, out, out_len);
}

int EVP_DecryptFinal(EVP_CIPHER_CTX *ctx, unsigned char *out, int *out_len)
{
    return EVP_DecryptFinal_ex(ctx, out, out_len);
}
