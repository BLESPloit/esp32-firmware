#pragma once

esp_err_t crypto_init(void);
void crypto_deinit(void);
int lua_aes_ecb_encrypt(lua_State* L);
int lua_aes_ecb_decrypt(lua_State* L);
int lua_aes_cbc_encrypt(lua_State* L);
int lua_aes_cbc_decrypt(lua_State* L);
int lua_sha256(lua_State* L);
int lua_sha256_first_16(lua_State* L);
int lua_ecdh_generate_keypair(lua_State* L);
int lua_ecdh_compute_shared(lua_State* L);
int lua_x25519_generate_keypair(lua_State* L);
int lua_x25519_compute_shared(lua_State* L);
int lua_random_bytes(lua_State* L);
int lua_rsa_pkcs1_encrypt(lua_State* L);
int lua_rsa_pkcs1_decrypt(lua_State* L);
int lua_rsa_sha256_sign(lua_State* L);
int lua_rsa_sha256_verify(lua_State* L);
int lua_hmac_sha256(lua_State* L);
int lua_aes_cmac(lua_State* L);
int lua_xor_bytes(lua_State* L);

void lua_crypto_register_functions(lua_State *L);