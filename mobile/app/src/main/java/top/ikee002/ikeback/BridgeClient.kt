package top.ikee002.ikeback

import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

class UnauthorizedException : IOException("课堂密钥被教师机拒绝")

/**
 * Talks to the teacher machine's ikeback bridge.
 *
 * Reuses the per-lesson connection key. The key itself is never transmitted:
 * every request proves knowledge of it with an HMAC-SHA256 over the method,
 * path, nonce and body hash. /state issues a single-use challenge, so one
 * nonce can never authorize two requests (see IkebackCrypto).
 *
 * Responses must carry a nonce, Unix timestamp and HMAC over the exact body.
 * No body (including an error) is parsed until verification succeeds. This
 * detects LAN tampering but does not provide TLS confidentiality or resistance
 * to a lesson-key holder; see the residual transport limitation in the report.
 * Uses only the JDK/Android built-ins - no Retrofit, OkHttp or crypto library.
 */
class BridgeClient {
    var port: Int = Announcement.DEFAULT_PORT
    var lessonKey: String = ""
        set(value) {
            field = value.trim()
        }

    private var nonce: String = ""

    fun hasKey(): Boolean = lessonKey.isNotEmpty()

    private fun url(host: String, route: String): URL =
        URL("http://$host:$port/api/classroom/$route")

    private data class HttpResponse(
        val code: Int,
        val body: String,
        val retryAfterMs: Long,
    )

    private fun request(host: String, route: String, method: String, body: String?, auth: Boolean): String {
        // Challenges are single-use on the teacher machine, so every
        // authenticated call obtains a fresh /state nonce first.
        val responseKey = lessonKey
        if (auth && responseKey.isEmpty()) throw UnauthorizedException()
        var challenge = if (auth) connect(host).nonce else IkebackCrypto.generateNonce()
        var attempt = 0
        while (true) {
            val response = exchange(host, route, method, body, challenge, responseKey)
            // /state is intentionally rate-limited per peer. Retry only signed
            // throttle responses, with a fresh challenge and a bounded delay.
            if (route == "state" && response.code == HttpURLConnection.HTTP_UNAVAILABLE && attempt < 4) {
                Thread.sleep(response.retryAfterMs)
                challenge = IkebackCrypto.generateNonce()
                attempt++
                continue
            }
            if (response.code == HttpURLConnection.HTTP_UNAUTHORIZED) throw UnauthorizedException()
            if (response.code !in 200..299) {
                val error = try { JSONObject(response.body).optString("error") } catch (_: Exception) { "" }
                throw IOException("$route: HTTP ${response.code}${if (error.isNotEmpty()) " ($error)" else ""}")
            }
            return response.body
        }
    }

    private fun exchange(
        host: String,
        route: String,
        method: String,
        body: String?,
        challenge: String,
        responseKey: String,
    ): HttpResponse {
        val connection = url(host, route).openConnection() as HttpURLConnection
        try {
            connection.connectTimeout = 5_000
            connection.readTimeout = 15_000
            connection.requestMethod = method
            connection.setRequestProperty("Accept", "application/json, */*")
            // The public /state call uses a fresh nonce too, so its signed
            // response cannot be replayed. Authenticated calls reuse their
            // single-use request challenge as the response nonce.
            connection.setRequestProperty("X-Classroom-Nonce", challenge)
            if (responseKey.isNotEmpty() && route != "state") {
                connection.setRequestProperty("X-Classroom-Token",
                    IkebackCrypto.token(responseKey, method, "/api/classroom/$route", challenge, body))
            }
            if (body != null) {
                connection.doOutput = true
                connection.setRequestProperty("Content-Type", "application/json; charset=utf-8")
                connection.outputStream.use { it.write(body.toByteArray(Charsets.UTF_8)) }
            }
            val code = connection.responseCode
            val stream = if (code in 200..299) connection.inputStream else connection.errorStream
            val raw = readResponse(stream)
            // Missing signatures are a hard failure, never an unsigned fallback.
            val verified = IkebackCrypto.verifyResponse(
                lessonKey = responseKey,
                body = raw,
                nonce = connection.getHeaderField("X-Classroom-Response-Nonce"),
                timestamp = connection.getHeaderField("X-Classroom-Response-Timestamp"),
                signature = connection.getHeaderField("X-Classroom-Response-Signature"),
                expectedNonce = challenge,
            ).toString(Charsets.UTF_8)
            val retryAfter = connection.getHeaderFieldInt("Retry-After", 1)
                .coerceIn(1, 2).toLong() * 1_000
            return HttpResponse(code, verified, retryAfter)
        } finally {
            connection.disconnect()
        }
    }

    private fun readResponse(stream: java.io.InputStream?): ByteArray {
        if (stream == null) return ByteArray(0)
        val output = java.io.ByteArrayOutputStream()
        stream.use { input ->
            val buffer = ByteArray(8192)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                if (output.size() + count > MAX_RESPONSE_BYTES) throw IOException("教师机响应超过 8 MiB")
                output.write(buffer, 0, count)
            }
        }
        return output.toByteArray()
    }

    /** Public metadata endpoint. Always returns the current challenge. */
    fun connect(host: String): TeacherState {
        val state = parseTeacherState(JSONObject(request(host, "state", "GET", null, auth = false)))
        nonce = state.nonce
        return state
    }

    fun scores(host: String): ScoreSnapshot =
        parseScoreSnapshot(request(host, "scores", "GET", null, auth = true))

    /** Students in the exact order the teacher dragged them on the seat grid. */
    fun seats(host: String): List<SeatStudent> =
        parseSeats(request(host, "seats", "GET", null, auth = true))

    /** One-tap scoring from the phone (delta in -5..+5, reason optional). */
    fun score(host: String, seat: String, delta: Int, reason: String): Boolean {
        val body = JSONObject()
            .put("seat", seat)
            .put("delta", delta)
            .put("reason", reason)
            .toString()
        return JSONObject(request(host, "score", "POST", body, auth = true)).optBoolean("ok")
    }

    fun events(host: String): List<ActivityEvent> =
        parseEvents(request(host, "events", "GET", null, auth = true))

    fun roster(host: String): String =
        request(host, "roster", "GET", null, auth = true)

    fun wordCloud(host: String, seat: String): String =
        request(host, "wordcloud?seat=${java.net.URLEncoder.encode(seat, "UTF-8")}", "GET", null, auth = true)

    fun report(host: String): String =
        request(host, "report", "GET", null, auth = true)

    /** request() refreshes the single-use challenge before sending. */
    fun command(
        host: String,
        name: String,
        enabled: Boolean,
        text: String = "",
        teacherIp: String = "",
    ): Boolean {
        val body = JSONObject()
            .put("name", name)
            .put("enabled", enabled)
            .put("text", text)
            .put("teacherIp", teacherIp)
            .toString()
        val reply = JSONObject(request(host, "command", "POST", body, auth = true))
        return reply.optBoolean("ok")
    }

    fun pushScores(host: String, snapshotRaw: String): Boolean {
        val body = JSONObject().put("scores", JSONObject(snapshotRaw)).toString()
        return JSONObject(request(host, "sync", "POST", body, auth = true)).optBoolean("ok")
    }

    /** Teacher-machine copy of the timetable. Empty object when never uploaded. */
    fun timetable(host: String): JSONObject {
        return JSONObject(request(host, "timetable", "GET", null, auth = true))
            .optJSONObject("timetable") ?: JSONObject()
    }

    fun pushTimetable(host: String, timetableJson: String): Boolean {
        val body = JSONObject().put("timetable", JSONObject(timetableJson)).toString()
        return JSONObject(request(host, "timetable", "POST", body, auth = true)).optBoolean("ok")
    }

    /** Per-class roster used by the 「班级」 page. */
    fun classes(host: String): String {
        return request(host, "classes", "GET", null, auth = true)
    }

    private companion object {
        const val MAX_RESPONSE_BYTES = 8 * 1024 * 1024
    }
}
