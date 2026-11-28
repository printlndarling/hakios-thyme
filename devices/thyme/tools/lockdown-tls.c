/* Memory BIOs keep TLS entirely in the Lockdown daemon. The USB driver
 * carries the bytes and has no access to the session key. */
#include "lockdown-tls.h"

#include <limits.h>
#include <openssl/err.h>
#include <stdio.h>

static X509 *
decode_certificate(CFDataRef data)
{
	const unsigned char *cursor = CFDataGetBytePtr(data);
	return d2i_X509(NULL, &cursor, CFDataGetLength(data));
}

static int
verify_client(X509_STORE_CTX *store, void *host)
{
	if (X509_verify_cert(store) != 1) {
		return 0;
	}
	unsigned char approved[EVP_MAX_MD_SIZE], peer[EVP_MAX_MD_SIZE];
	unsigned int approved_size, peer_size;
	if (!X509_digest(host, EVP_sha256(), approved, &approved_size) ||
	    !X509_digest(X509_STORE_CTX_get0_cert(store), EVP_sha256(), peer, &peer_size) ||
	    approved_size != peer_size || CRYPTO_memcmp(approved, peer, peer_size) != 0) {
		X509_STORE_CTX_set_error(store, X509_V_ERR_CERT_REJECTED);
		return 0;
	}
	return 1;
}

void
lockdown_tls_clear(struct lockdown_tls *tls)
{
	SSL_free(tls->connection);
	X509_free(tls->host);
	if (tls->id) CFRelease(tls->id);
	OPENSSL_cleanse(tls->plaintext, tls->plaintext_length);
	tls->connection = NULL;
	tls->host = NULL;
	tls->id = NULL;
	tls->plaintext_length = 0;
}

bool
lockdown_tls_start(struct lockdown_tls *tls, const struct lockdown_certificates *certs, SecKeyRef key)
{
	CFUUIDRef uuid = CFUUIDCreate(kCFAllocatorDefault);
	tls->id = uuid ? CFUUIDCreateString(kCFAllocatorDefault, uuid) : NULL;
	if (uuid) CFRelease(uuid);

	X509 *root = decode_certificate(certs->root);
	X509 *device = decode_certificate(certs->device);
	tls->host = decode_certificate(certs->host);
	CFDataRef key_data = key ? SecKeyCopyExternalRepresentation(key, NULL) : NULL;
	const unsigned char *cursor = key_data ? CFDataGetBytePtr(key_data) : NULL;
	EVP_PKEY *private_key = key_data ? d2i_AutoPrivateKey(NULL, &cursor, CFDataGetLength(key_data)) : NULL;
	if (key_data) CFRelease(key_data);

	SSL_CTX *context = SSL_CTX_new(TLS_server_method());
	bool ready = tls->id && root && device && tls->host && private_key && context &&
	    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) == 1 &&
	    SSL_CTX_set_max_proto_version(context, TLS1_2_VERSION) == 1 &&
	    X509_STORE_add_cert(SSL_CTX_get_cert_store(context), root) == 1 &&
	    SSL_CTX_use_certificate(context, device) == 1 &&
	    SSL_CTX_use_PrivateKey(context, private_key) == 1 && SSL_CTX_check_private_key(context) == 1;
	EVP_PKEY_free(private_key);
	X509_free(device);
	X509_free(root);
	if (ready) {
		SSL_CTX_set_options(context, SSL_OP_NO_TICKET | SSL_OP_NO_RENEGOTIATION);
		SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
		SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
		SSL_CTX_set_cert_verify_callback(context, verify_client, tls->host);
		tls->connection = SSL_new(context);
	}
	SSL_CTX_free(context);

	BIO *input = tls->connection ? BIO_new(BIO_s_mem()) : NULL;
	BIO *output = tls->connection ? BIO_new(BIO_s_mem()) : NULL;
	if (!input || !output) {
		BIO_free(input);
		BIO_free(output);
		lockdown_tls_clear(tls);
		return false;
	}
	SSL_set_bio(tls->connection, input, output);
	SSL_set_accept_state(tls->connection);
	return true;
}

bool
lockdown_tls_receive(struct lockdown_tls *tls, const uint8_t *bytes, size_t length)
{
	SSL *connection = tls->connection;
	if (!connection || length > INT_MAX || BIO_write(SSL_get_rbio(connection), bytes, (int)length) != (int)length) {
		return false;
	}
	ERR_clear_error();
	if (!SSL_is_init_finished(connection)) {
		int result = SSL_do_handshake(connection);
		if (result != 1) {
			return SSL_get_error(connection, result) == SSL_ERROR_WANT_READ;
		}
		return true;
	}
	size_t available = sizeof(tls->plaintext) - tls->plaintext_length, read_size = 0;
	if (!available) {
		return false;
	}
	int result = SSL_read_ex(connection, tls->plaintext + tls->plaintext_length, available, &read_size);
	if (result == 1) {
		tls->plaintext_length += read_size;
		/* More data would exceed the maximum decrypted request. */
		return SSL_pending(connection) == 0;
	}
	int error = SSL_get_error(connection, result);
	if (error == SSL_ERROR_ZERO_RETURN) {
		SSL_shutdown(connection);
	}
	return error == SSL_ERROR_WANT_READ;
}

int
lockdown_tls_output(struct lockdown_tls *tls, uint8_t *output, size_t capacity)
{
	if (!tls->connection) {
		return 0;
	}
	BIO *bio = SSL_get_wbio(tls->connection);
	size_t pending = BIO_ctrl_pending(bio);
	if (pending > capacity || pending > INT_MAX) {
		return -1;
	}
	return pending ? BIO_read(bio, output, (int)pending) : 0;
}
