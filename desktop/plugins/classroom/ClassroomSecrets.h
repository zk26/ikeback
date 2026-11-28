/*
 * ClassroomSecrets.h - protect secrets on disk and authenticate bridge requests
 *
 * This file is part of the Ikeback classroom modification.
 */

#pragma once

#include <QByteArray>
#include <QString>

class ClassroomSecrets
{
public:
	//! Windows: CryptProtectData bound to the interactive teacher account.
	//! The "dpapi:" prefix is what is written to settings.json.
	static QString protect( const QString& plain );
	static QString reveal( const QString& stored );

	//! True for the pre-2.3 "enc:" format (encrypted with the shipped public key).
	static bool isLegacyProtected( const QString& stored );

	//! 128 bit of CryptGenRandom-backed entropy, hex encoded (32 characters).
	static QString generateBridgeKey();
	//! One-time challenge for a single HTTP request, hex encoded.
	static QString generateNonce();

	//! HMAC-SHA256(key, method "\n" path "\n" nonce "\n" SHA256(body)), lowercase hex.
	//! Binding method/path/body makes a captured token useless for any other request.
	static QByteArray requestToken( const QString& key, const QString& method, const QString& path,
									const QString& nonce, const QByteArray& body );

	//! HMAC-SHA256(key, nonce "\n" timestamp "\n" SHA256(exactBody)), lowercase hex.
	static QByteArray responseToken( const QString& key, const QString& nonce,
									 const QString& timestamp, const QByteArray& body );

	static bool constantTimeEquals( const QByteArray& left, const QByteArray& right );

	//! SHA-256 (hex, lowercase) of the super-admin key. Students only ever see
	//! this digest (settings.json / HKLM), never the plain key.
	//! Returns an empty digest for an empty key or the retired default password.
	static QString adminKeyHash( const QString& key );
	//! Deprecated migration shims: both return empty. There is no default key.
	static QString defaultAdminKey();
	static QString defaultAdminKeyHash();
	//! True only for a well-formed digest that is not the retired default.
	static bool adminKeyHashConfigured( const QString& storedHash );
	//! True when `key` matches a configured, non-default digest. An empty digest
	//! means "no key configured" and fails closed.
	static bool verifyAdminKey( const QString& key, const QString& storedHash );
};
