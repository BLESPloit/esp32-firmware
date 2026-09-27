#include <stdio.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

// mbedTLS cryptographic libraries
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/md.h"
#include "mbedtls/rsa.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include <stdlib.h>

#define TAG "LUA crypto"

static bool crypto_initialized = false;
// Entropy context for ECDH (shared globally)
static mbedtls_entropy_context entropy;
static mbedtls_ctr_drbg_context ctr_drbg;


// ── Initialization ──────────────────────────────────────────────────────────── 

esp_err_t crypto_init(void) 
{
    if (crypto_initialized) {
        return ESP_OK;
    }
    
    const char *pers = "fastpair_ecdh";
    
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                     (const unsigned char *)pers, strlen(pers));
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed failed: -0x%04x", -ret);
        return ESP_FAIL;
    }
    
    crypto_initialized = true;
    ESP_LOGI(TAG, "Cryptographic subsystem initialized");
    return ESP_OK;
}

void crypto_deinit(void) 
{
    if (crypto_initialized) {
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        crypto_initialized = false;
    }
}

// ── LUA crypto bindings ──────────────────────────────────────────────────────────── 

/**
 * AES-128-ECB Encryption
 * Lua: ciphertext = aes_encrypt(key, plaintext)
 * key: 16-byte binary string
 * plaintext: 16-byte binary string (must be exact block size)
 * Returns: 16-byte encrypted binary string
 */
int lua_aes_ecb_encrypt(lua_State* L) 
{
    size_t key_len, plain_len;
    const unsigned char *key = (const unsigned char *)luaL_checklstring(L, 1, &key_len);
    const unsigned char *plaintext = (const unsigned char *)luaL_checklstring(L, 2, &plain_len);
    
    if (key_len != 16) {
        return luaL_error(L, "AES key must be exactly 16 bytes, got %d", key_len);
    }
    
    if (plain_len != 16) {
        return luaL_error(L, "AES plaintext must be exactly 16 bytes, got %d", plain_len);
    }
    
    unsigned char output[16];
    mbedtls_aes_context aes;
    
    mbedtls_aes_init(&aes);
    
    int ret = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        return luaL_error(L, "AES setkey failed: -0x%04x", -ret);
    }
    
    ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, plaintext, output);
    mbedtls_aes_free(&aes);
    
    if (ret != 0) {
        return luaL_error(L, "AES encryption failed: -0x%04x", -ret);
    }
    
    lua_pushlstring(L, (const char *)output, 16);
    return 1;
}

/**
 * AES-128-ECB Decryption
 * Lua: plaintext = aes_decrypt(key, ciphertext)
 * key: 16-byte binary string
 * ciphertext: 16-byte binary string
 * Returns: 16-byte decrypted binary string
 */
int lua_aes_ecb_decrypt(lua_State* L) 
{
    size_t key_len, cipher_len;
    const unsigned char *key = (const unsigned char *)luaL_checklstring(L, 1, &key_len);
    const unsigned char *ciphertext = (const unsigned char *)luaL_checklstring(L, 2, &cipher_len);
    
    if (key_len != 16) {
        return luaL_error(L, "AES key must be exactly 16 bytes, got %d", key_len);
    }
    
    if (cipher_len != 16) {
        return luaL_error(L, "AES ciphertext must be exactly 16 bytes, got %d", cipher_len);
    }
    
    unsigned char output[16];
    mbedtls_aes_context aes;
    
    mbedtls_aes_init(&aes);
    
    int ret = mbedtls_aes_setkey_dec(&aes, key, 128);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        return luaL_error(L, "AES setkey failed: -0x%04x", -ret);
    }
    
    ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, ciphertext, output);
    mbedtls_aes_free(&aes);
    
    if (ret != 0) {
        return luaL_error(L, "AES decryption failed: -0x%04x", -ret);
    }
    
    lua_pushlstring(L, (const char *)output, 16);
    return 1;
}

/**
 * SHA-256 Hash
 * Lua: hash = sha256(data)
 * data: binary string of any length
 * Returns: 32-byte hash as binary string
 */
int lua_sha256(lua_State* L) 
{
    size_t data_len;
    const unsigned char *data = (const unsigned char *)luaL_checklstring(L, 1, &data_len);
    
    unsigned char output[32];
    
    // Use hardware-accelerated SHA-256 if available
    int ret = mbedtls_sha256(data, data_len, output, 0);  // 0 = SHA-256 (not SHA-224)
    
    if (ret != 0) {
        return luaL_error(L, "SHA-256 failed: -0x%04x", -ret);
    }
    
    lua_pushlstring(L, (const char *)output, 32);
    return 1;
}

/**
 * SHA-256 Hash (first 16 bytes only)
 * Lua: key = sha256_first_16(data)
 * Optimized for Fast Pair AES key derivation
 */
int lua_sha256_first_16(lua_State* L) 
{
    size_t data_len;
    const unsigned char *data = (const unsigned char *)luaL_checklstring(L, 1, &data_len);
    
    unsigned char output[32];
    
    int ret = mbedtls_sha256(data, data_len, output, 0);
    
    if (ret != 0) {
        return luaL_error(L, "SHA-256 failed: -0x%04x", -ret);
    }
    
    // Return only first 16 bytes for AES-128 key
    lua_pushlstring(L, (const char *)output, 16);
    return 1;
}

/**
 * Generate ECDH secp256r1 Keypair
 * Lua: private_key, public_key = ecdh_generate_keypair()
 * Returns: 
 *   - private_key: 32-byte binary string
 *   - public_key: 64-byte binary string (uncompressed X || Y, without 0x04 prefix)
 */
int lua_ecdh_generate_keypair(lua_State* L) 
{
    if (!crypto_initialized) {
        if (crypto_init() != ESP_OK) {
            return luaL_error(L, "Crypto initialization failed");
        }
    }
    
    // Use lower-level ECP functions directly (mbedTLS 3.x compatible)
    mbedtls_ecp_group grp;
    mbedtls_mpi d;           // Private key
    mbedtls_ecp_point Q;     // Public key
    
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Q);
    
    // Load secp256r1 (P-256) curve
    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        mbedtls_ecp_group_free(&grp);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_point_free(&Q);
        return luaL_error(L, "Failed to load secp256r1 curve: -0x%04x", -ret);
    }
    
    // Generate keypair
    ret = mbedtls_ecdh_gen_public(&grp, &d, &Q,
                                   mbedtls_ctr_drbg_random,
                                   &ctr_drbg);
    if (ret != 0) {
        mbedtls_ecp_group_free(&grp);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_point_free(&Q);
        return luaL_error(L, "ECDH keypair generation failed: -0x%04x", -ret);
    }
    
    // Export private key (32 bytes for P-256)
    unsigned char private_key[32];
    ret = mbedtls_mpi_write_binary(&d, private_key, 32);
    if (ret != 0) {
        mbedtls_ecp_group_free(&grp);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_point_free(&Q);
        return luaL_error(L, "Failed to export private key: -0x%04x", -ret);
    }
    
    // Export public key (65 bytes uncompressed: 0x04 || X || Y)
    unsigned char public_key_full[65];
    size_t olen;
    ret = mbedtls_ecp_point_write_binary(&grp, &Q,
                                          MBEDTLS_ECP_PF_UNCOMPRESSED,
                                          &olen,
                                          public_key_full,
                                          sizeof(public_key_full));
    if (ret != 0 || olen != 65) {
        mbedtls_ecp_group_free(&grp);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_point_free(&Q);
        return luaL_error(L, "Failed to export public key: -0x%04x (len=%zu)", -ret, olen);
    }
    
    // Cleanup
    mbedtls_ecp_group_free(&grp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_point_free(&Q);
    
    // Push private key (32 bytes)
    lua_pushlstring(L, (const char *)private_key, 32);
    
    // Push public key (64 bytes, skip 0x04 prefix for Fast Pair)
    lua_pushlstring(L, (const char *)(public_key_full + 1), 64);
    
    return 2;  // Return 2 values
}


/**
 * Compute ECDH Shared Secret
 * Lua: shared_secret = ecdh_compute_shared(private_key, peer_public_key)
 * private_key: 32-byte binary string (our private key)
 * peer_public_key: 64-byte binary string (peer's public key, no prefix)
 * Returns: 32-byte shared secret (X coordinate of shared point)
 */
int lua_ecdh_compute_shared(lua_State* L) 
{
    size_t priv_len, pub_len;
    const unsigned char *private_key = (const unsigned char *)luaL_checklstring(L, 1, &priv_len);
    const unsigned char *peer_public = (const unsigned char *)luaL_checklstring(L, 2, &pub_len);
    
    if (priv_len != 32) {
        return luaL_error(L, "Private key must be 32 bytes, got %d", priv_len);
    }
    
    if (pub_len != 64) {
        return luaL_error(L, "Peer public key must be 64 bytes, got %d", pub_len);
    }
    
    if (!crypto_initialized) {
        if (crypto_init() != ESP_OK) {
            return luaL_error(L, "Crypto initialization failed");
        }
    }
    
    // Use lower-level ECP functions for mbedTLS 3.x
    mbedtls_ecp_group grp;
    mbedtls_mpi d;           // Our private key
    mbedtls_ecp_point Qp;    // Peer's public key
    mbedtls_mpi z;           // Shared secret
    
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&Qp);
    mbedtls_mpi_init(&z);
    
    // Load secp256r1 curve
    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) {
        goto cleanup_error;
    }
    
    // Import our private key
    ret = mbedtls_mpi_read_binary(&d, private_key, 32);
    if (ret != 0) {
        goto cleanup_error;
    }
    
    // Import peer public key (prepend 0x04 for uncompressed format)
    unsigned char peer_public_full[65];
    peer_public_full[0] = 0x04;
    memcpy(peer_public_full + 1, peer_public, 64);
    
    ret = mbedtls_ecp_point_read_binary(&grp, &Qp, peer_public_full, 65);
    if (ret != 0) {
        goto cleanup_error;
    }
    
    // Compute shared secret: z = d * Qp
    ret = mbedtls_ecdh_compute_shared(&grp, &z, &Qp, &d,
                                       mbedtls_ctr_drbg_random,
                                       &ctr_drbg);
    if (ret != 0) {
        goto cleanup_error;
    }
    
    // Export shared secret (32 bytes for P-256)
    unsigned char shared_secret[32];
    ret = mbedtls_mpi_write_binary(&z, shared_secret, 32);
    if (ret != 0) {
        goto cleanup_error;
    }
    
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Qp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    
    lua_pushlstring(L, (const char *)shared_secret, 32);
    return 1;

cleanup_error:
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Qp);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    return luaL_error(L, "ECDH computation failed: -0x%04x", -ret);
}

/**
 * Generate cryptographically secure random bytes
 * Lua: random_data = random_bytes(length)
 * length: number of bytes to generate
 * Returns: binary string of random data
 */
int lua_random_bytes(lua_State* L) 
{
    int length = luaL_checkinteger(L, 1);
    
    if (length <= 0 || length > 1024) {
        return luaL_error(L, "Invalid length: %d (must be 1-1024)", length);
    }
    
    unsigned char *buffer = malloc(length);
    if (!buffer) {
        return luaL_error(L, "Memory allocation failed");
    }
    
    // Use ESP32 hardware RNG
    esp_fill_random(buffer, length);
    
    lua_pushlstring(L, (const char *)buffer, length);
    free(buffer);
    
    return 1;
}


static int lua_reject_rsa_modulus(lua_State *L, size_t n_len)
{
    if (n_len == 64 || n_len == 128 || n_len == 256) {
        return 0;
    }
    return luaL_error(L, "modulus must be 64, 128, or 256 bytes, got %d", (int) n_len);
}

static int lua_reject_rsa_public_exponent(lua_State *L, size_t e_len, size_t n_len)
{
    if (e_len >= 1 && e_len <= n_len) {
        return 0;
    }
    return luaL_error(L, "public_exponent length must be 1..%d, got %d", (int) n_len, (int) e_len);
}

// Import N and E, and D when non-NULL. On failure the context is freed and this raises.
// On success the caller must mbedtls_rsa_free(rsa).
static void lua_rsa_setup(lua_State *L, mbedtls_rsa_context *rsa,
                          const unsigned char *n, size_t n_len,
                          const unsigned char *e, size_t e_len,
                          const unsigned char *d, size_t d_len)
{
    if (!crypto_initialized) {
        if (crypto_init() != ESP_OK) {
            luaL_error(L, "Crypto initialization failed");
            return;
        }
    }
    mbedtls_rsa_init(rsa);
    int ret = mbedtls_rsa_import_raw(rsa, n, n_len, NULL, 0, NULL, 0,
                                     d, d ? d_len : 0, e, e_len);
    if (ret == 0) {
        ret = mbedtls_rsa_complete(rsa);
    }
    if (ret != 0) {
        mbedtls_rsa_free(rsa);
        luaL_error(L, "RSA key is invalid: -0x%04x", -ret);
        return;
    }
    if (mbedtls_rsa_get_len(rsa) != n_len) {
        mbedtls_rsa_free(rsa);
        luaL_error(L, "modulus must not have leading zero bytes");
    }
}

static int lua_aes_cbc_crypt(lua_State *L, int mode)
{
    size_t key_len, iv_len, data_len;
    const unsigned char *key = (const unsigned char *) luaL_checklstring(L, 1, &key_len);
    const unsigned char *iv = (const unsigned char *) luaL_checklstring(L, 2, &iv_len);
    const unsigned char *data = (const unsigned char *) luaL_checklstring(L, 3, &data_len);

    if (key_len != 16 && key_len != 32) {
        return luaL_error(L, "key must be 16 or 32 bytes, got %d", (int) key_len);
    }
    if (iv_len != 16) {
        return luaL_error(L, "iv must be exactly 16 bytes, got %d", (int) iv_len);
    }
    if (data_len == 0 || data_len > 4096 || (data_len % 16) != 0) {
        return luaL_error(L, "data length must be a non-zero multiple of 16 and at most 4096, got %d",
                          (int) data_len);
    }

    unsigned char iv_copy[16];
    memcpy(iv_copy, iv, 16);

    unsigned char *output = malloc(data_len);
    if (!output) {
        return luaL_error(L, "Memory allocation failed");
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int ret = (mode == MBEDTLS_AES_ENCRYPT)
              ? mbedtls_aes_setkey_enc(&aes, key, (unsigned) key_len * 8)
              : mbedtls_aes_setkey_dec(&aes, key, (unsigned) key_len * 8);
    if (ret != 0) {
        mbedtls_aes_free(&aes);
        free(output);
        return luaL_error(L, "AES setkey failed: -0x%04x", -ret);
    }

    ret = mbedtls_aes_crypt_cbc(&aes, mode, data_len, iv_copy, data, output);
    mbedtls_aes_free(&aes);
    if (ret != 0) {
        free(output);
        return luaL_error(L, "AES-CBC failed: -0x%04x", -ret);
    }

    lua_pushlstring(L, (const char *) output, data_len);
    free(output);
    return 1;
}

int lua_aes_cbc_encrypt(lua_State *L)
{
    return lua_aes_cbc_crypt(L, MBEDTLS_AES_ENCRYPT);
}

int lua_aes_cbc_decrypt(lua_State *L)
{
    return lua_aes_cbc_crypt(L, MBEDTLS_AES_DECRYPT);
}

int lua_rsa_pkcs1_encrypt(lua_State *L)
{
    size_t n_len, e_len, plain_len;
    const unsigned char *n = (const unsigned char *) luaL_checklstring(L, 1, &n_len);
    const unsigned char *e = (const unsigned char *) luaL_checklstring(L, 2, &e_len);
    const unsigned char *plain = (const unsigned char *) luaL_checklstring(L, 3, &plain_len);

    lua_reject_rsa_modulus(L, n_len);
    lua_reject_rsa_public_exponent(L, e_len, n_len);
    if (plain_len == 0 || plain_len > n_len - 11) {
        return luaL_error(L, "plaintext length must be 1..%d, got %d",
                          (int) (n_len - 11), (int) plain_len);
    }

    mbedtls_rsa_context rsa;
    lua_rsa_setup(L, &rsa, n, n_len, e, e_len, NULL, 0);

    unsigned char *output = malloc(n_len);
    if (!output) {
        mbedtls_rsa_free(&rsa);
        return luaL_error(L, "Memory allocation failed");
    }

    int ret = mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &ctr_drbg,
                                        plain_len, plain, output);
    mbedtls_rsa_free(&rsa);
    if (ret != 0) {
        free(output);
        return luaL_error(L, "RSA encrypt failed: -0x%04x", -ret);
    }

    lua_pushlstring(L, (const char *) output, n_len);
    free(output);
    return 1;
}

int lua_rsa_pkcs1_decrypt(lua_State *L)
{
    size_t n_len, e_len, d_len, cipher_len;
    const unsigned char *n = (const unsigned char *) luaL_checklstring(L, 1, &n_len);
    const unsigned char *e = (const unsigned char *) luaL_checklstring(L, 2, &e_len);
    const unsigned char *d = (const unsigned char *) luaL_checklstring(L, 3, &d_len);
    const unsigned char *cipher = (const unsigned char *) luaL_checklstring(L, 4, &cipher_len);

    lua_reject_rsa_modulus(L, n_len);
    lua_reject_rsa_public_exponent(L, e_len, n_len);
    if (d_len != n_len) {
        return luaL_error(L, "private_exponent must be exactly %d bytes, got %d",
                          (int) n_len, (int) d_len);
    }
    if (cipher_len != n_len) {
        return luaL_error(L, "ciphertext must be exactly %d bytes, got %d",
                          (int) n_len, (int) cipher_len);
    }

    mbedtls_rsa_context rsa;
    lua_rsa_setup(L, &rsa, n, n_len, e, e_len, d, d_len);

    unsigned char *output = malloc(n_len);
    if (!output) {
        mbedtls_rsa_free(&rsa);
        return luaL_error(L, "Memory allocation failed");
    }

    size_t olen = 0;
    int ret = mbedtls_rsa_pkcs1_decrypt(&rsa, mbedtls_ctr_drbg_random, &ctr_drbg,
                                        &olen, cipher, output, n_len);
    mbedtls_rsa_free(&rsa);
    if (ret != 0) {
        free(output);
        return luaL_error(L, "PKCS#1 padding is invalid");
    }

    lua_pushlstring(L, (const char *) output, olen);
    free(output);
    return 1;
}

int lua_rsa_sha256_sign(lua_State *L)
{
    size_t n_len, e_len, d_len, msg_len;
    const unsigned char *n = (const unsigned char *) luaL_checklstring(L, 1, &n_len);
    const unsigned char *e = (const unsigned char *) luaL_checklstring(L, 2, &e_len);
    const unsigned char *d = (const unsigned char *) luaL_checklstring(L, 3, &d_len);
    const unsigned char *msg = (const unsigned char *) luaL_checklstring(L, 4, &msg_len);

    lua_reject_rsa_modulus(L, n_len);
    lua_reject_rsa_public_exponent(L, e_len, n_len);
    if (d_len != n_len) {
        return luaL_error(L, "private_exponent must be exactly %d bytes, got %d",
                          (int) n_len, (int) d_len);
    }
    if (msg_len == 0 || msg_len > 4096) {
        return luaL_error(L, "message length must be 1..4096, got %d", (int) msg_len);
    }

    unsigned char hash[32];
    int ret = mbedtls_sha256(msg, msg_len, hash, 0);
    if (ret != 0) {
        return luaL_error(L, "SHA-256 failed: -0x%04x", -ret);
    }

    mbedtls_rsa_context rsa;
    lua_rsa_setup(L, &rsa, n, n_len, e, e_len, d, d_len);

    unsigned char *sig = malloc(n_len);
    if (!sig) {
        mbedtls_rsa_free(&rsa);
        return luaL_error(L, "Memory allocation failed");
    }

    ret = mbedtls_rsa_pkcs1_sign(&rsa, mbedtls_ctr_drbg_random, &ctr_drbg,
                                 MBEDTLS_MD_SHA256, 32, hash, sig);
    mbedtls_rsa_free(&rsa);
    if (ret != 0) {
        free(sig);
        return luaL_error(L, "RSA sign failed: -0x%04x", -ret);
    }

    lua_pushlstring(L, (const char *) sig, n_len);
    free(sig);
    return 1;
}

int lua_rsa_sha256_verify(lua_State *L)
{
    size_t n_len, e_len, msg_len, sig_len;
    const unsigned char *n = (const unsigned char *) luaL_checklstring(L, 1, &n_len);
    const unsigned char *e = (const unsigned char *) luaL_checklstring(L, 2, &e_len);
    const unsigned char *msg = (const unsigned char *) luaL_checklstring(L, 3, &msg_len);
    const unsigned char *sig = (const unsigned char *) luaL_checklstring(L, 4, &sig_len);

    lua_reject_rsa_modulus(L, n_len);
    lua_reject_rsa_public_exponent(L, e_len, n_len);
    if (msg_len == 0 || msg_len > 4096) {
        return luaL_error(L, "message length must be 1..4096, got %d", (int) msg_len);
    }
    if (sig_len != n_len) {
        return luaL_error(L, "signature must be exactly %d bytes, got %d",
                          (int) n_len, (int) sig_len);
    }

    unsigned char hash[32];
    int ret = mbedtls_sha256(msg, msg_len, hash, 0);
    if (ret != 0) {
        return luaL_error(L, "SHA-256 failed: -0x%04x", -ret);
    }

    mbedtls_rsa_context rsa;
    lua_rsa_setup(L, &rsa, n, n_len, e, e_len, NULL, 0);
    ret = mbedtls_rsa_pkcs1_verify(&rsa, MBEDTLS_MD_SHA256, 32, hash, sig);
    mbedtls_rsa_free(&rsa);

    if (ret == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (ret == MBEDTLS_ERR_RSA_VERIFY_FAILED || ret == MBEDTLS_ERR_RSA_INVALID_PADDING) {
        lua_pushboolean(L, 0);
        return 1;
    }
    return luaL_error(L, "RSA verify failed: -0x%04x", -ret);
}

int lua_hmac_sha256(lua_State *L)
{
    size_t key_len, data_len;
    const unsigned char *key = (const unsigned char *) luaL_checklstring(L, 1, &key_len);
    const unsigned char *data = (const unsigned char *) luaL_checklstring(L, 2, &data_len);

    if (key_len == 0 || key_len > 1024) {
        return luaL_error(L, "key length must be 1..1024, got %d", (int) key_len);
    }

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return luaL_error(L, "SHA-256 is not available");
    }

    unsigned char output[32];
    int ret = mbedtls_md_hmac(info, key, key_len, data, data_len, output);
    if (ret != 0) {
        return luaL_error(L, "HMAC-SHA256 failed: -0x%04x", -ret);
    }

    lua_pushlstring(L, (const char *) output, 32);
    return 1;
}

int lua_aes_cmac(lua_State *L)
{
    size_t key_len, data_len;
    const unsigned char *key = (const unsigned char *) luaL_checklstring(L, 1, &key_len);
    const unsigned char *data = (const unsigned char *) luaL_checklstring(L, 2, &data_len);

    if (key_len != 16) {
        return luaL_error(L, "key must be exactly 16 bytes, got %d", (int) key_len);
    }

    const mbedtls_cipher_info_t *info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (!info) {
        return luaL_error(L, "AES-128-ECB is not available");
    }

    unsigned char output[16];
    int ret = mbedtls_cipher_cmac(info, key, 128, data, data_len, output);
    if (ret != 0) {
        return luaL_error(L, "AES-CMAC failed: -0x%04x", -ret);
    }

    lua_pushlstring(L, (const char *) output, 16);
    return 1;
}

int lua_xor_bytes(lua_State *L)
{
    size_t a_len, b_len;
    const unsigned char *a = (const unsigned char *) luaL_checklstring(L, 1, &a_len);
    const unsigned char *b = (const unsigned char *) luaL_checklstring(L, 2, &b_len);

    if (a_len != b_len || a_len == 0 || a_len > 4096) {
        return luaL_error(L, "xor_bytes arguments must be equal length, 1..4096, got %d and %d",
                          (int) a_len, (int) b_len);
    }

    unsigned char *output = malloc(a_len);
    if (!output) {
        return luaL_error(L, "Memory allocation failed");
    }
    for (size_t i = 0; i < a_len; i++) {
        output[i] = (unsigned char) (a[i] ^ b[i]);
    }
    lua_pushlstring(L, (const char *) output, a_len);
    free(output);
    return 1;
}


static int x25519_clamp(mbedtls_mpi *d)
{
    int ret = mbedtls_mpi_set_bit(d, 0, 0);
    if (ret == 0) {
        ret = mbedtls_mpi_set_bit(d, 1, 0);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_set_bit(d, 2, 0);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_set_bit(d, 255, 0);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_set_bit(d, 254, 1);
    }
    return ret;
}

// Lua: private_key, public_key = x25519_generate_keypair()
// Both values are 32-byte little-endian strings. The scalar is clamped.
int lua_x25519_generate_keypair(lua_State *L)
{
    if (!crypto_initialized) {
        if (crypto_init() != ESP_OK) {
            return luaL_error(L, "Crypto initialization failed");
        }
    }

    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);

    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret == 0) {
        ret = mbedtls_ecdh_gen_public(&grp, &d, &q, mbedtls_ctr_drbg_random, &ctr_drbg);
    }

    unsigned char priv[32];
    unsigned char pub[32];
    size_t olen = 0;
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary_le(&d, priv, sizeof(priv));
    }
    if (ret == 0) {
        ret = mbedtls_ecp_point_write_binary(&grp, &q, MBEDTLS_ECP_PF_COMPRESSED,
                                             &olen, pub, sizeof(pub));
        if (ret == 0 && olen != sizeof(pub)) {
            ret = MBEDTLS_ERR_ECP_BAD_INPUT_DATA;
        }
    }

    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    if (ret != 0) {
        return luaL_error(L, "X25519 keypair generation failed: -0x%04x", -ret);
    }

    lua_pushlstring(L, (const char *) priv, sizeof(priv));
    lua_pushlstring(L, (const char *) pub, sizeof(pub));
    return 2;
}

// Lua: shared = x25519_compute_shared(private_key, peer_public_key)
// Both inputs and the result are 32-byte little-endian strings.
int lua_x25519_compute_shared(lua_State *L)
{
    size_t priv_len = 0;
    size_t pub_len = 0;
    const unsigned char *priv = (const unsigned char *) luaL_checklstring(L, 1, &priv_len);
    const unsigned char *peer = (const unsigned char *) luaL_checklstring(L, 2, &pub_len);
    if (priv_len != 32) {
        return luaL_error(L, "private_key must be exactly 32 bytes, got %d", (int) priv_len);
    }
    if (pub_len != 32) {
        return luaL_error(L, "peer_public_key must be exactly 32 bytes, got %d", (int) pub_len);
    }
    if (!crypto_initialized) {
        if (crypto_init() != ESP_OK) {
            return luaL_error(L, "Crypto initialization failed");
        }
    }

    mbedtls_ecp_group grp;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    mbedtls_mpi z;
    mbedtls_ecp_group_init(&grp);
    mbedtls_mpi_init(&d);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&z);

    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
    if (ret == 0) {
        ret = mbedtls_mpi_read_binary_le(&d, priv, priv_len);
    }
    if (ret == 0) {
        ret = x25519_clamp(&d);
    }
    if (ret == 0) {
        ret = mbedtls_ecp_point_read_binary(&grp, &q, peer, pub_len);
    }
    if (ret == 0) {
        ret = mbedtls_ecdh_compute_shared(&grp, &z, &q, &d,
                                          mbedtls_ctr_drbg_random, &ctr_drbg);
    }

    unsigned char shared[32];
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary_le(&z, shared, sizeof(shared));
    }

    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&grp);
    if (ret != 0) {
        return luaL_error(L, "X25519 computation failed: -0x%04x", -ret);
    }

    lua_pushlstring(L, (const char *) shared, sizeof(shared));
    return 1;
}


void lua_crypto_register_functions(lua_State *L)
{
   // Register cryptographic functions
    lua_register(L, "aes_ecb_encrypt", lua_aes_ecb_encrypt);
    lua_register(L, "aes_ecb_decrypt", lua_aes_ecb_decrypt);
    lua_register(L, "aes_cbc_encrypt", lua_aes_cbc_encrypt);
    lua_register(L, "aes_cbc_decrypt", lua_aes_cbc_decrypt);
    lua_register(L, "sha256", lua_sha256);
    lua_register(L, "sha256_first_16", lua_sha256_first_16);
    lua_register(L, "ecdh_generate_keypair", lua_ecdh_generate_keypair);
    lua_register(L, "ecdh_compute_shared", lua_ecdh_compute_shared);
    lua_register(L, "x25519_generate_keypair", lua_x25519_generate_keypair);
    lua_register(L, "x25519_compute_shared", lua_x25519_compute_shared);
    lua_register(L, "random_bytes", lua_random_bytes);
    lua_register(L, "rsa_pkcs1_encrypt", lua_rsa_pkcs1_encrypt);
    lua_register(L, "rsa_pkcs1_decrypt", lua_rsa_pkcs1_decrypt);
    lua_register(L, "rsa_sha256_sign", lua_rsa_sha256_sign);
    lua_register(L, "rsa_sha256_verify", lua_rsa_sha256_verify);
    lua_register(L, "hmac_sha256", lua_hmac_sha256);
    lua_register(L, "aes_cmac", lua_aes_cmac);
    lua_register(L, "xor_bytes", lua_xor_bytes);

    ESP_LOGI(TAG, "Lua crypto functions registered");
}