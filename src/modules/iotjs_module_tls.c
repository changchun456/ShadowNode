#include "mbedtls/net.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "mbedtls/error.h"
#include "mbedtls/platform.h"

#include "iotjs_def.h"
#include "iotjs_objectwrap.h"
#include "iotjs_module_crypto.h"
#include "iotjs_module_buffer.h"
#include "iotjs_module_tls_bio.h"

#define DEBUG_LEVEL 1

enum {
  SSL_HANDSHAKE_READY = 0,
  SSL_HANDSHAKING     = 1,
  SSL_HANDSHAKE_DONE  = 2,
};

typedef struct {
  iotjs_jobjectwrap_t   jobjectwrap;

  /**
   * SSL common structure
   */
  mbedtls_x509_crt      ca_;
  mbedtls_ssl_context   ssl_;
  mbedtls_ssl_config    config_;
  int                   handshake_state;

  /**
   * BIO buffer
   */
  BIO*                  ssl_bio_;
  BIO*                  app_bio_;

} IOTJS_VALIDATED_STRUCT(iotjs_tlswrap_t);

static JNativeInfoType this_module_native_info = { .free_cb = NULL };

/* List of trusted root CA certificates
 * currently only GlobalSign, the CA for os.mbed.com
 *
 * To add more than one root, just concatenate them.
 */
const char SSL_CA_PEM[] = 
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDujCCAqKgAwIBAgILBAAAAAABD4Ym5g0wDQYJKoZIhvcNAQEFBQAwTDEgMB4GA1UECxMX\n"
  "R2xvYmFsU2lnbiBSb290IENBIC0gUjIxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMT\n"
  "Ckdsb2JhbFNpZ24wHhcNMDYxMjE1MDgwMDAwWhcNMjExMjE1MDgwMDAwWjBMMSAwHgYDVQQL\n"
  "ExdHbG9iYWxTaWduIFJvb3QgQ0EgLSBSMjETMBEGA1UEChMKR2xvYmFsU2lnbjETMBEGA1UE\n"
  "AxMKR2xvYmFsU2lnbjCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKbPJA6+Lm8o\n"
  "mUVCxKs+IVSbC9N/hHD6ErPLv4dfxn+G07IwXNb9rfF73OX4YJYJkhD10FPe+3t+c4isUoh7\n"
  "SqbKSaZeqKeMWhG8eoLrvozps6yWJQeXSpkqBy+0Hne/ig+1AnwblrjFuTosvNYSuetZfeLQ\n"
  "BoZfXklqtTleiDTsvHgMCJiEbKjNS7SgfQx5TfC4LcshytVsW33hoCmEofnTlEnLJGKRILzd\n"
  "C9XZzPnqJworc5HGnRusyMvo4KD0L5CLTfuwNhv2GXqF4G3yYROIXJ/gkwpRl4pazq+r1feq\n"
  "CapgvdzZX99yqWATXgAByUr6P6TqBwMhAo6CygPCm48CAwEAAaOBnDCBmTAOBgNVHQ8BAf8E\n"
  "BAMCAQYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUm+IHV2ccHsBqBt5ZtJot39wZhi4w\n"
  "NgYDVR0fBC8wLTAroCmgJ4YlaHR0cDovL2NybC5nbG9iYWxzaWduLm5ldC9yb290LXIyLmNy\n"
  "bDAfBgNVHSMEGDAWgBSb4gdXZxwewGoG3lm0mi3f3BmGLjANBgkqhkiG9w0BAQUFAAOCAQEA\n"
  "mYFThxxol4aR7OBKuEQLq4GsJ0/WwbgcQ3izDJr86iw8bmEbTUsp9Z8FHSbBuOmDAGJFtqkI\n"
  "k7mpM0sYmsL4h4hO291xNBrBVNpGP+DTKqttVCL1OmLNIG+6KYnX3ZHu01yiPqFbQfXf5WRD\n"
  "LenVOavSot+3i9DAgBkcRcAtjOj4LaR0VknFBbVPFd5uRHg5h6h+u/N5GJG79G+dwfCMNYxd\n"
  "AfvDbbnvRG15RjF+Cv6pgsH/76tuIMRQyV+dTZsXjAzlAcmgQWpzU/qlULRuJQ/7TBj0/VLZ\n"
  "jmmx6BEP3ojY+x1J96relc8geMJgEtslQIxq/H5COEBkEveegeGTLg==\n"
  "-----END CERTIFICATE-----\n";

static iotjs_tlswrap_t* iotjs_tlswrap_create(const jerry_value_t value) {
  iotjs_tlswrap_t* tlswrap = IOTJS_ALLOC(iotjs_tlswrap_t);
  IOTJS_VALIDATED_STRUCT_CONSTRUCTOR(iotjs_tlswrap_t, tlswrap);
  iotjs_jobjectwrap_initialize(&_this->jobjectwrap, 
                               value,
                               &this_module_native_info);

  mbedtls_x509_crt_init(&_this->ca_);
  mbedtls_ssl_init(&_this->ssl_);
  mbedtls_ssl_config_init(&_this->config_);
  return tlswrap;
}

static void print_mbedtls_error(const char *name, int err) {
  char buf[128];
  mbedtls_strerror(err, buf, sizeof(buf));
  mbedtls_printf("%s() failed: -0x%04x (%d): %s\n", name, -err, err, buf);
}

// static void iotjs_tlswrap_destroy(iotjs_tlswrap_t* tlswrap) {
//   IOTJS_VALIDATED_STRUCT_DESTRUCTOR(iotjs_tlswrap_t, tlswrap);
//   iotjs_jobjectwrap_destroy(&_this->jobjectwrap);
//   IOTJS_RELEASE(tlswrap);
// }

JS_FUNCTION(TlsConstructor) {
  DJS_CHECK_THIS();

  const jerry_value_t jtls = JS_GET_THIS();
  iotjs_tlswrap_t* tlswrap = iotjs_tlswrap_create(jtls);
  IOTJS_VALIDATED_STRUCT_METHOD(iotjs_tlswrap_t, tlswrap);

  jerry_value_t opts = jargv[0];
  _this->handshake_state = SSL_HANDSHAKE_READY;

  int ret = -1;

  /**
   * options.ca
   */
  jerry_value_t jca_txt = iotjs_jval_get_property(opts, "ca");
  if (jerry_value_is_string(jca_txt)) {
    iotjs_string_t ca_txt = iotjs_jval_as_string(jca_txt);
    ret = mbedtls_x509_crt_parse(&_this->ca_,
                                 (const unsigned char*)iotjs_string_data(&ca_txt),
                                 (size_t)iotjs_string_size(&ca_txt) + 1);
    iotjs_string_destroy(&ca_txt);
  } else {
    ret = mbedtls_x509_crt_parse(&_this->ca_, 
                                 (const unsigned char*)SSL_CA_PEM,
                                 sizeof(SSL_CA_PEM));
  }
  if (ret != 0) {
    print_mbedtls_error("mbedtls_parse_crt", ret);
    return JS_CREATE_ERROR(COMMON, "parse x509 failed.");
  }
  mbedtls_ssl_conf_ca_chain(&_this->config_, &_this->ca_, NULL);
  mbedtls_ssl_conf_rng(&_this->config_, mbedtls_ctr_drbg_random, &drgb_ctx);
  jerry_release_value(jca_txt);

  if ((ret = mbedtls_ssl_config_defaults(&_this->config_,
                                         MBEDTLS_SSL_IS_CLIENT,
                                         MBEDTLS_SSL_TRANSPORT_STREAM,
                                         MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
    return JS_CREATE_ERROR(COMMON, "SSL configuration failed.");
  }

  /**
   * options.cert
   * options.key
   */
  // jerry_value_t jcert_txt = iotjs_jval_get_property(opts, "cert");
  // jerry_value_t jkey_txt = iotjs_jval_get_property(opts, "key");
  // if (jerry_value_is_string(jcert_txt) && jerry_value_is_string(jkey_txt)) {
  //   iotjs_string_t cert_txt = iotjs_jval_as_string(jcert_txt);
  //   iotjs_string_t key_txt = iotjs_jval_as_string(jkey_txt);

  //   mbedtls_x509_crt cert_chain;
  //   mbedtls_x509_crt_init(&cert_chain);
  //   if (0 != mbedtls_x509_crt_parse(&cert_chain, 
  //                                   (const unsigned char*)iotjs_string_data(&cert_txt),
  //                                   iotjs_string_size(&cert_txt) + 1)) {
  //     // TODO free tokens
  //     return JS_CREATE_ERROR(COMMON, "failed to parse cert");
  //   }

  //   mbedtls_pk_context key_ctx;
  //   mbedtls_pk_init(&key_ctx);
  //   if (0 != mbedtls_pk_parse_key(&key_ctx,
  //                                 (const unsigned char*)iotjs_string_data(&key_txt),
  //                                 iotjs_string_size(&key_txt) + 1,
  //                                 NULL, 0)) {
  //     // TODO free tokens
  //     return JS_CREATE_ERROR(COMMON, "failed to parse key pem");
  //   }

  //   if (0 != mbedtls_ssl_set_hs_own_cert(&_this->ssl_, 
  //                                        &cert_chain, 
  //                                        &key_ctx)) {
  //     // TODO free tokens
  //     return JS_CREATE_ERROR(COMMON, "failed to set cert/key");
  //   }

  //   iotjs_string_destroy(&cert_txt);
  //   iotjs_string_destroy(&key_txt);
  // }
  // jerry_release_value(jcert_txt);
  // jerry_release_value(jkey_txt);

  /**
   * options.rejectUnauthorized
   */
  jerry_value_t jrejectUnauthorized = iotjs_jval_get_property(opts, "rejectUnauthorized");
  bool rejectUnauthorized = iotjs_jval_as_boolean(jrejectUnauthorized);
  mbedtls_ssl_conf_authmode(&_this->config_, 
                            rejectUnauthorized ? 
                              MBEDTLS_SSL_VERIFY_REQUIRED : 
                              MBEDTLS_SSL_VERIFY_NONE);
  jerry_release_value(jrejectUnauthorized);

  /**
   * options.servername
   */
  jerry_value_t jservername = iotjs_jval_get_property(opts, "servername");
  iotjs_string_t servername = iotjs_jval_as_string(jservername);
  mbedtls_ssl_set_hostname(&_this->ssl_, iotjs_string_data(&servername));
  jerry_release_value(jservername);

  /**
   * setup
   */
  if ((ret = mbedtls_ssl_setup(&_this->ssl_, &_this->config_)) != 0) {
    return JS_CREATE_ERROR(COMMON, "SSL failed.");
  }

  /**
   * Set BIO
   */
  _this->ssl_bio_ = SSL_BIO_new(BIO_BIO);
  _this->app_bio_ = SSL_BIO_new(BIO_BIO);
  BIO_make_bio_pair(_this->ssl_bio_, _this->app_bio_);
  mbedtls_ssl_set_bio(&_this->ssl_, _this->ssl_bio_, BIO_net_send, BIO_net_recv, NULL);
  return jerry_create_undefined();
}

jerry_value_t iotjs_tlswrap_encode_data(iotjs_tlswrap_t_impl_t* _this, 
                                        iotjs_bufferwrap_t* inbuf) {
  size_t rv = (size_t)mbedtls_ssl_write(&_this->ssl_, 
                                        (const unsigned char*)iotjs_bufferwrap_buffer(inbuf),
                                        iotjs_bufferwrap_length(inbuf));
  size_t pending = 0;
  if ((pending = BIO_ctrl_pending(_this->app_bio_)) > 0) {
    const char tmpbuf[pending];
    memset((void*)tmpbuf, 0, pending);
    rv = (size_t)BIO_read(_this->app_bio_, tmpbuf, pending);

    jerry_value_t out = iotjs_bufferwrap_create_buffer(rv);
    iotjs_bufferwrap_t* outbuf = iotjs_bufferwrap_from_jbuffer(out);
    iotjs_bufferwrap_copy(outbuf, (const char*)tmpbuf, rv);
    return out;
  } else {
    return jerry_create_null();
  }
}

void iotjs_tlswrap_stay_update(iotjs_tlswrap_t_impl_t* _this) {
  size_t pending = BIO_ctrl_pending(_this->app_bio_);
  if (pending > 0) {
    char src[pending];
    BIO_read(_this->app_bio_, src, sizeof(src));

    jerry_value_t jthis = iotjs_jobjectwrap_jobject(&_this->jobjectwrap);
    jerry_value_t fn = iotjs_jval_get_property(jthis, "onwrite");
    iotjs_jargs_t jargv = iotjs_jargs_create(1);
    jerry_value_t jbuffer = iotjs_bufferwrap_create_buffer((size_t)pending);
    iotjs_bufferwrap_t* buffer_wrap = iotjs_bufferwrap_from_jbuffer(jbuffer);

    iotjs_bufferwrap_copy(buffer_wrap, (const char*)src, pending);
    iotjs_jargs_append_jval(&jargv, jbuffer);
    iotjs_make_callback(fn, jthis, &jargv);

    // TODO(Yorkie): should free
  }
}

int iotjs_tlswrap_error_handler(iotjs_tlswrap_t_impl_t* _this, const int code) {
  if (code == MBEDTLS_ERR_SSL_WANT_WRITE || 
    code == MBEDTLS_ERR_SSL_WANT_READ) {
    iotjs_tlswrap_stay_update(_this);
  } else if (code < 0) {
    print_mbedtls_error("mbedtls_ssl_handshake", code);
  }
  return code;
}

JS_FUNCTION(TlsHandshake) {
  JS_DECLARE_THIS_PTR(tlswrap, tlswrap);
  IOTJS_VALIDATED_STRUCT_METHOD(iotjs_tlswrap_t, tlswrap);

  if (_this->handshake_state == SSL_HANDSHAKE_DONE) {
    return jerry_create_number(SSL_HANDSHAKE_DONE);
  }
  _this->handshake_state = SSL_HANDSHAKING;
  
  int rv = 0;
  rv = mbedtls_ssl_handshake(&_this->ssl_);
  rv = iotjs_tlswrap_error_handler(_this, rv);
  if (rv == 0) {
    _this->handshake_state = SSL_HANDSHAKE_DONE;

    int verify_status = (int)mbedtls_ssl_get_verify_result(&_this->ssl_);
    if (verify_status) {
      char buf[512];
      mbedtls_x509_crt_verify_info(buf, sizeof(buf), "::", (uint32_t)verify_status);
      mbedtls_printf("%s\n", buf);
    }

    // notify to the JS layer "onhandshakedone".
    jerry_value_t fn = iotjs_jval_get_property(jthis, "onhandshakedone");
    iotjs_jargs_t argv = iotjs_jargs_create(0);
    iotjs_make_callback(fn, jthis, &argv);
    jerry_release_value(fn);
  }
  return jerry_create_number(_this->handshake_state);
}

JS_FUNCTION(TlsWrite) {
  JS_DECLARE_THIS_PTR(tlswrap, tlswrap);
  IOTJS_VALIDATED_STRUCT_METHOD(iotjs_tlswrap_t, tlswrap);

  iotjs_bufferwrap_t* buf = iotjs_bufferwrap_from_jbuffer(jargv[0]);
  return iotjs_tlswrap_encode_data(_this, buf);
}

JS_FUNCTION(TlsRead) {
  JS_DECLARE_THIS_PTR(tlswrap, tlswrap);
  IOTJS_VALIDATED_STRUCT_METHOD(iotjs_tlswrap_t, tlswrap);

  iotjs_bufferwrap_t* bufwrap = iotjs_bufferwrap_from_jbuffer(jargv[0]);
  size_t size = iotjs_bufferwrap_length(bufwrap);
  BIO_write(_this->app_bio_, iotjs_bufferwrap_buffer(bufwrap), size);

  jerry_value_t checker = iotjs_jval_get_property(jthis, "handshake");
  jerry_value_t res = iotjs_make_callback_with_result(checker, jthis,
                                                      iotjs_jargs_get_empty());

  // if handshaking, just returns
  if (iotjs_jval_as_number(res) == SSL_HANDSHAKING) {
    jerry_release_value(res);
    return jerry_create_boolean(true);
  }

  int rv = -1;
  unsigned char* decrypted = (unsigned char*)malloc(size);
  memset(decrypted, 0, size);

  rv = mbedtls_ssl_read(&_this->ssl_, decrypted, size);
  rv = iotjs_tlswrap_error_handler(_this, rv);
  
  if (rv > 0) {
    jerry_value_t fn = iotjs_jval_get_property(jthis, "onread");
    iotjs_jargs_t jargv = iotjs_jargs_create(1);
    jerry_value_t jbuffer = iotjs_bufferwrap_create_buffer((size_t)(rv));
    iotjs_bufferwrap_t* buffer_wrap = iotjs_bufferwrap_from_jbuffer(jbuffer);

    iotjs_bufferwrap_copy(buffer_wrap, (const char*)decrypted, (size_t)(rv));
    iotjs_jargs_append_jval(&jargv, jbuffer);
    iotjs_make_callback(fn, jthis, &jargv);
    jerry_release_value(fn);
  }

  jerry_release_value(res);
  return jerry_create_number(0);
}

jerry_value_t InitTls() {
  jerry_value_t tls = jerry_create_object();
  jerry_value_t tlsConstructor =
      jerry_create_external_function(TlsConstructor);
  iotjs_jval_set_property_jval(tls, "TlsWrap", tlsConstructor);

  jerry_value_t proto = jerry_create_object();
  iotjs_jval_set_method(proto, "handshake", TlsHandshake);
  iotjs_jval_set_method(proto, "write", TlsWrite);
  iotjs_jval_set_method(proto, "read", TlsRead);
  iotjs_jval_set_property_jval(tlsConstructor, "prototype", proto);

  jerry_release_value(proto);
  jerry_release_value(tlsConstructor);
  return tls;
}