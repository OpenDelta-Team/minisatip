/*
Minisatip does not assume any responsability related to functionality of this
code. It also does not include any certificates, please procure them from
alternative source

 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <openssl/aes.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "adapter.h"
#include "ca.h"
#include "dvb.h"
#include "dvbapi.h"
#include "minisatip.h"
#include "pmt.h"
#include "poll.h"
#include "search.h"
#include "socketworks.h"
#include <linux/dvb/ca.h>

#include "utils.h"

#define MAX_ELEMENTS 33
#define MAX_PAIRS 10
#define DEFAULT_LOG LOG_DVBCA

#define EN50221_APP_RM_RESOURCEID MKRID(1, 1, 1)
#define EN50221_APP_AI_RESOURCEID MKRID(2, 1, 1)
#define EN50221_APP_CA_RESOURCEID MKRID(3, 1, 1)
#define EN50221_APP_DATETIME_RESOURCEID MKRID(36, 1, 1)
#define EN50221_APP_MMI_RESOURCEID MKRID(64, 1, 1)
#define EN50221_APP_DVB_RESOURCEID MKRID(32, 1, 1)
#define TS101699_APP_RM_RESOURCEID MKRID(1, 1, 2)
#define TS101699_APP_AI_RESOURCEID MKRID(2, 1, 2)
#define CIPLUS_APP_AI_RESOURCEID MKRID(2, 1, 3)
#define CIPLUS_APP_AI_RESOURCEID_MULTI MKRID(2, 1, 4)
#define BLUEBOOK_A173_2_APP_AI_RESOURCE_ID MKRID(2, 1, 5)
#define CIPLUS_APP_DVB_RESOURCEID MKRID(32, 1, 2)
#define TS103205_APP_DVB_RESOURCEID MKRID(32, 1, 3)
#define TS103205_APP_DVB_MULTISTREAM_RESOURCEID MKRID(32, 2, 1)
#define TS103205_APP_CA_MULTISTREAM_RESOURCEID MKRID(3, 2, 1)
#define TS103205_APP_MMI_RESOURCEID MKRID(64, 2, 1)
#define CIPLUS_APP_CC_RESOURCEID MKRID(140, 64, 1)
#define CIPLUS_APP_CC_RESOURCEID_TWO MKRID(140, 64, 2)
#define TS103205_APP_CC_RESOURCEID_THREE MKRID(140, 64, 4)
#define TS103205_APP_CC_MULTISTREAM_RESOURCEID MKRID(140, 65, 1)
#define CIPLUS_APP_LANG_RESOURCEID MKRID(141, 64, 1)
#define CIPLUS_APP_UPGR_RESOURCEID MKRID(142, 64, 1)
#define CIPLUS_APP_OPRF_RESOURCEID MKRID(143, 64, 1)
#define TS103205_APP_OPRF_TWO_RESOURCEID MKRID(143, 64, 2)
#define TS103205_APP_OPRF_THREE_RESOURCEID MKRID(143, 64, 3)
#define TS103205_APP_MULTISTREAM_RESOURCEID MKRID(144, 1, 1)
#define CIPLUS_APP_SAS_RESOURCEID MKRID(150, 64, 1)
#define TS101699_APP_AMMI_RESOURCEID MKRID(65, 1, 1)
#define CIPLUS_APP_AMMI_RESOURCEID MKRID(65, 1, 2)
#define TS103205_APP_AMMI_RESOURCEID MKRID(65, 2, 1)

#if OPENSSL_VERSION_NUMBER < 0x10100000L
int DH_set0_pqg(DH *dh, BIGNUM *p, BIGNUM *q, BIGNUM *g) {
    /* If the fields p and g in d are NULL, the corresponding input
     * parameters MUST be non-NULL.  q may remain NULL.
     */
    if ((dh->p == NULL && p == NULL) || (dh->g == NULL && g == NULL))
        return 0;

    if (p != NULL) {
        BN_free(dh->p);
        dh->p = p;
    }
    if (q != NULL) {
        BN_free(dh->q);
        dh->q = q;
    }
    if (g != NULL) {
        BN_free(dh->g);
        dh->g = g;
    }

    if (q != NULL) {
        dh->length = BN_num_bits(q);
    }

    return 1;
}

void DH_get0_key(const DH *dh, const BIGNUM **pub_key,
                 const BIGNUM **priv_key) {
    if (pub_key != NULL)
        *pub_key = dh->pub_key;
    if (priv_key != NULL)
        *priv_key = dh->priv_key;
}

void DH_set_flags(DH *dh, int flags) { dh->flags |= flags; }
#endif

char ci_name_underscore[128];
int ci_number;
int logging = 0;
char logfile[256];
int extract_ci_cert = 0;

extern char *listmgmt_str[];
typedef struct ca_device ca_device_t;

uint32_t datatype_sizes[MAX_ELEMENTS] = {
    0,   50,  0,  0, 0, 8,  8,  0, 0, 0, 0,  0, 32, 256, 256, 0, 0,
    256, 256, 32, 8, 8, 32, 32, 0, 8, 2, 32, 1, 32, 1,   0,   32};

struct element {
    uint8_t *data;
    uint32_t size;
    /* buffer valid */
    int valid;
};

struct ci_buffer {
    size_t size;
    unsigned char data[];
};

struct cc_ctrl_data {
    /* ci+ credentials */
    struct element elements[MAX_ELEMENTS];

    /* DHSK */
    uint8_t dhsk[256];

    /* KS_host */
    uint8_t ks_host[32];

    /* derived keys */
    uint8_t sek[16];
    uint8_t sak[16];

    /* AKH checks - module performs 5 tries to get correct AKH */
    unsigned int akh_index;

    /* authentication data */
    uint8_t dh_exp[256];

    /* certificates */
    struct cert_ctx *cert_ctx;

    /* private key of device-cert */
    RSA *rsa_device_key;
};

struct ca_device {
    int enabled;
    SCAPMT capmt[MAX_CA_PMT];
    int max_ca_pmt, multiple_pmt;
    int fd, sock;
    int slot_id;
    char is_active;
    int tc;
    int id;
    int ignore_close;
    int init_ok;
    uint16_t caid[MAX_CAID];
    uint32_t caids;

    struct cc_ctrl_data private_data;

    // Session Numbers: as all the objects are stored inside of the ca_device,
    // don't store data inside of the sessions
    int session_number;

    int uri_mask;

    struct list_head *txq;
    struct list_head *mmiq;

    /*
     * CAM module info
     */
    char ci_name[128];

    char cam_menu_string[64];
    char pin_str[10];
    char force_ci;
    uint8_t key[2][16], iv[2][16];
    int sp, is_ciplus, parity;

    /*
     * CAM date time handling
     */
    uint8_t datetime_response_interval;
    time_t datetime_next_send;
};

int dvbca_id;
ca_device_t *ca_devices[MAX_ADAPTERS];

struct cert_ctx {
    X509_STORE *store;

    /* Host */
    X509 *cust_cert;
    X509 *device_cert;

    /* Module */
    X509 *ci_cust_cert;
    X509 *ci_device_cert;
};

struct aes_xcbc_mac_ctx {
    uint8_t K[3][16];
    uint8_t IV[16];
    AES_KEY key;
    int buflen;
};

struct struct_application_handler {
    int resource;
    char *name;
    int (*callback)(struct ca_device *d, int resource, uint8_t *buffer,
                    int len);
    int (*create)(struct ca_device *d, int session_id, int resource);
};

typedef enum {
    CIPLUS_DATA_RATE_72_MBPS = 0,
    CIPLUS_DATA_RATE_96_MBPS = 1,
} ciplus13_data_rate_t;

unsigned char dh_p[256] =
    {/* prime */
     0xd6, 0x27, 0x14, 0x7a, 0x7c, 0x0c, 0x26, 0x63, 0x9d, 0x82, 0xeb, 0x1f,
     0x4a, 0x18, 0xff, 0x6c, 0x34, 0xad, 0xea, 0xa6, 0xc0, 0x23, 0xe6, 0x65,
     0xfc, 0x8e, 0x32, 0xc3, 0x33, 0xf4, 0x91, 0xa7, 0xcc, 0x88, 0x58, 0xd7,
     0xf3, 0xb3, 0x17, 0x5e, 0xb0, 0xa8, 0xeb, 0x5c, 0xd4, 0xd8, 0x3a, 0xae,
     0x8e, 0x75, 0xa1, 0x50, 0x5f, 0x5d, 0x67, 0xc5, 0x40, 0xf4, 0xb3, 0x68,
     0x35, 0xd1, 0x3a, 0x4c, 0x93, 0x7f, 0xca, 0xce, 0xdd, 0x83, 0x29, 0x01,
     0xc8, 0x4b, 0x76, 0x81, 0x56, 0x34, 0x83, 0x31, 0x92, 0x72, 0x65, 0x7b,
     0xac, 0xd9, 0xda, 0xa9, 0xd1, 0xd3, 0xe5, 0x77, 0x58, 0x6f, 0x5b, 0x44,
     0x3e, 0xaf, 0x7f, 0x6d, 0xf5, 0xcf, 0x0a, 0x80, 0x0d, 0xa5, 0x56, 0x4f,
     0x4b, 0x85, 0x41, 0x0f, 0x13, 0x41, 0x06, 0x1f, 0xf3, 0xd9, 0x65, 0x36,
     0xae, 0x47, 0x41, 0x1f, 0x1f, 0xe0, 0xde, 0x69, 0xe5, 0x86, 0x2a, 0xa1,
     0xf2, 0x48, 0x02, 0x92, 0x68, 0xa6, 0x37, 0x9f, 0x76, 0x4f, 0x7d, 0x94,
     0x5d, 0x10, 0xe5, 0xab, 0x5d, 0xb2, 0xf3, 0x12, 0x8c, 0x79, 0x03, 0x92,
     0xa6, 0x7f, 0x8a, 0x78, 0xb0, 0xba, 0xc5, 0xb5, 0x31, 0xc5, 0xc8, 0x22,
     0x6e, 0x29, 0x02, 0x40, 0xab, 0xe7, 0x5c, 0x23, 0x33, 0x7f, 0xcb, 0x86,
     0xc7, 0xb4, 0xfd, 0xaa, 0x44, 0xcd, 0x9c, 0x9f, 0xba, 0xac, 0x3a, 0xcf,
     0x7e, 0x31, 0x5f, 0xa8, 0x47, 0xce, 0xca, 0x1c, 0xb4, 0x77, 0xa0, 0xec,
     0x9a, 0x46, 0xd4, 0x79, 0x7b, 0x64, 0xbb, 0x6c, 0x91, 0xb2, 0x38, 0x01,
     0x65, 0x11, 0x45, 0x9f, 0x62, 0x08, 0x6f, 0x31, 0xcf, 0xc4, 0xba, 0xdc,
     0xd0, 0x03, 0x91, 0xf1, 0x18, 0x1f, 0xcb, 0x4d, 0xfc, 0x73, 0x5a, 0xa2,
     0x15, 0xb8, 0x3c, 0x8d, 0x80, 0x92, 0x1c, 0xa1, 0x03, 0xd0, 0x83, 0x2f,
     0x5f, 0xe3, 0x07, 0x69};

unsigned char dh_g[256] =
    {/* generator */
     0x95, 0x7d, 0xd1, 0x49, 0x68, 0xc1, 0xa5, 0xf1, 0x48, 0xe6, 0x50, 0x4f,
     0xa1, 0x10, 0x72, 0xc4, 0xef, 0x12, 0xec, 0x2d, 0x94, 0xbe, 0xc7, 0x20,
     0x2c, 0x94, 0xf9, 0x68, 0x67, 0x0e, 0x22, 0x17, 0xb5, 0x5c, 0x0b, 0xca,
     0xac, 0x9f, 0x25, 0x9c, 0xd2, 0xa6, 0x1a, 0x20, 0x10, 0x16, 0x6a, 0x42,
     0x27, 0x83, 0x47, 0x42, 0xa0, 0x07, 0x52, 0x09, 0x33, 0x97, 0x4e, 0x30,
     0x57, 0xd8, 0xb7, 0x1e, 0x46, 0xa6, 0xba, 0x4e, 0x40, 0x6a, 0xe9, 0x1a,
     0x5a, 0xa0, 0x74, 0x56, 0x92, 0x55, 0xc2, 0xbd, 0x44, 0xcd, 0xb3, 0x33,
     0xf7, 0x35, 0x46, 0x25, 0xdf, 0x84, 0x19, 0xf3, 0xe2, 0x7a, 0xac, 0x4e,
     0xee, 0x1a, 0x86, 0x3b, 0xb3, 0x87, 0xa6, 0x66, 0xc1, 0x70, 0x21, 0x41,
     0xd3, 0x58, 0x36, 0xb5, 0x3b, 0x6e, 0xa1, 0x55, 0x60, 0x9a, 0x59, 0xd3,
     0x85, 0xd8, 0xdc, 0x6a, 0xff, 0x41, 0xb6, 0xbf, 0x42, 0xde, 0x64, 0x00,
     0xd0, 0xee, 0x3a, 0xa1, 0x8a, 0xed, 0x12, 0xf9, 0xba, 0x54, 0x5c, 0xdb,
     0x06, 0x24, 0x49, 0xe8, 0x47, 0xcf, 0x5b, 0xe4, 0xbb, 0xc0, 0xaa, 0x8a,
     0x8c, 0xbe, 0x73, 0xd9, 0x02, 0xea, 0xee, 0x8d, 0x87, 0x5b, 0xbf, 0x78,
     0x04, 0x41, 0x9e, 0xa8, 0x5c, 0x3c, 0x49, 0xde, 0x88, 0x6d, 0x62, 0x21,
     0x7f, 0xf0, 0x5e, 0x2d, 0x1d, 0xfc, 0x47, 0x0d, 0x1b, 0xaa, 0x4e, 0x0d,
     0x78, 0x20, 0xfe, 0x57, 0x0f, 0xca, 0xdf, 0xeb, 0x3c, 0x84, 0xa7, 0xe1,
     0x61, 0xb2, 0x95, 0x98, 0x07, 0x73, 0x8e, 0x51, 0xc6, 0x87, 0xe4, 0xcf,
     0xf1, 0x5f, 0x86, 0x99, 0xec, 0x8d, 0x44, 0x92, 0x2c, 0x99, 0xf6, 0xc0,
     0xf4, 0x39, 0xe8, 0x05, 0xbf, 0xc1, 0x56, 0xde, 0xfe, 0x93, 0x75, 0x06,
     0x69, 0x87, 0x83, 0x06, 0x51, 0x80, 0xa5, 0x6e, 0xa6, 0x19, 0x7d, 0x3b,
     0xef, 0xfb, 0xe0, 0x4a};

int dvbca_close_device(ca_device_t *c);
static int ca_send_datetime(ca_device_t *d);
int populate_resources(ca_device_t *d, int *resource_ids);
int ca_write_apdu_session(ca_device_t *d, int session_number, int resource,
                          const void *data, int len);
int ca_write_apdu(ca_device_t *d, int resource, const void *data, int len);
int find_session_for_resource(int resource);
int asn_1_decode(int *length, unsigned char *asn_1_array);

////// MISC.C

int get_random(unsigned char *dest, int len) {
    int fd;
    char *urnd = "/dev/urandom";

    fd = open(urnd, O_RDONLY);
    if (fd < 0) {
        LOG("cannot open %s", urnd);
        return -1;
    }

    if (read(fd, dest, len) != len) {
        LOG("cannot read from %s", urnd);
        close(fd);
        return -2;
    }

    close(fd);

    return len;
}

int add_padding(uint8_t *dest, unsigned int len, unsigned int blocklen) {
    uint8_t padding = 0x80;
    int count = 0;

    while (len & (blocklen - 1)) {
        *dest++ = padding;
        ++len;
        ++count;
        padding = 0;
    }

    return count;
}

static int get_bin_from_nibble(int in) {
    if ((in >= '0') && (in <= '9'))
        return in - 0x30;

    if ((in >= 'A') && (in <= 'Z'))
        return in - 0x41 + 10;

    if ((in >= 'a') && (in <= 'z'))
        return in - 0x61 + 10;

    LOG("fixme: unsupported chars in hostid");

    return 0;
}

void str2bin(uint8_t *dst, char *data, int len) {
    int i;

    for (i = 0; i < len; i += 2)
        *dst++ = (get_bin_from_nibble(data[i]) << 4) |
                 get_bin_from_nibble(data[i + 1]);
}

uint32_t UINT32(const unsigned char *in, unsigned int len) {
    uint32_t val = 0;
    unsigned int i;

    for (i = 0; i < len; i++) {
        val <<= 8;
        val |= *in++;
    }

    return val;
}

int BYTE32(unsigned char *dest, uint32_t val) {
    *dest++ = val >> 24;
    *dest++ = val >> 16;
    *dest++ = val >> 8;
    *dest++ = val;

    return 4;
}

int BYTE16(unsigned char *dest, uint16_t val) {
    *dest++ = val >> 8;
    *dest++ = val;
    return 2;
}

void cert_strings(char *certfile) {
    int c;
    unsigned count;
    //      off_t offset;
    FILE *file;
    char string[256];
    int n = 2; /* too short string to be usefull */
    int line = 0;

    file = fopen(certfile, "r");
    if (!file) {
        LOG("Could not open certificate file %s", certfile);
        return;
    }
    LOG("#########################################################\n");
    //      offset = 0;
    count = 0;
    do {
        if (line > 14)
            n = 8; /* after usefull info be stricter */
        c = fgetc(file);
        //              if (isprint(c) || c == '\t')
        if (isprint(c)) {
            string[count] = c;
            count++;
        } else {
            if (count > n) /* line feed */
            {
                string[count - 1] = 0;
                LOG("%s\n", string);
                line++;
            }
            count = 0;
        }
        //              offset++;
    } while ((c != EOF) && (line < 16)); /* only frst 15 lines */
    fclose(file);
    LOG("#########################################################\n");
    return;
}

/// END MISC

///// AES_XCBC_MAC

int aes_xcbc_mac_init(struct aes_xcbc_mac_ctx *ctx, const uint8_t *key) {
    AES_KEY aes_key;
    int y, x;

    memset(&aes_key, 0, sizeof(aes_key));

    AES_set_encrypt_key(key, 128, &aes_key);

    for (y = 0; y < 3; y++) {
        for (x = 0; x < 16; x++)
            ctx->K[y][x] = y + 1;
        AES_ecb_encrypt(ctx->K[y], ctx->K[y], &aes_key, 1);
    }

    /* setup K1 */
    AES_set_encrypt_key(ctx->K[0], 128, &ctx->key);

    memset(ctx->IV, 0, 16);
    ctx->buflen = 0;

    return 0;
}

int aes_xcbc_mac_process(struct aes_xcbc_mac_ctx *ctx, const uint8_t *in,
                         unsigned int len) {
    while (len) {
        if (ctx->buflen == 16) {
            AES_ecb_encrypt(ctx->IV, ctx->IV, &ctx->key, 1);
            ctx->buflen = 0;
        }
        ctx->IV[ctx->buflen++] ^= *in++;
        --len;
    }

    return 0;
}

int aes_xcbc_mac_done(struct aes_xcbc_mac_ctx *ctx, uint8_t *out) {
    int i;

    if (ctx->buflen == 16) {
        /* K2 */
        for (i = 0; i < 16; i++)
            ctx->IV[i] ^= ctx->K[1][i];
    } else {
        ctx->IV[ctx->buflen] ^= 0x80;
        /* K3 */
        for (i = 0; i < 16; i++)
            ctx->IV[i] ^= ctx->K[2][i];
    }

    AES_ecb_encrypt(ctx->IV, ctx->IV, &ctx->key, 1);
    memcpy(out, ctx->IV, 16);

    return 0;
}

////// END_AES_XCBC_MAC
////// DH_RSA_MISC

static int pkcs_1_mgf1(const uint8_t *seed, unsigned long seedlen,
                       uint8_t *mask, unsigned long masklen) {
    unsigned long hLen, x;
    uint32_t counter;
    uint8_t *buf;

    /* get hash output size */
    hLen = 20; /* SHA1 */

    /* allocate memory */
    buf = malloc(hLen);
    if (buf == NULL) {
        LOG("error mem");
        return -1;
    }

    /* start counter */
    counter = 0;

    while (masklen > 0) {
        /* handle counter */
        BYTE32(buf, counter);
        ++counter;

        /* get hash of seed || counter */
        unsigned char buffer[0x18];
        memcpy(buffer, seed, seedlen);
        memcpy(buffer + 0x14, buf, 4);
        SHA1(buffer, 0x18, buf);

        /* store it */
        for (x = 0; x < hLen && masklen > 0; x++, masklen--)
            *mask++ = buf[x];
    }

    free(buf);
    return 0;
}

static int pkcs_1_pss_encode(const uint8_t *msghash, unsigned int msghashlen,
                             unsigned long saltlen,
                             unsigned long modulus_bitlen, uint8_t *out,
                             unsigned int outlen) {
    unsigned char *DB, *mask, *salt, *hash;
    unsigned long x, y, hLen, modulus_len;
    int err = -1;
    unsigned char *hashbuf;
    unsigned int hashbuflen;

    hLen = 20; /* SHA1 */
    modulus_len = (modulus_bitlen >> 3) + (modulus_bitlen & 7 ? 1 : 0);

    /* allocate ram for DB/mask/salt/hash of size modulus_len */
    DB = malloc(modulus_len);
    mask = malloc(modulus_len);
    salt = malloc(modulus_len);
    hash = malloc(modulus_len);

    hashbuflen = 8 + msghashlen + saltlen;
    hashbuf = malloc(hashbuflen);

    if (!(DB && mask && salt && hash && hashbuf)) {
        LOG("out of memory");
        goto LBL_ERR;
    }

    /* generate random salt */
    if (saltlen > 0) {
        if (get_random(salt, saltlen) != (long)saltlen) {
            LOG("rnd failed");
            goto LBL_ERR;
        }
    }

    /* M = (eight) 0x00 || msghash || salt, hash = H(M) */
    memset(hashbuf, 0, 8);
    memcpy(hashbuf + 8, msghash, msghashlen);
    memcpy(hashbuf + 8 + msghashlen, salt, saltlen);
    SHA1(hashbuf, hashbuflen, hash);

    /* generate DB = PS || 0x01 || salt, PS == modulus_len - saltlen - hLen - 2
     * zero bytes */
    x = 0;
    memset(DB + x, 0, modulus_len - saltlen - hLen - 2);
    x += modulus_len - saltlen - hLen - 2;
    DB[x++] = 0x01;
    memcpy(DB + x, salt, saltlen);
    x += saltlen;

    err = pkcs_1_mgf1(hash, hLen, mask, modulus_len - hLen - 1);
    if (err)
        goto LBL_ERR;

    /* xor against DB */
    for (y = 0; y < (modulus_len - hLen - 1); y++)
        DB[y] ^= mask[y];

    /* output is DB || hash || 0xBC */
    if (outlen < modulus_len) {
        err = -1;
        LOG("error overflow");
        goto LBL_ERR;
    }

    /* DB len = modulus_len - hLen - 1 */
    y = 0;
    memcpy(out + y, DB, modulus_len - hLen - 1);
    y += modulus_len - hLen - 1;

    /* hash */
    memcpy(out + y, hash, hLen);
    y += hLen;

    /* 0xBC */
    out[y] = 0xBC;

    /* now clear the 8*modulus_len - modulus_bitlen most significant bits */
    out[0] &= 0xFF >> ((modulus_len << 3) - (modulus_bitlen - 1));

    err = 0;
LBL_ERR:
    free(hashbuf);
    free(hash);
    free(salt);
    free(mask);
    free(DB);

    return err;
}

/* DH */

int dh_gen_exp(uint8_t *dest, int dest_len, uint8_t *dh_g, int dh_g_len,
               uint8_t *dh_p, int dh_p_len) {
    DH *dh;
    BIGNUM *p, *g;
    const BIGNUM *priv_key;
    int len;
    unsigned int gap;

    dh = DH_new();

    p = BN_bin2bn(dh_p, dh_p_len, 0);
    g = BN_bin2bn(dh_g, dh_g_len, 0);
    DH_set0_pqg(dh, p, NULL, g);
    DH_set_flags(dh, DH_FLAG_NO_EXP_CONSTTIME);

    DH_generate_key(dh);

    DH_get0_key(dh, NULL, &priv_key);
    len = BN_num_bytes(priv_key);
    if (len > dest_len) {
        LOG("len > dest_len");
        return -1;
    }

    gap = dest_len - len;
    memset(dest, 0, gap);
    BN_bn2bin(priv_key, &dest[gap]);

    DH_free(dh);

    return 0;
}

/* dest = base ^ exp % mod */
int dh_mod_exp(uint8_t *dest, int dest_len, uint8_t *base, int base_len,
               uint8_t *mod, int mod_len, uint8_t *exp, int exp_len) {
    BIGNUM *bn_dest, *bn_base, *bn_exp, *bn_mod;
    BN_CTX *ctx;
    int len;
    unsigned int gap;

    bn_base = BN_bin2bn(base, base_len, NULL);
    bn_exp = BN_bin2bn(exp, exp_len, NULL);
    bn_mod = BN_bin2bn(mod, mod_len, NULL);
    ctx = BN_CTX_new();

    bn_dest = BN_new();
    BN_mod_exp(bn_dest, bn_base, bn_exp, bn_mod, ctx);
    BN_CTX_free(ctx);

    len = BN_num_bytes(bn_dest);
    if (len > dest_len) {
        LOG("len > dest_len");
        return -1;
    }

    gap = dest_len - len;
    memset(dest, 0, gap);
    BN_bn2bin(bn_dest, &dest[gap]);

    BN_free(bn_dest);
    BN_free(bn_mod);
    BN_free(bn_exp);
    BN_free(bn_base);

    return 0;
}

int dh_dhph_signature(uint8_t *out, uint8_t *nonce, uint8_t *dhph, RSA *r) {
    unsigned char dest[302];
    uint8_t hash[20];
    unsigned char dbuf[256];

    dest[0x00] = 0x00; /* version */
    dest[0x01] = 0x00;
    dest[0x02] = 0x08; /* len (bits) */
    dest[0x03] = 0x01; /* version data */

    dest[0x04] = 0x01; /* msg_label */
    dest[0x05] = 0x00;
    dest[0x06] = 0x08; /* len (bits) */
    dest[0x07] = 0x02; /* message data */

    dest[0x08] = 0x02; /* auth_nonce */
    dest[0x09] = 0x01;
    dest[0x0a] = 0x00; /* len (bits) */
    memcpy(&dest[0x0b], nonce, 32);

    dest[0x2b] = 0x04; /* DHPH - DH public key host */
    dest[0x2c] = 0x08;
    dest[0x2d] = 0x00; /* len (bits) */
    memcpy(&dest[0x2e], dhph, 256);

    SHA1(dest, 0x12e, hash);

    if (pkcs_1_pss_encode(hash, 20, 20, 0x800, dbuf, sizeof(dbuf))) {
        LOG("pss encode failed");
        return -1;
    }

    RSA_private_encrypt(sizeof(dbuf), dbuf, out, r, RSA_NO_PADDING);

    return 0;
}

int is_ca_initializing(int i) {
    if (i >= 0 && i < MAX_ADAPTERS && ca_devices[i] && ca_devices[i]->enabled &&
        !ca_devices[i]->init_ok)
        return 1;
    return 0;
}

int get_max_pmt_for_ca(int i) {
    if (i >= 0 && i < MAX_ADAPTERS && ca_devices[i] && ca_devices[i]->enabled)
        return (ca_devices[i]->multiple_pmt + 1) * ca_devices[i]->max_ca_pmt;
    return MAX_CA_PMT;
}

void send_cw_to_all_pmts(ca_device_t *d, int parity) {
    int i;
    for (i = 0; i < d->max_ca_pmt; i++) {
        if (PMT_ID_IS_VALID(d->capmt[i].pmt_id)) {
            send_cw(d->capmt[i].pmt_id, CA_ALGO_AES128_CBC, parity,
                    d->key[parity], d->iv[parity], 3720);
        }
        if (PMT_ID_IS_VALID(d->capmt[i].other_id)) {
            send_cw(d->capmt[i].other_id, CA_ALGO_AES128_CBC, parity,
                    d->key[parity], d->iv[parity], 3720);
        }
    }
}

void disable_cws_for_all_pmts(ca_device_t *d) {
    int i;
    for (i = 0; i < d->max_ca_pmt; i++) {
        if (PMT_ID_IS_VALID(d->capmt[i].pmt_id)) {
            disable_cw(d->capmt[i].pmt_id);
        }
        if (PMT_ID_IS_VALID(d->capmt[i].other_id)) {
            disable_cw(d->capmt[i].other_id);
        }
    }
}

int create_capmt(SCAPMT *ca, int listmgmt, uint8_t *capmt, int capmt_len,
                 int cmd_id) {
    int pos = 0;
    SPMT *pmt = get_pmt(ca->pmt_id);
    SPMT *other = get_pmt(ca->other_id);
    int version = ca->version;

    if (!pmt)
        LOG_AND_RETURN(1, "%s: PMT %d not found", __FUNCTION__, ca->pmt_id);

    capmt[pos++] = listmgmt;
    copy16(capmt, pos, ca->sid);
    pos += 2;
    capmt[pos++] = ((version & 0xF) << 1) | 0x1;
    capmt[pos++] = 0; // PI LEN 2 bytes, set 0
    capmt[pos++] = 0;

    pos += CAPMT_add_PMT(capmt + pos, capmt_len - pos, pmt, cmd_id);
    if (other) {
        pos += CAPMT_add_PMT(capmt + pos, capmt_len - pos, other, cmd_id);
    }

    return pos;
}

// Creates and Sends 2 PMTs to bundle them into the same CAPMT
// Second PMT can be NULL
int send_capmt(ca_device_t *d, SCAPMT *ca, int listmgmt, int reason) {
    uint8_t capmt[8192];
    memset(capmt, 0, sizeof(capmt));

    int size = create_capmt(ca, listmgmt, capmt, sizeof(capmt), reason);
    hexdump("CAPMT: ", capmt, size);

    if (size <= 0)
        LOG_AND_RETURN(TABLES_RESULT_ERROR_NORETRY, "create_capmt failed");

    int session_number = find_session_for_resource(EN50221_APP_CA_RESOURCEID);

    ca_write_apdu_session(d, session_number, TAG_CA_PMT, capmt, size);
    return 0;
}

int get_enabled_pmts_for_ca(ca_device_t *d) {
    int i, enabled_pmts = 0;
    for (i = 0; i < d->max_ca_pmt; i++) {
        if (PMT_ID_IS_VALID(d->capmt[i].pmt_id))
            enabled_pmts++;
        if (PMT_ID_IS_VALID(d->capmt[i].other_id))
            enabled_pmts++;
    }
    return enabled_pmts;
}

SCAPMT *add_pmt_to_capmt(ca_device_t *d, SPMT *pmt, int multiple) {
    int ca_pos;
    SCAPMT *res = NULL;
    for (ca_pos = 0; ca_pos < d->max_ca_pmt; ca_pos++) {
        if (d->capmt[ca_pos].pmt_id == -1) {
            d->capmt[ca_pos].pmt_id = pmt->id;
            res = d->capmt + ca_pos;
            break;
        }
        if (multiple && d->capmt[ca_pos].other_id == -1) {
            d->capmt[ca_pos].other_id = pmt->id;
            res = d->capmt + ca_pos;
            break;
        }
    }

    if (res) {
        res->version = (res->version + 1) & 0xF;
    } else {
        LOG("CA %d all channels used %d, multiple allowed %d", d->id,
            d->max_ca_pmt, multiple);
        for (ca_pos = 0; ca_pos < d->max_ca_pmt; ca_pos++) {
            LOG("CAPMT %d pmts %d %d", ca_pos, d->capmt[ca_pos].pmt_id,
                d->capmt[ca_pos].other_id);
        }
    }

    return res;
}

int get_active_capmts(ca_device_t *d) {
    int i, active = 0;
    for (i = 0; i < d->max_ca_pmt; i++)
        if (PMT_ID_IS_VALID(d->capmt[i].pmt_id) ||
            PMT_ID_IS_VALID(d->capmt[i].other_id))
            active++;
    return active;
}

// Sending _LIST, _MORE and _LAST leads to decryption issues on some CAMs
// (existing channels are restarted) This method tries to send _ONLY for the
// first PMT or when the sid changes (when 2 PMTs are packed inside the same
// CAPMT) and uses just _UPDATE to update the existing and new CAPMT According
// to the docs _ADD should be used for new CAPMTs but _UPDATE with a new sid
// works To not leak sids, the first channel will have the original SID (some
// CAMs do not like this), the other ones a fixed SID

int dvbca_process_pmt(adapter *ad, SPMT *spmt) {
    ca_device_t *d = ca_devices[ad->id];
    uint16_t pid, sid, ver;
    int listmgmt;
    SPMT *first = NULL;

    if (!d)
        return TABLES_RESULT_ERROR_NORETRY;
    if (!d->init_ok)
        LOG_AND_RETURN(TABLES_RESULT_ERROR_RETRY, "CAM not yet initialized");

    SCAPMT *capmt = add_pmt_to_capmt(d, spmt, d->multiple_pmt);
    if (!capmt)
        LOG_AND_RETURN(TABLES_RESULT_ERROR_RETRY,
                       "No free slots to add PMT %d to CA %d", spmt->id, d->id);

    first = get_pmt(capmt->pmt_id);
    if (!first)
        LOG_AND_RETURN(TABLES_RESULT_ERROR_NORETRY,
                       "First PMT not found from capmt %d", capmt->pmt_id);
    pid = spmt->pid;
    ver = capmt->version;
    sid = spmt->sid;

    listmgmt = get_active_capmts(d) == 1 ? CLM_ONLY : CLM_UPDATE;
    if (listmgmt == CLM_ONLY) {
        if (capmt->sid == first->sid)
            listmgmt = CLM_UPDATE;
        capmt->sid = first->sid;
        int i;
        for (i = 0; i < d->max_ca_pmt; i++) {
            int sid = MAKE_SID_FOR_CA(d->id, i);
            if (capmt != d->capmt + i) {
                if (capmt->sid == sid)
                    d->capmt[i].sid = MAKE_SID_FOR_CA(d->id, MAX_CA_PMT);
                else
                    d->capmt[i].sid = sid;
            }
        }
    }

    LOG("PMT CA %d pmt %d pid %u (%s) ver %u sid %u (%d), "
        "enabled_pmts %d, "
        "%s, PMTS to be send %d %d, pos %ld",
        spmt->adapter, spmt->id, pid, spmt->name, ver, sid, capmt->sid,
        get_enabled_pmts_for_ca(d), listmgmt_str[listmgmt], capmt->pmt_id,
        capmt->other_id, capmt - d->capmt);

    if (send_capmt(d, capmt, listmgmt, CA_PMT_CMD_ID_OK_DESCRAMBLING))
        LOG_AND_RETURN(TABLES_RESULT_ERROR_NORETRY, "send_capmt failed");

    if (d->key[0][0])
        send_cw(spmt->id, CA_ALGO_AES128_CBC, 0, d->key[0], d->iv[0],
                3600); // 1 hour
    if (d->key[1][0])
        send_cw(spmt->id, CA_ALGO_AES128_CBC, 1, d->key[1], d->iv[1], 3600);

    return 0;
}

// Remove the PMT from the CAPMT associated with this device
// if the pmt_id is not valid and other_id is valid,
// exchange the IDs
void remove_pmt_from_device(ca_device_t *d, SPMT *pmt) {
    int i;
    for (i = 0; i < d->max_ca_pmt; i++) {
        if (d->capmt[i].pmt_id == pmt->id) {
            d->capmt[i].pmt_id = PMT_INVALID;
            if (PMT_ID_IS_VALID(d->capmt[i].other_id)) {
                d->capmt[i].pmt_id = d->capmt[i].other_id;
                d->capmt[i].other_id = PMT_INVALID;
                d->capmt[i].version++;
            }
        }
        if (d->capmt[i].other_id == pmt->id) {
            d->capmt[i].other_id = PMT_INVALID;
            d->capmt[i].version++;
        }
    }
    return;
}

SCAPMT *get_capmt_for_pmt(ca_device_t *d, SPMT *pmt) {
    int i;
    for (i = 0; i < d->max_ca_pmt; i++)
        if (d->capmt[i].pmt_id == pmt->id || d->capmt[i].other_id == pmt->id)
            return d->capmt + i;
    return NULL;
}

int dvbca_del_pmt(adapter *ad, SPMT *spmt) {
    ca_device_t *d = ca_devices[ad->id];
    int listmgmt = -1;

    SCAPMT *capmt = get_capmt_for_pmt(d, spmt);
    if (!capmt)
        LOG_AND_RETURN(0, "CAPMT not found for pmt %d", spmt->id);

    remove_pmt_from_device(d, spmt);
    if (PMT_ID_IS_VALID(capmt->pmt_id)) {
        listmgmt = CLM_UPDATE;
        if (send_capmt(d, capmt, listmgmt, CA_PMT_CMD_ID_OK_DESCRAMBLING))
            LOG_AND_RETURN(TABLES_RESULT_ERROR_NORETRY,
                           "%s: send_capmt failed for pmt ids %d", __FUNCTION__,
                           capmt->pmt_id)
    }
    LOG("PMT CA %d DEL pmt %d pid %u, sid %u (%u), ver %d, %s name: %s",
        spmt->adapter, spmt->id, spmt->pid, spmt->sid, capmt->sid,
        capmt->version, listmgmt >= 0 ? listmgmt_str[listmgmt] : "NO UPDATE",
        spmt->name);
    return 0;
}

static int ciplus13_app_ai_data_rate_info(ca_device_t *d,
                                          ciplus13_data_rate_t rate) {
    LOG("setting CI+ CAM data rate to %s Mbps", rate ? "96" : "72");

    ca_write_apdu(d, CI_DATA_RATE_INFO, &rate, sizeof(rate));
    return 0;
}

static struct element *element_get(struct cc_ctrl_data *cc_data,
                                   unsigned int id) {
    /* array index */
    if ((id < 1) || (id >= MAX_ELEMENTS)) {
        LOG("element_get: invalid id");
        return NULL;
    }
    return &cc_data->elements[id];
}

static void element_invalidate(struct cc_ctrl_data *cc_data, unsigned int id) {
    struct element *e;

    e = element_get(cc_data, id);
    if (e) {
        free(e->data);
        memset(e, 0, sizeof(struct element));
    }
}

static void element_init(struct cc_ctrl_data *cc_data) {
    unsigned int i;

    for (i = 1; i < MAX_ELEMENTS; i++)
        element_invalidate(cc_data, i);
}

static int element_set(struct cc_ctrl_data *cc_data, unsigned int id,
                       const uint8_t *data, uint32_t size) {
    struct element *e;

    e = element_get(cc_data, id);
    if (e == NULL)
        return 0;

    /* check size */
    if ((datatype_sizes[id] != 0) && (datatype_sizes[id] != size)) {
        LOG("size %d of datatype_id %d doesn't match", size, id);
        return 0;
    }

    free(e->data);
    e->data = malloc(size);
    memcpy(e->data, data, size);
    e->size = size;
    e->valid = 1;

    LOGM("_element_set_ stored %d with len %d", id, size);
    //       hexdump("DATA: ", (void *)data, size);
    return 1;
}

static int element_set_certificate(struct cc_ctrl_data *cc_data,
                                   unsigned int id, X509 *cert) {
    unsigned char *cert_der = NULL;
    int cert_len;

    cert_len = i2d_X509(cert, &cert_der);
    if (cert_len <= 0) {
        LOG("can not get data in DER format");
        return 0;
    }

    if (!element_set(cc_data, id, cert_der, cert_len)) {
        LOG("can not store element (%d)", id);
        return 0;
    }

    return 1;
}

static int element_set_hostid_from_certificate(struct cc_ctrl_data *cc_data,
                                               unsigned int id, X509 *cert) {
    X509_NAME *subject;
    int nid_cn = OBJ_txt2nid("CN");
    char hostid[20];
    uint8_t bin_hostid[8];

    if ((id != 5) && (id != 6)) {
        LOG("wrong datatype_id for hostid");
        return 0;
    }

    subject = X509_get_subject_name(cert);
    X509_NAME_get_text_by_NID(subject, nid_cn, hostid, sizeof(hostid));

    if (strlen(hostid) != 16) {
        LOG("malformed hostid");
        return 0;
    }
    LOG("%sID: %s", id == 5 ? "HOST" : "CICAM", hostid);

    str2bin(bin_hostid, hostid, 16);

    if (!element_set(cc_data, id, bin_hostid, sizeof(bin_hostid))) {
        LOG("can not set hostid");
        return 0;
    }

    return 1;
}

static int element_valid(struct cc_ctrl_data *cc_data, unsigned int id) {
    struct element *e;

    e = element_get(cc_data, id);

    return e && e->valid;
}

static unsigned int element_get_buf(struct cc_ctrl_data *cc_data, uint8_t *dest,
                                    unsigned int id) {
    struct element *e;

    e = element_get(cc_data, id);
    if (e == NULL)
        return 0;

    if (!e->valid) {
        LOG("element_get_buf: datatype %d not valid", id);
        return 0;
    }

    if (!e->data) {
        LOG("element_get_buf: datatype %d doesn't exist", id);
        return 0;
    }

    if (dest)
        memcpy(dest, e->data, e->size);

    return e->size;
}

static unsigned int element_get_req(struct cc_ctrl_data *cc_data, uint8_t *dest,
                                    unsigned int id) {
    unsigned int len = element_get_buf(cc_data, &dest[3], id);

    if (len == 0) {
        LOG("can not get element %d", id);
        return 0;
    }

    dest[0] = id;
    dest[1] = len >> 8;
    dest[2] = len;

    return 3 + len;
}

static uint8_t *element_get_ptr(struct cc_ctrl_data *cc_data, unsigned int id) {
    struct element *e;

    e = element_get(cc_data, id);
    if (e == NULL)
        return NULL;

    if (!e->valid) {
        LOG("element_get_ptr: datatype %u not valid", id);
        return NULL;
    }

    if (!e->data) {
        LOG("element_get_ptr: datatype %u doesn't exist", id);

        return NULL;
    }

    return e->data;
}

void get_authdata_filename(char *dest, size_t len, unsigned int slot,
                           char *ci_name) {
    char cin[128];
    /* add module name to slot authorization bin file */
    memset(cin, 0, sizeof(cin));
    strncpy(cin, ci_name, sizeof(cin) - 1);
    /* quickly replace blanks */
    int i = 0;
    while (cin[i] != 0) {
        if (cin[i] == 32)
            cin[i] = 95; /* underscore _ */
        i++;
    };

    snprintf(dest, len, "%s/ci_auth_%s_%d.bin", opts.cache_dir, cin, slot);
}

static int get_authdata(uint8_t *host_id, uint8_t *dhsk, uint8_t *akh,
                        unsigned int slot, unsigned int index, char *ci_name) {
    char filename[FILENAME_MAX];
    int fd;
    uint8_t chunk[8 + 256 + 32];
    unsigned int i;
    /* 5 pairs of data only */
    if (index > MAX_PAIRS)
        return 0;

    get_authdata_filename(filename, sizeof(filename), slot, ci_name);

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        LOG("cannot open %s", filename);
        return 0;
    }

    for (i = 0; i < MAX_PAIRS; i++) {
        if (read(fd, chunk, sizeof(chunk)) != sizeof(chunk)) {
            LOG("cannot read auth_data");
            close(fd);
            return 0;
        }
        if (i == index) {
            memcpy(host_id, chunk, 8);
            memcpy(dhsk, &chunk[8], 256);
            memcpy(akh, &chunk[8 + 256], 32);
            close(fd);
            return 1;
        }
    }

    close(fd);
    return 0;
}

static int write_authdata(unsigned int slot, const uint8_t *host_id,
                          const uint8_t *dhsk, const uint8_t *akh,
                          char *ci_name) {
    char filename[FILENAME_MAX];
    int fd;
    uint8_t buf[(8 + 256 + 32) * MAX_PAIRS];
    unsigned int entries;
    unsigned int i;

    int ret = 0;

    for (entries = 0; entries < MAX_PAIRS; entries++) {
        int offset = (8 + 256 + 32) * entries;
        if (!get_authdata(&buf[offset], &buf[offset + 8],
                          &buf[offset + 8 + 256], slot, entries, ci_name))
            break;

        /* check if we got this pair already */
        if (!memcmp(&buf[offset + 8 + 256], akh, 32)) {
            LOG("data already stored");
            return 1;
        }
    }

    LOG("got %d entries", entries);

    get_authdata_filename(filename, sizeof(filename), slot, ci_name);

    fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        LOG("cannot open %s for writing - authdata not stored", filename);
        return 0;
    }

    /* store new entry first */
    if (write(fd, host_id, 8) != 8) {
        LOG("error in write");
        goto end;
    }

    if (write(fd, dhsk, 256) != 256) {
        LOG("error in write");
        goto end;
    }

    if (write(fd, akh, 32) != 32) {
        LOG("error in write");
        goto end;
    }

    /* skip the last one if exists */
    if (entries > 3)
        entries = 3;

    for (i = 0; i < entries; i++) {
        int offset = (8 + 256 + 32) * i;
        if (write(fd, &buf[offset], (8 + 256 + 32)) != (8 + 256 + 32)) {
            LOG("error in write");
            goto end;
        }
    }

    ret = 1;
end:
    close(fd);

    return ret;
}

static int data_initialize(ca_device_t *d) {
    uint8_t buf[32], host_id[8];

    memset(&d->private_data, 0, sizeof(d->private_data));

    /* clear storage of credentials */
    element_init(&d->private_data);

    /* set status field - OK */
    memset(buf, 0, 1);
    if (!element_set(&d->private_data, 30, buf, 1)) {
        LOG("can not set status in elements");
    }

    /* set uri versions */
    memset(buf, 0, 32);
    buf[31] = d->uri_mask; // uri version bitmask, e.g. 1-3
    LOG("uri version bitmask set to '%d'", buf[31]);
    if (!element_set(&d->private_data, 29, buf, 32)) {
        LOG("can not set uri_versions in elements");
    }
    /* load first AKH */
    d->private_data.akh_index = 0;
    if (!get_authdata(host_id, d->private_data.dhsk, buf, d->id,
                      d->private_data.akh_index, d->ci_name)) {
        //        if (!get_authdata(host_id, data->dhsk, buf, 1,
        //        data->akh_index)) {
        /* no AKH available */
        memset(buf, 0, sizeof(buf));
        d->private_data.akh_index = 5; /* last one */
    }

    if (!element_set(&d->private_data, 22, buf, 32)) {
        LOG("can not set AKH in elements");
    }

    if (!element_set(&d->private_data, 5, host_id, 8)) {
        LOG("can not set host_id elements");
    }

    return 1;
}
/* content_control commands */

static int sac_check_auth(const uint8_t *data, unsigned int len, uint8_t *sak) {
    struct aes_xcbc_mac_ctx ctx;
    uint8_t calced_signature[16];

    if (len < 16) {
        LOG("auth too short");
        return 0;
    }

    aes_xcbc_mac_init(&ctx, sak);
    aes_xcbc_mac_process(&ctx, (uint8_t *)"\x04", 1); /* header len */
    aes_xcbc_mac_process(&ctx, data, len - 16);
    aes_xcbc_mac_done(&ctx, calced_signature);

    if (memcmp(&data[len - 16], calced_signature, 16)) {
        LOG("signature wrong");
        return 0;
    }

    LOG("auth ok!");
    return 1;
}

static int sac_gen_auth(uint8_t *out, uint8_t *in, unsigned int len,
                        uint8_t *sak) {
    struct aes_xcbc_mac_ctx ctx;

    aes_xcbc_mac_init(&ctx, sak);
    aes_xcbc_mac_process(&ctx, (uint8_t *)"\x04", 1); /* header len */
    aes_xcbc_mac_process(&ctx, in, len);
    aes_xcbc_mac_done(&ctx, out);

    return 16;
}

static void generate_key_seed(struct cc_ctrl_data *cc_data) {
    /* this is triggered by new ns_module */

    /* generate new key_seed -> SEK/SAK key derivation */

    SHA256_CTX sha;

    SHA256_Init(&sha);
    SHA256_Update(&sha, &cc_data->dhsk[240], 16);
    SHA256_Update(&sha, element_get_ptr(cc_data, 22),
                  element_get_buf(cc_data, NULL, 22));
    SHA256_Update(&sha, element_get_ptr(cc_data, 20),
                  element_get_buf(cc_data, NULL, 20));
    SHA256_Update(&sha, element_get_ptr(cc_data, 21),
                  element_get_buf(cc_data, NULL, 21));
    SHA256_Final(cc_data->ks_host, &sha);
}

static void generate_ns_host(struct cc_ctrl_data *cc_data)

{
    uint8_t buf[8];
    get_random(buf, sizeof(buf));
    element_set(cc_data, 20, buf, sizeof(buf));
}

static int generate_SAK_SEK(uint8_t *sak, uint8_t *sek,
                            const uint8_t *ks_host) {
    AES_KEY key;
    const uint8_t key_data[16] = {0xea, 0x74, 0xf4, 0x71, 0x99, 0xd7,
                                  0x6f, 0x35, 0x89, 0xf0, 0xd1, 0xdf,
                                  0x0f, 0xee, 0xe3, 0x00};
    uint8_t dec[32];
    int i;

    /* key derivation of sak & sek */
    memset(&key, 0, sizeof(key));

    AES_set_encrypt_key(key_data, 128, &key);

    for (i = 0; i < 2; i++)
        AES_ecb_encrypt(&ks_host[16 * i], &dec[16 * i], &key, 1);

    for (i = 0; i < 16; i++)
        sek[i] = ks_host[i] ^ dec[i];

    for (i = 0; i < 16; i++)
        sak[i] = ks_host[16 + i] ^ dec[16 + i];

    return 0;
}

static int sac_crypt(uint8_t *dst, const uint8_t *src, unsigned int len,
                     const uint8_t *key_data, int encrypt) {
    uint8_t iv[16] = {0xf7, 0x70, 0xb0, 0x36, 0x03, 0x61, 0xf7, 0x96,
                      0x65, 0x74, 0x8a, 0x26, 0xea, 0x4e, 0x85, 0x41};
    AES_KEY key;

    /* AES_ENCRYPT is '1' */
    memset(&key, 0, sizeof(key));

    if (encrypt)
        AES_set_encrypt_key(key_data, 128, &key);
    else
        AES_set_decrypt_key(key_data, 128, &key);

    AES_cbc_encrypt(src, dst, len, &key, iv, encrypt);

    return 0;
}

static int verify_cb(int ok, X509_STORE_CTX *ctx) {
    if (X509_STORE_CTX_get_error(ctx) == X509_V_ERR_CERT_NOT_YET_VALID) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        if (t->tm_year < 2015) {
            LOG("seems our rtc is wrong - ignore!");
            return 1;
        }
    }

    if (X509_STORE_CTX_get_error(ctx) == X509_V_ERR_CERT_HAS_EXPIRED)
        return 1;
    return 0;
}

static RSA *rsa_privatekey_open(const char *filename) {
    FILE *fp;
    RSA *r = NULL;

    fp = fopen(filename, "r");
    if (!fp) {
        LOG("can not open %s", filename);
        return NULL;
    }

    PEM_read_RSAPrivateKey(fp, &r, NULL, NULL);
    if (!r) {
        LOG("read error");
    }

    fclose(fp);

    return r;
}

static X509 *certificate_open(const char *filename) {
    FILE *fp;
    X509 *cert;

    fp = fopen(filename, "r");
    if (!fp) {
        LOG("can not open %s", filename);
        return NULL;
    }

    cert = PEM_read_X509(fp, NULL, NULL, NULL);
    if (!cert) {
        LOG("can not read cert");
    }

    fclose(fp);

    return cert;
}

static int certificate_validate(struct cert_ctx *ctx, X509 *cert) {
    X509_STORE_CTX *store_ctx;
    int ret;

    store_ctx = X509_STORE_CTX_new();

    X509_STORE_CTX_init(store_ctx, ctx->store, cert, NULL);
    X509_STORE_CTX_set_verify_cb(store_ctx, verify_cb);
    X509_STORE_CTX_set_flags(store_ctx, X509_V_FLAG_IGNORE_CRITICAL);

    ret = X509_verify_cert(store_ctx);

    if (ret != 1) {
        LOG("%s",
            X509_verify_cert_error_string(X509_STORE_CTX_get_error(store_ctx)));
    }

    X509_STORE_CTX_free(store_ctx);

    if (ret == 1)
        return 1;
    else
        return 0;
}

static X509 *certificate_load_and_check(struct cert_ctx *ctx,
                                        const char *filename) {
    X509 *cert;

    if (!ctx->store) {
        /* we assume this is the first certificate added - so its root-ca */
        ctx->store = X509_STORE_new();
        if (!ctx->store) {
            LOG("can not create cert_store");
            exit(-1);
        }

        if (X509_STORE_load_locations(ctx->store, filename, NULL) != 1) {
            LOG("load of first certificate (root_ca) failed");
            exit(-1);
        }

        return NULL;
    }

    cert = certificate_open(filename);
    if (!cert) {
        LOG("can not open certificate %s", filename);
        return NULL;
    }

    if (!certificate_validate(ctx, cert)) {
        LOG("can not vaildate certificate");
        X509_free(cert);
        return NULL;
    }

    /* push into store - create a chain */
    if (X509_STORE_load_locations(ctx->store, filename, NULL) != 1) {
        LOG("load of certificate failed");
        X509_free(cert);
        return NULL;
    }

    return cert;
}

static X509 *certificate_import_and_check(struct cert_ctx *ctx,
                                          const uint8_t *data, int len) {
    X509 *cert;

    cert = d2i_X509(NULL, &data, len);
    if (!cert) {
        LOG("can not read certificate");
        return NULL;
    }

    if (!certificate_validate(ctx, cert)) {
        LOG("can not vaildate certificate");
        X509_free(cert);
        return NULL;
    }

    X509_STORE_add_cert(ctx->store, cert);

    return cert;
}

static X509 *import_ci_certificates(struct cc_ctrl_data *cc_data,
                                    unsigned int id) {
    struct cert_ctx *ctx = cc_data->cert_ctx;
    X509 *cert;
    uint8_t buf[2048];
    unsigned int len;

    len = element_get_buf(cc_data, buf, id);

    cert = certificate_import_and_check(ctx, buf, len);
    if (!cert) {
        LOG("cannot read/verify DER cert");
        return NULL;
    }

    return cert;
}

static int check_ci_certificates(struct cc_ctrl_data *cc_data) {
    struct cert_ctx *ctx = cc_data->cert_ctx;

    /* check if both certificates are available before we push and verify them
     */

    /* check for CICAM_BrandCert */
    if (!element_valid(cc_data, 8)) {
        LOG("CICAM brand cert invalid");
        return -1;
    }

    /* check for CICAM_DevCert */
    if (!element_valid(cc_data, 16)) {
        LOG("CICAM device cert invalid");
        return -1;
    }

    if (extract_ci_cert) {
        /* write ci device cert to disk */
        char ci_cert_file[256];
        memset(ci_cert_file, 0, sizeof(ci_cert_file));
        snprintf(ci_cert_file, sizeof(ci_cert_file) - 1, "%s/ci_cert_%s_%d.der",
                 opts.cache_dir, ci_name_underscore, ci_number);
        LOG("CI%d EXTRACTING %s", ci_number, ci_cert_file);
        int fd =
            open(ci_cert_file, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd < 0)
            LOG_AND_RETURN(-1, "opening %s for writing failed", ci_cert_file);
        int ret = write(fd, element_get_ptr(cc_data, 16),
                        element_get_buf(cc_data, NULL, 16));
        if (ret)
            LOG("write cert failed");
        close(fd);
        /* display strings in der cert file */
        cert_strings(ci_cert_file);
    }

    /* import CICAM_BrandCert */
    if ((ctx->ci_cust_cert = import_ci_certificates(cc_data, 8)) == NULL) {
        LOG("can not import brand cert");
        return -1;
    }

    /* import CICAM_DevCert */
    if ((ctx->ci_device_cert = import_ci_certificates(cc_data, 16)) == NULL) {
        LOG("can not import device cert");
        return -1;
    }

    /* everything seems to be fine here - so extract the CICAM_id from cert */
    if (!element_set_hostid_from_certificate(cc_data, 6, ctx->ci_device_cert)) {
        LOG("can not set cicam_id in elements");
        return -1;
    }

    return 0;
}

static int generate_akh(struct cc_ctrl_data *cc_data) {
    uint8_t akh[32];
    SHA256_CTX sha;

    SHA256_Init(&sha);
    SHA256_Update(&sha, element_get_ptr(cc_data, 6),
                  element_get_buf(cc_data, NULL, 6));
    SHA256_Update(&sha, element_get_ptr(cc_data, 5),
                  element_get_buf(cc_data, NULL, 5));
    SHA256_Update(&sha, cc_data->dhsk, 256);
    SHA256_Final(akh, &sha);

    element_set(cc_data, 22, akh, sizeof(akh));

    return 0;
}

static int check_dh_challenge(ca_device_t *d) {
    /* check if every element for calculation of DHSK & AKH is available */
    struct cc_ctrl_data *cc_data = &d->private_data;
    LOG("checking ...");

    /* check for auth_nonce */
    if (!element_valid(cc_data, 19)) {
        LOG("auth nonce invalid");
        return 0;
    }

    /* check for CICAM_id */
    if (!element_valid(cc_data, 6)) {
        LOG("cicam id invalid");
        return 0;
    }

    /* check for DHPM */
    if (!element_valid(cc_data, 14)) {
        LOG("dphm invalid");
        return 0;
    }

    /* check for Signature_B */
    if (!element_valid(cc_data, 18)) {
        LOG("signature B invalid");
        return 0;
    }

    /* calculate DHSK - DHSK = DHPM ^ dh_exp % dh_p */
    dh_mod_exp(cc_data->dhsk, 256, element_get_ptr(cc_data, 14), 256, dh_p,
               sizeof(dh_p), cc_data->dh_exp, 256);

    /* gen AKH */
    generate_akh(cc_data);

    /* disable 5 tries of startup -> use new calculated one */
    cc_data->akh_index = 5;

    LOG("writing authdata ...");
    /* write to disk */
    write_authdata(d->id, element_get_ptr(cc_data, 5), cc_data->dhsk,
                   element_get_ptr(cc_data, 22), d->ci_name);

    return 1;
}

static int restart_dh_challenge(struct cc_ctrl_data *cc_data) {
    uint8_t dhph[256], sign_A[256];
    struct cert_ctx *ctx;
    LOG(".... rechecking ...");

    if (!cc_data->cert_ctx) {
        ctx = calloc(1, sizeof(struct cert_ctx));
        cc_data->cert_ctx = ctx;
    } else {
        ctx = cc_data->cert_ctx;
    }

    /* load certificates and device key */
    certificate_load_and_check(ctx, "/etc/ssl/certs/root.pem");
    ctx->cust_cert =
        certificate_load_and_check(ctx, "/etc/ssl/certs/customer.pem");
    ctx->device_cert =
        certificate_load_and_check(ctx, "/etc/ssl/certs/device.pem");

    if (!ctx->cust_cert || !ctx->device_cert) {
        LOG("can not check loader certificates");
        return -1;
    }

    /* add data to element store */
    if (!element_set_certificate(cc_data, 7, ctx->cust_cert))
        LOG("can not store cert in elements");

    if (!element_set_certificate(cc_data, 15, ctx->device_cert))
        LOG("can not store cert in elements");

    if (!element_set_hostid_from_certificate(cc_data, 5, ctx->device_cert))
        LOG("can not set hostid in elements");

    cc_data->rsa_device_key = rsa_privatekey_open("/etc/ssl/certs/device.pem");
    if (!cc_data->rsa_device_key) {
        LOG("can not read private key");
        return -1;
    }

    /* invalidate elements */
    element_invalidate(cc_data, 6);
    element_invalidate(cc_data, 14);
    element_invalidate(cc_data, 18);
    element_invalidate(cc_data, 22); /* this will refuse a unknown cam */

    /* new dh_exponent */
    dh_gen_exp(cc_data->dh_exp, 256, dh_g, sizeof(dh_g), dh_p, sizeof(dh_p));

    /* new DHPH  - DHPH = dh_g ^ dh_exp % dh_p */
    dh_mod_exp(dhph, sizeof(dhph), dh_g, sizeof(dh_g), dh_p, sizeof(dh_p),
               cc_data->dh_exp, 256);

    /* store DHPH */
    element_set(cc_data, 13, dhph, sizeof(dhph));

    /* create Signature_A */
    dh_dhph_signature(sign_A, element_get_ptr(cc_data, 19), dhph,
                      cc_data->rsa_device_key);

    /* store Signature_A */
    element_set(cc_data, 17, sign_A, sizeof(sign_A));

    return 0;
}

static int generate_uri_confirm(struct cc_ctrl_data *cc_data,
                                const uint8_t *sak) {
    SHA256_CTX sha;
    uint8_t uck[32];
    uint8_t uri_confirm[32];

    /* calculate UCK (uri confirmation key) */
    SHA256_Init(&sha);
    SHA256_Update(&sha, sak, 16);
    SHA256_Final(uck, &sha);

    /* calculate uri_confirm */
    SHA256_Init(&sha);
    SHA256_Update(&sha, element_get_ptr(cc_data, 25),
                  element_get_buf(cc_data, NULL, 25));
    SHA256_Update(&sha, uck, 32);
    SHA256_Final(uri_confirm, &sha);

    element_set(cc_data, 27, uri_confirm, 32);

    return 0;
}

static void check_new_key(ca_device_t *d, struct cc_ctrl_data *cc_data) {
    const uint8_t s_key[16] = {0x3e, 0x20, 0x15, 0x84, 0x2c, 0x37, 0xce, 0xe3,
                               0xd6, 0x14, 0x57, 0x3e, 0x3a, 0xab, 0x91, 0xb6};
    AES_KEY aes_ctx;
    uint8_t dec[32];
    uint8_t *kp;
    uint8_t slot;
    unsigned int i;
    memset(&aes_ctx, 0, sizeof(aes_ctx));

    /* check for keyprecursor */
    if (!element_valid(cc_data, 12)) {
        LOG("key precursor invalid");
        return;
    }

    /* check for slot */
    if (!element_valid(cc_data, 28)) {
        LOG("slot(key register) invalid");
        return;
    }
    kp = element_get_ptr(cc_data, 12);
    element_get_buf(cc_data, &slot, 28);

    AES_set_encrypt_key(s_key, 128, &aes_ctx);
    for (i = 0; i < 32; i += 16)
        AES_ecb_encrypt(&kp[i], &dec[i], &aes_ctx, 1);

    for (i = 0; i < 32; i++)
        dec[i] ^= kp[i];

    LOGM("=== descrambler_set_key === adapter = CA%i key regiser (0-even, "
         "1-odd) "
         "= %i",
         d->id, slot);
    char buf[400];
    int pos;
    pos = sprintf(buf, "KEY: ");
    for (i = 0; i < 16; i++)
        pos += sprintf(buf + pos, "%02X ", dec[i]);
    pos += sprintf(buf + pos, "\n\t\t\t\t\t\tIV: ");
    for (i = 16; i < 32; i++)
        pos += sprintf(buf + pos, "%02X ", dec[i]);
    LOG("received from CI+ CAM %d: %s", d->id, buf);

    memcpy(d->key[slot], dec, 16);
    memcpy(d->iv[slot], dec + 16, 16);
    d->parity = slot;

    send_cw_to_all_pmts(d, slot);

    d->is_ciplus = 1;

    /* reset */
    element_invalidate(cc_data, 12);
    element_invalidate(cc_data, 28);
}

static int data_get_handle_new(ca_device_t *d, unsigned int id) {
    /* handle trigger events */
    struct cc_ctrl_data *cc_data = &d->private_data;

    /* depends on new received items */
    // LOG("!!!!!!!!!!!!!!!!!!!! data_get_handle_new ID =
    // %i!!!!!!!!!!!!!!!!!!!!!!!!!", id);
    switch (id) {
    case 8: /* CICAM_BrandCert */
        /* this results in CICAM_ID when cert-chain is verified and ok */
        if (check_ci_certificates(cc_data))
            break;
        /* generate DHSK & AKH */
        check_dh_challenge(d);
        break;

    case 19: /* auth_nonce - triggers new dh keychallenge - invalidates DHSK &
              * AKH
              */
        /* generate DHPH & Signature_A */
        if (restart_dh_challenge(cc_data) != 0)
            d->force_ci = 1;
        break;

    case 21: /* Ns_module - triggers SAC key calculation */
        generate_ns_host(cc_data);
        generate_key_seed(cc_data);
        generate_SAK_SEK(cc_data->sak, cc_data->sek, cc_data->ks_host);
        break;

        /* SAC data messages */

    case 28: /* key register */
        check_new_key(d, cc_data);
        break;
    case 25: // uri_message
             //        case 26:        /* program_number */
        generate_uri_confirm(cc_data, cc_data->sak);
        break;

    case 6:  /* CICAM_id */
    case 12: /* keyprecursor */
    case 14: /* DHPM */
    case 16: /* CICAM_DevCert */
    case 18: /* Signature_B */
    case 26: /* program_number */
        LOGM("not need to be handled id %d", id);
        break;

    default:
        LOG("unhandled id %d", id);
        break;
    }

    return 0;
}

static int data_req_handle_new(ca_device_t *d, unsigned int id) {
    switch (id) {
    case 22: /* AKH */
    {
        uint8_t akh[32], host_id[8];
        memset(akh, 0, sizeof(akh));
        if (d->private_data.akh_index != 5) {
            if (!get_authdata(host_id, d->private_data.dhsk, akh, d->id,
                              d->private_data.akh_index++, d->ci_name))
                d->private_data.akh_index = 5;
            if (!element_set(&d->private_data, 22, akh, 32))
                LOG("cannot set AKH in elements");
            if (!element_set(&d->private_data, 5, host_id, 8))
                LOG("cannot set host_id in elements");
        }
    }
    default:
        break;
    }

    return 0;
}

static int data_get_loop(ca_device_t *d, struct cc_ctrl_data *cc_data,
                         const unsigned char *data, unsigned int datalen,
                         unsigned int items) {
    unsigned int i;
    int dt_id, dt_len;
    unsigned int pos = 0;

    for (i = 0; i < items; i++) {
        if (pos + 3 > datalen)
            return 0;
        dt_id = data[pos++];
        dt_len = data[pos++] << 8;
        dt_len |= data[pos++];
        if (pos + dt_len > datalen)
            return 0;
        LOGM("set element(dt_id) %d dt_len = %i", dt_id, dt_len);
        //                hexdump("data_get_loop: ", (void *)&data[pos],
        //                dt_len);
        element_set(cc_data, dt_id, &data[pos], dt_len);
        data_get_handle_new(d, dt_id);

        pos += dt_len;
    }

    return pos;
}

static int data_req_loop(ca_device_t *d, unsigned char *dest,
                         const unsigned char *data, unsigned int datalen,
                         unsigned int items) {
    int dt_id;
    unsigned int i;
    int pos = 0;
    int len;

    if (items > datalen)
        return -1;

    for (i = 0; i < items; i++) {
        dt_id = *data++;
        LOGM("req element %d", dt_id);
        data_req_handle_new(
            d,
            dt_id); /* check if there is any action needed before we answer */
        len = element_get_req(&d->private_data, dest, dt_id);
        if (len == 0) {
            LOG("cannot get element %d", dt_id);
            return -1;
        }
        pos += len;
        dest += len;
    }

    return pos;
}

/////////////////////////////////////////////////////////////////////////////////////////

void ci_ccmgr_cc_open_cnf(ca_device_t *d) {
    const uint8_t bitmap = 0x01;

    data_initialize(d);
    LOGM("SEND ------------ CC_OPEN_CNF----------- ");
    ca_write_apdu(d, 0x9f9002, &bitmap, 1);
}

static int ci_ccmgr_cc_sac_send(ca_device_t *d, uint32_t tag, uint8_t *data,
                                unsigned int pos) {
    struct cc_ctrl_data *cc_data = &d->private_data;
    if (pos < 8)
        return 0;
    LOG("______________________ci_ccmgr_cc_sac_send______________________");
    //	_hexdump("TAG:   ", &tag, 3);
    //	_hexdump("UNENCRYPTED:  ", data, pos);

    pos += add_padding(&data[pos], pos - 8, 16);
    BYTE16(&data[6], pos - 8); /* len in header */

    pos += sac_gen_auth(&data[pos], data, pos, cc_data->sak);
    sac_crypt(&data[8], &data[8], pos - 8, cc_data->sek, AES_ENCRYPT);

    //        _hexdump("ENCRYPTED    ",data, pos);
    ca_write_apdu(d, tag, data, pos);

    return 1;
}

static int ci_ccmgr_cc_sac_data_req(ca_device_t *d, const uint8_t *data,
                                    unsigned int len) {
    struct cc_ctrl_data *cc_data = &d->private_data;
    uint32_t data_cnf_tag = 0x9f9008;
    uint8_t dest[2048];
    uint8_t tmp[len];
    int id_bitmask, dt_nr;
    unsigned int serial;
    int answ_len;
    int pos = 0;
    unsigned int rp = 0;

    if (len < 10)
        return 0;
    //_hexdump("ci_ccmgr_cc_sac_data_req:", data, len);

    memcpy(tmp, data, 8);
    sac_crypt(&tmp[8], &data[8], len - 8, cc_data->sek, AES_DECRYPT);
    data = tmp;

    if (!sac_check_auth(data, len, cc_data->sak)) {
        LOG("check_auth of message failed");
        return 0;
    }

    disable_cws_for_all_pmts(d);

    serial = UINT32(&data[rp], 4);
    LOGM("serial sac data req: %d", serial);

    /* skip serial & header */
    rp += 8;

    id_bitmask = data[rp++];

    /* handle data loop */
    dt_nr = data[rp++];
    rp += data_get_loop(d, cc_data, &data[rp], len - rp, dt_nr);

    if (len < rp + 1)
        return 0;

    dt_nr = data[rp++];

    /* create answer */
    pos += BYTE32(&dest[pos], serial);
    pos += BYTE32(&dest[pos], 0x01000000);

    dest[pos++] = id_bitmask;
    dest[pos++] = dt_nr; /* dt_nbr */

    answ_len = data_req_loop(d, &dest[pos], &data[rp], len - rp, dt_nr);
    if (answ_len <= 0) {
        LOG("cannot req data");
        return 0;
    }
    pos += answ_len;

    LOGM("SEND ------------ CC_SAC_DATA_CNF----------- ");
    //        _hexdump("sac_data_send", &dest[8], pos-8);  //skip serial and
    //        header
    return ci_ccmgr_cc_sac_send(d, data_cnf_tag, dest, pos);
}

static void ci_ccmgr_cc_sac_sync_req(ca_device_t *d, const uint8_t *data,
                                     unsigned int len) {
    int sync_cnf_tag = 0x9f9010;
    uint8_t dest[64];
    unsigned int serial;
    int pos = 0;

    //      hexdump("cc_sac_sync_req: ", (void *)data, len);

    serial = UINT32(data, 4);

    pos += BYTE32(&dest[pos], serial);
    pos += BYTE32(&dest[pos], 0x01000000);

    /* status OK */
    dest[pos++] = 0;

    LOG("SEND ------------ CC_SAC_SYNC_CNF----------- ");
    ci_ccmgr_cc_sac_send(d, sync_cnf_tag, dest, pos);
}

static void ci_ccmgr_cc_sync_req(ca_device_t *d, const uint8_t *data,
                                 unsigned int len) {
    const uint8_t status = 0x00; /* OK */
    LOGM("SEND ------------ CC_SYNC_CNF----------- ");
    ca_write_apdu(d, 0x9f9006, &status, 1);
}

static int ci_ccmgr_cc_data_req(ca_device_t *d, const uint8_t *data,
                                unsigned int len) {
    struct cc_ctrl_data *cc_data = &d->private_data;
    uint8_t dest[2048 * 2];
    int dt_nr;
    int id_bitmask;
    int answ_len;
    unsigned int rp = 0;

    if (len < 2)
        return 0;

    id_bitmask = data[rp++];

    /* handle data loop */
    dt_nr = data[rp++];

    // printf("CC_DATA:   ");
    // hexdump(cc_data, sizeof(cc_data));
    rp += data_get_loop(d, cc_data, &data[rp], len - rp, dt_nr);

    if (len < rp + 1)
        return 0;

    /* handle req_data loop */
    dt_nr = data[rp++];

    dest[0] = id_bitmask;
    dest[1] = dt_nr;

    answ_len = data_req_loop(d, &dest[2], &data[rp], len - rp, dt_nr);
    if (answ_len <= 0) {
        LOG("cannot req data");
        return 0;
    }

    answ_len += 2;

    LOGM("SEND ------------ CC_DATA_CNF----------- ");
    ca_write_apdu(d, 0x9f9004, dest, answ_len);

    return 1;
}

static int CIPLUS_APP_CC_create(ca_device_t *d, int session_number,
                                int resource_id) {
    /* CI Plus Implementation Guidelines V1.0.6 (2013-10)
5.3.1 URI version advertisement
A Host should advertise URI v1 only when Content Control v1 is selected
by the CICAM */
    if (resource_id == CIPLUS_APP_CC_RESOURCEID)
        d->uri_mask = 1;
    else
        d->uri_mask = 3;
    return 0;
}
static int CIPLUS_APP_CC_handler(ca_device_t *d, int tag, uint8_t *data,
                                 int len) {
    int session_number = d->session_number;

    LOG("RECV ciplus cc msg CAM%i, session_num %u, tag %x len %i dt_id %i",
        d->id, session_number, tag, len, data[2]);
    // printf(" RECV DATA:   ");
    // hexdump(data,len<33?len:32);

    switch (tag) {

    case CIPLUS_TAG_CC_OPEN_REQ: // 01
        ci_ccmgr_cc_open_cnf(d);
        break;
    case CIPLUS_TAG_CC_DATA_REQ: // 03
        ci_ccmgr_cc_data_req(d, data, len);
        break;
    case CIPLUS_TAG_CC_SYNC_REQ: // 05
        ci_ccmgr_cc_sync_req(d, data, len);
        break;
    case CIPLUS_TAG_CC_SAC_DATA_REQ: // 07
        ci_ccmgr_cc_sac_data_req(d, data, len);
        break;
    case CIPLUS_TAG_CC_SAC_SYNC_REQ: // 09
        ci_ccmgr_cc_sac_sync_req(d, data, len);
        break;
    default:
        LOG("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! unknown cc "
            "tag "
            "%x "
            "len %u",
            tag, len);
    }
    return 0;
}

static int CIPLUS_APP_LANG_handler(ca_device_t *d, int tag, uint8_t *data,
                                   int data_length) {
    LOG("host_lang&country_receive");
    //        if (data_length)
    //                hexdump(data, data_length);

    uint8_t data_reply_lang[3]; // ISO 639 Part 2
    data_reply_lang[0] = 0x65;  /* e */
    data_reply_lang[1] = 0x6e;  /* n */
    data_reply_lang[2] = 0x67;  /* g */

    uint8_t data_reply_country[3]; // ISO 3166-1 alpha 3
    data_reply_country[0] = 0x55;  /* U */
    data_reply_country[1] = 0x53;  /* S */
    data_reply_country[2] = 0x41;  /* A */

    switch (tag) {
    case CIPLUS_TAG_COUNTRY_ENQ: /* country enquiry */
    {
        LOG("country answered with '%c%c%c'", data_reply_country[0],
            data_reply_country[1], data_reply_country[2]);
        /* host country reply */
        ca_write_apdu(d, 0x9f8101, data_reply_country, 3);
        break;
    }
    case CIPLUS_TAG_LANG_ENQ: /* language enquiry */
    {
        LOG("language answered with '%c%c%c'", data_reply_lang[0],
            data_reply_lang[1], data_reply_lang[2]);
        /* host language reply */
        ca_write_apdu(d, 0x9f8111, data_reply_lang, 3);
        break;
    }
    default:
        LOG("unknown host lac apdu tag %02x", tag);
    }
    return 0;
}

static int CIPLUS_APP_SAS_handler(ca_device_t *d, int tag, uint8_t *data,
                                  int data_length) {
    hexdump("CIPLUS_APP_SAS_handler", data, data_length);

    switch (tag) {
    case CIPLUS_TAG_SAS_CONNECT_CNF: /* */
    {
        if (data[8] == 0) {
            ca_write_apdu(d, 0x9f9a07, 0x00, 0);
        }
        break;
    }
    default:
        LOG("unknown SAS apdu tag %03x", tag);
    }

    return 0;
} /* not working, just for fun */

static int CIPLUS_APP_OPRF_handler(ca_device_t *d, int tag, uint8_t *data,
                                   int data_length) {
    char buf[400];
    int pos, i;
    pos = sprintf(buf, " ");
    hexdump("CIPLUS_APP_OPRF_handler", data, data_length);

    uint8_t data_oprf_search[9];
    data_oprf_search[0] = 0x03; /* unattended mode bit=0 + length in bytes
                                   of the service types */
    data_oprf_search[1] = 0x01; /* service MPEG-2 television (0x01) */
    data_oprf_search[2] = 0x16; /* service h264 SD (0x16) */
    data_oprf_search[3] = 0x19; /* service h264 HD (0x19) */
    data_oprf_search[4] = 0x02; /* length in bytes of the delivery_capability */
    data_oprf_search[5] = 0x43; /* DVB-S */
    data_oprf_search[6] = 0x79; /* DVB-S2 */
    data_oprf_search[7] =
        0x01; /* length in bytes of the application_capability */
    data_oprf_search[8] = 0x00; /* System Software Update service */

    uint8_t data_oprf_tune_status[64];
    data_oprf_tune_status[0] = 0x01; // unprocessed descriptor_number
    data_oprf_tune_status[1] = 0x50; // signal strength
    data_oprf_tune_status[2] = 0x50; // signal quality
    data_oprf_tune_status[3] = 0x00; // status 0 - OK
    data_oprf_tune_status[4] = 0x0d; // lenght next part (13 bytes)

    switch (tag) {
    case CIPLUS_TAG_OPERATOR_STATUS: /* operator_status 01 */
    {
        LOG("CAM_OPRF_operator_status_receive");
        uint8_t tag_part = 0x04; /* operator_info_req */
        if (data[1] & 0x20)      // initialised_flag - profile initialised
            tag_part = 0x08;     /* operator_exit */
        if (data[1] & 0x40) {
            LOG("CI+ CA%d: operator profile %s initialised", d->id,
                data[1] & 0x60 ? "" : "NOT ");
        } else
            LOG("CI+ CA%d: operator profile disabled", d->id);
        ca_write_apdu(d, 0x9f9c00 | tag_part, 0x00, 0);
        break;
    }
    case CIPLUS_TAG_OPERATOR_INFO: /* operator_info */
    {
        LOG("CAM_OPRF_operator_info_receive");
        ca_write_apdu(d, 0x9f9c06, data_oprf_search, 9);
        break;
    }
    case 0x9f9c03: /* operator_nit */
    {
        LOG("CAM_OPRF_operator_nit_receive");
        /* operator_exit */
        ca_write_apdu(d, 0x9f9c08, 0x00, 0);
        break;
    }
    case CIPLUS_TAG_OPERATOR_SEARCH_STATUS: /* operator_search_status */
    {
        LOG("CAM_OPRF_operator_search_status_receive");

        if (data[1] & 0x02) // refresh_request_flag == 2 (urgent request)
        {
            /* operator_search_start */
            ca_write_apdu(d, 0x9f9c06, data_oprf_search, 9);
        } else /* operator_nit */
            ca_write_apdu(d, 0x9f9c02, 0x00, 0);
        break;
    }
    case CIPLUS_TAG_OPERATOR_TUNE: /* operator_tune */
    {
        char *pol = "H";
        if (data[5] & 0x20)
            pol = "V";
        else if (data[5] & 0x40)
            pol = "L";
        else if (data[5] & 0x60)
            pol = "R";
        LOG("CAM_OPRF_operator_tune_receive");
        for (i = 0; i < data_length; i++) {
            pos += sprintf(buf + pos, "%02X ", data[i]);
            data_oprf_tune_status[i + 5] = data[i + 2];
        }
        LOG("CI+ CA%d: %s", d->id, buf);
        LOG("Please TUNE to transponder %x%x%x %c", data[2], data[3], data[4],
            *pol);
        // data_oprf_tune_status[13]=0xC6; //psk8 dvb-s2
        sleep_msec(3 * 1000); // wait 3 secs
        ca_write_apdu(d, 0x9f9c0a, data_oprf_tune_status, 18);
        break;
    }
    default:
        LOG("unknown OPRF apdu tag %03x", tag);
    }
    return 0;
}

static int CIPLUS_APP_UPGR_handler(ca_device_t *d, int tag, uint8_t *data,
                                   int data_length) {
    int fd = d->fd;
    ca_slot_info_t info;

    const uint8_t answer = 0x00; // 0x00 - mean no upgrade, 0x01 - upgrade,
                                 // 0x02 - ask user by mmi

    LOG("CAM_fw_upgrade_receive");

    switch (tag) {
    case CIPLUS_TAG_FIRMWARE_UPGR: /* */
    {
        LOG("CI+ CA%i Firmware Upgrade Command detected... ", d->id);
        /* cam firmware update reply */
        ca_write_apdu(d, 0x9f9d02, &answer, 1);
        break;
    }
    case CIPLUS_TAG_FIRMWARE_UPGR_PRGRS: /* */
    {
        LOG("CI+ CA%i Firmware Upgrade Progress %i percents", d->id, data[4]);
        break;
    }
    case CIPLUS_TAG_FIRMWARE_UPGR_COMPLT: /* */
    {
        LOG("CI+ CA%i Firmware Upgrade Complete (reset status %i)", d->id,
            data[4]);
        if (data[4] < 2) { // reset requred
            if (ioctl(fd, CA_RESET, &info))
                LOG_AND_RETURN(0, "%s: Could not reset ca %d", __FUNCTION__,
                               d->id);
            return 1;
        }
        break;
    }
    default:
        LOG("unknown fw upgrade apdu tag %03x", tag);
    }
    return 0;
} /* works now, be careful!!! just in case upgrade disabled */

static int ca_send_datetime(ca_device_t *d) {
    unsigned char msg[6];
    time_t tv = time(NULL);
    uint16_t mjd = tv / 86400 + 40587; // mjd 01.01.1970 is 40587
    tv %= 86400;
    uint8_t hh = tv / 3600;
    tv %= 3600;
    uint8_t mm = tv / 60;
    tv %= 60;
    uint8_t ss = tv;

    msg[0] = 5; // not using offset
    msg[1] = (mjd >> 8) & 0xff;
    msg[2] = mjd & 0xff;
    msg[3] = ((hh / 10) << 4) | (hh % 10);
    msg[4] = ((mm / 10) << 4) | (mm % 10);
    msg[5] = ((ss / 10) << 4) | (ss % 10);
    int session_number =
        find_session_for_resource(EN50221_APP_DATETIME_RESOURCEID);
    ca_write_apdu_session(d, session_number, TAG_DATE_TIME, msg, 6);
    d->datetime_next_send = tv + d->datetime_response_interval;
    LOG("Sending time to CA %d, response interval: %d", d->id,
        d->datetime_response_interval)
    return 0;
}

int APP_empty(struct ca_device *d, int resource, uint8_t *buffer, int len) {
    LOG("Got unhandled resource %06X", resource);
    return 0;
}

static int APP_RM_create(ca_device_t *d, int session_number, int resource_id) {
    LOG("--------------------S_SCALLBACK_REASON_CAMCONNECTED---------"
        "APP_RM_RESOURCEID-------------------------");
    ca_write_apdu(d, TAG_PROFILE_ENQUIRY, NULL, 0);
    return 0;
}

int APP_RM_handler(struct ca_device *d, int resource, uint8_t *buffer,
                   int len) {
    int resource_ids[100], rlen;
    switch (resource) {
    case TAG_PROFILE:
        ca_write_apdu(d, TAG_PROFILE_CHANGE, NULL, 0);
        break;
    case TAG_PROFILE_ENQUIRY:
        // do we really need this to force CI only mode ?
        rlen = populate_resources(d, resource_ids);

        ca_write_apdu(d, TAG_PROFILE, resource_ids, rlen * 4);
        break;
    default:
        LOG("unexpected tag in ResourceManagerHandle (0x%x)", resource);
    }
    return 0;
}

static int APP_AI_create(ca_device_t *d, int session_number, int resource_id) {
    int ai_version = resource_id & 0x3f;
    LOG("%s: CAM requested version %d of the Application Information resource",
        __FUNCTION__, ai_version);

    // Versions 1 and 2 of the Application Information resource only
    // expects us to make an inquiry
    ca_write_apdu(d, TAG_APP_INFO_ENQUIRY, NULL, 0);

    // Announce 96 Mbps data rate support to CAMs implementing version
    // 3 or newer of the Application Information resource
    if (ai_version >= 3) {
        ciplus13_app_ai_data_rate_info(d, CIPLUS_DATA_RATE_96_MBPS);
    }
    return 0;
}

static int CIPLUS_APP_AI_handler(ca_device_t *d, int resource_id, uint8_t *data,
                                 int data_length) {
    int session_number = d->session_number;

    LOG("RECV ciplus AI msg CA %u, session_num %u, resource_id %x", d->id,
        session_number, resource_id);

    switch (resource_id) {
    case CIPLUS_TAG_APP_INFO:

        hexdump("CIPLUS_TAG_APP_INFO", data, data_length);
        LOG("  Application type: %02x", data[0]);
        LOG("  Application manufacturer: %04X", (data[2] << 8) | data[1]);
        LOG("  Manufacturer code: %04X", (data[4] << 8) | data[3]);

        uint8_t dl = data[5];
        if ((dl + 6) > data_length) {
            dl = data_length - 6;
        }
        memcpy(d->ci_name, data + 6, dl);
        if (dl > 0)
            d->ci_name[dl] = '\0'; // NOSONAR
        LOG("  Menu string: %s (%d)", d->ci_name, dl);

        break;
    case CIPLUS_TAG_CICAM_RESET:
        hexdump("CIPLUS_TAG_CICAM_RESET", data, data_length);
        LOG("CA %d Reset requested", d->id);
        // TODO: close the CA device
        break;
    default:
        LOG("unexpected tag in %s (0x%x)", __FUNCTION__, resource_id);
    }
    return 0;
}

static int APP_CA_create(ca_device_t *d, int session_number, int resource_id) {
    LOG("%s", __FUNCTION__);
    ca_write_apdu(d, TAG_CA_INFO_ENQUIRY, NULL, 0);
    return 0;
}

int APP_CA_handler(struct ca_device *d, int resource, uint8_t *data, int len) {
    int i, overwritten = 0, caid_count = len / 2;

    switch (resource) {
    case TAG_CA_INFO:
        d->init_ok = 1;
        if (d->caids) {
            data = (uint8_t *)d->caid;
            caid_count = d->caids;
            overwritten = 1;
        }
        for (i = 0; i < caid_count; i++) {
            int caid = (data[i * 2 + 0] << 8) | data[i * 2 + 1];
            LOG("   %s CA ID: %04X for CA%d",
                overwritten ? "Forced" : "Supported", caid, d->id);
            add_caid_mask(dvbca_id, d->id, caid, 0xFFFF);
        }
        break;
    default:
        LOG("%s: unexpected tag (0x%x)", __FUNCTION__, resource);
    }
    return 0;
}

int APP_DateTime_handler(struct ca_device *d, int resource, uint8_t *buffer,
                         int len) {
    switch (resource) {
    case TAG_DATE_TIME_ENQUIRY:
        if (buffer)
            d->datetime_response_interval = *buffer;
        break;
    default:
        LOG("unexpected tag in %s (0x%x)", __FUNCTION__, resource);
    }
    ca_send_datetime(d);
    return 0;
}

int ca_app_mmi_answ(ca_device_t *d, uint8_t answ_id, uint8_t *text,
                    uint32_t text_count) {
    uint8_t buf[10 + text_count];
    int len = 1 + text_count;

    buf[0] = answ_id;

    if (text_count > 0) {
        memcpy(buf + 1, text, text_count);
    }
    return ca_write_apdu(d, TAG_ANSWER, buf, len);
}

int ca_app_mmi_menu_answ(ca_device_t *d, uint8_t answ_id) {
    return ca_write_apdu(d, TAG_MENU_ANSWER, &answ_id, 1);
}

int ca_app_mmi_close(ca_device_t *d, uint8_t answ_id) {
    return ca_write_apdu(d, TAG_CLOSE_MMI, &answ_id, 1);
}

int APP_MMI_handler(struct ca_device *d, int resource, uint8_t *buffer,
                    int len) {
    hexdump("Got MMI: ", buffer, len);
    switch (resource) {
    case TAG_DISPLAY_CONTROL: {
        int cmd_id = buffer[0];
        int mmi_mode = buffer[1];
        if (cmd_id == MMI_DISPLAY_CONTROL_CMD_ID_SET_MMI_MODE) {

            LOG("mmi display ctl cb received for CA %u session_num %u "
                "cmd_id 0x%02x mmi_mode %u",
                d->id, d->session_number, cmd_id, mmi_mode);
            uint8_t data[] = {MMI_DISPLAY_REPLY_ID_MMI_MODE_ACK, mmi_mode};
            ca_write_apdu(d, TAG_DISPLAY_REPLY, data, sizeof(data));
        }
        break;
    }
    case TAG_ENQUIRY: {

        // extract the information
        uint8_t blind_answer = (buffer[0] & 0x01) ? 1 : 0;
        uint8_t answer_length = buffer[1];
        uint8_t *text = buffer + 2;

        char buffer[256];

        snprintf(buffer, sizeof(buffer), "%.*s", len - 2, text);

        LOG("MMI enquiry from CAM in CA %u:  %s (%s%u digits)", d->id, text,
            blind_answer ? "blind " : "", answer_length);

        if (strlen((char *)d->pin_str) == answer_length) {
            LOG("answering to PIN enquiry");
            ca_app_mmi_answ(d, MMI_ANSW_ID_ANSWER, (uint8_t *)d->pin_str,
                            answer_length);
        }

        ca_app_mmi_close(d, MMI_CLOSE_MMI_CMD_ID_IMMEDIATE);

        break;
    }
    case TAG_LIST_LAST:
    case TAG_MENU_LAST: {
        int i;
        uint8_t *data = buffer;
        uint8_t *max = data + len;
        if (data > max)
            break;
        int n = *data++;
        if (n == 0xFF)
            n = 0;
        else
            n++;
        LOG("MMI menu from CAM in the slot %u: %d items", d->id, n);
        for (i = 0; i < (n + 3); ++i) {
            int textlen;
            if ((data + 3) > max)
                break;
            DEBUGM("[UI] text tag: %02x %02x %02x", data[0], data[1], data[2]);
            data += 3;
            data += asn_1_decode(&textlen, data);
            DEBUGM("[UI] %d bytes text", textlen);
            if ((data + textlen) > max)
                break;
            char str[textlen + 1];
            memcpy(str, data, textlen);
            str[textlen] = '\0';
            LOG("[UI] %s", str);
            data += textlen;
        }
        ca_app_mmi_menu_answ(d, 0x01);
        /* cancel menu */
        ca_app_mmi_close(d, MMI_CLOSE_MMI_CMD_ID_IMMEDIATE);

        break;
    }
    default:
        LOG("%s: CA %d unknown tag resource %06X", __FUNCTION__, d->id,
            resource);
    }

    return 0;
}

static int APP_SAS_create(ca_device_t *d, int session_number, int resource_id) {
    LOG("%s-------------------------", __FUNCTION__);
    uint8_t data[] = {0x69, 0x74, 0x64, 0x74, 0x74, 0x63, 0x61, 0x00};
    // private_Host_application_ID
    ca_write_apdu(d, 0x9f9a00, data, sizeof(data));
    return 0;
}

static int APP_LANG_create(ca_device_t *d, int session_number,
                           int resource_id) {
    uint8_t data_reply_lang[] = {0x65, 0x6e, 0x67};    // eng
    uint8_t data_reply_country[] = {0x55, 0x53, 0x41}; // USA
    LOG("%s: Sending language to the CAM", __FUNCTION__);
    ca_write_apdu(d, 0x9F8101, data_reply_country, sizeof(data_reply_country));
    ca_write_apdu(d, 0x9F8111, data_reply_lang, sizeof(data_reply_lang));
    return 0;
}

#define DEFAPP(a, b, c)                                                        \
    { .resource = a, .name = #a, .callback = b, .create = c }
// this contains all known resource ids so we can see if the cam asks for
// something exotic
struct struct_application_handler application_handler[] = {
    DEFAPP(0, NULL, NULL), // 0 is reserved
    // Resource Manager
    DEFAPP(EN50221_APP_RM_RESOURCEID, APP_RM_handler, APP_RM_create),
    DEFAPP(TS101699_APP_RM_RESOURCEID, APP_RM_handler, APP_RM_create),
    // Application Information
    DEFAPP(EN50221_APP_AI_RESOURCEID, CIPLUS_APP_AI_handler, APP_AI_create),
    DEFAPP(TS101699_APP_AI_RESOURCEID, CIPLUS_APP_AI_handler, APP_AI_create),
    DEFAPP(CIPLUS_APP_AI_RESOURCEID, CIPLUS_APP_AI_handler, APP_AI_create),
    // Conditional Access Support
    DEFAPP(EN50221_APP_CA_RESOURCEID, APP_CA_handler, APP_CA_create),
    // TS103205_APP_CA_MULTISTREAM_RESOURCEID, // Multi-stream
    // Host Control - Does not seem like other apps are annoucing it
    DEFAPP(EN50221_APP_DVB_RESOURCEID, APP_empty, NULL),
    DEFAPP(CIPLUS_APP_DVB_RESOURCEID, APP_empty, NULL),
    DEFAPP(TS103205_APP_DVB_RESOURCEID, APP_empty, NULL),
    // TS103205_APP_DVB_MULTISTREAM_RESOURCEID, // Multi-stream
    // Date-Time
    DEFAPP(EN50221_APP_DATETIME_RESOURCEID, APP_DateTime_handler, NULL),
    // MMI
    DEFAPP(EN50221_APP_MMI_RESOURCEID, APP_MMI_handler, NULL),
    // TS103205_APP_MMI_RESOURCEID, // Multi-stream
    // Low Speed Communication is not supported
    // Content Control
    DEFAPP(CIPLUS_APP_CC_RESOURCEID, CIPLUS_APP_CC_handler,
           CIPLUS_APP_CC_create),
    DEFAPP(CIPLUS_APP_CC_RESOURCEID_TWO, CIPLUS_APP_CC_handler,
           CIPLUS_APP_CC_create),
    DEFAPP(TS103205_APP_CC_RESOURCEID_THREE, CIPLUS_APP_CC_handler,
           CIPLUS_APP_CC_create),
    // DEFAPP(TS103205_APP_CC_MULTISTREAM_RESOURCEID, CIPLUS_APP_CC_handler,
    // CIPLUS_APP_CC_create), // Multi-stream
    // Host Lang & Country
    DEFAPP(CIPLUS_APP_LANG_RESOURCEID, CIPLUS_APP_LANG_handler,
           APP_LANG_create),
    // CICAM Upgrade
    DEFAPP(CIPLUS_APP_UPGR_RESOURCEID, CIPLUS_APP_UPGR_handler, NULL),
    // Operator Profile
    DEFAPP(CIPLUS_APP_OPRF_RESOURCEID, CIPLUS_APP_OPRF_handler, NULL),
    DEFAPP(TS103205_APP_OPRF_TWO_RESOURCEID, CIPLUS_APP_OPRF_handler, NULL),
    DEFAPP(TS103205_APP_OPRF_THREE_RESOURCEID, CIPLUS_APP_OPRF_handler, NULL),
    // SAS
    DEFAPP(CIPLUS_APP_SAS_RESOURCEID, CIPLUS_APP_SAS_handler, APP_SAS_create),
    // Application MMI
    DEFAPP(TS101699_APP_AMMI_RESOURCEID, APP_empty, NULL),
    DEFAPP(CIPLUS_APP_AMMI_RESOURCEID, APP_empty, NULL),
    // TS103205_APP_AMMI_RESOURCEID, // Multi-stream
    // Multi-stream capability,
    // TS103205_APP_MULTISTREAM_RESOURCEID
};

int find_session_for_resource(int resource) {
    int i;
    for (i = 1;
         i < sizeof(application_handler) / sizeof(application_handler[0]); i++)
        if (resource == application_handler[i].resource)
            return i;
    return -1;
}

int populate_resources(ca_device_t *d, int *resource_ids) {
    int i;
    for (i = 1;
         i < sizeof(application_handler) / sizeof(application_handler[0]);
         i++) {
        int resource = application_handler[i].resource;
        if (d->force_ci && resource == CIPLUS_APP_CC_RESOURCEID) {
            break;
        }
        resource_ids[i - 1] = htonl(resource);
    }
    return i - 1;
}

/* from dvb-apps */
int asn_1_decode(int *length, unsigned char *asn_1_array) {
    uint8_t length_field;

    length_field = asn_1_array[0];

    if (length_field < 0x80) {
        // there is only one word
        *length = length_field & 0x7f;
        return 1;
    } else if (length_field == 0x81) {
        *length = asn_1_array[1];
        return 2;
    } else if (length_field == 0x82) {
        *length = (asn_1_array[1] << 8) | asn_1_array[2];
        return 3;
    }

    return -1;
}

int asn_1_encode(int length, uint8_t *asn_1_array) {
    if (length < 0x80) {
        asn_1_array[0] = length & 0x7f;
        return 1;
    } else if (length < 0x100) {
        asn_1_array[0] = 0x81;
        asn_1_array[1] = length;
        return 2;
    } else {
        asn_1_array[0] = 0x82;
        asn_1_array[1] = length >> 8;
        asn_1_array[2] = length;
        return 3;
    }

    // never reached
}

// Enigma handles only session data, that's why the method just writes the SPDU
// to the CA device For dvbca codepath, this wraps the session data into a TPDU
// and sends it to the device.
int ca_write_tpdu(ca_device_t *d, int tag, uint8_t *buf, int len) {
#ifdef ENIGMA

    int written = write(d->fd, buf, len);
    if (written != len) {
        LOG("incomplete write to CA %d fd %d, expected %d got %d, errno %d",
            d->id, d->fd, len, written, errno);
        return 1;
    }
    return 0;
#else
    uint8_t p_data[10 + len];
    int i_size;
    int connection_id = d->slot_id + 1;

    i_size = 0;
    p_data[0] = d->slot_id;
    p_data[1] = connection_id;
    p_data[2] = tag;

    switch (tag) {
    case T_RCV:
    case T_CREATE_TC:
    case T_CTC_REPLY:
    case T_DELETE_TC:
    case T_DTC_REPLY:
    case T_REQUEST_TC:
        p_data[3] = 1; /* length */
        p_data[4] = connection_id;
        i_size = 5;
        break;

    case T_NEW_TC:
    case T_TC_ERROR:
        p_data[3] = 2; /* length */
        p_data[4] = connection_id;
        p_data[5] = buf[0];
        i_size = 6;
        break;

    case T_DATA_LAST:
    case T_DATA_MORE: {
        /* len <= MAX_TPDU_DATA */
        uint8_t *p = p_data + 3;
        p += asn_1_encode(len + 1, p);
        *p++ = connection_id;
        if (len > 0) {
            memcpy(p, buf, len);
            p += len;
        }

        i_size = p - p_data;
        break;
    }

    default:
        break;
    }

    if (tag != T_RCV || buf != NULL) {
        _hexdump("Writing to CA: ", p_data, i_size);
    }

    int written = write(d->fd, p_data, i_size);
    if (written != i_size) {
        LOG("incomplete write to CA %d fd %d, expected %d got %d, errno %d", d->id, d->fd,
            i_size, written, errno);
        return 1;
    }
    return 0;

#endif
}

// writes session data to the CAM.
int ca_write_spdu(ca_device_t *d, int session_number, unsigned char tag,
                  const void *data, int len, const void *apdu, int alen) {
    unsigned char pkt[4096];
    unsigned char *ptr = pkt;
    memset(pkt, 0xFF, sizeof(pkt));
    *ptr++ = tag;
    ptr += asn_1_encode(len + 2, ptr);
    if (data)
        memcpy(ptr, data, len);
    ptr += len;
    *ptr++ = session_number >> 8;
    *ptr++ = session_number;

    if (apdu)
        memcpy(ptr, apdu, alen);

    ptr += alen;

    LOG("%s: CA %d, session %d, name %s, write tag %02X, data length %d",
        __FUNCTION__, d->id, session_number,
        application_handler[session_number].name, tag, ptr - pkt);
    if (ptr > pkt)
        _hexdump("Session data: ", pkt, ptr - pkt);
    return ca_write_tpdu(d, T_DATA_LAST, pkt, ptr - pkt);
}

// write an APDU using the current session
int ca_write_apdu_session(ca_device_t *d, int session_number, int tag,
                          const void *data, int len) {
    unsigned char pkt[len + 3 + 4];
    int l;

    pkt[0] = (tag >> 16) & 0xFF;
    pkt[1] = (tag >> 8) & 0xFF;
    pkt[2] = tag & 0xFF;

    l = asn_1_encode(len, pkt + 3);
    if (data)
        memcpy(pkt + 3 + l, data, len);
    LOG("ca_write_apdu: CA %d, session %d, name %s, write tag %06X, data "
        "length %d",
        d->id, session_number, application_handler[session_number].name, tag,
        len);
    return ca_write_spdu(d, session_number, ST_SESSION_NUMBER, 0, 0, pkt,
                         len + 3 + l);
}

int ca_write_apdu(ca_device_t *d, int resource, const void *data, int len) {
    return ca_write_apdu_session(d, d->session_number, resource, data, len);
}
// reads a TPDU from the CAM. This is called only on DVBCA path
int ca_read_tpdu(int socket, void *buf, int buf_len, sockets *ss, int *rb) {
    static int i;
    ca_device_t *c = ca_devices[ss->sid];
    i += 1;
    *rb = 0;
    uint8_t data[4096];
    int len;
    unsigned char *d;
    len = read(c->fd, data, sizeof(data));
    if (len > 0 && !((len == 6) && data[5] == 0)) {
        _hexdump("READ TPDU: ", data, len);
    }
    d = data;
    /* taken from the dvb-apps */
    int data_length = len - 2;
    d += 2; /* remove leading slot and connection id */
    while (data_length > 0) {
        unsigned char tpdu_tag = d[0];
        int asn_data_length;
        int length_field_len;
        if ((length_field_len = asn_1_decode(&asn_data_length, d + 1)) < 0) {
            LOG("Received data with invalid asn from module on device %d",
                c->id);
            break;
        }

        if ((asn_data_length < 1) ||
            (asn_data_length > (data_length - (1 + length_field_len)))) {
            LOG("Received data with invalid length from module on device "
                "%d",
                c->id);
            break;
        }

        if (asn_data_length > *rb + buf_len) {
            LOG("Received data for CA %d larger than socket buffer (len "
                "%d)",
                c->id, *rb + buf_len);
            break;
        }

        d += 1 + length_field_len + 1;
        data_length -= (1 + length_field_len + 1);
        asn_data_length--;

        switch (tpdu_tag) {
        case T_CTC_REPLY:
            LOG("Got CTC Replay (slot %d)", c->slot_id);
            ca_write_tpdu(c, T_DATA_LAST, NULL, 0);

            break;
        case T_DELETE_TC:
            LOG("Got \"Delete Transport Connection\" from module ");
            return 0; // this will reset the module
            break;
        case T_DTC_REPLY:
            LOG("Got \"Delete Transport Connection Replay\" from "
                "module!\n");
            break;
        case T_REQUEST_TC:
            LOG("Got \"Request Transport Connection\" from Module "
                "->currently not handled!");
            break;
        case T_DATA_MORE:
        case T_DATA_LAST:
            memcpy(buf + *rb, d, asn_data_length);
            *rb += asn_data_length;
            break;
        case T_SB: {
            if (d[0] & 0x80) {
                LOGM("->data ready (%d)\n", c->slot_id);
                // send the RCV and ask for the data
                ca_write_tpdu(c, T_RCV, NULL, 0);
            }
            break;
        }
        default:
            printf("unhandled tpdu_tag 0x%0x\n", tpdu_tag);
        }
        // skip over the consumed data
        d += asn_data_length;
        data_length -= asn_data_length;
    } // while (data_length)
    return 1;
}

// Session data contains the application data and the session_id for that
// application
// this function reads multiple APDUs for the same session and calls the
// application handler for each
int ca_read_apdu(ca_device_t *d, uint8_t *buf, int buf_len) {
    int i = 0;
    uint8_t *data = buf;
    int llen, len;
    int tag;
    int session_number = d->session_number;
    _hexdump("APDU", buf, buf_len);
    while (i < buf_len) {
        data = buf + i;
        llen = asn_1_decode(&len, data + 3);
        tag = (data[0] << 16) | (data[1] << 8) | data[2];
        // data points to the actual APDU data
        data += 3 + llen;
        LOG("%s: CA %d, session %d, name %s, read tag %06X, data length %d",
            __FUNCTION__, d->id, session_number,
            application_handler[session_number].name, tag, len);

        if (len > 0)
            _hexdump("data: ", data, len);

        application_handler[d->session_number].callback(d, tag, data, len);

        i += 3 + llen + len;
    }
    return 0;
}

// Reads session data. Session data is provided by both enigma and DVBCA
// code path
int ca_read(sockets *s) {
    unsigned char *data = s->buf;
    uint32_t resource_identifier;
    int session_id = -1;
    ca_device_t *d = ca_devices[s->sid];
    int len, status = 0;
    _hexdump("CAREAD:   ", data, s->rlen);
    int tag = data[0];
    int llen = asn_1_decode(&len, data + 1);
    int i = 1 + llen + len;

    switch (tag) {
    case ST_OPEN_SESSION_REQUEST:
        copy32r(resource_identifier, data, 2);
        session_id = find_session_for_resource(resource_identifier);
        status = 0;
        if (session_id == -1)
            session_id = 0xF0;
        char pkt[6];
        pkt[0] = status;
        memcpy(pkt + 1, data + 2, 4);
        d->session_number = session_id;
        ca_write_spdu(d, session_id, ST_OPEN_SESSION_RESPONSE, pkt, 5, NULL, 0);
        if (application_handler[session_id].create)
            application_handler[session_id].create(d, session_id,
                                                   resource_identifier);

        break;

    case ST_CLOSE_SESSION_REQUEST:
        // closing the CI session
        copy16r(resource_identifier, data, 2);
        int ca_session_number =
            find_session_for_resource(EN50221_APP_CA_RESOURCEID);
        if (resource_identifier == ca_session_number) {
            d->ignore_close = 1;
            d->init_ok = 0;
        }
        LOG("Received close session %s",
            application_handler[resource_identifier].name);
        break;
    case ST_SESSION_NUMBER:
        copy16r(d->session_number, data, 1 + llen);
        LOG("got ST_SESSION_NUMBER for session_id %d", d->session_number);
        ca_read_apdu(d, data + i, s->rlen - i);
        break;
    }

    s->rlen = 0;

    // Send regular date/time updates to the CAM
    if (d->datetime_response_interval && time(NULL) > d->datetime_next_send) {
        ca_send_datetime(d);
    }

    return 0;
}

int ca_close(sockets *s) {
    LOG("requested ca close for sock %d, sock_id %d", s->id, s->sock);
    return 0;
}

int ca_timeout(sockets *s) {
    ca_device_t *d = ca_devices[s->sid];
    ca_write_tpdu(d, T_RCV, NULL, 0);
    return 0;
}

// reads session data on enigma devices. Handles specific issues (such as
// reading 0 bytes) and adds only the session data to sockets buffer
int ca_read_enigma(int socket, void *buf, int len, sockets *ss, int *rb) {
    static int i;
    i += 1;
    int rl = read(socket, buf, len);
    *rb = 0;
    if (rl > 0) {
        *rb = rl;
    }
    if (0 && ss->revents && POLLPRI) {
        LOG("Got POLLPRI for sock %d, sock_id %d, closing CI", ss->id,
            ss->sock);
        return 0;
    }
    return 1;
}

// initializes the enigma CA
int ca_init_enigma(ca_device_t *d) {
    int fd = d->fd;
    d->is_ciplus = 0;

    ioctl(fd, 0);

    d->sock = sockets_add(fd, NULL, d->id, TYPE_TCP, (socket_action)ca_read,
                          (socket_action)ca_close, (socket_action)NULL);
    if (d->sock < 0)
        LOG_AND_RETURN(0, "%s: sockets_add failed", __FUNCTION__);

    sockets_setread(d->sock, ca_read_enigma);
    return 0;
}

// initializes the dvbca devices
int ca_init_en50221(ca_device_t *d) {
    ca_slot_info_t info;
    int64_t st = getTick();
    __attribute__((unused)) int tries = 800; // wait up to 8s for the CAM
    int fd = d->fd;
    d->slot_id = -1;
    d->is_ciplus = 0;
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, CA_RESET, &info))
        LOG_AND_RETURN(0, "%s: Could not reset ca %d", __FUNCTION__, d->id);

    do {
        if (ioctl(fd, CA_GET_SLOT_INFO, &info))
            LOG_AND_RETURN(0, "%s: Could not get info1 for ca %d", __FUNCTION__,
                           d->id);
        if (info.flags & CA_CI_MODULE_READY)
            break;
        sleep_msec(10);
    } while (tries-- > 0);

    if (ioctl(fd, CA_GET_SLOT_INFO, &info))
        LOG_AND_RETURN(0, "%s: Could not get info2 for ca %d, tries %d",
                       __FUNCTION__, d->id, tries);

    LOG("initializing CA, fd %d type %d flags 0x%x, after %jd ms", fd,
        info.type, info.flags, (getTick() - st));

    if (info.type != CA_CI_LINK) {
        LOG("incompatible CA interface");
        goto fail;
    }

    if (!(info.flags & CA_CI_MODULE_READY)) {
        LOG("CA module not present or not ready");
        goto fail;
    }
    d->slot_id = 0;
    ca_write_tpdu(d, T_CREATE_TC, NULL, 0);

    d->sock = sockets_add(fd, NULL, d->id, TYPE_TCP, (socket_action)ca_read,
                          (socket_action)ca_close, (socket_action)ca_timeout);
    if (d->sock < 0)
        LOG_AND_RETURN(0, "%s: sockets_add failed", __FUNCTION__);
    sockets_timeout(d->sock, 1000);
    sockets_setread(d->sock, ca_read_tpdu);

    return 0;
fail:
    close(fd);
    d->fd = -1;
    d->enabled = 0;
    return 1;
}

ca_device_t *alloc_ca_device() {
    ca_device_t *d = malloc1(sizeof(ca_device_t));
    if (!d) {
        LOG_AND_RETURN(NULL, "Could not allocate memory for CA device");
    }
    memset(d, 0, sizeof(ca_device_t));
    d->max_ca_pmt = MAX_CA_PMT;
    return d;
}

void flush_handle(int fd) {
    uint8_t buf[1024];
    int flags = fcntl(fd, F_GETFL);
    if (!(flags & O_NONBLOCK) && (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1))
        LOG("Failed to set the handle %d as non blocking", fd);

    while (read(fd, buf, 256) > 0)
        ;
    if (!(flags & O_NONBLOCK) && (fcntl(fd, F_SETFL, flags) == -1))
        LOG("Failed to set the handle %d as blocking", fd);
}
int dvbca_init_dev(adapter *ad) {
    ca_device_t *c = ca_devices[ad->id];
    int fd;
    char ca_dev_path[100];

    if (c && c->enabled)
        return TABLES_RESULT_OK;

    if (ad->type != ADAPTER_DVB && ad->type != ADAPTER_CI)
        return TABLES_RESULT_ERROR_NORETRY;
#ifdef ENIGMA
    sprintf(ca_dev_path, "/dev/ci%d", ad->pa);
#else
    sprintf(ca_dev_path, "/dev/dvb/adapter%d/ca%d", ad->pa, ad->fn);
#endif
    fd = open(ca_dev_path, O_RDWR);
    if (fd < 0)
        LOG_AND_RETURN(TABLES_RESULT_ERROR_NORETRY,
                       "No CA device detected on adapter %d: file %s", ad->id,
                       ca_dev_path);
    flush_handle(fd);
    if (!c) {
        c = ca_devices[ad->id] = alloc_ca_device();
        if (!c) {
            close(fd);
            LOG_AND_RETURN(0, "Could not allocate memory for CA device %d",
                           ad->id);
        }
    }
    c->enabled = 1;
    c->ignore_close = 0;
    c->fd = fd;
    c->id = ad->id;
    c->slot_id = ad->pa;
    c->init_ok = 0;
    c->is_active = 0;

    memset(c->capmt, -1, sizeof(c->capmt));
    memset(c->key[0], 0, sizeof(c->key[0]));
    memset(c->key[1], 0, sizeof(c->key[1]));
    memset(c->iv[0], 0, sizeof(c->iv[0]));
    memset(c->iv[1], 0, sizeof(c->iv[1]));

    if (!c->force_ci) {
        if (access("/etc/ssl/certs/root.pem", F_OK) != 0) {
            c->force_ci = 0;
        }
    }
#ifdef ENIGMA
    if (ca_init_enigma(c)) {
#else
    if (ca_init_en50221(c)) {
#endif
        dvbca_close_device(c);
        return TABLES_RESULT_ERROR_NORETRY;
    }
    return TABLES_RESULT_OK;
}

int dvbca_close_device(ca_device_t *c) {
    LOG("closing CA device %d, fd %d", c->id, c->fd);
    c->enabled = 0;
    // cleanup
    if (c->fd >= 0)
        close(c->fd);
    EVP_cleanup();
    ERR_free_strings();
    return 0;
}
int dvbca_close_dev(adapter *ad) {
    ca_device_t *c = ca_devices[ad->id];
    if (c && c->enabled &&
        !c->ignore_close) // do not close the CA unless in a bad state
    {
        dvbca_close_device(c);
    }
    return 1;
}

int dvbca_close() {
    int i;
    for (i = 0; i < MAX_ADAPTERS; i++)
        if (ca_devices[i] && ca_devices[i]->enabled) {
            dvbca_close_device(ca_devices[i]);
        }
    return 0;
}

SCA_op dvbca;

void dvbca_init() // you can search the devices here and fill the ca_devices,
                  // then open them here (for example independent CA devices),
                  // or use dvbca_init_dev to open them (like in this module)
{
    memset(&dvbca, 0, sizeof(dvbca));
    dvbca.ca_init_dev = dvbca_init_dev;
    dvbca.ca_close_dev = dvbca_close_dev;
    dvbca.ca_add_pmt = dvbca_process_pmt;
    dvbca.ca_del_pmt = dvbca_del_pmt;
    dvbca.ca_close_ca = dvbca_close;
    dvbca.ca_ts = NULL; // dvbca_ts;
    dvbca_id = add_ca(&dvbca, 0xFFFFFFFF);
}

char *get_ca_pin(int i) {
    if (ca_devices[i])
        return ca_devices[i]->pin_str;
    return NULL;
}

void set_ca_pin(int i, char *pin) {
    if (!ca_devices[i])
        ca_devices[i] = alloc_ca_device();
    if (!ca_devices[i])
        return;
    memset(ca_devices[i]->pin_str, 0, sizeof(ca_devices[i]->pin_str));
    strncpy(ca_devices[i]->pin_str, pin, sizeof(ca_devices[i]->pin_str) - 1);
}

void force_ci_adapter(int i) {
    if (!ca_devices[i])
        ca_devices[i] = alloc_ca_device();
    if (!ca_devices[i])
        return;
    ca_devices[i]->force_ci = 1;
}

void set_ca_adapter_force_ci(char *o) {
    int i, j, la, st, end;
    char buf[1000], *arg[40], *sep;
    SAFE_STRCPY(buf, o);
    la = split(arg, buf, ARRAY_SIZE(arg), ',');
    for (i = 0; i < la; i++) {
        sep = strchr(arg[i], '-');

        if (sep == NULL) {
            st = end = map_int(arg[i], NULL);
        } else {
            st = map_int(arg[i], NULL);
            end = map_int(sep + 1, NULL);
        }
        for (j = st; j <= end; j++) {

            force_ci_adapter(j);
            LOG("Forcing CA %d to CI", j);
        }
    }
}

void set_ca_adapter_pin(char *o) {
    int i, j, la, st, end;
    char buf[1000], *arg[40], *sep, *seps;
    SAFE_STRCPY(buf, o);
    la = split(arg, buf, ARRAY_SIZE(arg), ',');
    for (i = 0; i < la; i++) {
        sep = strchr(arg[i], '-');
        seps = strchr(arg[i], ':');

        if (!seps)
            continue;

        if (sep == NULL) {
            st = end = map_int(arg[i], NULL);
        } else {
            st = map_int(arg[i], NULL);
            end = map_int(sep + 1, NULL);
        }
        for (j = st; j <= end; j++) {
            set_ca_pin(j, seps + 1);
            LOG("Setting CA %d pin to %s", j, seps + 1);
        }
    }
}

void set_ca_multiple_pmt(char *o) {
    int i, la, ddci;
    char buf[1000], *arg[40], *sep, *seps;
    SAFE_STRCPY(buf, o);
    la = split(arg, buf, ARRAY_SIZE(arg), ',');
    for (i = 0; i < la; i++) {
        sep = strchr(arg[i], ':');

        if (!sep)
            continue;

        int max_ca_pmt = atoi(sep + 1);

        ddci = map_intd(arg[i], NULL, -1);
        if (!ca_devices[ddci])
            ca_devices[ddci] = alloc_ca_device();
        if (!ca_devices[ddci])
            return;
        ca_devices[ddci]->multiple_pmt = 1;
        ca_devices[ddci]->max_ca_pmt = max_ca_pmt;
        LOG("Forcing CA %d to use multiple PMTs with maximum channels %d", ddci,
            max_ca_pmt);
        seps = sep;
        while ((seps = strchr(seps + 1, '-'))) {
            int caid = strtoul(seps + 1, NULL, 16);
            if (caid > 0)
                ca_devices[ddci]->caid[ca_devices[ddci]->caids++] = caid;
            LOG("Forcing CA %d to use CAID %04X", ddci, caid);
        }
    }
}
