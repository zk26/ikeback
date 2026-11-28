package top.ikee002.ikeback

import java.io.IOException
import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Shared with the teacher machine (ClassroomSecrets::requestToken).
 *
 * token = HMAC-SHA256(
 *     key   = lessonKey,
 *     msg   = method + "\n" + path + "\n" + nonce + "\n" + SHA256(body).hex
 * )
 *
 * Binding the method, full path (including the query string) and the request
 * body means a captured token cannot be replayed against a different call.
 */
object IkebackCrypto {
    fun sha256Hex(bytes: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(bytes)
        return digest.joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
    }

    fun token(
        lessonKey: String,
        method: String,
        path: String,
        nonce: String,
        body: String?,
    ): String {
        val bodyHash = sha256Hex((body ?: "").toByteArray(Charsets.UTF_8))
        val message = "$method\n$path\n$nonce\n$bodyHash"
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(lessonKey.toByteArray(Charsets.UTF_8), "HmacSHA256"))
        return mac.doFinal(message.toByteArray(Charsets.UTF_8))
            .joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
    }

    /** Fresh nonce sent on public /state calls and echoed by signed responses. */
    fun generateNonce(): String {
        val bytes = ByteArray(16)
        SecureRandom().nextBytes(bytes)
        return bytes.joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
    }

    /**
     * Response protocol (additive to the existing request protocol):
     *   signature = HMAC-SHA256(key,
     *       nonce + "\n" + timestampSeconds + "\n" + SHA256(exactResponseBody).hex)
     *
     * The exact UTF-8 body bytes are authenticated, not a re-serialized JSON
     * object. The teacher echoes the request nonce and signs success AND error
     * bodies. A five-minute clock-skew window limits captured-response replay.
     */
    fun responseToken(lessonKey: String, body: String, nonce: String, timestamp: String): String {
        val message = "$nonce\n$timestamp\n${sha256Hex(body.toByteArray(Charsets.UTF_8))}"
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(lessonKey.toByteArray(Charsets.UTF_8), "HmacSHA256"))
        return mac.doFinal(message.toByteArray(Charsets.UTF_8))
            .joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
    }

    /** Returns the exact body only after mandatory integrity/freshness checks. */
    fun verifyResponse(
        lessonKey: String,
        body: String,
        nonce: String?,
        timestamp: String?,
        signature: String?,
        expectedNonce: String,
        nowMillis: Long = System.currentTimeMillis(),
        maxSkewMillis: Long = 300_000L,
    ): String = verifyResponse(
        lessonKey, body.toByteArray(Charsets.UTF_8), nonce, timestamp, signature,
        expectedNonce, nowMillis, maxSkewMillis,
    ).toString(Charsets.UTF_8)

    fun verifyResponse(
        lessonKey: String,
        body: ByteArray,
        nonce: String?,
        timestamp: String?,
        signature: String?,
        expectedNonce: String,
        nowMillis: Long = System.currentTimeMillis(),
        maxSkewMillis: Long = 300_000L,
    ): ByteArray {
        if (lessonKey.isEmpty() || expectedNonce.isEmpty() || nonce.isNullOrEmpty() ||
            timestamp.isNullOrEmpty() || signature.isNullOrEmpty() || nonce != expectedNonce
        ) {
            throw ResponseIntegrityException()
        }
        if (timestamp.length !in 1..20 || !timestamp.all(Char::isDigit) || !signature.matches(Regex("[0-9a-fA-F]{64}"))) {
            throw ResponseIntegrityException()
        }
        val issued = timestamp.toLongOrNull() ?: throw ResponseIntegrityException()
        val now = nowMillis / 1000
        if (issued < now - maxSkewMillis / 1000 || issued > now + maxSkewMillis / 1000) {
            throw ResponseIntegrityException()
        }
        val message = "$nonce\n$timestamp\n${sha256Hex(body)}"
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(lessonKey.toByteArray(Charsets.UTF_8), "HmacSHA256"))
        val expected = mac.doFinal(message.toByteArray(Charsets.UTF_8))
            .joinToString("") { byte -> "%02x".format(byte.toInt() and 0xff) }
        if (!MessageDigest.isEqual(hexBytes(expected), hexBytes(signature))) {
            throw ResponseIntegrityException()
        }
        return body
    }

    private fun hexBytes(value: String): ByteArray = ByteArray(value.length / 2) { index ->
        value.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
}

class ResponseIntegrityException : IOException("教师机响应签名缺失、已过期或不匹配")
