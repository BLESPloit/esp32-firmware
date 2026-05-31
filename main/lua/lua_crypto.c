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
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/md.h"

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


void lua_crypto_register_functions(lua_State *L)
{
   // Register cryptographic functions
    lua_register(L, "aes_ecb_encrypt", lua_aes_ecb_encrypt);
    lua_register(L, "aes_ecb_decrypt", lua_aes_ecb_decrypt);
    lua_register(L, "sha256", lua_sha256);
    lua_register(L, "sha256_first_16", lua_sha256_first_16);
    lua_register(L, "ecdh_generate_keypair", lua_ecdh_generate_keypair);
    lua_register(L, "ecdh_compute_shared", lua_ecdh_compute_shared);
    lua_register(L, "random_bytes", lua_random_bytes);

    ESP_LOGI(TAG, "Lua crypto functions registered");
}