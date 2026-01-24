// blufi_security.c BLUFI 安全实现
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_blufi_api.h"
#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "mbedtls/md5.h"
#include "esp_crc.h"
#include "esp_random.h"

// 引入私有头文件
#include "blufi_custom_priv.h"

/* 以下代码逻辑与官方例程完全一致，处理 DH 密钥协商和 AES 加解密 
*/

struct blufi_security {
#define DH_SELF_PUB_KEY_LEN     128
#define DH_PARAM_LEN_MAX        1024
    uint8_t  self_public_key[DH_SELF_PUB_KEY_LEN];
#define SHARE_KEY_LEN           128
    uint8_t  share_key[SHARE_KEY_LEN];
    size_t   share_len;
#define PSK_LEN                 16
    uint8_t  psk[PSK_LEN];
    uint8_t  *dh_param;
    int      dh_param_len;
    uint8_t  iv[16];
    mbedtls_dhm_context dhm;
    mbedtls_aes_context aes;
};
static struct blufi_security *blufi_sec;

static int myrand( void *rng_state, unsigned char *output, size_t len ) {
    esp_fill_random(output, len);
    return( 0 );
}

void blufi_dh_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data, int *output_len, bool *need_free) {
    int ret;
    uint8_t type = data[0];

    if (blufi_sec == NULL) {
        BLUFI_ERROR("BLUFI Security is not initialized");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    switch (type) {
    case 0x00: // SEC_TYPE_DH_PARAM_LEN
        blufi_sec->dh_param_len = ((data[1]<<8)|data[2]);
        if (blufi_sec->dh_param) {
            free(blufi_sec->dh_param);
            blufi_sec->dh_param = NULL;
        }
        blufi_sec->dh_param = (uint8_t *)malloc(blufi_sec->dh_param_len);
        if (blufi_sec->dh_param == NULL) {
            btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            return;
        }
        break;
    case 0x01: // SEC_TYPE_DH_PARAM_DATA
        if (blufi_sec->dh_param == NULL) {
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        uint8_t *param = blufi_sec->dh_param;
        memcpy(blufi_sec->dh_param, &data[1], blufi_sec->dh_param_len);
        ret = mbedtls_dhm_read_params(&blufi_sec->dhm, &param, &param[blufi_sec->dh_param_len]);
        if (ret) {
            btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
            return;
        }
        free(blufi_sec->dh_param);
        blufi_sec->dh_param = NULL;
        
        const int dhm_len = mbedtls_dhm_get_len(&blufi_sec->dhm);
        ret = mbedtls_dhm_make_public(&blufi_sec->dhm, dhm_len, blufi_sec->self_public_key, DH_SELF_PUB_KEY_LEN, myrand, NULL);
        if (ret) {
            btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
            return;
        }
        ret = mbedtls_dhm_calc_secret( &blufi_sec->dhm, blufi_sec->share_key, SHARE_KEY_LEN, &blufi_sec->share_len, myrand, NULL);
        if (ret) {
            btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
            return;
        }
        ret = mbedtls_md5(blufi_sec->share_key, blufi_sec->share_len, blufi_sec->psk);
        if (ret) {
            btc_blufi_report_error(ESP_BLUFI_CALC_MD5_ERROR);
            return;
        }
        mbedtls_aes_setkey_enc(&blufi_sec->aes, blufi_sec->psk, PSK_LEN * 8);
        *output_data = &blufi_sec->self_public_key[0];
        *output_len = dhm_len;
        *need_free = false;
        break;
    }
}

int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len) {
    int ret;
    size_t iv_offset = 0;
    uint8_t iv0[16];
    if (!blufi_sec) return -1;
    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));
    iv0[0] = iv8;
    ret = mbedtls_aes_crypt_cfb128(&blufi_sec->aes, MBEDTLS_AES_ENCRYPT, crypt_len, &iv_offset, iv0, crypt_data, crypt_data);
    return ret ? -1 : crypt_len;
}

int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len) {
    int ret;
    size_t iv_offset = 0;
    uint8_t iv0[16];
    if (!blufi_sec) return -1;
    memcpy(iv0, blufi_sec->iv, sizeof(blufi_sec->iv));
    iv0[0] = iv8;
    ret = mbedtls_aes_crypt_cfb128(&blufi_sec->aes, MBEDTLS_AES_DECRYPT, crypt_len, &iv_offset, iv0, crypt_data, crypt_data);
    return ret ? -1 : crypt_len;
}

uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len) {
    return esp_crc16_be(0, data, len);
}

esp_err_t blufi_security_init(void) {
    blufi_sec = (struct blufi_security *)malloc(sizeof(struct blufi_security));
    if (blufi_sec == NULL) return ESP_FAIL;
    memset(blufi_sec, 0x0, sizeof(struct blufi_security));
    mbedtls_dhm_init(&blufi_sec->dhm);
    mbedtls_aes_init(&blufi_sec->aes);
    memset(blufi_sec->iv, 0x0, sizeof(blufi_sec->iv));
    return 0;
}

void blufi_security_deinit(void) {
    if (blufi_sec == NULL) return;
    if (blufi_sec->dh_param) {
        free(blufi_sec->dh_param);
        blufi_sec->dh_param = NULL;
    }
    mbedtls_dhm_free(&blufi_sec->dhm);
    mbedtls_aes_free(&blufi_sec->aes);
    free(blufi_sec);
    blufi_sec = NULL;
}